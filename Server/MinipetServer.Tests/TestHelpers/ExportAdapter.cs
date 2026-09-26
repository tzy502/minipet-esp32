using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using MiniPet.Export;
using MinipetServer.Models;
using MinipetServer.Services;
using Xunit;
using static MinipetServer.Tests.TestHelpers.ExportReplayModels;

namespace MinipetServer.Tests.TestHelpers;

/// <summary>
/// M2 API 适配层：AssetExporter 导包 → Mpak 读取器解包 → 规整为回放测试的纯数据模型。
/// 所有对 M2 交付代码（AssetExporter / Mpak 读取器 / 默认装扮）的调用集中在此，
/// M2 实际 API 与假设有出入时只改这个文件。
/// </summary>
internal static class ExportAdapter
{
    // ── 布局约定（对齐 LayoutPackWriter 语义注释）──
    /// <summary>LAYOUT 帧内 piece 列表本身即「绘制序」（底→顶，权威序），回放不再按 z 重排。</summary>
    public static bool PieceListIsDrawOrder => true;
    /// <summary>LAYOUT piece (x,y) 不含帧位移 move：回放绘制位置 = (x + move_dx, y + move_dy)。</summary>
    public static bool MoveAppliedInPieceXY => false;

    // ═══ 默认装扮（男体 2000 / 发 30000 / 脸 20000，对齐桌面版默认与 M1 黑盒测试）═══
    public static CharacterAppearance DefaultAppearance() => new()
    {
        Gender = 0,
        BodyId = 2000,
        Hair = new ItemInfo { Id = "30000" },
        Face = new ItemInfo { Id = "20000" },
    };

    // ═══ 导出（AssetExporter.Run 全量跑一次，进程内缓存；取 walk1 LAYOUT + 装扮 PARTS）═══
    private static readonly Lazy<(byte[] Parts, byte[] Layout)> _export = new(RunExport,
        LazyThreadSafetyMode.ExecutionAndPublication);

    public static (byte[] Parts, byte[] Layout) ExportDefaultWalk1() => _export.Value;

    private static (byte[] Parts, byte[] Layout) RunExport()
    {
        var appearance = DefaultAppearance();
        string jsonPath = Path.Combine(Path.GetTempPath(), $"minipet-m2test-appearance-{Guid.NewGuid():N}.json");
        string outRoot = Path.Combine(Path.GetTempPath(), $"minipet-m2test-export-{Guid.NewGuid():N}");
        File.WriteAllText(jsonPath, JsonSerializer.Serialize(appearance));
        try
        {
            var exporter = new AssetExporter(WzFixture.Wz);
            var summary = exporter.Run(new ExportOptions
            {
                AppearancePath = jsonPath,
                OutRoot = outRoot,
                DeviceId = "m2test",
                Maps = new List<string>(),      // 不导地图
                FontSizes = new List<int>(),    // 不导字体
                IncludeAudio = false,
                IncludeFontTime = false,
            });
            var layout = summary.Assets.FirstOrDefault(a =>
                a.Kind == MpakKind.Layout && a.Extra.TryGetValue("action", out var act) && (string?)act == "walk1");
            var parts = summary.Assets.FirstOrDefault(a =>
                a.Kind == MpakKind.Parts && a.Extra.TryGetValue("entity", out var ent) && (string?)ent == "paperdoll:default");
            Assert.NotNull(layout);
            Assert.NotNull(parts);
            return (parts!.Bytes, layout!.Bytes);
        }
        finally
        {
            try { Directory.Delete(outRoot, true); } catch { /* 临时产物清理失败不影响测试 */ }
            try { File.Delete(jsonPath); } catch { }
        }
    }

    // ═══ M2-ADAPT：信封校验（Mpak.TryRead：MAGIC→len→crc→hash 全量校验）═══
    public static bool Validate(byte[] package) =>
        Mpak.TryRead(package, out _) == Mpak.ReadError.None;

    // ═══ M2-ADAPT：解包（Mpak 读取器解信封 → 规格 §三/§四 解 payload）═══
    public static Dictionary<uint, ReplayPart> UnpackParts(byte[] package)
    {
        var payload = ReadPayload(package, MpakKind.Parts);
        return SpecMpakParser.ParseParts(payload);
    }

    public static ReplayLayout UnpackLayout(byte[] package)
    {
        var payload = ReadPayload(package, MpakKind.Layout);
        return SpecMpakParser.ParseLayout(payload);
    }

    private static byte[] ReadPayload(byte[] package, MpakKind expectKind)
    {
        var err = Mpak.TryRead(package, out var packet);
        Assert.True(err == Mpak.ReadError.None, $"Mpak.TryRead 失败: {err}");
        Assert.Equal(expectKind, packet!.Kind);
        return packet.Payload;
    }
}

/// <summary>
/// 按算法级规格（docs/ai/algorithm-asset-format.md §三/§四）解析 PARTS/LAYOUT payload。
/// 读取器侧的「解包」最小闭环：小端、定长头、偏移索引。
/// </summary>
internal static class SpecMpakParser
{
    public static Dictionary<uint, ReplayPart> ParseParts(byte[] p)
    {
        var r = new Reader(p);
        uint partCount = r.U32();
        var index = new (uint PartId, ushort ExprGroup, int W, int H, int Ox, int Oy, uint Offset)[partCount];
        for (int i = 0; i < partCount; i++)
        {
            index[i] = (r.U32(), r.U16(), r.U16(), r.U16(), r.I16(), r.I16(), r.U32());
            _ = r.U16();   // 索引项 20B 尾填充
        }
        var parts = new Dictionary<uint, ReplayPart>();
        foreach (var e in index)
        {
            int pxCount = e.W * e.H;
            int pitch = (e.W * 2 + 3) / 4 * 4;            // RGB565 行对齐 4B
            var pixels = new ushort[pxCount];
            // offset = 位图数据区相对偏移；换算成 payload 内绝对位置需加索引区长度
            int indexLen = 4 + index.Length * 20;
            int recStart = indexLen + (int)e.Offset;
            for (int y = 0; y < e.H; y++)
            {
                int rowOff = recStart + y * pitch;
                for (int x = 0; x < e.W; x++)
                    pixels[y * e.W + x] = r.U16At(rowOff + x * 2);
            }
            // 1bit alpha 掩码附在像素区后：w*h bit 连续 MSB 在前，尾部补零到 4B 对齐
            int pixelBytes = pitch * e.H;
            int maskLen = (pxCount + 7) / 8;
            int maskStart = recStart + pixelBytes;
            byte[]? mask = maskStart + maskLen <= p.Length ? p[maskStart..(maskStart + maskLen)] : null;
            parts[e.PartId] = new ReplayPart
            {
                PartId = e.PartId, ExprGroup = e.ExprGroup,
                Width = e.W, Height = e.H, OriginX = e.Ox, OriginY = e.Oy,
                Pixels565 = pixels, AlphaMask = mask,
            };
        }
        return parts;
    }

    public static ReplayLayout ParseLayout(byte[] p)
    {
        var r = new Reader(p);
        _ = r.U32();                                   // entity_id
        var action = FixedString(r, 32);
        uint frameCount = r.U32();
        uint expressionCount = r.U32();
        var expressions = new List<string>();
        for (int i = 0; i < expressionCount; i++) expressions.Add(FixedString(r, 32));
        var frames = new List<ReplayFrame>();
        for (int f = 0; f < frameCount; f++)
        {
            int delay = (int)r.U32();
            short dx = r.I16(), dy = r.I16();
            uint pieceCount = r.U32();                 // 帧头 12B（无 pad）
            var pieces = new List<ReplayPiece>();
            for (int i = 0; i < pieceCount; i++)
            {
                uint partId = r.U32();
                byte exprIndex = r.U8();
                short x = r.I16(), y = r.I16();
                byte flip = r.U8();
                sbyte z = r.I8();
                _ = r.U8();                            // piece 12B 尾填充
                pieces.Add(new ReplayPiece { PartId = partId, ExprIndex = exprIndex, X = x, Y = y, Flip = flip, Z = z });
            }
            frames.Add(new ReplayFrame { DelayMs = delay, MoveDx = dx, MoveDy = dy, Pieces = pieces });
        }
        return new ReplayLayout
        {
            Action = action, FrameCount = (int)frameCount, ExpressionCount = (int)expressionCount,
            Expressions = expressions, Frames = frames,
        };
    }

    private static string FixedString(Reader r, int len)
    {
        var bytes = r.Bytes(len);
        int end = Array.IndexOf(bytes, (byte)0);
        return Encoding.UTF8.GetString(bytes, 0, end < 0 ? len : end);
    }

    private sealed class Reader(byte[] data)
    {
        private int _pos;
        public uint U32() { var v = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(_pos, 4)); _pos += 4; return v; }
        public ushort U16() { var v = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(_pos, 2)); _pos += 2; return v; }
        public short I16() { var v = BinaryPrimitives.ReadInt16LittleEndian(data.AsSpan(_pos, 2)); _pos += 2; return v; }
        public byte U8() => data[_pos++];
        public sbyte I8() => (sbyte)data[_pos++];
        public byte[] Bytes(int n) { var v = data[_pos..(_pos + n)]; _pos += n; return v; }
        public ushort U16At(int absPos) => BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(absPos, 2));
    }
}
