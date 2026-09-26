using System.IO;
using System.IO.Hashing;
using System.Linq;
using System.Text;
using System.Text.Json;
using MinipetServer.Config;
using MinipetServer.Device;
using MinipetServer.Models;
using MinipetServer.Services;
using SkiaSharp;

namespace MinipetServer.Api;

/// <summary>
/// 缩略图服务（E4/E7 → docs/ai/web-paperdoll-alignment.md §五.服务端2）：真实 WZ 渲染 + data/cache/thumbs/ 磁盘缓存。
/// - type=part：部件图标（icon 候选序：发型 default/hair、脸型 blink/0/face、其他 info/icon →
///   回退 {root}/icon、{root}/iconRaw → stand 首帧 stand1/0、stand/0）→ 等比缩放进 64×64 透明画布；
/// - type=paperdoll：拼串覆盖 seed/default-appearance.json 基底 → PaperdollService 真实合成
///   stand1 首帧（无帧试 stand）→ 等比缩放进 size×size 透明画布（size 64/128/192/256，默认 192）；
/// - 其他 type（mob/npc/…）：M3 确定性纯色占位（行为不回归）。
/// 所有失败路径（WZ 未加载 / 渲染异常 / 空帧）回落 RenderPlaceholder 色块，不 500；
/// part/paperdoll 的失败占位不落盘（WZ 稍后加载成功可重试出真图）。
/// </summary>
public sealed class ThumbService
{
    public const int Size = 64;
    public const int DefaultPaperdollSize = 192;
    private static readonly int[] PaperdollSizes = { 64, 128, 192, 256 };

    private readonly ServerPaths _paths;
    private readonly WzService _wz;
    private readonly CacheManager _cache;

    // PaperdollService 专用渲染实例 + 串行锁：SetAppearance 换 _current/清缓存的整套流程
    // 非并发安全（多请求并发复用同实例会外观串台/字典损坏），所有整套合成在此排队；
    // 磁盘缓存命中在锁外直接返回，不排队。渲染后 ClearBitmapCache 防 WZ 位图缓存无界增长。
    private readonly object _paperdollLock = new();
    private PaperdollService? _paperdoll;

    // 基底外观 seed（神子）原文：首次 paperdoll 请求读一次盘，之后复用字符串；
    // 每请求反序列化出独立对象再覆盖（避免共享实例被并发改写）。
    private readonly object _seedLock = new();
    private string? _seedJson;
    private bool _seedLoaded;

    // seed 字段 camelCase ↔ CharacterAppearance PascalCase（抄 AssetExporter.LoadAppearance 的选项）
    private static readonly JsonSerializerOptions SeedJsonOpts = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    public ThumbService(ServerPaths paths, WzService wz, CacheManager cache)
    {
        _paths = paths;
        _wz = wz;
        _cache = cache;
    }

    /// <summary>
    /// 取缩略图 PNG。type=part 用 folder/img 定位部件；type=paperdoll 用 id 拼串 + size；
    /// 旧 type（mob/npc/preset…）只看 type/id（M3 占位行为保持不变）。
    /// </summary>
    public byte[] GetOrCreatePng(string type, string id, string? folder = null, string? img = null, int? size = null)
    {
        if (type == "part") return GetOrCreatePartPng(folder ?? "", id, img ?? "");
        if (type == "paperdoll") return GetOrCreatePaperdollPng(id, size ?? DefaultPaperdollSize);

        // 旧类型：M3 确定性纯色占位（缓存机制不变：有缓存文件直接回，无则渲染+落盘）
        var file = Path.Combine(_paths.ThumbsDir, $"t_{StorageUtil.SafeFileId(type)}.{StorageUtil.SafeFileId(id)}.{Size}.png");
        if (File.Exists(file)) return File.ReadAllBytes(file);
        var png = RenderPlaceholder(type, id);
        StorageUtil.AtomicWriteAllBytes(file, png);
        return png;
    }

    // ═══════════════════════════════════════════
    // type=part：WZ 部件图标
    // ═══════════════════════════════════════════

    private byte[] GetOrCreatePartPng(string folder, string id, string img)
    {
        // 文件名含 folder/id/img 防碰撞（img 形如 "03010.img"，'.' 走 SafeFileId 不会出非法字符）
        var segs = new[] { "t_part", StorageUtil.SafeFileId(folder), StorageUtil.SafeFileId(id) }.ToList();
        if (!string.IsNullOrEmpty(img)) segs.Add(StorageUtil.SafeFileId(img));
        segs.Add(Size.ToString());
        var file = Path.Combine(_paths.ThumbsDir, string.Join(".", segs) + ".png");
        if (File.Exists(file)) return File.ReadAllBytes(file);

        var png = TryRenderPart(folder, id, img);
        if (png != null)
        {
            StorageUtil.AtomicWriteAllBytes(file, png);
            return png;
        }
        // 失败不落盘：WZ 未加载/无效 id 时占位钉死会导致 WZ 就绪后仍出旧色块，下次请求可重试
        return RenderPlaceholder("part", $"{folder}/{id}");
    }

    /// <summary>
    /// 部件 WZ 根路径：普通部件 Character/{folder}/{id 8 位补零}.img（folder 为 WZ 目录名）；
    /// 椅子在 Item/Install 容器 img（形如 03010.img，由 query 传入）下；皮肤（bodyId 2000-2999）root 即 Character/{id}.img。
    /// 对齐桌面 MaterialBrowserWindow.CategoryWzRoot。
    /// </summary>
    private static string PartRoot(string folder, string id, string img)
    {
        if (folder.Equals("Install", StringComparison.OrdinalIgnoreCase)) return $"Item/Install/{img}/{id.PadLeft(8, '0')}";
        // 皮肤 body img 同为 8 位补零（实测 Character/00002000.img），stand 帧多一层 body（stand1/0/body）
        if (folder.Equals("Body", StringComparison.OrdinalIgnoreCase)) return $"Character/{id.PadLeft(8, '0')}.img";
        return $"Character/{folder}/{id.PadLeft(8, '0')}.img";
    }

    /// <summary>icon 候选序首个提取点：发型 default/hair、脸型 blink/0/face、其余 info/icon（真实 WZ 实测有效）。</summary>
    private static string PrimaryIcon(string folder, string root)
    {
        if (folder.Equals("Hair", StringComparison.OrdinalIgnoreCase)) return $"{root}/default/hair";
        if (folder.Equals("Face", StringComparison.OrdinalIgnoreCase)) return $"{root}/blink/0/face";
        return $"{root}/info/icon";
    }

    private byte[]? TryRenderPart(string folder, string id, string img)
    {
        if (!_wz.IsLoaded) return null;
        if (string.IsNullOrWhiteSpace(folder) || string.IsNullOrWhiteSpace(id)) return null;
        var root = PartRoot(folder, id, img);
        var candidates = new List<string>
        {
            PrimaryIcon(folder, root),
            $"{root}/icon",       // 通用回退
            $"{root}/iconRaw",
            $"{root}/stand1/0",   // icon 全失 → 部件 stand 首帧（皮肤等无 info/icon 的靠这里）
            $"{root}/stand/0",
        };
        // 皮肤身体帧多一层 body（实测 Character/00002000.img/stand1/0/body = 440B 命中）
        if (folder.Equals("Body", StringComparison.OrdinalIgnoreCase))
        {
            candidates.Add($"{root}/stand1/0/body");
            candidates.Add($"{root}/stand/0/body");
        }
        foreach (var path in candidates)
        {
            byte[]? raw;
            try { raw = _wz.ExtractPng(path); }
            catch { continue; }
            if (raw == null || raw.Length == 0) continue;
            using var src = SKBitmap.Decode(raw);
            var png = FitToCanvas(src, Size);
            if (png != null) return png;
        }
        return null;
    }

    // ═══════════════════════════════════════════
    // type=paperdoll：整套真实合成
    // ═══════════════════════════════════════════

    private byte[] GetOrCreatePaperdollPng(string payload, int requestedSize)
    {
        var size = NormalizePaperdollSize(requestedSize);
        // 拼串太长不能直接做文件名：XxHash64(id串+size)（WzService 侧已用同款哈希）
        var hash = XxHash64.HashToUInt64(Encoding.UTF8.GetBytes(payload + "#" + size)).ToString("x16");
        var file = Path.Combine(_paths.ThumbsDir, $"t_paperdoll.{hash}.{size}.png");
        if (File.Exists(file)) return File.ReadAllBytes(file);

        var png = TryRenderPaperdoll(payload, size);
        if (png != null)
        {
            StorageUtil.AtomicWriteAllBytes(file, png);
            return png;
        }
        return RenderPlaceholder("paperdoll", payload); // 失败不落盘（WZ 稍后就绪可重试）
    }

    private static int NormalizePaperdollSize(int size)
        => PaperdollSizes.Contains(size) ? size : DefaultPaperdollSize;

    private byte[]? TryRenderPaperdoll(string payload, int size)
    {
        if (!_wz.IsLoaded) return null;
        var appearance = BuildAppearance(payload);
        if (appearance == null) return null;
        try
        {
            lock (_paperdollLock) // 并发红线：PaperdollService 单实例串行渲染（见类头注释）
            {
                _paperdoll ??= new PaperdollService(_wz, _cache, new SpriteService(_cache));
                _paperdoll.SetAppearance(appearance);
                var (frame, _, _, _, _) = _paperdoll.RenderFrame("stand1", 0);
                if (frame == null) frame = _paperdoll.RenderFrame("stand", 0).Frame;
                var png = FitToCanvas(frame, size);
                frame?.Dispose();
                _paperdoll.ClearBitmapCache(); // 防 WZ 位图缓存随外观数无界增长（磁盘缓存命中后不再渲染）
                return png;
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Thumb] paperdoll 渲染失败（size={size}）: {ex.Message}");
            return null;
        }
    }

    /// <summary>
    /// 拼串 → CharacterAppearance：基底 = seed/default-appearance.json（神子），
    /// 拼串出现的键覆盖基底（含显式 "-" = 该槽不穿 → null），未出现的键保持基底。
    /// 格式：k:v 以 '|' 连接；头三个键 g（gender 0/1）/ear（humanEar|ear|lefEar|highlefEar）/body（bodyId），
    /// 后跟 16 个槽位键 hair face cap cape coat overall pants shoes weapon shield glove
    /// faceAccessory eyeAccessory earring mount chair（全槽显式，"-"=null）。
    /// </summary>
    private CharacterAppearance? BuildAppearance(string payload)
    {
        var seed = LoadSeedJson();
        CharacterAppearance a;
        try
        {
            a = seed == null
                ? new CharacterAppearance()
                : JsonSerializer.Deserialize<CharacterAppearance>(seed, SeedJsonOpts) ?? new CharacterAppearance();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Thumb] 基底外观解析失败，用空外观兜底: {ex.Message}");
            a = new CharacterAppearance();
        }

        foreach (var seg in (payload ?? "").Split('|'))
        {
            var kv = seg.Split(':', 2);
            if (kv.Length != 2) continue;
            var key = kv[0].Trim();
            var val = kv[1].Trim();
            if (key.Length == 0 || val.Length == 0) continue;
            switch (key)
            {
                case "g": if (val == "0" || val == "1") a.Gender = int.Parse(val); break;
                case "ear": if (PaperdollService.AllEarTypes.Contains(val)) a.Ear = val; break;
                case "body": if (int.TryParse(val, out var body) && body > 0) a.BodyId = body; break;
                case "hair": a.Hair = SlotItem(val); break; // 模型非空槽："-" = 空 ItemInfo（不穿）
                case "face": a.Face = SlotItem(val); break;
                case "cap": a.Cap = SlotItemOpt(val); break;
                case "cape": a.Cape = SlotItemOpt(val); break;
                case "coat": a.Coat = SlotItemOpt(val); break;
                case "overall": a.Overall = SlotItemOpt(val); break;
                case "pants": a.Pants = SlotItemOpt(val); break;
                case "shoes": a.Shoes = SlotItemOpt(val); break;
                case "weapon": a.Weapon = SlotItemOpt(val); break;
                case "shield": a.Shield = SlotItemOpt(val); break;
                case "glove": a.Glove = SlotItemOpt(val); break;
                case "faceAccessory": a.FaceAccessory = SlotItemOpt(val); break;
                case "eyeAccessory": a.EyeAccessory = SlotItemOpt(val); break;
                case "earring": a.Earring = SlotItemOpt(val); break;
                case "mount": a.Mount = SlotOpt(val, v => new MountInfo { Id = v }); break;
                case "chair": a.Chair = SlotOpt(val, v => new ChairInfo { Id = v }); break;
                // 未知键忽略（前向兼容）
            }
        }
        return a;
    }

    private static ItemInfo SlotItem(string v) => v == "-" ? new ItemInfo() : new ItemInfo { Id = v };
    private static ItemInfo? SlotItemOpt(string v) => v == "-" ? null : new ItemInfo { Id = v };
    private static T? SlotOpt<T>(string v, Func<string, T> make) where T : class => v == "-" ? null : make(v);

    /// <summary>seed 原文（读盘至多一次，进程内缓存；找不到返回 null → 空外观兜底）。</summary>
    private string? LoadSeedJson()
    {
        if (_seedLoaded) return _seedJson;
        lock (_seedLock)
        {
            if (!_seedLoaded)
            {
                _seedLoaded = true;
                try
                {
                    var path = Path.Combine(MiniPet.Export.DeviceProfile.FindSeedRoot(), "default-appearance.json");
                    if (File.Exists(path)) _seedJson = File.ReadAllText(path);
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine($"[Thumb] 读取基底外观 seed 失败: {ex.Message}");
                }
            }
            return _seedJson;
        }
    }

    // ═══════════════════════════════════════════
    // 通用渲染
    // ═══════════════════════════════════════════

    /// <summary>等比缩放进 size×size 透明画布居中（保持 alpha；src 空/异常返回 null）。
    /// 高质量重采样走 ScalePixels（SkiaSharp 3.x DrawBitmap 无 sampling 重载），再 1:1 贴到居中位置。</summary>
    private static byte[]? FitToCanvas(SKBitmap? src, int canvasSize)
    {
        if (src == null || src.Width <= 0 || src.Height <= 0) return null;
        try
        {
            var scale = Math.Min(canvasSize / (float)src.Width, canvasSize / (float)src.Height);
            var w = Math.Max(1, (int)Math.Round(src.Width * scale));
            var h = Math.Max(1, (int)Math.Round(src.Height * scale));
            using var dst = new SKBitmap(canvasSize, canvasSize, SKColorType.Bgra8888, SKAlphaType.Premul);
            using var canvas = new SKCanvas(dst);
            canvas.Clear(SKColors.Transparent);
            if (w == src.Width && h == src.Height)
            {
                canvas.DrawBitmap(src, new SKPoint((canvasSize - w) / 2f, (canvasSize - h) / 2f));
            }
            else
            {
                using var scaled = new SKBitmap(w, h, SKColorType.Bgra8888, SKAlphaType.Premul);
                src.ScalePixels(scaled, new SKSamplingOptions(SKCubicResampler.Mitchell));
                canvas.DrawBitmap(scaled, new SKPoint((canvasSize - w) / 2f, (canvasSize - h) / 2f));
            }
            using var image = SKImage.FromBitmap(dst);
            using var data = image.Encode();
            return data.ToArray();
        }
        catch { return null; }
    }

    /// <summary>
    /// 占位渲染：色相 = XxHash64(type/id) % 360（同图同色，跨进程稳定），下沿色带 + 标签两字符。
    /// </summary>
    private static byte[] RenderPlaceholder(string type, string id)
    {
        using var bmp = new SKBitmap(Size, Size);
        var info = new SKImageInfo(Size, Size, SKColorType.Bgra8888, SKAlphaType.Opaque);
        using var surface = SKSurface.Create(info, bmp.GetPixels(), Size * 4);
        var canvas = surface.Canvas;

        var hash = XxHash64.HashToUInt64(Encoding.UTF8.GetBytes(type + "/" + id));
        var hue = hash % 360;
        canvas.Clear(SKColor.FromHsv(hue, 40, 78));

        using (var bar = new SKPaint { Color = SKColor.FromHsv(hue, 40, 48) })
        {
            canvas.DrawRect(0, Size - 20, Size, 20, bar);
        }

        var label = LabelOf(type, id);
        using var font = new SKFont(SKTypeface.Default, 14);
        using var text = new SKPaint { Color = SKColors.White, IsAntialias = true };
        var width = font.MeasureText(label, text);
        canvas.DrawText(label, (Size - width) / 2f, Size - 6, font, text);

        using var image = SKImage.FromBitmap(bmp);
        using var data = image.Encode();
        return data.ToArray();
    }

    private static string LabelOf(string type, string id)
    {
        var t = string.IsNullOrWhiteSpace(type) ? "?" : type.Trim();
        // map → "M", paperdoll → "P", npc → "N", preset → "S"？取类型首字母大写 + id 首段首字符
        var c1 = char.ToUpperInvariant(t[0]);
        var raw = id.Trim();
        var c2 = raw.Length > 0 ? char.ToUpperInvariant(raw[0]) : '*';
        return $"{c1}{c2}";
    }
}
