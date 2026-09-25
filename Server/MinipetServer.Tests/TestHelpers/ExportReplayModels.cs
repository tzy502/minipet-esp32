namespace MinipetServer.Tests.TestHelpers;

/// <summary>回放测试的纯数据模型（M2 Mpak 读取器输出 → 这里规整，测试算法只依赖这些结构）。</summary>
public static class ExportReplayModels
{
    /// <summary>PARTS 包里的单个部件位图（RGB565 + 1bit alpha）。</summary>
    public sealed class ReplayPart
    {
        public required uint PartId { get; init; }
        public required ushort ExprGroup { get; init; }
        public required int Width { get; init; }
        public required int Height { get; init; }
        public required int OriginX { get; init; }
        public required int OriginY { get; init; }
        /// <summary>RGB565 像素，行主序（stride/padding 由适配层展平）。</summary>
        public required ushort[] Pixels565 { get; init; }
        /// <summary>1bit alpha 掩码（MSB 优先，每 8 像素 1 字节）；无 alpha 的部件为 null = 全不透明。</summary>
        public required byte[]? AlphaMask { get; init; }

        public bool IsOpaque(int x, int y)
        {
            if (AlphaMask == null) return true;
            int i = y * Width + x;
            return (AlphaMask[i >> 3] & (0x80 >> (i & 7))) != 0;
        }

        public (byte R, byte G, byte B) GetRgb565(int x, int y)
        {
            ushort p = Pixels565[y * Width + x];
            byte r = (byte)Math.Round(((p >> 11) & 0x1F) * 255.0 / 31);
            byte g = (byte)Math.Round(((p >> 5) & 0x3F) * 255.0 / 63);
            byte b = (byte)Math.Round((p & 0x1F) * 255.0 / 31);
            return (r, g, b);
        }
    }

    /// <summary>LAYOUT 帧内一条 piece 引用。</summary>
    public sealed class ReplayPiece
    {
        public required uint PartId { get; init; }
        /// <summary>255 = 非表情件。</summary>
        public required byte ExprIndex { get; init; }
        public required int X { get; init; }
        public required int Y { get; init; }
        public required byte Flip { get; init; }
        public required sbyte Z { get; init; }
    }

    public sealed class ReplayFrame
    {
        public required int DelayMs { get; init; }
        public required int MoveDx { get; init; }
        public required int MoveDy { get; init; }
        public required List<ReplayPiece> Pieces { get; init; }
    }

    public sealed class ReplayLayout
    {
        public required string Action { get; init; }
        public required int FrameCount { get; init; }
        public required int ExpressionCount { get; init; }
        public required List<string> Expressions { get; init; }
        public required List<ReplayFrame> Frames { get; init; }
    }
}
