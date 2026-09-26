using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using MinipetServer.Services;
using SkiaSharp;

namespace MiniPet.Export;

/// <summary>
/// kind=4 FONT（字体包）—— 算法规格 §六。
///
/// payload：
/// <code>
/// [u8]  size_px（16/24/32）
/// [u8]  bpp（4）
/// [u32] glyph_count
/// [glyph_count × 12B]: unicode u32 | w u16 | h u16 | advance u8 | off_x i8 | bearing_y i8
/// [... ] 位图数据区（4bpp：每像素 4bit 灰度（渲染 alpha），行 pitch = Align4(ceil(w/2))，高半字节=左像素）
/// </code>
///
/// 字段命名对齐 LVGL lv_font_bin（advance/off_x/bearing_y 语义），但头自成体系（hash 寻址与差量）。
/// 渲染源：宋体（SimSun；macOS 回退 Songti SC / Noto Serif CJK SC，最终回退系统默认）。
/// 三档字号 = 三个包；字符集默认 ASCII 可打印区 + 常用标点，可用 --charset-file 供给 CJK 语料。
///
/// 本文件同时承担 **fontTime 时钟数字素材导出**（§七.5）：WZ Map/Obj/etc.img/clock/fontTime 的
/// 0-9/am/pm/comma 按 PARTS 包导出，part_id 保留段 900..912，origin 用 WzService.GetOrigin
/// （outlink 解析链已内建）；排布参数（锚点 +18+3/+83、AMPM_GAP=12、comma 偶显奇隐）固件硬编码，不进包。
/// </summary>
public static class FontPackWriter
{
    public const int GlyphEntrySize = 12;

    public sealed class GlyphInfo
    {
        public uint Unicode;
        public ushort W;
        public ushort H;
        public byte Advance;
        public sbyte OffX;
        public sbyte BearingY;
        public required byte[] Bitmap4Bpp;
    }

    public static byte[] Build(byte sizePx, IReadOnlyList<GlyphInfo> glyphs)
    {
        var ms = new MemoryStream(4096);
        using (var w = new BinaryWriter(ms, Encoding.UTF8, leaveOpen: true))
        {
            w.Write(sizePx);
            w.Write((byte)4);
            w.Write((ushort)0);   // 头部 8B 对齐填充（与固件 parse_font 一致）
            w.Write((uint)glyphs.Count);
            // 位图数据区紧随索引（无显式偏移：glyph 尺寸可推算 4bpp 行对齐长度）
            var blobs = new List<byte[]>(glyphs.Count);
            foreach (var g in glyphs) blobs.Add(g.Bitmap4Bpp);
            for (int i = 0; i < glyphs.Count; i++)
            {
                var g = glyphs[i];
                w.Write(g.Unicode);
                w.Write(g.W);
                w.Write(g.H);
                w.Write(g.Advance);
                w.Write(g.OffX);
                w.Write(g.BearingY);
                w.Write((byte)0);   // glyph 12B 尾填充（与固件一致）
            }
            foreach (var b in blobs) w.Write(b);
        }
        return ms.ToArray();
    }

    // ═══════════════════════════════════════════
    // 字形渲染（SkiaSharp 光栅化 → 4bpp）
    // ═══════════════════════════════════════════

    /// <summary>宋体候选链（Windows → macOS → Linux → 系统默认）。</summary>
    private static readonly string[] SongCandidates =
        { "SimSun", "宋体", "Songti SC", "STSong", "Noto Serif CJK SC", "Noto Serif SC", "Microsoft YaHei" };

    /// <summary>默认字符集：ASCII 可打印 + 常用中英文标点（CJK 语料经 charsetFile 补充）。</summary>
    public static string DefaultCharset()
    {
        var sb = new StringBuilder();
        for (int c = 0x20; c <= 0x7E; c++) sb.Append((char)c);
        sb.Append("，。、：；！？（）【】《》“”‘’—…·℃");
        return sb.ToString();
    }

    /// <summary>
    /// 选字面：显式 family 直接用；否则沿宋体候选链按 FamilyName 精确命中。
    /// ⚠️ FromFamilyName 未命中时 Skia 返回**共享的默认字面包装**（macOS 实测同名复用）——
    /// 对其 Dispose 会毒化全局缓存，后续再取即 AccessViolation。故此处一律不 Dispose，
    /// 生命周期交给进程（导出器短驻；字形渲染一次即弃）。
    /// </summary>
    private static SKTypeface? PickTypeface(string? fontFamily)
    {
        if (!string.IsNullOrEmpty(fontFamily))
        {
            var explicitTf = SKTypeface.FromFamilyName(fontFamily);
            if (explicitTf != null && explicitTf.FamilyName == fontFamily) return explicitTf;
            return null; // 显式指定未命中 → 走候选链
        }
        foreach (var name in SongCandidates)
        {
            var tf = SKTypeface.FromFamilyName(name);
            if (tf != null && tf.FamilyName == name) return tf;
        }
        return null;
    }

    /// <summary>
    /// 渲染一档字号的 FONT payload。advance/off_x/bearing_y 以「墨迹包围盒」实测
    /// （离屏光栅化后扫描非零 alpha 像素），bearing_y = 墨迹顶到基线的距离（向上为负，对齐 FreeType惯例）。
    /// </summary>
    public static byte[] RenderFontPack(int sizePx, string charset, string? fontFamily = null)
    {
        var glyphs = new List<GlyphInfo>();
        var tf = PickTypeface(fontFamily);
        using (var font = new SKFont(tf ?? SKTypeface.Default, sizePx))
        {
            int pad = Math.Max(2, sizePx / 4);
            int canvasW = (int)Math.Ceiling(font.MeasureText("龘")) + pad * 4;
            int canvasH = sizePx * 2 + pad * 4;
            using var scratch = new SKBitmap(new SKImageInfo(Math.Max(canvasW, 8), Math.Max(canvasH, 8), SKColorType.Bgra8888, SKAlphaType.Unpremul));
            foreach (var ch in new HashSet<char>(charset))
            {
                if (char.IsSurrogate(ch)) continue; // BMP 之外暂不支持（4bpp 单字形结构可扩展）
                string s = ch.ToString();
                float adv = font.MeasureText(s);
                if (adv <= 0 && !char.IsWhiteSpace(ch)) continue;

                using (var c = new SKCanvas(scratch))
                {
                    c.Clear(SKColors.Transparent);
                    using var paint = new SKPaint { Color = SKColors.White, IsAntialias = true };
                    c.DrawText(s, pad, sizePx + pad, font, paint); // 基线 y = sizePx + pad
                }

                // 扫描墨迹包围盒（空格等无墨字符 → 空白字形，防 bbox 反转溢出）
                int minX = int.MaxValue, minY = int.MaxValue, maxX = -1, maxY = -1;
                unsafe
                {
                    byte* p = (byte*)scratch.GetPixels();
                    int stride = scratch.RowBytes;
                    for (int y = 0; y < scratch.Height; y++)
                    {
                        byte* row = p + (long)y * stride;
                        for (int x = 0; x < scratch.Width; x++)
                        {
                            if (row[x * 4 + 3] > 0)
                            {
                                if (x < minX) minX = x;
                                if (x > maxX) maxX = x;
                                if (y < minY) minY = y;
                                if (y > maxY) maxY = y;
                            }
                        }
                    }
                }

                if (maxX < 0)
                {
                    // 空白字形：1×1 全零位图（advance 保留，字符间空隙由 advance 决定）
                    glyphs.Add(new GlyphInfo
                    {
                        Unicode = ch,
                        W = 1,
                        H = 1,
                        Advance = (byte)Clamp(Math.Max(1, (int)Math.Round(adv)), 1, 255),
                        OffX = 0,
                        BearingY = 0,
                        Bitmap4Bpp = new byte[4],
                    });
                    continue;
                }

                int gw = maxX - minX + 1;
                int gh = maxY - minY + 1;
                // 4bpp：高半字节 = 偶数列像素，行 pitch = Align4(ceil(w/2))
                int pitch = PartPackWriter.Align4((gw + 1) / 2);
                var bmp4 = new byte[pitch * gh];
                unsafe
                {
                    byte* p = (byte*)scratch.GetPixels();
                    int stride = scratch.RowBytes;
                    for (int y = 0; y < gh; y++)
                    {
                        byte* row = p + (long)(minY + y) * stride;
                        for (int x = 0; x < gw; x++)
                        {
                            int a = row[(minX + x) * 4 + 3] >> 4;
                            int di = y * pitch + (x >> 1);
                            if ((x & 1) == 0) bmp4[di] |= (byte)(a << 4);
                            else bmp4[di] |= (byte)a;
                        }
                    }
                }

                glyphs.Add(new GlyphInfo
                {
                    Unicode = ch,
                    W = (ushort)gw,
                    H = (ushort)gh,
                    Advance = (byte)Clamp(Math.Max(1, (int)Math.Round(adv)), 1, 255),
                    OffX = (sbyte)Clamp(minX - pad, -128, 127),
                    BearingY = (sbyte)Clamp(minY - (sizePx + pad), -128, 127),
                    Bitmap4Bpp = bmp4,
                });
            }
            glyphs.Sort((a, b) => a.Unicode.CompareTo(b.Unicode));
        }
        return Build((byte)sizePx, glyphs);
    }

    private static int Clamp(int v, int lo, int hi) => v < lo ? lo : v > hi ? hi : v;

    // ═══════════════════════════════════════════
    // fontTime 时钟数字（§七.5，PARTS 保留段 900..912）
    // ═══════════════════════════════════════════

    /// <summary>fontTime 部件顺序 → part_id 900 起的保留段映射。</summary>
    public static readonly (string Name, uint PartId)[] FontTimeParts =
    {
        ("0", 900), ("1", 901), ("2", 902), ("3", 903), ("4", 904),
        ("5", 905), ("6", 906), ("7", 907), ("8", 908), ("9", 909),
        ("am", 910), ("pm", 911), ("comma", 912),
    };

    /// <summary>
    /// 导出 fontTime 全套为 PARTS payload。素材为 outlink 形式（源节点 1×1 占位 + _outlink 指向 _Canvas），
    /// ExtractPng/GetOrigin 已含解析链，直接调用；origin 实测全部 (0,0)。
    /// </summary>
    public static byte[] ExportFontTimeParts(WzService wz)
    {
        var entries = new List<PartPackWriter.PartEntry>();
        foreach (var (name, partId) in FontTimeParts)
        {
            string path = $"Map/Obj/etc.img/clock/fontTime/{name}";
            var png = wz.ExtractPng(path);
            if (png == null)
            {
                Console.Error.WriteLine($"[FontPackWriter] fontTime 缺素材: {path}");
                continue;
            }
            using var bmp = SKBitmap.Decode(png);
            if (bmp == null) continue;
            var (ox, oy) = wz.GetOrigin(path);
            entries.Add(new PartPackWriter.PartEntry
            {
                PartId = partId,
                ExprGroup = 0,
                Bitmap = bmp.Copy(),
                OriginX = ox,
                OriginY = oy,
            });
        }
        if (entries.Count == 0) throw new InvalidOperationException("fontTime 全部素材缺失（WZ 数据异常）");
        return PartPackWriter.Build(entries);
    }
}
