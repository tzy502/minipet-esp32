namespace MinipetServer.Music;

public enum MusicSourceState
{
    Ok,
    Degraded,
    Down,
    Disabled,
}

public sealed class MusicSourceHealth
{
    public MusicSourceState State { get; init; }
    public string? Detail { get; init; }
    public DateTime CheckedAtUtc { get; init; } = DateTime.UtcNow;
}

public sealed class MusicTrackInfo
{
    /// <summary>源内曲目 id（WZ 源=库内相对路径；QQ 源=songmid）。</summary>
    public string Id { get; set; } = "";
    public string Title { get; set; } = "";
    public string? Category { get; set; }
    public string Source { get; set; } = "";
    public long Bytes { get; set; }
}

/// <summary>取流结果：流 + MIME（设备端只见 /api/device/bgm/stream 这一个 URL，E2/E8）。</summary>
public sealed class MusicStream : IAsyncDisposable
{
    public required Stream Stream { get; init; }
    public required string MimeType { get; init; }
    public required MusicTrackInfo Track { get; init; }

    public async ValueTask DisposeAsync() => await Stream.DisposeAsync();
}

/// <summary>音源不可用（停用/未知/同源降级耗尽）——BgmRouter 短路规则的最终失败态。</summary>
public sealed class BgmSourceUnavailableException : Exception
{
    public string SourceName { get; }
    public BgmSourceUnavailableException(string source, string reason) : base(reason) => SourceName = source;
}

/// <summary>
/// 取流源抽象（E8，软件设计 2.2 BgmRouter 条目）：WZ 摘取 MP3 字节流 / QQ 经网关取链转发。
/// 可插拔：新源实现本接口注册进 DI 即被 BgmRouter 收编。
/// </summary>
public interface IMusicSource
{
    /// <summary>源标识："wz" | "qq"（设备/配置引用用这个名字）。</summary>
    string Name { get; }

    /// <summary>是否启用（QQ 源可整体停用；停用源在设备上入口置灰，E8）。</summary>
    bool IsEnabled { get; }

    MusicSourceHealth Health();

    Task<IReadOnlyList<MusicTrackInfo>> ListTracksAsync(CancellationToken ct = default);

    /// <summary>取曲目字节流（禁止整载内存——必须 FileStream / 网关分块转发）。</summary>
    Task<MusicStream> GetTrackStreamAsync(string trackId, CancellationToken ct = default);
}
