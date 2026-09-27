using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;
using MiniPet.Export;
using MinipetServer.Config;

namespace MinipetServer.Services;

/// <summary>
/// 字体包服务（E12 字体链补链，2026-09-27）：固件菜单/气泡全部走 kind=4 FONT 包
/// （asset_dl kind_dir → minipet/font），而 FONT 此前只在 CLI 导出器里产出且默认
/// 单档 16px、DeviceId="default" —— 运行时没有任何组件给设备补字体，新设备 manifest
/// 里永远没有 FONT → 设备报 "font 1 not loaded"，菜单/气泡无字。
///
/// 本服务：确保 data/cache/export/{deviceId}/ 里有 16/24/32 三档 FONT 包并合并进该设备
/// manifest-assets.json（骨架抄 PaperdollPackService.EnsurePacked：per-device 锁串行 +
/// 读索引 → 幂等跳过 → 写 {hash}.mpak → 合并条目 → 回写索引）。
///
/// 字形来源优先级：
///   1) 出厂种子 Server/seed/fonts/font-{size}.json（4bpp 中间格式，脚本
///      scripts/fontpack/build-fonts.sh 从系统宋体预生成，16/24px 各 3892 字、32px 666 字）
///      → FontPackWriter.Build 打包。**容器内首选**：镜像里没有中文字体，Skia 现场
///      渲染 CJK 会全豆腐块，而种子 JSON 与字体无关、字节级确定（同 hash 幂等）。
///   2) 种子缺失 → FontPackWriter.RenderFontPack 现场栅格化（仅保证 ASCII/常用标点）。
///
/// 三档都是独立包（一档一 hash），设备按 manifest 条目全量拉取；同档位重打包（种子更新）
/// 时旧条目会被替换删除，避免同档位两个包同时下发（固件按字号装表，重复档位行为未定义）。
/// </summary>
public sealed class FontPackService
{
    /// <summary>三档字号（固件 UI 16/24/32；见 algorithm-asset-format.md §六）。</summary>
    public static readonly int[] DefaultSizes = { 16, 24, 32 };

    private readonly ServerPaths _paths;
    private readonly ConcurrentDictionary<string, object> _deviceLocks = new();

    public FontPackService(ServerPaths paths)
    {
        _paths = paths ?? throw new ArgumentNullException(nameof(paths));
    }

    /// <summary>
    /// 确保设备索引含指定档位的 FONT 包（缺省 16/24/32）。返回实际新写入的包数
    /// （0 = 全部已登记，幂等）。种子缺失/打包失败抛异常，由调用方记录（hello 侧只记事件）。
    /// 同步方法（打包 1~2 秒，纯 CPU + 磁盘；WZ 锁无关——字体不读 WZ）。
    /// </summary>
    public int EnsureFonts(string deviceId, IReadOnlyList<int>? sizes = null)
    {
        ValidateDeviceId(deviceId);
        var want = (sizes == null || sizes.Count == 0 ? DefaultSizes : sizes)
            .Where(s => s > 0).Distinct().OrderBy(s => s).ToList();
        if (want.Count == 0) return 0;

        lock (_deviceLocks.GetOrAdd(deviceId, _ => new object()))
        {
            var deviceDir = Path.Combine(_paths.ExportRoot, deviceId);
            Directory.CreateDirectory(deviceDir);
            var indexPath = Path.Combine(deviceDir, ManifestBuilder.AssetsManifestFileName);
            var root = ReadIndex(indexPath);
            var assetsObj = root["assets"] as JsonObject ?? new JsonObject();
            bool changed = false;
            int generated = 0;

            foreach (var size in want)
            {
                byte[] payload = BuildPayload(size);
                var (hash, file) = Mpak.BuildWithHash(MpakKind.Font, payload);
                string key = $"{hash:x16}";

                // 同档位旧条目（种子换代 / 换字符集）：先摘掉，防同字号两包同时下发
                var staleKeys = assetsObj
                    .Where(kv => IsFontOfSize(kv.Value, size))
                    .Select(kv => kv.Key)
                    .ToList();
                bool sameHashPresent = staleKeys.Contains(key, StringComparer.Ordinal);
                if (sameHashPresent)
                {
                    // 内容一致 → 保留本条目，只清同档位的**其它**旧条目（换字符集后的残留）
                    foreach (var k in staleKeys.Where(k => !string.Equals(k, key, StringComparison.Ordinal)))
                    {
                        assetsObj.Remove(k);
                        changed = true;
                    }
                    continue;
                }
                foreach (var k in staleKeys)
                {
                    assetsObj.Remove(k);
                    changed = true;
                }

                File.WriteAllBytes(Path.Combine(deviceDir, Mpak.HashFileName(hash)), file);
                assetsObj[key] = EntryOf(hash, payload.Length, size);
                changed = true;
                generated++;
                Console.WriteLine($"[FontPack] 设备 {deviceId} 生成 FONT {size}px 包 {key}（{payload.Length} B）");
            }

            if (changed)
            {
                root["assets"] = assetsObj;
                root["deviceId"] = deviceId;
                root["generated"] = DateTimeOffset.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'");
                File.WriteAllText(indexPath, root.ToJsonString(new JsonSerializerOptions
                {
                    WriteIndented = true,
                    Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping, // 中文 label 不转义
                }));
            }
            return generated;
        }
    }

    /// <summary>该设备索引里已登记的 FONT 档位（升序；诊断/幂等判定用，不打包）。</summary>
    public IReadOnlyList<int> PackedSizes(string deviceId)
    {
        try
        {
            var indexPath = Path.Combine(_paths.ExportRoot, deviceId, ManifestBuilder.AssetsManifestFileName);
            var root = ReadIndex(indexPath);
            if (root["assets"] is not JsonObject assets) return Array.Empty<int>();
            var sizes = new List<int>();
            foreach (var kv in assets)
            {
                if (kv.Value is not JsonObject e) continue;
                if (!string.Equals(e["kind"]?.GetValue<string>(), MpakKind.Font.DirName(), StringComparison.Ordinal)) continue;
                var s = e["size"];
                if (s == null) continue;
                // size 以数字写入；兼容历史/手写索引里的字符串形态
                if (s is JsonValue jv && jv.TryGetValue(out int iv)) sizes.Add(iv);
                else if (int.TryParse(s.ToString(), out var parsed)) sizes.Add(parsed);
            }
            return sizes.Distinct().OrderBy(s => s).ToList();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[FontPack] 读设备 {deviceId} 索引失败: {ex.Message}");
            return Array.Empty<int>();
        }
    }

    // ── 内部 ──────────────────────────────────────────────────────────────

    private static void ValidateDeviceId(string deviceId)
    {
        if (string.IsNullOrWhiteSpace(deviceId)) throw new ArgumentException("deviceId 不能为空", nameof(deviceId));
        if (deviceId.Contains('/') || deviceId.Contains('\\') || deviceId.Contains(".."))
            throw new ArgumentException($"deviceId 非法: {deviceId}", nameof(deviceId));
    }

    private static bool IsFontOfSize(JsonNode? node, int size)
    {
        if (node is not JsonObject e) return false;
        if (!string.Equals(e["kind"]?.GetValue<string>(), MpakKind.Font.DirName(), StringComparison.Ordinal)) return false;
        var s = e["size"];
        if (s == null) return false;
        if (s is JsonValue jv && jv.TryGetValue(out int iv)) return iv == size;
        return int.TryParse(s.ToString(), out var parsed) && parsed == size;
    }

    /// <summary>FONT 条目：字段口径对齐 ManifestBuilder.EntryToJson + size 元数据。</summary>
    private static JsonObject EntryOf(ulong hash, int bytes, int size)
    {
        return new JsonObject
        {
            ["kind"] = MpakKind.Font.DirName(),
            ["bytes"] = bytes,
            ["file"] = Mpak.HashFileName(hash),
            ["url"] = $"/api/device/asset/{hash:x16}",
            ["label"] = $"字体 {size}px",
            // size：数字节点（档位标注 + 重打包时的同档替换判定）；FONT 无 selector 字段
            //（不进设备选择器，对齐导出器 selector:null 口径）
            ["size"] = size,
        };
    }

    /// <summary>打包一档字号：种子 JSON 优先，缺失回退 Skia 现场渲染。</summary>
    private static byte[] BuildPayload(int size)
    {
        string seedFile = Path.Combine(DeviceProfile.FindSeedRoot(), "fonts", $"font-{size}.json");
        if (File.Exists(seedFile)) return BuildFromSeedJson(seedFile, size);

        Console.Error.WriteLine(
            $"[FontPack] 种子字体缺失（{seedFile}）→ 回退 Skia 现场渲染 {size}px（仅 ASCII/常用标点；CJK 需种子 JSON）");
        return FontPackWriter.RenderFontPack(size, FontPackWriter.DefaultCharset());
    }

    /// <summary>
    /// 种子 JSON（scripts/fontpack/convert.js 产物）→ kind=4 payload：字段命名对齐
    /// FontPackWriter.GlyphInfo；bitmap 为 4bpp hex（行按字节对齐、左像素在高半字节）。
    /// </summary>
    private static byte[] BuildFromSeedJson(string path, int size)
    {
        var seed = JsonSerializer.Deserialize<SeedFontFile>(File.ReadAllText(path), SeedJsonOpts)
                   ?? throw new InvalidDataException($"种子字体解析失败: {path}");
        if (seed.Glyphs == null || seed.Glyphs.Count == 0)
            throw new InvalidDataException($"种子字体无字形: {path}");

        var glyphs = new List<FontPackWriter.GlyphInfo>(seed.Glyphs.Count);
        foreach (var g in seed.Glyphs)
        {
            byte[] bitmap;
            try
            {
                bitmap = string.IsNullOrEmpty(g.Bitmap) ? Array.Empty<byte>() : Convert.FromHexString(g.Bitmap);
            }
            catch (FormatException)
            {
                throw new InvalidDataException($"种子字体 {path} 字形 U+{g.Unicode:X4} bitmap 非 hex");
            }
            glyphs.Add(new FontPackWriter.GlyphInfo
            {
                Unicode = g.Unicode,
                W = (ushort)Math.Clamp(g.W, 0, ushort.MaxValue),
                H = (ushort)Math.Clamp(g.H, 0, ushort.MaxValue),
                Advance = (byte)Math.Clamp(g.Advance, 0, 255),
                OffX = (sbyte)Math.Clamp(g.OffX, -128, 127),
                BearingY = (sbyte)Math.Clamp(g.BearingY, -128, 127),
                Bitmap4Bpp = bitmap,
            });
        }
        return FontPackWriter.Build((byte)size, glyphs);
    }

    private static JsonObject ReadIndex(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                var node = JsonNode.Parse(File.ReadAllText(path));
                if (node is JsonObject o) return o;
            }
        }
        catch { /* 损坏索引按空处理，下方重建 */ }
        return new JsonObject();
    }

    private static readonly JsonSerializerOptions SeedJsonOpts = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    /// <summary>Server/seed/fonts/font-{size}.json 的形态（scripts/fontpack/README.md §中间 JSON 格式）。</summary>
    private sealed class SeedFontFile
    {
        [JsonPropertyName("size_px")] public int SizePx { get; set; }
        [JsonPropertyName("bpp")] public int Bpp { get; set; }
        [JsonPropertyName("glyph_count")] public int GlyphCount { get; set; }
        [JsonPropertyName("glyphs")] public List<SeedGlyph>? Glyphs { get; set; }
    }

    private sealed class SeedGlyph
    {
        [JsonPropertyName("unicode")] public uint Unicode { get; set; }
        [JsonPropertyName("w")] public int W { get; set; }
        [JsonPropertyName("h")] public int H { get; set; }
        [JsonPropertyName("advance")] public int Advance { get; set; }
        [JsonPropertyName("off_x")] public int OffX { get; set; }
        [JsonPropertyName("bearing_y")] public int BearingY { get; set; }
        [JsonPropertyName("bitmap")] public string Bitmap { get; set; } = "";
    }
}
