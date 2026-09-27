/**
 * 素材收藏 / 最近使用（E4「地图选择含收藏（喂给设备选择器的「最近+收藏」）」）。
 *
 * 设计口径（T3）：
 *   1. **localStorage 是本地真源、也是离线兜底**——`minipet.materials.favorites` 键与旧版完全兼容
 *      （桶名 map / mob / npc / pd_{part}，旧数据不迁移不丢）；`minipet.materials.recent` 为新增。
 *   2. 服务端**当前没有**收藏存储/端点（AdminEndpoints.cs 无该路由）→ 按「探测到才启用」：
 *      探针命中（`GET /api/admin/materials/favorites` 在位）才把收藏同步上去；
 *      缺失（404/405/501）则完全退回本机模式，UI 如实标注，不假装已同步。
 *   3. 服务端上线后的合并语义 = **并集**（local ∪ remote），任何一侧都不丢；再整体回推。
 *      接口契约与需求见 Web/docs/interfaces-needed-from-server.md §T7。
 *   4. 「最近」在成功推送到设备时记录（E4 的「最近」= 最近真的用过）。
 *
 * 模块级单例：MaterialsView（星标）与 DeviceDetailView（设备选择器分组）共享同一份响应式状态，
 * 两个页面互相切换时不重读、不串台。
 */
import { ref } from 'vue'
import { getMaterialFavorites, probeMaterialFavorites, putMaterialFavorites } from '../api/client'

/** 收藏 localStorage 键（沿用旧键名，保证既有浏览器数据不丢）。 */
export const FAV_KEY = 'minipet.materials.favorites'
/** 最近使用 localStorage 键（新增）。 */
export const RECENT_KEY = 'minipet.materials.recent'
/** 「最近」每桶保留条数。 */
export const RECENT_MAX = 12

/** 固定三桶（其它桶名如 pd_{part} 动态出现，不做白名单限制）。 */
const BASE_BUCKETS = ['map', 'mob', 'npc']

// ── localStorage 安全读写（无权限/隐私模式下不抛）─────────────────────────
function lsGet(key) {
  try { return localStorage.getItem(key) } catch { return null }
}
function lsSet(key, value) {
  try { localStorage.setItem(key, value); return true } catch { return false }
}

function normalizeBuckets(raw) {
  const out = { map: [], mob: [], npc: [] }
  if (raw && typeof raw === 'object' && !Array.isArray(raw)) {
    for (const [k, v] of Object.entries(raw)) {
      if (Array.isArray(v)) out[k] = v.map((x) => String(x))
    }
  }
  return out
}

// ── 收藏 ─────────────────────────────────────────────────────────────────
export function loadFavorites() {
  try {
    return normalizeBuckets(JSON.parse(lsGet(FAV_KEY) || '{}'))
  } catch {
    return { map: [], mob: [], npc: [] }
  }
}

/** 全局共享的收藏状态。 */
export const favorites = ref(loadFavorites())

export function persistFavorites() {
  return lsSet(FAV_KEY, JSON.stringify(favorites.value))
}

/** 该桶的收藏 id 数组（响应式；设备选择器分组直接用）。 */
export function favoriteIds(bucket) {
  return favorites.value[bucket] ?? []
}

export function isFavorite(bucket, id) {
  return favoriteIds(bucket).includes(String(id))
}

/** 切换收藏，返回切换后是否已收藏。 */
export function toggleFavorite(bucket, id) {
  const sid = String(id)
  const arr = favorites.value[bucket] ?? (favorites.value[bucket] = [])
  const i = arr.indexOf(sid)
  if (i >= 0) arr.splice(i, 1)
  else arr.push(sid)
  persistFavorites()
  return i < 0
}

// ── 最近使用 ─────────────────────────────────────────────────────────────
export function loadRecents() {
  try {
    return normalizeBuckets(JSON.parse(lsGet(RECENT_KEY) || '{}'))
  } catch {
    return { map: [], mob: [], npc: [] }
  }
}

/** 全局共享的「最近使用」状态。 */
export const recents = ref(loadRecents())

export function persistRecents() {
  return lsSet(RECENT_KEY, JSON.stringify(recents.value))
}

/** 该桶的最近使用 id 数组（最新在前）。 */
export function recentIds(bucket) {
  return recents.value[bucket] ?? []
}

/** 记一次「最近使用」（去重后置顶，超出 RECENT_MAX 截断）。 */
export function recordRecent(bucket, id) {
  const sid = String(id)
  if (!sid) return
  const arr = recents.value[bucket] ?? (recents.value[bucket] = [])
  const i = arr.indexOf(sid)
  if (i >= 0) arr.splice(i, 1)
  arr.unshift(sid)
  if (arr.length > RECENT_MAX) arr.length = RECENT_MAX
  persistRecents()
}

// ── 服务端同步（探测到才启用）─────────────────────────────────────────────
/** unknown | ok | missing | error | syncing */
export const syncState = ref('unknown')
export const syncNote = ref('')
/** 最近一次同步动作的可读结果（UI 提示用）。 */
export const lastSyncText = ref('')

/** 探测服务端收藏端点（404/405/501 或 SPA fallback → missing）。 */
export async function probeSync() {
  syncState.value = 'syncing'
  try {
    const r = await probeMaterialFavorites()
    if (r.supported === false) {
      syncState.value = 'missing'
      syncNote.value = r.error || `HTTP ${r.status}`
    } else if (r.supported === true && r.shapeOk === true) {
      syncState.value = 'ok'
      syncNote.value = ''
    } else {
      // 路由存在但响应形态不符 / 服务端报错 / 网络不可达 → 一律退回本机并如实标注原因
      syncState.value = 'error'
      syncNote.value = r.error || `HTTP ${r.status ?? '—'}：响应形态不符`
    }
    return r
  } catch (e) {
    syncState.value = 'error'
    syncNote.value = e?.message || '探测失败'
    return { supported: null, error: syncNote.value }
  }
}

function unionIds(local, remote) {
  const out = [...(Array.isArray(local) ? local.map(String) : [])]
  for (const id of Array.isArray(remote) ? remote.map(String) : []) {
    if (!out.includes(id)) out.push(id)
  }
  return out
}

/**
 * 拉服务端收藏并与本地求并集（本地永不丢），随后把并集回推。
 * 端点缺失时抛错（调用方先 probeSync 判定），不改动本地。
 * 返回 { pulled, pushed, buckets }
 */
export async function pullAndMerge() {
  const data = await getMaterialFavorites()
  const remote = normalizeBuckets(data?.favorites ?? {})
  const merged = { ...favorites.value }
  for (const [bucket, ids] of Object.entries(remote)) {
    merged[bucket] = unionIds(favorites.value[bucket], ids)
  }
  favorites.value = merged
  persistFavorites()
  const pushed = await putMaterialFavorites(exportFavorites())
  const count = Object.values(merged).reduce((n, arr) => n + arr.length, 0)
  lastSyncText.value = `已与服务端合并（拉取 ${Object.values(remote).reduce((n, a) => n + a.length, 0)} 条 / 合并后 ${count} 条${pushed?.ok === false ? '；回推被拒' : ''}）`
  return { pulled: remote, pushed, buckets: merged }
}

/** 只回推本地收藏（不拉取）。 */
export async function pushFavorites() {
  const r = await putMaterialFavorites(exportFavorites())
  const count = Object.values(favorites.value).reduce((n, arr) => n + arr.length, 0)
  lastSyncText.value = `已回推 ${count} 条收藏到服务端`
  return r
}

/** 导出当前收藏（深拷贝，供请求体使用）。 */
export function exportFavorites() {
  return JSON.parse(JSON.stringify(favorites.value))
}

export { BASE_BUCKETS }
