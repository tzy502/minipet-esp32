using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using SkiaSharp;

namespace MiniPet.Export;

/// <summary>
/// kind=1 PARTS（部件图包）—— 算法规格 §三。
///
/// payload：
/// <code>
/// [u32] part_count
/// [part_count × 20B] 索引：part_id u32 | expr_group u16 | w u16 | h u16 | origin_x i16 | origin_y i16 | offset u32
///                     offset = 位图数据区内该图的显式偏移（payload 起算，指向数据区）
/// [... ] 位图数据区（连续；每图 = RGB565 像素（行对齐 4B）+ 1bit alpha 掩码）
/// </code>
///
/// 每个部件的位图记录为「RGBA5650 变体」的确定性布局（设备端零分支顺序读）：
/// <code>
/// [w × h × 2B] RGB565 像素，行 pitch = Align4(w*2)，行尾零填充
/// [mask]       1bit alpha 掩码（alpha ≥ 128 → 1），w*h bit 连续按行打包（MSB 在前），
///              字节数 = ceil(w*h/8)，区域尾部补零到 4B 对齐
/// </code>
/// 无 alpha 的部件（如视口快照）掩码为全 1 —— 布局对任意 w/h 均可由索引推算，无需存在标志位。
/// 掩码/行距推算公式（回放与固件同源）：
/// pitch = (w*2 + 3) &amp; ~3；maskBytes = ((w*h + 7) &gt;&gt; 3 + 3) &amp; ~3；recordSize = pitch*h + maskBytes。
///
/// expr_group：face 类部件的表情变体组号 —— 同组 25 个变体共享 group、part_id 连续
/// （组内第 i 个 id = 组基准 + expression 列表偏移 i）；非 face 件 group=0。
/// </summary>
public static class PartPackWriter
{
    /// <summary>
    /// 索引条目字节数：字段 u32+u16+u16+u16+i16+i16+u32 = 18B 顺序紧排。
    /// （规格 §三 标注「20B」但字段和为 18B——按字段实长 18B 定稿，与回放读取器
    /// SpecMpakParser 的顺序读取一致；若未来需要 20B 对齐再以 flag 位扩展。）
    /// </summary>
    public const int IndexEntrySize = 18;

    /// <summary>一个部件（位图 + 元数据）。调用方负责处置 Bitmap。</summary>
    public sealed class PartEntry
    {
        public uint PartId;
        public ushort ExprGroup;
        public SKBitmap Bitmap = null!;
        public int OriginX;
        public int OriginY;
        /// <summary>同一位图数据可被多个 part_id 引用（表情变体缺帧回退 default 时复用偏移）；null = 自身数据。</summary>
        public string? ShareBitmapOf;
    }

    /// <summary>编码后的部件记录（供 offset 显式寻址与跨包复用）。</summary>
    public sealed class EncodedPart
    {
        public required byte[] Data;
        public int W;
        public int H;
    }

    /// <summary>SKBitmap(BGRA8888) → RGB565 + 1bit 掩码记录（纯函数，无进程级缓存）。</summary>
    public static EncodedPart Encode(SKBitmap bmp)
    {
        int w = bmp.Width, h = bmp.Height;
        return new EncodedPart { W = w, H = h, Data = EncodeRgb565WithMask(bmp) };
    }

    /// <summary>编码核心：BGRA8888（或 RGBA8888）位图 → [RGB565 行对齐 4B][1bit mask 补 4B]。</summary>
    public static byte[] EncodeRgb565WithMask(SKBitmap bmp)
        => EncodeCore(bmp, withMask: true);

    /// <summary>纯 RGB565（不透明图层用：BGMAP static_back；无掩码尾，行对齐 4B）。</summary>
    public static byte[] EncodeRgb565(SKBitmap bmp)
        => EncodeCore(bmp, withMask: false);

    private static byte[] EncodeCore(SKBitmap bmp, bool withMask)
    {
        int w = bmp.Width, h = bmp.Height;
        if (w <= 0 || h <= 0) return Array.Empty<byte>();

        // 统一到可逐像素读取的格式（ExtractPng→Decode 的结果为 BGRA8888；此处兼容两种 N32）
        SKBitmap src = bmp;
        SKBitmap? normalized = null;
        if (bmp.ColorType is not (SKColorType.Bgra8888 or SKColorType.Rgba8888))
        {
            normalized = new SKBitmap(new SKImageInfo(w, h, SKColorType.Bgra8888, SKAlphaType.Unpremul));
            using (var c = new SKCanvas(normalized)) { c.DrawBitmap(bmp, 0, 0); }
            src = normalized;
        }

        int pitch = Align4(w * 2);
        int pixelBytes = pitch * h;
        int maskBytes = withMask ? Align4((w * h + 7) >> 3) : 0;
        var outBuf = new byte[pixelBytes + maskBytes];

        bool bgra = src.ColorType == SKColorType.Bgra8888;
        try
        {
            unsafe
            {
                byte* basePtr = (byte*)src.GetPixels();
                int stride = src.RowBytes;
                for (int y = 0; y < h; y++)
                {
                    byte* row = basePtr + (long)y * stride;
                    int rowOut = y * pitch;
                    int bitBase = y * w; // 掩码按行连续打包（w*h bit 总体连续，此处等价逐像素序）
                    for (int x = 0; x < w; x++)
                    {
                        byte b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2], a = row[x * 4 + 3];
                        if (bgra) { /* 已按 BGRA 取 */ }
                        else { (r, b) = (b, r); } // RGBA8888 → 交换 R/B
                        int a8 = a;
                        if (a8 != 0 && a8 != 255 && src.AlphaType == SKAlphaType.Premul)
                        {
                            // 1bit 掩码下半透明像素将变为不透明：反预乘恢复原色，避免边缘发暗
                            r = (byte)Math.Min(255, r * 255 / a8);
                            g = (byte)Math.Min(255, g * 255 / a8);
                            b = (byte)Math.Min(255, b * 255 / a8);
                        }
                        ushort rgb565 = (ushort)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                        outBuf[rowOut + x * 2] = (byte)(rgb565 & 0xFF);
                        outBuf[rowOut + x * 2 + 1] = (byte)(rgb565 >> 8);
                        if (withMask && a >= 128)
                        {
                            int bit = bitBase + x;
                            outBuf[pixelBytes + (bit >> 3)] |= (byte)(0x80 >> (bit & 7));
                        }
                    }
                }
            }
        }
        finally
        {
            normalized?.Dispose();
        }
        return outBuf;
    }

    internal static int Align4(int v) => (v + 3) & ~3;

    /// <summary>
    /// 组装 PARTS payload。条目顺序即写入顺序；offset 显式寻址（不复用数据的条目按顺序追加，
    /// ShareBitmapOf 命中先前条目的位图时复用其 offset）。
    /// </summary>
    public static byte[] Build(IReadOnlyList<PartEntry> parts)
    {
        if (parts == null || parts.Count == 0) throw new ArgumentException("PARTS 包至少需要一个部件", nameof(parts));

        // 先编码全部（显式 offset 需要先知道长度）
        var starts = new int[parts.Count];
        var sizes = new (int W, int H)[parts.Count];
        var pathToOffset = new Dictionary<string, (int Off, int W, int H)>(StringComparer.Ordinal); // ShareBitmapOf → (数据区 offset, 尺寸)
        var blobs = new List<byte[]>();
        int dataLen = 0;
        for (int i = 0; i < parts.Count; i++)
        {
            var p = parts[i];
            if (p.ShareBitmapOf != null && pathToOffset.TryGetValue(p.ShareBitmapOf, out var shared))
            {
                starts[i] = shared.Off;
                sizes[i] = (shared.W, shared.H);
                continue;
            }
            var enc = Encode(p.Bitmap);
            starts[i] = dataLen;
            sizes[i] = (enc.W, enc.H);
            blobs.Add(enc.Data);
            dataLen += enc.Data.Length;
            if (p.ShareBitmapOf != null) pathToOffset[p.ShareBitmapOf] = (starts[i], enc.W, enc.H);
        }

        int indexLen = 4 + parts.Count * IndexEntrySize;
        var ms = new MemoryStream(indexLen + dataLen);
        var w = new BinaryWriter(ms, System.Text.Encoding.UTF8, leaveOpen: true);
        w.Write((uint)parts.Count);
        for (int i = 0; i < parts.Count; i++)
        {
            var p = parts[i];
            int wpx = sizes[i].W;
            int hpx = sizes[i].H;
            w.Write(p.PartId);
            w.Write(p.ExprGroup);
            w.Write((ushort)wpx);
            w.Write((ushort)hpx);
            w.Write((short)p.OriginX);
            w.Write((short)p.OriginY);
            w.Write((uint)(indexLen + starts[i])); // payload 起算的数据区绝对偏移
        }
        foreach (var blob in blobs) w.Write(blob);
        w.Flush();
        return ms.ToArray();
    }
}
