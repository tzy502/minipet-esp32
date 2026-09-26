using MinipetServer.Services;

namespace MinipetServer.Api;

/// <summary>
/// Web 纸娃娃编辑器素材目录 API（docs/ai/web-paperdoll-alignment.md 问题④ 定稿 · 第三章 3.1-3.3/3.5 + 第五章服务端）：
/// GET /api/admin/catalog?part={key}&amp;gender={0|1} → { part, total, items:[{id,name,icon,img?}] }。
/// 逻辑对齐桌面版 MaterialBrowserWindow.Logic.cs 的 LoadFromWz / AddCharacterCategory /
/// AddAccessoryCategories / MatchesGender（千位表修正版）：
/// - 枚举：GetDirectoryChildren("Character/{folder}")（全量目录子节点，不用 _Canvas 老写法防缺件）；
/// - 饰品：Character/Accessory 按 id/10000 前缀拆 面饰101/眼饰102/耳环103，其余前缀丢弃；
/// - 椅子：Item/Install/{img文件名} 内 GetImgChildren 数字 id（img 是 Wz_Image 节点，
///   GetDirectoryChildren 对其返回空——必须传 Name 带 .img，传目录 Id 得空列表），
///   硬过滤 3010000 ≤ id &lt; 3021000，icon 额外带 &amp;img={所在img文件名}；
/// - 皮肤：Character 目录 2000–2999（换肤 Body id），名字查 GetItemName("Body", id)；
/// - 中文名：GetItemName(folder, id)，folder 用 WZ folder 名（与桌面一致——注意 overall 枚举
///   Character/Longcoat 名字查 Longcoat，mount 枚举 Character/TamingMob 名字查 TamingMob），
///   空回退 "{中文类目}_{id}"（回退名参与前端发型折叠判定 IsFallbackHairName）；
/// - 性别过滤：仅发型/脸型，gender 缺省或非 0/1 不过滤，其他类目永不按性别过滤；
/// - 排序：int.Parse(id) 数值升序，解析失败排最后（LINQ 稳定排序保持失败者原相对顺序）；
/// - icon：/api/admin/thumb?type={part}&amp;folder={WZ folder}&amp;id={id}（ThumbService 并行接真实渲染）。
/// 结果按 (part, genderFilter) 内存缓存（首次冷加载 Hair 1.7 万条逐条查中文名约数秒属正常），
/// 订阅 WzService.WzReloaded 重载代际事件整体失效（订阅口径同 MusicCatalogService.OnWzReloaded，
/// 参数代际号仅作失效触发）。WZ 未加载 → 503（WzService 经构造注入，DI 单例）。
/// 局域网信任模型：v1 无鉴权（同 AdminEndpoints）。
/// </summary>
public static class AdminCatalogEndpoints
{
    public static void Map(WebApplication app)
    {
        var g = app.MapGroup("/api/admin");

        g.MapGet("/catalog", (string? part, string? gender, WzService wz) =>
        {
            // part 必须是 16 装备槽 + 皮肤 之一
            if (part == null || !Parts.TryGetValue(part, out var def))
                return Results.Json(new
                {
                    error = $"part 非法：{part ?? "<null>"}（可用：{string.Join("/", Parts.Keys)}）",
                }, statusCode: 400);

            // WZ 未加载 → 503（不做全量枚举；加载态由 WzService.LoadWz 维护）
            if (!wz.IsLoaded)
                return Results.Json(new { error = "WZ 未加载" }, statusCode: 503);

            // gender 仅对发型/脸型生效；缺省或非 0/1 = 不过滤
            int? genderFilter = null;
            if ((def.Key == "hair" || def.Key == "face") && (gender == "0" || gender == "1"))
                genderFilter = gender == "0" ? 0 : 1;

            EnsureSubscribed(wz);
            var items = GetOrBuild(wz, def, genderFilter);
            return Results.Json(new { part = def.Key, total = items.Count, items });
        });
    }

    // ── 类目表（与桌面版 SettingsWindow.Paperdoll 16 槽 + 皮肤一致）────────────

    private enum PartKind
    {
        Normal,    // Character/{NameFolder} 目录枚举
        Accessory, // Character/Accessory 枚举后按 AccPrefix（id/10000）拆
        Chair,     // Item/Install 各 img 内枚举
        Skin,      // Character 目录 2000-2999
    }

    /// <summary>类目定义：Key=API part 值；Cn=中文类目名（回退名前缀）；NameFolder=GetItemName
    /// 与 icon folder 参数共用的 WZ folder 名（Normal 类目同时就是 Character/ 下的枚举目录名）。</summary>
    private sealed class PartDef
    {
        public PartDef(string key, string cn, string nameFolder, PartKind kind, int accPrefix = 0)
        {
            Key = key;
            Cn = cn;
            NameFolder = nameFolder;
            Kind = kind;
            AccPrefix = accPrefix;
        }

        public string Key { get; }
        public string Cn { get; }
        public string NameFolder { get; }
        public PartKind Kind { get; }
        /// <summary>饰品拆分前缀（101 面饰 / 102 眼饰 / 103 耳环），仅 Kind=Accessory 有效。</summary>
        public int AccPrefix { get; }
    }

    private static readonly Dictionary<string, PartDef> Parts = new()
    {
        ["hair"] = new("hair", "发型", "Hair", PartKind.Normal),
        ["face"] = new("face", "脸型", "Face", PartKind.Normal),
        ["cap"] = new("cap", "帽子", "Cap", PartKind.Normal),
        ["cape"] = new("cape", "披风", "Cape", PartKind.Normal),
        ["coat"] = new("coat", "上衣", "Coat", PartKind.Normal),
        ["overall"] = new("overall", "套服", "Longcoat", PartKind.Normal),
        ["pants"] = new("pants", "裤子", "Pants", PartKind.Normal),
        ["shoes"] = new("shoes", "鞋子", "Shoes", PartKind.Normal),
        ["weapon"] = new("weapon", "武器", "Weapon", PartKind.Normal),
        ["shield"] = new("shield", "盾牌", "Shield", PartKind.Normal),
        ["glove"] = new("glove", "手套", "Glove", PartKind.Normal),
        ["faceAccessory"] = new("faceAccessory", "面饰", "Accessory", PartKind.Accessory, 101),
        ["eyeAccessory"] = new("eyeAccessory", "眼饰", "Accessory", PartKind.Accessory, 102),
        ["earring"] = new("earring", "耳环", "Accessory", PartKind.Accessory, 103),
        ["mount"] = new("mount", "坐骑", "TamingMob", PartKind.Normal),
        ["chair"] = new("chair", "椅子", "Install", PartKind.Chair),
        ["skin"] = new("skin", "皮肤", "Body", PartKind.Skin),
    };

    // ── 结果缓存：per (part, genderFilter)，WZ 重载整体失效 ─────────────────────

    private static readonly object _cacheLock = new();
    private static Dictionary<(string Part, int? Gender), List<object>> _cache = new();
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
            // 参数 = WZ 重载代际号，本类只把它当失效触发（口径同 MusicCatalogService.OnWzReloaded）
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
    /// WzLib 内部自有锁，且 GetItemName 有自身缓存），完成后代际相符才发布。</summary>
    private static List<object> GetOrBuild(WzService wz, PartDef def, int? genderFilter)
    {
        var key = (def.Key, genderFilter);
        int gen;
        lock (_cacheLock)
        {
            if (_cache.TryGetValue(key, out var cached)) return cached;
            gen = _gen;
        }
        var built = Build(wz, def, genderFilter);
        lock (_cacheLock)
        {
            if (gen == _gen) _cache[key] = built;
        }
        return built;
    }

    // ── 构建：裸 id 枚举 → 性别过滤 → 数值排序 → 查中文名出 DTO ────────────────

    private static List<object> Build(WzService wz, PartDef def, int? genderFilter)
    {
        var ids = def.Kind switch
        {
            PartKind.Chair => EnumChairs(wz),
            PartKind.Skin => EnumSkins(wz),
            // 饰品三类同枚举 Accessory 后按 id/10000 前缀拆（实测 101→764 / 102→319 / 103→312，其余丢弃）
            PartKind.Accessory => wz.GetDirectoryChildren("Character/Accessory")
                .Where(c => int.TryParse(c.Id, out var n) && n / 10000 == def.AccPrefix)
                .Select(c => (Id: c.Id, Img: (string?)null))
                .ToList(),
            _ => wz.GetDirectoryChildren($"Character/{def.NameFolder}")
                .Select(c => (Id: c.Id, Img: (string?)null))
                .ToList(),
        };

        // 性别过滤只发生在发型/脸型（GetOrBuild 的 key 已保证仅这两类带 genderFilter）
        if (genderFilter.HasValue)
            ids = ids.Where(e => MatchesGender(def.Key, e.Id, genderFilter.Value)).ToList();

        // 数值升序；解析失败排最后（OrderBy/ThenBy 稳定排序 → 失败者保持原相对顺序）
        ids = ids.Select(e => (e, ok: int.TryParse(e.Id, out var n), n))
            .OrderBy(t => t.ok ? 0 : 1)
            .ThenBy(t => t.n)
            .Select(t => t.e)
            .ToList();

        return ids.Select(e =>
        {
            var cn = wz.GetItemName(def.NameFolder, e.Id);
            return (object)new
            {
                id = e.Id,
                name = string.IsNullOrEmpty(cn) ? $"{def.Cn}_{e.Id}" : cn,
                // type 恒为 part（ThumbService 按 type=part + folder/img 定位部件图标）
                icon = $"/api/admin/thumb?type=part&folder={def.NameFolder}&id={e.Id}"
                       + (e.Img != null ? $"&img={e.Img}" : ""),
                img = e.Img, // 仅椅子有值（"03010.img" 等）；null 由全局 WhenWritingNull 省略
            };
        }).ToList();
    }

    /// <summary>启动预热（WarmupService 调）：全 part 构建一遍（含中文名缓存填充），返回预热条目总数。</summary>
    public static int WarmupAll(WzService wz)
    {
        int total = 0;
        foreach (var def in Parts.Values) total += GetOrBuild(wz, def, null).Count;
        return total;
    }

    /// <summary>椅子枚举：Item/Install 下每个 .img（取 Name 传 GetImgChildren——传目录 Id 得空列表，
    /// 桌面版已踩坑）→ img 内数字 id，硬过滤 3010000 ≤ id &lt; 3021000（Install 内 0304.img 等是其他家具）。</summary>
    private static List<(string Id, string? Img)> EnumChairs(WzService wz)
    {
        var result = new List<(string Id, string? Img)>();
        foreach (var (_, imgFile) in wz.GetDirectoryChildren("Item/Install"))
        {
            foreach (var id in wz.GetImgChildren($"Item/Install/{imgFile}"))
            {
                if (int.TryParse(id, out var n) && n >= 3010000 && n < 3021000)
                    result.Add((id, imgFile));
            }
        }
        return result;
    }

    /// <summary>皮肤枚举：Character 目录 2000-2999（换肤 Body id；head=body+10000 成对，不单列）。</summary>
    private static List<(string Id, string? Img)> EnumSkins(WzService wz)
        => wz.GetDirectoryChildren("Character")
            .Where(c => int.TryParse(c.Id, out var b) && b >= 2000 && b <= 2999)
            .Select(c => (Id: c.Id, Img: (string?)null))
            .ToList();

    /// <summary>发型/脸型性别过滤（逐条对齐桌面 MaterialBrowserWindow.MatchesGender 千位表修正版，
    /// 2026-08-16 起弃用 n&gt;=31000 老规则——男发 36633 千位 6 曾被误杀）：
    /// 千位 = (id/1000)%10；发型 0/3/5/6=男、1/4/7/8=女、其余通用；脸型 0/3/5/7=男、1/4/6/8=女、其余通用。
    /// 千位为通用或等于当前 gender 才保留；非发型/脸型不过滤。</summary>
    internal static bool MatchesGender(string partKey, string id, int gender)
    {
        if (partKey != "hair" && partKey != "face") return true;
        if (!int.TryParse(id, out var n)) return true;
        int tag = (n / 1000) % 10;
        int itemGender = partKey == "hair"
            ? tag switch { 0 or 3 or 5 or 6 => 0, 1 or 4 or 7 or 8 => 1, _ => 2 }
            : tag switch { 0 or 3 or 5 or 7 => 0, 1 or 4 or 6 or 8 => 1, _ => 2 };
        return itemGender == 2 || itemGender == gender;
    }
}
