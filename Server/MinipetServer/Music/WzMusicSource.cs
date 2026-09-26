using System.IO;
using MinipetServer.Config;
using MinipetServer.Device;
using MinipetServer.Services;

namespace MinipetServer.Music;

/// <summary>
/// WZ 曲库源（2026-09-26 起正式形态）：曲目目录经 MusicCatalogService 从 WZ Sound.wz
/// 摘取（Bgm*.img 全量，本地缓存目录 mp3 一并合并视图）；取流 = 磁盘文件优先，未命中
/// 时按需 WzService.ExtractSound 提取 mp3 落盘缓存（data/music/wz/{img}/{track}.mp3）
/// 再文件流直出——同一曲目只提取一次，与缩略图同模式。
/// </summary>
public sealed class WzMusicSource : IMusicSource
{
    private readonly string _root;
    private readonly WzService _wz;
    private readonly MusicCatalogService _catalog;

    public WzMusicSource(ServerPaths paths, WzService wz, MusicCatalogService catalog)
    {
        _root = paths.MusicRoot;
        _wz = wz;
        _catalog = catalog;
    }

    public string Name => "wz";
    public bool IsEnabled => true;

    public MusicSourceHealth Health()
    {
        if (!_wz.IsLoaded && !Directory.Exists(_root))
            return new MusicSourceHealth { State = MusicSourceState.Down, Detail = "WZ 未加载且曲库目录不存在" };
        var snapshot = _catalog.GetLibrarySnapshot();
        if (snapshot != null)
            return new MusicSourceHealth { State = MusicSourceState.Ok, Detail = $"{snapshot.Count} 首（WZ 曲库" + (Directory.Exists(_root) ? " + 本地缓存）" : "）") };
        if (Directory.Exists(_root))
            return new MusicSourceHealth { State = MusicSourceState.Ok, Detail = "目录就绪（WZ 曲库尚未扫描）" };
        return new MusicSourceHealth { State = MusicSourceState.Down, Detail = "WZ 未加载（曲库目录不存在）" };
    }

    /// <summary>曲目列表：MusicCatalogService 单飞构建的 WZ 曲库（+本地缓存合并视图），30s 内存缓存。</summary>
    public Task<IReadOnlyList<MusicTrackInfo>> ListTracksAsync(CancellationToken ct = default)
        => Task.Run(async () =>
        {
            var tracks = await _catalog.GetCatalogAsync(ct).ConfigureAwait(false);
            ct.ThrowIfCancellationRequested();
            IReadOnlyList<MusicTrackInfo> list = tracks.Select(t => new MusicTrackInfo
            {
                Id = t.Key,                    // "Bgm00.img/SleepyWood"——取流按此解析（本地文件同名时优先文件）
                Title = t.Track,
                Category = t.Img,              // "Bgm00.img"（曲库页分类列）
                Source = Name,
                Bytes = 0,                     // WZ 曲目未提取前列表不带体积（避免全量解包）；提取后按缓存文件大小
            }).ToList();
            return list;
        }, ct);

    /// <summary>
    /// 取曲目流：磁盘文件（data/music/wz/{id}.mp3，含历史手动放置/按需提取缓存）优先；
    /// 未命中且 WZ 已加载 → ExtractSound 提取 + 原子落盘缓存后直出文件流。
    /// </summary>
    public Task<MusicStream> GetTrackStreamAsync(string trackId, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(trackId)) throw new ArgumentException("trackId 为空", nameof(trackId));
        var rel = trackId.Trim().Replace('\\', '/');
        if (rel.Contains("..")) throw new FileNotFoundException($"非法曲目 id：{trackId}");

        // 1) 磁盘优先（历史目录形态 / 已提取缓存）
        var full = Path.GetFullPath(Path.Combine(_root, rel + ".mp3"));
        var rootPrefix = Path.GetFullPath(_root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (File.Exists(full) && full.StartsWith(rootPrefix, StringComparison.Ordinal))
            return Task.FromResult(OpenStream(full, rel, bytes: null));

        // 2) WZ 按需提取（id 形如 "Bgm00.img/SleepyWood"）
        var seg = rel.Split('/');
        if (!_wz.IsLoaded || seg.Length != 2) throw new FileNotFoundException($"曲目不存在：{trackId}");
        var (imgName, trackName) = (seg[0], seg[1]);
        var png = _wz.ExtractSound(imgName, trackName)
                  ?? throw new FileNotFoundException($"WZ 曲库中不存在：{trackId}");

        // 落盘缓存（曲名可能含文件系统非法字符 → SafeFileId；同曲同名稳定）
        try
        {
            var dir = Path.Combine(_root, StorageUtil.SafeFileId(imgName));
            Directory.CreateDirectory(dir);
            var cached = Path.Combine(dir, StorageUtil.SafeFileId(trackName) + ".mp3");
            StorageUtil.AtomicWriteAllBytes(cached, png);
            full = cached;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[WzMusicSource] 曲目缓存落盘失败（{trackId}）：{ex.Message}（直出内存流）");
        }

        if (File.Exists(full)) return Task.FromResult(OpenStream(full, rel, bytes: null));

        // 落盘失败兜底：内存流直出
        var ms = new MemoryStream(png, writable: false);
        var name = seg[^1];
        return Task.FromResult(new MusicStream
        {
            Stream = ms,
            MimeType = "audio/mpeg",
            Track = new MusicTrackInfo { Id = rel, Title = name, Source = Name, Bytes = png.Length },
        });
    }

    /// <summary>文件流直出；bytes 非 null 时为落盘失败的内存兜底（Bytes 元数据用提取大小）。</summary>
    private MusicStream OpenStream(string full, string rel, long? bytes)
    {
        var fs = new FileStream(full, FileMode.Open, FileAccess.Read, FileShare.Read, 64 * 1024, useAsync: true);
        var name = Path.GetFileNameWithoutExtension(full);
        return new MusicStream
        {
            Stream = fs,
            MimeType = "audio/mpeg",
            Track = new MusicTrackInfo { Id = rel, Title = name, Source = Name, Bytes = bytes ?? fs.Length },
        };
    }
}
