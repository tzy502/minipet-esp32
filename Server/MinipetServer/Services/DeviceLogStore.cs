using System.Collections.Concurrent;
using System.Text;

namespace MinipetServer.Services;

/// <summary>
/// 设备端环形日志的服务端副本（E14：「设备环形日志缓冲，Web 可拉取，排障不用插线」）。
///
/// 契约来源：<c>docs/ai/keys-touch-handoff.md</c> §7.2（固件侧已按该契约实现并每 20s
/// 上报，服务端此前没有接收端 → 日志全丢）。
///
/// 关键约定：
/// - 入参 <c>msgHex</c> = 日志正文 UTF-8 字节的十六进制（固件侧零堆分配/零转义的代价），
///   服务端 <c>Convert.FromHexString</c> → UTF-8 解码后存明文。
/// - <b>幂等</b>：固件在失败时会整批重传，因此按 <c>(会话, seq)</c> 去重；
///   seq 在设备重启后从 0 重新开始，故用「seq 不递增即视为新会话」分段，
///   新会话清空该设备旧日志（设备侧环缓本身就是最近 152 条，服务端是它的副本）。
/// - <b>有界</b>：每设备保留最近 <see cref="MaxLines"/> 行（超出丢最旧），
///   Web 侧增量轮询用 sinceSeq。
/// - <c>ts</c> 在设备未校时时是开机毫秒（远小于 1.7e12），此时
///   <c>ClockSynced=false</c>，Web 应回退显示服务端接收时间。
/// </summary>
public sealed class DeviceLogStore
{
    public const int MaxLines = 512;          // 每设备保留行数（契约建议 ≥512）
    private const long ClockSyncedThreshold = 1_700_000_000_000L;   // ms：低于此视为未校时
    /// <summary>重启判定余量：本批 t 比上批 t 小这么多以上 → 认定设备重启过（ms）。</summary>
    private const uint RestartTMarginMs = 5_000;

    public sealed record LogLine(uint Seq, long TsMs, uint TMs, string Lvl, string Tag, string Msg, DateTime ReceivedUtc);

    private sealed class DeviceLogs
    {
        public readonly LinkedList<LogLine> Lines = new();
        public uint LastSeq;
        public bool ClockSynced;
        public long LastTsMs;
        public DateTime LastReceivedUtc = DateTime.MinValue;
        public int TotalAccepted;   // 累计接收（含被环缓淘汰的），Web 可展示"共 N 条"
        public uint LastTMs;        // 最后一次接收行的"开机毫秒"（t）：会话判定与重启检测
        public int Seen;            // 本会话已接收行数（0 = 全新设备）
    }

    private readonly ConcurrentDictionary<string, DeviceLogs> _byDevice = new();
    private readonly object _lock = new();

    /// <summary>
    /// 接收一批设备日志。返回 (接收后 lastSeq, 本批新增行数)。
    ///
    /// 幂等（设备上报失败会整批重传）：
    /// - 同会话内 <c>seq &lt;= LastSeq</c> 的行直接丢弃 → 重传/乱序都不产生重复行；
    /// - 会话判定只能用设备本已上报的 <c>t</c>（开机毫秒：同一次开机内单调递增，
    ///   重启后必然回落到接近 0）。**不能只看 seq 回退** —— 重传批的 seq 同样
    ///   ≤ LastSeq，会被误判成"设备重启"而清库重收（实测 bug：连发三次同批，
    ///   返回始终 accepted=2，库里反复重建）。
    /// </summary>
    public (uint LastSeq, int Added) Append(string deviceId, uint since, IEnumerable<LogLine> batch)
    {
        if (string.IsNullOrWhiteSpace(deviceId)) return (0, 0);
        var st = _byDevice.GetOrAdd(deviceId, _ => new DeviceLogs());
        lock (_lock)
        {
            var batchList = batch.ToList();
            // 重启判定：本批最大 t 比上一批的 t 还小一截（斜率超过阈值）→ 新会话
            bool newSession = st.Seen > 0 && batchList.Count > 0 &&
                              batchList.Max(l => l.TMs) + RestartTMarginMs < st.LastTMs;
            if (newSession)
            {
                st.Lines.Clear();
                st.TotalAccepted = 0;
                st.LastSeq = 0;
                st.LastTMs = 0;
            }

            int added = 0, dup = 0;
            foreach (var line in batchList)
            {
                if (line.Seq <= st.LastSeq) { dup++; continue; }   // 重传/乱序：丢弃（幂等）
                st.Lines.AddLast(line);
                while (st.Lines.Count > MaxLines) st.Lines.RemoveFirst();
                st.LastSeq = line.Seq;
                st.LastTsMs = line.TsMs;
                st.LastTMs = line.TMs;
                st.ClockSynced = line.TsMs >= ClockSyncedThreshold;
                st.LastReceivedUtc = line.ReceivedUtc;
                st.TotalAccepted++;
                st.Seen++;
                added++;
            }
            if (added > 0 || newSession)
                Console.Error.WriteLine($"[DeviceLog] {deviceId} 批 {batchList.Count} 行 → 新增 {added}、" +
                    $"去重 {dup}；lastSeq={st.LastSeq} 会话={(newSession ? "新（设备重启）" : "延续")}");
            return (st.LastSeq, added);
        }
    }

    /// <summary>Web 拉取：sinceSeq 之后的行（含过滤），最多 limit 行（取最新的 limit 行）。</summary>
    public (List<LogLine> Items, uint LastSeq, bool ClockSynced, int Total, DateTime LastReceivedUtc)?
        Query(string deviceId, uint sinceSeq, int limit, string? level, string? tag)
    {
        if (!_byDevice.TryGetValue(deviceId, out var st)) return null;
        lock (_lock)
        {
            IEnumerable<LogLine> q = st.Lines;
            if (sinceSeq > 0) q = q.Where(l => l.Seq > sinceSeq);
            if (!string.IsNullOrWhiteSpace(level))
            {
                var lv = level.Trim().ToUpperInvariant();
                q = q.Where(l => string.Equals(l.Lvl, lv, StringComparison.OrdinalIgnoreCase));
            }
            if (!string.IsNullOrWhiteSpace(tag))
            {
                var tg = tag.Trim();
                q = q.Where(l => l.Tag.Contains(tg, StringComparison.OrdinalIgnoreCase));
            }
            var list = q.ToList();
            if (limit > 0 && list.Count > limit) list = list.Skip(list.Count - limit).ToList();
            return (list, st.LastSeq, st.ClockSynced, st.TotalAccepted, st.LastReceivedUtc);
        }
    }

    /// <summary>msgHex → UTF-8 明文（非法 hex 返回 null，调用方跳过该行）。</summary>
    public static string? DecodeHex(string? hex)
    {
        if (string.IsNullOrEmpty(hex)) return "";
        try
        {
            var bytes = Convert.FromHexString(hex);
            return Encoding.UTF8.GetString(bytes);
        }
        catch (FormatException)
        {
            return null;
        }
    }

    public IEnumerable<string> KnownDevices => _byDevice.Keys;
}
