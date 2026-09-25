using System.IO;
using System.Text;
using MinipetServer.Config;
using MinipetServer.Device;
using SkiaSharp;

namespace MinipetServer.Api;

/// <summary>
/// 缩略图服务（E4/E7）：64×64 PNG，磁盘缓存 data/cache/thumbs/。
/// M3 阶段先出「确定性纯色占位」（色相由 type/id hash 决定 + 角标文字）；
/// M4 素材浏览器接入后替换 RenderPlaceholder 为真实渲染（SkiaSharp 已就位），
/// 缓存机制不变：有缓存文件直接回，无则渲染+落盘。
/// </summary>
public sealed class ThumbService
{
    public const int Size = 64;

    private readonly ServerPaths _paths;

    public ThumbService(ServerPaths paths) => _paths = paths;

    public byte[] GetOrCreatePng(string type, string id)
    {
        var file = Path.Combine(_paths.ThumbsDir, $"t_{StorageUtil.SafeFileId(type)}.{StorageUtil.SafeFileId(id)}.{Size}.png");
        if (File.Exists(file)) return File.ReadAllBytes(file);
        var png = RenderPlaceholder(type, id);
        StorageUtil.AtomicWriteAllBytes(file, png);
        return png;
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

        var hash = System.IO.Hashing.XxHash64.HashToUInt64(Encoding.UTF8.GetBytes(type + "/" + id));
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
