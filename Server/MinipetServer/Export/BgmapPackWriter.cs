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
/// [u32] static_back_len, static_back_off
/// [u32] tile_layer_len, tile_layer_off
/// [u32] strip_count
/// [strip_count × 16B]: part_ref u64 | y i16 | speed_x i16(px/s) | rx_parallax u8 | blend u8
/// [... ] static_back：整幅 RGB565（vw×vh 不透明底，行对齐 4B）
/// [... ] tile_layer：RGBA5650（RGB565 行对齐 4B + 1bit mask 补 4B，同 PARTS 编码）
/// </code>
///
/// - 两层 len/off 为 payload 起算的显式偏移；条带数据不在本包内（part_ref 指向独立小 PARTS 包的 content_hash）。
/// - 条带图已由导出器按 cx 周期预平铺为「一个完整循环周期宽」的图，设备按 offset_x mod 图宽 循环 blit。
/// - 冰箱贴 profile：条带数可为 0（导出器按 profile 决定）。
/// </summary>
public static class BgmapPackWriter
{
    public const int MapIdSize = 32;

    public sealed class BgmapStrip
    {
        /// <summary>条带 PARTS 包的 content_hash（manifest 同一身份）。</summary>
        public ulong PartRef;
        /// <summary>导出相机下条带静止时的视口内 y（px）。</summary>
        public short Y;
        /// <summary>水平滚动速度 px/s（ScrollH：rx*5，带符号）。</summary>
        public short SpeedX;
        /// <summary>WZ rx 视差系数（0..255 截断，信息性）。</summary>
        public byte RxParallax;
        /// <summary>混合强度（WZ alpha，255=不透明）。</summary>
        public byte Blend;
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
    }

    public static byte[] Build(BgmapInput input)
    {
        if (string.IsNullOrEmpty(input.MapId)) throw new ArgumentException("BGMAP 需要 map_id", nameof(input));
        if (Encoding.UTF8.GetByteCount(input.MapId) > MapIdSize)
            throw new ArgumentException($"map_id 超长: {input.MapId}");

        // static_back = 纯 RGB565（不透明底，无掩码尾）；tile_layer = RGBA5650（RGB565 + 1bit mask，同 PARTS 编码）
        var staticData = input.StaticBack != null
            ? PartPackWriter.EncodeRgb565(input.StaticBack)
            : Array.Empty<byte>();
        var tileData = input.TileLayer != null
            ? PartPackWriter.EncodeRgb565WithMask(input.TileLayer)
            : Array.Empty<byte>();

        int headerLen = MapIdSize + 2 + 2 + 4 + 4 + 4 + 4 + 4 + input.Strips.Count * 16;
        int staticOff = headerLen;
        int tileOff = staticOff + staticData.Length;

        var ms = new MemoryStream(headerLen + staticData.Length + tileData.Length);
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
            w.Write(staticData);
            w.Write(tileData);
        }
        return ms.ToArray();
    }
}
