using System.Text.Json;
using System.Text.Json.Serialization;
using MinipetServer.Api;
using MinipetServer.Config;
using MinipetServer.Device;
using MinipetServer.Health;
using MinipetServer.Manifest;
using MinipetServer.Music;
using MinipetServer.Services;

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
builder.Services.AddSingleton<DeviceEventLog>();         // 设备事件环形日志（E14，logs 页拉取）
builder.Services.AddSingleton<HealthReport>();           // 事件聚合（E11）
builder.Services.AddSingleton<DeviceManifestService>();  // 按设备 manifest + rev
builder.Services.AddSingleton<WzMusicSource>();
builder.Services.AddSingleton<QqMusicSource>();
builder.Services.AddSingleton<IMusicSource>(sp => sp.GetRequiredService<WzMusicSource>());
builder.Services.AddSingleton<IMusicSource>(sp => sp.GetRequiredService<QqMusicSource>());
builder.Services.AddSingleton<BgmRouter>();              // 音源路由 + 同源降级（E8）
builder.Services.AddSingleton<CacheManager>();           // WZ 位图/精灵 LRU（纸娃娃缩略图渲染共享）
builder.Services.AddSingleton<WzService>();              // WZ 读取（catalog API / 纸娃娃真实缩略图共用）
builder.Services.AddSingleton<ThumbService>();           // 64×64 缩略图 + 磁盘缓存（part/paperdoll 走真实渲染）
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

// ── WZ 启动加载（catalog API / 纸娃娃真实缩略图依赖）──────────────────────
// 后台线程加载（全量目录加载耗时数秒，不阻塞监听）；路径取运行时配置 Wz.DataPath
// （设置页可改），热重载换路径时自动重载。失败只记日志：相关端点按 IsLoaded 降级
// （catalog 503 / 缩略图回退占位），服务本身照常起。
var wzSvc = app.Services.GetRequiredService<WzService>();
void LoadWzFromConfig(string dataPath)
{
    var (ok, err) = wzSvc.LoadWz("", dataPath);
    if (ok)
    {
        app.Logger.LogInformation("[Wz] WZ 已加载：{Path}", dataPath);
        // clock_table 魔法值校准（E9/R15）：WZ 就绪后把世界锚点换算成烘焙视口坐标补进
        // config（只补缺失/替换样例占位，Web 改过的值不动；写入 → Changed → manifest rev+1）
        try { ClockTableSeeder.Run(wzSvc, cfgSvc, app.Logger); }
        catch (Exception ex) { app.Logger.LogWarning("[Clock] 校准调度失败：{Message}", ex.Message); }
    }
    else
    {
        app.Logger.LogWarning("[Wz] WZ 加载失败（{Path}）：{Error} —— catalog/纸娃娃缩略图将降级", dataPath, err);
    }
}
_ = Task.Run(() => LoadWzFromConfig(cfgSvc.Current.Wz.DataPath));
cfgSvc.Changed += e =>
{
    if (!string.Equals(e.New.Wz.DataPath, e.Old.Wz.DataPath, StringComparison.OrdinalIgnoreCase))
        Task.Run(() => LoadWzFromConfig(e.New.Wz.DataPath));
};

// ── 神子默认预设种子（首启无任何纸娃娃预设时注册，Web 编辑器/设备选择器可见）──
var presetStore = app.Services.GetRequiredService<PresetStore>();
if (presetStore.List().All(p => p.Type != "paperdoll"))
{
    try
    {
        var seedFile = Path.Combine(MiniPet.Export.DeviceProfile.FindSeedRoot(), "default-appearance.json");
        var seedJson = JsonDocument.Parse(File.ReadAllText(seedFile)).RootElement.Clone();
        presetStore.Create("神子", "paperdoll", seedJson);
        app.Logger.LogInformation("[Preset] 已注册默认纸娃娃预设「神子」（seed: {File}）", seedFile);
    }
    catch (Exception ex)
    {
        app.Logger.LogWarning("[Preset] 神子预设种子注册失败：{Message}", ex.Message);
    }
}

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
AdminCatalogEndpoints.Map(app);
MaterialsEndpoints.Map(app);

// ── SPA fallback（E4 问题2 修复 2026-09-26）：Vue Router 用 createWebHistory
//    （URI 路径模式），直接访问/刷新 /materials 等非根路径时静态文件中间件
//    找不到对应文件 → 404。fallback 到 index.html 交给前端路由接管。
//    注意必须放在所有 API 端点之后：/api/* 已匹配则不会走到这里。
app.MapFallbackToFile("/index.html");

app.Run();
