using System.IO;
using MinipetServer.Config;
using MinipetServer.Device;
using MinipetServer.Health;
using MinipetServer.Manifest;
using MinipetServer.Services;

namespace MinipetServer.Api;

/// <summary>
/// Web「选镜头」（服务端选相机机位）端点，前缀 /api/admin/devices/{id}/camera。
///
/// 背景：设备渲染的是"整图地图"，相机 = 可见窗口左上角的世界坐标（世界 1x，屏 2x ⇒
/// 可见窗口 = 屏宽/2 世界像素，480 屏 = 240×240），可平移范围 x∈[0,vw-240]、y∈[0,vh-240]。
/// 设备上只能用手指拖，体验差且看不见"整张图里选的是哪"——所以选择/预览在 Web 做，
/// 记录在服务端（用户口径：服务端为主口径，设备 NVS/本地卡只是断网辅助）。
///
/// 端点一览（全部局域网信任模型，v1 无鉴权，同 AdminEndpoints）：
///   GET  /maps                          → 该设备已有 BGMAP 地图清单（vw/vh 读 BGMAP 包头）+ 已记录机位
///   GET  /maps/{mapId}/preview?maxW=    → 整图预览 PNG（磁盘缓存；框外半黑由 Web 叠加）
///   GET  /maps/{mapId}/viewport?x=&y=   → 该机位下"设备实际会看到的那一屏" PNG（1x，Web 按 2x 显示）
///   PUT  /camera  {mapId,x,y}           → 记录坐标（幂等 upsert 到 data/camera-positions.json）
///   GET  /camera                        → 全部机位（Web 重新打开时回填；/maps 里也带一份）
///   DELETE /maps/{mapId}                → 从该设备素材清单删除这张图（BGMAP + 仅它引用的派生
///                                         素材）并 BumpRev；正在使用的图 409 拒绝（见端点注释）
/// 下发设备仍走既有 POST /api/admin/devices/{id}/command 的 type=cam 通道（AdminEndpoints）。
/// </summary>
public static class CameraEndpoints
{
    public static void Map(WebApplication app)
    {
        var g = app.MapGroup("/api/admin");

        /// <summary>
        /// 该设备已有的 BGMAP 地图清单。数据源 = 设备导出目录的 manifest-assets.json
        /// （selector=map &amp; kind=BGMAP），尺寸从 **BGMAP 包头**解（vw/vh 与设备实际拿到的那份包
        /// 严格一致），不硬编码、不猜 WZ bbox。同时带回已记录机位与 lastMapId（Web 回填用）。
        /// </summary>
        g.MapGet("/devices/{id}/camera/maps", (string id, DeviceRegistry reg, CameraService camera) =>
        {
            if (reg.Get(id) == null) return NotFoundDevice(id);
            var (maps, lastMapId) = camera.ListMaps(id);
            return Results.Json(new
            {
                deviceId = id,
                total = maps.Count,
                lastMapId,
                maps,
                note = "地图清单来自该设备 manifest 的 BGMAP 条目；vw/vh 读 BGMAP 包头；"
                       + "viewport=full 才可平移（window 包是烘死的 240×240 一屏）",
            });
        });

        /// <summary>全部已记录机位（deviceId → mapId → {x,y,vw,vh,updatedUtc}）。</summary>
        g.MapGet("/devices/{id}/camera", (string id, DeviceRegistry reg, CameraPlanStore store) =>
        {
            if (reg.Get(id) == null) return NotFoundDevice(id);
            var (lastMapId, positions) = store.Get(id);
            return Results.Json(new
            {
                deviceId = id,
                lastMapId,
                positions,
                file = "data/camera-positions.json",
                note = "服务端为主口径：设备 NVS 里的机位只是断网辅助，以这里为准",
            });
        });

        /// <summary>
        /// 整图预览 PNG。maxW = 长边像素上限（默认 1440，夹取 [240,4096]）。
        /// 响应头带 X-Map-Vw/X-Map-Vh/X-Preview-Width，Web 据此把"世界坐标 ↔ 显示像素"
        /// 换算成取景框位置（不靠图片像素反推，避免缩放取整误差）。
        /// </summary>
        g.MapGet("/devices/{id}/camera/maps/{mapId}/preview", (string id, string mapId, DeviceRegistry reg, CameraService camera,
            WzService wz, int? maxW = null, bool? refresh = null) =>
        {
            if (reg.Get(id) == null) return NotFoundDevice(id);
            if (!wz.IsLoaded) return Results.Json(new { error = "WZ 未加载（到「设置」页配置后重试）" }, statusCode: 503);
            var map = camera.FindMap(id, mapId);
            if (map == null) return Results.Json(new
            {
                error = $"设备 {id} 未登记地图 {mapId}（先到「素材推送」把地图推到该设备）",
            }, statusCode: 400);

            var png = camera.RenderPreviewPng(id, map, maxW ?? CameraService.DefaultPreviewMaxW, refresh == true);
            if (png == null) return Results.Json(new { error = $"地图 {mapId} 渲染失败（WZ 数据缺失？）" }, statusCode: 500);
            return Png(png, resp =>
            {
                resp.Headers["X-Map-Vw"] = map.Vw.ToString();
                resp.Headers["X-Map-Vh"] = map.Vh.ToString();
                resp.Headers["X-Map-Viewport"] = map.Viewport;
                // 预览是"整图 → 显示像素"的等比缩放：Web 用 naturalWidth/vw 算比例即可；
                // 这里额外给出服务端实际渲染宽度，省得等图片加载完才能摆框。
                resp.Headers["X-Preview-Width"] = PreviewWidth(map.Vw, maxW ?? CameraService.DefaultPreviewMaxW).ToString();
            });
        });

        /// <summary>
        /// 机位视口 PNG（设备实际会看到的那一屏；窗口包按导出相机取景）。
        /// x/y 越界自动夹取（与设备 render_cam_set 内部夹取同口径），返回值里给出夹取后的实际值。
        /// Web 还会带一个 h=&lt;BGMAP content_hash&gt; 参数——本端点**不读它**，只用来让浏览器
        /// 在地图被重推后自然换 URL（否则会吃旧缓存，画面与设备对不上）。
        /// </summary>
        g.MapGet("/devices/{id}/camera/maps/{mapId}/viewport", (string id, string mapId, DeviceRegistry reg, CameraService camera,
            WzService wz, int? x = null, int? y = null) =>
        {
            if (reg.Get(id) == null) return NotFoundDevice(id);
            if (!wz.IsLoaded) return Results.Json(new { error = "WZ 未加载（到「设置」页配置后重试）" }, statusCode: 503);
            var map = camera.FindMap(id, mapId);
            if (map == null) return Results.Json(new
            {
                error = $"设备 {id} 未登记地图 {mapId}（先到「素材推送」把地图推到该设备）",
            }, statusCode: 400);

            var (cx, cy) = CameraService.ClampCamera(map, x ?? 0, y ?? 0);
            var png = camera.RenderViewportPng(id, map, cx, cy);
            if (png == null) return Results.Json(new { error = $"地图 {mapId} 渲染失败（WZ 数据缺失？）" }, statusCode: 500);
            return Png(png, resp =>
            {
                resp.Headers["X-Cam-X"] = cx.ToString();
                resp.Headers["X-Cam-Y"] = cy.ToString();
                resp.Headers["X-Cam-Max-X"] = map.MaxX.ToString();
                resp.Headers["X-Cam-Max-Y"] = map.MaxY.ToString();
                resp.Headers["X-Cam-Win-W"] = map.WinW.ToString();
                resp.Headers["X-Cam-Win-H"] = map.WinH.ToString();
                resp.Headers["X-Cam-Clamped"] = (cx != (x ?? 0) || cy != (y ?? 0)) ? "1" : "0";
            });
        });

        /// <summary>
        /// 从该设备素材清单里**删除一张地图**（Web「选镜头」卡片地图列表的删除按钮）。
        ///
        /// 语义：摘掉 manifest-assets.json 里这张图的 BGMAP 主条目 + 仅它引用的派生素材
        /// （条带 PARTS / 缩略图，判据与引用计数见 DeviceAssetService.DeleteMap 注释），
        /// 然后 BumpRev —— 设备长轮询被唤醒 → 拉新清单 → 对账剪除本地条目
        /// （固件 asset_dl.c prune_stale_locked：清单里没有的条目一律剪掉，菜单里随之消失）。
        /// 磁盘 mpak 不动（设备侧 LRU 自己淘汰；服务端留文件可重推，删除不可逆故不做）。
        ///
        /// 保护：**设备当前正在使用的那张图拒绝删除**（409，中文原因），见下方口径注释。
        /// ?force=true 可越过保护（页面在二次确认后自动带上）——留给"清单里只剩这一张、又不想要它"
        /// 的场景：设备会回落到清单里的其它图，清单空了则背景为黑（返回体 warning 会说明）。
        /// 幂等：清单里没有这张图 → 200 { removed:false, idempotent:true }（不是 500，也不必 404——
        /// 页面重试/重复点击都应当成功）。设备不存在 404、mapId 空 400。
        /// </summary>
        g.MapDelete("/devices/{id}/camera/maps/{mapId}", (string id, string mapId, DeviceRegistry reg,
            CameraService camera, CameraPlanStore store, DeviceAssetService assets, DeviceManifestService mfst,
            DeviceEventLog eventLog, bool? force = null) =>
        {
            if (reg.Get(id) == null) return NotFoundDevice(id);
            var mid = mapId?.Trim();
            if (string.IsNullOrEmpty(mid))
                return Results.Json(new { error = "mapId 必填（路径参数，取自 GET …/camera/maps 的 mapId）" },
                    statusCode: 400);

            /* ══ 「当前正在使用的图」保护（用户口径：删了会没有背景可渲染）══════════════
             * 服务端现成口径只有一份：data/camera-positions.json 的 lastMapId，写它的是
             *   ① PUT …/camera（「选镜头」上送机位 = 这张图正在被设备渲染）；
             *   ② POST …/push {switch:true}（AdminEndpoints 切图后补记一笔，2026-10-02 加）。
             * 设备端**不回报** active_map（它只把 active_map 写本地 /sdcard/minipet/manifest.json），
             * 所以这是服务端能拿到的最强信号，局限必须在返回体里讲清：
             *   · 服务端从未记录过切图/机位（新设备、换服务端实例）→ 判不出来，放行；
             *   · 用户在设备菜单上手动换过图 → 服务端不知道，放行。
             * 兜底：固件在"活动图不在清单里"时会回落到清单首图（1.85B 的 fallback 装载），
             * 不会永久黑屏；所以这里选**拒绝**（提示先切图）而不是自动切默认图 ——
             * 切图是另一条指令链路（要等设备下完包再发 SET_MAP），混进删除请求里会让人
             * 以为"已经切好了"，明确让用户先切更不容易出错。
             * 返回体 note 里也带上这句口径，便于页面/排障看到判据。 */
            var (lastMapId, _) = store.Get(id);
            string activeNote = string.IsNullOrEmpty(lastMapId)
                ? "服务端没有该设备的切图/机位记录（口径：最后设过机位或最后推送切图的那张），本次未做占用保护"
                : $"服务端口径的当前图 = {lastMapId}（最后设过机位 / 最后推送切图的那张）";
            bool forced = force == true;
            if (!forced && !string.IsNullOrEmpty(lastMapId) && string.Equals(lastMapId, mid, StringComparison.Ordinal))
            {
                var cur = camera.FindMap(id, mid);
                var curLabel = cur != null ? $"{cur.Label}（{mid}）" : mid;
                Console.WriteLine($"[MapDelete] 设备 {id} 拒绝删除当前正在使用的图 {mid}（lastMapId 口径）");
                return Results.Json(new
                {
                    error = $"地图 {curLabel}是设备当前正在使用的图（{activeNote}）——删了会没有背景可渲染。"
                            + "请先切到别的图：到「素材推送」把目标地图推一次（缺省勾选自动切图即会切过去），"
                            + "或在「选镜头」里给目标图上送一次机位，然后再删这张。"
                            + "（确实要删：DELETE …?force=true —— 设备会回落到清单里的其它图，清单空了则背景为黑）",
                    code = "map_in_use",
                    deviceId = id,
                    mapId = mid,
                    activeMapId = lastMapId,
                    forceHint = "加 ?force=true 可强制删除（页面在二次确认后会自动带上）",
                }, statusCode: 409);
            }
            if (forced)
                Console.WriteLine($"[MapDelete] 设备 {id} **强制删除**当前正在使用的图 {mid}（force=true，用户二次确认）");

            var result = assets.DeleteMap(id, mid);
            long rev = mfst.GetCurrentRev(id);
            if (result.Removed)
            {
                // 清单变了 → rev+1（BumpRev 内部还会往指令队列塞 manifest 唤醒指令，
                // 长轮询立即返回；设备收到新清单即对账剪除本地条目）
                rev = mfst.BumpRev(id, $"删除地图 {mid}（用户操作）");
                eventLog.Append(id, $"删除地图 {mid}（{result.Label}）：摘除 BGMAP 1 条 + 派生素材 "
                                    + $"{result.RemovedAssets.Count} 条，物理删除 {result.FilesDeleted} 个文件"
                                    + $"（{result.BytesFreed / 1024}KB），剩余 {result.RemainingMaps} 张"
                                    + "；设备端下次同步即对账删文件");
                Console.WriteLine($"[MapDelete] 设备 {id} 删除地图 {mid}：派生 {result.RemovedAssets.Count} 条"
                                  + $"（保留 {result.KeptAssets.Count}），剩余 {result.RemainingMaps} 张，rev={rev}");
            }
            else
            {
                Console.WriteLine($"[MapDelete] 设备 {id} 删除地图 {mid}：清单里没有这张图 → 幂等返回（rev={rev} 不变）");
            }

            return Results.Json(new
            {
                ok = true,
                deviceId = id,
                mapId = mid,
                label = result.Label,
                removed = result.Removed,
                /** true = 该图本来就不在清单里（重复删除/已被删过）：成功但什么都没做 */
                idempotent = !result.Removed,
                bgmapHash = result.BgmapHash,
                removedAssets = result.RemovedAssets,
                removedCount = result.RemovedAssets.Count,
                /** 判定为仍被引用而保留的候选（正常为空；非空说明有另一张图共用了同内容素材） */
                keptAssets = result.KeptAssets,
                keptCount = result.KeptAssets.Count,
                remainingMaps = result.RemainingMaps,
                /** 【2026-10-02 用户口径】本次**物理删除**的包文件数 + 释放字节（服务端导出目录） */
                filesDeleted = result.FilesDeleted,
                bytesFreed = result.BytesFreed,
                manifestRev = rev,
                activeMapId = lastMapId,
                /** true = 本次是"强制删除正在使用的图"（force=true，页面二次确认后才带） */
                forced,
                warning = result.RemainingMaps == 0
                    ? "该设备清单里已经没有任何地图：设备下次同步后没有 BGMAP 可渲染"
                      + "（固件会尝试回落默认图 000010000，那份没登记则背景为黑）"
                    : null,
                note = result.Removed
                    ? "已从该设备素材清单移除该地图及其专用素材（BGMAP 主包 + 条带 PARTS + 缩略图）；"
                      + "磁盘 .mpk 保留（设备侧按清单对账剪除条目，文件由其 LRU 淘汰）；"
                      + "设备下次同步（长轮询 ≤55s，已被 rev 唤醒）后本地清单里不再有这张图。"
                      + (forced ? "本次为强制删除正在使用的图：设备会回落到清单里的其它图，清单已空则背景为黑。" : "")
                    : "该图本来就不在该设备清单里（幂等：无需删除）。"
            });
        });

        /// <summary>
        /// 记录机位（幂等 upsert）。body { mapId, x, y }；坐标按该图 vw/vh 夹取后落盘
        /// （服务端不存越界值——设备侧也会夹，两边存不一样的值只会让"回填"看起来漂移）。
        /// 返回落盘后的实际值，前端以返回值回显（不乐观看待自己发出去的数）。
        /// </summary>
        g.MapPut("/devices/{id}/camera", (string id, CameraSaveRequest body, DeviceRegistry reg, CameraService camera,
            CameraPlanStore store, DeviceEventLog eventLog,
            DeviceAssetService assets, DeviceManifestService mfst, CommandQueue queue,
            MinipetServer.Config.ServerPaths paths, HealthReport health) =>
        {
            if (reg.Get(id) == null) return NotFoundDevice(id);
            var mapId = body?.MapId?.Trim();
            if (string.IsNullOrEmpty(mapId))
                return Results.Json(new { error = "mapId 必填（取自 GET …/camera/maps 的 mapId）" }, statusCode: 400);
            if (body?.X == null || body.Y == null)
                return Results.Json(new { error = "x / y 必填（整图世界坐标，0 = 地图 bbox 左上角）" }, statusCode: 400);

            var map = camera.FindMap(id, mapId);
            if (map == null) return Results.Json(new
            {
                error = $"设备 {id} 未登记地图 {mapId}（先到「素材推送」把地图推到该设备）",
            }, statusCode: 400);

            /* ══ 【窗口包不夹取 2026-10-01 · 真机 bug 修复】══════════════════════════
             * 现象（本地实例实测）：对 240×240 窗口包上送 (1377,286)，ClampCamera 用的是
             * maxX = max(0, vw-240) = 0 → **用户坐标被夹成 (0,0)**，随后即使自动整图重推成功，
             * 下发的也是 (0,0) —— 用户要的机位直接丢失（日志："请求值 1377,286 已按图尺寸夹取"）。
             * 修法：窗口包（viewport != full）**先原样保存用户坐标**（此时地图还没有可平移
             * 余量，夹取没有意义）；等自动整图重推完成、拿到真实 vw/vh 后，在后台任务里
             * 重新夹取 → 更新记录 → 下发设备。整图包照旧即时夹取（行为不变）。 */
            bool isWindowMap = !string.Equals(map.Viewport, "full", StringComparison.OrdinalIgnoreCase);
            var (cx, cy) = isWindowMap
                ? (body.X.Value, body.Y.Value)             // 窗口包：原样保留，重推后再夹
                : CameraService.ClampCamera(map, body.X.Value, body.Y.Value);
            var saved = store.Save(id, mapId, cx, cy, map.Vw, map.Vh);
            eventLog.Append(id, isWindowMap
                ? $"机位记录（窗口包原样保留）：地图 {mapId} x={cx} y={cy} —— 待整图重推后按真实尺寸夹取"
                : $"机位记录：地图 {mapId} x={cx} y={cy}（范围 x[0,{map.MaxX}] y[0,{map.MaxY}]）");
            Console.WriteLine($"[Camera] 设备 {id} 地图 {mapId} 机位已记录 → ({cx},{cy})"
                              + (isWindowMap
                                  ? "（窗口包：原样保留，不夹取；整图重推后再夹取下发）"
                                  : (cx != body.X.Value || cy != body.Y.Value
                                      ? $"（请求值 {body.X},{body.Y} 已按图尺寸夹取）" : "")));
            /* ══ 【窗口包自动整图覆盖 2026-10-01 · 用户口径】══════════════════════════
             * 用户："非整图包 的直接给我覆盖了" —— 上送机位时若该图还是 240×240 窗口包，
             * 自动按**整图口径**重推覆盖（设备侧相机只对整图包可用，窗口包会被能力门拒绝，
             * 现象就是"上送了但没反应"）。此前只记坐标、不发包，用户必须自己再去「素材推送」
             * 手动按整图重推一次，没人知道要做这一步。
             * 顺序铁律（与 /devices/{id}/push 一致，勿颠倒）：
             *   ① EnsureMapAsync(fullMap:true) 登记资产（覆盖同 map_id 旧窗口条目）
             *   ② BumpRev 唤醒设备长轮询 → 设备拉到新 manifest
             *   ③ 等设备下完包（大图十几 MB，真机实测 4MB≈8s / 19MB≈26s）再发相机指令，
             *      否则 cam 先到 = 仍按旧窗口包判定 → 又被拒
             *   ④ 若本来就在整图包上 → 不重推，直接下发（省一次打包与下载）。
             * ⚠️ 若设备正好渲染着这张图，重推会触发一次切图重载（屏上短暂 loading），
             *    这是覆盖口径的必然代价；返回体用 repushed 字段告知 Web 侧。 */
            bool isWindow = !string.Equals(map.Viewport, "full", StringComparison.OrdinalIgnoreCase);
            string bgHash = isWindow ? "" : (FindBgmapHash(paths, id, mapId) ?? "");
            if (isWindow)
            {
                var aid = id; var amid = mapId;
                _ = Task.Run(async () =>
                {
                    try
                    {
                        Console.WriteLine($"[Camera] 设备 {aid} 地图 {amid} 当前为窗口包 → 自动按整图口径重推覆盖");
                        bool gen = assets.EnsureMapAsync(aid, amid, fullMap: true, tiled: true);
                        mfst.BumpRev(aid, $"相机上送：地图 {amid} 整图口径覆盖（窗口包不可平移）");
                        string? hash = FindBgmapHash(paths, aid, amid);
                        if (hash == null)
                        {
                            Console.Error.WriteLine($"[Camera] 设备 {aid} 地图 {amid} 整图重推后仍找不到 BGMAP 条目 → 未下发机位");
                            return;
                        }
                        var st = store.Get(aid, amid);
                        // 等设备下完包再发机位；按包体量估算（下限 8s，上限 40s）
                        int waitMs = (int)Math.Clamp(map.Vw * (long)map.Vh / 900, 8000, 40000);
                        await Task.Delay(waitMs);
                        if (st == null) return;
                        /* 重推已完成 → 重新读该图的**整图口径**尺寸，把当时原样保留的用户坐标
                         * 按真实 maxX/maxY 夹取并落盘，再下发（否则会把 0,0 或越界坐标发出去）。 */
                        var fresh = camera.FindMap(aid, amid) ?? map;
                        var (fx, fy) = CameraService.ClampCamera(fresh, st.X, st.Y);
                        bool reclamped = fx != st.X || fy != st.Y;
                        var saved2 = store.Save(aid, amid, fx, fy, fresh.Vw, fresh.Vh);
                        queue.Enqueue(aid, "cam", new { x = fx, y = fy });
                        eventLog.Append(aid, $"相机重推整图后下发机位：地图 {amid} ({fx},{fy})"
                                             + (reclamped ? $"（原送 {st.X},{st.Y} 按整图尺寸夹取）" : "")
                                             + $"（等 {waitMs}ms）");
                        Console.WriteLine($"[Camera] 设备 {aid} 地图 {amid} 机位 ({fx},{fy}) 已在整图重推后下发"
                                          + $"（hash={hash} 等 {waitMs}ms，整图 {fresh.Vw}x{fresh.Vh}"
                                          + (reclamped ? $"，原送 {st.X},{st.Y} 已夹取" : "") + $"，记录 {saved2.UpdatedUtc:HH:mm:ss}）");
                    }
                    catch (Exception ex)
                    {
                        Console.Error.WriteLine($"[Camera] 设备 {aid} 地图 {amid} 整图重推失败: {ex.Message}");
                        health.RecordEvent(aid, "camera_repush_error",
                            System.Text.Json.JsonSerializer.SerializeToElement(new { mapId = amid, error = ex.Message }));
                        eventLog.Append(aid, $"相机整图重推失败：{amid}（{ex.Message}）");
                    }
                });
            }
            else if (!string.IsNullOrEmpty(bgHash))
            {
                queue.Enqueue(id, "cam", new { x = cx, y = cy });
                eventLog.Append(id, $"机位下发设备：地图 {mapId} ({cx},{cy})");
            }

            return Results.Json(new
            {
                ok = true,
                deviceId = id,
                mapId,
                x = cx,
                y = cy,
                requested = new { x = body.X.Value, y = body.Y.Value },
                /** 窗口包时为 false（坐标原样保留，等整图重推后再夹取），整图包时为常规夹取结果 */
                clamped = !isWindowMap && (cx != body.X.Value || cy != body.Y.Value),
                maxX = map.MaxX,
                maxY = map.MaxY,
                vw = map.Vw,
                vh = map.Vh,
                updatedUtc = saved.UpdatedUtc,
                viewport = map.Viewport,
                /** true = 该图原是窗口包，已自动按整图口径重推并在下载完成后下发机位 */
                repushed = isWindow,
                note = isWindow
                    ? "此图原为 240×240 窗口包（设备侧相机不可用）→ 已自动按整图口径重推覆盖；"
                      + "你送的坐标已**原样保留**，待整图下完后按真实尺寸夹取并下发"
                      + "（大图约 10~40s，屏上会短暂 loading）"
                    : "已写入 data/camera-positions.json 并直接下发设备（整图包，相机可用）",
            });
        });
    }

    /// <summary>
    /// 查该设备 manifest 里某张地图的 BGMAP content_hash（下发切图/相机指令用的身份）。
    /// 与 AdminEndpoints.FindBgmapHash 同口径（按 selector=map + map=mapId + kind=BGMAP 匹配）；
    /// 这里复制一份而不跨类调用，是为了让相机端点自成一体、不依赖 AdminEndpoints 的私有实现。
    /// </summary>
    private static string? FindBgmapHash(ServerPaths paths, string deviceId, string mapId)
    {
        try
        {
            var indexPath = Path.Combine(paths.ExportDirFor(deviceId), "manifest-assets.json");
            if (!File.Exists(indexPath)) return null;
            var root = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(indexPath)) as System.Text.Json.Nodes.JsonObject;
            if (root?["assets"] is not System.Text.Json.Nodes.JsonObject assets) return null;
            foreach (var kv in assets)
            {
                if (kv.Value is not System.Text.Json.Nodes.JsonObject e) continue;
                if (!string.Equals(e["selector"]?.GetValue<string>(), "map", StringComparison.OrdinalIgnoreCase)) continue;
                if (!string.Equals(e["kind"]?.GetValue<string>(), "BGMAP", StringComparison.OrdinalIgnoreCase)) continue;
                if (string.Equals(e["map"]?.GetValue<string>(), mapId, StringComparison.Ordinal)) return kv.Key;
            }
        }
        catch (Exception ex) { Console.Error.WriteLine($"[Camera] 查 BGMAP hash 失败: {ex.Message}"); }
        return null;
    }

    /// <summary>预览图服务端实际输出宽度（等比缩放到长边 maxW；不放大）。</summary>
    private static int PreviewWidth(int vw, int maxW)
    {
        maxW = Math.Clamp(maxW, 240, 4096);
        return vw <= maxW ? vw : maxW;
    }

    /// <summary>PNG 响应 + 自定义头（Results.File 不便加头；图片直出响应体，不套 JSON/base64）。</summary>
    private static IResult Png(byte[] png, Action<HttpResponse> decorate)
        => new PngWithHeadersResult(png, decorate);

    private static IResult NotFoundDevice(string id)
        => Results.Json(new { error = $"设备不存在：{id}" }, statusCode: 404);

    /// <summary>PUT /camera 请求体。</summary>
    public sealed class CameraSaveRequest
    {
        public string? MapId { get; set; }
        public int? X { get; set; }
        public int? Y { get; set; }
    }

    /// <summary>带响应头的 PNG 结果（相机端点专用）。</summary>
    private sealed class PngWithHeadersResult : IResult
    {
        private readonly byte[] _png;
        private readonly Action<HttpResponse> _decorate;

        public PngWithHeadersResult(byte[] png, Action<HttpResponse> decorate)
        {
            _png = png;
            _decorate = decorate;
        }

        public async Task ExecuteAsync(HttpContext httpContext)
        {
            var resp = httpContext.Response;
            resp.ContentType = "image/png";
            resp.ContentLength = _png.Length;
            _decorate(resp);
            await resp.Body.WriteAsync(_png, httpContext.RequestAborted);
        }
    }
}
