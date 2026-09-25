using System.IO;
using MinipetServer.Config;

namespace MinipetServer.Music;

/// <summary>
/// WZ 曲库源（M3 形态）：扫 data/music/wz/ 目录 mp3 列表 + 文件流直出。
/// （正式形态在 M8/M10 演进为经 MusicCatalogService 从 WZ 摘取；接口不变。）
/// </summary>
public sealed class WzMusicSource : IMusicSource
{
    private static readonly TimeSpan CacheTtl = TimeSpan.FromSeconds(30);

    private readonly string _root;
    private readonly object _gate = new();
    private List<MusicTrackInfo>? _cache;
    private DateTime _cacheAtUtc;

    public WzMusicSource(ServerPaths paths) => _root = paths.MusicRoot;

    public string Name => "wz";
    public bool IsEnabled => true;

    public MusicSourceHealth Health()
    {
        List<MusicTrackInfo>? c;
        lock (_gate) c = _cache;
        if (!Directory.Exists(_root))
            return new MusicSourceHealth { State = MusicSourceState.Down, Detail = $"曲库目录不存在：{_root}（放入 mp3 即恢复）" };
        var n = c?.Count;
        return new MusicSourceHealth
        {
            State = MusicSourceState.Ok,
            Detail = n.HasValue ? $"{n} 首（缓存于 {_cacheAtUtc:HH:mm:ss}）" : "目录就绪（尚未扫描）",
        };
    }

    public Task<IReadOnlyList<MusicTrackInfo>> ListTracksAsync(CancellationToken ct = default)
        => Task.Run(() =>
        {
            lock (_gate)
            {
                if (_cache != null && DateTime.UtcNow - _cacheAtUtc < CacheTtl) return (IReadOnlyList<MusicTrackInfo>)_cache;
            }

            var list = new List<MusicTrackInfo>();
            if (Directory.Exists(_root))
            {
                foreach (var f in Directory.EnumerateFiles(_root, "*.mp3", SearchOption.AllDirectories)
                             .OrderBy(p => p, StringComparer.Ordinal))
                {
                    ct.ThrowIfCancellationRequested();
                    var rel = Path.GetRelativePath(_root, f);
                    var id = rel[..^Path.GetExtension(rel).Length].Replace('\\', '/');
                    var seg = id.Split('/');
                    list.Add(new MusicTrackInfo
                    {
                        Id = id,
                        Title = seg[^1],
                        Category = seg.Length > 1 ? seg[0] : null,
                        Source = Name,
                        Bytes = new FileInfo(f).Length,
                    });
                }
            }

            lock (_gate)
            {
                _cache = list;
                _cacheAtUtc = DateTime.UtcNow;
                return (IReadOnlyList<MusicTrackInfo>)list;
            }
        }, ct);

    public Task<MusicStream> GetTrackStreamAsync(string trackId, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(trackId)) throw new ArgumentException("trackId 为空", nameof(trackId));
        var rel = trackId.Trim().Replace('\\', '/');
        if (rel.Contains("..")) throw new FileNotFoundException($"非法曲目 id：{trackId}");

        var full = Path.GetFullPath(Path.Combine(_root, rel + ".mp3"));
        var rootPrefix = Path.GetFullPath(_root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (!full.StartsWith(rootPrefix, StringComparison.Ordinal))
            throw new FileNotFoundException($"曲目不在曲库内：{trackId}");
        if (!File.Exists(full)) throw new FileNotFoundException($"曲目不存在：{trackId}");

        var fs = new FileStream(full, FileMode.Open, FileAccess.Read, FileShare.Read, 64 * 1024, useAsync: true);
        var name = Path.GetFileNameWithoutExtension(full);
        return Task.FromResult(new MusicStream
        {
            Stream = fs,
            MimeType = "audio/mpeg",
            Track = new MusicTrackInfo { Id = rel, Title = name, Source = Name, Bytes = fs.Length },
        });
    }
}
