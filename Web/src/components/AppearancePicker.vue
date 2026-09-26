<script setup>
/**
 * 装扮部件选择弹窗（对齐桌面素材浏览器 MaterialBrowserWindow）：
 * - 数据从服务端 catalog API 拉全量（getCatalog），组件内存 Map 缓存 per `${part}|${gender}`，重复打开不重拉
 * - 搜索：防抖 300ms，本地过滤中文名（忽略大小写）或 id 包含
 * - 发型同名折叠：多变体折叠为组行（组名 + N 变体 + ▸/▾），回退名（发型_ 前缀）不折叠
 * - 分页：「加载更多」每次追加 100 行；缩略图懒加载，失败回退类目 emoji
 * - 选中条目 → emit('update:modelValue', id) 并关闭；底部「清空」→ emit null（对齐规范 3.8）
 */
import { computed, reactive, ref, watch, onBeforeUnmount } from 'vue'
import { NModal, NInput, NButton, NSpin, NResult, NEmpty } from 'naive-ui'
import { CATEGORIES, isHairFallbackName, groupHairItems, numericId } from '../utils/appearance'
import { getCatalog } from '../api/client'

const props = defineProps({
  /** 弹窗显隐（父组件 v-model:show） */
  show: { type: Boolean, default: false },
  /** 类目 key（CATEGORIES 里的一项，如 'hair'） */
  part: { type: String, default: '' },
  /** 当前草稿性别（0 男 / 1 女），仅 genderFilter 类目（发型/脸型）拉目录时使用 */
  gender: { type: Number, default: 0 },
  /** 当前已选部件 id（或 null）—— 对应行高亮 */
  modelValue: { type: [String, Number], default: null },
})
const emit = defineEmits(['update:show', 'update:modelValue'])

/** 每次追加的行数（「加载更多」步进） */
const PAGE_SIZE = 100

// ── 目录数据 ─────────────────────────────────────────────────────────────
// 组件内存缓存：`${part}|${gender}` → { items, total }（重复打开不重拉）
const catalogCache = new Map()
const loading = ref(false)
const loadError = ref('')
const allItems = ref([]) // 全量条目（拉取后按数字 id 升序兜底排序）
const totalCount = ref(0)
let loadSeq = 0 // 请求代际：part/gender 切换时丢弃旧响应，防串台

// ── 搜索 / 折叠 / 分页状态 ───────────────────────────────────────────────
const searchText = ref('') // 搜索框原始输入（v-model）
const query = ref('') // 防抖 300ms 后的过滤词（空 = 全量）
let searchTimer = null
const expandedGroups = reactive(new Set()) // 发型折叠组展开状态（存组名，过滤刷新不丢）
const failedIcons = reactive(new Set()) // 缩略图加载失败的条目 id → 回退类目 emoji
const visibleCount = ref(PAGE_SIZE) // 已展示行数（「加载更多」累加）

const activeCategory = computed(() => CATEGORIES.find((c) => c.key === props.part))
const catLabel = computed(() => activeCategory.value?.label ?? '部件')
const catIcon = computed(() => activeCategory.value?.icon ?? '📦')
const isHair = computed(() => props.part === 'hair')

// 数字 id 升序（解析失败排最后，对齐桌面 NumericId）
function byIdAsc(a, b) {
  const na = numericId(a.id)
  const nb = numericId(b.id)
  return (Number.isFinite(na) ? na : Infinity) - (Number.isFinite(nb) ? nb : Infinity)
}

// 本地过滤：中文名（忽略大小写）或 id 字符串包含（规范 3.5）
const filteredItems = computed(() => {
  const q = query.value.trim()
  if (!q) return allItems.value
  const ql = q.toLowerCase()
  return allItems.value.filter(
    (it) => (it.name ?? '').toLowerCase().includes(ql) || (it.id ?? '').includes(q)
  )
})

// 展示行：普通条目行 + 发型多变体组行（组行后紧跟展开的变体行）；普通条目按 id 升序（上游已排）
const displayRows = computed(() => {
  const items = filteredItems.value
  if (!isHair.value) return items.map((it) => ({ type: 'item', item: it }))
  const groupByName = new Map(groupHairItems(items).map((g) => [g.name, g]))
  const rows = []
  const emitted = new Set()
  for (const it of items) {
    // 回退名（发型_ 前缀）不折叠，作为普通行渲染
    const g = isHairFallbackName(it.name ?? '') ? null : groupByName.get(it.name)
    if (g) {
      if (emitted.has(g.name)) continue // 变体已随组行输出过
      emitted.add(g.name)
      rows.push({ type: 'group', group: g })
      if (expandedGroups.has(g.name)) {
        for (const v of g.variants) rows.push({ type: 'item', item: v })
      }
    } else {
      rows.push({ type: 'item', item: it })
    }
  }
  return rows
})

const visibleRows = computed(() => displayRows.value.slice(0, visibleCount.value))
const hasMore = computed(() => visibleRows.value.length < displayRows.value.length)

// ── 目录加载 ─────────────────────────────────────────────────────────────
async function loadCatalog(force = false) {
  const cat = activeCategory.value
  if (!cat) return
  // 性别过滤仅 genderFilter 类目（发型/脸型，规范 3.3）：其余类目不传 gender
  const g = cat.genderFilter ? props.gender : undefined
  const key = `${cat.key}|${g ?? ''}`
  if (!force && catalogCache.has(key)) {
    const c = catalogCache.get(key)
    allItems.value = c.items
    totalCount.value = c.total
    loadError.value = ''
    return
  }
  const seq = ++loadSeq
  loading.value = true
  loadError.value = ''
  try {
    const data = await getCatalog(cat.key, g)
    if (seq !== loadSeq) return // 期间已切换类目/性别，丢弃旧响应
    const items = [...(data?.items ?? [])].sort(byIdAsc)
    const entry = { items, total: data?.total ?? items.length }
    catalogCache.set(key, entry)
    allItems.value = items
    totalCount.value = entry.total
  } catch (e) {
    if (seq !== loadSeq) return
    loadError.value = e?.serverError || e?.message || '素材目录加载失败（WZ 未加载？到「设置」页配置后重试）'
    allItems.value = []
    totalCount.value = 0
  } finally {
    if (seq === loadSeq) loading.value = false
  }
}

function resetListState() {
  searchText.value = ''
  query.value = ''
  visibleCount.value = PAGE_SIZE
}

// ── 交互 ─────────────────────────────────────────────────────────────────
function pickItem(item) {
  emit('update:modelValue', item.id)
  emit('update:show', false)
}

function clearSlot() {
  emit('update:modelValue', null)
  emit('update:show', false)
}

function toggleGroup(name) {
  if (expandedGroups.has(name)) expandedGroups.delete(name)
  else expandedGroups.add(name)
}

function loadMore() {
  visibleCount.value += PAGE_SIZE
}

function isSelected(item) {
  return props.modelValue != null && String(props.modelValue) === String(item.id)
}

// 缩略图地址：icon 优先（椅子类目 icon 缺失时用 img）
function thumbSrc(item) {
  return item.icon || item.img || ''
}

function onThumbError(item) {
  failedIcons.add(String(item.id))
}

// ── 侦听 ─────────────────────────────────────────────────────────────────
// 弹窗打开：重置搜索/分页并拉目录（缓存命中不重拉）
watch(
  () => props.show,
  (v) => {
    if (v && props.part) {
      resetListState()
      loadCatalog()
    }
  }
)

// 弹窗开着时切换类目：重置并按新类目拉取
watch(
  () => props.part,
  () => {
    if (props.show && props.part) {
      resetListState()
      loadCatalog()
    }
  }
)

// 性别切换：genderFilter 类目（发型/脸型）重拉列表（缓存 key 含 gender，切换不误用旧数据）
watch(
  () => props.gender,
  () => {
    if (props.show && activeCategory.value?.genderFilter) {
      resetListState()
      loadCatalog()
    }
  }
)

// 搜索防抖 300ms（对齐桌面 timer 方案）
watch(searchText, (v) => {
  clearTimeout(searchTimer)
  searchTimer = setTimeout(() => {
    query.value = (v || '').trim()
  }, 300)
})

// 过滤词变化：分页归位 + 搜索命中的发型组自动展开（组名或任一变体 name/id 命中，对齐桌面 BuildDisplayRows）
watch(query, (q) => {
  visibleCount.value = PAGE_SIZE
  q = (q || '').trim()
  if (!q || !isHair.value) return
  const ql = q.toLowerCase()
  for (const g of groupHairItems(filteredItems.value)) {
    const hit =
      g.name.toLowerCase().includes(ql) ||
      g.variants.some((v) => (v.id ?? '').includes(q) || (v.name ?? '').toLowerCase().includes(ql))
    if (hit) expandedGroups.add(g.name)
  }
})

onBeforeUnmount(() => clearTimeout(searchTimer))
</script>

<template>
  <n-modal
    :show="show"
    preset="card"
    :title="`选择${catLabel}`"
    style="width: 720px; max-width: 94vw"
    :mask-closable="true"
    @update:show="(v) => emit('update:show', v)"
  >
    <!-- 顶部：搜索框（防抖 300ms）+ 已显示/总数提示 -->
    <div class="picker-toolbar">
      <n-input
        v-model:value="searchText"
        clearable
        :placeholder="`搜索${catLabel}中文名或 ID`"
        :disabled="loading || !!loadError"
      >
        <template #prefix>🔍</template>
      </n-input>
      <span class="picker-count">
        {{ loading ? '加载中…' : `已显示 ${visibleRows.length} / ${displayRows.length} 行 · 共 ${totalCount} 件` }}
      </span>
    </div>

    <!-- 列表区（max-height 60vh 滚动；弹窗不可见时整体卸载，缩略图不加载） -->
    <div class="picker-list">
      <div v-if="loading" class="picker-center"><n-spin size="large" /></div>
      <n-result v-else-if="loadError" status="warning" title="素材目录加载失败" :description="loadError">
        <template #footer>
          <n-button size="small" @click="loadCatalog(true)">重试</n-button>
        </template>
      </n-result>
      <n-empty
        v-else-if="!displayRows.length"
        description="无匹配条目（试试清除搜索或切换性别）"
        style="padding: 40px 0"
      />
      <template v-else>
        <template
          v-for="row in visibleRows"
          :key="row.type === 'group' ? `group:${row.group.name}` : `item:${row.item.id}`"
        >
          <!-- 发型多变体组行：组名 + 变体数 + 展开箭头，点击折叠/展开 -->
          <div v-if="row.type === 'group'" class="picker-row group-row" @click="toggleGroup(row.group.name)">
            <span class="group-arrow">{{ expandedGroups.has(row.group.name) ? '▾' : '▸' }}</span>
            <span class="thumb emoji-thumb">{{ catIcon }}</span>
            <div class="row-info">
              <div class="row-name">{{ row.group.name }}</div>
              <div class="row-sub">{{ row.group.variants.length }} 个变体 · 点击{{ expandedGroups.has(row.group.name) ? '折叠' : '展开' }}</div>
            </div>
          </div>
          <!-- 普通条目行：缩略图（懒加载，失败回退类目 emoji）+ 中文名 + id 副文本；当前已选高亮 -->
          <div
            v-else
            class="picker-row item-row"
            :class="{ selected: isSelected(row.item) }"
            @click="pickItem(row.item)"
          >
            <span
              v-if="!thumbSrc(row.item) || failedIcons.has(String(row.item.id))"
              class="thumb emoji-thumb"
            >{{ catIcon }}</span>
            <img
              v-else
              class="thumb img-thumb"
              loading="lazy"
              :src="thumbSrc(row.item)"
              :alt="row.item.name"
              @error="onThumbError(row.item)"
            />
            <div class="row-info">
              <div class="row-name">{{ row.item.name }}</div>
              <div class="row-sub">{{ row.item.id }}</div>
            </div>
            <span v-if="isSelected(row.item)" class="selected-mark">当前</span>
          </div>
        </template>
        <!-- 加载更多：每次追加 100 行 -->
        <div v-if="hasMore" class="picker-more">
          <n-button size="small" block @click="loadMore">
            加载更多（还有 {{ displayRows.length - visibleRows.length }} 行）
          </n-button>
        </div>
      </template>
    </div>

    <!-- 底部：清空此槽位（标注当前类目中文名）+ 关闭 -->
    <template #action>
      <div class="picker-footer">
        <span class="picker-hint">
          当前类目：{{ catLabel }}{{ activeCategory?.genderFilter ? '（已按性别过滤）' : '' }}
        </span>
        <div class="footer-btns">
          <n-button quaternary type="error" @click="clearSlot">清空{{ catLabel }}（此槽位）</n-button>
          <n-button @click="emit('update:show', false)">关闭</n-button>
        </div>
      </div>
    </template>
  </n-modal>
</template>

<style scoped>
.picker-toolbar {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 10px;
}
.picker-toolbar .n-input {
  flex: 1;
  min-width: 0;
}
.picker-count {
  font-size: 12px;
  opacity: 0.6;
  white-space: nowrap;
}
.picker-list {
  max-height: 60vh;
  overflow: auto;
  border: 1px solid rgba(128, 128, 140, 0.22);
  border-radius: 6px;
  padding: 4px;
}
.picker-center {
  display: flex;
  justify-content: center;
  align-items: center;
  min-height: 200px;
}
.picker-row {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 6px 10px;
  border-radius: 6px;
  cursor: pointer;
}
.picker-row:hover {
  background: rgba(128, 128, 140, 0.12);
}
.item-row.selected {
  background: rgba(94, 156, 255, 0.12);
  box-shadow: inset 0 0 0 1px rgba(94, 156, 255, 0.55);
}
.group-row {
  background: rgba(94, 156, 255, 0.06);
}
.group-arrow {
  width: 14px;
  flex: none;
  text-align: center;
  font-size: 11px;
  opacity: 0.6;
}
.thumb {
  width: 40px;
  height: 40px;
  flex: none;
}
.img-thumb {
  object-fit: contain;
  background: transparent;
  image-rendering: pixelated;
}
.emoji-thumb {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  font-size: 20px;
  border-radius: 4px;
  background: rgba(128, 128, 140, 0.1);
}
.row-info {
  flex: 1;
  min-width: 0;
}
.row-name {
  font-size: 13px;
  font-weight: 500;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.row-sub {
  font-size: 11px;
  opacity: 0.55;
  font-family: 'SF Mono', Menlo, Consolas, monospace;
}
.selected-mark {
  flex: none;
  font-size: 11px;
  color: #5e9cff;
}
.picker-more {
  padding: 8px 4px 4px;
}
.picker-footer {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 12px;
}
.picker-hint {
  font-size: 12px;
  opacity: 0.6;
}
.footer-btns {
  display: flex;
  gap: 8px;
  flex: none;
}
</style>
