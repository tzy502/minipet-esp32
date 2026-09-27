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
/// Web 管理端点（E4/E13，前缀 /api/admin）：设备卡片与配置 / 配对 / 缩略图 /
/// 纸娃娃预设 CRUD / 曲库与音源管理（cookie 导入·健康·启停）/ 设置读写（WZ 校验）/
/// 设备事件日志 / OTA 触发 / 素材推送到设备（E7：地图/NPC 资产登记+切图）。
/// 局域网信任模型：v1 无鉴权。
/// </summary>
public static class AdminEndpoints
{
    public static void Map(WebApplication app)
    {
        var g = app.MapGroup("/api/admin");

        g.MapGet("/devices", (DeviceRegistry reg, HealthReport health) => Results.Json(new
        {
            devices = reg.List().Select(d => DeviceCard(d, reg, health)).ToList(),
        }));

        g.MapGet("/devices/{id}", (string id, DeviceRegistry reg, ConfigService cfg, HealthReport health)
            => Detail(id, reg, cfg, health) ?? NotFoundDevice(id));

        g.MapPut("/devices/{id}", (string id, DeviceUpdateRequest body, DeviceRegistry reg, ConfigService cfg,
            HealthReport health, DeviceManifestService mfst, DeviceEventLog eventLog, PaperdollPackService packs) =>
        {
            var existing = reg.Get(id);
            if (existing == null) return NotFoundDevice(id);

            bool petChanged = body.PetConfig.HasValue; // 显式传了（含传 null 清空）→ 换宠换装 → manifest rev+1
            var updated = reg.Update(id, d =>
            {
                if (body.Name != null) d.Name = body.Name.Trim();
                if (petChanged)
                    d.PetConfig = body.PetConfig!.Value.ValueKind == JsonValueKind.Null ? null : body.PetConfig;
                if (body.Bgm != null)
                {
                    if (body.Bgm.Source != null) d.Bgm.Source = body.Bgm.Source.Trim().ToLowerInvariant();
                    if (body.Bgm.Volume is >= 0 and <= 100) d.Bgm.Volume = body.Bgm.Volume.Value;
                }
                if (body.Thresholds != null) d.Thresholds = body.Thresholds;
            });
            if (updated == null) return NotFoundDevice(id);

            // 事件日志：重命名 / petConfig 变更（含清空）/ BGM 与阈值保存（预设删除为非设备事件，不记）
            if (body.Name != null && !string.Equals(existing.Name, updated.Name, StringComparison.Ordinal))
                eventLog.Append(id, $"设备重命名：「{existing.Name}」→「{updated.Name}」");
            if (petChanged)
            {
                bool cleared = body.PetConfig!.Value.ValueKind == JsonValueKind.Null;
                eventLog.Append(id, cleared ? "petConfig 清空（回默认宠物）" : "petConfig 变更（换宠换装）");
                // 装扮打包后台跑（PARTS+LAYOUT 导出数秒）：先生成设备资产包，成功才 bump rev——
                // 设备 poll 拉新 manifest 时包已就位。此前只存配置直接 bump → manifest assets
                // 未变 → 设备不拉包，「应用到设备」无效的根因。打包失败记日志+设备事件，不 bump
                // （设备不读 petConfig 本身，只认 manifest assets）。清空（null）无需打包。
                if (cleared)
                {
                    mfst.BumpRev(id, "petConfig 清空");
                }
                else
                {
                    var snap = body.PetConfig!.Value;
                    _ = Task.Run(() =>
                    {
                        try
                        {
                            var packed = packs.EnsurePacked(id, snap);
                            mfst.BumpRev(id, packed ? "装扮包已生成（petConfig 变更）" : "装扮包未变化（同外观已打包）");
                        }
                        catch (Exception ex)
                        {
                            Console.Error.WriteLine($"[PaperdollPack] 设备 {id} 装扮打包失败: {ex.Message}");
                            health.RecordEvent(id, "pack_error", JsonSerializer.SerializeToElement(new { error = ex.Message }));
                        }
                    });
                }
            }
            if (body.Bgm != null)
                eventLog.Append(id, $"BGM 偏好保存：source={updated.Bgm.Source}，volume={updated.Bgm.Volume}");
            if (body.Thresholds != null)
                eventLog.Append(id, "按设备阈值覆盖保存：IMU 死区/轻拍/重拍/待机时钟");
            return Results.Json(new { device = Detail(id, reg, cfg, health) });
        });

        g.MapPost("/pair", (PairRequest body, DeviceRegistry reg, DeviceEventLog eventLog) =>
        {
            if (string.IsNullOrWhiteSpace(body?.Code))
                return Results.Json(new { error = "code 必填" }, statusCode: 400);
            var dev = reg.TryPair(body.Code, body.Name ?? "");
            if (dev == null)
                return Results.Json(new { error = "配对码无效或已过期（6 位码 10 分钟内有效）" }, statusCode: 404);
            eventLog.Append(dev.DeviceId,
                string.IsNullOrWhiteSpace(dev.Name) ? "配对完成（未命名）" : $"配对完成：命名「{dev.Name}」");
            return Results.Json(new { device = dev });
        });

        // 缩略图：SkiaSharp + 真实 WZ 渲染，data/cache/thumbs/ 缓存（docs/ai/web-paperdoll-alignment.md §五.服务端2）。
        // type=part/paperdoll 真实渲染；mob/npc/map 由 MaterialsEndpoints 配套真实化（详见 ThumbService）。
        g.MapGet("/thumb", (ThumbService thumbs, string type, string id, string? folder = null, string? img = null, int? size = null)
            => Results.File(thumbs.GetOrCreatePng(type, id, folder, img, size), "image/png"));

        // ── 素材收藏（E4「地图选择含收藏」；Web 探测到端点即自动从 localStorage
        //    单机模式切到服务端同步，client.js 的 FAVORITES_PATH 契约）──
        g.MapGet("/materials/favorites", (FavoritesStore favs)
            => Results.Json(new { favorites = favs.Get() }));
        g.MapPut("/materials/favorites", (FavoritesRequest body, FavoritesStore favs) =>
        {
            // body.favorites 缺失 = 清空（整表替换语义，与 Web 全量上传一致）
            var saved = favs.Replace(body?.Favorites);
            return Results.Json(new { ok = true, favorites = saved });
        });

        // ── 纸娃娃预设 CRUD（data/presets/，供设备选择器「纸娃娃 tab」，E7/E4）──        g.MapGet("/presets", (PresetStore presets) => Results.Json(new { presets = presets.List() }));
        g.MapPost("/presets", (PresetUpsertRequest body, PresetStore presets) =>
        {
            if (string.IsNullOrWhiteSpace(body?.Name))
                return Results.Json(new { error = "name 必填" }, statusCode: 400);
            var preset = presets.Create(body.Name, body.Type ?? "paperdoll", body.Data);
            return Results.Json(new { preset }, statusCode: 201);
        });
        g.MapGet("/presets/{id}", (string id, PresetStore presets)
            => presets.Get(id) is { } p ? Results.Json(new { preset = p })
                                        : Results.Json(new { error = $"预设不存在：{id}" }, statusCode: 404));
        g.MapPut("/presets/{id}", (string id, PresetUpsertRequest body, PresetStore presets) =>
        {
            var updated = presets.Update(id, body?.Name, body?.Type, body?.Data);
            return updated is { } p ? Results.Json(new { preset = p })
                                    : Results.Json(new { error = $"预设不存在：{id}" }, statusCode: 404);
        });
        g.MapDelete("/presets/{id}", (string id, PresetStore presets) =>
        {
            var ok = presets.Delete(id);
            return ok ? Results.Json(new { ok = true }) : Results.Json(new { error = $"预设不存在：{id}" }, statusCode: 404);
        });

        // ── 曲库与音源管理（E8：Web 只管曲库/歌单/cookie，不做点歌按钮）──
        g.MapGet("/music/tracks", async (string? source, BgmRouter router, CancellationToken ct) =>
        {
            var src = router.Resolve(source);
            if (src == null)
                return Results.Json(new { error = $"未知音源：{source}（可用：{string.Join("/", router.Sources.Select(s => s.Name))}）" }, statusCode: 404);
            var tracks = await src.ListTracksAsync(ct);
            return Results.Json(new { source = src.Name, count = tracks.Count, tracks });
        });

        // 音源健康（E8/E4）：qq 附带 cookie 导入时间与过期标记 —— Web 曲库页健康标签 +
        // 「cookie 待过期」提示用；cookieStale=true 表示超过 QqMusicSource.CookieStaleThreshold（7 天）
        g.MapGet("/music/sources", (BgmRouter router, ConfigService cfg, QqGatewayProcess gateway) =>
        {
            var qq = cfg.Current.QqMusic;
            var (stale, ageDays) = QqMusicSource.CookieFreshness(qq);
            return Results.Json(new
            {
                sources = router.Sources.Select(s => new
                {
                    name = s.Name,
                    enabled = s.IsEnabled,
                    health = s.Health(),
                    // QQ 专属字段（其它源为 null → JSON 序列化按 WhenWritingNull 省略）
                    cookieSavedAtUtc = s.Name == "qq" ? qq.CookieSavedAtUtc : null,
                    cookieStale = s.Name == "qq" && stale,
                    cookieAgeDays = s.Name == "qq" ? ageDays : null,
                    gateway = s.Name == "qq" ? gateway.Status : null,
                }).ToList(),
            });
        });

        g.MapGet("/music/sources/{name}/health", (string name, BgmRouter router) =>
            router.Resolve(name) is { } src
                ? Results.Json(new { name = src.Name, enabled = src.IsEnabled, health = src.Health() })
                : Results.Json(new { error = $"未知音源：{name}" }, statusCode: 404));

        g.MapPut("/music/sources/{name}", (string name, SourceToggleRequest body, BgmRouter router, ConfigService cfg) =>
        {
            var src = router.Resolve(name);
            if (src == null) return Results.Json(new { error = $"未知音源：{name}" }, statusCode: 404);
            if (src.Name == "wz")
                return Results.Json(new { error = "WZ 源为默认源，不可停用（E8）" }, statusCode: 400);
            if (body?.Enabled == null)
                return Results.Json(new { error = "enabled 必填" }, statusCode: 400);

            var (applied, errors) = cfg.Update(c => c.QqMusic.Enabled = body.Enabled.Value);
            if (applied == null) return Results.Json(new { errors }, statusCode: 400);
            return Results.Json(new { name = src.Name, enabled = body.Enabled.Value, health = src.Health() });
        });

        // cookie 导入（QQ）：落配置 + 导入时间（CookieSavedAtUtc，E4 过期告警的依据）；
        // 导入后立刻推给网关（若已就绪）并触发一次巡检（网关可能刚被拉起/重启）。
        g.MapPost("/music/sources/qq/cookie", (CookieRequest body, ConfigService cfg, QqMusicSource qq,
            QqGatewayProcess gateway) =>
        {
            var cookie = body?.Cookie?.Trim() ?? "";
            var (applied, errors) = cfg.Update(c =>
            {
                c.QqMusic.Cookie = cookie;
                c.QqMusic.CookieSavedAtUtc = string.IsNullOrEmpty(cookie) ? null : DateTime.UtcNow;
            });
            if (applied == null) return Results.Json(new { errors }, statusCode: 400);

            // 推 cookie 给网关 + 立刻巡检（配置变更本身也会触发 QqGatewayProcess 的 Changed 巡检，
            // 这里同步补一次是为了让响应里的 health 立即反映新 cookie）
            if (!string.IsNullOrEmpty(cookie))
            {
                try { gateway.EnsureHealthyNow(); }
                catch (Exception ex) { Console.Error.WriteLine($"[QqGateway] cookie 导入后巡检失败: {ex.Message}"); }
            }
            var (stale, ageDays) = QqMusicSource.CookieFreshness(cfg.Current.QqMusic);
            return Results.Json(new
            {
                ok = true,
                imported = !string.IsNullOrEmpty(cookie),
                cookieSavedAtUtc = cfg.Current.QqMusic.CookieSavedAtUtc,
                cookieStale = stale,
                cookieAgeDays = ageDays,
                health = qq.Health(),
                note = string.IsNullOrEmpty(cookie)
                    ? "cookie 已清空"
                    : "cookie 已保存并推送网关；取链/转发由 /api/device/bgm/stream 实时完成（直链不下发设备）",
            });
        });

        // ── 设置读写（E3 双通道之 Web 通道；写同一份 data/config/appsettings.json）──
        g.MapGet("/settings", (ConfigService cfg) => Results.Json(new
        {
            config = cfg.Current,
            wzPathExists = Directory.Exists(cfg.Current.Wz.DataPath),
            note = "端口属部署层 .env（MINIPET_PORT），不入 appsettings，此页只读展示（E3 定稿）",
        }));

        // WZ 路径独立校验（设置页选完路径即时验证，不落盘）：恒 200——ok=false 表校验
        // 不通过而非 HTTP 错误；空 body/缺 path 由 ValidateWzPath 分支回「不能为空」。
        g.MapPost("/settings/validate-path", (ValidatePathRequest body, ConfigService cfg) =>
        {
            var (ok, message) = cfg.ValidateWzPath(body?.Path);
            return Results.Json(new { ok, message });
        });

        // ── 设备实时指令下发（E4 25 表情手动指定 / E12 台词气泡 / 亮度 / 重启 / E8 BGM）──
        // Web 侧已就绪（Web/src/api/client.js sendDeviceCommand），此前无端点（405）→ 前端禁用态。
        // payload 形状必须与固件 poller.c 的两条解析路径逐字对齐：
        //   expression / action / bubble → 【裸 JSON 字符串】（固件找 payload 本身或 payload.id）
        //   brightness                   → 数值放 n（固件走 pn 通道）
        //   bgm play/pause/resume/stop/next/prev → 裸 JSON 字符串（poller.c:208-217 主解析分支）
        //   bgm vol / source             → **不带 type 的旧口径** {t:"bgm",v:"vol"|"source",n:N}
        //                                  （主解析分支的 bgm 没有 vol/source；只有旧口径
        //                                   handle_cmd（poller.c:77-88）实现）
        //   ⚠ 写成 {"value":"…"} / {"n":…} 之类的对象会被固件静默忽略（既不报错也不生效）——勿改。
        g.MapPost("/devices/{id}/command", (string id, DeviceCommandRequest body,
            DeviceRegistry reg, CommandQueue queue, DeviceEventLog eventLog) =>
        {
            var dev = reg.Get(id);
            if (dev == null) return NotFoundDevice(id);
            var type = body?.Type?.Trim().ToLowerInvariant();
            if (string.IsNullOrEmpty(type))
                return Results.Json(new { error = "type 必填" }, statusCode: 400);

            string[] stringTypes = { "expression", "action", "bubble" };
            if (stringTypes.Contains(type))
            {
                var value = body?.Value?.Trim();
                if (string.IsNullOrEmpty(value))
                    return Results.Json(new { error = $"{type} 需要 value" }, statusCode: 400);
                if (type == "bubble" && System.Text.Encoding.UTF8.GetByteCount(value) > 95)
                    return Results.Json(new { error = "bubble 文本超 95 字节（固件 mp_cmd_t.s=char[96]）" },
                        statusCode: 400);
                var cmd = queue.Enqueue(id, type, value);
                eventLog.Append(id, $"指令下发：{type}={value}");
                return Results.Json(new { ok = true, seq = cmd.Seq, type, value }, statusCode: 202);
            }

            // BGM（E8：控制入口在设备，Web 也可下发纯桌宠指令；曲目流仍由设备走
            // /api/device/bgm/stream 实时拉取）。value 用固件字面量，勿改写。
            if (type == "bgm")
            {
                var bgmValue = body?.Value?.Trim().ToLowerInvariant() ?? "";
                // 6 个「开关/切歌」动词：固件主解析分支直接认字符串 payload
                string[] bgmVerbs = { "play", "pause", "resume", "stop", "next", "prev" };
                if (bgmVerbs.Contains(bgmValue))
                {
                    var cmd = queue.Enqueue(id, "bgm", bgmValue);
                    eventLog.Append(id, $"指令下发：bgm={bgmValue}");
                    return Results.Json(new { ok = true, seq = cmd.Seq, type, value = bgmValue }, statusCode: 202);
                }
                // 音量绝对值：固件 v="vol"（MP_AUDIO_VOL，a=n），仅旧口径可达；顺带落设备偏好
                // （vol 生效后设备还会经 POST /api/device/bgm/cmd 回传一次，两处口径一致）
                if (bgmValue is "vol" or "volume")
                {
                    if (body?.N is not (>= 0 and <= 100))
                        return Results.Json(new { error = "bgm=vol 需要 n∈[0,100]（固件 vol_apply 夹取 0..100）" },
                            statusCode: 400);
                    var cmd = queue.EnqueueLegacy(id, "bgm", "vol", body.N);
                    reg.Update(id, d => d.Bgm.Volume = body.N!.Value);
                    eventLog.Append(id, $"指令下发：bgm=vol n={body.N}");
                    return Results.Json(new { ok = true, seq = cmd.Seq, type, value = "vol", n = body.N },
                        statusCode: 202);
                }
                // 音源切换：固件 v="source"（MP_AUDIO_SOURCE，a=n；0=WZ 曲库 / 1=QQ 音乐），同样仅旧口径可达
                if (bgmValue == "source")
                {
                    if (body?.N is not (0 or 1))
                        return Results.Json(new { error = "bgm=source 需要 n=0（WZ 曲库）或 1（QQ 音乐）" },
                            statusCode: 400);
                    var cmd = queue.EnqueueLegacy(id, "bgm", "source", body.N);
                    reg.Update(id, d => d.Bgm.Source = body.N == 1 ? "qq" : "wz");
                    eventLog.Append(id, $"指令下发：bgm=source n={body.N}");
                    return Results.Json(new { ok = true, seq = cmd.Seq, type, value = "source", n = body.N },
                        statusCode: 202);
                }
                return Results.Json(new
                {
                    error = $"bgm 的 value 非法：{body?.Value}"
                        + "（可用：play/pause/resume/stop/next/prev；音量 vol + n；音源 source + n）"
                }, statusCode: 400);
            }

            if (type == "brightness")
            {
                if (body?.N is not (>= 0 and <= 100))
                    return Results.Json(new { error = "brightness 需要 n∈[0,100]" }, statusCode: 400);
                var cmd = queue.Enqueue(id, "brightness", new { n = body.N });
                eventLog.Append(id, $"指令下发：brightness={body.N}");
                return Results.Json(new { ok = true, seq = cmd.Seq, type, n = body.N }, statusCode: 202);
            }

            if (type == "reboot")
            {
                var cmd = queue.Enqueue(id, "reboot", new { });
                eventLog.Append(id, "指令下发：reboot");
                return Results.Json(new { ok = true, seq = cmd.Seq, type }, statusCode: 202);
            }

            return Results.Json(new
            {
                error = $"type 非法：{body?.Type}（可用：expression/action/bubble/brightness/reboot/bgm）"
            }, statusCode: 400);
        });

        g.MapPut("/settings", (MinipetConfig body, ConfigService cfg) =>
        {
            var (applied, errors) = cfg.Replace(body, validateWzPath: true);
            if (applied == null)
                return Results.Json(new { errors }, statusCode: 400);
            return Results.Json(new
            {
                ok = true,
                config = applied,
                wzPathExists = Directory.Exists(applied.Wz.DataPath),
            });
        });

        // ── 设备事件日志（E14：服务端环形日志，每设备 200 条，重启清零）+ 健康事件流 ──
        g.MapGet("/logs/{id}", (string id, DeviceRegistry reg, HealthReport health, DeviceEventLog eventLog) =>
        {
            var dev = reg.Get(id);
            if (dev == null) return NotFoundDevice(id);
            var summary = health.GetSummary(id);
            return Results.Json(new
            {
                deviceId = id,
                online = DeviceRegistry.IsOnline(dev),
                note = "服务端事件环形日志（每设备 200 条，服务重启清零）",
                lines = eventLog.Tail(id),
                events = summary?.Recent ?? new List<DeviceEventRecord>(),
            });
        });

        // ── 设备端环形日志（E14：「排障不用插线」；设备每 20s 增量上报，
        //    契约见 docs/ai/keys-touch-handoff.md §7.2）──
        // 与上面 /logs/{id} 的区别：/logs 是**服务端事件**（在线/指令/错误），
        // 本端点是**设备串口日志**的服务端副本（固件环形缓冲 152 条 → 这里 512 条）。
        g.MapGet("/device-logs/{id}", (string id, uint? sinceSeq, int? limit, string? level, string? tag,
            DeviceRegistry reg, DeviceLogStore logs) =>
        {
            var dev = reg.Get(id);
            if (dev == null) return NotFoundDevice(id);
            return DeviceLogsView(id, sinceSeq ?? 0, limit ?? 200, level, tag, logs);
        });
        // UUID 口径（.NET 侧设备记录主键是 deviceId；UUID 便于按硬件号直接查）
        g.MapGet("/device-logs/by-uuid/{uuid}", (string uuid, uint? sinceSeq, int? limit, string? level, string? tag,
            DeviceRegistry reg, DeviceLogStore logs) =>
        {
            var dev = reg.List().FirstOrDefault(d =>
                string.Equals(d.Uuid, uuid, StringComparison.OrdinalIgnoreCase));
            if (dev == null) return Results.Json(new { error = $"未注册设备 uuid：{uuid}" }, statusCode: 404);
            return DeviceLogsView(dev.DeviceId, sinceSeq ?? 0, limit ?? 200, level, tag, logs);
        });

        // ── OTA 触发（E11：入队升级指令，设备 WiFi 拉包自更新，双分区回滚）──
        g.MapPost("/ota/{id}", (string id, OtaRequest body, DeviceRegistry reg, CommandQueue queue,
            DeviceManifestService mfst, DeviceEventLog eventLog) =>
        {
            var dev = reg.Get(id);
            if (dev == null) return NotFoundDevice(id);
            if (string.IsNullOrWhiteSpace(body?.Ver))
                return Results.Json(new { error = "ver 必填（如 0.3.1；对应 data/firmware/0.3.1.bin）" }, statusCode: 400);

            var url = $"/api/device/firmware/{body.Ver}.bin";
            var cmd = queue.Enqueue(id, "ota", new { ver = body.Ver, url });
            mfst.BumpRev(id, $"OTA {body.Ver} 下发");
            eventLog.Append(id, $"OTA 下发：v{body.Ver}（seq {cmd.Seq}）");
            return Results.Json(new { queued = true, seq = cmd.Seq, ver = body.Ver, url });
        });

        // ── 素材推送到设备（E7/E13：地图/NPC 资产登记进该设备 manifest，可选直接切图）──
        // body { kind: "map"|"npc", id: "200000100", switch: true }：
        // 打包数秒（WZ 锁内）→ 后台 Task.Run，端点立即 202；顺序铁律 = 先登记资产 → 再
        // BumpRev（设备长轮询被唤醒、拉到新 manifest）→ 最后 enqueue 切图指令（仅 map 且
        // switch!=false；固件 poller.c 消费 {"t":"map","v":id} → MP_CMD_SET_MAP → dispatch_map），
        // 反了设备会先收到切图指令而新 manifest 还没拉到。
        g.MapPost("/devices/{id}/push", (string id, DevicePushRequest body, DeviceRegistry reg,
            DeviceAssetService assets, WzService wz, DeviceManifestService mfst, CommandQueue queue,
            DeviceEventLog eventLog, HealthReport health, Config.ServerPaths paths) =>
        {
            try
            {
                var dev = reg.Get(id);
                if (dev == null) return NotFoundDevice(id);
                var kind = body?.Kind?.Trim().ToLowerInvariant();
                if (kind != "map" && kind != "npc")
                    return Results.Json(new { error = "kind 必须是 map 或 npc" }, statusCode: 400);
                var assetId = body?.Id?.Trim();
                if (string.IsNullOrEmpty(assetId))
                    return Results.Json(new { error = "id 必填（素材编号，如地图 200000100）" }, statusCode: 400);
                // WZ 未加载 → 503（打包必然失败，提前拦；同 materials 目录降级口径）
                if (!wz.IsLoaded)
                    return Results.Json(new { error = "WZ 未加载（到「设置」页配置后重试）" }, statusCode: 503);

                bool switchAfter = body?.Switch != false;
                _ = Task.Run(() =>
                {
                    try
                    {
                        // 登记资产（幂等，内部 per-device 锁 + WZ 锁，打包数秒；同步方法，
                        // 调用方放后台线程）→ bump rev（设备长轮询被唤醒、拉到新 manifest）
                        // → 最后才 enqueue 切图指令（设备先拿到新 manifest 再收到 map 指令才稳）。
                        bool generated = kind == "map"
                            ? assets.EnsureMapAsync(id, assetId)
                            : assets.EnsureNpcAsync(id, assetId);
                        Console.WriteLine($"[DevicePush] 设备 {id} {kind} {assetId} 资产登记{(generated ? "完成（新打包）" : "跳过（已登记，幂等）")}");
                        // 登记成功必 bump：manifest-assets.json 变了，rev 不动设备感知不到。
                        // BumpRev 内部会往指令队列塞 manifest 唤醒指令 → 长轮询立即返回。
                        mfst.BumpRev(id, kind == "map" ? $"资产变更：地图 {assetId}" : $"资产变更：NPC {assetId}");
                        if (kind == "map" && switchAfter)
                        {
                            // SET_MAP 指令载荷 = BGMAP 资产 hash（固件 dispatch_map 按 hash 匹配
                            // asset_dl_map_path，传地图 id 会静默留在旧地图）。设备拉 manifest/素材
                            // 需要几秒，指令先到时 asset_dl 找不到 BGMAP 条目会静默失败且不自愈
                            // （set_active_map 查无条目即返回）→ 双发：立即 + 15s 后重发一次
                            //（局域网 BGMAP ~1MB <2s，15s 足够；幂等切换无害）。
                            var bgHash = FindBgmapHash(paths, id, assetId);
                            if (bgHash != null)
                            {
                                queue.Enqueue(id, "map", new { id = bgHash });
                                _ = Task.Run(async () =>
                                {
                                    await Task.Delay(TimeSpan.FromSeconds(15));
                                    queue.Enqueue(id, "map", new { id = bgHash });
                                });
                            }
                            else
                            {
                                Console.Error.WriteLine($"[DevicePush] 设备 {id} 地图 {assetId} 未找到 BGMAP 条目，未发切图指令");
                            }
                        }
                        eventLog.Append(id, kind == "map"
                            ? $"推送地图 {assetId}（资产已登记）"
                            : $"推送 NPC {assetId}（资产已登记）");
                    }
                    catch (Exception ex)
                    {
                        Console.Error.WriteLine($"[DevicePush] 设备 {id} 推送 {kind} {assetId} 失败: {ex.Message}");
                        health.RecordEvent(id, "push_error",
                            JsonSerializer.SerializeToElement(new { kind, assetId, error = ex.Message }));
                        eventLog.Append(id, $"推送失败：{kind} {assetId}（{ex.Message}）");
                    }
                });
                return Results.Json(new { ok = true, note = "后台打包中，完成后自动下发" }, statusCode: 202);
            }
            catch (Exception ex)
            {
                // 同步段意外异常（含 DeviceAssetService 解析/前置检查抛错）→ 500 {error}
                Console.Error.WriteLine($"[DevicePush] 设备 {id} 推送请求异常: {ex.Message}");
                return Results.Json(new { error = ex.Message }, statusCode: 500);
            }
        });

        // 设备健康（E11：降级/错误事件聚合 → Web 可见）
        g.MapGet("/devices/{id}/health", (string id, DeviceRegistry reg, HealthReport health) =>
        {
            var dev = reg.Get(id);
            if (dev == null) return NotFoundDevice(id);
            return Results.Json(new
            {
                deviceId = id,
                online = DeviceRegistry.IsOnline(dev),
                lastSeenUtc = dev.LastSeenUtc,
                health = health.GetSummary(id),
            });
        });

        // ── 字体包补链（E12）：手动触发某设备的 16/24/32 三档 FONT 打包 ──
        // 新设备 hello 已自动补（DeviceEndpoints.ScheduleFirstBootProvisioning），这里是修复入口：
        // 字体种子换代 / 设备 font 目录被清 / 想单档重推时用。同步执行（种子 JSON 打包 ~1s，
        // 不占 WZ 锁）；body 可省，{"sizes":[16,24,32]} 指定档位。
        g.MapPost("/devices/{id}/fonts", (string id, FontSizesRequest? body, DeviceRegistry reg,
            FontPackService fonts, DeviceManifestService mfst, DeviceEventLog eventLog) =>
        {
            var dev = reg.Get(id);
            if (dev == null) return NotFoundDevice(id);
            try
            {
                int generated = fonts.EnsureFonts(id, body?.Sizes);
                if (generated > 0) mfst.BumpRev(id, $"字体包已生成（{generated} 档 FONT）");
                eventLog.Append(id, generated > 0
                    ? $"字体包手动补齐：新生成 {generated} 档 FONT"
                    : "字体包手动补齐：已是最新（幂等跳过）");
                return Results.Json(new
                {
                    ok = true,
                    deviceId = id,
                    generated,
                    packedSizes = fonts.PackedSizes(id),
                    manifestRev = mfst.GetCurrentRev(id),
                });
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[FontPack] 设备 {id} 字体打包失败: {ex.Message}");
                return Results.Json(new { error = ex.Message }, statusCode: 500);
            }
        });
    }

    // ── DTO ───────────────────────────────────────────────────────────────

    public sealed class PairRequest
    {
        public string? Code { get; set; }
        public string? Name { get; set; }
    }

    public sealed class DeviceUpdateRequest
    {
        public string? Name { get; set; }
        /// <summary>换宠换装 JSON（按设备隔离，E13）；显式传 null = 清空回默认宠物。</summary>
        public JsonElement? PetConfig { get; set; }
        public DeviceBgmPrefsUpdate? Bgm { get; set; }
        /// <summary>按设备阈值覆盖；null = 不改。传对象 = 覆盖全局。</summary>
        public DeviceThresholdsConfig? Thresholds { get; set; }
    }

    public sealed class DeviceBgmPrefsUpdate
    {
        public string? Source { get; set; }
        public int? Volume { get; set; }
    }

    public sealed class SourceToggleRequest
    {
        public bool? Enabled { get; set; }
    }

    public sealed class CookieRequest
    {
        public string? Cookie { get; set; }
    }

    /// <summary>字体补链请求体（POST /devices/{id}/fonts）；sizes 省略 = 16/24/32。</summary>
    public sealed class FontSizesRequest
    {
        public List<int>? Sizes { get; set; }
    }

    /// <summary>WZ 路径独立校验请求体（POST /settings/validate-path，不落盘）。</summary>
    public sealed class ValidatePathRequest
    {
        public string? Path { get; set; }
    }

    public sealed class OtaRequest
    {
        public string? Ver { get; set; }
    }

    /// <summary>
    /// 素材推送请求体（POST /devices/{id}/push）：kind=map|npc；id=素材编号；
    /// switch=登记成功后是否直接下发切图指令（仅 map 生效，缺省 true）。
    /// </summary>
    /// <summary>查设备 manifest-assets.json 里指定地图的 BGMAP 条目 hash（SET_MAP 指令载荷，固件按 hash 匹配）。</summary>
    private static string? FindBgmapHash(Config.ServerPaths paths, string deviceId, string mapId)
    {
        try
        {
            var indexPath = Path.Combine(paths.ExportDirFor(deviceId), "manifest-assets.json");
            if (!System.IO.File.Exists(indexPath)) return null;
            var root = System.Text.Json.Nodes.JsonNode.Parse(System.IO.File.ReadAllText(indexPath)) as System.Text.Json.Nodes.JsonObject;
            if (root?["assets"] is not System.Text.Json.Nodes.JsonObject assets) return null;
            foreach (var kv in assets)
            {
                if (kv.Value is not System.Text.Json.Nodes.JsonObject e) continue;
                var sel = e["selector"]?.GetValue<string>();
                var map = e["map"]?.GetValue<string>();
                var kind = e["kind"]?.GetValue<string>();
                if (string.Equals(sel, "map", StringComparison.OrdinalIgnoreCase)
                    && map == mapId && string.Equals(kind, "BGMAP", StringComparison.OrdinalIgnoreCase))
                    return kv.Key;
            }
        }
        catch (Exception ex) { Console.Error.WriteLine($"[DevicePush] 查 BGMAP hash 失败: {ex.Message}"); }
        return null;
    }

    public sealed class DevicePushRequest
    {
        public string? Kind { get; set; }
        public string? Id { get; set; }
        public bool? Switch { get; set; }
    }

    /// <summary>
    /// 设备实时指令（POST /devices/{id}/command）：
    /// type = expression | action | bubble（用 Value）| brightness（用 N）| reboot |
    ///        bgm（Value = play|pause|resume|stop|next|prev；音量 Value=vol + N∈[0,100]；
    ///             音源 Value=source + N∈{0,1}）。
    /// 与固件 poller.c 的 payload 约定一一对应（bgm 的 vol/source 走旧口径 {t,v,n}），勿改字段语义。
    /// </summary>
    public sealed class DeviceCommandRequest
    {
        public string? Type { get; set; }
        public string? Value { get; set; }
        public int? N { get; set; }
    }

    public sealed class PresetUpsertRequest
    {
        public string? Name { get; set; }
        public string? Type { get; set; }
        public JsonElement? Data { get; set; }
    }

    /// <summary>PUT /materials/favorites：{ favorites: { map:[], mob:[], npc:[] } }（整表替换）。</summary>
    public sealed class FavoritesRequest
    {
        public Dictionary<string, List<string>>? Favorites { get; set; }
    }

    // ── 视图组装 ──────────────────────────────────────────────────────────

    /// <summary>设备串口日志视图（E14）。契约形态见 docs/ai/keys-touch-handoff.md §7.2：
    /// 未校时（ts 是开机毫秒）时 ClockSynced=false，Web 应回退显示 ReceivedUtc。</summary>
    private static IResult DeviceLogsView(string deviceId, uint sinceSeq, int limit, string? level, string? tag,
        DeviceLogStore logs)
    {
        var r = logs.Query(deviceId, sinceSeq, limit <= 0 ? 200 : Math.Min(limit, 2000), level, tag);
        if (r == null)
        {
            return Results.Json(new
            {
                deviceId,
                lastSeq = 0u,
                clockSynced = false,
                total = 0,
                note = "尚未收到该设备的日志上报（设备每 20s POST /api/device/log；离线或旧固件无此通道）",
                items = Array.Empty<object>(),
            });
        }
        var (items, lastSeq, clockSynced, total, lastReceived) = r.Value;
        return Results.Json(new
        {
            deviceId,
            lastSeq,
            clockSynced,
            total,
            lastReceivedUtc = lastReceived == DateTime.MinValue ? (DateTime?)null : lastReceived,
            note = "设备端环形日志（固件 152 条 → 服务端副本 512 条，超出丢最旧）",
            items = items.Select(l => new
            {
                seq = l.Seq,
                // 未校时时 ts 是开机毫秒：给出接收时间兜底，Web 按 clockSynced 判定
                tsUtc = clockSynced ? DateTimeOffset.FromUnixTimeMilliseconds(l.TsMs).UtcDateTime : (DateTime?)null,
                tsRawMs = l.TsMs,
                t = l.TMs,
                lvl = l.Lvl,
                tag = l.Tag,
                msg = l.Msg,
                receivedUtc = l.ReceivedUtc,
            }).ToList(),
        });
    }

    private static object DeviceCard(DeviceRecord d, DeviceRegistry reg, HealthReport health)
    {
        var h = health.GetSummary(d.DeviceId);
        return new
        {
            d.DeviceId,
            d.Uuid,
            d.Name,
            d.Paired,
            online = DeviceRegistry.IsOnline(d),
            d.LastSeenUtc,
            d.Firmware,
            profile = d.Profile,
            bgm = d.Bgm,
            hasPetConfig = d.PetConfig.HasValue,
            health = h == null ? null : new { h.LastError, h.LastErrorUtc, h.BatteryPercent, h.TotalEvents },
        };
    }

    private static object? Detail(string id, DeviceRegistry reg, ConfigService cfg, HealthReport health)
    {
        var d = reg.Get(id);
        if (d == null) return null;
        var th = d.Thresholds ?? cfg.Current.Device;
        return new
        {
            device = new
            {
                d.DeviceId,
                d.Uuid,
                d.Name,
                d.Paired,
                online = DeviceRegistry.IsOnline(d),
                d.LastSeenUtc,
                d.CreatedAtUtc,
                d.Firmware,
                d.Profile,
                petConfig = d.PetConfig,
                d.Bgm,
                thresholds = th,
                thresholdsSource = d.Thresholds != null ? "device" : "global",
            },
            health = health.GetSummary(id),
        };
    }

    private static IResult NotFoundDevice(string deviceId)
        => Results.Json(new { error = $"设备不存在：{deviceId}" }, statusCode: 404);
}

/// <summary>
/// 纸娃娃预设存取（data/presets/{id}.json，一文件一预设）。
/// 预设 = Web 编辑器存的换装组合，喂给设备选择器「纸娃娃 tab」（E7）。
/// </summary>
public sealed class PresetStore
{
    public sealed class Preset
    {
        public string Id { get; set; } = "";
        public string Name { get; set; } = "";
        /// <summary>paperdoll | npc | map（选择器 tab 归属）。</summary>
        public string Type { get; set; } = "paperdoll";
        public JsonElement? Data { get; set; }
        public DateTime CreatedAtUtc { get; set; } = DateTime.UtcNow;
        public DateTime UpdatedAtUtc { get; set; } = DateTime.UtcNow;
    }

    private readonly string _dir;

    public PresetStore(Config.ServerPaths paths)
    {
        _dir = paths.PresetsDir;
        Directory.CreateDirectory(_dir);
    }

    public List<Preset> List()
        => Directory.EnumerateFiles(_dir, "*.json")
            .Select(f => StorageUtil.ReadJson<Preset>(f))
            .Where(p => p != null)
            .OrderByDescending(p => p!.UpdatedAtUtc)
            .Select(p => p!)
            .ToList();

    public Preset? Get(string id)
    {
        var file = FileOf(id);
        return File.Exists(file) ? StorageUtil.ReadJson<Preset>(file) : null;
    }

    public Preset Create(string name, string type, JsonElement? data)
    {
        var preset = new Preset
        {
            Id = "p-" + Guid.NewGuid().ToString("N")[..8],
            Name = name.Trim(),
            Type = string.IsNullOrWhiteSpace(type) ? "paperdoll" : type.Trim(),
            Data = data,
        };
        StorageUtil.AtomicWriteAllText(FileOf(preset.Id), JsonSerializer.Serialize(preset, StorageUtil.JsonOpts));
        return preset;
    }

    public Preset? Update(string id, string? name, string? type, JsonElement? data)
    {
        var existing = Get(id);
        if (existing == null) return null;
        if (!string.IsNullOrWhiteSpace(name)) existing.Name = name.Trim();
        if (!string.IsNullOrWhiteSpace(type)) existing.Type = type.Trim();
        if (data.HasValue) existing.Data = data;
        existing.UpdatedAtUtc = DateTime.UtcNow;
        StorageUtil.AtomicWriteAllText(FileOf(id), JsonSerializer.Serialize(existing, StorageUtil.JsonOpts));
        return existing;
    }

    public bool Delete(string id)
    {
        var file = FileOf(id);
        if (!File.Exists(file)) return false;
        File.Delete(file);
        return true;
    }

    private string FileOf(string id) => Path.Combine(_dir, StorageUtil.SafeFileId(id) + ".json");
}

/// <summary>
/// 素材收藏存取（E4「地图选择含收藏」/ E7 选择器收藏 tab）。
/// 落盘 data/config/favorites.json，形态与 Web 契约严格一致：
///   { "favorites": { "map": ["200000100", …], "mob": [...], "npc": [...] } }
///
/// 背景：Web 侧收藏原本只有 localStorage 单机模式——client.js 明确写着
/// 「服务端**当前没有**收藏存储/端点（AdminEndpoints.cs 无 /materials/favorites 路由）」，
/// 探测不到就退回本机。服务端补上本端点后，Web 无需改动即自动启用同步
/// （接口需求见 Web/docs/interfaces-needed-from-server.md §T7）。
///
/// 约定：桶名固定三桶（与 Web FAVORITE_BUCKETS 同源）；未知桶名只接受不落盘之外
/// 不做特殊处理（原样保留，便于将来加桶）；id 逐个 trim + 去重 + 上限保护，
/// 防 Web 侧异常写入把文件撑爆。
/// </summary>
public sealed class FavoritesStore
{
    private const int MaxIdsPerBucket = 500;
    private static readonly string[] Buckets = { "map", "mob", "npc" };

    private readonly string _file;
    private readonly object _lock = new();
    private Dictionary<string, List<string>> _buckets = new();

    public FavoritesStore(Config.ServerPaths paths)
    {
        _file = Path.Combine(paths.ConfigDir, "favorites.json");
        Directory.CreateDirectory(paths.ConfigDir);
        Load();
    }

    private void Load()
    {
        try
        {
            var doc = StorageUtil.ReadJson<FavoritesDoc>(_file);
            _buckets = Normalize(doc?.Favorites);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Favorites] 读取失败，按空收藏继续: {ex.Message}");
            _buckets = Normalize(null);
        }
    }

    /// <summary>GET 用：永远返回三桶齐全的对象（缺失桶 = 空数组，前端不用判 undefined）。</summary>
    public Dictionary<string, List<string>> Get()
    {
        lock (_lock) return _buckets.ToDictionary(kv => kv.Key, kv => new List<string>(kv.Value));
    }

    /// <summary>PUT 用：整表替换（Web 侧本来就把三桶全量发上来）。返回落盘后的形态。</summary>
    public Dictionary<string, List<string>> Replace(Dictionary<string, List<string>>? incoming)
    {
        lock (_lock)
        {
            _buckets = Normalize(incoming);
            StorageUtil.AtomicWriteAllText(_file,
                JsonSerializer.Serialize(new FavoritesDoc { Favorites = _buckets }, StorageUtil.JsonOpts));
            return _buckets.ToDictionary(kv => kv.Key, kv => new List<string>(kv.Value));
        }
    }

    private static Dictionary<string, List<string>> Normalize(Dictionary<string, List<string>>? src)
    {
        var outp = new Dictionary<string, List<string>>();
        foreach (var b in Buckets)
        {
            var list = new List<string>();
            if (src != null && src.TryGetValue(b, out var ids) && ids != null)
            {
                foreach (var raw in ids)
                {
                    var id = raw?.Trim();
                    if (string.IsNullOrEmpty(id)) continue;
                    if (!list.Contains(id)) list.Add(id);
                    if (list.Count >= MaxIdsPerBucket) break;
                }
            }
            outp[b] = list;
        }
        // 未知桶原样保留（将来加桶时旧数据不丢），同样做去重与上限
        if (src != null)
        {
            foreach (var kv in src)
            {
                if (outp.ContainsKey(kv.Key)) continue;
                var list = new List<string>();
                foreach (var raw in kv.Value ?? new List<string>())
                {
                    var id = raw?.Trim();
                    if (string.IsNullOrEmpty(id) || list.Contains(id)) continue;
                    list.Add(id);
                    if (list.Count >= MaxIdsPerBucket) break;
                }
                outp[kv.Key] = list;
            }
        }
        return outp;
    }

    private sealed class FavoritesDoc
    {
        public Dictionary<string, List<string>> Favorites { get; set; } = new();
    }
}
