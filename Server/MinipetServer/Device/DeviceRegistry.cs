using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using MinipetServer.Config;

namespace MinipetServer.Device;

// ── 设备表模型（E13）───────────────────────────────────────────────────────
// deviceId → { profile, petConfig（装扮JSON，按设备隔离）, bgm 偏好, 阈值, 名称, 固件版本, lastSeen }

public sealed class DeviceProfile
{
    public int W { get; set; }
    public int H { get; set; }
    public string Shape { get; set; } = "";
    public int Psram { get; set; }
    public bool Audio { get; set; }
}

public sealed class DeviceBgmPrefs
{
    public string Source { get; set; } = "wz";
    public int Volume { get; set; } = 60;
    /// <summary>
    /// 最近一次「点播」的曲目（源内 key，WZ 源 = 库内相对路径）。
    /// 【Web 点歌 2026-09-27】用户报障「页面 bgm 没有选择歌曲的地方」——曲库页新增点播按钮后，
    /// 选中曲目落在这里，页面刷新/重进仍能显示「当前曲目」；设备端现场切歌会经
    /// POST /api/device/bgm/cmd 回写覆盖（该端点已带 trackId）。
    /// </summary>
    public string? TrackId { get; set; }
    /// <summary>点播曲名（展示用；避免页面为一行标题再拉一次全库）。</summary>
    public string? TrackTitle { get; set; }
}

public sealed class DeviceRecord
{
    public string DeviceId { get; set; } = "";
    public string Uuid { get; set; } = "";
    public string Name { get; set; } = "";
    public bool Paired { get; set; }
    public DeviceProfile? Profile { get; set; }
    /// <summary>纸娃娃装扮 JSON（自由结构 {mapId, appearance{...}}；E13 按设备隔离，A 设备换装不影响 B）。</summary>
    public JsonElement? PetConfig { get; set; }
    public DeviceBgmPrefs Bgm { get; set; } = new();
    /// <summary>按设备阈值覆盖；null = 跟随全局配置（ConfigService.Device）。</summary>
    public DeviceThresholdsConfig? Thresholds { get; set; }
    /// <summary>按设备台词气泡覆盖（E12）；null = 跟随全局配置（ConfigService.Speech）。
    /// 供多设备差异化台词用（E13 每设备独立配置）；Web 侧暂未编辑此字段，手改 devices.json 即生效
    ///（SpeechScheduler 每拍读设备表快照，无需重启）。</summary>
    public SpeechConfig? Speech { get; set; }
    public string Firmware { get; set; } = "";
    public DateTime? LastSeenUtc { get; set; }
    public DateTime CreatedAtUtc { get; set; } = DateTime.UtcNow;
}

public sealed class PairingEntry
{
    public string Code { get; set; } = "";
    public string DeviceId { get; set; } = "";
    public DateTime ExpiresAtUtc { get; set; }
}

public sealed class DeviceStore
{
    public List<DeviceRecord> Devices { get; set; } = new();
    public List<PairingEntry> Pairings { get; set; } = new();
}

/// <summary>
/// 设备表（E13）：data/devices.json JSON CRUD，ReaderWriterLockSlim 保护，
/// temp + File.Move 原子写。配对码 6 位、10 分钟过期（屏显 → Web 输入完成绑定命名）。
/// Touch（lastSeen 心跳）只改内存不落盘，避免 poll 高频写放大；真正变更时才 Save。
/// </summary>
public sealed class DeviceRegistry
{
    public static readonly TimeSpan PairingTtl = TimeSpan.FromMinutes(10);
    /// <summary>在线判定窗口：poll 长轮询 ≤55s + 余量。</summary>
    public static readonly TimeSpan OnlineWindow = TimeSpan.FromSeconds(90);

    private readonly ServerPaths _paths;
    private readonly ReaderWriterLockSlim _rw = new();
    private DeviceStore _store = new();

    public DeviceRegistry(ServerPaths paths)
    {
        _paths = paths;
        Directory.CreateDirectory(paths.DataDir);
        var loaded = StorageUtil.ReadJson<DeviceStore>(paths.DevicesFile);
        if (loaded != null)
        {
            loaded.Devices ??= new List<DeviceRecord>();
            loaded.Pairings ??= new List<PairingEntry>();
            _store = loaded;
            PruneExpiredPairingsLocked();
        }
    }

    // ── 查询 ──────────────────────────────────────────────────────────────

    public DeviceRecord? Get(string deviceId)
    {
        _rw.EnterReadLock();
        try
        {
            return _store.Devices.FirstOrDefault(d => string.Equals(d.DeviceId, deviceId, StringComparison.Ordinal)) is { } found
                ? Clone(found) : null;
        }
        finally { _rw.ExitReadLock(); }
    }

    /// <summary>
    /// 设备列表（2026-10-01 排序定稿）：**当前在线排最前**，同组内按 lastSeen 倒序
    /// （最近连接的靠前）；离线设备沉底。理由：Web「设备」页与手机端都以"我现在
    /// 要操作哪台"为第一诉求，在线且刚连过的必须一眼可见（用户口径：保持连接的
    /// 排序在前面）。排序在服务端统一做，前端不再自己排（两端观感一致）。
    /// </summary>
    public List<DeviceRecord> List()
    {
        _rw.EnterReadLock();
        try
        {
            var now = DateTime.UtcNow;
            return _store.Devices
                .Select(Clone)
                .OrderByDescending(d => IsOnline(d, now))          // 在线优先
                .ThenByDescending(d => d.LastSeenUtc ?? DateTime.MinValue)  // 最近连接优先
                .ThenBy(d => d.Name, StringComparer.Ordinal)       // 稳定兜底（避免同秒抖动）
                .ToList();
        }
        finally { _rw.ExitReadLock(); }
    }

    /// <summary>
    /// 删除设备（用户口径："删除链接过的硬件"）——把该设备从设备表里摘掉：
    ///   · 仅删登记信息（DeviceRecord）；配对码、指令队列由各自服务按 deviceId
    ///     自然失效，不需要级联清理；
    ///   · 已落盘的导出资产目录（data/cache/export/&lt;deviceId&gt;）**保留**——设备
    ///     若再次上报会用同一个 deviceId 复用，删了反而要重烘全部素材；
    ///   · 设备若还在线，下次 poll 会因 Get 返回 null 被拒（404 device_not_found），
    ///     即"删掉后立刻掉线"，这是预期行为（要重新接入必须重新 hello）。
    /// 返回 true = 确实删掉了一条。
    /// </summary>
    public bool Remove(string deviceId)
    {
        _rw.EnterWriteLock();
        try
        {
            int n = _store.Devices.RemoveAll(d => string.Equals(d.DeviceId, deviceId, StringComparison.Ordinal));
            if (n > 0) SaveLocked();
            return n > 0;
        }
        finally { _rw.ExitWriteLock(); }
    }

    public static bool IsOnline(DeviceRecord d, DateTime? now = null)
        => d.LastSeenUtc is { } seen && (now ?? DateTime.UtcNow) - seen < OnlineWindow;

    // ── 注册 / 心跳（hello）───────────────────────────────────────────────

    /// <summary>hello 入口：按 UUID 幂等注册（匿名可用，配对只解锁 Web 管理）。</summary>
    public DeviceRecord GetOrCreateByUuid(string uuid, DeviceProfile? profile, string? firmware)
    {
        var u = uuid.Trim();
        _rw.EnterWriteLock();
        try
        {
            var dev = _store.Devices.FirstOrDefault(d => string.Equals(d.Uuid, u, StringComparison.OrdinalIgnoreCase));
            if (dev == null)
            {
                dev = new DeviceRecord
                {
                    DeviceId = NewDeviceId(),
                    Uuid = u,
                    Name = "",
                    Paired = false,
                    Profile = profile,
                    Firmware = firmware ?? "",
                    LastSeenUtc = DateTime.UtcNow,
                    CreatedAtUtc = DateTime.UtcNow,
                };
                _store.Devices.Add(dev);
                SaveLocked();
            }
            else
            {
                if (profile != null) dev.Profile = profile;
                if (!string.IsNullOrEmpty(firmware)) dev.Firmware = firmware;
                dev.LastSeenUtc = DateTime.UtcNow;
            }
            return Clone(dev);
        }
        finally { _rw.ExitWriteLock(); }
    }

    /// <summary>心跳：只改内存 lastSeen（poll/event/bgm 等高频入口）。</summary>
    public void Touch(string deviceId)
    {
        _rw.EnterWriteLock();
        try
        {
            var dev = _store.Devices.FirstOrDefault(d => string.Equals(d.DeviceId, deviceId, StringComparison.Ordinal));
            if (dev != null) dev.LastSeenUtc = DateTime.UtcNow;
        }
        finally { _rw.ExitWriteLock(); }
    }

    /// <summary>变更（落盘）。返回更新后的拷贝；设备不存在返回 null。</summary>
    public DeviceRecord? Update(string deviceId, Action<DeviceRecord> mutate)
    {
        _rw.EnterWriteLock();
        try
        {
            var dev = _store.Devices.FirstOrDefault(d => string.Equals(d.DeviceId, deviceId, StringComparison.Ordinal));
            if (dev == null) return null;
            mutate(dev);
            SaveLocked();
            return Clone(dev);
        }
        finally { _rw.ExitWriteLock(); }
    }

    // ── 配对（E13：屏显 6 位码 → Web 输入绑定命名）──────────────────────

    /// <summary>给设备签发配对码（6 位数字，10 分钟过期）；已有未过期码直接复用（避免屏显跳变）。</summary>
    public string IssuePairingCode(string deviceId)
    {
        _rw.EnterWriteLock();
        try
        {
            PruneExpiredPairingsLocked();
            var existing = _store.Pairings.FirstOrDefault(p => p.DeviceId == deviceId);
            if (existing != null) return existing.Code;

            var active = _store.Pairings.Select(p => p.Code).ToHashSet(StringComparer.Ordinal);
            string code;
            do code = RandomNumberGenerator.GetInt32(0, 1_000_000).ToString("D6");
            while (active.Contains(code));

            _store.Pairings.Add(new PairingEntry
            {
                Code = code,
                DeviceId = deviceId,
                ExpiresAtUtc = DateTime.UtcNow + PairingTtl,
            });
            SaveLocked();
            return code;
        }
        finally { _rw.ExitWriteLock(); }
    }

    /// <summary>Web 侧凭码绑定：校验有效期 → paired=true + 命名。失败返回 null。</summary>
    public DeviceRecord? TryPair(string code, string name)
    {
        if (string.IsNullOrWhiteSpace(code)) return null;
        _rw.EnterWriteLock();
        try
        {
            PruneExpiredPairingsLocked();
            var entry = _store.Pairings.FirstOrDefault(p => string.Equals(p.Code, code.Trim(), StringComparison.Ordinal));
            if (entry == null) return null;
            var dev = _store.Devices.FirstOrDefault(d => d.DeviceId == entry.DeviceId);
            _store.Pairings.Remove(entry);
            if (dev == null) return null;
            dev.Paired = true;
            if (!string.IsNullOrWhiteSpace(name)) dev.Name = name.Trim();
            SaveLocked();
            return Clone(dev);
        }
        finally { _rw.ExitWriteLock(); }
    }

    /// <summary>设备当前未消费的配对码（hello 幂等返回用）；过期返回 null。</summary>
    public string? ActivePairingCode(string deviceId)
    {
        _rw.EnterReadLock();
        try
        {
            return _store.Pairings.FirstOrDefault(p => p.DeviceId == deviceId && p.ExpiresAtUtc > DateTime.UtcNow)?.Code;
        }
        finally { _rw.ExitReadLock(); }
    }

    // ── 内部 ──────────────────────────────────────────────────────────────

    private void PruneExpiredPairingsLocked()
    {
        var now = DateTime.UtcNow;
        _store.Pairings.RemoveAll(p => p.ExpiresAtUtc <= now);
    }

    private void SaveLocked()
    {
        PruneExpiredPairingsLocked();
        var json = JsonSerializer.Serialize(_store, StorageUtil.JsonOpts);
        StorageUtil.AtomicWriteAllText(_paths.DevicesFile, json);
    }

    private static string NewDeviceId()
    {
        Span<byte> buf = stackalloc byte[3];
        RandomNumberGenerator.Fill(buf);
        return "dev-" + Convert.ToHexString(buf).ToLowerInvariant();
    }

    private static DeviceRecord Clone(DeviceRecord d) =>
        JsonSerializer.Deserialize<DeviceRecord>(JsonSerializer.Serialize(d, StorageUtil.JsonOpts), StorageUtil.JsonOpts) ?? d;
}
