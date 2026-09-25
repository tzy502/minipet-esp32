using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace MiniPet.Export;

/// <summary>
/// kind=2 LAYOUT（布局表包）—— 算法规格 §四。一个实体一个动作的布局；表情是独立维度。
///
/// payload：
/// <code>
/// [u32]  entity_id
/// [char32] action 名（UTF-8 定长 32B，零填充）
/// [u32]  frame_count
/// [u32]  expression_count
/// [expression_count × char32] 表情名列表
/// [frame_count × frame_header]:
///     [u32] delay_ms
///     [i16] move_dx、[i16] move_dy、[u16] pad（合占 u32×2 区域）
///     [u32] piece_count
///     [piece_count × 12B]: part_id u32 | expr_index u8(255=非表情件) | x i16 | y i16 | flip u8 | z i8
/// </code>
///
/// 语义约定（与桌面版 RenderFrame 对齐，回放工具按此重建）：
/// - 帧内 piece 列表按「绘制序」排列（底→顶，z 大者先画；z 值 = 帧内折叠相对序：最底 = piece_count-1 … 最顶 = 0，
///   i8 上限 127，超过时 z 截断但列表顺序仍为权威绘制序）。
/// - x/y = 部件相对「body 锚点（navel 系）」的偏移（不含帧位移 move、不含 union 画布偏移）；
///   回放/设备画布 = 全 piece 矩形联合（起点 = 最小 x/y），绘制位置 = (x + move_dx, y + move_dy)。
///   与桌面 RenderFrame 同轴：RenderFrame 的画布 piece 位置 = (FinalX - bx - bounds.Left + mv)，
///   联合画布起点 bounds.Left 由全部帧的最小 (FinalX - bx + mv) 构成 → 联合起点对齐后两画布逐像素同位。
/// - expr_index = 本布局几何所基于的表情在 expression 列表中的下标（face 变体件的标记，
///   ≠255 表示渲染时需按当前表情在 part 的 expr_group 内做同位替换）。
/// </summary>
public static class LayoutPackWriter
{
    public const int ActionNameSize = 32;
    public const int PieceSize = 12;
    public const byte ExprNone = 255;

    public sealed class LayoutPiece
    {
        public uint PartId;
        public byte ExprIndex = ExprNone;
        public short X;
        public short Y;
        public byte Flip;
        public sbyte Z;
    }

    public sealed class LayoutFrame
    {
        public uint DelayMs;
        public short MoveDx;
        public short MoveDy;
        public List<LayoutPiece> Pieces = new();
    }

    public sealed class LayoutInput
    {
        public uint EntityId;
        public string Action = "";
        public List<string> Expressions = new();
        public List<LayoutFrame> Frames = new();
    }

    /// <summary>写 char32 定长 UTF-8 区（超长截断，短则零填充）。</summary>
    public static void WriteFixedString(BinaryWriter w, string s, int size)
    {
        var bytes = Encoding.UTF8.GetBytes(s ?? "");
        if (bytes.Length > size) bytes = bytes[..size]; // 按字节截断（多字节截尾由调用方保证不超长）
        w.Write(bytes);
        if (bytes.Length < size) w.Write(new byte[size - bytes.Length]);
    }

    public static byte[] Build(LayoutInput input)
    {
        if (string.IsNullOrEmpty(input.Action)) throw new ArgumentException("LAYOUT 需要 action 名", nameof(input));
        if (Encoding.UTF8.GetByteCount(input.Action) > ActionNameSize)
            throw new ArgumentException($"action 名超长: {input.Action}");
        foreach (var e in input.Expressions)
            if (Encoding.UTF8.GetByteCount(e) > ActionNameSize)
                throw new ArgumentException($"表情名超长: {e}");

        var ms = new MemoryStream(4096);
        using (var w = new BinaryWriter(ms, Encoding.UTF8, leaveOpen: true))
        {
            w.Write(input.EntityId);
            WriteFixedString(w, input.Action, ActionNameSize);
            w.Write((uint)input.Frames.Count);
            w.Write((uint)input.Expressions.Count);
            foreach (var e in input.Expressions) WriteFixedString(w, e, ActionNameSize);

            foreach (var f in input.Frames)
            {
                w.Write(f.DelayMs);
                w.Write(f.MoveDx);
                w.Write(f.MoveDy);
                w.Write((ushort)0); // pad
                w.Write((uint)f.Pieces.Count);
                foreach (var p in f.Pieces)
                {
                    w.Write(p.PartId);
                    w.Write(p.ExprIndex);
                    w.Write(p.X);
                    w.Write(p.Y);
                    w.Write(p.Flip);
                    w.Write(p.Z);
                }
            }
        }
        return ms.ToArray();
    }
}
