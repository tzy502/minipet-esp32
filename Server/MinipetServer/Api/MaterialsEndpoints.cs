using MinipetServer.Services;

namespace MinipetServer.Api;

/// <summary>
/// Web 素材浏览页素材枚举 API（素材浏览页真实化 · 服务端）：
/// GET /api/admin/materials?kind=map|mob|npc → { kind, total, items:[{id,name}] }。
/// 替换前端写死的 10 个静态 id 占位（缩略图由前端拼 /api/admin/thumb?type={kind}&amp;id={id}，
/// ThumbService 侧 mob/npc/map 已接真实 WZ 渲染）。
/// - 枚举：ListMapIds()/ListMobIds()/ListNpcIds()（实测 21422/11118/15789 条，裸 id 无前导零）；
/// - 中文名：GetMapName/GetMobName/GetNpcName，空回退 "{中文类目}_{id}"（地图/怪物/NPC）；
///   GetMapName 依赖启动时后台构建的地图目录，未就绪时其内部回退 map_{id} 直接用，不阻塞等待；
/// - 排序：int.Parse(id) 数值升序，解析失败排最后（LINQ 稳定排序保持失败者原相对顺序）；
/// - kind 未知/缺省 → 400；WZ 未加载 → 503（同 AdminCatalogEndpoints 降级口径）。
/// 结果按 kind 内存缓存，订阅 WzService.WzReloaded 重载代际事件整体失效（订阅口径同
/// AdminCatalogEndpoints.OnWzReloaded，参数代际号仅作失效触发）。局域网信任模型：v1 无鉴权。
/// </summary>
public static class MaterialsEndpoints
{
    public static void Map(WebApplication app)
    {
        var g = app.MapGroup("/api/admin");

        g.MapGet("/materials", (string? kind, WzService wz) =>
        {
            // kind 必须是 map/mob/npc 之一
            if (kind == null || !Kinds.TryGetValue(kind, out var def))
                return Results.Json(new
                {
                    error = $"kind 非法：{kind ?? "<null>"}（可用：{string.Join("/", Kinds.Keys)}）",
                }, statusCode: 400);

            // WZ 未加载 → 503（不做全量枚举；加载态由 WzService.LoadWz 维护）
            if (!wz.IsLoaded)
                return Results.Json(new { error = "WZ 未加载" }, statusCode: 503);

            EnsureSubscribed(wz);
            var items = GetOrBuild(wz, def);
            return Results.Json(new { kind = def.Key, total = items.Count, items });
        });
    }

    // ── 类目表（素材浏览页三个 tab：地图/怪物/NPC）────────────────────────────

    /// <summary>类目定义：Key=API kind 值；Cn=中文类目名（回退名前缀）；
    /// ListIds=全量 id 枚举；GetName=中文名查询（空串 = 无名）。</summary>
    private sealed class KindDef
    {
        public KindDef(string key, string cn, Func<WzService, List<string>> listIds,
            Func<WzService, string, string> getName)
        {
            Key = key;
            Cn = cn;
            ListIds = listIds;
            GetName = getName;
        }

        public string Key { get; }
        public string Cn { get; }
        public Func<WzService, List<string>> ListIds { get; }
        public Func<WzService, string, string> GetName { get; }
    }

    private static readonly Dictionary<string, KindDef> Kinds = new()
    {
        ["map"] = new("map", "地图", wz => wz.ListMapIds(), (wz, id) => wz.GetMapName(id)),
        ["mob"] = new("mob", "怪物", wz => wz.ListMobIds(), (wz, id) => wz.GetMobName(id)),
        ["npc"] = new("npc", "NPC", wz => wz.ListNpcIds(), (wz, id) => wz.GetNpcName(id)),
    };

    // ── 结果缓存：per kind，WZ 重载整体失效（口径同 AdminCatalogEndpoints）────

    private static readonly object _cacheLock = new();
    private static Dictionary<string, List<object>> _cache = new();
    // 本地代际：WzReloaded 时自增；构建前后比对不符不发布（构建期间 WZ 重载 → 结果过期，下个请求重建）
    private static int _gen;
    // 已订阅的 WzService 实例（DI 单例正常恒为同一实例；换实例时解绑重订并清缓存）
    private static WzService? _subscribed;

    private static void EnsureSubscribed(WzService wz)
    {
        if (_subscribed == wz) return;
        lock (_cacheLock)
        {
            if (_subscribed == wz) return;
            if (_subscribed != null) _subscribed.WzReloaded -= OnWzReloaded;
            _subscribed = wz;
            _cache = new();
            _gen++;
            // 参数 = WZ 重载代际号，本类只把它当失效触发（口径同 AdminCatalogEndpoints.OnWzReloaded）
            wz.WzReloaded += OnWzReloaded;
        }
    }

    private static void OnWzReloaded(int wzGeneration)
    {
        lock (_cacheLock)
        {
            _gen++;
            _cache = new();
        }
    }

    /// <summary>取缓存；未命中则锁外全量构建（并发首建可能重复算一次，无害——
    /// WzLib 内部自有锁，且名字查询各有自身缓存），完成后代际相符才发布。</summary>
    private static List<object> GetOrBuild(WzService wz, KindDef def)
    {
        int gen;
        lock (_cacheLock)
        {
            if (_cache.TryGetValue(def.Key, out var cached)) return cached;
            gen = _gen;
        }
        var built = Build(wz, def);
        lock (_cacheLock)
        {
            if (gen == _gen) _cache[def.Key] = built;
        }
        return built;
    }

    // ── 构建：全量枚举 → 数值排序 → 查中文名出 DTO ────────────────────────────

    private static List<object> Build(WzService wz, KindDef def)
    {
        // 数值升序；解析失败排最后（OrderBy/ThenBy 稳定排序 → 失败者保持原相对顺序）
        var ids = def.ListIds(wz)
            .Select(id => (id, ok: int.TryParse(id, out var n), n))
            .OrderBy(t => t.ok ? 0 : 1)
            .ThenBy(t => t.n)
            .Select(t => t.id)
            .ToList();

        return ids.Select(id =>
        {
            var cn = def.GetName(wz, id);
            // GetMapName 的 miss 哨兵是英文回退 map_{id}（地图目录未就绪/真无名都会是它），
            // 统一改写为中文回退 {类目}_{id}，与其余类目口径一致
            if (!string.IsNullOrEmpty(cn) && cn == $"map_{id}") cn = "";
            return (object)new
            {
                id,
                name = string.IsNullOrEmpty(cn) ? $"{def.Cn}_{id}" : cn,
            };
        }).ToList();
    }
}
