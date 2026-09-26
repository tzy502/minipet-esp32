using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Hashing;
using System.Linq;
using System.Text.Json;
using System.Threading;
using MinipetServer.Models;
using MinipetServer.Services;
using SkiaSharp;

namespace MiniPet.Export;

/// <summary>导出选项（CLI / 宿主服务共用）。</summary>
public sealed class ExportOptions
{
    /// <summary>装扮 JSON 路径（CharacterAppearance 可序列化结构；缺省用 seed/default-appearance.json）。</summary>
    public string? AppearancePath;
    /// <summary>设备 profile 名或路径（默认 amoled216）。</summary>
    public string Profile = "amoled216";
    /// <summary>要导出的地图 id 列表。</summary>
    public List<string> Maps = new();
    /// <summary>输出根目录（设备子目录 OutRoot/{DeviceId}）。</summary>
    public string OutRoot = "data/cache/export";
    public string DeviceId = "default";
    /// <summary>字体档位（px）；空 = 不导出 FONT。</summary>
    public List<int> FontSizes = new() { 16 };
    /// <summary>是否导出 AUDIO_META（WZ 曲库）。</summary>
    public bool IncludeAudio = true;
    /// <summary>是否导出 fontTime 时钟数字 PARTS。</summary>
    public bool IncludeFontTime = true;
    /// <summary>manifest firmware 版本（占位；OTA 在 M6+ 生效）。</summary>
    public string FirmwareVer = "0.0.0";
    /// <summary>FONOT 字符集语料文件（UTF-8 文本，取唯一字符）；缺省 = 内置默认字符集。</summary>
    public string? CharsetFile;
    /// <summary>字体族覆盖（缺省走宋体候选链）。</summary>
    public string? FontFamily;
}

/// <summary>单个导出产物（一个 hash 一个文件）。</summary>
public sealed class ExportedAsset
{
    public ulong Hash;
    public MpakKind Kind;
    public required byte[] Bytes;
    public required string FileName;
    public long ByteCount => Bytes.Length;
    public string Label = "";
    /// <summary>map|paperdoll|npc|clock；null = 不进设备选择器。</summary>
    public string? Selector;
    /// <summary>manifest 扩展字段（entity/action/map/thumb）。</summary>
    public Dictionary<string, object?> Extra = new();
}

/// <summary>导出结果摘要。</summary>
public sealed class ExportSummary
{
    public string DeviceDir = "";
    public List<ExportedAsset> Assets = new();
    public List<string> Warnings = new();
    public Dictionary<string, int[]> ClockTable = new();
    public string AssetsManifestPath = "";
    public string ManifestPath = "";
    public int Rev;
}

/// <summary>
/// M2 素材导出器编排（算法规格 §十）：
/// 装扮 JSON + 设备 profile → 复用 PaperdollService 部件管线产出 PARTS（整套装扮一个包，含 25 表情变体）
/// + 每动作一个 LAYOUT；地图 → BGMAP（static_back 全 back 快照 / tile 层关 back 只渲 tile+obj / 条带按 cx 周期
/// 预平铺为独立小 PARTS）；fontTime → PARTS 保留段 900..912；FONT / AUDIO_META 按需；
/// 输出 data/cache/export/{deviceId}/ 下 {hash}.mpak + manifest-assets.json + manifest.json（rev 每设备单调递增）。
/// </summary>
///
/// <remarks>
/// ═════════════ 导出器 ↔ 设备端渲染契约（LAYOUT 坐标语义，金样测试 ExportReplayTests 锁死）═════════════
///
/// 「LAYOUT piece x/y = 桌面版合成画布内 piece 左上角绝对坐标（含 origin 平移），画布包围盒 =
///   所有 piece 联合；设备端渲染 = 画布 1x 合成 → 2x nearest → 屏幕水平居中/底对齐 40px」
///
/// 逐条展开（与桌面版 PaperdollService.RenderFrame 逐像素同轴）：
/// 1. piece 绘制位置公式（桌面版权威式）：canvas_x = FinalX - bodyAnchorX - bounds.Left + moveDx，
///    canvas_y 同理。其中 FinalX/Y = AnchorX/Y - origin（含 WZ origin 平移的 navel 系坐标）、
///    bodyAnchor = body 部件锚点（navel 系原点）、bounds = 该动作全部帧全部 piece 矩形的联合包围盒、
///    move = 该帧 WZ move 位移。导出时 move 直接含入 x/y。
/// 2. 画布 = 一个动作一张（跨帧共用，防逐帧抖动），尺寸 = bounds.W×bounds.H，
///    随 manifest-assets.json 对应 LAYOUT 条目的 "bounds":[w,h] 下发（像素 1x）；x/y ∈ [0, bounds]。
/// 3. 帧头 move_dx/move_dy 仅为桌面版对齐参考数据（已含入 x/y），设备端不得再加算。
/// 4. 设备端摆放 = 画布 1x 合成 → 2x nearest 放大 → 屏幕水平居中、底对齐 40px；
///    落点公式唯一权威实现见 Export/PlacementMath.cs（固件照抄同一公式）。
/// 金样测试：ExportReplayTests 按「画布绝对坐标 1x 合成」重建位图，与 RenderFrame 输出同包围盒逐像素对比。
/// </remarks>
public sealed class AssetExporter
{
    /// <summary>导出的常见动作集（算法规格 §十.3；无帧的动作自动跳过）。</summary>
    public static readonly string[] ExportActions =
    {
        "walk1", "stand1", "stand2", "blink", "jump", "fly", "prone", "ladder", "rope", "alert", "heal",
    };

    /// <summary>默认动作引用（设备端开机/待机播它；manifest-assets.json LAYOUT/PARTS 条目 "defaultAction"）。</summary>
    public const string DefaultAction = "stand1";

    private readonly WzService _wz;
    private readonly CacheManager _cache;
    private readonly MapService _map;
    private readonly MusicCatalogService _music;

    public AssetExporter(WzService wz)
    {
        _wz = wz ?? throw new ArgumentNullException(nameof(wz));
        _cache = new CacheManager();
        _map = new MapService(_wz, _cache);
        _music = new MusicCatalogService(_wz);
    }

    /// <summary>
    /// 宿主服务入口（PaperdollPackService 用）：只为给定外观产出装扮资产（PARTS 整包 +
    /// 各动作 LAYOUT + fontTime 时钟数字），不写盘、不动 manifest——写盘与索引合并由调用方
    /// （按设备目录）负责。返回外观 hash 与资产列表。
    /// </summary>
    public (string AppearanceHash, List<ExportedAsset> Assets) ExportAppearanceAssets(CharacterAppearance appearance)
    {
        if (!_wz.IsLoaded) throw new InvalidOperationException("WZ 未加载（先调用 WzService.LoadWz）");
        var summary = new ExportSummary();
        ExportPaperdoll(appearance, summary);
        try
        {
            var payload = FontPackWriter.ExportFontTimeParts(_wz);
            AddAsset(summary, MpakKind.Parts, payload, "时钟数字 fontTime", selector: "clock");
        }
        catch (Exception ex) { summary.Warnings.Add($"fontTime 导出失败: {ex.Message}"); }
        return (PaperdollService.HashAppearance(appearance), summary.Assets);
    }

    public ExportSummary Run(ExportOptions o)
    {
        if (o == null) throw new ArgumentNullException(nameof(o));
        if (!_wz.IsLoaded) throw new InvalidOperationException("WZ 未加载（先调用 WzService.LoadWz）");

        var summary = new ExportSummary();
        string deviceDir = Path.Combine(o.OutRoot, o.DeviceId);
        Directory.CreateDirectory(deviceDir);
        summary.DeviceDir = deviceDir;

        // ── 1. 装扮：PARTS + 全动作 LAYOUT ──
        var profile = DeviceProfile.Load(o.Profile);
        if (!string.IsNullOrEmpty(o.AppearancePath))
        {
            ExportPaperdoll(LoadAppearance(o.AppearancePath), summary);
        }
        else
        {
            summary.Warnings.Add("未指定 --appearance，跳过纸娃娃导出");
        }

        // ── 2. 地图：BGMAP + 条带小 PARTS + 缩略图 + clock_table ──
        foreach (var mapId in o.Maps.Where(m => !string.IsNullOrWhiteSpace(m)).Distinct())
        {
            try { ExportMap(mapId.Trim(), profile, summary); }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[AssetExporter] 地图 {mapId} 导出失败: {ex}");
                summary.Warnings.Add($"地图 {mapId} 导出失败: {ex.Message}");
            }
        }

        // ── 3. fontTime 时钟数字（PARTS 保留段 900..912）──
        if (o.IncludeFontTime)
        {
            try
            {
                var payload = FontPackWriter.ExportFontTimeParts(_wz);
                AddAsset(summary, MpakKind.Parts, payload, "时钟数字 fontTime", selector: "clock");
            }
            catch (Exception ex) { summary.Warnings.Add($"fontTime 导出失败: {ex.Message}"); }
        }

        // ── 4. FONT（按档位一包一字号）──
        foreach (var size in o.FontSizes.Distinct().OrderBy(s => s))
        {
            try
            {
                string charset = o.CharsetFile != null && File.Exists(o.CharsetFile)
                    ? File.ReadAllText(o.CharsetFile)
                    : FontPackWriter.DefaultCharset();
                var payload = FontPackWriter.RenderFontPack(size, charset, o.FontFamily);
                AddAsset(summary, MpakKind.Font, payload, $"字体 {size}px", selector: null);
            }
            catch (Exception ex) { summary.Warnings.Add($"FONT {size}px 导出失败: {ex.Message}"); }
        }

        // ── 5. AUDIO_META（WZ 曲库元数据）──
        if (o.IncludeAudio)
        {
            try
            {
                var tracks = _music.GetCatalogAsync(CancellationToken.None).GetAwaiter().GetResult() ?? new List<MusicTrack>();
                var metas = tracks.Select(t => new AudioMetaWriter.AudioTrackMeta
                {
                    Id = AudioMetaWriter.TrackIdForKey(t.Key),
                    Title = t.Track,
                    Source = 0, // 0=WZ
                    DurationS = (uint)Math.Max(0, t.Ms / 1000),
                }).ToList();
                var payload = AudioMetaWriter.Build(metas);
                AddAsset(summary, MpakKind.AudioMeta, payload, $"曲库元数据（{metas.Count} 曲）", selector: null);
            }
            catch (Exception ex) { summary.Warnings.Add($"AUDIO_META 导出失败: {ex.Message}"); }
        }

        // ── 6. 落盘 + manifest ──
        foreach (var a in summary.Assets)
        {
            File.WriteAllBytes(Path.Combine(deviceDir, a.FileName), a.Bytes);
        }
        ManifestBuilder.WriteAssetsManifest(deviceDir, o.DeviceId, summary.Assets, out var assetsManifestPath);
        summary.AssetsManifestPath = assetsManifestPath;
        summary.Rev = ManifestBuilder.WriteFullManifest(deviceDir, summary.Assets, o.FirmwareVer, summary.ClockTable, out var manifestPath);
        summary.ManifestPath = manifestPath;
        return summary;
    }

    // ═══════════════════════════════════════════
    // 单动作导出（回放测试/预览用：ExportAdapter 消费）
    // ═══════════════════════════════════════════

    /// <summary>
    /// 导出一套装扮在单个动作下的 PARTS 完整 MPAK 文件字节
    /// （含该动作全部帧部件 + face 类 25 表情变体组，offset 显式寻址）。
    /// </summary>
    public byte[] ExportParts(CharacterAppearance appearance, string action)
    {
        var core = ExportAppearanceCore(appearance, new[] { action }, null);
        return Mpak.Build(MpakKind.Parts, core.PartsPayload);
    }

    /// <summary>导出一套装扮在单个动作下的 LAYOUT 完整 MPAK 文件字节（无该动作帧时抛 InvalidOperationException）。</summary>
    public byte[] ExportLayout(CharacterAppearance appearance, string action)
    {
        var core = ExportAppearanceCore(appearance, new[] { action }, null);
        if (!core.Layouts.TryGetValue(action, out var payload))
            throw new InvalidOperationException($"动作 {action} 无可导出帧（appearance hash={PaperdollService.HashAppearance(appearance)}）");
        return Mpak.Build(MpakKind.Layout, payload);
    }

    // ═══════════════════════════════════════════
    // 装扮导出（PARTS + LAYOUT）
    // ═══════════════════════════════════════════

    private static CharacterAppearance LoadAppearance(string path)
    {
        if (!File.Exists(path)) throw new FileNotFoundException($"装扮 JSON 不存在: {path}");
        var opts = new JsonSerializerOptions { PropertyNameCaseInsensitive = true, ReadCommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true };
        return JsonSerializer.Deserialize<CharacterAppearance>(File.ReadAllText(path), opts)
            ?? throw new InvalidDataException($"装扮 JSON 解析失败: {path}");
    }

    /// <summary>face 类部件（表情变体维度）：脸型 + 101 面饰（表情结构，对齐 CollectPiecesForFrame 分类）。</summary>
    private static bool IsExpressionCategory(string category)
        => category == "face" || category == "faceaccessory";

    /// <summary>一次装扮导出的核心产物（PARTS payload + 各动作 LAYOUT payload）。</summary>
    private sealed class AppearanceExport
    {
        public byte[] PartsPayload = Array.Empty<byte>();
        public Dictionary<string, byte[]> Layouts = new(StringComparer.Ordinal);
        /// <summary>动作 → 画布联合包围盒 [w,h]（1x 像素；manifest LAYOUT 条目 "bounds"，契约见类头注释）。</summary>
        public Dictionary<string, int[]> LayoutBounds = new(StringComparer.Ordinal);
        public string AppearanceHash = "";
        public List<string> SkippedActions = new();
    }

    private void ExportPaperdoll(CharacterAppearance appearance, ExportSummary summary)
    {
        var core = ExportAppearanceCore(appearance, ExportActions, summary.Warnings);
        if (core.PartsPayload.Length > 0)
        {
            AddAsset(summary, MpakKind.Parts, core.PartsPayload, "纸娃娃装扮", selector: "paperdoll",
                extra: new Dictionary<string, object?>
                {
                    ["entity"] = "paperdoll:default",
                    ["appearanceHash"] = core.AppearanceHash,
                    ["defaultAction"] = DefaultAction,
                });
        }
        foreach (var (action, payload) in core.Layouts)
        {
            var extra = new Dictionary<string, object?>
            {
                ["entity"] = "paperdoll:default",
                ["action"] = action,
                ["defaultAction"] = DefaultAction,
            };
            if (core.LayoutBounds.TryGetValue(action, out var bounds))
                extra["bounds"] = bounds; // [w,h] 画布联合包围盒（1x 像素，契约见类头注释）
            AddAsset(summary, MpakKind.Layout, payload, $"布局 {action}", selector: "paperdoll", extra: extra);
        }
    }

    /// <summary>
    /// 装扮导出核心：对给定动作集跑 CollectPiecesForFrame 组合管线（25 表情 × 各帧），
    /// 组装整套装扮 PARTS（含表情变体组）+ 每动作一个 LAYOUT。warnings 可为 null（单动作测试路径）。
    /// </summary>
    private AppearanceExport ExportAppearanceCore(CharacterAppearance appearance, IEnumerable<string> actions,
        List<string>? warnings)
    {
        var result = new AppearanceExport();
        var doll = new PaperdollService(_wz, _cache, new SpriteService(_cache));
        doll.SetAppearance(appearance);
        string hash = PaperdollService.HashAppearance(appearance);
        result.AppearanceHash = hash;
        void Warn(string msg) { warnings?.Add(msg); Console.Error.WriteLine($"[AssetExporter] {msg}"); }

        // 25 表情（KnownExpressions 顺序 = 设备端 expression 列表顺序）
        var expressions = PaperdollService.KnownExpressions.Select(e => e.Key).ToList();
        int defaultIdx = expressions.IndexOf(PaperdollService.ExpressionDefault);
        if (defaultIdx < 0) defaultIdx = 0;

        var allocator = new PartIdAllocator();
        // 登记键 → 部件（非表情件按 piecePath；表情槽按 FaceSlotKey）
        var partsByPath = new Dictionary<string, RegisteredPart>(StringComparer.Ordinal);
        // 表情家族：famKey(category|pieceName) → 25 槽位（slot i = 第 i 表情的 piecePath；null=缺帧回退）
        var families = new Dictionary<string, string?[]>(StringComparer.Ordinal);
        // 家族 → 连续 25 id 的基准 / 组号（per 导出，防跨外观串组）
        var familyBase = new Dictionary<string, long>(StringComparer.Ordinal);
        var familyGroup = new Dictionary<long, int>();
        int groupCounter = 0;

        foreach (var action in actions)
        {
            int frameCount;
            try { frameCount = doll.GetFrameCount(action); } catch { frameCount = 0; }
            if (frameCount <= 0) { result.SkippedActions.Add(action); Warn($"动作 {action} 无帧，跳过"); continue; }

            var frames = new List<LayoutPackWriter.LayoutFrame>();
            PaperdollInternals.UnionBounds boundsLast = default; // 各帧共用同一画布（GetBounds 按动作缓存），记成功帧的即可
            for (int f = 0; f < frameCount; f++)
            {
                List<PaperdollInternals.PieceView> pieces;
                PaperdollInternals.UnionBounds bounds;
                (int Dx, int Dy) move;
                try
                {
                    pieces = PaperdollInternals.RunPipeline(doll, hash, appearance, action, f,
                        PaperdollService.ExpressionDefault, out bounds, out move);
                }
                catch (Exception ex)
                {
                    Warn($"动作 {action} 帧 {f} 管线异常: {ex.Message}");
                    continue;
                }
                if (pieces.Count == 0 || bounds.W <= 0 || bounds.H <= 0) continue;
                boundsLast = bounds;

                // 画布原点 = body 锚点（对齐 RenderFrame：bx/by 取 body 锚）
                var bodyP = pieces.FirstOrDefault(p => p.Category == "body");
                int bx = bodyP?.AnchorX ?? 0, by = bodyP?.AnchorY ?? 0;

                // 表情变体登记：对该帧跑全部表情的部件收集（_frameSourcesCache 有缓存，仅首帧真实走 WZ）
                for (int ei = 0; ei < expressions.Count; ei++)
                {
                    if (ei == defaultIdx) continue;
                    List<PaperdollInternals.PieceView> rawPieces;
                    try { rawPieces = PaperdollInternals.CollectRaw(doll, hash, appearance, action, f, expressions[ei]); }
                    catch { continue; }
                    foreach (var p in rawPieces.Where(p => IsExpressionCategory(p.Category)))
                    {
                        string famKey = $"{p.Category}|{p.PieceName}";
                        if (!families.TryGetValue(famKey, out var slots))
                        {
                            slots = new string?[expressions.Count];
                            families[famKey] = slots;
                        }
                        slots[ei] ??= p.PiecePath;
                    }
                }

                var layoutPieces = new List<LayoutPackWriter.LayoutPiece>();
                var drawOrder = pieces.OrderByDescending(p => p.ZIndex).ToList(); // 底→顶绘制序
                for (int i = 0; i < drawOrder.Count; i++)
                {
                    var p = drawOrder[i];
                    uint partId;
                    byte exprIndex;
                    if (IsExpressionCategory(p.Category))
                    {
                        string famKey = $"{p.Category}|{p.PieceName}";
                        if (!families.TryGetValue(famKey, out var slots))
                        {
                            slots = new string?[expressions.Count];
                            families[famKey] = slots;
                        }
                        slots[defaultIdx] ??= p.PiecePath;
                        if (!familyBase.TryGetValue(famKey, out var baseId))
                        {
                            baseId = allocator.Alloc(p.Category, expressions.Count); // 连续 25 个 id
                            familyBase[famKey] = baseId;
                            familyGroup[baseId] = ++groupCounter;
                        }
                        partId = (uint)(baseId + defaultIdx);
                        exprIndex = (byte)defaultIdx;
                        RegisterFaceSlot(p, (int)baseId, defaultIdx, familyGroup, partsByPath);
                    }
                    else
                    {
                        if (!partsByPath.TryGetValue(p.PiecePath, out var reg))
                        {
                            reg = new RegisteredPart
                            {
                                PartId = allocator.Alloc(p.Category, 1),
                                ExprGroup = 0,
                                PiecePath = p.PiecePath,
                            };
                            partsByPath[p.PiecePath] = reg;
                        }
                        partId = reg.PartId;
                        exprIndex = LayoutPackWriter.ExprNone;
                    }

                    // 契约（见类头注释）：x/y = 桌面版合成画布内 piece 左上角绝对坐标
                    //   = FinalX - bx - bounds.Left + move（含 origin 平移 + 帧位移 move + 画布原点平移），
                    // 与 RenderFrame 的 canvas.DrawBitmap 位置逐像素同轴；画布包围盒 = bounds（manifest "bounds"）。
                    // 帧头 move_dx/move_dy 仅参考数据，设备端不得再加算。
                    int px = p.FinalX - bx - bounds.Left + move.Dx;
                    int py = p.FinalY - by - bounds.Top + move.Dy;
                    if (px is < short.MinValue or > short.MaxValue || py is < short.MinValue or > short.MaxValue)
                    {
                        Warn($"{action}/{f} 部件 {p.PiecePath} 坐标越界 ({px},{py})，截断");
                        px = Math.Clamp(px, short.MinValue, short.MaxValue);
                        py = Math.Clamp(py, short.MinValue, short.MaxValue);
                    }
                    layoutPieces.Add(new LayoutPackWriter.LayoutPiece
                    {
                        PartId = partId,
                        ExprIndex = exprIndex,
                        X = (short)px,
                        Y = (short)py,
                        Flip = 0,
                        Z = (sbyte)Math.Clamp(drawOrder.Count - 1 - i, sbyte.MinValue, sbyte.MaxValue),
                    });
                }

                frames.Add(new LayoutPackWriter.LayoutFrame
                {
                    DelayMs = (uint)Math.Max(1, doll.GetFrameDelay(action, f)),
                    MoveDx = (short)Math.Clamp(move.Dx, short.MinValue, short.MaxValue),
                    MoveDy = (short)Math.Clamp(move.Dy, short.MinValue, short.MaxValue),
                    Pieces = layoutPieces,
                });
            }

            if (frames.Count == 0) { result.SkippedActions.Add(action); Warn($"动作 {action} 全部帧无效，跳过"); continue; }
            result.Layouts[action] = LayoutPackWriter.Build(new LayoutPackWriter.LayoutInput
            {
                EntityId = 1, // paperdoll:default
                Action = action,
                Expressions = expressions,
                Frames = frames,
            });
            // 画布联合包围盒（RunPipeline 的 bounds = GetBounds(hash, a, action)，跨帧共用同一张画布）
            result.LayoutBounds[action] = new[] { boundsLast.W, boundsLast.H };
        }

        // 补齐表情家族 25 槽位的 part 登记（缺帧槽回退 default / 首个可用槽，ShareDataOf 去重数据）
        foreach (var (famKey, slots) in families)
        {
            if (!familyBase.TryGetValue(famKey, out var baseId)) continue; // 家族从未在布局出现（数据残缺）→ 不导出
            var group = familyGroup[baseId];
            string? fallback = slots[defaultIdx] ?? slots.FirstOrDefault(s => s != null);
            for (int ei = 0; ei < slots.Length; ei++)
            {
                string path = slots[ei] ?? fallback;
                if (path == null) continue; // 整族无素材（不可能走到，防御）
                string key = FaceSlotKey(baseId, ei);
                if (partsByPath.ContainsKey(key)) continue;
                partsByPath[key] = new RegisteredPart
                {
                    PartId = (uint)(baseId + ei),
                    ExprGroup = (ushort)group,
                    PiecePath = path,
                    ShareDataOf = path,
                };
            }
        }

        // 组装 PARTS（整套装扮一个包）
        var entries = new List<PartPackWriter.PartEntry>();
        foreach (var reg in partsByPath.Values.OrderBy(r => r.PartId))
        {
            var bmp = DecodePieceBitmap(reg.PiecePath);
            if (bmp == null) { Warn($"部件位图缺失: {reg.PiecePath}"); continue; }
            var (ox, oy) = _wz.GetOrigin(reg.PiecePath);
            entries.Add(new PartPackWriter.PartEntry
            {
                PartId = reg.PartId,
                ExprGroup = reg.ExprGroup,
                Bitmap = bmp,
                OriginX = ox,
                OriginY = oy,
                ShareBitmapOf = reg.ShareDataOf,
            });
        }
        if (entries.Count > 0)
        {
            result.PartsPayload = PartPackWriter.Build(entries);
        }
        // 部件位图缓存随导出收尾释放（防同进程二次导出拿到已释放对象）
        foreach (var bmp in _bmpCache.Values) bmp.Dispose();
        _bmpCache.Clear();
        return result;
    }

    private readonly Dictionary<string, SKBitmap> _bmpCache = new(StringComparer.Ordinal);

    private static string FaceSlotKey(long baseId, int exprIndex) => $"__faceslot__{baseId}+{exprIndex}";

    /// <summary>登记家族 default 槽（布局引用的部件本体）。</summary>
    private static void RegisterFaceSlot(PaperdollInternals.PieceView p, int baseId, int defaultIdx,
        Dictionary<long, int> familyGroup, Dictionary<string, RegisteredPart> partsByPath)
    {
        string key = FaceSlotKey(baseId, defaultIdx);
        if (partsByPath.ContainsKey(key)) return;
        var group = familyGroup[baseId];
        partsByPath[key] = new RegisteredPart
        {
            PartId = (uint)(baseId + defaultIdx),
            ExprGroup = (ushort)group,
            PiecePath = p.PiecePath,
            ShareDataOf = p.PiecePath,
        };
    }

    private SKBitmap? DecodePieceBitmap(string piecePath)
    {
        if (_bmpCache.TryGetValue(piecePath, out var cached)) return cached;
        SKBitmap? bmp = null;
        try
        {
            var png = _wz.ExtractPng(piecePath);
            if (png != null) bmp = SKBitmap.Decode(png);
        }
        catch (Exception ex) { Console.Error.WriteLine($"[AssetExporter] DecodePieceBitmap({piecePath}): {ex.Message}"); }
        if (bmp != null) _bmpCache[piecePath] = bmp;
        return bmp;
    }

    private sealed class RegisteredPart
    {
        public uint PartId;
        public ushort ExprGroup;
        public string PiecePath = "";
        /// <summary>数据区复用键（表情槽回退/重复路径去重）。</summary>
        public string? ShareDataOf;
    }

    // ═══════════════════════════════════════════
    // 地图导出（BGMAP + 条带 + 缩略图 + clock_table）
    // ═══════════════════════════════════════════

    /// <summary>条带判定 = ScrollH/ScrollV 位（对齐 MapService.GetBackTileMode：bit2/bit3）。</summary>
    private static int BackTileMode(int type) => type switch
    {
        0 => 0, 1 => 1, 2 => 2, 3 => 3, 4 => 5, 5 => 10, 6 => 7, 7 => 11, _ => 0,
    };

    private static bool IsStripBack(MapBack b)
    {
        if (b.Ani == 2 || b.Alpha == 0) return false;
        if (b.ScreenMode != 0 && b.ScreenMode != 1) return false; // 可见性判定同 DrawBackViewport（DisplayMode=0）
        int tm = BackTileMode(b.Type);
        return (tm & 4) != 0 || (tm & 8) != 0;
    }

    private void ExportMap(string mapId, DeviceProfile profile, ExportSummary summary)
    {
        var map = _map.LoadMap(mapId);
        if (map == null) { summary.Warnings.Add($"地图 {mapId} 加载失败"); return; }

        int vw = profile.ViewportW, vh = profile.ViewportH;
        // 相机中心：含 clock 配置的图以 clock 锚点为优先（2026-09-26 定稿）——480 视口下地图中心
        // 相机多看不到 clock 面板（实测 18/26 出界）；这 26 张"售票处/码头"图的存在意义就是
        // 显示时钟，以锚点为中心烘焙 → 面板入镜 + 时钟落面板（与桌面版 1080 视口验收一致）。
        // 无 clock 的图保持地图中心。ClockTableSeeder 换算与此同口径。
        string clockMapRoot = MapService.GetMapWzPath(mapId);
        int clockAnchorX = _wz.GetIntProperty($"{clockMapRoot}/clock/x");
        int clockAnchorY = _wz.GetIntProperty($"{clockMapRoot}/clock/y");
        bool hasClock = clockAnchorX != 0 || clockAnchorY != 0;
        var (ccx, ccy) = hasClock
            ? ((float)clockAnchorX, (float)clockAnchorY)
            : MapService.GetMapCenter(map);
        var (camX, camY) = MapService.ClampCamera(map, ccx, ccy, 1f, vw, vh); // 必须夹取（clock-display-spec §五）

        // 条带 = ScrollH/V 项（profile 关条带时置空 → strip_count=0）
        var stripBacks = profile.Strips ? map.Backs.Where(IsStripBack).ToList() : new List<MapBack>();

        // 1. static_back：全 back 快照（剔除条带项，避免动画层残影；front back 一并烘入）
        var staticMap = CloneMap(map, map.Backs.Where(b => !stripBacks.Contains(b)).ToList());
        _map.MapShowBack = true;
        _map.MapShowTile = false;
        _map.MapShowObj = false;
        _map.MapShowLife = false;
        _map.MapShowPortal = false;
        _map.MapShowFoothold = false;
        var staticBmp = _map.RenderViewport(staticMap, camX, camY, 1f, 0, vw, vh);
        if (staticBmp == null) { summary.Warnings.Add($"地图 {mapId} static_back 渲染失败"); return; }

        // 2. tile_layer：关 back 只渲 tile/obj（透明底，RGBA5650）
        _map.MapShowBack = false;
        _map.MapShowTile = true;
        _map.MapShowObj = true;
        _map.MapShowLife = false;
        _map.MapShowPortal = false;
        var tileBmp = _map.RenderViewport(map, camX, camY, 1f, 0, vw, vh);

        // 3. 条带：图按 cx 周期预平铺 → 独立小 PARTS 包（part_id=1，group=0）
        var strips = new List<BgmapPackWriter.BgmapStrip>();
        foreach (var b in stripBacks)
        {
            try
            {
                string texPath = b.Resource.ResourceUrl;
                var png = _wz.ExtractPng(texPath);
                if (png == null) { summary.Warnings.Add($"条带素材缺失 {texPath}"); continue; }
                using var srcBmp = SKBitmap.Decode(png);
                if (srcBmp == null) continue;
                int period = b.Cx > 0 ? b.Cx : srcBmp.Width;
                using var tiled = PreTileHorizontal(srcBmp, period);
                var (ox, oy) = _wz.GetOrigin(texPath);
                var stripPayload = PartPackWriter.Build(new[]
                {
                    new PartPackWriter.PartEntry { PartId = 1, ExprGroup = 0, Bitmap = tiled, OriginX = ox, OriginY = oy },
                });
                var stripAsset = AddAsset(summary, MpakKind.Parts, stripPayload, $"条带 {mapId}#{b.Id}", selector: null);

                int tm = BackTileMode(b.Type);
                bool scrollH = (tm & 4) != 0, scrollV = (tm & 8) != 0;
                // y：导出相机下的静止落点（对齐 DrawBackViewport 公式：非滚动轴带视差，整体 floor）
                float worldY = b.Y;
                if (scrollV) { /* t=0 时滚动偏移为 0 */ }
                else worldY += camY * (100 + b.Ry) / 100f;
                worldY = (float)Math.Floor(worldY);
                float screenY = worldY - camY + vh / 2f;

                strips.Add(new BgmapPackWriter.BgmapStrip
                {
                    PartRef = stripAsset.Hash,
                    Y = (short)Math.Clamp((int)Math.Round(screenY), short.MinValue, short.MaxValue),
                    SpeedX = (short)Math.Clamp(scrollH ? b.Rx * 5 : 0, short.MinValue, short.MaxValue),
                    RxParallax = (byte)Math.Clamp(b.Rx, 0, 255),
                    Blend = (byte)(b.Alpha > 0 ? b.Alpha : 255),
                });
            }
            catch (Exception ex)
            {
                summary.Warnings.Add($"地图 {mapId} 条带 {b.Id} 失败: {ex.Message}");
            }
        }

        // 4. BGMAP 主包
        string mapName = "";
        try { mapName = _wz.GetMapName(mapId) ?? ""; } catch { /* 目录服务未预热时名称可缺省 */ }
        var bgmapPayload = BgmapPackWriter.Build(new BgmapPackWriter.BgmapInput
        {
            MapId = mapId,
            Vw = (ushort)vw,
            Vh = (ushort)vh,
            StaticBack = staticBmp,
            TileLayer = tileBmp,
            Strips = strips,
        });

        // 5. 缩略图（96×96，nearest 保持像素风）
        ulong thumbHash = 0;
        try
        {
            using var thumb = new SKBitmap(new SKImageInfo(96, 96, SKColorType.Bgra8888, SKAlphaType.Unpremul));
            using (var c = new SKCanvas(thumb))
            {
                c.Clear(SKColors.Black);
                var rect = FitRect(vw, vh, 96, 96);
                c.DrawBitmap(staticBmp, rect);
            }
            using var img = SKImage.FromBitmap(thumb);
            var pngBytes = img.Encode(SKEncodedImageFormat.Png, 90).ToArray();
            thumbHash = XxHash64.HashToUInt64(pngBytes);
            summary.Assets.Add(new ExportedAsset
            {
                Hash = thumbHash,
                Kind = MpakKind.Thumb,
                Bytes = pngBytes,
                FileName = $"{thumbHash:x16}.png",
                Label = $"缩略图 {mapId}",
                Selector = null,
            });
        }
        catch (Exception ex) { summary.Warnings.Add($"缩略图 {mapId} 失败: {ex.Message}"); }

        var extra = new Dictionary<string, object?> { ["map"] = mapId };
        if (thumbHash != 0) extra["thumb"] = $"{thumbHash:x16}";
        AddAsset(summary, MpakKind.Bgmap, bgmapPayload,
            string.IsNullOrEmpty(mapName) ? $"地图 {mapId}" : mapName, selector: "map", extra: extra);

        // 6. clock_table 建议值（R15：烘焙视口内屏幕坐标；WZ clock 世界锚点经导出相机换算）
        try
        {
            if (hasClock)
            {
                int sx = (int)Math.Round(clockAnchorX - camX + vw / 2f);
                int sy = (int)Math.Round(clockAnchorY - camY + vh / 2f);
                summary.ClockTable[mapId] = new[] { sx, sy };
            }
        }
        catch (Exception ex) { summary.Warnings.Add($"clock_table {mapId} 读取失败: {ex.Message}"); }

        staticBmp.Dispose();
        tileBmp?.Dispose();
    }

    private static SKRect FitRect(int srcW, int srcH, int dstW, int dstH)
    {
        float scale = Math.Min((float)dstW / srcW, (float)dstH / srcH);
        float w = srcW * scale, h = srcH * scale;
        return new SKRect((dstW - w) / 2f, (dstH - h) / 2f, (dstW - w) / 2f + w, (dstH - h) / 2f + h);
    }

    /// <summary>按 period（cx 周期）水平预平铺为「一个完整循环周期宽」的图（规格 §五；固件 offset_x mod 图宽 循环）。</summary>
    private static SKBitmap PreTileHorizontal(SKBitmap src, int period)
    {
        int w = Math.Max(1, period), h = src.Height;
        var dst = new SKBitmap(new SKImageInfo(w, h, SKColorType.Bgra8888, SKAlphaType.Unpremul));
        using var c = new SKCanvas(dst);
        c.Clear(SKColors.Transparent);
        for (int x = 0; x < w + src.Width; x += src.Width)
        {
            c.DrawBitmap(src, x, 0);
            if (x >= w) break;
        }
        return dst;
    }

    /// <summary>MapInfo 浅克隆（只替换 Backs；Layers 等共享引用，渲染只读）。</summary>
    private static MapInfo CloneMap(MapInfo m, List<MapBack> backs) => new()
    {
        Id = m.Id,
        Layers = m.Layers,
        Footholds = m.Footholds,
        Lifes = m.Lifes,
        Portals = m.Portals,
        Ropes = m.Ropes,
        MinX = m.MinX, MinY = m.MinY, MaxX = m.MaxX, MaxY = m.MaxY,
        VRLeft = m.VRLeft, VRTop = m.VRTop, VRRight = m.VRRight, VRBottom = m.VRBottom,
        Backs = backs,
    };

    // ═══════════════════════════════════════════
    // 产物登记
    // ═══════════════════════════════════════════

    private ExportedAsset AddAsset(ExportSummary summary, MpakKind kind, byte[] payload, string label,
        string? selector, Dictionary<string, object?>? extra = null)
    {
        var (hash, file) = Mpak.BuildWithHash(kind, payload);
        var asset = new ExportedAsset
        {
            Hash = hash,
            Kind = kind,
            Bytes = file,
            FileName = Mpak.HashFileName(hash),
            Label = label,
            Selector = selector,
            Extra = extra ?? new Dictionary<string, object?>(),
        };
        summary.Assets.Add(asset);
        return asset;
    }
}

/// <summary>
/// part_id 稳定分配器（规格 §三：导出器分配 body=1, head=2, hair=10.., face=20.., coat=30..；
/// fontTime 保留段 900..912 由 FontPackWriter 固定占用，本分配器不进入该段）。
/// 同一装扮的分配结果确定（类别发现序固定）→ part_id 跨包稳定，支撑未来单件级差量。
/// </summary>
internal sealed class PartIdAllocator
{
    private sealed class Band
    {
        public uint Next;
        public readonly uint End;
        public Band(uint b, uint w) { Next = b; End = b + w; }
    }

    private static readonly (string Cat, uint Base, uint Width)[] BandTable =
    {
        ("body", 1, 1),
        ("head", 2, 1),
        ("hair", 10, 90),      // 10..99：跨动作多 canvas（hairShade 等）
        ("face", 100, 150),    // 100..249：25 表情 × 家族（连续分配）
        ("faceaccessory", 250, 50),
        ("cap", 300, 64), ("cape", 364, 64), ("coat", 428, 64), ("overall", 492, 64),
        ("pants", 556, 64), ("shoe", 620, 64), ("glove", 684, 64), ("shield", 748, 64),
        ("weapon", 812, 64), ("earring", 876, 24),
        ("eyeaccessory", 913, 64), ("mount", 977, 64), ("chair", 1041, 64),
        ("effect", 1105, 64), ("misc", 1169, 512),
    };

    private readonly Dictionary<string, Band> _bands =
        BandTable.ToDictionary(t => t.Cat, t => new Band(t.Base, t.Width), StringComparer.Ordinal);

    private uint _spill = 1700;

    public uint Alloc(string category, int count)
    {
        count = Math.Max(1, count);
        if (!_bands.TryGetValue(category, out var band)) _bands[category] = band = new Band(1700 + (uint)_bands.Count * 512, 512);
        if (band.Next + (uint)count > band.End)
        {
            // 段溢出 → 转入全局溢流区（不与其他段冲突）
            uint id = _spill;
            _spill += (uint)count;
            band.Next = band.End; // 防再次进入本段
            return id;
        }
        uint result = band.Next;
        band.Next += (uint)count;
        return result;
    }
}
