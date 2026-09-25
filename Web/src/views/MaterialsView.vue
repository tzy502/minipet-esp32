<script setup>
/**
 * 素材浏览器（E4）：地图 / 纸娃娃部件 / 怪物NPC 三个 tab。
 * 缩略图走服务端 GET /admin/thumb（64×64，M3 为确定性占位渲染），
 * 懒加载（NImage lazy）+ 收藏星标（localStorage，喂设备选择器「最近+收藏」，E7）。
 * 注：后端按类目列举素材 id 的目录 API 尚未提供，各 tab id 先静态占位。
 */
import { computed, ref } from 'vue'
import {
  NTabs, NTabPane, NImage, NImageGroup, NEmpty, NCheckbox, NCard, NSpace, NTag, NButton, useMessage,
} from 'naive-ui'
import { thumbUrl } from '../api/client'

const TABS = [
  { key: 'map', label: '地图', type: 'map' },
  { key: 'paperdoll', label: '纸娃娃部件', type: 'paperdoll' },
  { key: 'mob', label: '怪物 / NPC', type: 'mob' },
]

// 静态占位 id（目录 API 上线后替换为真实列表）
const PLACEHOLDER_IDS = {
  map: ['200000100', '200000200', '220000100', '220000300', '230000000', '250000100', '260000100', '270000100', '300000010', '310000000'],
  paperdoll: ['30000', '30030', '30120', '20000', '20012', '1040002', '1041044', '1060002', '1302000', '1452008'],
  mob: ['100100', '1210100', '130100', '210100', '2110300', '2220100', '2300100', '3110100', '4230101', '5120003'],
}

// ── 收藏（localStorage 持久化，按 tab 分桶）───────────────────────────────
const FAV_KEY = 'minipet.materials.favorites'
const favorites = ref(loadFavs())

function loadFavs() {
  try {
    return { map: [], paperdoll: [], mob: [], ...JSON.parse(localStorage.getItem(FAV_KEY) || '{}') }
  } catch {
    return { map: [], paperdoll: [], mob: [] }
  }
}
function persistFavs() {
  localStorage.setItem(FAV_KEY, JSON.stringify(favorites.value))
}
function isFav(tab, id) {
  return favorites.value[tab]?.includes(id)
}
function toggleFav(tab, id) {
  const bucket = favorites.value[tab] ?? (favorites.value[tab] = [])
  const i = bucket.indexOf(id)
  if (i >= 0) bucket.splice(i, 1)
  else bucket.push(id)
  persistFavs()
}

// ── 当前 tab 数据（占位数组；只看收藏过滤）───────────────────────────────
const activeTab = ref('map')
const onlyFav = ref(false)
const message = useMessage()

const ids = computed(() => PLACEHOLDER_IDS[activeTab.value] ?? [])
const shownIds = computed(() =>
  onlyFav.value ? ids.value.filter((id) => isFav(activeTab.value, id)) : ids.value
)
const favCount = computed(() => ids.value.filter((id) => isFav(activeTab.value, id)).length)

function thumbType(tabKey) {
  return TABS.find((t) => t.key === tabKey)?.type ?? 'map'
}
function copyId(id) {
  navigator.clipboard?.writeText(id).then(
    () => message.success(`已复制 ${id}`),
    () => message.error('复制失败')
  )
}
</script>

<template>
  <div>
    <n-card size="small" class="mb12">
      <n-space align="center" justify="space-between" style="width: 100%">
        <n-space align="center">
          <n-tag :bordered="false" type="info">服务端渲染缩略图</n-tag>
          <span class="hint">素材 id 为静态占位（后端目录 API 未上线）；缩略图真实调 /admin/thumb</span>
        </n-space>
        <n-checkbox v-model:checked="onlyFav">只看收藏（{{ favCount }}）</n-checkbox>
      </n-space>
    </n-card>

    <n-tabs v-model:value="activeTab" type="line" animated>
      <n-tab-pane v-for="tab in TABS" :key="tab.key" :name="tab.key" :tab="tab.label">
        <n-empty v-if="!shownIds.length" description="该分类暂无收藏项" style="padding: 60px 0" />
        <n-image-group v-else>
          <div class="thumb-grid">
            <div v-for="id in shownIds" :key="id" class="thumb-cell">
              <n-image
                lazy
                :src="thumbUrl(thumbType(tab.key), id)"
                :alt="`${tab.label} ${id}`"
                width="96"
                height="96"
                object-fit="cover"
                style="border-radius: 6px"
              />
              <div class="thumb-id" title="点击复制 id" @click="copyId(id)">{{ id }}</div>
              <n-button
                class="star"
                size="tiny"
                circle
                :type="isFav(tab.key, id) ? 'warning' : 'default'"
                :tertiary="!isFav(tab.key, id)"
                @click="toggleFav(tab.key, id)"
              >
                {{ isFav(tab.key, id) ? '★' : '☆' }}
              </n-button>
            </div>
          </div>
        </n-image-group>
      </n-tab-pane>
    </n-tabs>
  </div>
</template>

<style scoped>
.mb12 { margin-bottom: 12px; }
.hint { font-size: 12px; opacity: 0.6; }
.thumb-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(112px, 1fr));
  gap: 14px;
}
.thumb-cell { position: relative; text-align: center; }
.thumb-id {
  font-size: 11px;
  opacity: 0.7;
  margin-top: 4px;
  cursor: copy;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.star { position: absolute; top: 2px; right: 2px; }
</style>
