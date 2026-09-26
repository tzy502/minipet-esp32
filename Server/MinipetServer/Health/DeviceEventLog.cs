using System.Collections.Concurrent;

namespace MinipetServer.Health;

/// <summary>
/// 设备事件环形日志（E14）：服务端侧关键设备事件（注册/上线/配对/重命名/换宠换装/
/// BGM 与阈值保存/OTA 下发/健康事件上报）各记一行文本。
/// 每设备环形缓冲 200 条，纯内存（服务重启清零可接受），供 GET /api/admin/logs/{id} 拉取。
/// </summary>
public sealed class DeviceEventLog
{
    private const int RingCapacity = 200;

    /// <summary>时间戳固定 UTC+8（部署容器多为 UTC 时区，不随宿主机漂移）。</summary>
    private static readonly TimeSpan Utc8 = TimeSpan.FromHours(8);

    private sealed class State
    {
        public readonly object Gate = new();
        public readonly Queue<string> Ring = new();
    }

    private readonly ConcurrentDictionary<string, State> _states = new(StringComparer.Ordinal);

    /// <summary>追加一行（自动加 [HH:mm:ss] UTC+8 时间戳前缀；空设备号/空行忽略）。</summary>
    public void Append(string deviceId, string line)
    {
        if (string.IsNullOrEmpty(deviceId) || string.IsNullOrWhiteSpace(line)) return;
        var st = _states.GetOrAdd(deviceId, _ => new State());
        var ts = DateTime.UtcNow.Add(Utc8).ToString("HH:mm:ss");
        lock (st.Gate)
        {
            st.Ring.Enqueue($"[{ts}] {line.Trim()}");
            while (st.Ring.Count > RingCapacity) st.Ring.Dequeue();
        }
    }

    /// <summary>倒序最近事件（最新在前）；无记录返回空列表。</summary>
    public List<string> Tail(string deviceId, int max = RingCapacity)
    {
        if (max <= 0 || !_states.TryGetValue(deviceId, out var st)) return new List<string>();
        lock (st.Gate)
        {
            return st.Ring.Reverse().Take(max).ToList();
        }
    }
}
