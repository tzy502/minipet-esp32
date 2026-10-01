using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using SkiaSharp;

namespace MiniPet.Export;

/// <summary>
/// kind=3 BGMAP（地图背景包）—— 算法规格 §五。
///
/// payload：
/// <code>
/// [char32] map_id
/// [u16] vw, vh                    // 设备视口（导出时按设备 profile 定制）
///                                 // R2 整图包：= 整图世界尺寸（1x 原始像素，禁止降采样）
/// [u32] static_back_len, static_back_off
/// [u32] tile_layer_len, tile_layer_off
/// [u32] strip_count
/// [strip_count × 16B]: part_ref u64 | y i16 | speed_x i16(px/s) | rx_parallax u8 | blend u8
///                                 // R2 整图包：y = 带图顶边在「整图世界系」的 y（0 = 地图 bbox 顶边）
/// [... ] static_back：整幅 RGB565（vw×vh 不透明底，行对齐 4B）
/// [... ] tile_layer：RGBA5650（RGB565 行对齐 4B + 1bit mask 补 4B，同 PARTS 编码）
/// [... ] 可选整图扩展块（**仅 R2 整图包**；老固件安全，见下）
/// </code>
///
/// - 两层 len/off 为 payload 起算的显式偏移；条带数据不在本包内（part_ref 指向独立小 PARTS 包的 content_hash）。
/// - 条带图已由导出器按 cx 周期预平铺为「一个完整循环周期宽」的图，设备按 offset_x mod 图宽 循环 blit。
/// - 冰箱贴 profile：条带数可为 0（导出器按 profile 决定）。
///
/// ═════════════ 整图扩展块（GroundTable != null 时追加；R2 全图相机方案）═════════════
/// 布局（全部小端；位于 payload **尾部**，紧跟 tile 数据之后，4B 对齐）：
/// <code>
/// [u32 magic      ] = 0x4D504745
/// [u32 ground_len ] = ground_count × 2（= vw × 2 字节）
/// [u32 ground_off ] payload 起算偏移（指向 ground 表首字节）
/// [u32 flags      ] bit0 = 1：整图包（vw/vh = 整图世界尺寸、strip.y = 世界系 y）
/// [ground_count × u16] 地面表：第 x 列 = 整图世界系 x 处的地面 Y（0xFFFF = 该列无 foothold）
/// </code>
/// magic 的**字节布局**（务必逐字对齐，固件按 u32 读）：文件中 4 字节顺序为
/// 45 47 50 4D（即 'E','G','P','M'）；按小端 u32 解释即 0x4D504745
/// （写代码时用常量 BgmapExtensionMagic = 0x4D504745，不要按 ASCII 文本字符串比较）。
///
/// 【为什么放尾部而不是 strips 与 static 之间 —— 读老固件代码的结论】
/// `Firmware/board216/main/render/mpak.c` 的 parse_bgmap 在读完 56B 头后有**硬校验/自愈**：
/// <code>
/// uint32_t strips_end = (56u + strip_count*14u + 3u) & ~3u;
/// if (static_back_off != strips_end || tile_layer_off != strips_end + static_back_len
///     || strips_end + static_back_len + tile_layer_len > payload_len) { 校正为实际布局 }
/// </code>
/// 即老固件要求 static_back_off 恰好等于「56+14n 补 4B」，否则它会**用算出来的值覆盖**头部偏移。
/// 若把扩展块插在 strips 与 static 之间，老固件会把扩展块的头 4 字节当成 static 起点读，
/// 整幅 static 左移 16B（画面错位）。放到 tile **之后**（尾部）则：
///   static_back_off == (56+14n+3)&~3  ✓、tile_layer_off == static_off+static_back_len ✓、
///   两者之和 == tile 末尾 ≤ payload_len ✓（尾部还有扩展块，判据是 `>` 不是 `==`）
/// ⇒ 老固件既不告警也不校正，static/tile 读取位置与不含扩展块时**逐字节相同**；
///    `mpak_bgmap_read_static/read_tile` 只按 header 偏移+长度读，尾部多余字节无影响。
/// 另外老固件对整图包本身还有两道自己的门槛（与本扩展块无关，属固件侧待升级项）：
///   ① `vw > 512 || vh > 512` → MPAK_ERR_FMT（2270×1807 会被拒收）；
///   ② `static_back_len != vw*vh*2` 严格相等（本编码器按行 4B 对齐，宽度奇数时该等式不成立）。
/// 故整图包**默认关闭**：现网设备继续拿窗口包，绝不会因为本改动装不上。
///
/// 新固件取扩展块的定位规则（无需新字段）：
///   ext_off = align4(tile_layer_off + tile_layer_len)；读 16B 头，校验 magic 后再用 ground_off。
/// </summary>
public static class BgmapPackWriter
{
    public const int MapIdSize = 32;

    /// <summary>整图扩展块 magic（u32 小端；文件字节序 = 45 47 50 4D）。</summary>
    public const uint BgmapExtensionMagic = 0x4D504745;

    /// <summary>整图扩展块头长度（4×u32）。</summary>
    public const int BgmapExtensionHeaderSize = 16;

    /// <summary>flags bit0：整图包（vw/vh = 整图世界尺寸；strip.y = 世界系 y）。</summary>
    public const uint BgmapFlagFullMap = 1u;

    /// <summary>地面表「该列无 foothold」哨兵。</summary>
    public const ushort GroundNone = 0xFFFF;

    public sealed class BgmapStrip
    {
        /// <summary>条带 PARTS 包的 content_hash（manifest 同一身份）。</summary>
        public ulong PartRef;
        /// <summary>
        /// 窗口包（默认口径）：导出相机下条带静止时的**视口内** y（px，屏幕系）。
        /// 整图包（R2）：带图顶边在**整图世界系**的 y（0 = 地图 bbox 顶边 (MinX,MinY)；
        /// 与 static/tile 像素坐标、地面表列索引同一原点）。
        /// </summary>
        public short Y;
        /// <summary>水平滚动速度 px/s（ScrollH：rx*5，带符号）。</summary>
        public short SpeedX;
        /// <summary>WZ rx 视差系数（0..255 截断，信息性）。</summary>
        public byte RxParallax;
        /// <summary>混合强度（WZ alpha，255=不透明）。</summary>
        public byte Blend;
        /// <summary>
        /// 仅供导出端**层序重排**用的标识（不进 wire）：条带 = WZ 资源路径；
        /// 静态烘焙段 = "seg:{首个 back id}"。设备端不感知（BgmapPackWriter.Build 不写它）。
        /// </summary>
        public string Label = "";
    }

    public sealed class BgmapInput
    {
        public string MapId = "";
        public ushort Vw;
        public ushort Vh;
        /// <summary>static_back 快照（RenderViewport 全 back、剔除条带项后的视口渲染，BGRA）。</summary>
        public SKBitmap? StaticBack;
        /// <summary>tile 层（关 back 只渲 tile/obj，BGRA 透明底）。</summary>
        public SKBitmap? TileLayer;
        public List<BgmapStrip> Strips = new();
        /// <summary>
        /// 整图包（R2）地面表：长度必须 == Vw，第 x 列 = 整图世界系 x 处地面 Y
        /// （0xFFFF = 无 foothold）。**非 null 时**才追加尾部扩展块并置 flags bit0；
        /// 默认 null ⇒ payload 与旧版逐字节相同（现网零风险）。
        /// </summary>
        public ushort[]? GroundTable;

        /// <summary>是否整图包（= 是否追加扩展块）。</summary>
        public bool IsFullMap => GroundTable != null;
    }

    public static byte[] Build(BgmapInput input)
    {
        if (string.IsNullOrEmpty(input.MapId)) throw new ArgumentException("BGMAP 需要 map_id", nameof(input));
        if (Encoding.UTF8.GetByteCount(input.MapId) > MapIdSize)
            throw new ArgumentException($"map_id 超长: {input.MapId}");
        if (input.GroundTable != null && input.GroundTable.Length != input.Vw)
            throw new ArgumentException(
                $"整图地面表长度 {input.GroundTable.Length} != vw {input.Vw}（第 x 列 ↔ 世界 x）", nameof(input));

        // static_back = 纯 RGB565（不透明底，无掩码尾）；tile_layer = RGBA5650（RGB565 + 1bit mask，同 PARTS 编码）
        var staticData = input.StaticBack != null
            ? PartPackWriter.EncodeRgb565(input.StaticBack)
            : Array.Empty<byte>();
        var tileData = input.TileLayer != null
            ? PartPackWriter.EncodeRgb565WithMask(input.TileLayer)
            : Array.Empty<byte>();

        // 【布局修正 2026-09-27】每条条带实际序列化 14B（part_ref u64 + y i16 +
        // speed_x i16 + rx u8 + blend u8，见 mpak_wire_strip_t/_Static_assert），
        // 旧代码按 16B/条 估 headerLen ⇒ static_back_off/tile_layer_off 比真实位置
        // 大 8B：地图两层整体左移 4 像素、tile 掩码错位 64 像素（真机：对象错位 +
        // 块状黑斑 + 竖缝）。这里按实际字节数算，并把头部补到 4B 对齐。
        int stripsLen = input.Strips.Count * 14;
        int headerLen = MapIdSize + 2 + 2 + 4 + 4 + 4 + 4 + 4 + stripsLen;
        int headerPad = (4 - (headerLen & 3)) & 3;
        headerLen += headerPad;
        int staticOff = headerLen;
        int tileOff = staticOff + staticData.Length;

        // ── 整图扩展块（仅整图包）：追加在 tile 之后（尾部）──
        // 位置选择理由见类头注释（老固件 parse_bgmap 会硬校正 static/tile 偏移）。
        int tileEnd = tileOff + tileData.Length;
        int extPad = (4 - (tileEnd & 3)) & 3;
        int extOff = tileEnd + extPad;
        int groundOff = extOff + BgmapExtensionHeaderSize;
        int groundBytes = input.GroundTable != null ? input.GroundTable.Length * 2 : 0;
        int payloadLen = extOff + (input.GroundTable != null
            ? BgmapExtensionHeaderSize + groundBytes + ((4 - (groundBytes & 3)) & 3)
            : 0);

        var ms = new MemoryStream(payloadLen);
        using (var w = new BinaryWriter(ms, Encoding.UTF8, leaveOpen: true))
        {
            LayoutPackWriter.WriteFixedString(w, input.MapId, MapIdSize);
            w.Write(input.Vw);
            w.Write(input.Vh);
            w.Write((uint)staticData.Length);
            w.Write((uint)staticOff);
            w.Write((uint)tileData.Length);
            w.Write((uint)tileOff);
            w.Write((uint)input.Strips.Count);
            foreach (var s in input.Strips)
            {
                w.Write(s.PartRef);
                w.Write(s.Y);
                w.Write(s.SpeedX);
                w.Write(s.RxParallax);
                w.Write(s.Blend);
            }
            for (int i = 0; i < headerPad; i++) w.Write((byte)0);
            w.Write(staticData);
            w.Write(tileData);
            if (input.GroundTable != null)
            {
                for (int i = 0; i < extPad; i++) w.Write((byte)0);
                w.Write(BgmapExtensionMagic);          // 文件字节序 = 45 47 50 4D
                w.Write((uint)groundBytes);            // = vw × 2
                w.Write((uint)groundOff);              // payload 起算
                w.Write(BgmapFlagFullMap);             // bit0 = 1：整图包
                foreach (var g in input.GroundTable) w.Write(g);
                int tailPad = (4 - (groundBytes & 3)) & 3;
                for (int i = 0; i < tailPad; i++) w.Write((byte)0);
            }
        }
        return ms.ToArray();
    }
}
