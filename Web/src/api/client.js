import axios from 'axios'

/**
 * 统一 Axios 实例（E4）。
 * - baseURL '/api'：开发期走 Vite proxy（vite.config.js），生产构建产物由 ASP.NET Core
 *   wwwroot 同源托管（E3 单容器），两种形态都无需改代码。
 * - 错误统一 console 归因；业务侧拿到的 rejection 带 serverError 字段
 *   （后端约定的 { error: "..." } / { errors: [...] }），方便页面兜底展示。
 */
const http = axios.create({
  baseURL: '/api',
  timeout: 10000,
  headers: { 'Content-Type': 'application/json' },
})

http.interceptors.response.use(
  (res) => res,
  (err) => {
    const status = err?.response?.status
    const data = err?.response?.data
    const serverError =
      (data && (data.error || (Array.isArray(data.errors) && data.errors.join('；')))) ||
      (err?.code === 'ECONNABORTED' ? '请求超时（10s）' : null) ||
      (status ? `请求失败（HTTP ${status}）` : '网络不可达（服务端未启动？）')
    // 统一错误出口：页面只管兜底渲染，日志这里一次打全
    console.error(`[api] ${err?.config?.method?.toUpperCase()} ${err?.config?.url} → ${status ?? 'no-response'}: ${serverError}`)
    err.serverError = serverError
    return Promise.reject(err)
  }
)

/** 从 rejection 里取服务端可读错误文案。 */
export function errText(e, fallback = '请求失败') {
  return e?.serverError || e?.message || fallback
}

// ── 缩略图 URL 拼装（GET，直接喂给 <img>/<n-image>，不走 axios）────────────
// 服务端：64×64 PNG，data/cache/thumbs 磁盘缓存（type=part/paperdoll/mob/npc/map 均真实渲染）
export function thumbUrl(type, id) {
  return `/api/admin/thumb?type=${encodeURIComponent(type)}&id=${encodeURIComponent(id)}`
}

// ── 健康锚点（M5 部署验收）───────────────────────────────────────────────
export function health() {
  return http.get('/health').then((r) => r.data)
}

// ── 设备（E13）───────────────────────────────────────────────────────────
// GET /admin/devices → { devices: [...] }（卡片字段：online/hasPetConfig/health 等）
export function listDevices() {
  return http.get('/admin/devices').then((r) => r.data)
}
export function getDevice(id) {
  return http.get(`/admin/devices/${encodeURIComponent(id)}`).then((r) => r.data)
}
// PUT /admin/devices/{id}：换名 / 换宠换装（petConfig 显式传 null=清空回默认）/ bgm / 阈值覆盖
export function updateDevice(id, payload) {
  return http.put(`/admin/devices/${encodeURIComponent(id)}`, payload).then((r) => r.data)
}
// POST /admin/pair：6 位配对码入册（10 分钟有效）
export function pair(code, name = '') {
  return http.post('/admin/pair', { code: String(code).trim(), name }).then((r) => r.data)
}
// POST /admin/ota/{id}：下发升级指令，设备 WiFi 拉包自更新（E11）
export function triggerOta(deviceId, ver) {
  return http.post(`/admin/ota/${encodeURIComponent(deviceId)}`, { ver: String(ver).trim() }).then((r) => r.data)
}

// ── 素材推送到设备（E7/E13：地图 / NPC 资产登记进该设备 manifest）──────────
// POST /admin/devices/{id}/push  body { kind: "map"|"npc", id: "<素材编号>", switch: bool }
//   → 202 { ok: true, note: "后台打包中，完成后自动下发" }（打包在后台 Task.Run，数秒）
//   → 400 kind/id 非法；404 设备不存在；503 WZ 未加载；500 同步段异常
// switch=true 且 kind=map 时：服务端登记资产 → bump rev → 自动下发切图指令（设备自动切换）
// switch 对 npc 无效（服务端只在 kind=map 分支切图）；素材编号保留原始字符串（如 "200000100"）
export function pushMaterial(deviceId, kind, id, switchAfter = true) {
  return http
    .post(`/admin/devices/${encodeURIComponent(deviceId)}/push`, {
      kind: String(kind),
      id: String(id).trim(),
      switch: !!switchAfter,
    })
    .then((r) => r.data)
}

// ── 设备指令（动作 / 表情 / 气泡，E4/E12）────────────────────────────────
// ✅ 服务端端点【已上线】（AdminEndpoints.cs「设备实时指令下发」段）：实测
//   POST {"type":"expression","value":"default"} → 202 {"ok":true,"seq":47,...}（见下方 probeDeviceCommand）。
//   白名单 type：expression / action / bubble / brightness / reboot（+ 工作区已加 bgm）；bubble ≤95 字节。
//   固件侧已支持（Firmware/main/net/poller.c:141-210 摊平 commands[].type + payload：
//   payload 须是裸字符串或 {"id":"..."}，形如 {"value":"..."} 会被固件静默忽略）。
//   本函数仍保留「探测到才启用」语义：老版本部署实例上端点缺失时 UI 自动退回禁用态。
//   期望：POST /api/admin/devices/{id}/command
//         body { type: "expression"|"action"|"bubble", value: "<名字/文本>", durationMs?: number }
//         → 202 { ok: true, seq: <n>, type, value }
export const DEVICE_COMMAND_TYPE = Object.freeze({ EXPRESSION: 'expression', ACTION: 'action', BUBBLE: 'bubble' })

/** 设备指令端点的完整路径（UI 展示「需要服务端提供什么」时用同一份字符串）。 */
export function deviceCommandPath(deviceId) {
  return `/api/admin/devices/${encodeURIComponent(deviceId)}/command`
}

/** 下发设备指令（动作/表情/气泡/bgm）。服务端未上线该端点或未放行该 type 时 reject（404/405/400）。 */
export function sendDeviceCommand(deviceId, type, value, opts = {}) {
  const body = { type: String(type), value: String(value) }
  if (opts.durationMs != null) body.durationMs = Number(opts.durationMs)
  // 数值通道：服务端 DeviceCommandRequest.N → JSON `n`（固件 poller.c 的 pn 通道）
  if (opts.n != null) body.n = Number(opts.n)
  return http.post(`/admin/devices/${encodeURIComponent(deviceId)}/command`, body).then((r) => r.data)
}

/**
 * 探测指令端点是否可用（表情调试卡片开卡/「重新检测」用）。
 * 语义：POST {"type":"expression","value":"default"}——default 是视觉无变化的空操作表情，
 * 端点已上线时这一发不会改变设备观感（仅消耗一个队列 seq）。
 * 返回 { supported: true | false | null, status?, error? }
 *   true  = 路由存在（2xx；或 400/422/503 等业务错误 → 说明路由在，只是入参/依赖问题）
 *   false = 404/405/501 路由不存在 → UI 禁用按钮并给出接口需求
 *   null  = 网络不可达等无法判定
 */
export async function probeDeviceCommand(deviceId) {
  try {
    const res = await sendDeviceCommand(deviceId, DEVICE_COMMAND_TYPE.EXPRESSION, 'default')
    return { supported: true, status: res?.seq != null ? 202 : 200, data: res }
  } catch (e) {
    const status = e?.response?.status
    if (status === 404 || status === 405 || status === 501) return { supported: false, status }
    if (status) return { supported: true, status, error: errText(e) }
    return { supported: null, error: errText(e) }
  }
}

// ── BGM 播放控制（E8 附加：Web 只读展示 + 可选远程下发）───────────────────
// E8 定稿「控制入口在设备触摸屏，Web 只管曲库/歌单/cookie」——本组函数是用户
// 实测反馈后加的「超出需求」的远程控制通道，UI 侧一律「探测到才启用」。
//
// 固件实证（Firmware/main/net/poller.c:208-217，type+payload 通道）：
//   bgm 分支要求 payload 是【字符串】（cJSON_IsString(vitem)），且只认
//   play / pause / resume / stop / next / prev → MP_AUDIO_*；
//   ⚠ vol / source 只在 poller.c:77-88 的旧扁平通道（{t,v,n}）有分支，而
//     CommandQueue 只会产出 {seq,type,payload} 形态 → 数值型音量（{"n":50}）
//     在 poller.c:171-173 被折算成 vitem=NULL → bgm 分支不匹配 → 静默丢弃。
//     故「音量下发」需服务端放行 + 固件补 pn 分支（见 docs/interfaces-needed-from-server.md §T6）。
export const DEVICE_COMMAND_BGM = 'bgm'

/** 固件认的 bgm 值（poller.c:211-216 字符串分支）。 */
export const BGM_COMMAND = Object.freeze({
  PLAY: 'play', PAUSE: 'pause', RESUME: 'resume', STOP: 'stop', NEXT: 'next', PREV: 'prev', VOLUME: 'vol',
})

/**
 * 探测哨兵值：固件字符串分支不认它（m.type 保持 MP_AUDIO_NONE → 不 mp_post_audio），
 * 所以用它探测「服务端是否放行 bgm」对设备零副作用（不会出声、不改音量）。
 */
export const BGM_PROBE_VALUE = '__probe__'

/** 下发一条 bgm 指令：value 取 BGM_COMMAND；音量（VOLUME）需带 opts.n。 */
export function sendBgmCommand(deviceId, value, opts = {}) {
  return sendDeviceCommand(deviceId, DEVICE_COMMAND_BGM, value, opts)
}

/**
 * 探测 admin 指令端点是否放行 type=bgm（曲库页播放控制卡开卡/「重新检测」用）。
 * 语义：POST {"type":"bgm","value":"__probe__"}（哨兵值，设备零副作用）。
 * 返回 { supported: true | false | null, status?, error?, probeRejected? }
 *   true  = 放行 bgm。含「400 但不是『type 非法：bgm』」的情形（例：哨兵值被 value 白名单拒
 *           ——对一个正确的实现来说这是**预期**响应，故 probeRejected=true 而非判失败）
 *   false = 404/405/501 路由不存在；或 400 且错误文案明确说「type 非法/不支持：bgm」
 *           （服务端 AdminEndpoints.cs:281-284 白名单当前就是这一形态）
 *   null  = 网络不可达等无法判定
 */
export async function probeBgmCommand(deviceId) {
  try {
    const res = await sendBgmCommand(deviceId, BGM_PROBE_VALUE)
    return { supported: true, status: res?.seq != null ? 202 : 200, data: res }
  } catch (e) {
    const status = e?.response?.status
    const msg = errText(e)
    if (status === 404 || status === 405 || status === 501) return { supported: false, status, error: msg }
    // 只在「错误文案把 bgm 判为非法 type」时判定不支持；避免把「bgm 的 value 必须是 …」
    // 这类「已认 type、拒了 value」的 400 误判成不支持
    const typeRejected =
      /bgm/i.test(msg) && /type[\s=:：]*[^，,。;\s]{0,12}?(非法|不支持|未知|无效)/i.test(msg)
    if (status === 400 && typeRejected) return { supported: false, status, error: msg }
    if (status) return { supported: true, status, error: msg, probeRejected: true }
    return { supported: null, error: msg }
  }
}

/** 设备 BGM 偏好（服务端已支持：PUT /admin/devices/{id} body { bgm: { source?, volume? } }）。 */
export function setDeviceBgmPrefs(deviceId, { source, volume } = {}) {
  const bgm = {}
  if (source != null) bgm.source = String(source)
  if (volume != null) bgm.volume = Number(volume)
  return updateDevice(deviceId, { bgm })
}

/** 设备事件环形日志（GET /admin/logs/{id} → { deviceId, online, note, lines: [...], events: [...] }）。 */
export function getDeviceLogs(deviceId) {
  return http.get(`/admin/logs/${encodeURIComponent(deviceId)}`).then((r) => r.data)
}

// ── 纸娃娃预设（E4 编辑器 → E7 设备选择器「纸娃娃 tab」）──────────────────
export function listPresets() {
  return http.get('/admin/presets').then((r) => r.data)
}
export function createPreset(name, type, data) {
  return http.post('/admin/presets', { name, type, data }).then((r) => r.data)
}
export function deletePreset(id) {
  return http.delete(`/admin/presets/${encodeURIComponent(id)}`).then((r) => r.data)
}

// ── 纸娃娃素材目录与真实合成缩略图（docs/ai/web-paperdoll-alignment.md 五）────
// GET /admin/catalog?part={key}&gender={0|1} → { part, total, items: [{ id, name, icon, img? }] }
// icon 已是完整 URL（/api/admin/thumb?type=part&...）；gender 仅 hair/face 生效，可省略
// 404/503 不在此兜底，走 axios reject → 上方拦截器统一 serverError 通道
export function getCatalog(part) {
  return http.get('/admin/catalog', { params: { part } }).then((r) => r.data)
}
// 纸娃娃真实合成 PNG 的 URL（id 为 appearance.js buildPaperdollId 拼串），直接喂 <img>，不走 axios
export function paperdollThumbUrl(id, size = 256) {
  return `/api/admin/thumb?type=paperdoll&id=${encodeURIComponent(id)}&size=${size}`
}

// ── 素材目录（素材浏览页 MaterialsView：地图 / 怪物 / NPC）──────────────────
// GET /admin/materials?kind=map|mob|npc → { kind, total, items: [{ id, name }] }
// 服务端实时扫 WZ（Map/Mob/Npc 目录）出 id+名称清单；WZ 未加载时 reject 走上方统一错误通道
export function getMaterials(kind) {
  return http.get('/admin/materials', { params: { kind } }).then((r) => r.data)
}

// ── 素材收藏（E4「地图选择含收藏」）────────────────────────────────────────
// ⚠ 服务端**当前没有**收藏存储/端点（AdminEndpoints.cs 无 /materials/favorites 路由）→
//   Web 侧按「探测到才启用」做：404/405/501 判「端点缺失」，收藏退回 localStorage 单机模式；
//   端点一旦按 Web/docs/interfaces-needed-from-server.md §T7 上线，UI 无需改动即启用同步。
// 约定契约（探测命中时的期望形态）：
//   GET /api/admin/materials/favorites → 200 { favorites: { "map": ["200000100", …], "npc": […] } }
//   PUT /api/admin/materials/favorites  body { favorites: {…} } → 200 { ok: true, favorites: {…} }
export const FAVORITES_PATH = '/api/admin/materials/favorites'

/** 收藏桶（与 Web 侧 localStorage 桶名一致）。 */
export const FAVORITE_BUCKETS = Object.freeze(['map', 'mob', 'npc'])

export function getMaterialFavorites() {
  return http.get('/admin/materials/favorites').then((r) => r.data)
}

export function putMaterialFavorites(favorites) {
  return http.put('/admin/materials/favorites', { favorites }).then((r) => r.data)
}

/**
 * 探测服务端收藏端点是否在位（T3）。
 * 返回 { supported: true|false|null, status?, shapeOk?, spaFallback?, data?, error? }
 *   true  = 路由在位**且**响应含 favorites 对象（shapeOk=true）
 *   false = 路由不存在；或回的不是该端点（含 SPA fallback）→ UI 退回「收藏仅本机 localStorage」
 *   null  = 网络不可达等无法判定
 *
 * ⚠ 实测坑（2026-09-27 真实实例）：未注册的 **GET** `/api/**` 不返回 404，而是被 SPA fallback
 *   接走 → `HTTP 200 + Content-Type: text/html`（index.html）。只按状态码判定会把「端点缺失」
 *   误判成「在位」→ 这里对 200 也校验响应形状与 HTML 兜底。
 *   （对照：同一路径 POST/PUT → 405，所以 POST 型探针不受影响。）
 */
export async function probeMaterialFavorites() {
  try {
    const res = await http.get('/admin/materials/favorites')
    const data = res?.data
    const favs = data?.favorites
    const shapeOk = !!favs && typeof favs === 'object' && !Array.isArray(favs)
    if (!shapeOk) {
      const spaFallback = typeof data === 'string' && /^\s*(<!DOCTYPE html|<html)/i.test(data)
      return {
        supported: false,
        shapeOk: false,
        spaFallback,
        status: res?.status ?? 200,
        error: spaFallback
          ? 'HTTP 200 但回的是 SPA index.html（GET 路由未注册，SPA fallback）'
          : 'HTTP 200 但响应缺 favorites 对象',
      }
    }
    return { supported: true, shapeOk: true, status: 200, data }
  } catch (e) {
    const status = e?.response?.status
    if (status === 404 || status === 405 || status === 501) return { supported: false, status }
    if (status) return { supported: true, shapeOk: false, status, error: errText(e) }
    return { supported: null, error: errText(e) }
  }
}

// ── 曲库与音源（E8：Web 只管曲库/cookie/启停，不做点歌）───────────────────
/**
 * 音源健康态枚举（服务端 MusicSourceState，Server/MinipetServer/Music/IMusicSource.cs:3-9）。
 * ⚠ 实测口径（2026-09-27，`GET /api/admin/music/sources`）：System.Text.Json **按数字**序列化
 *   → `health.state` 是 number（wz=0、qq 未启用=3），**不是**字符串；UI 必须先过 musicStateKey()
 *   再查表，禁止直接对 state 调字符串方法（旧实现 `s.toLowerCase()` 会 TypeError → 整页渲染失败）。
 */
export const MUSIC_SOURCE_STATE = Object.freeze({ OK: 0, DEGRADED: 1, DOWN: 2, DISABLED: 3 })

/** health.state → 'ok'|'degraded'|'down'|'disabled'（兼容服务端将来改字符串枚举的形态）。 */
export function musicStateKey(state) {
  if (typeof state === 'number') return Object.keys(MUSIC_SOURCE_STATE).find((k) => MUSIC_SOURCE_STATE[k] === state)?.toLowerCase() ?? ''
  return String(state ?? '').toLowerCase()
}

// GET /admin/music/tracks?source=wz|qq → { source, count, tracks: [{id,title,category,bytes}] }
export function musicTracks(source) {
  return http.get('/admin/music/tracks', { params: { source } }).then((r) => r.data)
}
// 音源列表（含 health）：GET /admin/music/sources
export function listMusicSources() {
  return http.get('/admin/music/sources').then((r) => r.data)
}
// 单源健康（任务口径 source/health 的实际后端路由）
export function musicSourceHealth(name) {
  return http.get(`/admin/music/sources/${encodeURIComponent(name)}/health`).then((r) => r.data)
}
// 启停（任务口径 source/enable 的实际后端路由；wz 为默认源不可停）
export function setMusicSourceEnabled(name, enabled) {
  return http.put(`/admin/music/sources/${encodeURIComponent(name)}`, { enabled }).then((r) => r.data)
}
// QQ cookie 导入（任务口径 source/cookie 的实际后端路由为 POST .../cookie）
export function setQqCookie(cookie) {
  return http.post('/admin/music/sources/qq/cookie', { cookie }).then((r) => r.data)
}

/**
 * 从 `GET /admin/music/sources` 的 qq 条目里解析 cookie 新鲜度（E4「cookie 超过 7 天告警」）。
 *
 * ⚠ 字段实测口径（2026-09-27 真实实例，AdminEndpoints.cs:146-165 + QqMusicSource.CookieFreshness）：
 *   · `cookieStale`  **恒存在**（所有源都有；非 qq 源恒 false）——服务端算好的判定结果；
 *   · `cookieSavedAtUtc` / `cookieAgeDays` 为 **null 时被 JSON 序列化省略**（WhenWritingNull）
 *     → 前端必须按 undefined 处理，不能假定字段存在；
 *   · 有 cookie 但无导入时间（老配置/手改 JSON）→ stale=true 且 ageDays 缺省（保守告警）。
 *
 * 返回 `{ imported, stale, ageDays, savedAtUtc, unknownAge }`（全为纯派生，不发请求）。
 */
export function qqCookieInfo(src) {
  const ageDays = src?.cookieAgeDays ?? null
  const savedAtUtc = src?.cookieSavedAtUtc ?? null
  const stale = src?.cookieStale === true
  return {
    imported: stale || ageDays != null || savedAtUtc != null,
    stale,
    ageDays, // number | null（null = 未导入 / 老配置无导入时间）
    savedAtUtc,
    unknownAge: stale && ageDays == null, // 老配置：有 cookie 但没记导入时间
  }
}

/**
 * 从 qq 条目的 `gateway` 对象解析网关降级原因（E4/E8）。
 *
 * ⚠ 实测口径：`gateway` 是**对象**（`QqGatewayStatus`，QqGatewayProcess.cs:365-378 的 camelCase 投影：
 *   enabled / scriptFound / running / healthy / cookieLoaded / pid / restarts / lastProbeUtc
 *   + 可选 scriptPath / startedAtUtc / lastError），**不是** "Ok/Degraded" 这种字符串枚举；
 *   整体是否可用由服务端聚合在 `health.state` + `health.detail`。
 *   非 qq 源该字段缺省（省略）→ present=false。
 *
 * 返回 `{ present, level, label, reason }`（level: ok | off | degraded | unknown）。
 */
export function qqGatewayInfo(src) {
  const g = src?.gateway
  if (!g || typeof g !== 'object') return { present: false, level: 'unknown', label: '网关状态未返回', reason: '' }
  if (!g.enabled) {
    return { present: true, level: 'off', label: '网关未启用', reason: g.lastError || '配置 QqMusic.Enabled=false（卡片开关可启用）' }
  }
  if (!g.scriptFound) {
    return {
      present: true, level: 'degraded', label: '未找到网关脚本',
      reason: g.lastError || '网关本体（Rain120/qq-music-api + 适配层）不在本仓库/镜像内，需服务端配好网关',
    }
  }
  if (!g.running) {
    return { present: true, level: 'degraded', label: '网关进程未运行', reason: g.lastError || `脚本 ${g.scriptPath ?? '(未知)'} 未能拉起` }
  }
  if (!g.healthy) {
    return { present: true, level: 'degraded', label: '网关健康检查失败', reason: g.lastError || `pid ${g.pid ?? '—'} 无响应` }
  }
  if (!g.cookieLoaded) {
    return { present: true, level: 'degraded', label: '网关卡在 cookie 未加载', reason: g.lastError || '导入 cookie 后服务端会推送网关' }
  }
  return { present: true, level: 'ok', label: '网关正常', reason: '' }
}

// ── 设置（E3 双通道之 Web 通道，写同一份 data/config/appsettings.json）────
// GET /admin/settings → { config, wzPathExists, note }
export function getSettings() {
  return http.get('/admin/settings').then((r) => r.data)
}
// PUT /admin/settings：全量回传同构 config（服务端对 WZ 路径做存在性硬校验）
export function putSettings(config) {
  return http.put('/admin/settings', config).then((r) => r.data)
}
/**
 * WZ 路径校验（设置页「校验」按钮）。
 * POST /admin/settings/validate-path { path } → { ok, message }（服务端 ConfigService
 * .ValidateWzPath 存在性硬校验，与 PUT settings 保存前校验同一套规则）。
 */
export async function validateWzPath(path) {
  try {
    const res = await http.post('/admin/settings/validate-path', { path })
    return { supported: true, ok: !!res.data?.ok, message: res.data?.message ?? (res.data?.ok ? '路径存在' : '路径不存在') }
  } catch (e) {
    if (e?.response?.status === 404 || e?.response?.status === 405) return { supported: false }
    throw e
  }
}

export default http
