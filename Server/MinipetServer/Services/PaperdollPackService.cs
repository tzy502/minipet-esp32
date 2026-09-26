using System.Collections.Concurrent;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using MiniPet.Export;
using MinipetServer.Config;

namespace MinipetServer.Services;

/// <summary>
/// 装扮资产包服务（2026-09-26 补链）：Web「应用到设备」下发的 petConfig 此前只存配置 +
/// bump rev，没人把它转成设备资产包 → 设备拉 manifest 发现 assets 没变 → 换装无效（根因）。
/// 本服务补上这段：petConfig → AssetExporter 装扮资产（PARTS 整包 + 各动作 LAYOUT +
/// fontTime）→ 写 {hash}.mpak 到 data/cache/export/{deviceId}/ → 合并 manifest-assets.json
/// （替换旧 selector=paperdoll 条目；clock 字体全设备同 hash 覆盖无害）。
/// 幂等：现有索引里已有同 appearanceHash 的装扮条目则跳过；同设备并发打包用 per-device
/// 锁串行（打包本体在 WZ 锁内本就串行，这里防重复写索引）。
/// </summary>
public sealed class PaperdollPackService
{
    private static readonly JsonSerializerOptions AppearanceJsonOpts = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    private readonly WzService _wz;
    private readonly ServerPaths _paths;
    private readonly ConcurrentDictionary<string, object> _deviceLocks = new();

    public PaperdollPackService(WzService wz, ServerPaths paths)
    {
        _wz = wz;
        _paths = paths;
    }

    /// <summary>
    /// 确保设备的 manifest-assets.json 含该 petConfig 对应的装扮资产。返回是否实际生成了新包。
    /// </summary>
    public bool EnsurePacked(string deviceId, JsonElement petConfig)
    {
        if (petConfig.ValueKind is not (JsonValueKind.Object)) throw new ArgumentException("petConfig 必须是外观 JSON 对象");
        var appearance = petConfig.Deserialize<Models.CharacterAppearance>(AppearanceJsonOpts)
                         ?? throw new InvalidDataException("petConfig 反序列化失败");
        lock (_deviceLocks.GetOrAdd(deviceId, _ => new object()))
        {
            var deviceDir = Path.Combine(_paths.ExportRoot, deviceId);
            Directory.CreateDirectory(deviceDir);
            var indexPath = Path.Combine(deviceDir, ManifestBuilder.AssetsManifestFileName);

            var exporter = new AssetExporter(_wz);
            var (hash, assets) = exporter.ExportAppearanceAssets(appearance);
            if (assets.Count == 0) throw new InvalidOperationException("装扮资产导出为空（WZ 数据缺失？）");

            // 幂等：现有索引已含同 appearanceHash 的装扮 → 无需重打
            var root = ReadIndex(indexPath);
            if (HasAppearance(root, hash))
            {
                return false;
            }

            // 替换语义：移除旧 selector=paperdoll 条目（旧装扮包不再被 manifest 引用）
            var assetsObj = root["assets"] as JsonObject ?? new JsonObject();
            var stale = assetsObj.Where(kv => (kv.Value as JsonObject)?["selector"]?.GetValue<string>() == "paperdoll")
                                 .Select(kv => kv.Key).ToList();
            foreach (var k in stale) assetsObj.Remove(k);

            // 写 mpak + 合并新条目
            foreach (var a in assets)
            {
                File.WriteAllBytes(Path.Combine(deviceDir, a.FileName), a.Bytes);
                assetsObj[$"{a.Hash:x16}"] = EntryOf(a);
            }
            root["assets"] = assetsObj;
            root["generated"] = DateTimeOffset.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'");
            File.WriteAllText(indexPath, root.ToJsonString(new JsonSerializerOptions
            {
                WriteIndented = true,
                Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
            }));
            return true;
        }
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

    private static bool HasAppearance(JsonObject root, string hash)
    {
        if (root["assets"] is not JsonObject ao) return false;
        foreach (var kv in ao)
            if ((kv.Value as JsonObject)?["appearanceHash"]?.GetValue<string>() == hash) return true;
        return false;
    }

    /// <summary>资产条目 → manifest JSON（对齐 ManifestBuilder.EntryToJson 字段口径）。</summary>
    private static JsonObject EntryOf(ExportedAsset a)
    {
        var o = new JsonObject
        {
            // 固件 asset_dl kind_dir 是 strcmp 大写白名单（PARTS/LAYOUT/BGMAP/FONT/AUDIO_META），
            // 小写会被设备登记元数据但永不下载（2026-09-26 接缝审查修正）
            ["kind"] = a.Kind.ToString().ToUpperInvariant(),
            ["bytes"] = a.ByteCount,
            ["file"] = a.FileName,
            ["url"] = $"/api/device/asset/{a.Hash:x16}",
            ["label"] = a.Label,
        };
        if (!string.IsNullOrEmpty(a.Selector)) o["selector"] = a.Selector;
        foreach (var (k, v) in a.Extra)
        {
            if (v is string sv) o[k] = sv;
            else if (v is int[] ia) { var arr = new JsonArray(); foreach (var i in ia) arr.Add(i); o[k] = arr; }
            else if (v != null) o[k] = v.ToString() ?? "";
        }
        return o;
    }
}
