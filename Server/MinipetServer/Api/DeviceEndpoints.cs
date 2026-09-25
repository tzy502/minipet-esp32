using System.Diagnostics;
using System.IO;
using System.Text.Json;
using MinipetServer.Config;
using MinipetServer.Device;
using MinipetServer.Health;
using MinipetServer.Manifest;
using MinipetServer.Music;

namespace MinipetServer.Api;

/// <summary>
/// 设备端点（E2，前缀 /api/device）：hello / manifest / asset / poll / event / bgm / firmware。
/// 协议要点：entities[] 首版 N=1 不改协议；无 Hermes/HA 字段；proto 版本号预留 v2 兼容；
/// 设备永远是 client；局域网信任模型 v1 无鉴权。
/// </summary>
public static class DeviceEndpoints
{
    /// <summary>长轮询挂起上限（55s：服务端重启余量 + 长轮询退避起点 60s 之内）。</summary>
    public static readonly TimeSpan PollMaxWait = TimeSpan.FromSeconds(55);

    public static void Map(WebApplication app)
    {
        var g = app.MapGroup("/api/device");

        g.MapPost("/hello", HandleHello);
        g.MapGet("/manifest", HandleManifest);
        g.MapGet("/asset/{hash}", HandleAsset);
        g.MapGet("/poll", HandlePoll);
        g.MapPost("/event", HandleEvent);
        g.MapGet("/bgm/stream", HandleBgmStream);
        g.MapPost("/bgm/cmd", HandleBgmCmd);
        g.MapGet("/firmware/{ver}.bin", HandleFirmware);
    }

    // ── DTO ───────────────────────────────────────────────────────────────

    public sealed class HelloProfileBody
    {
        public int W { get; set; }
        public int H { get; set; }
        public string? Shape { get; set; }
        public int Psram { get; set; }
        public bool Audio { get; set; }
    }

    public sealed class HelloRequest
    {
        public string? Uuid { get; set; }
        public string? Firmware { get; set; }
        public HelloProfileBody? Profile { get; set; }
    }

    public sealed class DeviceEventRequest
    {
        public string? DeviceId { get; set; }
        public string? Type { get; set; }
        public DateTime? TsUtc { get; set; }
        public JsonElement? Data { get; set; }
        /// <summary>可选：设备本地缓存 hash 集（E7 cached 标记的服务端计算源）。</summary>
        public List<string>? Hashes { get; set; }
    }

    public sealed class BgmCmdRequest
    {
        public string? DeviceId { get; set; }
        public string? Source { get; set; }
        public string? Cmd { get; set; }
        public int? Volume { get; set; }
        public string? Id { get; set; }
    }

    // ── 处理器 ────────────────────────────────────────────────────────────

    /// <summary>设备注册：profile+UUID+固件版本 → deviceId 与配置。匿名可用（配对只解锁 Web 管理，E13）。</summary>
    private static IResult HandleHello(
        HelloRequest body, DeviceRegistry reg, ConfigService cfg, DeviceManifestService mfst)
    {
        if (string.IsNullOrWhiteSpace(body?.Uuid))
            return Results.Json(new { error = "uuid 必填" }, statusCode: 400);

        DeviceProfile? profile = null;
        if (body.Profile != null)
        {
            profile = new DeviceProfile
            {
                W = body.Profile.W,
                H = body.Profile.H,
                Shape = body.Profile.Shape ?? "",
                Psram = body.Profile.Psram,
                Audio = body.Profile.Audio,
            };
        }

        var dev = reg.GetOrCreateByUuid(body.Uuid, profile, body.Firmware);

        string? code = null;
        if (!dev.Paired)
        {
            code = reg.ActivePairingCode(dev.DeviceId) ?? reg.IssuePairingCode(dev.DeviceId);
        }

        var th = dev.Thresholds ?? cfg.Current.Device;
        return Results.Json(new
        {
            deviceId = dev.DeviceId,
            proto = DeviceManifestService.Proto,
            paired = dev.Paired,
            name = dev.Paired ? dev.Name : null,
            pairingCode = code,
            config = new
            {
                imuDeadzoneDeg = th.ImuDeadzoneDeg,
                tapLightG = th.TapLightG,
                tapHardG = th.TapHardG,
                idleToClockMin = th.IdleToClockMin,
                bgmDefaultSource = cfg.Current.Bgm.DefaultSource,
                volume = dev.Bgm.Volume,
            },
            manifestRev = mfst.GetCurrentRev(dev.DeviceId),
            manifestUrl = $"/api/device/manifest?deviceId={dev.DeviceId}",
            pollUrl = $"/api/device/poll?deviceId={dev.DeviceId}",
            serverTimeUtc = DateTime.UtcNow,
        });
    }

    /// <summary>素材版本表：ETag 形如 "r{rev}c{cachedVersion}"——rev 为主版本（素材/配置），
    /// cachedVersion 为设备本地缓存集代数（E7）；命中 If-None-Match → 304（设计风险表：rev 未变 304 语义）。</summary>
    private static IResult HandleManifest(
        HttpRequest req, string deviceId, DeviceRegistry reg, DeviceManifestService mfst)
    {
        var dev = reg.Get(deviceId);
        if (dev == null) return NotFoundDevice(deviceId);

        var json = mfst.BuildManifestJson(deviceId, out var rev, out var cachedVersion);
        var etag = $"\"r{rev}c{cachedVersion}\"";
        var res = req.HttpContext.Response;
        res.Headers.ETag = etag;
        res.Headers.CacheControl = "no-cache";

        var inm = req.Headers.IfNoneMatch.ToString();
        if (!string.IsNullOrEmpty(inm) && inm.Contains(etag, StringComparison.Ordinal))
            return Results.StatusCode(StatusCodes.Status304NotModified);

        return Results.Content(json, "application/json; charset=utf-8");
    }

    /// <summary>单个素材包：流式回 data/cache/export/{deviceId}/（共享包回退全局），禁整载内存。</summary>
    private static IResult HandleAsset(string hash, string? deviceId, DeviceManifestService mfst)
    {
        var file = mfst.FindAssetFile(deviceId, hash);
        if (file == null)
            return Results.Json(new { error = $"素材不存在：{hash}（先跑导出器产出 manifest-assets 与 .mpk）" }, statusCode: 404);

        var fs = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 64 * 1024, useAsync: true);
        return Results.File(fs, "application/octet-stream", Path.GetFileName(file));
    }

    /// <summary>拉指令队列（长轮询挂起 ≤55s；按 seq 有序取走）。</summary>
    private static async Task<IResult> HandlePoll(
        HttpContext ctx, string deviceId, long since, DeviceRegistry reg, CommandQueue queue)
    {
        var dev = reg.Get(deviceId);
        if (dev == null) return NotFoundDevice(deviceId);
        reg.Touch(deviceId);

        var sw = Stopwatch.StartNew();
        var commands = await queue.PollAsync(deviceId, since, PollMaxWait, ctx.RequestAborted);
        sw.Stop();

        return Results.Json(new
        {
            deviceId,
            since,
            lastSeq = queue.GetLastSeq(deviceId),
            commands,
            waitedMs = sw.ElapsedMilliseconds,
        });
    }

    /// <summary>设备上报事件：触摸 / IMU 力度分级 / 倾斜状态变迁 / 低电 / 错误 / 降级 + 可选本地 hash 集。</summary>
    private static IResult HandleEvent(
        DeviceEventRequest body, DeviceRegistry reg, HealthReport health, DeviceManifestService mfst)
    {
        if (string.IsNullOrWhiteSpace(body?.DeviceId) || string.IsNullOrWhiteSpace(body.Type))
            return Results.Json(new { error = "deviceId 与 type 必填" }, statusCode: 400);
        var dev = reg.Get(body.DeviceId);
        if (dev == null) return NotFoundDevice(body.DeviceId);

        reg.Touch(dev.DeviceId);
        health.RecordEvent(dev.DeviceId, body.Type, body.Data);
        if (body.Hashes != null) mfst.SetCachedHashes(dev.DeviceId, body.Hashes);

        return Results.Json(new { ok = true, manifestRev = mfst.GetCurrentRev(dev.DeviceId) });
    }

    /// <summary>MP3 流：BgmRouter 流式转发（E8：设备只见此 URL；服务端实时取链/取文件，禁整载内存）。</summary>
    private static async Task<IResult> HandleBgmStream(
        HttpContext ctx, string deviceId, string? source, string id,
        DeviceRegistry reg, BgmRouter router, HealthReport health)
    {
        var dev = reg.Get(deviceId);
        if (dev == null) return NotFoundDevice(deviceId);
        reg.Touch(deviceId);

        try
        {
            var stream = await router.OpenStreamAsync(source, id, ctx.RequestAborted);
            return Results.File(stream.Stream, stream.MimeType);
        }
        catch (BgmSourceUnavailableException ex)
        {
            health.RecordEvent(dev.DeviceId, "bgm_failover",
                StorageUtil.ToElement(new { source = ex.SourceName, reason = ex.Message }));
            return Results.Json(new { error = ex.Message, source = ex.SourceName }, statusCode: 503);
        }
    }

    /// <summary>播放/暂停/切歌/音量（设备端现场控制的回传，E8：控制权在设备）。</summary>
    private static async Task<IResult> HandleBgmCmd(
        BgmCmdRequest body, DeviceRegistry reg, ConfigService cfg, BgmRouter router)
    {
        var valid = new[] { "play", "pause", "next", "prev", "select", "volume" };
        if (string.IsNullOrWhiteSpace(body?.DeviceId))
            return Results.Json(new { error = "deviceId 必填" }, statusCode: 400);
        var cmd = (body.Cmd ?? "").Trim().ToLowerInvariant();
        if (!valid.Contains(cmd))
            return Results.Json(new { error = $"cmd 非法：{body.Cmd}（可用：{string.Join("/", valid)}）" }, statusCode: 400);

        var dev = reg.Get(body.DeviceId);
        if (dev == null) return NotFoundDevice(body.DeviceId);
        reg.Touch(dev.DeviceId);

        var source = !string.IsNullOrWhiteSpace(body.Source)
            ? body.Source.Trim().ToLowerInvariant()
            : !string.IsNullOrWhiteSpace(dev.Bgm.Source) ? dev.Bgm.Source : cfg.Current.Bgm.DefaultSource;

        string? trackId = body.Id;
        if (cmd is "next" or "prev")
        {
            trackId = await router.NextTrackAsync(source, body.Id, cmd == "next" ? 1 : -1);
        }

        var volume = body.Volume is >= 0 and <= 100 ? body.Volume.Value : dev.Bgm.Volume;
        reg.Update(dev.DeviceId, d =>
        {
            d.Bgm.Source = source;
            d.Bgm.Volume = volume;
        });

        return Results.Json(new { ok = true, cmd, source, trackId, volume });
    }

    /// <summary>OTA 固件包：data/firmware/{ver}.bin（E11：设备直接 WiFi OTA，双分区回滚）。</summary>
    private static IResult HandleFirmware(string ver, DeviceManifestService mfst)
    {
        var file = mfst.GetFirmwareFile(ver);
        if (file == null)
            return Results.Json(new { error = $"固件不存在：{ver}.bin（放入 data/firmware/ 并写 latest.json）" }, statusCode: 404);
        var fs = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 64 * 1024, useAsync: true);
        return Results.File(fs, "application/octet-stream", ver + ".bin");
    }

    private static IResult NotFoundDevice(string deviceId)
        => Results.Json(new { error = $"未注册设备：{deviceId}（先 POST /api/device/hello）" }, statusCode: 404);
}
