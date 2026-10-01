using System;

namespace MiniPet.Export;

/// <summary>
/// 整图 BGMAP「128×128 世界像素瓦片」几何（**冻结契约** <c>docs/ai/map-tiled-format-contract.md</c> §2~§4 的
/// 服务端唯一实现点）。固件侧对应读取口径见契约 §6。
///
/// <code>
/// TILE = 128（世界像素）
/// gx = ceil(lw / TILE), gy = ceil(lh / TILE)        // lw/lh = 该层自身宽高
/// idx = ty * gx + tx                                 // 行主序
/// 像素区：每块固定 TILE*TILE*2 = 32768 B（RGB565 小端），右/下越界补 0
///         块内行主序、每行 TILE*2 = 256 B，无额外 padding
///         ⇒ 层像素区长度 = gx*gy*32768（**不是** lw*lh*2）
/// 掩码区：同网格，每块固定 TILE*TILE/8 = 2048 B，1bit/px，**行按字节对齐**
///         块内第 y 行占 [y*16, y*16+16)，位序 MSB-first（x=0 在最高位）
///         ⇒ 层掩码区长度 = gx*gy*2048（tile 层紧跟像素区之后，无额外补齐）
/// </code>
///
/// 三类层各自的 <c>lw × lh</c>：static / tile = <c>vw × vh</c>（整图世界尺寸）；
/// 条带 = 该条带**自身**源宽高（条带像素区在独立 PARTS 包里，按其 PARTS 索引里的 w/h 分块）。
/// 条带的滚动 / 周期平铺 / 世界对齐是运行时行为，与存储布局无关（契约 §4）。
/// </summary>
public static class TiledLayout
{
    /// <summary>瓦片边长（世界像素）。契约 §2：服务端只支持 128；固件按包内几何读，不硬编码。</summary>
    public const int TileSize = 128;

    /// <summary>单块像素字节数 = TILE*TILE*2 = 32768。</summary>
    public const int TilePixelBytes = TileSize * TileSize * 2;

    /// <summary>单块掩码字节数 = TILE*TILE/8 = 2048。</summary>
    public const int TileMaskBytes = TileSize * TileSize / 8;

    /// <summary>块内掩码行字节数 = TILE/8 = 16。</summary>
    public const int MaskRowBytes = TileSize / 8;

    /// <summary>水平瓦片数 gx = ceil(lw / TILE)（lw ≤ 0 → 0）。</summary>
    public static int GridX(int lw) => lw <= 0 ? 0 : (lw + TileSize - 1) / TileSize;

    /// <summary>垂直瓦片数 gy = ceil(lh / TILE)（lh ≤ 0 → 0）。</summary>
    public static int GridY(int lh) => lh <= 0 ? 0 : (lh + TileSize - 1) / TileSize;

    /// <summary>分块后像素区字节数 = gx*gy*32768。</summary>
    public static int PixelsLen(int lw, int lh)
        => (lw <= 0 || lh <= 0) ? 0 : GridX(lw) * GridY(lh) * TilePixelBytes;

    /// <summary>分块后掩码区字节数 = gx*gy*2048。</summary>
    public static int MaskLen(int lw, int lh)
        => (lw <= 0 || lh <= 0) ? 0 : GridX(lw) * GridY(lh) * TileMaskBytes;

    /// <summary>层像素区大小（含掩码与否由调用方按层决定）。</summary>
    public static int LayerPixelsWithMaskLen(int lw, int lh) => PixelsLen(lw, lh) + MaskLen(lw, lh);

    /// <summary>
    /// 契约 §6 的取值算式（像素）：世界 (x,y) → 相对层像素区起点的字节偏移。
    /// 越界（x∉[0,lw) 或 y∉[0,lh)）返回 -1（该像素在包内不存在；分块补 0 区不会被寻址）。
    /// </summary>
    public static long PixelOffset(int lw, int lh, int x, int y)
    {
        if (x < 0 || y < 0 || x >= lw || y >= lh) return -1;
        int gx = GridX(lw);
        long tile = (long)(y / TileSize) * gx + (x / TileSize);
        return tile * TilePixelBytes + ((long)(y % TileSize) * TileSize + (x % TileSize)) * 2;
    }

    /// <summary>
    /// 契约 §6 的取值算式（掩码）：世界 (x,y) → 相对层**掩码区**起点的字节偏移（不含像素区长度）。
    /// 越界返回 -1。位序 MSB-first：字节内 <c>0x80 &gt;&gt; (x % 8)</c>。
    /// </summary>
    public static long MaskOffset(int lw, int lh, int x, int y)
    {
        if (x < 0 || y < 0 || x >= lw || y >= lh) return -1;
        int gx = GridX(lw);
        long tile = (long)(y / TileSize) * gx + (x / TileSize);
        return tile * TileMaskBytes + (long)(y % TileSize) * MaskRowBytes + (x % TileSize) / 8;
    }

    /// <summary>掩码字节内的位掩码（MSB-first，与既有 tile 掩码位序一致）。</summary>
    public static byte MaskBit(int x) => (byte)(0x80 >> (x % 8));
}
