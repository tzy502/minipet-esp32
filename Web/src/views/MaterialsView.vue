<script setup>
/**
 * 素材浏览器（E4）：地图 / 纸娃娃部件 / 怪物 / NPC 四个 tab，目录全部实时读自 WZ。
 * - 地图/怪物/NPC：getMaterials(kind)（服务端扫 WZ Map/Mob/Npc 目录出 id+name 清单）
 * - 纸娃娃部件：16 类目切换（n-select）+ getCatalog(part)，item.icon 已是完整缩略图 URL
 * - 目录内存缓存 per（素材 kind / 类目 part），重复切换不重拉；搜索防抖 300ms 本地过滤
 * - 「加载更多」每次追加 100 条；缩略图 n-image 懒加载，失败回退 emoji
 * - 收藏星标（localStorage minipet.materials.favorites：map/mob/npc 桶 + 纸娃娃按类目 pd_{part}
 *   分桶；旧 paperdoll 桶为占位时代数据，弃用不迁移）；点击 id 复制
 */
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import {
  NButton, NCard, NCheckbox, NEmpty, NImage, NImageGroup, NInput, NResult, NSelect, NSpace,
  NSpin, NTabPane, NTabs, NTag, useMessage,
} from 'naive-ui'
import { getCatalog, getMaterials, thumbUrl } from '../api/client'
import { CATEGORIES, numericId } from '../utils/appearance'

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

// ── 收藏（localStorage 持久化，按桶分桶）──────────────────────────────────
// 桶名：map / mob / npc（素材 tab key）+ 纸娃娃 pd_{part}；旧 paperdoll 桶为占位时代数据，弃用不迁移
const FAV_KEY = 'minipet.materials.favorites'
const favorites = ref(loadFavs())

function loadFavs() {
  try {
    return { map: [], mob: [], npc: [], ...JSON.parse(localStorage.getItem(FAV_KEY) || '{}') }
  } catch {
    return { map: [], mob: [], npc: [] }
  }
}
function persistFavs() {
  localStorage.setItem(FAV_KEY, JSON.stringify(favorites.value))
}
function isFav(bucket, id) {
  return favorites.value[bucket]?.includes(id)
}
function toggleFav(bucket, id) {
  const arr = favorites.value[bucket] ?? (favorites.value[bucket] = [])
  const i = arr.indexOf(id)
  if (i >= 0) arr.splice(i, 1)
  else arr.push(id)
  persistFavs()
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
onMounted(loadList)
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
</style>
