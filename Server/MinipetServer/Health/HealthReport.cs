using System.Collections.Concurrent;
using System.Text.Json;

namespace MinipetServer.Health;

public sealed class DeviceEventRecord
{
    public DateTime TsUtc { get; set; } = DateTime.UtcNow;
    public string Type { get; set; } = "";
    public JsonElement? Data { get; set; }
}

public sealed class DeviceHealthSummary
{
    public string DeviceId { get; set; } = "";
    public DateTime? LastEventUtc { get; set; }
    public long TotalEvents { get; set; }
    public Dictionary<string, long> ByType { get; set; } = new(StringComparer.Ordinal);
    public string? LastError { get; set; }
    public DateTime? LastErrorUtc { get; set; }
    public int? BatteryPercent { get; set; }
    public List<DeviceEventRecord> Recent { get; set; } = new();
}

/// <summary>
/// 设备健康汇总（E11）：POST /api/device/event 聚合 → Web 可见的设备健康状态。
/// 每设备环形缓冲（最近 200 条）+ 分类计数 + 末次错误 + 电量；纯内存（服务重启清零可接受）。
/// 事件入队时同步写一行 DeviceEventLog（Web 日志页拉取，E14）。
/// </summary>
public sealed class HealthReport
{
    private const int RingCapacity = 200;

    private readonly DeviceEventLog _eventLog;

    /// <summary>构造注入事件环形日志：每条设备事件同步落一行文本。</summary>
    public HealthReport(DeviceEventLog eventLog) => _eventLog = eventLog;

    private static readonly HashSet<string> ErrorTypes = new(StringComparer.Ordinal)
    {
        "error", "fatal", "asset_fail", "asset_corrupt", "watchdog_fuse",
    };

    private sealed class State
    {
        public readonly object Gate = new();
        public readonly Queue<DeviceEventRecord> Ring = new();
        public readonly Dictionary<string, long> ByType = new(StringComparer.Ordinal);
        public long Total;
        public string? LastError;
        public DateTime? LastErrorUtc;
        public int? BatteryPercent;
        public DateTime? LastEventUtc;
    }

    private readonly ConcurrentDictionary<string, State> _states = new(StringComparer.Ordinal);

    public void RecordEvent(string deviceId, string type, JsonElement? data)
    {
        if (string.IsNullOrEmpty(deviceId) || string.IsNullOrEmpty(type)) return;
        var st = _states.GetOrAdd(deviceId, _ => new State());
        var isError = ErrorTypes.Contains(type);
        lock (st.Gate)
        {
            var now = DateTime.UtcNow;
            st.Total++;
            st.ByType[type] = st.ByType.GetValueOrDefault(type) + 1;
            st.LastEventUtc = now;
            st.Ring.Enqueue(new DeviceEventRecord { TsUtc = now, Type = type, Data = data });
            while (st.Ring.Count > RingCapacity) st.Ring.Dequeue();

            if (isError)
            {
                st.LastError = ExtractString(data, "message") ?? type;
                st.LastErrorUtc = now;
            }
            if (type is "battery" or "battery_low" or "low_battery")
            {
                var pct = ExtractInt(data, "percent") ?? ExtractInt(data, "battery");
                if (pct.HasValue) st.BatteryPercent = pct;
            }
        }

        // 同步写一行设备事件日志：{事件名/错误} {细节}（错误类前缀标注，便于日志页扫读）
        var detail = DetailOf(data);
        _eventLog.Append(deviceId, detail.Length == 0
            ? (isError ? $"错误 {type}" : type)
            : (isError ? $"错误 {type}" : type) + " " + detail);
    }

    public DeviceHealthSummary? GetSummary(string deviceId)
    {
        if (!_states.TryGetValue(deviceId, out var st)) return null;
        lock (st.Gate)
        {
            return new DeviceHealthSummary
            {
                DeviceId = deviceId,
                LastEventUtc = st.LastEventUtc,
                TotalEvents = st.Total,
                ByType = new Dictionary<string, long>(st.ByType, StringComparer.Ordinal),
                LastError = st.LastError,
                LastErrorUtc = st.LastErrorUtc,
                BatteryPercent = st.BatteryPercent,
                Recent = st.Ring.TakeLast(20).ToList(),
            };
        }
    }

    public List<DeviceHealthSummary> GetAll()
        => _states.Keys.Select(GetSummary).Where(s => s != null).Cast<DeviceHealthSummary>().ToList();

    /// <summary>事件日志细节：优先 message 字符串，其次原始 JSON（空对象/空数组省略）。</summary>
    private static string DetailOf(JsonElement? data)
    {
        if (data is not { } el) return "";
        if (el.ValueKind == JsonValueKind.String) return el.GetString() ?? "";
        var msg = ExtractString(data, "message");
        if (!string.IsNullOrEmpty(msg)) return msg!;
        if (el.ValueKind is JsonValueKind.Object or JsonValueKind.Array)
        {
            var raw = el.GetRawText();
            return raw is "{}" or "[]" ? "" : raw;
        }
        return "";
    }

    private static string? ExtractString(JsonElement? data, string prop)
    {
        if (data is not { ValueKind: JsonValueKind.Object } el) return null;
        if (el.TryGetProperty(prop, out var v))
        {
            return v.ValueKind switch
            {
                JsonValueKind.String => v.GetString(),
                JsonValueKind.Number => v.GetRawText(),
                _ => null,
            };
        }
        return null;
    }

    private static int? ExtractInt(JsonElement? data, string prop)
    {
        if (data is not { ValueKind: JsonValueKind.Object } el) return null;
        if (el.TryGetProperty(prop, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var n))
            return n;
        return null;
    }
}
