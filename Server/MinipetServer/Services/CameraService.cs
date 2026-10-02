using System.IO;
using System.Text.Json.Nodes;
using MiniPet.Export;
using MinipetServer.Config;
using MinipetServer.Device;
using MinipetServer.Models;
using SkiaSharp;

namespace MinipetServer.Services;

/// <summary>
/// 「选镜头」（服务端选相机机位）核心服务：把**设备上已有的 BGMAP 地图**列出来、
/// 按需渲染整图预览 / 机位视口，并把坐标交给 <see cref="CameraPlanStore"/> 落盘。
///
/// 为什么要在服务端重渲，而不是直接用导出目录里的 `_ref_&lt;mapId&gt;.png`：
/// 那张参考图是导出时的副产品（会被清理/换口径覆盖），且尺寸口径随导出参数变；
/// 这里按需渲染 + 落 data/cache/previews 缓存，Web 打开就能拿到，与设备里那份包
/// 的 vw/vh 严格对齐（尺寸读 BGMAP 包头，不猜 WZ bbox）。
///
/// 与设备渲染的口径关系（写清楚，避免"看着对不上"的误判）：
///   · 整图预览 = 导出器 `_ref_` 同口径：Back+Tile+Obj 全开、Life/Portal 关，相机=地图中心，
///     零缩放 → 图内像素 (px,py) 即整图世界系 (px,py)（0 = 地图 bbox 左上角）。
///   · 机位视口 = RenderViewport(相机 = 机位 + 窗口/2, zoom=1, 窗口尺寸) → 与桌面同一套 R2 算法；
///     设备是 2x 就近展开，Web 侧按 2x pixelated 显示即"设备实际观感"。
///   · 视差 back（rx≠0）在整图预览里按"整图中心相机"烘焙，与设备在某个机位下的视差偏移
///     可能差 ≤ rx%（机位视口那一张是按真实机位重渲的，无此差异）。
/// </summary>
public sealed class CameraService
{
    /// <summary>兜底可见窗口（世界像素）：480 屏 / Scale 2。真实值按设备 profile 算。</summary>
    public const int FallbackWindow = 240;

    /// <summary>整图预览默认长边上限（px）：4040×1996 整图直出 32MB 位图 + 数 MB PNG，
    /// 页面上根本显示不下，默认降到 1440 长边再落盘。</summary>
    public const int DefaultPreviewMaxW = 1440;

    private readonly ServerPaths _paths;
    private readonly WzService _wz;
    private readonly DeviceRegistry _reg;
    private readonly CameraPlanStore _store;

    /// <summary>本服务**独占**的 MapService：它会改写 MapShow* 分层开关（改开关与渲染必须原子，
    /// 详见 <see cref="_renderLock"/>），不能与别人的实例共用。</summary>
    private readonly MapService _map;

    /// <summary>「改开关 + 渲染」串行锁。MapService 内部只对渲染帧持锁，开关字段是 public 属性，
    /// 多请求并发时会出现"A 设了 flag、B 渲染到一半"，故这里整段串行（渲染本身 240×240 很快，
    /// 整图预览有磁盘缓存兜底）。</summary>
    private readonly object _renderLock = new();

    private readonly object _cacheLock = new();

    // MapInfo 解析（LoadMap）要遍历 WZ 节点，成本高；按 mapId 缓存最近几张（FIFO 淘汰）。
    private const int MaxMapInfoCache = 3;
    private readonly Dictionary<string, MapInfo> _mapInfos = new(StringComparer.Ordinal);
    private readonly Queue<string> _mapInfoOrder = new();

    // 机位视口 PNG 内存缓存：拖动时来回微调同一批坐标不重复渲染（每张 240×240 PNG ≈ 30KB）。
    private const int MaxViewportCache = 64;
    private readonly Dictionary<string, byte[]> _viewportCache = new(StringComparer.Ordinal);
    private readonly Queue<string> _viewportOrder = new();

    public CameraService(ServerPaths paths, WzService wz, DeviceRegistry reg, CameraPlanStore store)
    {
        _paths = paths ?? throw new ArgumentNullException(nameof(paths));
        _wz = wz ?? throw new ArgumentNullException(nameof(wz));
        _reg = reg ?? throw new ArgumentNullException(nameof(reg));
        _store = store ?? throw new ArgumentNullException(nameof(store));
        _map = new MapService(_wz, new CacheManager());
    }

    // ══════════════════════════════════════════════════════════════════════
    // 地图清单（读该设备 manifest-assets.json 的 BGMAP 条目 + BGMAP 包头尺寸）
    // ══════════════════════════════════════════════════════════════════════

    /// <summary>Web 列表/预览/下发都用的地图条目（camelCase 直接进 JSON）。</summary>
    public sealed class CameraMapInfo
    {
        public string MapId { get; set; } = "";
        /// <summary>中文显示名（manifest label 优先，缺失回退 WZ String/Map，再回退 地图_{id}）。</summary>
        public string Label { get; set; } = "";
        /// <summary>manifest 里导出器写的 label（未改写原值，排障用）。</summary>
        public string? AssetLabel { get; set; }
        public int Vw { get; set; }
        public int Vh { get; set; }
        public long Bytes { get; set; }
        /// <summary>BGMAP 包 content_hash（manifest 键；下发切图/SET_MAP 用的同一身份）。</summary>
        public string Hash { get; set; } = "";
        /// <summary>"full"（整图口径，可平移）/ "window"（240×240 窗口包，无平移余量）。</summary>
        public string Viewport { get; set; } = "window";
        /// <summary>"tiled" / "rows"（仅信息展示）。</summary>
        public string Layout { get; set; } = "rows";
        /// <summary>可平移（= 整图包且 vw/vh 大于可见窗口）。</summary>
        public bool Pannable { get; set; }
        public int WinW { get; set; }
        public int WinH { get; set; }
        /// <summary>机位取值上限：x ∈ [0, MaxX]、y ∈ [0, MaxY]（= vw−WinW / vh−WinH，下限 0）。</summary>
        public int MaxX { get; set; }
        public int MaxY { get; set; }
        /// <summary>服务端已记录的机位（未记录 = null，前端显示"未记录"而不是 0,0）。</summary>
        public int? X { get; set; }
        public int? Y { get; set; }
        public bool Saved { get; set; }
        public DateTime? UpdatedUtc { get; set; }
        /// <summary>WZ 小地图缩略图 URL（选择器里做图标；失败自动占位，不影响主流程）。</summary>
        public string ThumbUrl { get; set; } = "";
    }

    /// <summary>某设备全部可用地图 + 已记录机位 + 最后操作过的地图。</summary>
    public (List<CameraMapInfo> maps, string? lastMapId) ListMaps(string deviceId)
    {
        var (winW, winH) = WindowSize(deviceId);
        var (lastMapId, saved) = _store.Get(deviceId);
        var maps = new List<CameraMapInfo>();

        foreach (var (hash, entry) in ReadBgmapEntries(deviceId))
        {
            var mapId = entry["map"]?.GetValue<string>() ?? "";
            if (string.IsNullOrEmpty(mapId)) continue;
            var file = entry["file"]?.GetValue<string>();
            var mpakPath = string.IsNullOrEmpty(file) ? null : Path.Combine(_paths.ExportDirFor(deviceId), file);

            BgmapMeta? meta = mpakPath != null ? TryReadBgmapMeta(mpakPath) : null;
            var viewport = entry["viewport"]?.GetValue<string>();
            var layout = entry["layout"]?.GetValue<string>();

            int vw = meta?.Vw ?? 0, vh = meta?.Vh ?? 0;
            if (vw <= 0 || vh <= 0)
            {
                // 包头读不到（文件被清/损坏）→ 回退 WZ bbox：至少让页面能给出一张可预览的图，
                // 并显式把 viewport 标成 window（不可平移），避免用户按整图去拖一个不存在余量。
                var fallback = LoadMapInfo(mapId);
                vw = fallback != null ? fallback.MaxX - fallback.MinX : winW;
                vh = fallback != null ? fallback.MaxY - fallback.MinY : winH;
                viewport = "window";
            }
            bool full = meta?.HasExtension == true
                ? meta.FullMap
                : string.Equals(viewport, "full", StringComparison.OrdinalIgnoreCase);
            if (string.IsNullOrEmpty(layout))
                layout = meta?.Tiled == true ? "tiled" : "rows";

            int maxX = Math.Max(0, vw - winW);
            int maxY = Math.Max(0, vh - winH);
            saved.TryGetValue(mapId, out var pos);

            maps.Add(new CameraMapInfo
            {
                MapId = mapId,
                Label = ResolveLabel(mapId, entry["label"]?.GetValue<string>()),
                AssetLabel = entry["label"]?.GetValue<string>(),
                Vw = vw,
                Vh = vh,
                Bytes = entry["bytes"]?.GetValue<long>() ?? (meta?.Bytes ?? 0),
                Hash = hash,
                Viewport = full ? "full" : "window",
                Layout = layout!,
                Pannable = full && (maxX > 0 || maxY > 0),
                WinW = winW,
                WinH = winH,
                MaxX = maxX,
                MaxY = maxY,
                X = pos?.X,
                Y = pos?.Y,
                Saved = pos != null,
                UpdatedUtc = pos?.UpdatedUtc,
                ThumbUrl = $"/api/admin/thumb?type=map&id={Uri.EscapeDataString(mapId)}",
            });
        }

        // 数值升序（与素材页/曲库同一口径：解析失败排最后，LINQ 稳定排序保持原相对顺序）
        maps = maps
            .Select(m => (m, ok: int.TryParse(m.MapId, out var n), n))
            .OrderBy(t => t.ok ? 0 : 1)
            .ThenBy(t => t.n)
            .Select(t => t.m)
            .ToList();
        return (maps, lastMapId);
    }

    /// <summary>取该设备已登记的某张地图；未登记返回 null（端点据此 400，防止拿任意 WZ 地图当设备地图渲）。</summary>
    public CameraMapInfo? FindMap(string deviceId, string mapId)
    {
        var (maps, _) = ListMaps(deviceId);
        return maps.FirstOrDefault(m => string.Equals(m.MapId, mapId, StringComparison.Ordinal));
    }

    /// <summary>
    /// 设备可见窗口（世界像素）= **屏宽 / 该设备上报的缩放系数**。
    /// 系数来源：设备 hello 的 `profile.rcScale`（= 固件合成器的 RC_SCALE）；
    /// 0/缺失 = 旧固件 → 回落 PlacementMath.Scale（= 2，历史硬契约）。
    /// 【2026-10-01 修正】原实现写死除以 2，1.85B 做 A1（合成器 480→360、RC_SCALE=1）
    /// 后窗口应为 360 却仍按 240 算 → 机位夹取范围（1560 vs 实际 148）、预览窗口、
    /// 「设备视角」图全部与真机不符（用户报障："摄像机可以展示的地图大了但是对应
    /// 服务端推送的没修改"）。现在按设备上报值算，两端口径自动一致。
    /// </summary>
    public (int w, int h) WindowSize(string deviceId)
    {
        var p = _reg.Get(deviceId)?.Profile;
        int scale = p?.RcScale is > 0 ? p.RcScale : PlacementMath.Scale;
        int sw = p?.W is > 0 ? p.W : 480;
        int sh = p?.H is > 0 ? p.H : 480;
        return (Math.Max(1, sw / scale), Math.Max(1, sh / scale));
    }

    /// <summary>机位夹取：x ∈ [0, MaxX]、y ∈ [0, MaxY]（设备侧 render_cam_set 同样夹取，两边一致）。</summary>
    public static (int x, int y) ClampCamera(CameraMapInfo map, int x, int y)
        => (Math.Clamp(x, 0, Math.Max(0, map.MaxX)), Math.Clamp(y, 0, Math.Max(0, map.MaxY)));

    // ══════════════════════════════════════════════════════════════════════
    // 渲染：整图预览 / 机位视口
    // ══════════════════════════════════════════════════════════════════════

    /// <summary>
    /// 整图预览 PNG（按该地图 BGMAP 的 vw/vh 渲染，可降采样到 <paramref name="maxW"/> 长边）。
    /// 磁盘缓存 data/cache/previews/&lt;mapId&gt;_&lt;hash8&gt;_&lt;vw&gt;x&lt;vh&gt;_w&lt;maxW&gt;.png：
    /// 文件名带 BGMAP content_hash 前 8 位 —— 地图被重推/换口径后缓存自然失效，不用手工清。
    /// 返回 null = 地图加载失败（WZ 未加载/地图数据缺失）。
    /// </summary>
    public byte[]? RenderPreviewPng(string deviceId, CameraMapInfo mapInfo, int maxW, bool refresh = false)
    {
        maxW = Math.Clamp(maxW, 240, 4096);
        var hash8 = mapInfo.Hash.Length >= 8 ? mapInfo.Hash[..8] : "nohash";
        var file = Path.Combine(_paths.PreviewsDir,
            $"{SafeId(mapInfo.MapId)}_{hash8}_{mapInfo.Vw}x{mapInfo.Vh}_w{maxW}.png");
        if (!refresh && File.Exists(file))
        {
            try { return File.ReadAllBytes(file); } catch { /* 读失败 → 重渲 */ }
        }

        var s = LoadMapInfo(mapInfo.MapId);
        if (s == null) return null;

        byte[] png;
        lock (_renderLock)
        {
            ApplyLayerFlags(back: true, tile: true, obj: true);
            var (ccx, ccy) = MapService.GetMapCenter(s);
            using var raw = _map.RenderViewport(s, ccx, ccy, 1f, 0, mapInfo.Vw, mapInfo.Vh);
            if (raw == null) return null;
            using var scaled = Downscale(raw, maxW);
            using var image = SKImage.FromBitmap(scaled);
            using var data = image.Encode(SKEncodedImageFormat.Png, 90);
            png = data.ToArray();
        }

        try
        {
            StorageUtil.AtomicWriteAllBytes(file, png);
            PrunePreviews();
        }
        catch (Exception ex)
        {
            // 缓存写失败不影响出图（下次再渲一遍）
            Console.Error.WriteLine($"[Camera] 预览缓存写入失败 {file}: {ex.Message}");
        }
        return png;
    }

    /// <summary>
    /// 机位视口 PNG：**设备实际会看到的那一屏**（按真实机位渲，含视差层的正确偏移）。
    /// 尺寸 = 设备可见窗口（480 屏 → 240×240 世界像素，1x）；Web 按 2x nearest 显示即设备观感。
    /// 窗口包（vw/vh == 窗口尺寸，无平移余量）时按导出相机的同一规则取景（有 clock 锚点用锚点，
    /// 否则地图中心）——否则"设备看到的那一屏"根本无从谈起。
    /// </summary>
    public byte[]? RenderViewportPng(string deviceId, CameraMapInfo mapInfo, int x, int y)
    {
        var (x2, y2) = ClampCamera(mapInfo, x, y);
        var key = $"{mapInfo.Hash}|{mapInfo.WinW}x{mapInfo.WinH}|{x2},{y2}";
        lock (_cacheLock)
        {
            if (_viewportCache.TryGetValue(key, out var hit)) return hit;
        }

        var s = LoadMapInfo(mapInfo.MapId);
        if (s == null) return null;

        byte[] png;
        lock (_renderLock)
        {
            ApplyLayerFlags(back: true, tile: true, obj: true);
            float camX, camY;
            if (mapInfo.Pannable)
            {
                /* ⚠ 坐标换算（真机坐标系的坑，必须记住）：
                 * 设备机位 (x,y) 是**整图世界系**（0 = 地图 bbox 左上角，固件只有 vw/vh、没有 MinX/MinY），
                 * 而 RenderViewport 的 camCenter 是 **WZ 原始世界坐标** ⇒ 必须加 bbox 原点。
                 * 少加这一下不会报错，只是画面整体偏移 (MinX,MinY)（本图 = -895,-403，偏出去小半屏），
                 * 肉眼像"取景框选的和右边看到的不一致"。 */
                camX = s.MinX + x2 + mapInfo.WinW / 2f;
                camY = s.MinY + y2 + mapInfo.WinH / 2f;
            }
            else
            {
                // 窗口包：BGMAP 是用导出相机烘死的一屏，复刻 AssetExporter.ExportMap 的取景规则
                // （clock 锚点优先，否则地图中心；再按摄像机夹取），保证预览 = 设备那一屏。
                var root = _wz.GetMapWzPath(mapInfo.MapId);
                int ax = _wz.GetIntProperty($"{root}/clock/x");
                int ay = _wz.GetIntProperty($"{root}/clock/y");
                (camX, camY) = (ax != 0 || ay != 0)
                    ? ((float)ax, (float)ay)
                    : MapService.GetMapCenter(s);
                (camX, camY) = MapService.ClampCamera(s, camX, camY, 1f, mapInfo.WinW, mapInfo.WinH);
            }
            using var bmp = _map.RenderViewport(s, camX, camY, 1f, 0, mapInfo.WinW, mapInfo.WinH);
            if (bmp == null) return null;
            using var image = SKImage.FromBitmap(bmp);
            using var data = image.Encode(SKEncodedImageFormat.Png, 90);
            png = data.ToArray();
        }

        lock (_cacheLock)
        {
            if (_viewportCache.Count >= MaxViewportCache && _viewportOrder.Count > 0)
                _viewportCache.Remove(_viewportOrder.Dequeue());
            if (!_viewportCache.ContainsKey(key))
            {
                _viewportCache[key] = png;
                _viewportOrder.Enqueue(key);
            }
        }
        return png;
    }

    // ══════════════════════════════════════════════════════════════════════
    // 内部：地图加载 / 分层开关 / 缩放 / 包头解析
    // ══════════════════════════════════════════════════════════════════════

    /// <summary>
    /// 加载 WZ 地图（带 FIFO 缓存）。地图对象在渲染期只读，可安全复用；
    /// 淘汰只丢引用（SKBitmap 精灵缓存在 MapService 内部，另有上限保护）。
    /// </summary>
    private MapInfo? LoadMapInfo(string mapId)
    {
        lock (_cacheLock)
        {
            if (_mapInfos.TryGetValue(mapId, out var hit)) return hit;
        }
        var loaded = _wz.IsLoaded ? _map.LoadMap(mapId) : null;
        if (loaded == null) return null;
        lock (_cacheLock)
        {
            if (!_mapInfos.ContainsKey(mapId))
            {
                if (_mapInfos.Count >= MaxMapInfoCache && _mapInfoOrder.Count > 0)
                    _mapInfos.Remove(_mapInfoOrder.Dequeue());
                _mapInfos[mapId] = loaded;
                _mapInfoOrder.Enqueue(mapId);
            }
        }
        return loaded;
    }

    /// <summary>分层开关：与导出器 `_ref_&lt;mapId&gt;.png`（AssetExporter.cs:780-787）逐字同口径
    /// ——Back/Tile/Obj 开，Life/Portal/Foothold 关（设备包里本来也没有 life/portal）。</summary>
    private void ApplyLayerFlags(bool back, bool tile, bool obj)
    {
        _map.MapShowBack = back;
        _map.MapShowTile = tile;
        _map.MapShowObj = obj;
        _map.MapShowLife = false;
        _map.MapShowPortal = false;
        _map.MapShowFoothold = false;
    }

    /// <summary>等比缩到长边 maxW（已足够小则原图深拷贝，调用方负责 Dispose）。
    /// 重采样走 ScalePixels + Mitchell（SkiaSharp 3.x 的 DrawBitmap 已无 sampling 重载，
    /// 口径同 ThumbService.FitToCanvas）：整图缩略要能看清地形轮廓，不能最近邻。</summary>
    private static SKBitmap Downscale(SKBitmap src, int maxW)
    {
        if (src.Width <= maxW) return src.Copy();
        float ratio = maxW / (float)src.Width;
        int w = Math.Max(1, maxW);
        int h = Math.Max(1, (int)MathF.Round(src.Height * ratio));
        var dst = new SKBitmap(w, h, SKColorType.Bgra8888, SKAlphaType.Premul);
        src.ScalePixels(dst, new SKSamplingOptions(SKCubicResampler.Mitchell));
        return dst;
    }

    /// <summary>缓存淘汰：保留最近 64 张预览图，按最后写入时间删最旧的（磁盘缓存不设上限会无限长）。</summary>
    private void PrunePreviews()
    {
        try
        {
            var files = new DirectoryInfo(_paths.PreviewsDir).GetFiles("*.png")
                .OrderByDescending(f => f.LastWriteTimeUtc).ToList();
            foreach (var f in files.Skip(64))
            {
                try { f.Delete(); } catch { /* 被占用/权限：下次再删 */ }
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Camera] 预览缓存清理失败: {ex.Message}");
        }
    }

    /// <summary>读该设备 manifest-assets.json 里的 BGMAP 条目（键=content_hash）。</summary>
    private List<(string hash, JsonObject entry)> ReadBgmapEntries(string deviceId)
    {
        var result = new List<(string, JsonObject)>();
        var indexPath = Path.Combine(_paths.ExportDirFor(deviceId), "manifest-assets.json");
        if (!File.Exists(indexPath)) return result;
        try
        {
            var root = JsonNode.Parse(File.ReadAllText(indexPath)) as JsonObject;
            if (root?["assets"] is not JsonObject assets) return result;
            foreach (var kv in assets)
            {
                if (kv.Value is not JsonObject e) continue;
                if (!string.Equals(e["kind"]?.GetValue<string>(), "BGMAP", StringComparison.OrdinalIgnoreCase)) continue;
                if (!string.Equals(e["selector"]?.GetValue<string>(), "map", StringComparison.OrdinalIgnoreCase)) continue;
                result.Add((kv.Key, e));
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Camera] 读 {indexPath} 失败: {ex.Message}");
        }
        return result;
    }

    /// <summary>BGMAP 包头元信息（尺寸与口径的唯一事实源）。</summary>
    public sealed class BgmapMeta
    {
        public int Vw { get; set; }
        public int Vh { get; set; }
        public long Bytes { get; set; }
        public bool HasExtension { get; set; }
        public bool FullMap { get; set; }
        public bool Tiled { get; set; }
        public int StripCount { get; set; }
    }

    /// <summary>
    /// 只读 BGMAP 包头/尾扩展块解出 vw/vh/flags（**不整包读**：整图 BGMAP 17MB 级，
    /// 每次列表都全量读会把页拉垮）。定位规则与外层信封一致：
    ///   MPAK 头 40B：magic(4) ver(2) flags(2) pad(8) kind(8) hash(8) payload_len(4) reserved(4)
    ///   payload 头 56B：map_id(32) vw(2) vh(2) static_len(4) static_off(4) tile_len(4) tile_off(4) strip_count(4)
    ///   尾扩展块：ext_off = align4(tile_off + tile_len)，16B 头，flags 在 +12（bit0 整图 / bit1 分块）
    /// </summary>
    public static BgmapMeta? TryReadBgmapMeta(string mpakPath)
    {
        try
        {
            using var fs = File.OpenRead(mpakPath);
            var meta = new BgmapMeta { Bytes = fs.Length };
            Span<byte> mpakHeader = stackalloc byte[Mpak.HeaderSize];
            if (fs.Read(mpakHeader) != Mpak.HeaderSize) return null;
            if (!mpakHeader[..4].SequenceEqual("MPAK"u8)) return null;
            uint payloadLen = BitConverter.ToUInt32(mpakHeader[32..36]);
            if (payloadLen < 56) return null;

            Span<byte> hdr = stackalloc byte[56];
            if (fs.Read(hdr) != 56) return null;
            meta.Vw = BitConverter.ToUInt16(hdr[32..34]);
            meta.Vh = BitConverter.ToUInt16(hdr[34..36]);
            uint staticLen = BitConverter.ToUInt32(hdr[36..40]);
            uint staticOff = BitConverter.ToUInt32(hdr[40..44]);
            uint tileLen = BitConverter.ToUInt32(hdr[44..48]);
            uint tileOff = BitConverter.ToUInt32(hdr[48..52]);
            meta.StripCount = (int)BitConverter.ToUInt32(hdr[52..56]);
            if (staticOff > payloadLen || tileOff > payloadLen) return null;   // 明显损坏，别拿它当尺寸用

            long tileEnd = (long)tileOff + tileLen;
            long extOff = (tileEnd + 3) & ~3L;
            if (payloadLen >= extOff + BgmapPackWriter.BgmapExtensionHeaderSize)
            {
                fs.Seek(Mpak.HeaderSize + extOff, SeekOrigin.Begin);
                Span<byte> ext = stackalloc byte[BgmapPackWriter.BgmapExtensionHeaderSize];
                if (fs.Read(ext) == ext.Length &&
                    BitConverter.ToUInt32(ext[0..4]) == BgmapPackWriter.BgmapExtensionMagic)
                {
                    uint flags = BitConverter.ToUInt32(ext[12..16]);
                    meta.HasExtension = true;
                    meta.FullMap = (flags & BgmapPackWriter.BgmapFlagFullMap) != 0;
                    meta.Tiled = (flags & BgmapPackWriter.BgmapFlagTiled) != 0;
                }
            }
            return meta;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Camera] 读 BGMAP 包头失败 {mpakPath}: {ex.Message}");
            return null;
        }
    }

    /// <summary>中文名：manifest label（导出器已按 WZ 解析）→ WZ 现查 → 地图_{id}。</summary>
    private string ResolveLabel(string mapId, string? assetLabel)
    {
        if (!string.IsNullOrWhiteSpace(assetLabel) && assetLabel != $"map_{mapId}") return assetLabel!;
        try
        {
            var name = _wz.IsLoaded ? _wz.GetMapName(mapId) : "";
            if (!string.IsNullOrEmpty(name) && name != $"map_{mapId}") return name;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Camera] 取地图名 {mapId} 失败: {ex.Message}");
        }
        return $"地图_{mapId}";
    }

    /// <summary>文件名安全化（mapId 是数字串，这里只防脏数据带路径分隔符）。</summary>
    private static string SafeId(string id)
    {
        var chars = id.Select(c => char.IsLetterOrDigit(c) || c is '-' or '_' ? c : '_').ToArray();
        return new string(chars);
    }
}
