using MiniPet.Export;
using MinipetServer.Config;

namespace MinipetServer.Services;

/// <summary>
/// clock_table 魔法值校准器（E9/R15，2026-09-26 落地）：
/// WZ 就绪后按 seed/clock_table.json 的 26 张图清单，把「WZ clock 世界锚点」换算成
/// 「烘焙视口内屏幕坐标」补进 config.Clock.MapOffsets。
/// 口径 v2（clock 居中相机）：含 clock 的地图烘焙相机以 clock 锚点为中心（ClampCamera
/// 夹取）——480 视口下地图中心相机多看不到 clock 面板（实测 18/26 出界），这 26 张
/// 售票处/码头图的存在意义就是显示时钟；与 AssetExporter.ExportMap 同口径。
/// - Calib &lt; 2：全量重算覆盖（旧口径产出/历史样例占位——本轮占位清零）；
///   Calib == 2：只补缺失条目，Web 手改的值永不覆盖（表 = 唯一事实源）。
/// - 烘焙视口取默认 480×480（真机 profile amoled216 同为 480×480）。
/// - 写入走 ConfigService.Update → Changed → DeviceManifestService.BumpAllRev
///   （设备下次 poll 拿新 clock_table，与 Web 手改同一条 rev+1 通路）。
/// 锚点优先读 WZ clock 节点（权威），缺失回退 seed 实测值（2026-09-24 定稿）。
/// </summary>
public static class ClockTableSeeder
{
    /// <summary>当前校准口径版本（改烘焙相机/换算公式时 +1，触发全量重算）。</summary>
    private const int CalibVersion = 2;

    public static void Run(WzService wz, ConfigService cfg, Microsoft.Extensions.Logging.ILogger logger)
    {
        try
        {
            var anchors = LoadSeedAnchors();
            if (anchors.Count == 0)
            {
                logger.LogWarning("[Clock] seed/clock_table.json 为空，跳过校准");
                return;
            }

            var profile = new DeviceProfile(); // 默认烘焙视口 480×480
            int vw = profile.ViewportW, vh = profile.ViewportH;
            var mapSvc = new MapService(wz, new CacheManager());

            var calibrated = new List<(string Id, int[] XY)>();
            foreach (var (mapId, seedAnchor) in anchors)
            {
                var map = mapSvc.LoadMap(mapId);
                if (map == null)
                {
                    logger.LogWarning("[Clock] 地图 {MapId} 加载失败，跳过", mapId);
                    continue;
                }

                // 锚点：WZ clock 节点优先，缺失回退 seed 实测值
                string mapRoot = MapService.GetMapWzPath(mapId);
                int cw = wz.GetIntProperty($"{mapRoot}/clock/x");
                int ch = wz.GetIntProperty($"{mapRoot}/clock/y");
                if (cw == 0 && ch == 0 && seedAnchor is { Length: 2 })
                {
                    cw = seedAnchor[0];
                    ch = seedAnchor[1];
                }

                // v2 换算：相机中心 = clock 锚点（夹取），与 ExportMap 烘焙一致
                var (camX, camY) = MapService.ClampCamera(map, cw, ch, 1f, vw, vh);
                int sx = (int)Math.Round(cw - camX + vw / 2f);
                int sy = (int)Math.Round(ch - camY + vh / 2f);
                calibrated.Add((mapId, new[] { sx, sy }));
            }

            var current = cfg.Current;
            bool fullRecalib = current.Clock.Calib < CalibVersion;
            var existing = current.Clock.MapOffsets;
            var toWrite = fullRecalib
                ? calibrated.ToList()
                : calibrated.Where(e => !existing.ContainsKey(e.Id)).ToList();

            if (!fullRecalib && toWrite.Count == 0)
            {
                logger.LogInformation("[Clock] clock_table 已是最新（口径 v{V}，{Count} 条清单全命中），无需校准",
                    CalibVersion, anchors.Count);
                return;
            }

            var (applied, errors) = cfg.Update(c =>
            {
                foreach (var (id, xy) in toWrite) c.Clock.MapOffsets[id] = xy;
                c.Clock.Calib = CalibVersion;
            });
            if (applied == null)
            {
                logger.LogWarning("[Clock] clock_table 校准写入失败：{Errors}", string.Join("；", errors));
                return;
            }
            logger.LogInformation("[Clock] clock_table 校准完成（口径 v{V}，{Mode}）：写入 {Count} 条 clock 居中相机坐标（视口 {W}×{H}，清单 {Total} 张）",
                CalibVersion, fullRecalib ? "全量重算" : "补缺失", toWrite.Count, vw, vh, anchors.Count);
        }
        catch (Exception ex)
        {
            logger.LogWarning("[Clock] clock_table 校准异常：{Message}", ex.Message);
        }
    }

    /// <summary>读 seed/clock_table.json（key=地图 id，value=世界锚点 [x,y]；跳过 _comment）。</summary>
    private static Dictionary<string, int[]> LoadSeedAnchors()
    {
        var result = new Dictionary<string, int[]>(StringComparer.Ordinal);
        var file = Path.Combine(DeviceProfile.FindSeedRoot(), "clock_table.json");
        if (!File.Exists(file)) return result;
        using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(file));
        foreach (var prop in doc.RootElement.EnumerateObject())
        {
            if (prop.NameEquals("_comment")) continue;
            if (prop.Value.ValueKind != System.Text.Json.JsonValueKind.Array) continue;
            var xy = prop.Value.EnumerateArray().Take(2).Select(e => e.GetInt32()).ToArray();
            if (xy.Length == 2) result[prop.Name] = xy;
        }
        return result;
    }
}
