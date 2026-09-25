using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using MiniPet.Export;
using MinipetServer.Services;

// 装配补救：MinipetServer 的 WzComparerR2.WzLib 是裸 Reference（HintPath dll），编译资产会随
// ProjectReference 复制到本目录，但不进 Exporter.deps.json（sdk 对裸引用不做传递性 deps 流动）——
// 普通 exe 宿主按 deps.json 探测 → FileNotFound。这里挂 Default ALC 的 Resolving 钩子按文件名直载。
AssemblyLoadContext.Default.Resolving += static (alc, name) =>
{
    if (!string.Equals(name.Name, "WzComparerR2.WzLib", StringComparison.Ordinal)) return null;
    string path = Path.Combine(AppContext.BaseDirectory, "WzComparerR2.WzLib.dll");
    return File.Exists(path) ? alc.LoadFromAssemblyPath(path) : null;
};

// ═══════════════════════════════════════════
// M2 素材导出 CLI（算法规格 §十.7）
//   dotnet run --project Server/tools/Exporter -- \
//     --appearance Server/seed/default-appearance.json --profile amoled216 \
//     --maps 200000100,220000100 --out data/cache/export
// ═══════════════════════════════════════════

var options = new ExportOptions
{
    AppearancePath = null,
    Profile = "amoled216",
    Maps = new List<string>(),
    OutRoot = "data/cache/export",
    DeviceId = "default",
    FontSizes = new List<int> { 16 },
    IncludeAudio = true,
    IncludeFontTime = true,
    FirmwareVer = "0.0.0",
};

string? wzDataPath = Environment.GetEnvironmentVariable("MINIPET_WZ_DATA");

for (int i = 0; i < args.Length; i++)
{
    string arg = args[i];
    string Next() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"参数 {arg} 缺值");
    switch (arg)
    {
        case "--appearance": options.AppearancePath = Next(); break;
        case "--profile": options.Profile = Next(); break;
        case "--maps": options.Maps = Next().Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToList(); break;
        case "--out": options.OutRoot = Next(); break;
        case "--device-id": options.DeviceId = Next(); break;
        case "--wz": wzDataPath = Next(); break;
        case "--fonts":
            options.FontSizes = Next().Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                .Select(s => int.Parse(s.TrimEnd('p', 'x'))).ToList();
            break;
        case "--no-fonts": options.FontSizes.Clear(); break;
        case "--no-audio": options.IncludeAudio = false; break;
        case "--no-fonttime": options.IncludeFontTime = false; break;
        case "--firmware": options.FirmwareVer = Next(); break;
        case "--charset": options.CharsetFile = Next(); break;
        case "--font-family": options.FontFamily = Next(); break;
        case "--help" or "-h":
            Console.WriteLine("""
                用法: Exporter [--appearance <json>] [--profile <名|路径>] [--maps <id,...>] [--out <dir>]
                       [--device-id <id>] [--wz <WZ数据目录>] [--fonts 16,24,32|--no-fonts]
                       [--no-audio] [--no-fonttime] [--firmware <ver>] [--charset <file>] [--font-family <名>]
                默认: profile=amoled216  out=data/cache/export  fonts=16  audio=on  fontTime=on
                WZ 目录: --wz 或环境变量 MINIPET_WZ_DATA
                """);
            return 0;
        default:
            Console.Error.WriteLine($"未知参数: {arg}");
            return 2;
    }
}

if (string.IsNullOrEmpty(wzDataPath))
{
    Console.Error.WriteLine("未指定 WZ 数据目录（--wz 或环境变量 MINIPET_WZ_DATA）");
    return 2;
}

if (options.AppearancePath == null)
{
    string seed = Path.Combine(DeviceProfile.FindSeedRoot(), "default-appearance.json");
    if (File.Exists(seed)) options.AppearancePath = seed;
    else Console.Error.WriteLine("警告：未找到 seed/default-appearance.json，将跳过纸娃娃导出");
}

Console.WriteLine($"[Exporter] WZ 目录: {wzDataPath}");
var wz = new WzService();
var (ok, err) = wz.LoadWz("", wzDataPath);
if (!ok)
{
    Console.Error.WriteLine($"[Exporter] WZ 加载失败: {WzService.GetErrorText(err ?? WzError.WzPathInvalid)}");
    return 3;
}
Console.WriteLine($"[Exporter] WZ 加载完成（warning: {wz.LastWzLibWarning ?? "无"}）");

try
{
    var exporter = new AssetExporter(wz);
    var summary = exporter.Run(options);

    Console.WriteLine();
    Console.WriteLine($"[Exporter] 导出完成 → {summary.DeviceDir}");
    foreach (var a in summary.Assets.OrderByDescending(a => a.ByteCount))
    {
        Console.WriteLine($"  {a.Kind,-10} {a.Hash:x16}  {a.ByteCount,9} B  {a.Label}");
    }
    foreach (var w in summary.Warnings) Console.WriteLine($"  [warn] {w}");
    foreach (var (mapId, xy) in summary.ClockTable) Console.WriteLine($"  [clock] {mapId} → [{xy[0]},{xy[1]}]");
    Console.WriteLine($"  manifest-assets: {summary.AssetsManifestPath}");
    Console.WriteLine($"  manifest:        {summary.ManifestPath} (rev {summary.Rev})");
    Console.WriteLine($"  合计: {summary.Assets.Count} 个产物, {summary.Assets.Sum(a => a.ByteCount) / 1024.0 / 1024.0:F2} MB");
    return summary.Warnings.Count > 0 ? 1 : 0;
}
catch (Exception ex)
{
    Console.Error.WriteLine($"[Exporter] 失败: {ex}");
    return 4;
}
