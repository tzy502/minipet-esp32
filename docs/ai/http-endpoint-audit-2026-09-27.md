# 服务端 HTTP 端点自查报告（2026-09-27）

> 范围：`Server/MinipetServer/`（.NET 9）全部路由 × `Web/src/` 全部前端调用
> 方法：静态盘点（grep）+ **真实起服务端 curl 实测**（Release 产物 `bin/Release/net9.0/MinipetServer.dll`）
> 产物版本核对（开始时）：`MinipetServer.dll` 构建于 `2026-09-27 14:50:14`，最新源文件 `DeviceAssetService.cs` 改于 `14:50:13` → **产物不落后于源码**。

### ⚠️ 并行改动提示（审计期间工作树被他人改动，请注意结论对应的产物）

本审计与其它 agent 并行进行，工作树在审计中途被改动，**时间线**如下：

| 时刻 | 事件 | 影响 |
|---|---|---|
| 14:50:14 | `bin/Release` 原始构建 | **§1 总表、§4.1–4.6 的实测基线**（38097/38098 用此产物） |
| 15:01:09 | **本次**修改 `AdminEndpoints.cs`（presets 修复） | §2/§6 |
| 15:01:54 / 15:01:59 | **他人**修改 `Music/WzMusicSource.cs` / `Api/DeviceEndpoints.cs` | 非本次改动 |
| 15:02:04 | **他人重建** `bin/Release`（从含本次修复的工作树编译） | 见下「复核」 |
| 15:02:21+ | 38201 实测 `device/bgm/cmd` | 跑的是 **15:02:04 新产物**（故响应含他人新增的 `id` 字段） |
| 15:03:1x | **复核当前 `bin/Release`（38202）** | 见下「复核」 |

**对当前工作树的复核结论（38202 实测）**：
1. ✅ **本次 presets 修复已在当前 `bin/Release` 中**（他人 15:02:04 从工作树重建时一并编译进去）：
   `GET /api/admin/presets` → `200 application/json` / `{"presets":[{"name":"神子",…}]}` → **无需再重建**。
2. ✅ `music/tracks` 的 `bytes` 仍恒为 0（`count=1167`，分布 `[(0,1167)]`）——他人对 `WzMusicSource.cs` 的改动
   （`GetTrackStreamAsync` 的负数 id 反查）**未触及列表 `Bytes` 字段**，§4.4 结论不受影响。
3. ⚠️ `POST /api/device/bgm/cmd` 响应**新增了他人加入的 `id` 数值字段**：
   `{"ok":true,"cmd":"next","source":"wz","trackId":"Bgm00.img/FloralLife","id":1236564701,"volume":60}`
   （§4.7 的原始输出来自 38201 新产物，已含此字段；该端点是**设备侧**通道，Web 不调用，不影响前端核查结论。）
4. 本次改动**只在 `AdminEndpoints.cs`**；`DeviceEndpoints.cs` / `WzMusicSource.cs` 的 diff **不是本次产物**。
   报告中对 `DeviceEndpoints.cs` 的行号引用（27-36 / 201-203 / 302 / 320）均在他人改动行（332+）**之前**，仍然准确；
   `WzMusicSource.cs` 的行号已按他人改动后校正（见 §4.4）。

---

## 0. 结论摘要

| 项 | 数量 |
|---|---|
| 服务端注册路由总数 | **40** |
| 前端（Web）实际会调的端点 | **26** |
| **前端在调但服务端没有（真缺口）** | **1** → `GET /api/admin/presets`（已修，见 §2） |
| 路径/方法/请求体形状不一致 | **1 处真形状 bug**（PUT 设备双重包裹）+ 3 处**注释过期**误导 |
| 服务端注册但前端不用 | 14（设备端点 9 + 管理端点 5），均正常 |

**最重要的一条**：`GET /api/admin/presets` 的路由注册**被同一行的 `//` 注释吞掉**，服务端从未注册该路由。
因为存在 SPA fallback，它返回 **HTTP 200 + text/html**（index.html），前端 axios 拿到字符串 →
`data?.presets` 为 `undefined` → 预设列表**静默显示为空**（不报错、不提示）。这正是「页面看着在、其实不生效」的典型。

---

## 1. 端点总表（方法 | 路径 | 文件:行 | 实测状态码 | 结论）

实测环境：`38097`＝无 WZ（`MINIPET_DATA_DIR=/tmp/audit_srv`）；`38098`＝有 WZ（真实 `/Volumes/SSD/mxd/mxd/Data`）。

### 1.1 设备端点 `/api/device`（9 个，前缀 `DeviceEndpoints.cs:26`）

| 方法 | 路径 | 文件:行 | 实测状态码 | 结论 |
|---|---|---|---|---|
| POST | `/api/device/hello` | DeviceEndpoints.cs:27 | 200 | ✅ 返回 deviceId/proto/paired/pairingCode/config/manifestRev/manifestUrl/pollUrl |
| GET | `/api/device/manifest` | DeviceEndpoints.cs:28 | 200 | ✅ `{proto,rev,assets,entities,clock_table}` |
| GET | `/api/device/asset/{hash}` | DeviceEndpoints.cs:29 | 404 | ✅ 未导出时 `{"error":"素材不存在：…"}` |
| GET | `/api/device/poll` | DeviceEndpoints.cs:30 | 200 | ✅ 长轮询；**`timeoutMs` 不是参数**（见 §3.6） |
| POST | `/api/device/event` | DeviceEndpoints.cs:31 | 200 | ✅ `{ok,manifestRev}` |
| POST | `/api/device/log` | DeviceEndpoints.cs:33 | 200 | ✅ **幂等实测通过**（见 §4.1） |
| GET | `/api/device/bgm/stream` | DeviceEndpoints.cs:34 | 400 / 200 | ✅ 缺 `id` → 400（**空 body**）；带 id → 200 `audio/mpeg` 1666351 B 真 MP3 |
| POST | `/api/device/bgm/cmd` | DeviceEndpoints.cs:35 | 200 / 500 | ⚠️ **WZ 已加载：7 种 cmd 全 200**；**WZ 未加载：`next/prev` → 500**（见 §3.4） |
| GET | `/api/device/firmware/{ver}.bin` | DeviceEndpoints.cs:36 | 404 | ✅ 无包时 `{"error":"固件不存在：…"}` |

### 1.2 管理端点 `/api/admin`（28 个）

| 方法 | 路径 | 文件:行 | 实测状态码 | 结论 |
|---|---|---|---|---|
| GET | `/api/admin/devices` | AdminEndpoints.cs:24 | 200 | ✅ `{devices:[卡片]}` |
| GET | `/api/admin/devices/{id}` | AdminEndpoints.cs:29 | 200 / 404 | ✅ `{device:{…}}` |
| PUT | `/api/admin/devices/{id}` | AdminEndpoints.cs:32 | 200 / 404 | ⚠️ **响应双重包裹** `{"device":{"device":{…}}}`（见 §3.1） |
| POST | `/api/admin/pair` | AdminEndpoints.cs:93 | 404 | ✅ 非法码 404（非 400）；缺 code 400 |
| GET | `/api/admin/thumb` | AdminEndpoints.cs:107 | 400 / 200 | ✅ 缺参 400；**无 WZ 也 200 PNG 占位**（见 §3.5） |
| GET | `/api/admin/materials/favorites` | AdminEndpoints.cs:112 | 200 | ✅ **端点在位**（前端注释说没有，已过期） |
| PUT | `/api/admin/materials/favorites` | AdminEndpoints.cs:114 | 200 | ✅ 整表替换；缺 `favorites` = 清空 |
| GET | `/api/admin/presets` | AdminEndpoints.cs:122 | **200 text/html** | ❌ **原为缺口，已修复**（见 §2）→ 修复后 200 JSON |
| POST | `/api/admin/presets` | AdminEndpoints.cs:123 | 201 | ✅ `{preset:{…}}` |
| GET | `/api/admin/presets/{id}` | AdminEndpoints.cs:130 | 200 / 404 | ✅ 前端未用 |
| PUT | `/api/admin/presets/{id}` | AdminEndpoints.cs:133 | 200 / 404 | ✅ 前端未用 |
| DELETE | `/api/admin/presets/{id}` | AdminEndpoints.cs:139 | 200 / 404 | ✅ `{ok:true}` |
| GET | `/api/admin/music/tracks` | AdminEndpoints.cs:146 | 200 / 404 / **500** | ⚠️ **WZ 未加载 → 未捕获异常 500 空 body**（见 §3.4） |
| GET | `/api/admin/music/sources` | AdminEndpoints.cs:157 | 200 | ✅ `health.state` 是**数字**；null 字段被省略 |
| GET | `/api/admin/music/sources/{name}/health` | AdminEndpoints.cs:177 | 200 / 404 | ✅ |
| PUT | `/api/admin/music/sources/{name}` | AdminEndpoints.cs:182 | 200 / 400 / 404 | ✅ wz 不可停 → 400；未知源 404 |
| POST | `/api/admin/music/sources/qq/cookie` | AdminEndpoints.cs:198 | 200 | ✅ `{ok,imported,cookieSavedAtUtc,cookieStale,cookieAgeDays,health}` |
| GET | `/api/admin/settings` | AdminEndpoints.cs:232 | 200 | ✅ `{config,wzPathExists,note}` |
| POST | `/api/admin/settings/validate-path` | AdminEndpoints.cs:241 | 200 | ✅ `{ok,message}`（不存在也是 200 + `ok:false`） |
| POST | `/api/admin/devices/{id}/command` | AdminEndpoints.cs:257 | 202 / 400 / 404 | ✅ **全分支实测通过**（见 §4.2） |
| PUT | `/api/admin/settings` | AdminEndpoints.cs:347 | 200 / 400 | ✅ 全量同构回传；非法 WZ 路径 → 400 `{"errors":[…]}` |
| GET | `/api/admin/logs/{id}` | AdminEndpoints.cs:361 | 200 / 404 | ✅ 服务端**事件**日志 |
| GET | `/api/admin/device-logs/{id}` | AdminEndpoints.cs:380 | 200 / 404 | ✅ 设备串口日志；过滤参数生效（见 §4.3） |
| GET | `/api/admin/device-logs/by-uuid/{uuid}` | AdminEndpoints.cs:388 | 200 / 404 | ✅ 前端未用（固件/人工排查用） |
| POST | `/api/admin/ota/{id}` | AdminEndpoints.cs:398 | 200 / 400 | ✅ `{queued,seq,ver,url}`；缺 ver 400 |
| POST | `/api/admin/devices/{id}/push` | AdminEndpoints.cs:419 | 202 / 400 / 404 | ✅ `{ok,note}`；kind 非法/id 空 400 |
| GET | `/api/admin/devices/{id}/health` | AdminEndpoints.cs:497 | 200 | ✅ 前端未用 |
| POST | `/api/admin/devices/{id}/fonts` | AdminEndpoints.cs:514 | 200 | ✅ 前端未用（回复 `{ok,deviceId,generated,packedSizes,manifestRev}`） |

### 1.3 目录 / 素材端点（2 个）

| 方法 | 路径 | 文件:行 | 实测状态码 | 结论 |
|---|---|---|---|---|
| GET | `/api/admin/catalog` | AdminCatalogEndpoints.cs:33 | 200 / 400 / 503 | ✅ 有 WZ：`part=hair` → 17529 件；无 WZ：503 `{"error":"WZ 未加载"}` |
| GET | `/api/admin/materials` | MaterialsEndpoints.cs:24 | 200 / 400 / 503 | ✅ 有 WZ：`kind=map` → 21422 条；kind 非法 400 |

### 1.4 健康锚点（1 个）

| 方法 | 路径 | 文件:行 | 实测状态码 | 结论 |
|---|---|---|---|---|
| GET | `/api/health` | Program.cs:143 | 200 | ✅ `{ok,service,proto,timeUtc,wzPathExists,wzDataPath,devices,qqEnabled}` |

> 另有 SPA 兜底 `app.MapFallbackToFile("/index.html")`（Program.cs:164）——**任何未注册的 GET 路径都返回 200 + text/html**，这是多处「缺口被伪装成成功」的根因。

---

## 2.「前端在调但服务端没有」清单 ★最重要

### 2.1 `GET /api/admin/presets` —— 唯一真缺口（已修复）

**前端调用点**：`Web/src/api/client.js:205`
```js
export function listPresets() {
  return http.get('/admin/presets').then((r) => r.data)   // → GET /api/admin/presets
}
```
消费点 `Web/src/views/PaperdollView.vue:208-219`：
```js
const data = await listPresets()
presets.value = data?.presets ?? []      // ← HTML 字符串没有 .presets → 永远 []
```
（`AppearancePicker.vue` / `DeviceDetailView.vue` 也 import `listPresets` 走同一路径。）

**服务端 grep 结果**：`Api/AdminEndpoints.cs:121`（修复前）
```csharp
// ── 纸娃娃预设 CRUD（data/presets/，供设备选择器「纸娃娃 tab」，E7/E4）──        g.MapGet("/presets", (PresetStore presets) => Results.Json(new { presets = presets.List() }));
```
整行以 `//` 开头 → **`g.MapGet("/presets", …)` 整句被注释掉**，从未注册。
（同段的 `MapPost` / `MapGet/{id}` / `MapPut` / `MapDelete` 都正常注册——只有列表这一个被吞。）

**实测佐证**（老 Release 产物）：
```
$ curl -s -o /tmp/o -w "HTTP %{http_code} | %{content_type} | %{size_download}B\n" \
    http://127.0.0.1:38098/api/admin/presets
HTTP 200 | text/html | 404B
$ head -c 40 /tmp/o
<!DOCTYPE html>
```
对照：同前缀的 `GET /api/admin/presets/nope` → `404 {"error":"预设不存在：nope"}`（正常 JSON），
`POST /api/admin/presets` → `201`。**即「保存能成功、列表永远为空」**——用户会以为保存失败或数据丢了。

**为何静默**：`GET` 未注册 → 落入 SPA fallback → **HTTP 200 + text/html**。axios 视为成功，
`.then` 正常返回字符串，`data?.presets` → `undefined` → `?? []`，既不抛异常也不设 `presetsError`。
（同一路径的 POST/PUT 未被注册时是 405，反而会报错；只有 GET 会被 fallback 伪装。）

**修复（1 行，见 §6 diff）**：把注释与语句拆成两行。已构建验证：
```
$ curl -s -w "HTTP %{http_code} | %{content_type} | %{size_download}B\n" http://127.0.0.1:38200/api/admin/presets
HTTP 200 | application/json; charset=utf-8 | 575B
  ✅ JSON 解析成功；presets 条数 = 1
    - p-86898be5 神子 paperdoll
```
回归核对：`GET /api/admin/presets/nope` → 404 JSON；`POST /api/admin/presets` → 201；`GET /api/health` → 200。

**其余前端调用端点全部在位**——逐个 curl 打过，无第二处缺口。已顺带用脚本扫描全部 `Map*` 调用，
确认「被同行 `//` 吞掉」的注册**全仓库仅此 1 处**。

---

## 3.「形状不一致」清单

### 3.1 ⚠️ 真 bug：`PUT /api/admin/devices/{id}` 响应双重包裹 `device`

| 侧 | 位置 | 形态 |
|---|---|---|
| 服务端 | `Api/AdminEndpoints.cs:90` | `return Results.Json(new { device = Detail(id, reg, cfg, health) });` |
| 服务端 | `Api/AdminEndpoints.cs:719-726` | `Detail(...)` 本身已返回 `new { device = new { … } }` |
| → 实际响应 | 实测 | `{"device":{"device":{"deviceId":"dev-791dd5",…}}}` |
| 对照 | `GET /devices/{id}`（:29-30）直接返回 `Detail(...)` | `{"device":{…}}` ✅ 单层 |
| 前端 | `Web/src/stores/devices.js:84-86` | `const data = await updateDevice(...)` → `return data?.device` → 拿到 `{device:{…}}` 而非设备对象 |

**影响**：`applyPetConfig()` 是唯一消费者，但它在全 Web 源码中**没有任何调用点**（已 grep 确认），
故当前是**潜伏 bug** 而非线上故障。`DeviceDetailView.vue` 三处 `updateDevice` 都忽略返回值、随后 `load()` 重取，
所以 UI 目前看不出问题。**建议后续统一为单层 `{device:{…}}`。**

### 3.2 注释过期（3 处，代码本身没错，但会误导后续开发）

| 位置 | 注释说 | 实际 |
|---|---|---|
| `Web/src/api/client.js:234-236` | 「服务端**当前没有**收藏存储/端点（AdminEndpoints.cs 无 `/materials/favorites` 路由）」 | **已在位** `AdminEndpoints.cs:112/114`，实测 GET 200 / PUT 200 落库 |
| `Web/src/api/client.js:168` | 「服务端 `AdminEndpoints.cs:281-284` 白名单当前就是这一形态（拒 bgm）」 | bgm **已放行**（`AdminEndpoints.cs:283+`），实测 6 动词全 202 |
| `Web/src/api/client.js:415` | 「服务端尚未实现时返回 404」 | **已实现**（`AdminEndpoints.cs:380/388`），实测 200 |
| `Web/docs/interfaces-needed-from-server.md` §T3/T6 | 把收藏/bgm 列为「待服务端支持」 | 同上，均已上线（该文档已有「状态复核」小节承认 T2/T4/T5 过期，T3/T7 未同步） |

> 收藏探测的实际行为**恰好正确**：`probeMaterialFavorites()`（client.js:265-290）不只判状态码，
> 还校验响应含 `favorites` 对象 → 命中真端点 → `supported:true` → UI 自动从 localStorage 单机模式切到服务端同步。
> 这是「探测到才启用」设计的正确落地，**无需改动**。

### 3.3 `PUT /api/admin/devices/{id}` 静默忽略未知字段（200 而非 400）

`DeviceUpdateRequest`（`AdminEndpoints.cs:550-558`）**只有** `Name` / `PetConfig` / `Bgm` / `Thresholds`。
实测：
```
PUT {"speech":{...}}      → 200（被 System.Text.Json 静默丢弃）
PUT {"imuSensitivity":2.0} → 200（顶层，被丢弃）
```
- 前端**没有**发这两个字段（`updateDevice` 全部 4 个调用点已核对：`stores/devices.js:76`、`DeviceDetailView.vue:78/146/179`），故无线上 bug。
- `imuSensitivity` 走的是 `thresholds.imuSensitivity` → **服务端支持**（`DeviceThresholdsConfig.ImuSensitivity`，`ConfigService.cs:67`），实测落库 `1.5` ✅。
- `speech` 是**全局**配置，走 `PUT /api/admin/settings`，实测往返成功 ✅。
- 前端已有防御：`DeviceDetailView.vue:166/178` 只在响应确有该字段时才带上。

### 3.4 ⚠️ WZ 未加载时未捕获异常 → 500 空 body（应为 503）

```
System.InvalidOperationException: WZ 未加载，无法读取音乐目录
   at MusicCatalogService.BuildCatalogCore(…) MusicCatalogService.cs:1290
   at WzMusicSource.<>c__DisplayClass11_0.<<ListTracksAsync>b__0>d … WzMusicSource.cs:74
   at AdminEndpoints.<<Map>b__0_11>d … AdminEndpoints.cs:150          ← GET /admin/music/tracks
   at DeviceEndpoints.HandleBgmCmd … DeviceEndpoints.cs:320           ← POST /device/bgm/cmd
```
| 端点 | 无 WZ 实测 | 有 WZ 实测 |
|---|---|---|
| `GET /api/admin/music/tracks?source=wz` | **500**，**空 body**、无 content-type | 200，1167 首 |
| `POST /api/device/bgm/cmd` `cmd=next/prev` | **500**，空 body | 200，`trackId` 正常 |
| `GET /api/admin/music/tracks?source=qq` | 200 `count:0` | — |
| `GET /api/admin/music/tracks?source=bogus` | 404 `{"error":"未知音源：bogus（可用：wz/qq）"}` | — |

影响：`MusicView.loadTracks()` 的 axios reject 里 `data` 为空 → 前端只能显示通用文案「请求失败（HTTP 500）」。
其它同类端点（catalog/materials）都规范返回 **503 `{"error":"WZ 未加载"}`**，此处口径不统一。

**附带矛盾**：无 WZ 时 `GET /api/admin/music/sources` 仍报 `wz.health.state=0`（ok），
detail 写「目录就绪（WZ 曲库尚未扫描）」——而同一时刻 tracks 端点 500。健康态偏乐观。

### 3.5 ✅ 缩略图：无 WZ 也不 DllNotFound（超预期）

`Api/ThumbService.cs:24-25` 明确策略：「所有失败路径（WZ 未加载 / 渲染异常 / 空帧）回落 `RenderPlaceholder` 色块，**不 500**」。

无 WZ 实例（38097）实测——**全部 200 且是合法 PNG**，无 `DllNotFoundException`、无 500：

| 请求 | 状态 | 大小 | 文件类型 |
|---|---|---|---|
| `?type=part&id=12000` | 200 | 625 B | PNG 64×64 RGBA |
| `?type=paperdoll&id=12000` | 200 | 557 B | PNG 64×64 RGBA |
| `?type=mob&id=100100` | 200 | 625 B | PNG 64×64 RGBA |
| `?type=map&id=200000100` | 200 | 812 B | PNG 64×64 RGBA |
| `?type=npc&id=9010000` | 200 | 780 B | PNG 64×64 RGBA |
| `?`（缺参） | 400 | — | 空 body |

有 WZ 对照：`?type=part&id=12000` → 200 / 625 B；`?type=paperdoll&id=12000` → 200 / **38332 B**（真实合成图）。
> 注：无 WZ 时前端**无法区分**「真实渲染」与「占位色块」（都是 200）。这是 UX 提示问题，非功能缺陷。

### 3.6 其它口径备注（非缺陷，但易踩）

1. **`GET /api/device/poll` 不认 `timeoutMs`**：`HandlePoll`（`DeviceEndpoints.cs:201-203`）签名里**没有**该参数，恒定用 `PollMaxWait=55s`。
   实测 `?timeoutMs=1`：无待发指令时**挂满 55003 ms**才返回（`waitedMs:55003`）；有指令时立即返回（`waitedMs:1`）。
2. **缺必填 query 参数返回 400 + 空 body**（无 JSON `error`）：`/api/device/bgm/stream`（缺 `id`）、`/api/admin/thumb`（缺 `type`/`id`）。
   前端 `thumbUrl()`/`paperdollThumbUrl()` 恒定带全参，不受影响；但前端统一错误通道拿不到可读文案。
3. **`POST /api/admin/pair` 非法码 → 404**（不是 400）。前端 `pair()` 走统一 reject，UI 正常报错。
4. **`PUT /api/admin/devices/{id}` 的 `bgm.volume` 越界（999）静默忽略**（返回 200，保留旧值 42），不报 400。
5. **`GET /api/admin/device-logs/*` 的 `total` 是库内总数**，不随 `level/tag/sinceSeq/limit` 过滤变化；
   过滤只作用于 `items`（实测见 §4.3）。前端若拿 `total` 当「过滤后条数」会显示错。
6. **`/api/device/bgm/cmd` 的合法 cmd 集与 Web 指令通道不同**：
   设备侧 `play/pause/next/prev/select/volume`（`DeviceEndpoints.cs:302`）vs Web 侧 `play/pause/resume/stop/next/prev`（`AdminEndpoints.cs:288`）。
   两条通道，各自自洽，前端只用后者。

---

## 4. 关键端点原始 curl 输出

### 4.1 设备日志幂等 `POST /api/device/log` ✅ 通过

请求体（`msgHex` = UTF-8 字节的 hex）：
```json
{"proto":1,"deviceId":"dev-791dd5","since":0,"count":2,
 "logs":[{"seq":1,"ts":1758000000000,"t":1000,"lvl":"I","tag":"boot","msgHex":"e59bbae4bbb6e590afe58aa8"},
         {"seq":2,"ts":1758000001000,"t":2000,"lvl":"E","tag":"wifi","msgHex":"7769666920e696ade5bc80"}]}
```
```
--- 第 1 次上报 ---        {"ok":true,"accepted":2,"skipped":0,"lastSeq":2}
--- 第 2 次同批重传 ---    {"ok":true,"accepted":0,"skipped":0,"lastSeq":2}   ← ✅ 幂等
--- 第 3 次同批重传 ---    {"ok":true,"accepted":0,"skipped":0,"lastSeq":2}   ← ✅ 幂等
--- 追加 seq 3 ---         {"ok":true,"accepted":1,"skipped":0,"lastSeq":3}
```
`GET /api/admin/device-logs/dev-791dd5` 回读（hex 已正确解码为中文）：
```json
{"deviceId":"dev-791dd5","lastSeq":3,"clockSynced":true,"total":3,
 "items":[{"seq":1,"tsUtc":"2025-09-16T05:20:00Z","tsRawMs":1758000000000,"t":1000,"lvl":"I","tag":"boot","msg":"固件启动","receivedUtc":"2026-09-27T07:00:31.699309Z"},
          {"seq":2,"tsUtc":"2025-09-16T05:20:01Z","tsRawMs":1758000001000,"t":2000,"lvl":"E","tag":"wifi","msg":"wifi 断开","receivedUtc":"2026-09-27T07:00:31.699309Z"},
          {"seq":3,"tsUtc":"2025-09-16T05:20:02Z","tsRawMs":1758000002000,"t":3000,"lvl":"W","tag":"net","msg":"重试","receivedUtc":"2026-09-27T07:00:31.76106Z"}]}
```
`GET /api/admin/device-logs/by-uuid/audit-uuid-wz01` → 同形态（`deviceId":"dev-791dd5"`，200）；
不存在的 uuid → `404 {"error":"未注册设备 uuid：no-such-uuid"}`。

### 4.2 `POST /api/admin/devices/{id}/command` 全分支

```
POST {"type":"expression","value":"default"}  → 202 {"ok":true,"seq":1,"type":"expression","value":"default"}
POST {"type":"action","value":"default"}      → 202 {"ok":true,"seq":2,...}
POST {"type":"bubble","value":"你好"}          → 202 {"ok":true,"seq":4,...}
POST {"type":"bubble","value":"啊"×33}         → 400 {"error":"bubble 文本超 95 字节（固件 mp_cmd_t.s=char[96]）"}
POST {"type":"brightness","value":"80"}       → 400 {"error":"brightness 需要 n∈[0,100]"}   ← 正确：亮度走 n 通道
POST {"type":"reboot"}                        → 202 {"ok":true,"seq":14,"type":"reboot"}

── bgm 各 value（重点）──
POST {"type":"bgm","value":"play"}            → 202 {"ok":true,"seq":6,"type":"bgm","value":"play"}
POST {"type":"bgm","value":"pause"}           → 202 {"ok":true,"seq":8,...}
POST {"type":"bgm","value":"resume"}          → 202 {"ok":true,"seq":9,...}
POST {"type":"bgm","value":"stop"}            → 202 {"ok":true,"seq":10,...}
POST {"type":"bgm","value":"next"}            → 202 {"ok":true,"seq":11,...}
POST {"type":"bgm","value":"prev"}            → 202 {"ok":true,"seq":12,...}
POST {"type":"bgm","value":"vol","n":50}      → 202 {"ok":true,"seq":13,"type":"bgm","value":"vol","n":50}
POST {"type":"bgm","value":"vol"}             → 400 {"error":"bgm=vol 需要 n∈[0,100]（固件 vol_apply 夹取 0..100）"}
POST {"type":"bgm","value":"vol","n":150}     → 400 {"error":"bgm=vol 需要 n∈[0,100]（…）"}
POST {"type":"bgm","value":"bogus"}           → 400 {"error":"bgm 的 value 非法：bogus（可用：play/pause/resume/stop/next/prev；音量 vol + n；音源 source + n）"}
POST {"type":"bgm","value":"__probe__"}       → 400 {"error":"bgm 的 value 非法：__probe__（可用：…）"}   ← 探测哨兵，零副作用

POST {"type":"nosuchtype","value":"x"}        → 400 {"error":"type 非法：nosuchtype（可用：expression/action/bubble/brightness/reboot/bgm）"}
POST {"value":"x"}                            → 400 {"error":"type 必填"}
POST {"type":"expression"}                    → 400 {"error":"expression 需要 value"}
POST /api/admin/devices/NOPE/command          → 404 {"error":"设备不存在：NOPE"}
```
**`__probe__` 探测的行为核对**：前端 `probeBgmCommand()`（client.js:171-187）的 `typeRejected` 正则要求文案里出现
`type…非法`，而服务端实际文案是「bgm 的 **value** 非法」→ 不匹配 → 返回 `{supported:true, probeRejected:true}` →
**UI 正确启用 bgm 控件**。结论：探测结果正确（虽依赖了巧合，注释里描述的分支已过期）。

### 4.3 `GET /api/admin/device-logs/{id}` 过滤语义

```
?                → total=3 lastSeq=3 items=3 seqs=[1,2,3]
?sinceSeq=2      → total=3 lastSeq=3 items=1 seqs=[3]      ✅ 生效
?level=E         → total=3 lastSeq=3 items=1 seqs=[2]      ✅ 生效
?tag=wifi        → total=3 lastSeq=3 items=1 seqs=[2]      ✅ 生效
?limit=1         → total=3 lastSeq=3 items=1 seqs=[3]      ✅ 生效（取最新 1 条）
?limit=1&level=I → total=3 lastSeq=3 items=1 seqs=[1]      ✅ 组合生效
```
> **`total` 恒为库内总数**，不随过滤变化。

### 4.4 `GET /api/admin/music/tracks` —— `bytes` 恒为 0

```
$ curl -s "…/api/admin/music/tracks?source=wz"
HTTP 200 | application/json; charset=utf-8 | 129261 bytes
source=wz  count=1167
前 3 首：
[{"id":"Bgm00.img/SleepyWood",  "title":"SleepyWood",  "category":"Bgm00.img","source":"wz","bytes":0},
 {"id":"Bgm00.img/FloralLife", "title":"FloralLife", "category":"Bgm00.img","source":"wz","bytes":0},
 {"id":"Bgm00.img/GoPicnic",   "title":"GoPicnic",   "category":"Bgm00.img","source":"wz","bytes":0}]

bytes != 0 的曲目数：0 / 1167
bytes 取值分布：[(0, 1167)]
```
**判定：`bytes=0` 是「按设计」的，不会让前端/设备误判为空文件。** 三重佐证：

1. **服务端明确注释**（`Music/WzMusicSource.cs:82`）：
   `Bytes = 0, // WZ 曲目未提取前列表不带体积（避免全量解包）；提取后按缓存文件大小`
2. **前端只用于展示**：全 Web 仅 `MusicView.vue:72` 一列 `{ title:'大小', render: row => h('span', fmtBytes(row.bytes)) }`。
   `fmtBytes(0)`（`utils/format.js:10-15`）→ `"0 B"`。**没有任何基于 `bytes` 的禁用/过滤/判空逻辑**。
3. **设备根本看不到 `bytes`**：设备取流走 `GET /api/device/bgm/stream`，实测返回**真 MP3**：
   ```
   $ curl -s -o /tmp/bgm.bin -w "HTTP %{http_code} | %{content_type} | %{size_download} bytes\n" \
       "…/api/device/bgm/stream?deviceId=dev-791dd5&source=wz&id=Bgm00.img%2FSleepyWood"
   HTTP 200 | audio/mpeg | 1666351 bytes
   $ file /tmp/bgm.bin
   MPEG ADTS, layer III, v2, 80 kbps, 22.05 kHz, JntStereo
   ```
   （`GetTrackStreamAsync` 未命中缓存时按需从 WZ 提取 → `Bytes` 元数据用提取大小，`WzMusicSource.cs:151` / `:164`。）

> 唯一副作用：曲库页「大小」列 1167 行全是「0 B」。属**元数据缺失的展示瑕疵**，非「空文件」误判。

### 4.5 收藏同步 `GET/PUT /api/admin/materials/favorites` ✅

```
GET  /api/admin/materials/favorites → 200 {"favorites":{"map":[],"mob":[],"npc":[]}}
PUT  {"favorites":{"map":["200000100","220000100"],"mob":["100100"],"npc":["9010000"]}}
     → 200 {"ok":true,"favorites":{"map":["200000100","220000100"],"mob":["100100"],"npc":["9010000"]}}
GET  → 200 {"favorites":{"map":["200000100","220000100"],"mob":["100100"],"npc":["9010000"]}}   ✅ 落库
PUT  {} → 200 {"ok":true,"favorites":{"map":[],"mob":[],"npc":[]}}                              ← 缺 favorites = 清空
PUT  {"favorites":{"pd_hair":["30000"]}} → 200 {"ok":true,"favorites":{"map":[],"mob":[],"npc":[],"pd_hair":["30000"]}}  ← 非固定桶也收
```
契约与前端 `client.js:238-239` 的约定**完全一致**。

### 4.6 `PUT /api/admin/devices/{id}` 设备配置落库回验

```
PUT {"name":"审计设备"}                                    → 200  name="审计设备"
PUT {"bgm":{"source":"wz","volume":42}}                    → 200  bgm={"source":"wz","volume":42}
PUT {"bgm":{"volume":999}}                                 → 200  （静默忽略，保留 42）
PUT {"thresholds":{"imuSensitivity":1.5,"imuDeadzoneDeg":9,"tapLightG":2.5,"tapHardG":4.5,"idleToClockMin":6}}
                                                           → 200  thresholdsSource="device"，字段全落库
PUT {"thresholds":null}                                    → 200  回「跟随全局」
PUT {"petConfig":{"hair":"30000"}}                         → 200  petConfig={"hair":"30000"}
PUT {"petConfig":null}                                     → 200  清空回默认宠物
PUT {"speech":{...}} / {"imuSensitivity":2.0}              → 200  静默忽略（DTO 无此字段，见 §3.3）
PUT /api/admin/devices/NOPE                                → 404  {"error":"设备不存在：NOPE"}
```
`PUT /api/admin/settings` 往返（含 `speech` / `device.imuSensitivity`）：
```
PUT 全量 config（speech.enabled=true, idleSec=120, lines=["审计台词一","审计台词二"], device.imuSensitivity=1.8）
  → 200 {"ok":true,"config":{…}}
回读 → speech={"enabled":true,"idleSec":120,"lines":["审计台词一","审计台词二"]}
       device.imuSensitivity=1.8                              ✅ 落库
PUT 非法 WZ 路径 → 400 {"errors":["WZ 路径不存在：/no/such/wz"]}   ✅（前端拦截器兼容 errors 数组）
```

### 4.7 `POST /api/device/bgm/cmd`（设备侧通道，WZ 已加载）

```
cmd=play    → 200 {"ok":true,"cmd":"play","source":"wz","volume":60}
cmd=pause   → 200 {"ok":true,"cmd":"pause","source":"wz","volume":60}
cmd=next    → 200 {"ok":true,"cmd":"next","source":"wz","trackId":"Bgm00.img/FloralLife","id":1236564701,"volume":60}
cmd=prev    → 200 {"ok":true,"cmd":"prev","source":"wz","trackId":"Bgm_Picture.img/4350046","id":1930575787,"volume":60}
cmd=select  → 200 {"ok":true,"cmd":"select","source":"wz","volume":60}
cmd=volume  → 200 {"ok":true,"cmd":"volume","source":"wz","volume":60}
cmd=bogus   → 400 {"error":"cmd 非法：bogus（可用：play/pause/next/prev/select/volume）"}
缺 deviceId → 400 {"error":"deviceId 必填"}
```

---

## 5. 命令清单

### 5.1 实际跑过（有原始输出佐证）

**静态盘点**
```bash
grep -rn "MapGet\|MapPost\|MapPut\|MapDelete\|MapPatch" --include="*.cs" . | grep -v /obj/ | grep -v /bin/
grep -rn "http\.\(get\|post\|put\|delete\|patch\)\|thumbUrl\|paperdollThumbUrl\|/api/" --include="*.vue" --include="*.js" Web/src
grep -rn "updateDevice(\|applyPetConfig\|listPresets\|sendBgmCommand\|bytes" Web/src
python3  # 扫描全仓库「同行 // 吞掉 Map* 注册」
```

**起服务端（3 个实例，全部用后 kill）**
```bash
# 无 WZ（任务指定）
MINIPET_DATA_DIR=/tmp/audit_srv "/Volumes/SSD/C#/dotnet" bin/Release/net9.0/MinipetServer.dll --urls http://127.0.0.1:38097
# 有 WZ（补测 catalog/materials/music/thumb 真实渲染）
MINIPET_DATA_DIR=/tmp/audit_wz  "…/dotnet" bin/Release/net9.0/MinipetServer.dll --urls http://127.0.0.1:38098
# 修复验证（隔离构建产物，不动 bin/Release）
"…/dotnet" build MinipetServer.csproj -c Release -o /tmp/audit_build
MINIPET_DATA_DIR=/tmp/audit_fix "…/dotnet" /tmp/audit_build/MinipetServer.dll --urls http://127.0.0.1:38200
# 补测 bgm/cmd（有 WZ）
MINIPET_DATA_DIR=/tmp/audit_v2  "…/dotnet" bin/Release/net9.0/MinipetServer.dll --urls http://127.0.0.1:38201
```

**curl 覆盖**（40 个端点 × 正常/异常入参，脚本 `/tmp/sweep.sh` + `/tmp/mutate.sh`，已删）
```bash
curl -s -o … -w "HTTP %{http_code} | %{content_type}" -X {GET,POST,PUT,DELETE} "$B$path" -H 'Content-Type: application/json' -d "$body"
curl … -X POST /api/device/hello -d '{"uuid":"audit-uuid-0001","firmware":"audit-fw-1.0","profile":{…}}'   # 造模拟设备
curl … -X POST /api/device/log  -d "$BATCH"   # ×3 同批 → 验幂等
curl -s "…/api/device/bgm/stream?deviceId=…&source=wz&id=Bgm00.img%2FSleepyWood" -o /tmp/bgm.bin; file /tmp/bgm.bin
xxd -l 16 /tmp/bgm.bin
python3 -c "…"   # 统计 music/tracks 的 bytes 分布、校验 presets JSON 可解析
lsof -nP -iTCP -sTCP:LISTEN | grep -E "380[0-9][0-9]|382[0-9][0-9]"   # 端口占用核对/避让
```

**清理核对**
```bash
lsof -nP -iTCP -sTCP:LISTEN | grep -E ":(38097|38098|38200|38201)\b"   # → 全部已释放
rm -rf /tmp/audit_srv /tmp/audit_wz /tmp/audit_fix /tmp/audit_build /tmp/audit_v2 /tmp/sweep.sh /tmp/mutate.sh
```

### 5.2 只读代码推断（**未**实测）

- `/api/admin/devices/{id}/fonts` 的**实际字体包内容**（只验了状态码 200 与响应字段）。
- WZ 提取/导出路径（`/api/device/asset/{hash}` 的成功分支——本次数据目录无导出产物，只测到 404）。
- `/api/admin/device-logs/by-uuid/{uuid}` 在设备**离线/未校时**（`clockSynced:false`）时的前端展示回退。
- 固件侧对 `commands[].payload` 的真实消费（未碰 `Firmware/`，按约束）。
- 前端 `ui-smoke.mjs` 未运行。
- 他人 15:01 新增的「负数 id 反查」（`WzMusicSource.cs:98-108`）**未实测**——非本次范围。

### 5.3 实测产物版本对照（重要）

| 实例 | 端口 | 跑的产物 | 覆盖的结论 |
|---|---|---|---|
| 无 WZ | 38097 | 14:50:14 原始 Release | §1 总表、§3.4（无 WZ 500）、§3.5（thumb 占位）、§4.3 |
| 有 WZ | 38098 | 14:50:14 原始 Release | §1 总表、§3.1–3.3、§4.2、§4.4–4.6 |
| 修复验证 | 38200 | **本次隔离构建** `/tmp/audit_build` | §2/§6 presets 修复 |
| 补测 | 38201 | 15:02:04 新 Release | §4.7（bgm/cmd，含他人新增 `id`） |
| 复核 | 38202 | 15:02:04 新 Release | §0 顶部「并行改动提示」的 1/2/3 条 |

### 5.4 端口占用记录（避让，未干扰他人）

| 端口 | 归属 | 处置 |
|---|---|---|
| 38097 / 38098 / 38200 / 38201 | 本次审计 | 用后已 kill，已确认释放 |
| 38090 | NAS（Python） | **未碰** |
| 38095 / 38096 / 38099 / 38101 | 其它 agent 的 dotnet 实例 | **未碰**（38099 曾被误选，发现占用后立即改用 38200） |

> ⚠️ 一次自我纠错：首次尝试把修复验证实例起在 **38099**，该端口已被他人占用 → 绑定失败（`AddressInUseException`，进程 exit 134），
> 而我随后的 curl 实际打到了**别人的老产物**上，一度误判「修复无效」。换到 38200 后才得到正确结论。
> 报告 §2 的修复结论来自 38200（`/tmp/audit_build` 新产物）实测。

---

## 6. 代码改动（仅 1 处，1 行；已构建验证）

**文件**：`Server/MinipetServer/Api/AdminEndpoints.cs`（唯一改动，符合「前端在调、服务端确实没有、修复 ≤15 行」的放行条件）

```diff
diff --git a/Server/MinipetServer/Api/AdminEndpoints.cs b/Server/MinipetServer/Api/AdminEndpoints.cs
index 5abd512..8845ee1 100644
--- a/Server/MinipetServer/Api/AdminEndpoints.cs
+++ b/Server/MinipetServer/Api/AdminEndpoints.cs
@@ -118,7 +118,8 @@ public static class AdminEndpoints
             return Results.Json(new { ok = true, favorites = saved });
         });
 
-        // ── 纸娃娃预设 CRUD（data/presets/，供设备选择器「纸娃娃 tab」，E7/E4）──        g.MapGet("/presets", (PresetStore presets) => Results.Json(new { presets = presets.List() }));
+        // ── 纸娃娃预设 CRUD（data/presets/，供设备选择器「纸娃娃 tab」，E7/E4）──
+        g.MapGet("/presets", (PresetStore presets) => Results.Json(new { presets = presets.List() }));
         g.MapPost("/presets", (PresetUpsertRequest body, PresetStore presets) =>
         {
             if (string.IsNullOrWhiteSpace(body?.Name))
```

**验证**：`dotnet build -c Release -o /tmp/audit_build` → `0 Error(s)`（66 warnings，均为既有 nullable 告警）；
隔离新产物实测 `GET /api/admin/presets` → `200 application/json` / `{"presets":[{"id":"p-86898be5","name":"神子","type":"paperdoll"}]}`。

**当前 `bin/Release` 状态**：他人已于 `15:02:04` 从含本修复的工作树重建，**复核确认修复已生效（38202 实测 200 JSON）→ 无需再次重建**。
本修复**有意不自行重建 `bin/Release`**，以免打断其它 agent 正在运行的实例（验证走独立输出目录 `/tmp/audit_build`）。

**未做**（有意）：
- 未改 §3.1 的双重包裹（属「已注册但形状不一致」，不在放行条件内，交回决策）。
- 未改前端任何文件；**未碰 `Firmware/`**；未执行任何 `git commit/push/checkout/stash`。
- `git status` 中的 `Firmware/*`、`Server/.../DeviceEndpoints.cs`、`Server/.../Music/WzMusicSource.cs` 改动**均非本次产物**（其它 agent 并行修改）。

---

## 7. 建议优先级

| 优先级 | 项 | 位置 | 说明 |
|---|---|---|---|
| P0 | ✅ 已修且已在 `bin/Release` 生效：`GET /admin/presets` 被注释吞掉 | AdminEndpoints.cs:121→122 | 预设列表曾静默为空；当前产物实测 200 JSON |
| P1 | `PUT /admin/devices/{id}` 双重包裹 `device` | AdminEndpoints.cs:90（vs Detail :719） | 潜伏 bug；`stores/devices.js:86` 取错层 |
| P2 | WZ 未加载时 `music/tracks`、`device/bgm/cmd` 返回 500 空 body | AdminEndpoints.cs:150、DeviceEndpoints.cs:320 | 应与 catalog/materials 统一为 503 `{"error":"WZ 未加载"}` |
| P2 | `music/sources` 无 WZ 时仍报 `state=0 (ok)` | WzMusicSource.Health | 健康态偏乐观，与 tracks 500 自相矛盾 |
| P3 | 前端 3 处过期注释 / `interfaces-needed-from-server.md` §T3/T6 | client.js:168/234/415、Web/docs | 无功能影响，但会误导下一轮开发 |
| P3 | 曲库「大小」列恒显示 `0 B` | WzMusicSource.cs:82 + MusicView.vue:72 | 按设计；可考虑显示「—」而非「0 B」以免误解 |
