using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using MinipetServer.Config;
using MinipetServer.Device;

namespace MinipetServer.Manifest;

/// <summary>
/// 按设备 manifest（algorithm-asset-format.md 第八节，E13 按设备隔离）：
/// - proto 固定 1；rev 每设备单调递增，持久 data/revs.json（时钟表/换装/固件变更 → rev+1）；
/// - assets 从 data/cache/export/{deviceId}/manifest-assets.json 读（导出器产出），缺则空；
///   条目补 url="/api/device/asset/{hash}"，按设备上报的本地 hash 集回写 cached 标记（E7）；
/// - firmware 从 data/firmware/latest.json {ver} 读；
/// - clock_table 来自 ConfigService.Clock.MapOffsets（配置热更 → 全设备 rev+1 → 设备拉新 manifest）。
/// </summary>
public sealed class DeviceManifestService
{
    public const int Proto = 1;

    private sealed class CachedSet
    {
        public int Version { get; set; }
        public HashSet<string> Hashes { get; set; } = new(StringComparer.Ordinal);
    }

    private sealed class RevsFile
    {
        public Dictionary<string, long> Revs { get; set; } = new(StringComparer.Ordinal);
    }

    private sealed class CachedFile
    {
        public Dictionary<string, CachedSet> Devices { get; set; } = new(StringComparer.Ordinal);
    }

    private readonly ServerPaths _paths;
    private readonly ConfigService _cfg;
    private readonly DeviceRegistry _registry;
    private readonly object _gate = new();
    private Dictionary<string, long> _revs = new(StringComparer.Ordinal);
    private Dictionary<string, CachedSet> _cached = new(StringComparer.Ordinal);

    public DeviceManifestService(ServerPaths paths, ConfigService cfg, DeviceRegistry registry)
    {
        _paths = paths;
        _cfg = cfg;
        _registry = registry;
        _revs = StorageUtil.ReadJson<RevsFile>(paths.RevsFile)?.Revs ?? new Dictionary<string, long>(StringComparer.Ordinal);
        _cached = StorageUtil.ReadJson<CachedFile>(paths.CachedHashesFile)?.Devices
                  ?? new Dictionary<string, CachedSet>(StringComparer.Ordinal);
        cfg.Changed += OnConfigChanged;
    }

    /// <summary>时钟表（或任何设备相关段）热更 → 全设备 rev+1（E9：设备拉新 manifest 生效）。</summary>
    private void OnConfigChanged(ConfigChangedEventArgs e)
    {
        var oldClock = JsonSerializer.Serialize(e.Old.Clock.MapOffsets, StorageUtil.JsonOpts);
        var newClock = JsonSerializer.Serialize(e.New.Clock.MapOffsets, StorageUtil.JsonOpts);
        if (string.Equals(oldClock, newClock, StringComparison.Ordinal)) return;
        BumpAllRev("clock_table 变更");
    }

    public long GetCurrentRev(string deviceId)
    {
        lock (_gate) return _revs.GetValueOrDefault(deviceId, 1);
    }

    public long BumpRev(string deviceId, string reason)
    {
        lock (_gate)
        {
            var rev = _revs.GetValueOrDefault(deviceId, 1) + 1;
            _revs[deviceId] = rev;
            SaveRevsLocked();
            return rev;
        }
    }

    public long BumpAllRev(string reason)
    {
        lock (_gate)
        {
            var ids = _revs.Keys
                .Union(_registry.List().Select(d => d.DeviceId), StringComparer.Ordinal)
                .ToList();
            long max = 0;
            foreach (var id in ids)
            {
                _revs[id] = _revs.GetValueOrDefault(id, 1) + 1;
                max = Math.Max(max, _revs[id]);
            }
            SaveRevsLocked();
            return max;
        }
    }

    public int GetCachedVersion(string deviceId)
    {
        lock (_gate) return _cached.GetValueOrDefault(deviceId)?.Version ?? 0;
    }

    /// <summary>设备经 POST /api/device/event 上报本地 hash 集 → 服务端记录（manifest cached 标记的计算源，E7）。</summary>
    public void SetCachedHashes(string deviceId, IEnumerable<string> hashes)
    {
        var set = new HashSet<string>(hashes.Where(h => !string.IsNullOrWhiteSpace(h)), StringComparer.Ordinal);
        lock (_gate)
        {
            if (_cached.TryGetValue(deviceId, out var cur) && cur.Hashes.SetEquals(set)) return;
            _cached[deviceId] = new CachedSet { Version = (cur?.Version ?? 0) + 1, Hashes = set };
            SaveCachedLocked();
        }
    }

    /// <summary>构建 manifest JSON（不落盘，按需生成；assets 缺失时为空对象）。</summary>
    public string BuildManifestJson(string deviceId, out long rev, out int cachedVersion)
    {
        CachedSet? cached;
        lock (_gate)
        {
            rev = _revs.GetValueOrDefault(deviceId, 1);
            cached = _cached.GetValueOrDefault(deviceId);
            cachedVersion = cached?.Version ?? 0;
        }

        var root = new JsonObject
        {
            ["proto"] = Proto,
            ["rev"] = rev,
        };

        // assets：来自导出器 manifest-assets.json；补 url、按设备 cached 集标记
        var assets = LoadAssetsNode(deviceId);
        if (cached != null && cached.Hashes.Count > 0 && assets is { Count: > 0 })
        {
            foreach (var (hash, node) in assets)
            {
                if (node is not JsonObject entry) continue;
                entry["url"] = $"/api/device/asset/{hash}";
                if (cached.Hashes.Contains(hash)) entry["cached"] = true;
                else entry.Remove("cached");
            }
        }
        else if (assets is { Count: > 0 })
        {
            foreach (var (hash, node) in assets)
            {
                if (node is JsonObject entry)
                {
                    entry["url"] = $"/api/device/asset/{hash}";
                    entry.Remove("cached");
                }
            }
        }
        // DeepCopy：assets 来自刚解析的 manifest-assets.json，节点还挂着原父，直接再挂会抛
        // "The node already has a parent"；深拷贝断链后再进 manifest。
        root["assets"] = assets?.DeepClone() ?? new JsonObject();

        // firmware：data/firmware/latest.json {"ver":"0.3.1"}
        var latestFile = Path.Combine(_paths.FirmwareDir, "latest.json");
        try
        {
            if (File.Exists(latestFile))
            {
                var latest = JsonNode.Parse(File.ReadAllText(latestFile))?["ver"]?.GetValue<string>();
                if (!string.IsNullOrWhiteSpace(latest))
                    root["firmware"] = new JsonObject { ["ver"] = latest, ["url"] = $"/api/device/firmware/{latest}.bin" };
            }
        }
        catch { /* latest.json 损坏 → 无 firmware 项 */ }

        // clock_table：E9 魔法值表（Web 可改 → rev+1）
        var clock = new JsonObject();
        foreach (var (mapId, xy) in _cfg.Current.Clock.MapOffsets)
        {
            clock[mapId] = new JsonArray(xy.Select(x => (JsonNode)x!).ToArray());
        }
        root["clock_table"] = clock;

        return root.ToJsonString(new JsonSerializerOptions
        {
            WriteIndented = true,
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping, // 中文 label 不转义
        });
    }

    /// <summary>
    /// 素材包查找（GET asset/{hash}）：先 data/cache/export/{deviceId}/，再全局共享
    /// （字体/时钟数字等共享包）；文件名以 hash 开头、非 .json。
    /// </summary>
    public string? FindAssetFile(string? deviceId, string hash)
    {
        if (string.IsNullOrWhiteSpace(hash)) return null;
        if (!string.IsNullOrEmpty(deviceId))
        {
            var dir = _paths.ExportDirFor(deviceId);
            var hit = FindInDir(dir, hash);
            if (hit != null) return hit;
        }
        if (Directory.Exists(_paths.ExportRoot))
        {
            foreach (var sub in Directory.EnumerateDirectories(_paths.ExportRoot))
            {
                var hit = FindInDir(sub, hash);
                if (hit != null) return hit;
            }
        }
        return null;
    }

    /// <summary>固件 bin 路径（GET firmware/{ver}.bin / OTA 下发 url 的落地文件）。</summary>
    public string? GetFirmwareFile(string ver)
    {
        if (string.IsNullOrWhiteSpace(ver)) return null;
        if (!System.Text.RegularExpressions.Regex.IsMatch(ver, @"^[A-Za-z0-9._-]+$")) return null;
        var file = Path.Combine(_paths.FirmwareDir, ver + ".bin");
        return File.Exists(file) ? file : null;
    }

    private static string? FindInDir(string dir, string hash)
    {
        if (!Directory.Exists(dir)) return null;
        try
        {
            return Directory.EnumerateFiles(dir, hash + "*", SearchOption.AllDirectories)
                .FirstOrDefault(f => !f.EndsWith(".json", StringComparison.OrdinalIgnoreCase)
                                     && !f.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase));
        }
        catch { return null; }
    }

    private JsonObject? LoadAssetsNode(string deviceId)
    {
        var file = Path.Combine(_paths.ExportDirFor(deviceId), "manifest-assets.json");
        if (!File.Exists(file)) return null;
        try
        {
            var node = JsonNode.Parse(File.ReadAllText(file));
            if (node is JsonObject o && o["assets"] is JsonObject assets) return assets;
            if (node is JsonObject direct) return direct; // 兼容直接就是 assets 映射的形态
            return null;
        }
        catch { return null; }
    }

    private void SaveRevsLocked()
    {
        var json = JsonSerializer.Serialize(new RevsFile { Revs = _revs }, StorageUtil.JsonOpts);
        StorageUtil.AtomicWriteAllText(_paths.RevsFile, json);
    }

    private void SaveCachedLocked()
    {
        var json = JsonSerializer.Serialize(new CachedFile { Devices = _cached }, StorageUtil.JsonOpts);
        StorageUtil.AtomicWriteAllText(_paths.CachedHashesFile, json);
    }
}
