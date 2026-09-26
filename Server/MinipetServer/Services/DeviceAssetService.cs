using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Nodes;
using MiniPet.Export;
using MinipetServer.Config;

namespace MinipetServer.Services;

/// <summary>
/// 设备资产登记服务（2026-09-26 E7 补链）：把地图 / 怪物NPC 素材打包登记进设备 manifest
/// ——此前只有纸装扮有这条链路（PaperdollPackService）。模式对齐 PaperdollPackService：
/// AssetExporter 出资产 → 写 {hash}.mpak 到 data/cache/export/{deviceId}/ → 合并
/// manifest-assets.json；per-device 锁串行化（防同设备并发重复写索引）；幂等键 =
/// selector + 业务 id（map: 条目 extra.map==mapId；npc: extra.entity=="npc:{npcId}"），
/// 已登记则跳过不再重打。
/// 与纸装扮的语义差异：地图/NPC 是**累积收藏**——合并时**不删旧条目**（同 hash 覆盖无害），
/// 与装扮的「替换旧 selector=paperdoll」不同。
/// 字段口径：kind 用固件 asset_dl kind_dir 白名单的大写形态（"PARTS"/"LAYOUT"/"BGMAP"…，
/// 固件 strcmp 精确匹配，"Parts"/"parts" 会被拒收不下载）；selector 用小写
/// （"map"/"npc"，同固件 selector 比对口径）；其余字段（bytes/file/url/label/entity/
/// action/map/thumb/bounds/origin）对齐 ManifestBuilder.EntryToJson。
/// </summary>
public sealed class DeviceAssetService
{
    private readonly WzService _wz;
    private readonly ServerPaths _paths;
    private readonly ConcurrentDictionary<string, object> _deviceLocks = new();

    public DeviceAssetService(WzService wz, ServerPaths paths)
    {
        _wz = wz ?? throw new ArgumentNullException(nameof(wz));
        _paths = paths ?? throw new ArgumentNullException(nameof(paths));
    }

    /// <summary>
    /// 确保设备的 manifest-assets.json 已登记该地图资产包（BGMAP + 条带小 PARTS + 缩略图）。
    /// 返回是否实际生成新包（false = 索引已有该地图，幂等跳过）。
    /// 地图数据缺失/导出为空抛异常，由上层记录。
    /// 同步方法（打包本体在 WzService 内部锁内串行，同 EnsurePacked 口径；
    /// 需要异步语义由调用方 Task.Run 放后台线程）。
    /// </summary>
    public bool EnsureMapAsync(string deviceId, string mapId)
    {
        ValidateIds(deviceId, mapId, "地图 id");
        mapId = mapId.Trim();
        lock (_deviceLocks.GetOrAdd(deviceId, _ => new object()))
        {
            var deviceDir = DeviceDir(deviceId);
            var root = ReadIndex(Path.Combine(deviceDir, ManifestBuilder.AssetsManifestFileName));
            if (HasEntry(root, selector: "map", key: "map", value: mapId)) return false;

            var warnings = new List<string>();
            var assets = new AssetExporter(_wz).ExportMapAssets(mapId, warnings);
            MergeAndWrite(deviceDir, root, assets);
            return true;
        }
    }

    /// <summary>
    /// 确保设备的 manifest-assets.json 已登记该 NPC 资产包（PARTS 整包 + 每动作 LAYOUT）。
    /// 返回是否实际生成新包（false = 索引已有该 NPC，幂等跳过）。
    /// NPC 数据缺失/条带导出失败抛异常，由上层记录。
    /// 同步方法（打包本体在 WzService 内部锁内串行，同 EnsurePacked 口径；
    /// 需要异步语义由调用方 Task.Run 放后台线程）。
    /// </summary>
    public bool EnsureNpcAsync(string deviceId, string npcId)
    {
        ValidateIds(deviceId, npcId, "NPC id");
        npcId = npcId.Trim();
        lock (_deviceLocks.GetOrAdd(deviceId, _ => new object()))
        {
            var deviceDir = DeviceDir(deviceId);
            var root = ReadIndex(Path.Combine(deviceDir, ManifestBuilder.AssetsManifestFileName));
            if (HasEntry(root, selector: "npc", key: "entity", value: $"npc:{npcId}")) return false;

            var warnings = new List<string>();
            var assets = new AssetExporter(_wz).ExportNpcAssets(npcId, warnings);
            MergeAndWrite(deviceDir, root, assets);
            return true;
        }
    }

    /// <summary>
    /// 写 mpak + 合并索引（**不删旧条目**——地图/NPC 累积收藏语义，与装扮替换语义不同；
    /// 同 hash 覆盖无害）。调用方须已持有该设备的锁。
    /// </summary>
    private static void MergeAndWrite(string deviceDir, JsonObject root, List<ExportedAsset> assets)
    {
        Directory.CreateDirectory(deviceDir);
        var assetsObj = root["assets"] as JsonObject ?? new JsonObject();
        foreach (var a in assets)
        {
            File.WriteAllBytes(Path.Combine(deviceDir, a.FileName), a.Bytes);
            assetsObj[$"{a.Hash:x16}"] = EntryOf(a);
        }
        root["assets"] = assetsObj;
        root["generated"] = DateTimeOffset.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'");
        File.WriteAllText(Path.Combine(deviceDir, ManifestBuilder.AssetsManifestFileName),
            root.ToJsonString(new JsonSerializerOptions
            {
                WriteIndented = true,
                Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping, // 中文 label 不转义
            }));
    }

    private string DeviceDir(string deviceId) => Path.Combine(_paths.ExportRoot, deviceId);

    private static void ValidateIds(string deviceId, string id, string idName)
    {
        if (string.IsNullOrWhiteSpace(deviceId)) throw new ArgumentException("deviceId 不能为空", nameof(deviceId));
        if (deviceId.Contains('/') || deviceId.Contains('\\') || deviceId.Contains(".."))
            throw new ArgumentException($"deviceId 非法: {deviceId}", nameof(deviceId));
        if (string.IsNullOrWhiteSpace(id)) throw new ArgumentException($"{idName}不能为空", nameof(id));
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

    /// <summary>
    /// 幂等判定：索引里已有 selector 相同且 extra 字段 key==value 的条目。
    /// 主条目 kind 必须是固件白名单大写（map→BGMAP / npc→PARTS）：旧版小写条目视为未登记，
    /// 走重打覆盖——否则坏索引永不被纠正，设备永不下载（同 PaperdollPackService.HasAppearance）。
    /// </summary>
    private static bool HasEntry(JsonObject root, string selector, string key, string value)
    {
        var primaryKind = selector == "map" ? "BGMAP" : "PARTS";
        if (root["assets"] is not JsonObject ao) return false;
        foreach (var kv in ao)
        {
            if (kv.Value is not JsonObject e) continue;
            if (!string.Equals(e["selector"]?.GetValue<string>(), selector, StringComparison.Ordinal)) continue;
            if (!string.Equals(e[key]?.GetValue<string>(), value, StringComparison.Ordinal)) continue;
            if (string.Equals(e["kind"]?.GetValue<string>(), primaryKind, StringComparison.Ordinal)) return true;
        }
        return false;
    }

    /// <summary>
    /// 资产条目 → manifest JSON（字段口径对齐 ManifestBuilder.EntryToJson，差异仅 kind 大小写：
    /// 固件 asset_dl 用 strcmp 白名单匹配 kind_dir，必须大写 "PARTS"/"LAYOUT"/"BGMAP"…；
    /// 未知 kind（如 THUMB）设备端按元数据登记、不下载，前向兼容）。
    /// </summary>
    private static JsonObject EntryOf(ExportedAsset a)
    {
        var o = new JsonObject
        {
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
