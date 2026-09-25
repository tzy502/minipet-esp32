using System.IO;

namespace MinipetServer.Config;

/// <summary>
/// 服务端 data/ 目录布局的唯一事实源（E3：无数据库，一切 JSON 落 data/）。
/// 路径可通过环境变量 MINIPET_DATA_DIR 覆盖（默认 &lt;ContentRoot&gt;/data）。
/// </summary>
public sealed class ServerPaths
{
    public string DataDir { get; }

    public ServerPaths(string dataDir) => DataDir = dataDir;

    public string ConfigDir => Path.Combine(DataDir, "config");
    public string ConfigFile => Path.Combine(ConfigDir, "appsettings.json");

    public string DevicesFile => Path.Combine(DataDir, "devices.json");
    public string QueuesDir => Path.Combine(DataDir, "queues");
    public string RevsFile => Path.Combine(DataDir, "revs.json");
    public string CachedHashesFile => Path.Combine(DataDir, "cached-hashes.json");

    public string ExportRoot => Path.Combine(DataDir, "cache", "export");
    public string ThumbsDir => Path.Combine(DataDir, "cache", "thumbs");

    public string MusicRoot => Path.Combine(DataDir, "music", "wz");
    public string PresetsDir => Path.Combine(DataDir, "presets");
    public string FirmwareDir => Path.Combine(DataDir, "firmware");

    /// <summary>设备导出目录：data/cache/export/{deviceId}/（manifest-assets.json 与 .mpk 包都在此）。</summary>
    public string ExportDirFor(string deviceId) => Path.Combine(ExportRoot, deviceId);

    /// <summary>启动时创建全部运行目录（幂等）。</summary>
    public void EnsureAll()
    {
        Directory.CreateDirectory(DataDir);
        Directory.CreateDirectory(ConfigDir);
        Directory.CreateDirectory(QueuesDir);
        Directory.CreateDirectory(ExportRoot);
        Directory.CreateDirectory(ThumbsDir);
        Directory.CreateDirectory(MusicRoot);
        Directory.CreateDirectory(PresetsDir);
        Directory.CreateDirectory(FirmwareDir);
    }
}
