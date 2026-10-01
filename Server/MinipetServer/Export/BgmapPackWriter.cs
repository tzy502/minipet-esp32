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
///
/// ═════════════ 分块（tiled）布局（2026-10-01；冻结契约 docs/ai/map-tiled-format-contract.md）═════════════
/// 动因：SD 顺序读 1336 KB/s，而跨行距逐行读只有 ~130 KB/s（真机实测），装载 8~17s。
/// 开关：<see cref="BgmapInput.Tiled"/> ⇒ 尾扩展块 flags 置 **bit1 = <see cref="BgmapFlagTiled"/>**；
/// 信封 / 头 / 字段顺序 / 尺寸语义 / 地面表 / 条带语义**全部不变**，只有"层长度"的含义随 bit1 变：
/// <code>
/// static_back_len = gx*gy*32768                     （旧：vh*align4(vw*2)）
/// tile_layer_len  = gx*gy*32768 + gx*gy*2048        （旧：RGB 行区 + align4(掩码)）
///                   掩码区紧跟像素区之后、无额外补齐（沿用"掩码偏移 = 像素区之后"的既有口径）
/// gx = ceil(lw/128), gy = ceil(lh/128)，瓦片写满、越界补 0（契约 §2/§3）
/// </code>
/// **条带**：条带像素不在本包内（仍由 strip.part_ref 指向独立小 PARTS 包），因此 bit1 同时也是
/// 那些 PARTS 包的口径标志 —— 分块导出时条带小包的位图记录必须一并按瓦片编码
/// （<see cref="PartPackWriter.Build"/> 的 tiled 参数），三类层**不得混用**。
///
/// 【固件侧前置项，服务端实现时读 mpak.c 得出，已在契约文档末尾"实现注记"登记】
///   ① `parse_bgmap` 的 static/tile `*_len` 白名单（static_tight / static_aligned / tile_expect /
///      tile_expect_enc）在**读尾扩展块之前**判定 ⇒ 分块长度会被 MPAK_ERR_FMT 直接拒收，
///      必须先读 flags（或把分块长度纳入白名单）再判几何；
///   ② 条带小 PARTS 包：`parse_parts` 的 extent 推断只认 `pixel_bytes` /
///      `pixel_bytes+mask`（掩码补 4B）⇒ 分块记录（更大且非该两式）会被拒，需按 bit1 放行；
///   ③ 分块后条带像素寻址不得再用 `row_bytes`（契约 §5）。
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

    /// <summary>
    /// flags bit1：**分块（tiled）布局** —— 本包的三类像素层（static / tile / 条带）按
    /// <see cref="TiledLayout"/> 的 128×128 世界像素瓦片存储（冻结契约
    /// <c>docs/ai/map-tiled-format-contract.md</c> §1）。未置位 ⇒ 一切按旧「逐行」格式解析。
    /// </summary>
    public const uint BgmapFlagTiled = 2u;

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

        /// <summary>
        /// **分块（tiled）布局**（契约 §1~§5）：static / tile 两层改成 128×128 世界像素瓦片存储
        /// （tile 层掩码同网格分块、紧跟像素区之后），并在尾扩展块 flags 置 bit1。调用方还须把
        /// 条带小 PARTS 包也按分块口径编码（<see cref="PartPackWriter.Build"/> 的 tiled 参数）——
        /// bit1 是三类层共用的口径标志，**不允许混用**。
        /// 仅在整图包（<see cref="IsFullMap"/>）下可用：flags 位于尾扩展块，窗口包没有承载位。
        /// </summary>
        public bool Tiled;

        /// <summary>是否整图包（= 是否追加扩展块）。</summary>
        public bool IsFullMap => GroundTable != null;

        /// <summary>尾扩展块 flags 实写值（bit0 整图 / bit1 分块）。</summary>
        public uint Flags => BgmapFlagFullMap | (Tiled ? BgmapFlagTiled : 0u);
    }

    public static byte[] Build(BgmapInput input)
    {
        if (string.IsNullOrEmpty(input.MapId)) throw new ArgumentException("BGMAP 需要 map_id", nameof(input));
        if (Encoding.UTF8.GetByteCount(input.MapId) > MapIdSize)
            throw new ArgumentException($"map_id 超长: {input.MapId}");
        if (input.GroundTable != null && input.GroundTable.Length != input.Vw)
            throw new ArgumentException(
                $"整图地面表长度 {input.GroundTable.Length} != vw {input.Vw}（第 x 列 ↔ 世界 x）", nameof(input));
        /* 分块口径的承载位在尾扩展块 flags（bit1）里：没有扩展块就没有地方声明布局，
         * 强行按分块写会得到一个"旧固件当逐行读"的坏包 —— 直接拒绝。 */
        if (input.Tiled && !input.IsFullMap)
            throw new ArgumentException(
                "tiled（分块）布局仅支持整图包：flags bit1 位于尾扩展块，窗口包无承载字段", nameof(input));

        // static_back = 纯 RGB565（不透明底，无掩码尾）；tile_layer = RGBA5650（RGB565 + 1bit mask，同 PARTS 编码）
        // 分块口径（契约 §2/§3）：两层都是 128×128 瓦片；tile 掩码区同网格分块、紧跟像素区。
        bool tiled = input.Tiled;
        var staticData = input.StaticBack != null
            ? (tiled ? PartPackWriter.EncodeRgb565Tiled(input.StaticBack)
                     : PartPackWriter.EncodeRgb565(input.StaticBack))
            : Array.Empty<byte>();
        var tileData = input.TileLayer != null
            ? (tiled ? PartPackWriter.EncodeRgb565TiledWithMask(input.TileLayer)
                     : PartPackWriter.EncodeRgb565WithMask(input.TileLayer))
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
                w.Write(input.Flags);                  // bit0 = 整图包；bit1 = 分块（tiled）布局
                foreach (var g in input.GroundTable) w.Write(g);
                int tailPad = (4 - (groundBytes & 3)) & 3;
                for (int i = 0; i < tailPad; i++) w.Write((byte)0);
            }
        }
        return ms.ToArray();
    }
}
