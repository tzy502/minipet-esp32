using System;
using System.IO;
using System.Text.Json;

namespace MiniPet.Export;

/// <summary>
/// 设备 profile（Server/seed/profiles/*.json）。
/// 视口尺寸决定 BGMAP 的 vw/vh 与导出相机；psramMb/audio/touch 影响包取舍
/// （如冰箱贴 profile strip_count=0）。字段与 seed JSON 一一对应（camelCase）。
/// </summary>
public sealed class DeviceProfile
{
    public string Name { get; set; } = "";
    public int W { get; set; }
    public int H { get; set; }
    public string Shape { get; set; } = "square";
    public int PsramMb { get; set; }
    public bool Audio { get; set; } = true;
    public bool Touch { get; set; } = true;

    /// <summary>条带是否导出（省电型 profile 置 false → BGMAP strip_count=0）。</summary>
    public bool Strips { get; set; } = true;

    public int ViewportW => W > 0 ? W : 480;
    public int ViewportH => H > 0 ? H : 480;

    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    /// <summary>从文件或内置名加载；name 含路径分隔符按路径处理，否则按 seed 目录解析。</summary>
    public static DeviceProfile Load(string nameOrPath, string? seedRoot = null)
    {
        string path = nameOrPath.Contains(Path.DirectorySeparatorChar) || nameOrPath.Contains('/')
            ? nameOrPath
            : Path.Combine(FindSeedRoot(seedRoot), "profiles", $"{nameOrPath}.json");
        if (!File.Exists(path))
            throw new FileNotFoundException($"设备 profile 不存在: {path}");
        var profile = JsonSerializer.Deserialize<DeviceProfile>(File.ReadAllText(path), JsonOpts)
            ?? throw new InvalidDataException($"profile 解析失败: {path}");
        if (string.IsNullOrEmpty(profile.Name)) profile.Name = Path.GetFileNameWithoutExtension(path);
        return profile;
    }

    /// <summary>定位 Server/seed 目录：显式 seedRoot → cwd 向上找（CLI 从仓库任意子目录跑均可）。</summary>
    public static string FindSeedRoot(string? seedRoot = null, string? startDir = null)
    {
        if (!string.IsNullOrEmpty(seedRoot) && Directory.Exists(seedRoot)) return seedRoot;
        var dir = new DirectoryInfo(startDir ?? Environment.CurrentDirectory);
        for (int i = 0; i < 8 && dir != null; i++, dir = dir.Parent!)
        {
            string candidate = Path.Combine(dir.FullName, "Server", "seed");
            if (LooksLikeSeedRoot(candidate)) return candidate;
        }
        // publish / 容器布局（E12）：dotnet publish 把 Server/seed 拷到输出根（ContentRoot=/app）
        // → 向上找 seed/ 而不只是 Server/seed，否则镜像里字体/默认装扮种子全都找不到
        dir = new DirectoryInfo(startDir ?? Environment.CurrentDirectory);
        for (int i = 0; i < 8 && dir != null; i++, dir = dir.Parent!)
        {
            string candidate = Path.Combine(dir.FullName, "seed");
            if (LooksLikeSeedRoot(candidate)) return candidate;
        }
        // 开发期回退：构建输出目录向上通常两级即到仓库根；找不到时给相对路径让调用方报错
        return "Server/seed";
    }

    /// <summary>
    /// 目录像不像 Server/seed：含任一种子标志文件/子目录才算（防把同名普通目录当种子根；
    /// publish 布局由 csproj 的 Content Link 把整套种子拷到输出根 seed/）。
    /// </summary>
    private static bool LooksLikeSeedRoot(string dir)
        => Directory.Exists(dir)
           && (File.Exists(Path.Combine(dir, "default-appearance.json"))
               || Directory.Exists(Path.Combine(dir, "fonts"))
               || Directory.Exists(Path.Combine(dir, "profiles")));
}
