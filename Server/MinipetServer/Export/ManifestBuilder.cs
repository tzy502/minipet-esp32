using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace MiniPet.Export;

/// <summary>
/// manifest 合成（算法规格 §八）：
/// - manifest-assets.json：导出产物索引（hash → {kind,bytes,url,label,thumb,selector,entity,action,map}），
///   供 Web/服务端素材管理与按设备隔离的缓存下发；
/// - manifest.json：设备 manifest = assets + firmware + clock_table，proto=1、rev 每设备单调递增
///   （已存在 manifest.json 时 rev+1）。
/// selector: map|paperdoll|npc|clock —— 无 selector 字段的条目不进设备选择器（R7）；
/// THUMB 为缩略图附件（PNG 文件），设备按 kind 白名单可不拉取。
/// </summary>
public static class ManifestBuilder
{
    public const string AssetsManifestFileName = "manifest-assets.json";
    public const string ManifestFileName = "manifest.json";
    public const int Proto = 1;

    private static readonly JsonSerializerOptions JsonOpts = new() { WriteIndented = true };

    /// <summary>单条资产 → JSON 对象（assets 字典的 value）。</summary>
    private static JsonObject EntryToJson(ExportedAsset a)
    {
        var o = new JsonObject
        {
            ["kind"] = a.Kind.ToString(),
            ["bytes"] = a.ByteCount,
            ["url"] = $"/api/device/asset/{a.Hash:x16}",
            ["label"] = a.Label,
        };
        if (!string.IsNullOrEmpty(a.Selector)) o["selector"] = a.Selector;
        foreach (var (k, v) in a.Extra)
        {
            // 字符串直收；int[]（如 LAYOUT 条目的 "bounds":[w,h] 画布包围盒）→ JSON 数组；
            // 其余标量 ToString（避免 JsonValue.Create<T> 的反射 resolver 依赖，net9 源码生成上下文不可用）
            if (v is string sv) o[k] = sv;
            else if (v is int[] ia)
            {
                var arr = new JsonArray();
                foreach (var i in ia) arr.Add(i);
                o[k] = arr;
            }
            else if (v != null) o[k] = v.ToString() ?? "";
        }
        return o;
    }

    /// <summary>写 manifest-assets.json，返回路径。</summary>
    public static void WriteAssetsManifest(string deviceDir, string deviceId, IReadOnlyList<ExportedAsset> assets, out string path)
    {
        var root = new JsonObject
        {
            ["deviceId"] = deviceId,
            ["generated"] = DateTimeOffset.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'"),
            ["proto"] = Proto,
            ["assets"] = BuildAssetsObject(assets),
        };
        path = Path.Combine(deviceDir, AssetsManifestFileName);
        File.WriteAllText(path, root.ToJsonString(JsonOpts));
    }

    private static JsonObject BuildAssetsObject(IReadOnlyList<ExportedAsset> assets)
    {
        var dict = new JsonObject();
        foreach (var a in assets.OrderBy(x => x.Hash))
        {
            dict[$"{a.Hash:x16}"] = EntryToJson(a);
        }
        return dict;
    }

    /// <summary>
    /// 写 manifest.json（设备 diff 依据）。rev 语义（R7/R8）：每设备单调递增——目标目录已有
    /// manifest.json 时在其 rev 上 +1，否则从 1 起。返回写入的 rev。
    /// </summary>
    public static int WriteFullManifest(string deviceDir, IReadOnlyList<ExportedAsset> assets,
        string firmwareVer, IReadOnlyDictionary<string, int[]> clockTable, out string path)
    {
        int rev = 1;
        string existing = Path.Combine(deviceDir, ManifestFileName);
        if (File.Exists(existing))
        {
            try
            {
                var prev = JsonNode.Parse(File.ReadAllText(existing));
                var prevRev = prev?["rev"]?.GetValue<int>();
                if (prevRev.HasValue) rev = prevRev.Value + 1;
            }
            catch { /* 损坏的旧 manifest → 从 1 重写 */ }
        }

        var manifest = new JsonObject
        {
            ["proto"] = Proto,
            ["rev"] = rev,
            ["assets"] = BuildAssetsObject(assets),
            ["firmware"] = new JsonObject
            {
                ["ver"] = firmwareVer,
                ["url"] = $"/api/device/firmware/{firmwareVer}.bin",
            },
        };
        if (clockTable.Count > 0)
        {
            var ct = new JsonObject();
            foreach (var (mapId, xy) in clockTable)
            {
                ct[mapId] = new JsonArray(xy[0], xy[1]);
            }
            manifest["clock_table"] = ct;
        }
        else
        {
            manifest["clock_table"] = new JsonObject();
        }

        path = Path.Combine(deviceDir, ManifestFileName);
        File.WriteAllText(path, manifest.ToJsonString(JsonOpts));
        return rev;
    }
}
