using MinipetServer.Config;

namespace MinipetServer.Music;

public enum BgmFailoverKind
{
    /// <summary>同源内跳下一首重试（E8 短路规则：只在同源内降级）。</summary>
    SkipTrack,
    /// <summary>源整体不可用（连续失败达阈值 → Down，设备该源入口置灰）。</summary>
    SourceDown,
    /// <summary>Down 状态探测成功恢复。</summary>
    Recovered,
}

public sealed record BgmFailoverEvent(
    string Source,
    string? TrackId,
    string Reason,
    BgmFailoverKind Kind,
    DateTime TsUtc);

/// <summary>
/// BGM 路由器（E8）：统一音源选择 + 流式转发 + 同源降级状态机骨架 + failover 事件。
/// 铁律：失败只在同源内重试降级（跳下一首/重试取链），禁止跨源自动换歌；
/// 源整体不可用 → 状态 Down + 事件上报（设备侧 despair 表情、入口置灰）。
/// 编排决策吸收自桌面版 MusicPlayerService/MusicDecisions（纯函数部分后续直接复用）。
/// </summary>
public sealed class BgmRouter
{
    private const int ConsecutiveFailureThreshold = 3;
    private const int MaxSameSourceAttempts = 3;

    private sealed class SourceRuntime
    {
        public MusicSourceState State = MusicSourceState.Ok;
        public int ConsecutiveFailures;
        public string? LastError;
        public DateTime? DownSinceUtc;
    }

    private readonly Dictionary<string, IMusicSource> _sources;
    private readonly ConfigService _cfg;
    private readonly object _gate = new();
    private readonly Dictionary<string, SourceRuntime> _runtimes = new(StringComparer.Ordinal);

    /// <summary>failover 事件（订阅方：日志 / HealthReport 上报 / Web 展示）。</summary>
    public event Action<BgmFailoverEvent>? Failover;

    public BgmRouter(IEnumerable<IMusicSource> sources, ConfigService cfg)
    {
        _cfg = cfg;
        _sources = sources.ToDictionary(s => s.Name, StringComparer.Ordinal);
    }

    public IEnumerable<IMusicSource> Sources => _sources.Values;

    /// <summary>解析音源：空则用配置默认源；未知源返回 null。</summary>
    public IMusicSource? Resolve(string? name)
    {
        var n = string.IsNullOrWhiteSpace(name)
            ? _cfg.Current.Bgm.DefaultSource
            : name.Trim().ToLowerInvariant();
        return _sources.GetValueOrDefault(n);
    }

    /// <summary>
    /// 打开曲目流：同源降级链 = [请求曲, 下一首, 再下一首]（源 Down 时只探测请求曲）。
    /// 全部失败 → BgmSourceUnavailableException（不跨源）。
    /// </summary>
    public async Task<MusicStream> OpenStreamAsync(string? sourceName, string trackId, CancellationToken ct = default)
    {
        var src = Resolve(sourceName)
                  ?? throw new BgmSourceUnavailableException(sourceName ?? "", $"未知音源：{sourceName}（可用：{string.Join("/", _sources.Keys)}）");
        if (!src.IsEnabled)
            throw new BgmSourceUnavailableException(src.Name, $"音源 {src.Name} 已停用");

        var rt = Runtime(src.Name);
        bool down;
        lock (_gate) down = rt.State == MusicSourceState.Down;

        var chain = down
            ? new[] { trackId } // 半开探测：Down 态只试一次，成功即恢复
            : await BuildFallbackChainAsync(src, trackId, ct);

        Exception? last = null;
        foreach (var id in chain)
        {
            ct.ThrowIfCancellationRequested();
            try
            {
                var stream = await src.GetTrackStreamAsync(id, ct);
                lock (_gate)
                {
                    if (rt.State != MusicSourceState.Ok)
                    {
                        rt.State = MusicSourceState.Ok;
                        Failover?.Invoke(new BgmFailoverEvent(src.Name, id, "探测成功，源恢复", BgmFailoverKind.Recovered, DateTime.UtcNow));
                    }
                    rt.ConsecutiveFailures = 0;
                    rt.LastError = null;
                }
                return stream;
            }
            catch (Exception ex)
            {
                last = ex;
                lock (_gate)
                {
                    rt.ConsecutiveFailures++;
                    rt.LastError = ex.Message;
                    if (rt.ConsecutiveFailures >= ConsecutiveFailureThreshold && rt.State != MusicSourceState.Down)
                    {
                        rt.State = MusicSourceState.Down;
                        rt.DownSinceUtc = DateTime.UtcNow;
                        Failover?.Invoke(new BgmFailoverEvent(src.Name, id, ex.Message, BgmFailoverKind.SourceDown, DateTime.UtcNow));
                    }
                    else
                    {
                        Failover?.Invoke(new BgmFailoverEvent(src.Name, id, ex.Message, BgmFailoverKind.SkipTrack, DateTime.UtcNow));
                    }
                }
                if (rt.ConsecutiveFailures >= MaxSameSourceAttempts) break;
            }
        }

        throw new BgmSourceUnavailableException(src.Name,
            $"音源 {src.Name} 同源降级耗尽（E8 禁止跨源）：{last?.Message ?? "无候选曲目"}");
    }

    /// <summary>同源切歌（bgm/cmd next|prev 回传的解析；step=+1/-1）。无曲目返回 null。</summary>
    public async Task<string?> NextTrackAsync(string? sourceName, string? currentId, int step, CancellationToken ct = default)
    {
        var src = Resolve(sourceName);
        if (src == null || !src.IsEnabled) return null;
        var tracks = await src.ListTracksAsync(ct);
        if (tracks.Count == 0) return null;
        var idx = 0;
        if (!string.IsNullOrEmpty(currentId))
        {
            var i = tracks.Select((t, k) => (t, k)).FirstOrDefault(x => x.t.Id == currentId).k;
            if (i >= 0) idx = i;
        }
        var n = ((idx + step) % tracks.Count + tracks.Count) % tracks.Count;
        return tracks[n].Id;
    }

    private async Task<string[]> BuildFallbackChainAsync(IMusicSource src, string trackId, CancellationToken ct)
    {
        try
        {
            var tracks = await src.ListTracksAsync(ct);
            if (tracks.Count == 0) return new[] { trackId };
            var idx = -1;
            for (var i = 0; i < tracks.Count; i++)
            {
                if (string.Equals(tracks[i].Id, trackId, StringComparison.Ordinal)) { idx = i; break; }
            }
            if (idx < 0) return new[] { trackId };
            var chain = new List<string>();
            for (var step = 0; step < MaxSameSourceAttempts && step < tracks.Count; step++)
            {
                chain.Add(tracks[(idx + step) % tracks.Count].Id);
            }
            return chain.ToArray();
        }
        catch
        {
            return new[] { trackId }; // 列表拿不到：只试请求曲本身
        }
    }

    private SourceRuntime Runtime(string name)
    {
        lock (_gate)
        {
            if (!_runtimes.TryGetValue(name, out var rt))
            {
                rt = new SourceRuntime();
                _runtimes[name] = rt;
            }
            return rt;
        }
    }
}
