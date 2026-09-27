using System.Diagnostics;
using System.IO;
using System.Text.Json;
using MinipetServer.Config;
using MinipetServer.Device;
using MinipetServer.Health;
using MinipetServer.Manifest;
using MinipetServer.Music;
using MinipetServer.Services;

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
        // E14：设备环形日志上报（契约见 docs/ai/keys-touch-handoff.md §7.2）
        g.MapPost("/log", HandleLog);
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

    /// <summary>POST /api/device/log：设备端环形日志增量上报（字段名与固件严格一致）。</summary>
    public sealed class DeviceLogRequest
    {
        public int Proto { get; set; } = 1;
        public string? DeviceId { get; set; }
        /// <summary>设备侧上次成功送达的 seq；本次 logs 的 seq 均大于它。</summary>
        public uint Since { get; set; }
        public int Count { get; set; }
        public List<DeviceLogItem>? Logs { get; set; }
    }

    public sealed class DeviceLogItem
    {
        public uint Seq { get; set; }
        /// <summary>epoch 毫秒；未校时时是开机毫秒（&lt;&lt; 1.7e12）。</summary>
        public long Ts { get; set; }
        /// <summary>设备开机毫秒（本地时基，恒单调）。</summary>
        public uint T { get; set; }
        public string Lvl { get; set; } = "I";
        public string Tag { get; set; } = "";
        /// <summary>日志正文的 hex（UTF-8 字节的十六进制小写）。</summary>
        public string MsgHex { get; set; } = "";
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
        HelloRequest body, DeviceRegistry reg, ConfigService cfg, DeviceManifestService mfst, DeviceEventLog eventLog,
        PaperdollPackService packs, FontPackService fonts, HealthReport health)
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

        // 事件日志：注册前先取旧态（首次注册 / 断线后 hello 重新上线）
        var before = reg.List().FirstOrDefault(d =>
            string.Equals(d.Uuid, body.Uuid.Trim(), StringComparison.OrdinalIgnoreCase));

        var dev = reg.GetOrCreateByUuid(body.Uuid, profile, body.Firmware);

        if (before == null)
            eventLog.Append(dev.DeviceId, $"设备首次注册（hello 接入，固件 {dev.Firmware}）");
        else if (!DeviceRegistry.IsOnline(before))
            eventLog.Append(dev.DeviceId, "设备上线（hello 心跳）");

        string? code = null;
        if (!dev.Paired)
        {
            code = reg.ActivePairingCode(dev.DeviceId) ?? reg.IssuePairingCode(dev.DeviceId);
        }

        // E13/E12 首启 provisioning（后台补齐默认素材 + 字体包；失败只记事件，不阻断 hello）
        ScheduleFirstBootProvisioning(dev, packs, fonts, mfst, eventLog, health);

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
                imuSensitivity = th.ImuSensitivity,   /* E4：倍率，有效阈值 = 阈值 ÷ 灵敏度 */
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
        HttpContext ctx, string deviceId, long since, DeviceRegistry reg, CommandQueue queue, DeviceEventLog eventLog,
        DeviceManifestService mfst)
    {
        var dev = reg.Get(deviceId);
        if (dev == null) return NotFoundDevice(deviceId);
        var wasOnline = DeviceRegistry.IsOnline(dev); // Touch 前取旧态：离线→在线即记一行上线
        reg.Touch(deviceId);
        if (!wasOnline)
            eventLog.Append(deviceId, "设备上线（poll 心跳）");

        var sw = Stopwatch.StartNew();
        var commands = await queue.PollAsync(deviceId, since, PollMaxWait, ctx.RequestAborted);
        sw.Stop();

        return Results.Json(new
        {
            deviceId,
            since,
            lastSeq = queue.GetLastSeq(deviceId),
            // 线上形状：默认 {seq,type,payload}（不变）；bgm vol/source 这类只能走固件
            // handle_cmd 的指令由 DeviceCommand.Legacy 投影成旧口径 {seq,t,v,n}（无 type 字段，
            // poller.c 见 type 缺失即直通 handle_cmd）。详见 DeviceCommand.Legacy 注释。
            commands = commands.Select(c => c.ToWire()).ToList(),
            waitedMs = sw.ElapsedMilliseconds,
            // 设备素材 diff 依据（poller.c：mrev != 本地 rev → asset_dl_request_sync 立即拉包）——
            // 此前响应缺此字段，设备恒读 0，manifest 变更只能靠设备重启兜底
            mrev = mfst.GetCurrentRev(deviceId),
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

    /// <summary>
    /// 设备端日志上报（E14）：hex 解码 → 去重入库 → 200。
    /// 响应必须 200 设备才推进游标；非 200 设备 60s 后整批重传（幂等已由
    /// DeviceLogStore 的会话/seq 判重保证，重传不会产生重复行）。
    /// </summary>
    private static IResult HandleLog(DeviceLogRequest body, DeviceRegistry reg, DeviceLogStore logs)
    {
        if (string.IsNullOrWhiteSpace(body?.DeviceId))
            return Results.Json(new { error = "deviceId 必填" }, statusCode: 400);
        if (reg.Get(body.DeviceId) == null) return NotFoundDevice(body.DeviceId);

        var now = DateTime.UtcNow;
        var list = new List<DeviceLogStore.LogLine>(body.Logs?.Count ?? 0);
        int bad = 0;
        foreach (var it in body.Logs ?? new List<DeviceLogItem>())
        {
            var msg = DeviceLogStore.DecodeHex(it.MsgHex);
            if (msg == null) { bad++; continue; }     // 非法 hex：跳过该行但不整批拒收
            list.Add(new DeviceLogStore.LogLine(it.Seq, it.Ts, it.T, it.Lvl, it.Tag, msg, now));
        }

        var (lastSeq, added) = logs.Append(body.DeviceId, body.Since, list);
        reg.Touch(body.DeviceId);
        return Results.Json(new { ok = true, accepted = added, skipped = bad, lastSeq });
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

    /// <summary>播放/暂停/切歌/音量（设备端现场控制的回传，E8：控制权在设备）。
    /// 回传同时记一行设备事件日志——Web 曲库页「设备事件」要能看到「设备触摸屏上做了什么」
    /// （Web/docs/interfaces-needed-from-server.md §T6 实现要点 4）。</summary>
    private static async Task<IResult> HandleBgmCmd(
        BgmCmdRequest body, DeviceRegistry reg, ConfigService cfg, BgmRouter router, DeviceEventLog eventLog)
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

        // 事件日志：source/cmd 取固件字面量；trackId 有值才带（select/next/prev 才有曲目）
        eventLog.Append(dev.DeviceId,
            $"BGM：{cmd}（设备现场控制）· source={source} · volume={volume}"
            + (string.IsNullOrWhiteSpace(trackId) ? "" : $" · 曲目 {trackId}"));

        // 固件 bgm_cmd() 取的数字字段名是 "id"（cJSON_GetObjectItem(r,"id")），此前本端点只回
        // trackId 字符串 → 设备兜底取曲路径（本地无 AUDIO_META 包时走这里）永远拿不到 id，
        // next/prev/play 全部静默无动作。补回数字 id：按 AUDIO_META 同口径 XxHash32(trackKey)，
        // 以 int32 位型承载 u32（固件按 (int) 读，负数即高位 id 的位型）。
        int? trackNumId = string.IsNullOrWhiteSpace(trackId)
            ? null
            : unchecked((int)MiniPet.Export.AudioMetaWriter.TrackIdForKey(trackId));
        return Results.Json(new { ok = true, cmd, source, trackId, id = trackNumId, volume });
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

    // ── 首启 provisioning（E13 默认素材 + E12 字体链，2026-09-27 补链）────────────

    /// <summary>
    /// 设备首次 hello 时 data/cache/export/{deviceId}/ 是空的 → manifest 无 assets：
    /// 设备既没有宠物可显示（无 PARTS/LAYOUT，新建 uuid 首启屏幕空白）也没有字
    /// （无 FONT，固件报 "font 1 not loaded"，菜单/气泡全无字）。本方法在后台补齐：
    ///   · 外观：PetConfig 为空 → seed/default-appearance.json（出厂宠物）；
    ///     PetConfig 有值但包丢了（data 被清 / 换机）→ 用设备自己的配置重打。
    ///   · 字体：16/24/32 三档 FONT 包（FontPackService，种子 JSON 打包，与 WZ 无关）。
    /// 幂等：索引里已有 PARTS(selector=paperdoll) 与任一 FONT 条目时整体跳过（只读索引，
    /// 不触发打包）；两段各自失败只记日志 + 设备事件，绝不抛给 hello。
    /// 铁律：hello 不等打包（装扮导出数秒、字体 ~1s），打包完成即 BumpRev 唤醒设备长轮询。
    /// </summary>
    private static void ScheduleFirstBootProvisioning(DeviceRecord dev, PaperdollPackService packs,
        FontPackService fonts, DeviceManifestService mfst, DeviceEventLog eventLog, HealthReport health)
    {
        bool hasAppearance, hasFonts;
        try
        {
            hasAppearance = packs.HasPackedAppearance(dev.DeviceId);
            hasFonts = fonts.PackedSizes(dev.DeviceId).Count > 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Provision] 设备 {dev.DeviceId} 索引检查失败: {ex.Message}");
            return;
        }
        if (hasAppearance && hasFonts) return;

        bool hasPet = dev.PetConfig is { ValueKind: JsonValueKind.Object };
        _ = Task.Run(() =>
        {
            try
            {
                if (!hasAppearance)
                {
                    var appearance = hasPet ? dev.PetConfig!.Value : TryLoadSeedAppearance();
                    if (appearance.ValueKind != JsonValueKind.Object)
                    {
                        Console.Error.WriteLine($"[Provision] 设备 {dev.DeviceId} 无可用外观（PetConfig 空且 seed 缺失），跳过装扮 provisioning");
                        eventLog.Append(dev.DeviceId, "首启 provisioning：无可用外观（PetConfig 空 + seed 缺失）");
                    }
                    else
                    {
                        bool packed = packs.EnsurePacked(dev.DeviceId, appearance);
                        mfst.BumpRev(dev.DeviceId, packed ? "首启默认素材已生成" : "首启素材未变化（同外观已打包）");
                        eventLog.Append(dev.DeviceId, packed
                            ? (hasPet ? "首启 provisioning：按设备 petConfig 重打 PARTS+LAYOUT" : "首启 provisioning：默认外观 PARTS+LAYOUT 已下发")
                            : "首启 provisioning：装扮包已存在，跳过");
                        Console.WriteLine($"[Provision] 设备 {dev.DeviceId} 装扮 provisioning {(packed ? "完成" : "跳过（已存在）")}");
                    }
                }

                if (!hasFonts)
                {
                    int n = fonts.EnsureFonts(dev.DeviceId);
                    if (n > 0)
                    {
                        mfst.BumpRev(dev.DeviceId, $"字体包已生成（{n} 档 FONT）");
                        eventLog.Append(dev.DeviceId, $"首启 provisioning：{n} 档 FONT 字体包已下发（E12）");
                        Console.WriteLine($"[Provision] 设备 {dev.DeviceId} 字体 provisioning 完成（{n} 档）");
                    }
                    else
                    {
                        eventLog.Append(dev.DeviceId, "首启 provisioning：字体包已存在，跳过");
                    }
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[Provision] 设备 {dev.DeviceId} 首启 provisioning 失败: {ex.Message}");
                health.RecordEvent(dev.DeviceId, "provision_error", JsonSerializer.SerializeToElement(new { error = ex.Message }));
                eventLog.Append(dev.DeviceId, $"首启 provisioning 失败：{ex.Message}（设备稍后重连会重试）");
            }
        });
    }

    /// <summary>seed/default-appearance.json → JsonElement（出厂宠物外观）；缺失/损坏返回 default（ValueKind=Undefined）。</summary>
    private static JsonElement TryLoadSeedAppearance()
    {
        try
        {
            var file = Path.Combine(MiniPet.Export.DeviceProfile.FindSeedRoot(), "default-appearance.json");
            if (!File.Exists(file)) return default;
            return JsonDocument.Parse(File.ReadAllText(file)).RootElement.Clone();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Provision] seed 默认装扮读取失败: {ex.Message}");
            return default;
        }
    }
}
