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

        // ── 纸娃娃预设 CRUD（data/presets/，供设备选择器「纸娃娃 tab」，E7/E4）──
        g.MapGet("/presets", (PresetStore presets) => Results.Json(new { presets = presets.List() }));
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

        g.MapGet("/music/sources", (BgmRouter router) => Results.Json(new
        {
            sources = router.Sources.Select(s => new
            {
                name = s.Name,
                enabled = s.IsEnabled,
                health = s.Health(),
            }).ToList(),
        }));

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

        // cookie 导入（QQ）：只落配置 + 保存时间；有效期告警在 M10 网关接入后补
        g.MapPost("/music/sources/qq/cookie", (CookieRequest body, ConfigService cfg, QqMusicSource qq) =>
        {
            var (applied, errors) = cfg.Update(c => c.QqMusic.Cookie = body?.Cookie?.Trim() ?? "");
            if (applied == null) return Results.Json(new { errors }, statusCode: 400);
            return Results.Json(new
            {
                ok = true,
                imported = !string.IsNullOrEmpty(body?.Cookie),
                health = qq.Health(),
                note = "cookie 已保存；曲库/取链待 M10 node 网关接入（cookie 导入与启停已可用）",
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

    public sealed class PresetUpsertRequest
    {
        public string? Name { get; set; }
        public string? Type { get; set; }
        public JsonElement? Data { get; set; }
    }

    // ── 视图组装 ──────────────────────────────────────────────────────────

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
