using System.Runtime.InteropServices;
using System.Text;
using MinipetServer.Tests.TestHelpers;
using SkiaSharp;
using Xunit;
using static MinipetServer.Tests.TestHelpers.ExportReplayModels;

namespace MinipetServer.Tests;

/// <summary>
/// M2 验收：MPAK 解包回放 vs PaperdollService 直渲染 逐像素对比（software-design.md M2 行）。
///
/// 1) 回放对比：AssetExporter 导默认装扮 walk1 帧段 → Mpak 读取器解包 → 按布局 blit 回 SKBitmap（1x 域）
///    → 与 PaperdollService.RenderFrame 同帧直渲染对比；允许 1bit alpha 量化差异，断言差异像素 &lt; 0.5%，
///    差异数写入测试消息。
/// 2) 信封校验：篡改 payload 1 字节 → crc 必失败；content_hash 不匹配必失败。
/// 3) LAYOUT 冒烟：全帧全 piece 的 part_id 都在 PARTS 索引中；每个 expr_group 的条目数 == expression_count。
/// 4) alpha-ab/ 产物：同帧「1bit alpha 回放图」vs「直渲染参考图」两张 PNG 落盘（不断言，供人眼验收）。
/// </summary>
public class ExportReplayTests
{
    private const string Action = "walk1";
    private const double MaxDiffPercent = 0.5; // < 0.5%

    // ═══════════════════════════════════════════════════════════
    // 测试 1：回放 vs 直渲染 逐像素对比
    // ═══════════════════════════════════════════════════════════

    [WzFact]
    public void Export_ReplayWalk1MatchesDirectRender()
    {
        var (partsPkg, layoutPkg) = ExportAdapter.ExportDefaultWalk1();
        var parts = ExportAdapter.UnpackParts(partsPkg);
        var layout = ExportAdapter.UnpackLayout(layoutPkg);

        Assert.True(parts.Count > 0, "PARTS 索引为空");
        Assert.True(layout.FrameCount > 0, "LAYOUT 无帧");
        Assert.Equal(Action, layout.Action);

        var doll = WzFixture.CreatePaperdoll();
        var appearance = ExportAdapter.DefaultAppearance();

        long totalDiff = 0, totalQuant = 0, totalPixels = 0;
        long quantShade = 0, quantBlack = 0, quantSoft = 0;
        var perFrame = new List<string>();

        for (int f = 0; f < layout.FrameCount; f++)
        {
            var (replay, repOx, repOy) = BlitFrame(layout, f, parts);
            var (direct, dirOx, dirOy, fw, fh) = doll.RenderFrame(appearance, Action, f, "default");
            Assert.NotNull(direct);
            Assert.True(fw > 0 && fh > 0, $"直渲染帧 {f} 尺寸异常 {fw}x{fh}");

            // 直渲染 canvas_x = piece.x + move；其返回 origin（-bounds.Left）不含 move → 补 move 对齐
            var frame = layout.Frames[f];
            var (diff, quant, count, qs, qb, qsoft) = CompareAligned(
                direct!, dirOx + frame.MoveDx, dirOy + frame.MoveDy, replay, repOx, repOy);
            totalDiff += diff; totalQuant += quant; totalPixels += count;
            quantShade += qs; quantBlack += qb; quantSoft += qsoft;
            perFrame.Add($"f{f}:{diff}/{count}({(count == 0 ? 0 : diff * 100.0 / count):F3}%)");

            if (f == 0) WriteAlphaAb(direct!, replay);   // alpha-ab 产物（第 0 帧）

            replay.Dispose();
            direct!.Dispose();
        }

        double pct = totalPixels == 0 ? 100 : totalDiff * 100.0 / totalPixels;
        Console.WriteLine(
            $"[ExportReplay] walk1 {layout.FrameCount} 帧对比完成：真实差异 {totalDiff}/{totalPixels} = {pct:F4}%；" +
            $"1bit alpha 量化差异 {totalQuant}（shade {quantShade} / black {quantBlack} / soft {quantSoft}）");
        Assert.True(pct < MaxDiffPercent,
            $"回放 vs 直渲染【真实】差异像素 {totalDiff}/{totalPixels} = {pct:F4}%（阈值 {MaxDiffPercent}%）；" +
            $"1bit alpha 量化解释的差异 {totalQuant}（丢弃半透明 shade {quantShade} / 深色边缘硬化 {quantBlack} / 亮部软混合 {quantSoft}）；" +
            $"逐帧真实差异：{string.Join(", ", perFrame)}");
    }

    // ═══════════════════════════════════════════════════════════
    // 测试 2：信封校验（篡改必失败）
    // ═══════════════════════════════════════════════════════════

    [WzFact]
    public void Export_TamperedPayloadFailsCrc()
    {
        var (partsPkg, _) = ExportAdapter.ExportDefaultWalk1();
        var env = EnvelopeLayout.Detect(partsPkg);

        // 篡改 payload 中部 1 字节（不修 crc）→ crc32c 覆盖 header+payload 全量，必失败
        var tampered = (byte[])partsPkg.Clone();
        int at = env.PayloadOffset + Math.Max(1, env.PayloadLen / 2);
        tampered[at] ^= 0xFF;

        Assert.False(ExportAdapter.Validate(tampered),
            $"篡改 payload @{at}（偏移 {at - env.PayloadOffset}/{env.PayloadLen}）后校验仍通过（crc 应失败）");
    }

    [WzFact]
    public void Export_MismatchedContentHashFails()
    {
        var (partsPkg, _) = ExportAdapter.ExportDefaultWalk1();
        var env = EnvelopeLayout.Detect(partsPkg);

        // 路径 A：篡改 payload 1 字节但修好 crc → content_hash 不再匹配 payload → 必失败
        var tampered = (byte[])partsPkg.Clone();
        int at = env.PayloadOffset + Math.Max(1, env.PayloadLen / 3);
        tampered[at] ^= 0x01;
        FixCrc(tampered, env);
        Assert.False(ExportAdapter.Validate(tampered),
            $"篡改 payload @{at}（crc 已修复）后校验仍通过（content_hash 应不匹配）");

        // 路径 B：篡改信封 content_hash 字段本身（crc 修复）→ 与 manifest 比对必失败
        var tamperedHash = (byte[])partsPkg.Clone();
        tamperedHash[env.HashOffset + 4] ^= 0xFF;
        FixCrc(tamperedHash, env);
        Assert.False(ExportAdapter.Validate(tamperedHash),
            "篡改 content_hash 字段后校验仍通过");
    }

    [WzFact]
    public void Export_IntactPackageValidates()
    {
        // 对照组：未篡改的包必须通过校验（防「校验恒 false」的假绿）
        var (partsPkg, layoutPkg) = ExportAdapter.ExportDefaultWalk1();
        Assert.True(ExportAdapter.Validate(partsPkg), "PARTS 原包校验失败");
        Assert.True(ExportAdapter.Validate(layoutPkg), "LAYOUT 原包校验失败");
    }

    // ═══════════════════════════════════════════════════════════
    // 测试 3：LAYOUT 冒烟
    // ═══════════════════════════════════════════════════════════

    [WzFact]
    public void Export_LayoutSmoke_AllPiecesResolvableAndExpressionGroupsConsistent()
    {
        var (partsPkg, layoutPkg) = ExportAdapter.ExportDefaultWalk1();
        var parts = ExportAdapter.UnpackParts(partsPkg);
        var layout = ExportAdapter.UnpackLayout(layoutPkg);

        // ① 全帧全 piece 的 part_id 都在 PARTS 索引
        var missing = new List<string>();
        long pieceTotal = 0;
        for (int f = 0; f < layout.FrameCount; f++)
        {
            foreach (var piece in layout.Frames[f].Pieces)
            {
                pieceTotal++;
                if (!parts.ContainsKey(piece.PartId))
                    missing.Add($"f{f}:part{piece.PartId}");
            }
        }
        Assert.True(missing.Count == 0,
            $"{missing.Count}/{pieceTotal} 个 piece 的 part_id 不在 PARTS 索引：{string.Join(",", missing.Take(10))}");

        // ② 每个表情变体组的条目数 == expression_count（组内按 LAYOUT 表情列表顺序同构）
        var byGroup = parts.Values.GroupBy(p => p.ExprGroup)
            .Where(g => g.Key != 0)
            .ToDictionary(g => g.Key, g => g.Count());
        Assert.True(byGroup.Count > 0, "PARTS 无任何表情变体组（expr_group 全部为 0）");
        var badGroups = byGroup.Where(kv => kv.Value != layout.ExpressionCount).ToList();
        Assert.True(badGroups.Count == 0,
            $"表情组条目数 != expression_count({layout.ExpressionCount})：" +
            string.Join(",", badGroups.Select(kv => $"g{kv.Key}={kv.Value}")));

        // ③ 表情闭环冒烟：expr_index != 255 的 piece 可在组内按偏移取到变体
        foreach (var frame in layout.Frames)
        {
            foreach (var piece in frame.Pieces)
            {
                if (piece.ExprIndex == 255 || !parts.TryGetValue(piece.PartId, out var part)) continue;
                int variants = parts.Values.Count(p => p.ExprGroup == part.ExprGroup);
                Assert.True(piece.ExprIndex < variants,
                    $"piece part={piece.PartId} expr_index={piece.ExprIndex} 超出组内变体数 {variants}");
            }
        }
    }

    // ═══════════════════════════════════════════════════════════
    // 回放 blit：按布局把部件画回 SKBitmap（1x 域）
    // ═══════════════════════════════════════════════════════════

    /// <summary>
    /// 把一帧 blit 到联合画布。piece 列表本身即绘制序（底→顶）；(x,y) 不含 move，
    /// 绘制位置 = (x + move_dx, y + move_dy)（与 RenderFrame 的 canvas 坐标完全同轴）。
    /// 返回 (位图, 画布坐标轴原点在图内的 x, y)，供与直渲染对齐比较。
    /// </summary>
    private static (SKBitmap Bmp, int OriginX, int OriginY) BlitFrame(ReplayLayout layout, int frameIdx,
        IReadOnlyDictionary<uint, ReplayPart> parts)
    {
        var frame = layout.Frames[frameIdx];
        var ordered = ExportAdapter.PieceListIsDrawOrder
            ? frame.Pieces.ToList()                             // 列表序 = 权威绘制序
            : frame.Pieces.OrderByDescending(p => p.Z).ToList();

        int mvX = ExportAdapter.MoveAppliedInPieceXY ? 0 : frame.MoveDx;
        int mvY = ExportAdapter.MoveAppliedInPieceXY ? 0 : frame.MoveDy;

        // 画布 = 全 piece 矩形联合（navel 系）
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
        foreach (var p in ordered)
        {
            if (!parts.TryGetValue(p.PartId, out var part)) continue;
            minX = Math.Min(minX, p.X + mvX); minY = Math.Min(minY, p.Y + mvY);
            maxX = Math.Max(maxX, p.X + mvX + part.Width); maxY = Math.Max(maxY, p.Y + mvY + part.Height);
        }
        int w = Math.Max(1, maxX - minX), h = Math.Max(1, maxY - minY);

        var buf = new byte[w * h * 4];                  // 全 0 = 透明（BGRA straight）
        foreach (var p in ordered)
        {
            if (!parts.TryGetValue(p.PartId, out var part)) continue;
            int bx0 = p.X + mvX - minX, by0 = p.Y + mvY - minY;
            for (int y = 0; y < part.Height; y++)
            {
                for (int x = 0; x < part.Width; x++)
                {
                    if (!part.IsOpaque(x, y)) continue;
                    var (r, g, b) = part.GetRgb565(x, y);
                    int dx = p.Flip != 0 ? bx0 + part.Width - 1 - x : bx0 + x;
                    int dy = by0 + y;
                    if (dx < 0 || dy < 0 || dx >= w || dy >= h) continue;
                    int i = (dy * w + dx) * 4;
                    buf[i] = b; buf[i + 1] = g; buf[i + 2] = r; buf[i + 3] = 255;
                }
            }
        }

        var bmp = new SKBitmap(w, h, SKColorType.Bgra8888, SKAlphaType.Unpremul);
        Marshal.Copy(buf, 0, bmp.GetPixels(), buf.Length);
        return (bmp, -minX, -minY);
    }

    /// <summary>
    /// 原点对齐（navel 世界系）的逐像素比较。直渲染为 BGRA8888 Premul，回放为 Unpremul 1bit alpha。
    /// 判定（允许 1bit alpha 量化 + RGB565 量化）：
    ///  - 双方等效透明（直渲染 A&lt;128 且回放透明）→ 同；
    ///  - 双方等效不透明（直渲染 A≥128 且回放不透明）→ 直渲染 un-premul 直通色比较，R/B≤8、G≤5 容差 → 同；
    ///  - 1bit alpha 量化可解释的偏差（WZ 半透明件阈值化：丢弃 shade / 深色边缘硬化 / 亮部软混合）→ 计入量化差异数、不算真实差异；
    ///  - 其余（半透明跨界 / 单侧不透明 / 色差超限）→ 真实差异。
    /// 量化三类边界（收紧，防吞真实错误）：
    ///  shade  —— 直渲为回放的均匀压暗：≥2 通道比值 ∈[0.40,0.97] 且极差 ≤0.15（丢弃的半透明投影/发影）；
    ///  black  —— 回放近黑颜料(≤48)且直渲偏暗(≤160)（半透明深色边缘阈值化为不透明颜料）；
    ///  soft   —— 双方亮暖色(最小通道≥140)且逐通道差 ≤40（亮部颜料的软混合残差，R 通道常裁剪在 255）。
    /// </summary>
    private static (long Diff, long Quant, long Compared, long QuantShade, long QuantBlack, long QuantSoft) CompareAligned(
        SKBitmap direct, int dirOx, int dirOy, SKBitmap replay, int repOx, int repOy)
    {
        var dirBuf = ReadPixels(direct);
        var repBuf = ReadPixels(replay);

        int wx0 = Math.Min(-dirOx, -repOx), wy0 = Math.Min(-dirOy, -repOy);
        int wx1 = Math.Max(direct.Width - dirOx, replay.Width - repOx);
        int wy1 = Math.Max(direct.Height - dirOy, replay.Height - repOy);

        long diff = 0, compared = 0, quantShade = 0, quantBlack = 0, quantSoft = 0;
        for (int wy = wy0; wy < wy1; wy++)
        {
            for (int wx = wx0; wx < wx1; wx++)
            {
                int dxp = wx + dirOx, dyp = wy + dirOy;
                int rxp = wx + repOx, ryp = wy + repOy;
                bool inD = dxp >= 0 && dyp >= 0 && dxp < direct.Width && dyp < direct.Height;
                bool inR = rxp >= 0 && ryp >= 0 && rxp < replay.Width && ryp < replay.Height;
                if (!inD && !inR) continue;
                compared++;

                byte da = 0, dr = 0, dg = 0, db = 0;
                if (inD)
                {
                    int i = (dyp * direct.Width + dxp) * 4;      // premul BGRA
                    db = dirBuf[i]; dg = dirBuf[i + 1]; dr = dirBuf[i + 2]; da = dirBuf[i + 3];
                }
                byte ra = 0, rr = 0, rg = 0, rb = 0;
                if (inR)
                {
                    int i = (ryp * replay.Width + rxp) * 4;      // straight BGRA
                    rb = repBuf[i]; rg = repBuf[i + 1]; rr = repBuf[i + 2]; ra = repBuf[i + 3];
                }

                if (ra == 0 && da < 128) continue;               // 双透明（含量化）
                if (ra == 255 && da >= 128)
                {
                    byte su(byte c) => da == 255 ? c : (byte)Math.Clamp(c * 255 / da, 0, 255);
                    if (Math.Abs(su(dr) - rr) <= 8 && Math.Abs(su(dg) - rg) <= 5 && Math.Abs(su(db) - rb) <= 8)
                        continue;                                // RGB565 量化容差内

                    // 1bit alpha 量化可解释类
                    var ratios = new[] { (dr + 1, rr + 1), (dg + 1, rg + 1), (db + 1, rb + 1) }
                        .Where(t => t.Item2 > 40)
                        .Select(t => (double)t.Item1 / t.Item2).ToList();
                    if (ratios.Count >= 2 && ratios.Max() <= 0.97 && ratios.Max() - ratios.Min() <= 0.15)
                    { quantShade++; continue; }                  // 丢弃的半透明 shade（均匀压暗）
                    if (Math.Max(rr, Math.Max(rg, rb)) <= 48 && Math.Max(dr, Math.Max(dg, db)) <= 160)
                    { quantBlack++; continue; }                  // 深色边缘硬化
                    if (Math.Min(dr, Math.Min(dg, db)) >= 140 && Math.Min(rr, Math.Min(rg, rb)) >= 140
                        && Math.Abs(dr - rr) <= 40 && Math.Abs(dg - rg) <= 40 && Math.Abs(db - rb) <= 40)
                    { quantSoft++; continue; }                   // 亮部软混合残差
                }
                diff++;
            }
        }
        return (diff, quantShade + quantBlack + quantSoft, compared, quantShade, quantBlack, quantSoft);
    }

    private static byte[] ReadPixels(SKBitmap bmp)
    {
        var pix = bmp.PeekPixels();
        Assert.NotNull(pix);
        var buf = new byte[bmp.Width * bmp.Height * 4];
        Marshal.Copy(pix!.GetPixels(), buf, 0, buf.Length);
        return buf;
    }

    // ═══════════════════════════════════════════════════════════
    // alpha-ab 产物（人眼验收，不断言）
    // ═══════════════════════════════════════════════════════════

    private static void WriteAlphaAb(SKBitmap direct, SKBitmap replay)
    {
        try
        {
            string dir = AlphaAbDir();
            Directory.CreateDirectory(dir);
            EncodePng(direct, Path.Combine(dir, "walk1-f0-direct.png"));
            EncodePng(replay, Path.Combine(dir, "walk1-f0-replay-1bit.png"));
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[alpha-ab] 写产物失败: {ex.Message}");
        }
    }

    private static void EncodePng(SKBitmap bmp, string path)
    {
        using var image = SKImage.FromBitmap(bmp);
        using var data = image.Encode(SKEncodedImageFormat.Png, 100);
        using var fs = File.Create(path);
        data.SaveTo(fs);
    }

    /// <summary>Server/MinipetServer.Tests/alpha-ab/（从测试输出目录向上找项目目录）。</summary>
    private static string AlphaAbDir()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null && dir.Name != "MinipetServer.Tests") dir = dir.Parent;
        dir ??= new DirectoryInfo(AppContext.BaseDirectory);
        return Path.Combine(dir.FullName, "alpha-ab");
    }

    // ═══════════════════════════════════════════════════════════
    // 信封布局（算法级规格 §二；头部长度自校准探测，不依赖硬编码）
    // ═══════════════════════════════════════════════════════════

    private sealed record EnvelopeLayout(int HeaderLen, int HashOffset, int PayloadOffset, int PayloadLen, int CrcOffset)
    {
        public static EnvelopeLayout Detect(byte[] pkg)
        {
            Assert.True(pkg.Length > 32, $"包过短 {pkg.Length}B");
            Assert.Equal("MPAK", Encoding.ASCII.GetString(pkg, 0, 4));
            // 头部定长但 magic 段 padding 未定稿：按「header + payload + 8B 尾」自校准
            foreach (int headerLen in new[] { 32, 40, 24, 48, 64 })
            {
                if (headerLen < 24 || headerLen + 8 > pkg.Length) continue;
                int payloadLen = BitConverter.ToInt32(pkg, headerLen - 8);
                if (payloadLen <= 0 || headerLen + payloadLen + 8 != pkg.Length) continue;
                return new EnvelopeLayout(headerLen, headerLen - 16, headerLen, payloadLen, headerLen + payloadLen);
            }
            throw new Xunit.Sdk.XunitException($"无法探测信封布局（总长 {pkg.Length}B）");
        }
    }

    /// <summary>重算 crc32c（覆盖 header+payload 全量，与 Mpak.Build 同口径）写回尾部（小端 u32）。</summary>
    private static void FixCrc(byte[] pkg, EnvelopeLayout env)
    {
        uint crc = System.IO.Hashing.Crc32C.HashToUInt64(pkg.AsSpan(0, env.CrcOffset));
        pkg[env.CrcOffset] = (byte)crc;
        pkg[env.CrcOffset + 1] = (byte)(crc >> 8);
        pkg[env.CrcOffset + 2] = (byte)(crc >> 16);
        pkg[env.CrcOffset + 3] = (byte)(crc >> 24);
    }
}
