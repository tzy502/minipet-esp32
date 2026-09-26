using System.Text.Json;
using System.Text.Json.Serialization;
using MinipetServer.Api;
using MinipetServer.Config;
using MinipetServer.Device;
using MinipetServer.Health;
using MinipetServer.Manifest;
using MinipetServer.Music;

var builder = WebApplication.CreateBuilder(args);

// data/ 目录布局（E3：无数据库，一切 JSON 落 data/；MINIPET_DATA_DIR 可覆盖）
var dataDir = Environment.GetEnvironmentVariable("MINIPET_DATA_DIR")
              ?? Path.Combine(builder.Environment.ContentRootPath, "data");
var paths = new ServerPaths(dataDir);
paths.EnsureAll();

// ── DI ────────────────────────────────────────────────────────────────────
builder.Services.AddSingleton(paths);
builder.Services.AddSingleton<ConfigService>();          // data/config/appsettings.json + 热重载
builder.Services.AddSingleton<DeviceRegistry>();         // 设备表（E13）
builder.Services.AddSingleton<CommandQueue>();           // 每设备指令队列（E2）
builder.Services.AddSingleton<HealthReport>();           // 事件聚合（E11）
builder.Services.AddSingleton<DeviceManifestService>();  // 按设备 manifest + rev
builder.Services.AddSingleton<WzMusicSource>();
builder.Services.AddSingleton<QqMusicSource>();
builder.Services.AddSingleton<IMusicSource>(sp => sp.GetRequiredService<WzMusicSource>());
builder.Services.AddSingleton<IMusicSource>(sp => sp.GetRequiredService<QqMusicSource>());
builder.Services.AddSingleton<BgmRouter>();              // 音源路由 + 同源降级（E8）
builder.Services.AddSingleton<ThumbService>();           // 64×64 缩略图 + 磁盘缓存
builder.Services.AddSingleton<PresetStore>();            // 纸娃娃预设（data/presets/）

builder.Services.ConfigureHttpJsonOptions(o =>
{
    o.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.CamelCase;
    o.SerializerOptions.DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull;
});

var app = builder.Build();

// 立即实例化需要早绑定事件的单例（ConfigService.Changed → manifest rev / 日志）
var cfgSvc = app.Services.GetRequiredService<ConfigService>();
var router = app.Services.GetRequiredService<BgmRouter>();
_ = app.Services.GetRequiredService<DeviceManifestService>();
cfgSvc.Changed += e =>
    app.Logger.LogInformation("[Config] 已热重载（external={External}，WZ={Wz}）", e.External, e.New.Wz.DataPath);
router.Failover += e =>
    app.Logger.LogWarning("[BgmRouter] failover {Kind} source={Source} track={Track}: {Reason}", e.Kind, e.Source, e.TrackId, e.Reason);

// ── 中间件：静态托管 Vue 产物（wwwroot，E3 单容器同源）；API 路由优先 ─────
app.UseDefaultFiles();
app.UseStaticFiles();

// ── /api/health（M5 部署验收锚点）────────────────────────────────────────
app.MapGet("/api/health", (ConfigService c, DeviceRegistry reg) => Results.Json(new
{
    ok = true,
    service = "minipet-server",
    proto = DeviceManifestService.Proto,
    timeUtc = DateTime.UtcNow,
    wzPathExists = Directory.Exists(c.Current.Wz.DataPath),
    wzDataPath = c.Current.Wz.DataPath,
    devices = reg.List().Count,
    qqEnabled = c.Current.QqMusic.Enabled,
}));

DeviceEndpoints.Map(app);
AdminEndpoints.Map(app);

// ── SPA fallback（E4 问题2 修复 2026-09-26）：Vue Router 用 createWebHistory
//    （URI 路径模式），直接访问/刷新 /materials 等非根路径时静态文件中间件
//    找不到对应文件 → 404。fallback 到 index.html 交给前端路由接管。
//    注意必须放在所有 API 端点之后：/api/* 已匹配则不会走到这里。
app.MapFallbackToFile("/index.html");

app.Run();
