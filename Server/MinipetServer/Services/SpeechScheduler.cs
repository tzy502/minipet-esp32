using System.Text;
using MinipetServer.Config;
using MinipetServer.Device;
using MinipetServer.Health;

namespace MinipetServer.Services;

/// <summary>
/// 随机台词气泡调度器（E12：「**随机台词**：静置久了冒预设台词气泡，文本 Web 配置」；
/// 触发语义见 E6：静置 = 无触摸/按键/IMU 交互）。
///
/// 端到端链路（本类补的就是缺失的服务端第一环）：
///   SpeechScheduler（静置判定 + 随机挑一条）
///     → CommandQueue.Enqueue(deviceId, "bubble", 裸字符串)
///     → GET /api/device/poll 响应 commands[{seq,type:"bubble",payload:"<文本>"}]
///     → 固件 poller.c:203-205（type 分支）/ poller.c:62-65（旧口径分支）
///     → mp_cmd_t{type=MP_CMD_BUBBLE, s=UTF-8 文本} → state_machine.c:594 render_bubble_show(s, FONT_24)
///
/// 调度语义：
///   · **在线**设备（DeviceRegistry.IsOnline，LastSeen 90s 窗口）才发；
///   · 静置判据 = DeviceRegistry.LastSeenUtc（poll / event / bgm 每个入口都 Touch）——
///     设备长轮询最短也 hold 到有指令或 55s 超时，所以在线设备天然会攒出静置窗口；
///   · 该设备 Speech.Enabled && Lines 非空时才参与（设备级覆盖 DeviceRecord.Speech 优先于全局配置）；
///   · 两次台词之间至少 MinGap（防刷屏；静置再久也不会连发）；
///   · 单条文本 UTF-8 ≤ MaxUtf8Bytes（95）字节：固件 mp_cmd_t.s=char[96]（app_core.h:150），
///     服务端先按字节卡掉——超长文本若下发，固件 strlcpy 会在多字节字符中间截断（乱码），
///     故**跳过并记日志**（日志按台词库版本去重，不刷屏）。
///
/// 注册方式照抄 QqGatewayProcess（单例 + 构造起 Timer + IDisposable；Program.cs 早绑定）：
/// 定时器只读配置/设备表快照，不做网络 IO，异常一律吞进日志（绝不影响 API 线程）。
/// </summary>
public sealed class SpeechScheduler : IDisposable
{
    /// <summary>一拍间隔（静置判定精度；命令经 poll 长轮询即时下发，无需更密）。</summary>
    private static readonly TimeSpan TickInterval = TimeSpan.FromSeconds(5);

    /// <summary>同一设备两次台词最小间隔（防刷屏；E12 未规定具体值，取经验值 120s）。</summary>
    private static readonly TimeSpan MinGap = TimeSpan.FromSeconds(120);

    /// <summary>静置秒数下限——与 ConfigService.Normalize 的全局夹取口径一致（设备级覆盖同样适用）。</summary>
    private const int MinIdleSec = 30;

    /// <summary>单条台词 UTF-8 字节上限（固件 mp_cmd_t.s=char[96]，留 1 字节 NUL）。</summary>
    public const int MaxUtf8Bytes = 95;

    /// <summary>某设备的台词库计划（按台词库指纹缓存：只在配置变化时重算 + 记一次超长日志）。</summary>
    private sealed class Plan
    {
        public required string Fingerprint { get; init; }
        public required List<string> Valid { get; init; }
        public required int Dropped { get; init; }
        public required TimeSpan Idle { get; init; }
    }

    private readonly ConfigService _cfg;
    private readonly DeviceRegistry _registry;
    private readonly CommandQueue _queue;
    private readonly DeviceEventLog _eventLog;

    private readonly object _gate = new();
    private readonly Dictionary<string, Plan> _plans = new(StringComparer.Ordinal);
    private readonly Dictionary<string, DateTime> _lastSpeechUtc = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _lastLine = new(StringComparer.Ordinal);
    private Timer? _timer;
    private bool _disposed;

    public SpeechScheduler(ConfigService cfg, DeviceRegistry registry, CommandQueue queue, DeviceEventLog eventLog)
    {
        _cfg = cfg;
        _registry = registry;
        _queue = queue;
        _eventLog = eventLog;
        // 首拍延迟一个周期再跑（启动期 WZ 加载/首启 provisioning 正忙，让路；台词也不是实时功能）
        /* ══ 【自动台词气泡默认关闭 2026-10-01 · 用户口径】══════════════════════
         * 用户："整体去除气泡 效果不好" → 设备侧已不再渲染任何气泡（两板同口径，
         * 见固件 Kconfig MP_BUBBLE_ENABLE=n）。服务端的**静置自动台词**也不该再往
         * 设备推 bubble 指令——否则指令照样入队、设备照样唤醒/切态，只是屏上不显示，
         * 属于"看不见的副作用"（E9 待机时钟会被气泡指令唤醒）。
         * 此处默认**不启动定时器**；需要回归 E12 时把配置 speech.enabled 置 true
         * （或临时改这一行）。手动下发 bubble（Web「发一句」/ 取证魔数 ::shot）
         * 不受影响——那条路走 AdminEndpoints，不经过本调度器。 */
        bool speechOn = false;
        try
        {
            /* 开关口径：直接看配置里的 **台词表**——用户在 Web 设置页填了台词
             * （Lines 非空）才认为他要自动气泡，否则一律不启动。这样不需要新增
             * 配置字段，也不会出现"填了台词但功能被单独开关挡住"的困惑。 */
            speechOn = (_cfg.Current?.Speech?.Lines?.Count ?? 0) > 0;
        }
        catch { /* 配置不可读 = 保持关闭 */ }
        if (!speechOn)
        {
            Console.WriteLine("[SpeechScheduler] 自动台词气泡已关闭（气泡整体去除；置 config.speech.enabled=true 可恢复）");
            return;
        }
        _timer = new Timer(_ => SafeTick(), null, TickInterval, TickInterval);
    }

    /// <summary>手动跑一拍（排障/自测用；同步返回本拍下发条数）。同 QqGatewayProcess.EnsureHealthyNow 口径。</summary>
    public int RunOnceNow() => TickOnce(DateTime.UtcNow);

    private void SafeTick()
    {
        try { TickOnce(DateTime.UtcNow); }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Speech] 调度异常: {ex.Message}");
        }
    }

    /// <summary>
    /// 一拍（定时器调用；utcNowUtc 注入便于确定性自测）：对每台在线设备判定静置 → 随机挑一条 → 入队 bubble。
    /// 返回本拍实际入队的台词条数。
    /// </summary>
    public int TickOnce(DateTime utcNowUtc)
    {
        if (_disposed) return 0;
        var global = _cfg.Current.Speech;   // 配置快照（深拷贝），一拍一份
        int sent = 0;

        foreach (var dev in _registry.List())
        {
            try
            {
                var speech = dev.Speech ?? global;          // 设备级覆盖优先（E13 设备表）
                if (speech is null || !speech.Enabled) continue;
                if (dev.LastSeenUtc is not { } seen) continue;             // 从未 hello → 无静置基准
                if (!DeviceRegistry.IsOnline(dev, utcNowUtc)) continue;    // 离线设备不下发（醒来后按新心跳重新计时）

                var plan = GetPlan(dev.DeviceId, speech);
                if (plan.Valid.Count == 0) continue;
                if (utcNowUtc - seen < plan.Idle) continue;                // 静置不足（有 poll/event 到达即被 Touch 顶掉）

                lock (_gate)
                {
                    if (_lastSpeechUtc.TryGetValue(dev.DeviceId, out var last)
                        && utcNowUtc - last < MinGap) continue;            // 防刷屏：距上一条太近
                }

                var text = PickLine(dev.DeviceId, plan.Valid);
                var cmd = _queue.Enqueue(dev.DeviceId, "bubble", text);    // 裸字符串 payload（poller.c 期望形状，勿改）
                lock (_gate) _lastSpeechUtc[dev.DeviceId] = utcNowUtc;
                _eventLog.Append(dev.DeviceId,
                    $"随机台词（E12，静置 {(int)(utcNowUtc - seen).TotalSeconds}s）：{text}");
                Console.WriteLine($"[Speech] 设备 {dev.DeviceId} 静置台词已入队 seq={cmd.Seq}：{text}");
                sent++;
            }
            catch (Exception ex)
            {
                // 单设备失败不影响其它设备（配置被手改坏 / 队列落盘失败等）
                Console.Error.WriteLine($"[Speech] 设备 {dev.DeviceId} 台词调度失败: {ex.Message}");
            }
        }
        return sent;
    }

    /// <summary>
    /// 台词库计划（按指纹缓存）：过滤空白 + 超 95 字节的条目，超长条目**记一次日志**（配置变了才重记）。
    /// </summary>
    private Plan GetPlan(string deviceId, SpeechConfig speech)
    {
        var lines = speech.Lines ?? new List<string>();
        int idleSec = Math.Max(speech.IdleSec, MinIdleSec);   // 与 ConfigService.Normalize 同一夹取口径
        var fingerprint = $"{speech.Enabled}|{idleSec}|{string.Join('\u0001', lines)}";

        lock (_gate)
        {
            if (_plans.TryGetValue(deviceId, out var cached) && cached.Fingerprint == fingerprint) return cached;
        }

        var valid = new List<string>();
        int dropped = 0;
        foreach (var raw in lines)
        {
            var text = (raw ?? "").Trim();
            if (text.Length == 0) { dropped++; continue; }                       // 空行不算超长，静默跳过
            if (Encoding.UTF8.GetByteCount(text) > MaxUtf8Bytes) { dropped++; continue; }
            valid.Add(text);
        }

        var plan = new Plan
        {
            Fingerprint = fingerprint,
            Valid = valid,
            Dropped = dropped,
            Idle = TimeSpan.FromSeconds(idleSec),
        };
        lock (_gate) _plans[deviceId] = plan;

        if (dropped > 0)
        {
            Console.Error.WriteLine(
                $"[Speech] 设备 {deviceId}：{dropped} 条台词被跳过（空白或超 {MaxUtf8Bytes} 字节——"
                + "固件 mp_cmd_t.s=char[96]，超长会截断成乱码）");
        }
        return plan;
    }

    /// <summary>随机挑一条（池 &gt; 1 时避免与上一条重复，纯随机偶尔连发两句一样的显得假）。</summary>
    private string PickLine(string deviceId, List<string> pool)
    {
        if (pool.Count == 1)
        {
            lock (_gate) _lastLine[deviceId] = pool[0];
            return pool[0];
        }
        int idx = Random.Shared.Next(pool.Count);
        lock (_gate)
        {
            if (_lastLine.TryGetValue(deviceId, out var last) && pool[idx] == last)
                idx = (idx + 1) % pool.Count;
            _lastLine[deviceId] = pool[idx];
        }
        return pool[idx];
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _timer?.Dispose();
        _timer = null;
    }
}
