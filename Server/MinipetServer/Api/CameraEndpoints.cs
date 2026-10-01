using System.IO;
using MinipetServer.Config;
using MinipetServer.Device;
using MinipetServer.Health;
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
        /// 记录机位（幂等 upsert）。body { mapId, x, y }；坐标按该图 vw/vh 夹取后落盘
        /// （服务端不存越界值——设备侧也会夹，两边存不一样的值只会让"回填"看起来漂移）。
        /// 返回落盘后的实际值，前端以返回值回显（不乐观看待自己发出去的数）。
        /// </summary>
        g.MapPut("/devices/{id}/camera", (string id, CameraSaveRequest body, DeviceRegistry reg, CameraService camera,
            CameraPlanStore store, DeviceEventLog eventLog) =>
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

            var (cx, cy) = CameraService.ClampCamera(map, body.X.Value, body.Y.Value);
            var saved = store.Save(id, mapId, cx, cy, map.Vw, map.Vh);
            eventLog.Append(id, $"机位记录：地图 {mapId} x={cx} y={cy}（范围 x[0,{map.MaxX}] y[0,{map.MaxY}]）");
            Console.WriteLine($"[Camera] 设备 {id} 地图 {mapId} 机位已记录 → ({cx},{cy})"
                              + (cx != body.X.Value || cy != body.Y.Value
                                  ? $"（请求值 {body.X},{body.Y} 已按图尺寸夹取）" : ""));
            return Results.Json(new
            {
                ok = true,
                deviceId = id,
                mapId,
                x = cx,
                y = cy,
                requested = new { x = body.X.Value, y = body.Y.Value },
                clamped = cx != body.X.Value || cy != body.Y.Value,
                maxX = map.MaxX,
                maxY = map.MaxY,
                vw = map.Vw,
                vh = map.Vh,
                updatedUtc = saved.UpdatedUtc,
                note = "已写入 data/camera-positions.json（服务端为主口径）；下发设备请再调 POST "
                       + $"/api/admin/devices/{id}/command {{\"type\":\"cam\",\"value\":\"{cx},{cy}\"}}",
            });
        });
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
