using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using MinipetServer.Device;

namespace MinipetServer.Config;

// ── 配置模型（E3 / 软件设计 2.4）───────────────────────────────────────────
// 单一来源 data/config/appsettings.json；端口属部署层 .env，不入 appsettings（无端口项）。
// 注释策略：_comment 键约定（.NET 无 JSON5，序列化模型内嵌注释属性，读写往返稳定）。

public sealed class WzConfig
{
    [JsonPropertyName("_comment")]
    public string Comment { get; set; } = "冒险岛 WZ 数据目录（Data 目录绝对路径；容器内默认 /wz/Data，开源用户自定义）";

    public string DataPath { get; set; } = "/wz/Data";
}

public sealed class QqMusicConfig
{
    [JsonPropertyName("_comment")]
    public string Comment { get; set; } = "QQ 音乐音源：Enabled=总开关（停用则该源对设备置灰）；Cookie=网页版 cookie（Web 导入，CookieSavedAtUtc 记导入时间，超 7 天告警）；GatewayPort=容器内 node 网关端口；GatewayScript=网关入口脚本绝对路径（空=自动探测，见 QqGatewayProcess）";

    public bool Enabled { get; set; } = false;
    public string Cookie { get; set; } = "";
    public int GatewayPort { get; set; } = 3300;

    /// <summary>
    /// QQ 曲库列表用的搜索关键词（QQ 无"全库"概念：/music/tracks?source=qq 走网关 /search）。
    /// 空 = 曲库列表为空（不臆造默认歌单）；设备端 next/prev 依赖该列表。
    /// </summary>
    public string SearchKeyword { get; set; } = "";

    /// <summary>
    /// cookie 导入时间（UTC）——POST /api/admin/music/sources/qq/cookie 写入；清空 cookie 时置 null。
    /// /api/admin/music/sources 据此算 cookieStale（超过 7 天 = QQ 网页 cookie 大概率已失效，E4 告警）。
    /// </summary>
    public DateTime? CookieSavedAtUtc { get; set; }

    /// <summary>
    /// node 网关入口脚本绝对路径（如 /app/qq-gateway/index.js）。空 = 自动探测：
    /// MINIPET_QQ_GATEWAY 环境变量 → data/qq-gateway/index.js → 应用目录 qq-gateway/index.js。
    /// 网关本体（Rain120/qq-music-api + 适配层，见 Music/QqGatewayClient.cs 顶部契约注释）
    /// 不在本仓库、镜像也未内置 → 探测不到时 qq 源恒 Degraded 并给出明确原因（不造假实现）。
    /// </summary>
    public string GatewayScript { get; set; } = "";
}

public sealed class BgmConfig
{
    [JsonPropertyName("_comment")]
    public string Comment { get; set; } = "BGM 默认音源：wz | qq（设备未显式选源时用；E8 禁止跨源自动换歌）";

    public string DefaultSource { get; set; } = "wz";
}

public sealed class DeviceThresholdsConfig
{
    [JsonPropertyName("_comment")]
    public string Comment { get; set; } = "设备阈值：IMU 灵敏度 / 死区（度）/ 轻拍（g）/ 重拍（g）/ 无人交互转待机时钟（分钟）——设备表可按设备覆盖";

    /// <summary>
    /// IMU 灵敏度（E4 设置页要求；Web 侧已就绪并探测此字段启用输入框）：
    /// 倍率，有效阈值 = 阈值 ÷ 灵敏度。1.0 = 出厂口径；&gt;1 更灵敏，&lt;1 更迟钝。
    /// </summary>
    public double ImuSensitivity { get; set; } = 1.0;

    public double ImuDeadzoneDeg { get; set; } = 8;
    public double TapLightG { get; set; } = 2.0;
    public double TapHardG { get; set; } = 4.0;
    public int IdleToClockMin { get; set; } = 5;
}

/// <summary>
/// 随机台词气泡（E12）：静置 idleSec 秒后由服务端挑一条 lines 下发 bubble 指令。
/// Web 设置页已就绪（探测 config.speech 是否存在决定整卡启用/禁用）。
/// </summary>
public sealed class SpeechConfig
{
    [JsonPropertyName("_comment")]
    public string Comment { get; set; } = "随机台词气泡（E12）：enabled 开关 / idleSec 静置秒数 / lines 台词库（单条 UTF-8 ≤95 字节，固件 mp_cmd_t.s=char[96]）";

    public bool Enabled { get; set; }
    public int IdleSec { get; set; } = 300;
    public List<string> Lines { get; set; } = new();
}

public sealed class ClockConfig
{
    [JsonPropertyName("_comment")]
    public string Comment { get; set; } = "地图时钟位置表（E9 魔法值）：mapId → 烘焙视口内屏幕坐标 [x,y]；Web 可改 → manifest rev+1 → 设备拉新 manifest 生效";

    public Dictionary<string, int[]> MapOffsets { get; set; } = new(StringComparer.Ordinal);

    /// <summary>校准口径版本（ClockTableSeeder 写入）：小于当前口径时启动校准全量重算覆盖，
    /// 等于时只补缺失条目（Web 手改的值永不覆盖）。Web 保存回传请原样携带。</summary>
    public int Calib { get; set; }
}

/// <summary>
/// mDNS 服务广告（E14「设备可发现服务端」）：设备在配网页服务器地址留空时，
/// 靠 _minipet._tcp 组播发现本服务端。实现见 Services/MdnsAdvertiser.cs。
/// 缺字段即默认（Enabled=true / Port=0 自动 / Interface 空=自动），老配置文件无需迁移。
/// </summary>
public sealed class MdnsConfig
{
    [JsonPropertyName("_comment")]
    public string Comment { get; set; } = "mDNS 服务广告（E14）：Enabled=总开关（缺字段=开，配网页留空的设备靠它自动发现）；Port=广告端口（0=自动：MINIPET_PORT 环境变量 → 实际监听端口 → 38090；容器内监听 8080、对外 38090 时应显式填对外端口或给容器传 MINIPET_PORT）；Interface=只在该本机 IPv4 所在网卡上广告（空=自动：优先有默认网关的网卡）";

    /// <summary>总开关；默认开（老配置文件无该字段时为 true）。</summary>
    public bool Enabled { get; set; } = true;

    /// <summary>广告端口；0 = 自动（MINIPET_PORT → 实际监听端口 → 38090）。</summary>
    public int Port { get; set; }

    /// <summary>指定网卡（本机 IPv4 字符串）；空 = 自动选择。</summary>
    public string Interface { get; set; } = "";
}

public sealed class MinipetConfig
{
    public WzConfig Wz { get; set; } = new();
    public QqMusicConfig QqMusic { get; set; } = new();
    public BgmConfig Bgm { get; set; } = new();
    public DeviceThresholdsConfig Device { get; set; } = new();
    public ClockConfig Clock { get; set; } = new();
    /// <summary>随机台词气泡（E12）；Web 设置页按此段是否存在启用对应卡片。</summary>
    public SpeechConfig Speech { get; set; } = new();
    /// <summary>mDNS 服务广告（E14，默认开）。</summary>
    public MdnsConfig Mdns { get; set; } = new();
}

public sealed class ConfigChangedEventArgs : EventArgs
{
    public required MinipetConfig Old { get; init; }
    public required MinipetConfig New { get; init; }
    /// <summary>true = 文本编辑/外部写触发（FileSystemWatcher）；false = Web API 写。</summary>
    public bool External { get; init; }
}

/// <summary>
/// 配置服务（薄版重写）：data/config/appsettings.json 读写 + FileSystemWatcher 热重载事件
/// + WZ 路径存在性校验。线程安全：Current 返回深拷贝快照；Update 锁内克隆-变更-落盘-广播。
/// </summary>
public sealed class ConfigService : IDisposable
{
    /// <summary>
    /// 配置文件序列化口径：PascalCase 键（设计 2.4 文档形态，"Wz"/"QqMusic"/...），
    /// 读侧大小写不敏感（文本编辑随手写 "wz" 也能读）；与 API 层 camelCase 无关。
    /// </summary>
    private static readonly JsonSerializerOptions ConfigJsonOpts = new()
    {
        PropertyNameCaseInsensitive = true,
        WriteIndented = true,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping, // _comment 中文不转义
    };

    private readonly ServerPaths _paths;
    private readonly object _gate = new();
    private MinipetConfig _current;
    private readonly FileSystemWatcher? _watcher;
    private Timer? _debounce;
    private bool _selfWrite;

    /// <summary>配置变更（含热重载）。订阅方自行 diff 感兴趣的段（如 Clock.MapOffsets → manifest rev）。</summary>
    public event Action<ConfigChangedEventArgs>? Changed;

    public ConfigService(ServerPaths paths)
    {
        _paths = paths;
        Directory.CreateDirectory(paths.ConfigDir);

        var loaded = ReadConfigFile(paths.ConfigFile);
        _current = loaded ?? Default();
        Normalize(_current);
        if (loaded == null) Save(_current);

        try
        {
            _watcher = new FileSystemWatcher(paths.ConfigDir, Path.GetFileName(paths.ConfigFile))
            {
                NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.CreationTime,
                EnableRaisingEvents = true,
            };
            _watcher.Changed += (_, _) => ScheduleReload();
            _watcher.Created += (_, _) => ScheduleReload();
            _watcher.Renamed += (_, _) => ScheduleReload();
        }
        catch
        {
            _watcher = null; // 部分文件系统不支持 FSW：静默降级（仅失去热重载）
        }
    }

    public static MinipetConfig Default() => new()
    {
        Clock = new ClockConfig
        {
            // 与项目 appsettings.json 默认值一致（设计 2.4 示例；正式值由导出器建议/Web 校准）
            MapOffsets = new Dictionary<string, int[]>(StringComparer.Ordinal)
            {
                ["200000100"] = new[] { 123, 240 },
                ["220000100"] = new[] { 98, 258 },
            },
        },
    };

    /// <summary>当前配置快照（深拷贝，改了不影响服务内部状态）。</summary>
    public MinipetConfig Current
    {
        get { lock (_gate) return Clone(_current); }
    }

    /// <summary>WZ 路径存在性校验（设置页保存前调用；空路径视为不合法）。</summary>
    public (bool Ok, string Message) ValidateWzPath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return (false, "WZ 路径不能为空");
        var p = path.Trim();
        if (!Directory.Exists(p))
            return (false, $"WZ 路径不存在：{p}");
        return (true, $"OK：{p}");
    }

    /// <summary>
    /// 锁内克隆-变更-落盘-广播。validateWzPath=true 时对 DataPath 做存在性硬校验
    /// （校验失败不落盘并返回错误——Web PUT settings 用）。
    /// </summary>
    public (MinipetConfig? Applied, IReadOnlyList<string> Errors) Update(
        Action<MinipetConfig> mutate, bool validateWzPath = false)
    {
        MinipetConfig old;
        MinipetConfig next;
        List<string> errors = new();
        lock (_gate)
        {
            old = _current;
            next = Clone(_current);
            mutate(next);
            Normalize(next);

            if (string.IsNullOrWhiteSpace(next.Wz.DataPath))
                errors.Add("Wz.DataPath 不能为空");
            if (validateWzPath)
            {
                var (ok, msg) = ValidateWzPath(next.Wz.DataPath);
                if (!ok) errors.Add(msg);
            }
            if (errors.Count > 0) return (null, errors);

            _current = next;
            Save(next);
        }
        Changed?.Invoke(new ConfigChangedEventArgs { Old = old, New = Clone(next), External = false });
        return (Clone(next), Array.Empty<string>());
    }

    /// <summary>整体替换（PUT settings 全量写）。</summary>
    public (MinipetConfig? Applied, IReadOnlyList<string> Errors) Replace(MinipetConfig incoming, bool validateWzPath = false)
        => Update(c =>
        {
            c.Wz = incoming.Wz;
            // cookie 导入时间不是 Web 表单字段：Web 回传整份配置时不带 CookieSavedAtUtc，
            // 直接整体替换会把导入时间清成 null → cookieStale 告警永远不触发。cookie 未变则保留。
            var sameCookie = string.Equals(c.QqMusic.Cookie ?? "", incoming.QqMusic?.Cookie ?? "", StringComparison.Ordinal);
            var savedAt = c.QqMusic.CookieSavedAtUtc;
            c.QqMusic = incoming.QqMusic ?? new QqMusicConfig();
            if (sameCookie) c.QqMusic.CookieSavedAtUtc = savedAt;
            c.Bgm = incoming.Bgm;
            c.Device = incoming.Device;
            c.Clock = incoming.Clock;
            // 台词气泡必须在 Replace 里显式搬运：Web 全量回传若被丢弃，设置页
            // 存了也读不回来（Web 侧按 config.speech 是否存在判断是否启用该卡片）。
            c.Speech = incoming.Speech ?? new SpeechConfig();
            // mDNS 段：Web 全量回传没带（老前端）就保留现行值，带了才覆盖——
            // 否则一次「保存设置」会把 mDNS 开关/端口重置成默认。
            if (incoming.Mdns is not null) c.Mdns = incoming.Mdns;
        }, validateWzPath);

    private void ScheduleReload()
    {
        lock (_gate)
        {
            if (_selfWrite) { _selfWrite = false; return; } // 自己刚写的，跳过
            _debounce ??= new Timer(_ => ReloadFromDisk(), null, 400, Timeout.Infinite);
            _debounce.Change(400, Timeout.Infinite);
        }
    }

    private void ReloadFromDisk()
    {
        try
        {
            MinipetConfig old;
            MinipetConfig next;
            lock (_gate)
            {
            var loaded = ReadConfigFile(_paths.ConfigFile);
            if (loaded == null) return;
                Normalize(loaded);
                old = _current;
                next = loaded;
                _current = next;
            }
            Changed?.Invoke(new ConfigChangedEventArgs { Old = old, New = Clone(next), External = true });
        }
        catch
        {
            // 外部写坏文件：保持内存现行配置不动，等下一次合法变更
        }
    }

    private void Save(MinipetConfig config)
    {
        // 锁内调用（Update/ScheduleReload 已持锁或单线程）；_selfWrite 抑制自家写触发的 FSW 回读
        _selfWrite = true;
        var json = JsonSerializer.Serialize(config, ConfigJsonOpts);
        StorageUtil.AtomicWriteAllText(_paths.ConfigFile, json);
    }

    private static MinipetConfig? ReadConfigFile(string path)
    {
        if (!File.Exists(path)) return null;
        try
        {
            return JsonSerializer.Deserialize<MinipetConfig>(File.ReadAllText(path), ConfigJsonOpts);
        }
        catch (JsonException)
        {
            return null; // 手改坏文件 → 走默认（首次落种场景外由调用方保留现行值）
        }
    }

    private static void Normalize(MinipetConfig c)
    {
        c.Wz ??= new WzConfig();
        c.QqMusic ??= new QqMusicConfig();
        c.Bgm ??= new BgmConfig();
        c.Device ??= new DeviceThresholdsConfig();
        c.Clock ??= new ClockConfig();
        c.Speech ??= new SpeechConfig();
        c.Speech.Lines ??= new List<string>();
        c.Mdns ??= new MdnsConfig();                       // 老配置文件缺该段 → 默认（Enabled=true）
        c.Mdns.Interface ??= "";
        /* 端口夹取到合法区间；越界/负数一律回 0=自动（不让一个手滑的值把广告指到无效端口） */
        if (c.Mdns.Port is < 0 or > 65535) c.Mdns.Port = 0;
        /* 灵敏度夹取到合理区间（0.2–3.0）：0/负数会让有效阈值发散，过大等于关闭判定 */
        if (c.Device.ImuSensitivity < 0.2) c.Device.ImuSensitivity = 0.2;
        if (c.Device.ImuSensitivity > 3.0) c.Device.ImuSensitivity = 3.0;
        if (c.Speech.IdleSec < 30) c.Speech.IdleSec = 30;
        c.Clock.MapOffsets = new Dictionary<string, int[]>(c.Clock.MapOffsets ?? new Dictionary<string, int[]>(), StringComparer.Ordinal);
        c.Wz.DataPath ??= "";
        c.QqMusic.Cookie ??= "";
        c.QqMusic.GatewayScript ??= "";
        c.QqMusic.SearchKeyword ??= "";
        var src = (c.Bgm.DefaultSource ?? "").Trim().ToLowerInvariant();
        c.Bgm.DefaultSource = src.Length == 0 ? "wz" : src;
    }

    private static MinipetConfig Clone(MinipetConfig c) =>
        JsonSerializer.Deserialize<MinipetConfig>(JsonSerializer.Serialize(c, StorageUtil.JsonOpts), StorageUtil.JsonOpts)
        ?? new MinipetConfig();

    public void Dispose()
    {
        _watcher?.Dispose();
        _debounce?.Dispose();
    }
}
