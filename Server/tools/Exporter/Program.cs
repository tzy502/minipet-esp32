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
    // 分块（tiled）布局：**默认开**（仅整图包生效）——新导出直接是 128×128 瓦片口径，
    // 固件按块连续读（SD 顺序 1336KB/s vs 跨行距逐行 ~130KB/s）。
    // --legacy-rows 关掉它做逐行对拍/回退。
    Tiled = true,
};

string? wzDataPath = Environment.GetEnvironmentVariable("MINIPET_WZ_DATA");
string? dumpFootholds = null;   // --dump-footholds <mapId>：只打印地面层折线（固件降级表用，不导出资产）

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
        case "--dump-footholds": dumpFootholds = Next(); break;
        case "--full-map": options.FullMap = true; break;   // R2 整图口径（默认关 = 240×240 窗口包不变）
        case "--tiled": options.Tiled = true; break;        // 分块（128×128 瓦片）布局；**已默认开**，此处显式声明
        case "--legacy-rows": options.Tiled = false; break; // 关分块 → 旧逐行布局（对拍/回退用）
        case "--help" or "-h":
            Console.WriteLine("""
                用法: Exporter [--appearance <json>] [--profile <名|路径>] [--maps <id,...>] [--out <dir>]
                       [--device-id <id>] [--wz <WZ数据目录>] [--fonts 16,24,32|--no-fonts]
                       [--no-audio] [--no-fonttime] [--firmware <ver>] [--charset <file>] [--font-family <名>]
                       [--dump-footholds <mapId>] [--full-map] [--tiled|--legacy-rows]
                默认: profile=amoled216  out=data/cache/export  fonts=16  audio=on  fontTime=on
                WZ 目录: --wz 或环境变量 MINIPET_WZ_DATA
                --dump-footholds: 只打印该图 foothold 第 0 层（地面层）+ 设备视口相机换算，
                                  并输出可直接粘进固件的 C 数组（不导出任何资产）
                --full-map: R2 整图口径（vw/vh = 整图 1x 世界尺寸 + 条带 y 世界系 + 尾部地面表扩展块）；
                            **默认关**（现网 240×240 窗口包逐字节不变）。整图包体量 MB 级。
                --tiled:    分块（tiled）布局（**默认开**，需 --full-map 才生效）：static / tile（含掩码）/
                            条带三类层改 128×128 世界像素瓦片存储，尾扩展块 flags bit1=1；
                            固件按块连续读（SD 顺序 1336KB/s vs 跨行距逐行 ~130KB/s）。
                            契约 docs/ai/map-tiled-format-contract.md。
                --legacy-rows: 关分块 → 旧逐行布局（整图包 flags bit1=0）。同一张图分别用两种口径
                            导出到不同 --out 目录，即可用 Server/tools/bgmap-tiled-verify.py 逐像素对拍。
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

// ── --dump-footholds <mapId>：固件「降级地面表」生成（只读 WZ，不导出资产、不动服务端）──
if (!string.IsNullOrEmpty(dumpFootholds))
{
    var mapSvc = new MapService(wz, new CacheManager());
    var mi = mapSvc.LoadMap(dumpFootholds);
    if (mi == null) { Console.Error.WriteLine($"[dump] 地图加载失败: {dumpFootholds}"); return 5; }

    var prof = DeviceProfile.Load("amoled216");
    int vw = Math.Max(1, prof.ViewportW / Math.Max(1, PlacementMath.Scale));
    int vh = Math.Max(1, prof.ViewportH / Math.Max(1, PlacementMath.Scale));
    var (ccx, ccy) = MapService.GetMapCenter(mi);
    var (camX, camY) = MapService.ClampCamera(mi, ccx, ccy, 1f, vw, vh);
    float ox = camX - vw / 2f, oy = camY - vh / 2f;      // 视口左上角世界坐标（1x）

    Console.WriteLine($"[dump] map={mi.Id} 视口 {vw}x{vh} 相机中心=({camX:0.##},{camY:0.##}) 视口原点世界=({ox:0.##},{oy:0.##})");
    Console.WriteLine($"[dump] foothold 总数={mi.Footholds.Count} 第0层(地面)={mi.Footholds.Count(f => f.Layer == 0)}");
    Console.WriteLine($"[dump] 地图包围盒 X[{mi.MinX},{mi.MaxX}] Y[{mi.MinY},{mi.MaxY}] "
                    + $"VR X[{mi.VRLeft},{mi.VRRight}] Y[{mi.VRTop},{mi.VRBottom}]");
    Console.WriteLine($"[dump] GetMapCenter=({ccx:0.##},{ccy:0.##}) 未夹取；夹取后=({camX:0.##},{camY:0.##})");
    var g0 = mi.Footholds.Where(f => f.Layer == 0).ToList();
    if (g0.Count > 0)
        Console.WriteLine($"[dump] 地面层 y 范围 [{g0.Min(f => Math.Min(f.Y1, f.Y2))},{g0.Max(f => Math.Max(f.Y1, f.Y2))}] "
                        + $"x 范围 [{g0.Min(f => Math.Min(f.X1, f.X2))},{g0.Max(f => Math.Max(f.X1, f.X2))}]");
    Console.WriteLine("[dump] 各层分布: " + string.Join(" ", mi.Footholds.GroupBy(f => f.Layer)
        .OrderBy(g => g.Key).Select(g => $"L{g.Key}={g.Count()}")));

    var ground = mi.Footholds.Where(f => f.Layer == 0).ToList();
    if (ground.Count == 0) ground = mi.Footholds;
    var rows = new List<(int x1, int y1, int x2, int y2)>();
    foreach (var f in ground)
    {
        int x1 = Math.Min(f.X1, f.X2), x2 = Math.Max(f.X1, f.X2);
        if (x2 < ox - vw || x1 > ox + 2 * vw) continue;   // 视口两侧各留一屏，便于拖动
        int y1 = (f.X1 <= f.X2) ? f.Y1 : f.Y2;
        int y2 = (f.X1 <= f.X2) ? f.Y2 : f.Y1;
        rows.Add(((int)Math.Round(x1 - ox), (int)Math.Round(y1 - oy),
                  (int)Math.Round(x2 - ox), (int)Math.Round(y2 - oy)));
    }
    Console.WriteLine($"[dump] 视口内地面段 {rows.Count} 条（视口 1x 屏幕坐标，固件 ×2）:");
    foreach (var r in rows) Console.WriteLine($"[dump]   {{{r.x1}, {r.y1}, {r.x2}, {r.y2}}},");
    int? gyWorld = MapService.GetGroundY(mi, (int)Math.Round(camX));
    Console.WriteLine("[dump] 中列地面 y(1x 视口) = " + (gyWorld is int gy ? (gy - oy).ToString("0.##") : "null"));
    // 【相机实测】把整层渲染在若干候选 camY 上各出一张 240x240，供主机端与设备实际画面比对，
    // 用来判定"设备那 240x240 到底取的是世界哪一块"（= 导出相机到底是多少）
    for (int cy = -160; cy <= 300; cy += 10)
    {
        var cand = mapSvc.RenderViewport(mi, camX, camY + cy, 1f, 0, vw, vh);
        if (cand == null) continue;
        using var cfs = File.Create($"/tmp/camy_{cy + 1000}.png");
        cand.Encode(cfs, SkiaSharp.SKEncodedImageFormat.Png, 90);
    }
    Console.WriteLine("[dump] 候选相机渲染 → /tmp/camy_*.png");

    // 【地面带烘焙】当前导出视口(240x240) 只到世界 y=131.5，而地面 foothold 在 y≈245.5：
    // 把相机不动、加高视口到 500 行（视差仍按原相机算）→ 裁出缺失的下方那一条，
    // 供固件"相机下移"实验直接内置（纯固件，不走 NAS/换卡）。
    {
        int tall = 500;
        var tv = mapSvc.RenderViewport(mi, camX, camY, 1f, 0, vw, tall);
        if (tv != null)
        {
            int topRow = (int)Math.Round(oy + vh - oy + 0);          // 视口底行 = 世界 oy+vh
            // 目标：世界 y ∈ [oy+vh, oy+vh+130) 那 130 行（= 屏幕下沿再往下 130 世界像素）
            int wantWorldTop = (int)Math.Round(oy + vh);
            int cropY = (int)Math.Round(wantWorldTop - (camY - tall / 2f));
            if (cropY < 0) cropY = 0;
            int cropH = 130;
            if (cropY + cropH > tall) cropH = tall - cropY;
            var band = new SkiaSharp.SKBitmap(vw, cropH);
            using (var cv = new SkiaSharp.SKCanvas(band))
                cv.DrawBitmap(tv, new SkiaSharp.SKRect(0, cropY, vw, cropY + cropH),
                              new SkiaSharp.SKRect(0, 0, vw, cropH));
            using (var bfs = File.Create("/tmp/ground_band.png"))
                band.Encode(bfs, SkiaSharp.SKEncodedImageFormat.Png, 100);
            // RGB565 little-endian 原始数据（固件直接当 uint16 数组用，×2 展开到设备像素）
            var raw = new byte[vw * cropH * 2];
            int o = 0;
            for (int yy = 0; yy < cropH; yy++)
                for (int xx = 0; xx < vw; xx++)
                {
                    var c = band.GetPixel(xx, yy);
                    ushort v = (ushort)(((c.Red >> 3) << 11) | ((c.Green >> 2) << 5) | (c.Blue >> 3));
                    raw[o++] = (byte)(v & 0xFF); raw[o++] = (byte)(v >> 8);
                }
            Directory.CreateDirectory("/Users/<USER>/IdeaProjects/minipet-esp32/Firmware/main/render/data");
            File.WriteAllBytes("/Users/<USER>/IdeaProjects/minipet-esp32/Firmware/main/render/data/ground_band_000010000.bin", raw);
            Console.WriteLine($"[dump] 地面带 {vw}x{cropH} → /tmp/ground_band.png + 固件内置 bin ({raw.Length} B)");
        }
    }

    // 整图渲染（世界坐标 1:1 起于 MinX/MinY）→ 用于"设备那 240x240 到底取的是世界哪一块"的实测比对
    {
        int ww = mi.MaxX - mi.MinX, wh = mi.MaxY - mi.MinY;
        var whole = mapSvc.RenderViewport(mi, mi.MinX + ww / 2f, mi.MinY + wh / 2f, 1f, 0, ww, wh);
        if (whole != null)
        {
            using var wfs = File.Create("/tmp/whole_map.png");
            whole.Encode(wfs, SkiaSharp.SKEncodedImageFormat.Png, 90);
            Console.WriteLine($"[dump] 整图 {ww}x{wh} → /tmp/whole_map.png（世界原点 MinX={mi.MinX} MinY={mi.MinY}）");
        }
    }
    // 把 foothold 画在视口渲染图上（红线），用于肉眼核对"真地面 vs 画面里的草地"对不对
    mapSvc.MapShowFoothold = true;
    var ov = mapSvc.RenderViewport(mi, camX, camY, 1f, 0, vw, vh);
    if (ov != null)
    {
        using var fs = File.Create("/tmp/fh_overlay.png");
        ov.Encode(fs, SkiaSharp.SKEncodedImageFormat.Png, 100);
    }
    Console.WriteLine("[dump] foothold 叠加图 → /tmp/fh_overlay.png");
    mapSvc.MapShowFoothold = false;
    Console.WriteLine("static const MpFoothold kFh[] = {");
    foreach (var r in rows) Console.WriteLine($"    {{ {r.x1}, {r.y1}, {r.x2}, {r.y2} }},");
    Console.WriteLine("};");
    return 0;
}

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
