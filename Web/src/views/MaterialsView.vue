<script setup>
/**
 * 素材浏览器（E4）：地图 / 纸娃娃部件 / 怪物 / NPC 四个 tab，目录全部实时读自 WZ。
 * - 地图/怪物/NPC：getMaterials(kind)（服务端扫 WZ Map/Mob/Npc 目录出 id+name 清单）
 * - 纸娃娃部件：16 类目切换（n-select）+ getCatalog(part)，item.icon 已是完整缩略图 URL
 * - 目录内存缓存 per（素材 kind / 类目 part），重复切换不重拉；搜索防抖 300ms 本地过滤
 * - 「加载更多」每次追加 100 条；缩略图 n-image 懒加载，失败回退 emoji
 * - 收藏星标（localStorage minipet.materials.favorites：map/mob/npc 桶 + 纸娃娃按类目 pd_{part}
 *   分桶；旧 paperdoll 桶为占位时代数据，弃用不迁移）；点击 id 复制
 * - 收藏的服务端同步（T3/E4）：服务端暂无收藏端点 → 运行时探测
 *   （GET /api/admin/materials/favorites，404/405/501 = 缺失）→ 命中才拉取合并 + 回推；
 *   缺失/不可达时**完全退回本机 localStorage**（离线兜底，能力不减），UI 如实标注同步态。
 *   状态与读写共享 Web/src/utils/favorites.js（设备选择器的「最近+收藏」分组读同一份）。
 * - 「推送到设备」（T3/E4/E7）：📤 推送是真正上机动作 ——
 *   选目标设备 → POST /admin/devices/{id}/push {kind,id,switch} → 202 后台打包，
 *   完成后服务端 bump manifest rev + 自动下发切图指令（设备自动切换）。服务端 push 仅
 *   支持 kind=map|npc（AdminEndpoints.cs:254-329），故 mob/纸娃娃 tab 只给禁用态说明。
 */
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import {
  NButton, NCard, NCheckbox, NEmpty, NImage, NImageGroup, NInput, NModal, NResult, NSelect, NSpace,
  NSpin, NTabPane, NTabs, NTag, NTooltip, useMessage,
} from 'naive-ui'
import { errText, getCatalog, getMaterials, pushMaterial, thumbUrl } from '../api/client'
import * as favoritesApi from '../utils/favorites'
import { CATEGORIES, numericId } from '../utils/appearance'
import { useDevicesStore } from '../stores/devices'

const TABS = [
  { key: 'map', label: '地图' },
  { key: 'paperdoll', label: '纸娃娃部件' },
  { key: 'mob', label: '怪物' },
  { key: 'npc', label: 'NPC' },
]

/** 每次追加的格子数（「加载更多」步进）。 */
const PAGE_SIZE = 100

/** 缩略图加载失败/缺失时的回退 emoji（纸娃娃 tab 用类目 icon）。 */
const FALLBACK_EMOJI = { map: '🗺️', mob: '👾', npc: '🧑' }

// ── 收藏（localStorage 真源 + 探测到才启用的服务端同步）───────────────────
// 桶名：map / mob / npc（素材 tab key）+ 纸娃娃 pd_{part}；旧 paperdoll 桶为占位时代数据，弃用不迁移。
// 状态与读写全部收敛到 utils/favorites.js（与 DeviceDetailView 的设备选择器共享同一份单例）。
const {
  isFavorite: isFav, toggleFavorite: toggleFav,
  syncState, syncNote, lastSyncText, probeSync, pullAndMerge, pushFavorites,
} = favoritesApi
const syncing = ref(false)

/** 收藏同步态标签（探针结果 → UI 文案）。 */
const SYNC_TAG = {
  syncing: { type: 'info', label: '同步中…' },
  ok: { type: 'success', label: '收藏已同步服务端' },
  missing: { type: 'default', label: '收藏仅本机（端点缺失）' },
  error: { type: 'warning', label: '收藏仅本机（同步不可用）' },
  unknown: { type: 'default', label: '收藏仅本机' },
}
const syncTag = computed(() => SYNC_TAG[syncState.value] ?? SYNC_TAG.unknown)

/** 开卡：探测服务端收藏端点；命中（且形态正确）才拉取合并，缺失/不可达一律退回本机模式。 */
async function initFavoritesSync() {
  const r = await probeSync()
  if (r.supported === true && r.shapeOk !== false) {
    try {
      await pullAndMerge()
    } catch (e) {
      syncState.value = 'error'
      syncNote.value = errText(e, '同步失败')
    }
  }
}

/** 手动同步（按钮仅在端点命中时出现）：拉取合并（并集，本地不丢）+ 回推。 */
async function syncNow() {
  syncing.value = true
  try {
    await pullAndMerge()
    message.success(lastSyncText.value || '收藏已与服务端合并')
  } catch (e) {
    syncState.value = 'error'
    syncNote.value = errText(e, '同步失败')
    message.error(syncNote.value)
  } finally {
    syncing.value = false
  }
}

// ── 当前上下文（tab + 纸娃娃类目）─────────────────────────────────────────
const message = useMessage()
const activeTab = ref('map')
const currentPart = ref(CATEGORIES[0].key) // 纸娃娃 tab 当前类目
const onlyFav = ref(false)

/** 目录缓存 key：素材 tab 用 kind（map/mob/npc），纸娃娃 tab 用类目 part。 */
const contextKey = computed(() => (activeTab.value === 'paperdoll' ? currentPart.value : activeTab.value))
/** 当前收藏桶名。 */
const favBucket = computed(() => (activeTab.value === 'paperdoll' ? `pd_${currentPart.value}` : activeTab.value))
const activeCategory = computed(() => CATEGORIES.find((c) => c.key === currentPart.value))
const partOptions = CATEGORIES.map((c) => ({ label: `${c.icon} ${c.label}`, value: c.key }))

// ── 目录数据（内存缓存 per contextKey，重复切换不重拉）─────────────────────
const catalogCache = new Map() // contextKey → { items, total }
const items = ref([]) // 归一化 [{ id, name, icon? }]，按数字 id 升序
const total = ref(0)
const loading = ref(false)
const loadError = ref('')
let loadSeq = 0 // 请求代际：context 切换时丢弃旧响应，防串台

async function loadList(force = false) {
  const key = contextKey.value
  if (!key) return
  if (!force && catalogCache.has(key)) {
    const c = catalogCache.get(key)
    items.value = c.items
    total.value = c.total
    loadError.value = ''
    return
  }
  const seq = ++loadSeq
  loading.value = true
  loadError.value = ''
  try {
    // 纸娃娃部件走 catalog（icon 已是完整 URL，不传 gender）；地图/怪物/NPC 走 materials 目录
    const data = activeTab.value === 'paperdoll'
      ? await getCatalog(key)
      : await getMaterials(key)
    if (seq !== loadSeq) return
    const list = [...(data?.items ?? [])]
      .filter((it) => it?.id != null)
      .map((it) => ({ id: String(it.id), name: it.name ?? '', icon: it.icon || '' }))
      .sort((a, b) => numericId(a.id) - numericId(b.id))
    const entry = { items: list, total: data?.total ?? list.length }
    catalogCache.set(key, entry)
    items.value = list
    total.value = entry.total
  } catch (e) {
    if (seq !== loadSeq) return
    loadError.value = e?.serverError || e?.message || '素材目录加载失败（WZ 未加载？到「设置」页配置后重试）'
    items.value = []
    total.value = 0
  } finally {
    if (seq === loadSeq) loading.value = false
  }
}

// ── 搜索（防抖 300ms）+ 本地过滤（name 忽略大小写或 id 包含）+ 只看收藏 ────
const searchText = ref('') // 搜索框原始输入（v-model）
const query = ref('') // 防抖后的过滤词（空 = 全量）
let searchTimer = null
const visibleCount = ref(PAGE_SIZE) // 已展示格子数（「加载更多」累加）

const filteredItems = computed(() => {
  const q = query.value.trim()
  let list = items.value
  if (q) {
    const ql = q.toLowerCase()
    list = list.filter((it) => it.name.toLowerCase().includes(ql) || it.id.includes(q))
  }
  return onlyFav.value ? list.filter((it) => isFav(favBucket.value, it.id)) : list
})
const visibleItems = computed(() => filteredItems.value.slice(0, visibleCount.value))
const hasMore = computed(() => visibleItems.value.length < filteredItems.value.length)
const favCount = computed(() => items.value.filter((it) => isFav(favBucket.value, it.id)).length)

function loadMore() {
  visibleCount.value += PAGE_SIZE
}
function resetListState() {
  searchText.value = ''
  query.value = ''
  visibleCount.value = PAGE_SIZE
}

// ── 缩略图（失败回退 emoji）+ 交互 ────────────────────────────────────────
const failedThumbs = reactive(new Set()) // 加载失败的缩略图 `${contextKey}:${id}` → 回退 emoji

const fallbackEmoji = computed(() =>
  activeTab.value === 'paperdoll'
    ? activeCategory.value?.icon ?? '📦'
    : FALLBACK_EMOJI[activeTab.value] ?? '📦'
)
function thumbSrc(it) {
  return activeTab.value === 'paperdoll' ? it.icon : thumbUrl(activeTab.value, it.id)
}
function thumbFailed(it) {
  return failedThumbs.has(`${contextKey.value}:${it.id}`)
}
function onThumbError(it) {
  failedThumbs.add(`${contextKey.value}:${it.id}`)
}
function searchPlaceholder(tabKey) {
  if (tabKey === 'paperdoll') return activeCategory.value?.label ?? '部件'
  return TABS.find((t) => t.key === tabKey)?.label ?? '素材'
}
function copyId(id) {
  navigator.clipboard?.writeText(id).then(
    () => message.success(`已复制 ${id}`),
    () => message.error('复制失败')
  )
}

// ── 推送到设备（T3：素材上机的真实动作，服务端 POST /admin/devices/{id}/push）────
const devicesStore = useDevicesStore()
const pushShow = ref(false)
const pushTarget = ref(null) // { kind, id, name }
const pushDeviceId = ref(null)
const pushSwitch = ref(true) // 仅 kind=map 生效（服务端分支）；勾选=登记后自动切图
const pushing = ref(false)
const pushResult = ref(null) // { type: 'success'|'error', text }

/** 当前 tab 是否可推送：服务端只接受 map|npc。 */
const pushableKind = computed(() => (activeTab.value === 'map' || activeTab.value === 'npc' ? activeTab.value : null))
const deviceOptions = computed(() =>
  devicesStore.devices.map((d) => ({
    label: `${d.name || '未命名设备'}（${d.deviceId}）${d.online ? ' · 在线' : ' · 离线'}`,
    value: d.deviceId,
  }))
)

function openPush(it) {
  if (!pushableKind.value) {
    message.warning('服务端推送端点仅支持地图 / NPC（kind=map|npc）')
    return
  }
  pushTarget.value = { kind: activeTab.value, id: it.id, name: it.name }
  pushResult.value = null
  pushShow.value = true
  if (!devicesStore.devices.length) {
    devicesStore
      .fetchAll({ silent: true })
      .then(() => {
        if (!pushDeviceId.value) pushDeviceId.value = devicesStore.devices[0]?.deviceId ?? null
      })
      .catch(() => {})
  } else if (!pushDeviceId.value) {
    pushDeviceId.value = devicesStore.devices[0]?.deviceId ?? null
  }
}

/** 错误分支文案（400/404/503 是服务端 push 端点明确定义的三种拒绝）。 */
function pushErrorHint(status) {
  switch (status) {
    case 400: return '请求参数被拒：kind 必须是 map|npc，id 不能为空'
    case 404: return '设备不存在（可能已被移除，刷新设备列表后重选）'
    case 503: return 'WZ 未加载：到「设置」页配置 WZ 路径后重试'
    case 500: return '服务端处理异常（资产打包失败，详见服务端日志）'
    default: return ''
  }
}

async function doPush() {
  if (!pushTarget.value) return
  if (!pushDeviceId.value) {
    message.warning('请先选择目标设备')
    return
  }
  pushing.value = true
  pushResult.value = null
  try {
    const r = await pushMaterial(pushDeviceId.value, pushTarget.value.kind, pushTarget.value.id, pushSwitch.value)
    const text = `已受理（HTTP 202）：${r?.note || '后台打包中'}`
    pushResult.value = {
      type: 'success',
      text: `${text}${pushSwitch.value && pushTarget.value.kind === 'map' ? '；资产登记完成后服务端自动下发切图指令' : ''}`,
    }
    message.success(text)
  } catch (e) {
    const status = e?.response?.status
    const hint = pushErrorHint(status)
    const text = `${errText(e)}${status ? `（HTTP ${status}）` : ''}${hint ? ` · ${hint}` : ''}`
    pushResult.value = { type: 'error', text }
    message.error(text)
  } finally {
    pushing.value = false
  }
}

// ── 侦听：切 tab / 切类目重置并按缓存拉取；搜索防抖；过滤条件变化分页归位 ──
watch(activeTab, () => {
  resetListState()
  loadList()
})
watch(currentPart, () => {
  if (activeTab.value === 'paperdoll') {
    resetListState()
    loadList()
  }
})
watch(searchText, (v) => {
  clearTimeout(searchTimer)
  searchTimer = setTimeout(() => {
    query.value = (v || '').trim()
  }, 300)
})
watch([query, onlyFav], () => {
  visibleCount.value = PAGE_SIZE
})
onMounted(() => {
  loadList()
  initFavoritesSync()
})
onBeforeUnmount(() => clearTimeout(searchTimer))
</script>

<template>
  <div>
    <n-card size="small" class="mb12">
      <n-space align="center" justify="space-between" style="width: 100%">
        <n-space align="center">
          <n-tag :bordered="false" type="info">服务端渲染缩略图</n-tag>
          <span class="hint">素材目录实时读自 WZ（Map/Mob/Npc/Character）</span>
        </n-space>
        <n-space align="center" :size="8">
          <n-tooltip trigger="hover">
            <template #trigger>
              <n-tag :bordered="false" size="small" :type="syncTag.type">{{ syncTag.label }}</n-tag>
            </template>
            <div style="max-width: 380px; line-height: 1.7">
              <div>收藏以本机 localStorage（<code>minipet.materials.favorites</code>）为真源 —— 离线/端点缺失时能力不减。</div>
              <div v-if="syncState === 'ok'">服务端端点 <code>GET/PUT /api/admin/materials/favorites</code> 在位：进入本页自动拉取合并（并集，本地不丢）。{{ lastSyncText }}</div>
              <div v-else-if="syncState === 'missing'">
                服务端暂无收藏端点（探测：{{ syncNote || '路由未注册' }}）→ 同步未启用，收藏只在本机浏览器。
                接口需求已写入 <code>Web/docs/interfaces-needed-from-server.md</code> §T7。
              </div>
              <div v-else>探测未完成或不可达{{ syncNote ? `（${syncNote}）` : '' }}。</div>
            </div>
          </n-tooltip>
          <n-button v-if="syncState === 'ok'" size="tiny" secondary :loading="syncing" @click="syncNow">同步到服务端</n-button>
          <span class="hint">★ 收藏已喂给设备选择器（「最近+收藏」）；📤 推送才是上机动作（服务端 push 支持地图/NPC）</span>
        </n-space>
        <n-checkbox v-model:checked="onlyFav">只看收藏（{{ favCount }}）</n-checkbox>
      </n-space>
    </n-card>

    <n-tabs v-model:value="activeTab" type="line" animated>
      <n-tab-pane v-for="tab in TABS" :key="tab.key" :name="tab.key" :tab="tab.label">
        <!-- 工具栏：纸娃娃类目切换（n-select）+ 搜索框（防抖 300ms）+ 已显示/总数 -->
        <div class="toolbar">
          <n-select
            v-if="tab.key === 'paperdoll'"
            v-model:value="currentPart"
            :options="partOptions"
            size="small"
            class="part-select"
          />
          <n-input
            v-model:value="searchText"
            clearable
            size="small"
            class="search"
            :placeholder="`搜索${searchPlaceholder(tab.key)}名称或 ID`"
            :disabled="loading || !!loadError"
          >
            <template #prefix>🔍</template>
          </n-input>
          <span class="hint">
            {{ loading ? '加载中…' : `已显示 ${visibleItems.length} / ${filteredItems.length} 条 · 共 ${total} 条` }}
          </span>
        </div>

        <!-- 目录区：loading（n-spin）/ 错误态（重试）/ 空态 / 缩略图格子 -->
        <div v-if="loading" class="spin-center"><n-spin size="large" /></div>
        <n-result v-else-if="loadError" status="warning" title="素材目录加载失败" :description="loadError">
          <template #footer>
            <n-button size="small" @click="loadList(true)">重试</n-button>
          </template>
        </n-result>
        <n-empty
          v-else-if="!visibleItems.length"
          :description="onlyFav ? '该分类暂无收藏项' : '无匹配条目（试试清除搜索）'"
          style="padding: 60px 0"
        />
        <n-image-group v-else>
          <div class="thumb-grid">
            <div v-for="it in visibleItems" :key="it.id" class="thumb-cell">
              <div v-if="thumbFailed(it) || !thumbSrc(it)" class="thumb-fallback">{{ fallbackEmoji }}</div>
              <n-image
                v-else
                lazy
                :src="thumbSrc(it)"
                :alt="it.name || it.id"
                width="96"
                height="96"
                object-fit="cover"
                style="border-radius: 6px"
                @error="onThumbError(it)"
              />
              <div class="thumb-name" :title="it.name">{{ it.name || '—' }}</div>
              <div class="thumb-id" title="点击复制 id" @click="copyId(it.id)">{{ it.id }}</div>
              <n-button
                class="star"
                size="tiny"
                circle
                :type="isFav(favBucket, it.id) ? 'warning' : 'default'"
                :tertiary="!isFav(favBucket, it.id)"
                @click="toggleFav(favBucket, it.id)"
              >
                {{ isFav(favBucket, it.id) ? '★' : '☆' }}
              </n-button>
              <!-- 推送到设备：map/npc 可推；mob/纸娃娃服务端无对应 push 分支 → 禁用态说明 -->
              <n-tooltip v-if="tab.key === 'map' || tab.key === 'npc'" trigger="hover">
                <template #trigger>
                  <n-button class="push" size="tiny" circle secondary type="primary" @click="openPush(it)">📤</n-button>
                </template>
                推送到设备（{{ tab.key === 'map' ? '地图' : 'NPC' }}资产登记 + 自动切换）
              </n-tooltip>
              <n-tooltip v-else trigger="hover">
                <template #trigger>
                  <n-button class="push" size="tiny" circle secondary disabled>📤</n-button>
                </template>
                服务端 push 端点只支持 kind=map|npc（{{ tab.key === 'mob' ? '怪物' : '纸娃娃部件' }}暂无可推送资产类型）
              </n-tooltip>
            </div>
          </div>
        </n-image-group>

        <!-- 加载更多：每次追加 100 条 -->
        <div v-if="!loading && !loadError && hasMore" class="more">
          <n-button size="small" @click="loadMore">
            加载更多（还有 {{ filteredItems.length - visibleItems.length }} 条）
          </n-button>
        </div>
      </n-tab-pane>
    </n-tabs>

    <!-- 推送到设备（T3）：选设备 → POST /admin/devices/{id}/push → 202 后台打包 -->
    <n-modal
      v-model:show="pushShow"
      preset="card"
      title="推送到设备"
      style="width: 460px"
      :mask-closable="!pushing"
    >
      <n-space vertical :size="12">
        <n-space align="center" :size="8">
          <n-tag :bordered="false" type="info">{{ pushTarget?.kind === 'map' ? '地图' : 'NPC' }}</n-tag>
          <b>{{ pushTarget?.name || '—' }}</b>
          <span class="hint">{{ pushTarget?.id }}</span>
        </n-space>

        <div>
          <div class="hint">目标设备</div>
          <n-select
            v-model:value="pushDeviceId"
            :options="deviceOptions"
            :disabled="pushing || !deviceOptions.length"
            placeholder="选择设备"
          />
          <div v-if="!deviceOptions.length" class="hint err">
            暂无设备：先到「设备总览」配对（POST /admin/pair）
          </div>
        </div>

        <n-checkbox v-model:checked="pushSwitch" :disabled="pushing">
          登记后立即切换（仅地图生效；NPC 只登记资产）
        </n-checkbox>

        <div class="hint">
          推送 = 服务端把该素材打包进此设备 manifest（数秒，HTTP 202 受理）→ 设备轮询到 rev
          变化后自动拉包 → 完成后自动切换。收藏（★）以本机 localStorage 为真源
          <template v-if="syncState === 'ok'">，并已同步到服务端</template>
          <template v-else>（服务端收藏端点缺失 → 仅本机）</template>，且已喂给设备详情页的地图选择器。
        </div>

        <n-result
          v-if="pushResult"
          size="small"
          :status="pushResult.type === 'success' ? 'success' : 'error'"
          :title="pushResult.type === 'success' ? '已受理' : '推送失败'"
          :description="pushResult.text"
        />

        <n-space justify="end">
          <n-button :disabled="pushing" @click="pushShow = false">关闭</n-button>
          <n-button
            type="primary"
            :loading="pushing"
            :disabled="!pushDeviceId"
            @click="doPush"
          >
            推送到该设备
          </n-button>
        </n-space>
      </n-space>
    </n-modal>
  </div>
</template>

<style scoped>
.mb12 { margin-bottom: 12px; }
.hint { font-size: 12px; opacity: 0.6; }
.toolbar { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; margin-bottom: 12px; }
.part-select { width: 170px; flex: none; }
.search { flex: 1; min-width: 200px; max-width: 360px; }
.spin-center { display: flex; justify-content: center; padding: 60px 0; }
.more { display: flex; justify-content: center; margin-top: 14px; }
.thumb-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(112px, 1fr));
  gap: 14px;
}
.thumb-cell { position: relative; text-align: center; }
.thumb-fallback {
  width: 96px;
  height: 96px;
  margin: 0 auto;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 34px;
  border-radius: 6px;
  background: rgba(128, 128, 140, 0.1);
}
.thumb-name {
  font-size: 11px;
  margin-top: 4px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.thumb-id {
  font-size: 11px;
  opacity: 0.7;
  cursor: copy;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.star { position: absolute; top: 2px; right: 2px; }
.push { position: absolute; top: 2px; left: 2px; }
.err { color: #d03050; }
</style>
