using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Nodes;
using MiniPet.Export;
using MinipetServer.Config;

namespace MinipetServer.Services;

/// <summary>
/// 设备资产登记服务（2026-09-26 E7 补链）：把地图 / 怪物NPC 素材打包登记进设备 manifest
/// ——此前只有纸装扮有这条链路（PaperdollPackService）。模式对齐 PaperdollPackService：
/// AssetExporter 出资产 → 写 {hash}.mpak 到 data/cache/export/{deviceId}/ → 合并
/// manifest-assets.json；per-device 锁串行化（防同设备并发重复写索引）；幂等键 =
/// selector + 业务 id（map: 条目 extra.map==mapId；npc: extra.entity=="npc:{npcId}"），
/// 已登记则跳过不再重打。
/// 与纸装扮的语义差异：地图/NPC 是**累积收藏**——合并时**不删旧条目**（同 hash 覆盖无害），
/// 与装扮的「替换旧 selector=paperdoll」不同。
/// 字段口径：kind 用固件 asset_dl kind_dir 白名单的大写形态（"PARTS"/"LAYOUT"/"BGMAP"…，
/// 固件 strcmp 精确匹配，"Parts"/"parts" 会被拒收不下载）；selector 用小写
/// （"map"/"npc"，同固件 selector 比对口径）；其余字段（bytes/file/url/label/entity/
/// action/map/thumb/bounds/origin）对齐 ManifestBuilder.EntryToJson。
/// </summary>
public sealed class DeviceAssetService
{
    private readonly WzService _wz;
    private readonly ServerPaths _paths;
    private readonly ConcurrentDictionary<string, object> _deviceLocks = new();

    private readonly Device.DeviceRegistry _reg;   // E13：取设备 hello 上报的 profile

    public DeviceAssetService(WzService wz, ServerPaths paths, Device.DeviceRegistry reg)
    {
        _wz = wz ?? throw new ArgumentNullException(nameof(wz));
        _paths = paths ?? throw new ArgumentNullException(nameof(paths));
        _reg = reg ?? throw new ArgumentNullException(nameof(reg));
    }

    /// <summary>服务端设备记录里的 profile → 导出器 DeviceProfile（E13）。
    /// 字段口径与 hello 一致（w/h/shape/psram/audio）；null 或 w/h 缺失返回 null，
    /// 由导出器回落默认 480×480。 */
    private static MiniPet.Export.DeviceProfile? ToExportProfile(Device.DeviceProfile? p)
    {
        if (p == null || p.W <= 0 || p.H <= 0) return null;
        return new MiniPet.Export.DeviceProfile
        {
            Name = $"device-reported-{p.W}x{p.H}",
            W = p.W,
            H = p.H,
            Shape = string.IsNullOrWhiteSpace(p.Shape) ? "square" : p.Shape,
            PsramMb = p.Psram,
            Audio = p.Audio,
        };
    }

    /// <summary>
    /// 确保设备的 manifest-assets.json 已登记该地图资产包（BGMAP + 条带小 PARTS + 缩略图）。
    /// 返回是否实际生成新包（false = 索引已有该地图，幂等跳过）。
    /// 地图数据缺失/导出为空抛异常，由上层记录。
    /// <paramref name="fullMap"/>（R2 整图口径 2026-10-01，**默认 false**）：
    /// true = 按整图世界尺寸 1x 导出（vw/vh = 整图 bbox + 尾部地面表扩展块）；
    /// false = 现网 240×240 窗口口径（逐字节不变）。
    /// <paramref name="tiled"/>（分块布局 2026-10-01，**默认 true**）：整图包的 static/tile/条带
    /// 三层按 128×128 世界像素瓦片存储（BGMAP flags bit1=1）。仅 fullMap 时生效（窗口包无承载位）。
    /// 幂等口径：按条目里的 `viewport` 字段（"full"/"window"，缺失=window 兼容旧索引）**与**
    /// `layout` 字段（"tiled"，缺失=逐行兼容旧索引）**与请求口径都一致才跳过**；
    /// 口径不同则重导并替换该地图条目（否则切不回/切不过去，新固件也永远拿不到分块包）。
    /// 同步方法（打包本体在 WzService 内部锁内串行，同 EnsurePacked 口径；
    /// 需要异步语义由调用方 Task.Run 放后台线程）。
    /// </summary>
    public bool EnsureMapAsync(string deviceId, string mapId, bool fullMap = false, bool tiled = true)
    {
        ValidateIds(deviceId, mapId, "地图 id");
        mapId = mapId.Trim();
        bool wantTiled = fullMap && tiled;      // 窗口包无法承载分块标志（flags 在尾扩展块）
        lock (_deviceLocks.GetOrAdd(deviceId, _ => new object()))
        {
            var deviceDir = DeviceDir(deviceId);
            var root = ReadIndex(Path.Combine(deviceDir, ManifestBuilder.AssetsManifestFileName));
            string wantViewport = fullMap ? "full" : "window";
            if (HasEntry(root, selector: "map", key: "map", value: mapId, viewport: wantViewport,
                         layout: wantTiled ? "tiled" : "rows")) return false;

            var warnings = new List<string>();
            /* E13：按该设备 hello 上报的 profile 烘焙（w/h/shape/psram/audio）；
 * 取不到才回落默认 480×480（见 AssetExporter.ExportMapAssets 注释）。 */
            var devProfile = _reg.Get(deviceId)?.Profile;
            if (tiled && !fullMap)
                Console.WriteLine($"[DevicePush] 地图 {mapId} 为窗口口径 → 分块(tiled)不可用，按逐行导出");
            var assets = new AssetExporter(_wz).ExportMapAssets(mapId, warnings, ToExportProfile(devProfile),
                fullMap, wantTiled);
            // 口径切换（window ↔ full）时替换：先摘掉该地图的旧 BGMAP 条目，否则 manifest 里
            // 会同时存在两条同 map 的 selector=map 条目（设备列表出现重复项/切图 hash 取错）。
            RemoveMapEntries(root, mapId);
            /* 【同图多版本去重 2026-10-01 · 真机根因】清单是按 **content hash** 合并的，而
             * 「整图 vs 窗口」是同一张 map_id 的两种导出 → 重推整图包会生成**新 hash**，
             * 老窗口包条目仍留在 manifest 里 → 服务端 FindMap / 设备 asset_dl 命中哪条看顺序，
             * 真机表现 = 「按整图重推了，但相机仍报非整图包 / 背景还是小窗口」（用户报障：
             * 「非整图包 的直接给我覆盖了」）。修法：登记整图包后，把同 map_id 的**旧 BGMAP
             * 条目从 manifest 剔除**（磁盘文件保留不删，避免误伤他人缓存）。 */
            if (fullMap)
            {
                if (root["assets"] is JsonObject assetsObj)
                {
                    var newHashes = new HashSet<string>(assets.Select(a => $"{a.Hash:x16}"), StringComparer.Ordinal);
                    var drop = new List<string>();
                    foreach (var kv in assetsObj)
                    {
                        if (newHashes.Contains(kv.Key)) continue;                 // 本次新写入的，别删
                        if (kv.Value is not JsonObject e) continue;
                        if (!string.Equals(e["selector"]?.GetValue<string>(), "map", StringComparison.OrdinalIgnoreCase)) continue;
                        if (!string.Equals(e["kind"]?.GetValue<string>(), "BGMAP", StringComparison.OrdinalIgnoreCase)) continue;
                        if (!string.Equals(e["map"]?.GetValue<string>(), mapId, StringComparison.Ordinal)) continue;
                        drop.Add(kv.Key);
                    }
                    foreach (var h in drop)
                    {
                        assetsObj.Remove(h);
                        Console.WriteLine($"[DeviceAsset] 设备 {deviceId} 地图 {mapId}：剔除旧口径条目 {h}（整图覆盖窗口）");
                    }
                }
            }
            MergeAndWrite(deviceDir, root, assets);
            return true;
        }
    }

    /// <summary>
    /// 从该设备 manifest 里**删除一张地图**（Web「选镜头」卡片地图列表的删除按钮 → 端点
    /// DELETE /api/admin/devices/{id}/camera/maps/{mapId}）。
    ///
    /// 语义：只摘 manifest 条目（= 服务端"愿意服务"的清单），**不动磁盘 mpak 文件**——
    /// 设备侧对账（Firmware/…/asset_dl.c prune_stale_locked）本就以清单为准剪除本地索引，
    /// 磁盘文件由设备自己的 LRU 淘汰；服务端留文件是为了可重推/可复用，且删文件不可逆
    /// （同既有"换口径重导只摘旧条目"的 RemoveMapEntries 口径）。
    ///
    /// 删谁（判据，勿轻改）：
    ///   ① 主条目：selector=map &amp; kind=BGMAP &amp; extra.map==mapId（与 CameraService.ReadBgmapEntries /
    ///      AdminEndpoints.FindBgmapHash 同口径：kind/selector 忽略大小写，map 用 Ordinal 精确匹配）。
    ///   ② 派生条带 PARTS：label 以 `条带 {mapId}#` 或 `背景层 {mapId}#` 开头（AssetExporter.ExportMap
    ///      只有地图导出会写这两个前缀；`#` 分隔符保证 "200000000" 不会误配 "2000000000" 的图），
    ///      外加主包**条带表 part_ref** 直接引用的 hash（读包头 96B 解析，权威引用关系）。
    ///   ③ 派生缩略图：主条目 `thumb` 字段指向的 hash，或 label 恰为 `缩略图 {mapId}` 的 THUMB 条目。
    /// 共享资产（纸娃娃 / NPC 的 PARTS+LAYOUT、FONT、AUDIO_META）**永不进候选集**：label 不带上述
    /// 前缀、kind 也不是条带/缩略图形态 —— 这是"别误删共享资产"的第一道闸门。
    ///
    /// 第二道闸门 = **引用计数**：内容寻址（hash=内容身份）下同一份条带可能被另一张图的 BGMAP
    /// 条带表引用、同一张缩略图可能被两个 BGMAP 的 thumb 指向 ⇒ 只要还有**存活条目**引用它，
    /// 就保留（记进 KeptAssets 说明原因）。若存活 BGMAP 的包文件读不到（引用关系未知），
    /// 一律保守保留条带候选（多留一条无害，误删会让另一张图缺素材）。
    ///
    /// 幂等：清单里没有这张图 → Removed=false 直接返回（不写盘、不 bump rev），由端点回 200。
    /// 调用方负责 BumpRev（本服务不认识 DeviceManifestService）与"当前图"保护判定。
    /// </summary>
    public MapDeleteResult DeleteMap(string deviceId, string mapId)
    {
        ValidateIds(deviceId, mapId, "地图 id");
        mapId = mapId.Trim();
        lock (_deviceLocks.GetOrAdd(deviceId, _ => new object()))
        {
            var deviceDir = DeviceDir(deviceId);
            var root = ReadIndex(Path.Combine(deviceDir, ManifestBuilder.AssetsManifestFileName));
            var assetsObj = root["assets"] as JsonObject ?? new JsonObject();
            var result = new MapDeleteResult { MapId = mapId, RemainingMaps = CountBgmapMaps(assetsObj) };

            // ① 主条目
            var mainHashes = new List<string>();
            foreach (var kv in assetsObj)
                if (kv.Value is JsonObject e && IsBgmapOfMap(e, mapId)) mainHashes.Add(kv.Key);
            if (mainHashes.Count == 0) return result;      // 幂等：本来就没有这张图

            // ②/③ 候选派生条目（先收集，引用计数后再决定去留）
            var candidates = new Dictionary<string, DeletedAsset>(StringComparer.Ordinal);
            string stripA = $"条带 {mapId}#", stripB = $"背景层 {mapId}#", thumbLabel = $"缩略图 {mapId}";
            foreach (var kv in assetsObj)
            {
                if (mainHashes.Contains(kv.Key) || kv.Value is not JsonObject e) continue;
                var kind = e["kind"]?.GetValue<string>() ?? "";
                var label = e["label"]?.GetValue<string>() ?? "";
                if (string.Equals(kind, "PARTS", StringComparison.OrdinalIgnoreCase)
                    && (label.StartsWith(stripA, StringComparison.Ordinal) || label.StartsWith(stripB, StringComparison.Ordinal)))
                {
                    candidates[kv.Key] = new DeletedAsset
                    {
                        Hash = kv.Key, Kind = "PARTS", Label = label,
                        Reason = "该图专用条带 PARTS（label 前缀判定：" + (label.StartsWith(stripA, StringComparison.Ordinal) ? stripA : stripB) + "）",
                    };
                }
                else if (string.Equals(kind, "THUMB", StringComparison.OrdinalIgnoreCase)
                         && string.Equals(label, thumbLabel, StringComparison.Ordinal))
                {
                    candidates[kv.Key] = new DeletedAsset
                    {
                        Hash = kv.Key, Kind = "THUMB", Label = label,
                        Reason = $"该图专用缩略图（label = {thumbLabel}）",
                    };
                }
            }
            foreach (var h in mainHashes)
            {
                if (assetsObj[h] is not JsonObject main) continue;
                var file = main["file"]?.GetValue<string>();
                if (!string.IsNullOrEmpty(file))
                {
                    var refs = ReadBgmapStripRefs(Path.Combine(deviceDir, file));
                    if (refs != null)
                        foreach (var r in refs)
                            candidates.TryAdd(r, new DeletedAsset
                            {
                                Hash = r, Kind = "PARTS",
                                Label = (assetsObj[r] as JsonObject)?["label"]?.GetValue<string>(),
                                Reason = "被该图 BGMAP 条带表引用（part_ref）",
                            });
                }
                var thumbHash = main["thumb"]?.GetValue<string>();
                if (!string.IsNullOrEmpty(thumbHash))
                    candidates.TryAdd(thumbHash, new DeletedAsset
                    {
                        Hash = thumbHash, Kind = "THUMB",
                        Label = (assetsObj[thumbHash] as JsonObject)?["label"]?.GetValue<string>(),
                        Reason = "该图 BGMAP 的 thumb 字段指向",
                    });
            }

            // 引用计数：存活条目（除即将删除的主条目外）持有的 hash 引用
            var referenced = new HashSet<string>(StringComparer.Ordinal);
            bool stripRefsComplete = true;                 // false = 有存活 BGMAP 的包读不到 → 条带一律不删
            foreach (var kv in assetsObj)
            {
                if (mainHashes.Contains(kv.Key) || kv.Value is not JsonObject e) continue;
                var th = e["thumb"]?.GetValue<string>();
                if (!string.IsNullOrEmpty(th)) referenced.Add(th);         // 缩略图引用（纯 JSON，无需读包）
                if (!IsBgmapAny(e)) continue;
                var file = e["file"]?.GetValue<string>();
                if (string.IsNullOrEmpty(file)) { stripRefsComplete = false; continue; }
                var refs = ReadBgmapStripRefs(Path.Combine(deviceDir, file));
                if (refs == null) stripRefsComplete = false;
                else foreach (var r in refs) referenced.Add(r);
            }

            foreach (var (hash, cand) in candidates)
            {
                if (assetsObj[hash] is not JsonObject ce) continue;         // 已不在索引里（同 hash 只占一条）
                var kind = ce["kind"]?.GetValue<string>() ?? "";
                bool isParts = string.Equals(kind, "PARTS", StringComparison.OrdinalIgnoreCase);
                bool isThumb = string.Equals(kind, "THUMB", StringComparison.OrdinalIgnoreCase);
                if (!isParts && !isThumb)
                {
                    result.KeptAssets.Add(new DeletedAsset { Hash = hash, Kind = kind, Label = cand.Label,
                        Reason = "kind 不是 PARTS/THUMB（共享资产形态），不删" });
                    continue;
                }
                if (referenced.Contains(hash))
                {
                    result.KeptAssets.Add(new DeletedAsset { Hash = hash, Kind = kind, Label = cand.Label,
                        Reason = "仍被其它存活条目引用（引用计数 > 0："
                                 + (isThumb ? "同内容缩略图被另一个 BGMAP 的 thumb 指向" : "同内容条带被另一张图的 BGMAP 条带表共用")
                                 + "），保留" });
                    continue;
                }
                if (isParts && !stripRefsComplete)
                {
                    result.KeptAssets.Add(new DeletedAsset { Hash = hash, Kind = kind, Label = cand.Label,
                        Reason = "有存活 BGMAP 的包文件读不到（引用关系未知）→ 保守保留" });
                    continue;
                }
                assetsObj.Remove(hash);
                cand.Kind = kind;
                result.RemovedAssets.Add(cand);
            }

            foreach (var h in mainHashes)
            {
                if (assetsObj[h] is JsonObject main)
                {
                    result.BgmapHash ??= h;
                    result.Label ??= main["label"]?.GetValue<string>();
                }
                assetsObj.Remove(h);
            }
            root["assets"] = assetsObj;
            WriteIndex(deviceDir, root);
            /* 【摘条目的同时把包文件删掉 2026-10-02 用户口径"页面上删除地图要把设备的
             * 资源删掉"】旧实现只摘索引不删文件 —— 服务端导出目录里那几十 MB 的
             * BGMAP/条带/缩略图会一直躺着（216 设备目录一度堆到 200MB+）。
             * 只删本次真正摘掉的 hash（被引用保留的 KeptAssets 不动）；
             * 顺带清理同名的 .tmp / .mpak.tmp 残留。删除失败只记日志，不回滚索引。 */
            long freed = 0; int filesDeleted = 0;
            foreach (var a in result.RemovedAssets.Append(new DeletedAsset { Hash = result.BgmapHash ?? "" }))
            {
                if (string.IsNullOrEmpty(a.Hash)) continue;
                foreach (var ext in new[] { ".mpak", ".mpak.tmp", ".png" })
                {
                    var f = Path.Combine(deviceDir, a.Hash + ext);
                    try
                    {
                        if (!File.Exists(f)) continue;
                        var len = new FileInfo(f).Length;
                        File.Delete(f);
                        filesDeleted++; freed += len;
                    }
                    catch (Exception ex) { Console.Error.WriteLine($"[DeviceAsset] 删文件失败 {f}: {ex.Message}"); }
                }
            }
            result.Removed = true;
            result.FilesDeleted = filesDeleted;
            result.BytesFreed = freed;
            result.RemainingMaps = CountBgmapMaps(assetsObj);
            Console.WriteLine($"[DeviceAsset] 设备 {deviceId} 删除地图 {mapId}（{result.Label}）："
                              + $"摘除 BGMAP {mainHashes.Count} 条 + 派生素材 {result.RemovedAssets.Count} 条"
                              + $"（保守保留 {result.KeptAssets.Count} 条），**物理删除 {filesDeleted} 个文件、"
                              + $"释放 {freed / 1024}KB**，剩余地图 {result.RemainingMaps} 张");
            return result;
        }
    }

    /// <summary>删除结果（端点直接序列化回 Web）。</summary>
    public sealed class MapDeleteResult
    {
        public string MapId { get; set; } = "";
        /// <summary>主条目 label（导出器写的中文地图名）；幂等分支为 null。</summary>
        public string? Label { get; set; }
        /// <summary>true = 主条目确实被摘掉；false = 清单里本来就没有这张图（幂等）。</summary>
        public bool Removed { get; set; }
        public string? BgmapHash { get; set; }
        /// <summary>本次摘掉的派生条目（条带 PARTS / 缩略图），带中文判据。</summary>
        public List<DeletedAsset> RemovedAssets { get; set; } = new();
        /// <summary>判定为"仍被引用/引用关系未知"而**保留**的候选（排障用；不删）。</summary>
        public List<DeletedAsset> KeptAssets { get; set; } = new();
        /// <summary>删完该设备清单里还剩几张 BGMAP 地图。</summary>
        public int RemainingMaps { get; set; }
        /// <summary>本次物理删除的包文件数（.mpak/.tmp/.png）与释放字节数。</summary>
        public int FilesDeleted { get; set; }
        public long BytesFreed { get; set; }
    }

    /// <summary>被删/被保留的派生条目（hash + kind + label + 中文判据）。</summary>
    public sealed class DeletedAsset
    {
        public string Hash { get; set; } = "";
        public string Kind { get; set; } = "";
        public string? Label { get; set; }
        public string Reason { get; set; } = "";
    }

    private static bool IsBgmapOfMap(JsonObject e, string mapId)
        => string.Equals(e["kind"]?.GetValue<string>(), "BGMAP", StringComparison.OrdinalIgnoreCase)
           && string.Equals(e["selector"]?.GetValue<string>(), "map", StringComparison.OrdinalIgnoreCase)
           && string.Equals(e["map"]?.GetValue<string>(), mapId, StringComparison.Ordinal);

    private static bool IsBgmapAny(JsonObject e)
        => string.Equals(e["kind"]?.GetValue<string>(), "BGMAP", StringComparison.OrdinalIgnoreCase)
           && string.Equals(e["selector"]?.GetValue<string>(), "map", StringComparison.OrdinalIgnoreCase);

    /// <summary>清单里剩余的 BGMAP 地图条数（= Web 列表/设备菜单会看到的张数）。</summary>
    private static int CountBgmapMaps(JsonObject assetsObj)
    {
        int n = 0;
        foreach (var kv in assetsObj)
            if (kv.Value is JsonObject e && IsBgmapAny(e)) n++;
        return n;
    }

    /// <summary>
    /// 读 BGMAP 包的条带表 part_ref 集合（**只读文件头部 ~100B**：整图包 17MB 级，绝不能整包读）。
    /// 布局（BgmapPackWriter 类头 / mpak wire）：MPAK 信封 40B → payload 头 56B
    /// （map_id32 | vw2 | vh2 | static_len4 | static_off4 | tile_len4 | tile_off4 | strip_count4）
    /// → strip_count × 14B（part_ref u64 | y i16 | speed_x i16 | rx u8 | blend u8）。
    /// 返回 null = 文件缺失/头不合法（调用方按"引用关系未知"保守处理，绝不据此删派生条目）。
    /// </summary>
    private static List<string>? ReadBgmapStripRefs(string mpakPath)
    {
        try
        {
            if (!File.Exists(mpakPath)) return null;
            using var fs = File.OpenRead(mpakPath);
            var head = new byte[Mpak.HeaderSize + 56];
            if (fs.Read(head, 0, head.Length) != head.Length) return null;
            if (!head.AsSpan(0, 4).SequenceEqual("MPAK"u8)) return null;
            uint payloadLen = BitConverter.ToUInt32(head, 32);
            if (payloadLen < 56) return null;
            int stripCount = (int)BitConverter.ToUInt32(head, Mpak.HeaderSize + 52);
            if (stripCount is < 0 or > 4096) return null;              // 明显损坏：别拿它当引用集
            var refs = new List<string>(stripCount);
            if (stripCount == 0) return refs;
            var table = new byte[stripCount * 14];
            if (fs.Read(table, 0, table.Length) != table.Length) return null;
            for (int i = 0; i < stripCount; i++)
                refs.Add($"{BitConverter.ToUInt64(table, i * 14):x16}");   // manifest 键口径 = 16 位小写 hex
            return refs;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[DeviceAsset] 读 BGMAP 条带表失败 {mpakPath}: {ex.Message}");
            return null;
        }
    }

    /// <summary>
    /// 确保设备的 manifest-assets.json 已登记该 NPC 资产包（PARTS 整包 + 每动作 LAYOUT）。
    /// 返回是否实际生成新包（false = 索引已有该 NPC，幂等跳过）。
    /// NPC 数据缺失/条带导出失败抛异常，由上层记录。
    /// 同步方法（打包本体在 WzService 内部锁内串行，同 EnsurePacked 口径；
    /// 需要异步语义由调用方 Task.Run 放后台线程）。
    /// </summary>
    public bool EnsureNpcAsync(string deviceId, string npcId)
    {
        ValidateIds(deviceId, npcId, "NPC id");
        npcId = npcId.Trim();
        lock (_deviceLocks.GetOrAdd(deviceId, _ => new object()))
        {
            var deviceDir = DeviceDir(deviceId);
            var root = ReadIndex(Path.Combine(deviceDir, ManifestBuilder.AssetsManifestFileName));
            string entity = $"npc:{npcId}";
            /* 【导出器版本判据 2026-10-02】只看 selector+entity 会永远跳过重导 ——
             * 帧切片修复前的 NPC 包（每动作只有第 0 帧有像素）就再也换不掉。
             * 版本不符 → 清掉旧的一套条目后重导（同 entity 只留一套）。 */
            if (EntityUpToDate(root, "npc", entity)) return false;
            int dropped = RemoveEntityEntries(root, "npc", entity);
            if (dropped > 0)
                Console.WriteLine($"[DeviceAsset] {deviceId} {entity} 旧条目 {dropped} 条（导出器版本过旧）→ 重导替换");

            var warnings = new List<string>();
            var assets = new AssetExporter(_wz).ExportNpcAssets(npcId, warnings);
            MergeAndWrite(deviceDir, root, assets);
            return true;
        }
    }

    /// <summary>
    /// 为单个**怪物**产出并登记设备资产（PARTS 整包 + 每动作 LAYOUT，selector=mob、
    /// entity=mob:{id}）。与 EnsureNpcAsync 完全同模式（幂等键 = selector + entity），
    /// 只换成 Mob.wz 导出路径 —— 固件「怪物」页选中后就是按 entity 找这两个包渲染。
    /// 返回是否实际生成新包（false = 已登记，幂等跳过）。
    /// </summary>
    public bool EnsureMobAsync(string deviceId, string mobId)
    {
        ValidateIds(deviceId, mobId, "怪物 id");
        mobId = mobId.Trim();
        lock (_deviceLocks.GetOrAdd(deviceId, _ => new object()))
        {
            var deviceDir = DeviceDir(deviceId);
            var root = ReadIndex(Path.Combine(deviceDir, ManifestBuilder.AssetsManifestFileName));
            string entity = $"mob:{mobId}";
            if (EntityUpToDate(root, "mob", entity)) return false;
            int dropped = RemoveEntityEntries(root, "mob", entity);
            if (dropped > 0)
                Console.WriteLine($"[DeviceAsset] {deviceId} {entity} 旧条目 {dropped} 条（导出器版本过旧）→ 重导替换");

            var warnings = new List<string>();
            var assets = new AssetExporter(_wz).ExportMobAssets(mobId, warnings);
            MergeAndWrite(deviceDir, root, assets);
            return true;
        }
    }

    /// <summary>
    /// 查该设备索引里某实体（"mob:100100"/"npc:2100000"/"paperdoll:default"）的 PARTS 包内容 hash
    /// （16 hex 小写）。用途：推送怪物/NPC 后给设备下发**切换实体**指令前的核对/日志。
    /// 固件按 entity 字符串在本地清单里找 PARTS/LAYOUT，指令载荷只需带 entity。找不到返回 null。
    /// </summary>
    public string? FindEntityPartsHash(string deviceId, string entity)
    {
        var e = FindEntityEntry(deviceId, entity);
        return e?.Key;
    }

    /// <summary>该实体（mob:/npc:）默认动作名（PARTS 条目 extra.defaultAction）；无则 null。</summary>
    public string? FindEntityDefaultAction(string deviceId, string entity)
    {
        var e = FindEntityEntry(deviceId, entity);
        var da = e?.Value["defaultAction"]?.GetValue<string>();
        return string.IsNullOrEmpty(da) ? null : da;
    }

    /// <summary>索引里某实体的 PARTS 条目（key = 内容 hash 16 hex）；无则 null。</summary>
    private KeyValuePair<string, JsonObject>? FindEntityEntry(string deviceId, string entity)
    {
        try
        {
            var indexPath = Path.Combine(DeviceDir(deviceId), ManifestBuilder.AssetsManifestFileName);
            if (!File.Exists(indexPath)) return null;
            var root = JsonNode.Parse(File.ReadAllText(indexPath)) as JsonObject;
            if (root?["assets"] is not JsonObject assets) return null;
            foreach (var kv in assets)
            {
                if (kv.Value is not JsonObject e) continue;
                if (!string.Equals(e["kind"]?.GetValue<string>(), "PARTS", StringComparison.Ordinal)) continue;
                if (!string.Equals(e["entity"]?.GetValue<string>(), entity, StringComparison.Ordinal)) continue;
                return new KeyValuePair<string, JsonObject>(kv.Key, e);
            }
            return null;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[DeviceAsset] 查实体 {entity} 失败: {ex.Message}");
            return null;
        }
    }

    /// <summary>
    /// 写 mpak + 合并索引（**不删旧条目**——地图/NPC 累积收藏语义，与装扮替换语义不同；
    /// 同 hash 覆盖无害）。调用方须已持有该设备的锁。
    /// </summary>
    /// <summary>
    /// 按需登记曲库元数据（AUDIO_META）：写 {hash}.mpak 到设备导出目录并合并 manifest。
    /// 为什么单独一条：全量导出（CLI）才会生成 AUDIO_META，设备级导出只按需烘地图/
    /// NPC/装扮 → 走 Web 接入的设备没有曲目表（BGM 点播放无效）。见 AdminEndpoints
    /// 的 /devices/{id}/audio-meta 注释。
    /// </summary>
    public (string hash, long bytes) WriteAudioMeta(string deviceId)
    {
        var deviceDir = DeviceDir(deviceId);
        Directory.CreateDirectory(deviceDir);
        var a = new AssetExporter(_wz).ExportAudioMetaAsset();
        File.WriteAllBytes(Path.Combine(deviceDir, a.FileName), a.Bytes);
        var root = ReadIndex(Path.Combine(deviceDir, ManifestBuilder.AssetsManifestFileName));
        MergeAndWrite(deviceDir, root, new List<ExportedAsset> { a });
        return ($"{a.Hash:x16}", a.Bytes.Length);
    }

    private static void MergeAndWrite(string deviceDir, JsonObject root, List<ExportedAsset> assets)
    {
        var assetsObj = root["assets"] as JsonObject ?? new JsonObject();
        foreach (var a in assets)
        {
            File.WriteAllBytes(Path.Combine(deviceDir, a.FileName), a.Bytes);
            assetsObj[$"{a.Hash:x16}"] = EntryOf(a);
        }
        root["assets"] = assetsObj;
        WriteIndex(deviceDir, root);
    }

    /// <summary>
    /// 实体导出器版本（写进 PARTS/LAYOUT 条目 extra.exporterRev）。
    /// 为什么要有：实体登记的幂等键是「selector + entity」，只看键会**永远跳过重导** ——
    /// 真问题（2026-10-02）：帧切片修复前导出的 NPC 包，每个动作只有第 0 帧有像素
    /// （`DrawBitmap(bmp, f*cw, 0)` 在 x&gt;0 时不绘制）；用户重推同一个 NPC 时旧实现
    /// 直接跳过 → 设备永远拿不到修好的包。现在版本号一变就重导 + 换掉旧条目。
    /// 改导出格式/像素口径时 +1（AppendOnly：旧包在新版本下会被替换，不需要手工清库）。
    /// </summary>
    public const int EntityExporterRev = 2;

    /// <summary>实体在该设备索引里是否已按**当前导出器版本**登记（PARTS 条目判定）。</summary>
    private static bool EntityUpToDate(JsonObject root, string selector, string entity)
    {
        if (root["assets"] is not JsonObject ao) return false;
        foreach (var kv in ao)
        {
            if (kv.Value is not JsonObject e) continue;
            if (!string.Equals(e["selector"]?.GetValue<string>(), selector, StringComparison.Ordinal)) continue;
            if (!string.Equals(e["entity"]?.GetValue<string>(), entity, StringComparison.Ordinal)) continue;
            if (!string.Equals(e["kind"]?.GetValue<string>(), "PARTS", StringComparison.Ordinal)) continue;
            /* 容错读：EntryOf 的 extra 通用分支会把非 string/int[] 的值写成**字符串**
             * （未列类型走 v.ToString()）——真机验证时正是这样写出了 "2"，而
             * GetValue<int?>() 遇到 JsonValue(String) 会抛
             * "An element of type 'String' cannot be converted to 'System.Nullable<int>'"，
             * 整个 push 后台任务失败。这里数字/字符串两种形态都认（也兼容旧写法）。 */
            var jv = e["exporterRev"];
            int? rev = null;
            if (jv is JsonValue val)
            {
                if (val.TryGetValue<int>(out var iv)) rev = iv;
                else if (val.TryGetValue<string>(out var sv) && int.TryParse(sv, out var pv)) rev = pv;
            }
            return rev == EntityExporterRev;
        }
        return false;
    }

    /// <summary>摘掉该实体在索引里的全部条目（PARTS + 各动作 LAYOUT）——重导前清场，
    /// 防"同 entity 两套包"被设备端任选其一（旧坏包）；包文件留在磁盘，由设备端
    /// 清单对账（prune_stale_locked）自然淘汰。</summary>
    private static int RemoveEntityEntries(JsonObject root, string selector, string entity)
    {
        if (root["assets"] is not JsonObject ao) return 0;
        var doomed = new List<string>();
        foreach (var kv in ao)
        {
            if (kv.Value is not JsonObject e) continue;
            if (!string.Equals(e["selector"]?.GetValue<string>(), selector, StringComparison.Ordinal)) continue;
            if (!string.Equals(e["entity"]?.GetValue<string>(), entity, StringComparison.Ordinal)) continue;
            doomed.Add(kv.Key);
        }
        foreach (var k in doomed) ao.Remove(k);
        return doomed.Count;
    }

    /// <summary>
    /// 落盘索引（generated 时间戳刷新；中文 label 不转义，同既有写手口径）。
    /// 删除路径（DeleteMap）只摘条目、不写任何包文件，所以单独抽出来复用。
    /// 调用方须已持有该设备的锁（_deviceLocks）。
    /// </summary>
    private static void WriteIndex(string deviceDir, JsonObject root)
    {
        Directory.CreateDirectory(deviceDir);
        root["generated"] = DateTimeOffset.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'");
        File.WriteAllText(Path.Combine(deviceDir, ManifestBuilder.AssetsManifestFileName),
            root.ToJsonString(new JsonSerializerOptions
            {
                WriteIndented = true,
                Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping, // 中文 label 不转义
            }));
    }

    private string DeviceDir(string deviceId) => Path.Combine(_paths.ExportRoot, deviceId);

    private static void ValidateIds(string deviceId, string id, string idName)
    {
        if (string.IsNullOrWhiteSpace(deviceId)) throw new ArgumentException("deviceId 不能为空", nameof(deviceId));
        if (deviceId.Contains('/') || deviceId.Contains('\\') || deviceId.Contains(".."))
            throw new ArgumentException($"deviceId 非法: {deviceId}", nameof(deviceId));
        if (string.IsNullOrWhiteSpace(id)) throw new ArgumentException($"{idName}不能为空", nameof(id));
    }

    private static JsonObject ReadIndex(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                var node = JsonNode.Parse(File.ReadAllText(path));
                if (node is JsonObject o) return o;
            }
        }
        catch { /* 损坏索引按空处理，下方重建 */ }
        return new JsonObject();
    }

    /// <summary>
    /// 幂等判定：索引里已有 selector 相同且 extra 字段 key==value 的条目。
    /// 主条目 kind 必须是固件白名单大写（map→BGMAP / npc→PARTS）：旧版小写条目视为未登记，
    /// 走重打覆盖——否则坏索引永不被纠正，设备永不下载（同 PaperdollPackService.HasAppearance）。
    /// <paramref name="viewport"/>（可选）：再要求条目的 `viewport` 字段匹配（"full"/"window"）；
    /// 缺失该字段的旧条目按 "window" 处理（兼容 2026-10-01 之前登记的索引）。
    /// <paramref name="layout"/>（可选）：再要求条目的 `layout` 字段匹配（"tiled"/"rows"）；
    /// 缺失该字段的旧条目按 "rows"（逐行）处理 —— 2026-10-01 分块布局上线前的整图条目
    /// 都是逐行的，不这样判会导致"已登记 ⇒ 跳过"，设备永远拿不到分块包。
    /// </summary>
    private static bool HasEntry(JsonObject root, string selector, string key, string value,
        string? viewport = null, string? layout = null)
    {
        var primaryKind = selector == "map" ? "BGMAP" : "PARTS";
        if (root["assets"] is not JsonObject ao) return false;
        foreach (var kv in ao)
        {
            if (kv.Value is not JsonObject e) continue;
            if (!string.Equals(e["selector"]?.GetValue<string>(), selector, StringComparison.Ordinal)) continue;
            if (!string.Equals(e[key]?.GetValue<string>(), value, StringComparison.Ordinal)) continue;
            if (string.Equals(e["kind"]?.GetValue<string>(), primaryKind, StringComparison.Ordinal))
            {
                if (viewport == null && layout == null) return true;
                if (viewport != null)
                {
                    var vp = e["viewport"]?.GetValue<string>();
                    if (string.IsNullOrEmpty(vp)) vp = "window";   // 旧条目无 viewport 字段 = 窗口口径
                    if (!string.Equals(vp, viewport, StringComparison.Ordinal)) continue;
                }
                if (layout != null)
                {
                    var ly = e["layout"]?.GetValue<string>();
                    if (string.IsNullOrEmpty(ly)) ly = "rows";     // 旧条目无 layout 字段 = 逐行口径
                    if (!string.Equals(ly, layout, StringComparison.Ordinal)) continue;
                }
                return true;
            }
        }
        return false;
    }

    /// <summary>
    /// 摘掉索引里该地图的 BGMAP 条目（口径切换 window↔full 重导时调用；同 map 只能有一条
    /// selector=map 的 BGMAP，否则设备列表重复、切图 hash 取错）。返回摘掉的条数。
    /// 注：旧包的条带/缩略图条目（无 selector/map 字段）不在此清——与既有重导路径同口径。
    /// </summary>
    private static int RemoveMapEntries(JsonObject root, string mapId)
    {
        if (root["assets"] is not JsonObject ao) return 0;
        var doomed = new List<string>();
        foreach (var kv in ao)
        {
            if (kv.Value is not JsonObject e) continue;
            if (!string.Equals(e["selector"]?.GetValue<string>(), "map", StringComparison.Ordinal)) continue;
            if (!string.Equals(e["map"]?.GetValue<string>(), mapId, StringComparison.Ordinal)) continue;
            if (string.Equals(e["kind"]?.GetValue<string>(), "BGMAP", StringComparison.Ordinal)) doomed.Add(kv.Key);
        }
        foreach (var k in doomed) ao.Remove(k);
        return doomed.Count;
    }

    /// <summary>
    /// 资产条目 → manifest JSON（字段口径对齐 ManifestBuilder.EntryToJson，差异仅 kind 大小写：
    /// 固件 asset_dl 用 strcmp 白名单匹配 kind_dir，必须大写 "PARTS"/"LAYOUT"/"BGMAP"…；
    /// 未知 kind（如 THUMB）设备端按元数据登记、不下载，前向兼容）。
    /// </summary>
    private static JsonObject EntryOf(ExportedAsset a)
    {
        var o = new JsonObject
        {
            ["kind"] = a.Kind.DirName(),
            ["bytes"] = a.ByteCount,
            ["file"] = a.FileName,
            ["url"] = $"/api/device/asset/{a.Hash:x16}",
            ["label"] = a.Label,
        };
        if (!string.IsNullOrEmpty(a.Selector)) o["selector"] = a.Selector;
        foreach (var (k, v) in a.Extra)
        {
            if (v is string sv) o[k] = sv;
            else if (v is int[] ia) { var arr = new JsonArray(); foreach (var i in ia) arr.Add(i); o[k] = arr; }
            /* 数值/布尔按原生类型写（此前一律 ToString() → 数字变成字符串 "2"，
             * 读回时 GetValue<int?>() 直接抛异常：真机验证踩到） */
            else if (v is int iv) o[k] = iv;
            else if (v is long lv) o[k] = lv;
            else if (v is bool bv) o[k] = bv;
            else if (v != null) o[k] = v.ToString() ?? "";
        }
        return o;
    }
}
