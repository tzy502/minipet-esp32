using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using MinipetServer.Config;

namespace MinipetServer.Device;

public sealed class DeviceCommand
{
    /// <summary>设备内单调递增序号（跨重启持久，poll 用 since 增量语义）。</summary>
    public long Seq { get; set; }
    /// <summary>指令类型：action / expression / bubble / bgm / brightness / reboot / ota / ...（纯桌宠指令，E2）。</summary>
    public string Type { get; set; } = "";
    public JsonElement? Payload { get; set; }
    public DateTime EnqueuedAtUtc { get; set; } = DateTime.UtcNow;

    /// <summary>
    /// 固件「旧口径」标记（非空 → poll 响应按 {seq,t,v,n} 下发，**不带 type 字段**）。
    /// 背景（poller.c 两条解析路径）：
    ///   · 现代口径 {type,payload} → 专用解析分支（poller.c:191-221）。bgm 在该分支只实现
    ///     play/pause/resume/stop/next/prev 六个字符串值，**没有 vol/source**；
    ///   · 旧口径 {t,v,n}（无 type 键）→ `if (!t) { handle_cmd(jc); continue; }` 直通
    ///     handle_cmd（poller.c:77-88），那里才有 bgm vol（音量绝对值 n∈[0,100]）与
    ///     source（n: 0=WZ/1=QQ）。
    /// 因此音量/音源只能走本字段；写成 {"type":"bgm","payload":{"n":50}} 会被固件静默忽略
    /// （payload 非字符串 → cJSON_IsString(vitem) 为假）。
    /// </summary>
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public LegacyCommand? Legacy { get; set; }

    /// <summary>旧口径三元组（poller.c handle_cmd 期望的顶层 t/v/n）。</summary>
    public sealed class LegacyCommand
    {
        /// <summary>指令名（固件 t）：目前仅 "bgm"。</summary>
        public string T { get; set; } = "";
        /// <summary>指令值（固件 v）：固件原样 strcmp——"vol" / "source"。</summary>
        public string? V { get; set; }
        /// <summary>数值参数（固件 n）。</summary>
        public int? N { get; set; }
    }

    /// <summary>
    /// poll 响应里的线上形状：默认原样 {seq,type,payload,...}；Legacy 指令投影为
    /// {seq,t,v,n}（无 type → 固件直通 handle_cmd）。
    /// </summary>
    public object ToWire() => Legacy is { } l
        ? new { seq = Seq, t = l.T, v = l.V, n = l.N }
        : (object)this;
}

/// <summary>
/// 指令队列（E2）：每设备一个队列，内存为主 + data/queues/{deviceId}.json 持久兜底
/// （服务重启不丢已入队未取走的指令）。poll 按 seq 有序取走（取走即出队），
/// 无新指令时挂起等待（长轮询由端点层控制在 ≤55s）。
/// </summary>
public sealed class CommandQueue
{
    private sealed class QueueState
    {
        public required string DeviceId { get; init; }
        public long LastSeq;
        public readonly List<DeviceCommand> Pending = new();
        public readonly object Gate = new();
        public TaskCompletionSource Signal = NewSignal();
    }

    private static TaskCompletionSource NewSignal()
        => new(TaskCreationOptions.RunContinuationsAsynchronously);

    private readonly ServerPaths _paths;
    private readonly object _gate = new();
    private readonly Dictionary<string, QueueState> _states = new(StringComparer.Ordinal);

    public CommandQueue(ServerPaths paths)
    {
        _paths = paths;
        Directory.CreateDirectory(paths.QueuesDir);
    }

    private QueueState GetState(string deviceId)
    {
        lock (_gate)
        {
            if (!_states.TryGetValue(deviceId, out var st))
            {
                st = new QueueState { DeviceId = deviceId };
                var file = QueueFile(deviceId);
                var loaded = StorageUtil.ReadJson<DeviceQueueFile>(file);
                if (loaded != null)
                {
                    st.LastSeq = loaded.LastSeq;
                    if (loaded.Pending != null) st.Pending.AddRange(loaded.Pending);
                }
                _states[deviceId] = st;
            }
            return st;
        }
    }

    private string QueueFile(string deviceId)
        => Path.Combine(_paths.QueuesDir, StorageUtil.SafeFileId(deviceId) + ".json");

    /// <summary>入队（Web 侧 / OTA / 配置下发）。</summary>
    public DeviceCommand Enqueue(string deviceId, string type, object? payload = null)
    {
        var st = GetState(deviceId);
        lock (st.Gate)
        {
            var cmd = new DeviceCommand
            {
                Seq = ++st.LastSeq,
                Type = type,
                Payload = StorageUtil.ToElement(payload),
            };
            st.Pending.Add(cmd);
            PersistLocked(st);
            st.Signal.TrySetResult();
            st.Signal = NewSignal();
            return cmd;
        }
    }

    /// <summary>
    /// 入队「固件旧口径」指令（{t,v,n}，无 type 字段）——仅 bgm vol/source 用：
    /// 现代 {type,payload} 分支的 bgm 没有 vol/source 实现（详见 DeviceCommand.Legacy）。
    /// Type 仍记 "bgm"（队列持久化/事件日志可读），线上形状由 Legacy 决定。
    /// </summary>
    public DeviceCommand EnqueueLegacy(string deviceId, string t, string v, int? n)
    {
        var st = GetState(deviceId);
        lock (st.Gate)
        {
            var cmd = new DeviceCommand
            {
                Seq = ++st.LastSeq,
                Type = t,
                Payload = StorageUtil.ToElement(new { v, n }),
                Legacy = new DeviceCommand.LegacyCommand { T = t, V = v, N = n },
            };
            st.Pending.Add(cmd);
            PersistLocked(st);
            st.Signal.TrySetResult();
            st.Signal = NewSignal();
            return cmd;
        }
    }

    /// <summary>当前最大 seq（poll 响应回传，设备据此推进 since）。</summary>
    public long GetLastSeq(string deviceId)
    {
        var st = GetState(deviceId);
        lock (st.Gate) return st.LastSeq;
    }

    /// <summary>
    /// 取走 seq &gt; since 的全部待取指令（按序，取走即出队并持久化）；
    /// 队列空时挂起等待新指令，最长 maxWait（端点层传 ≤55s）。
    /// 取消（客户端断开）返回已到手的（通常为空）。
    /// </summary>
    public async Task<List<DeviceCommand>> PollAsync(string deviceId, long since, TimeSpan maxWait, CancellationToken ct)
    {
        var deadline = DateTime.UtcNow + maxWait;
        while (true)
        {
            var st = GetState(deviceId);
            Task wait;
            lock (st.Gate)
            {
                if (st.Pending.Count > 0)
                {
                    var take = st.Pending.Where(c => c.Seq > since).OrderBy(c => c.Seq).ToList();
                    if (take.Count > 0)
                    {
                        st.Pending.RemoveAll(c => c.Seq > since);
                        PersistLocked(st);
                        return take;
                    }
                }
                wait = st.Signal.Task;
            }

            var remaining = deadline - DateTime.UtcNow;
            if (remaining <= TimeSpan.Zero) return new List<DeviceCommand>();
            try
            {
                await Task.WhenAny(wait, Task.Delay(remaining, ct));
            }
            catch (OperationCanceledException)
            {
                return new List<DeviceCommand>();
            }
            if (ct.IsCancellationRequested) return new List<DeviceCommand>();
            // 被唤醒或到点：回到循环重查（双重检查防信号丢失竞态）
        }
    }

    private void PersistLocked(QueueState st)
    {
        var file = QueueFile(st.DeviceId);
        var json = JsonSerializer.Serialize(new DeviceQueueFile
        {
            DeviceId = st.DeviceId,
            LastSeq = st.LastSeq,
            Pending = st.Pending,
        }, StorageUtil.JsonOpts);
        StorageUtil.AtomicWriteAllText(file, json);
    }

    private sealed class DeviceQueueFile
    {
        public string DeviceId { get; set; } = "";
        public long LastSeq { get; set; }
        public List<DeviceCommand> Pending { get; set; } = new();
    }
}
