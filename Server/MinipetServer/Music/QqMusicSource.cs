using MinipetServer.Config;

namespace MinipetServer.Music;

/// <summary>
/// QQ 音乐源占位（M10 接入：容器内 node 网关子进程 + 实时取链转发）。
/// M3 阶段：Enabled 跟随配置，但取流永远不可用（Disabled/Down 状态上报）。
/// cookie 经 Web 导入仅落配置（PUT /api/admin/settings / POST .../cookie）。
/// </summary>
public sealed class QqMusicSource : IMusicSource
{
    private readonly ConfigService _cfg;
    public QqMusicSource(ConfigService cfg) => _cfg = cfg;

    public string Name => "qq";
    public bool IsEnabled => _cfg.Current.QqMusic.Enabled;

    public MusicSourceHealth Health() => IsEnabled
        ? new MusicSourceHealth
        {
            State = MusicSourceState.Down,
            Detail = "QQ 音源为占位实现（M10 接入 node 网关后生效；cookie 已可导入）",
        }
        : new MusicSourceHealth
        {
            State = MusicSourceState.Disabled,
            Detail = "未启用（配置 QqMusic.Enabled=false）",
        };

    public Task<IReadOnlyList<MusicTrackInfo>> ListTracksAsync(CancellationToken ct = default)
        => Task.FromResult<IReadOnlyList<MusicTrackInfo>>(Array.Empty<MusicTrackInfo>());

    public Task<MusicStream> GetTrackStreamAsync(string trackId, CancellationToken ct = default)
        => throw new BgmSourceUnavailableException(Name, "QQ 音源占位未实现（M10 接入）");
}
