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
// ⚠ 服务端端点【尚未提供】：AdminEndpoints.cs 目前只有 manifest/ota/map/push 四类入队，
//   没有任何 admin 端点能调 CommandQueue.Enqueue 下发 action/expression/bubble。
//   需求清单（方法/路径/请求体/期望响应/固件侧实证）见 Web/docs/interfaces-needed-from-server.md。
//   本函数按需求清单的约定形态预留：一旦服务端上线，UI 无需改动即可生效。
//   期望：POST /api/admin/devices/{id}/command
//         body { type: "expression"|"action"|"bubble", value: "<名字/文本>", durationMs?: number }
//         → 202 { ok: true, seq: <n>, type, value }
//   固件侧已支持（Firmware/main/net/poller.c:141-210 摊平 commands[].type + payload：
//   payload 须是裸字符串或 {"id":"..."}，形如 {"value":"..."} 会被固件静默忽略）。
export const DEVICE_COMMAND_TYPE = Object.freeze({ EXPRESSION: 'expression', ACTION: 'action', BUBBLE: 'bubble' })

/** 设备指令端点的完整路径（UI 展示「需要服务端提供什么」时用同一份字符串）。 */
export function deviceCommandPath(deviceId) {
  return `/api/admin/devices/${encodeURIComponent(deviceId)}/command`
}

/** 下发设备指令（动作/表情/气泡）。服务端未上线该端点时 reject（404/405）。 */
export function sendDeviceCommand(deviceId, type, value, opts = {}) {
  const body = { type: String(type), value: String(value) }
  if (opts.durationMs != null) body.durationMs = Number(opts.durationMs)
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

// ── 曲库与音源（E8：Web 只管曲库/cookie/启停，不做点歌）───────────────────
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
