<script setup>
/**
 * 曲库管理（E4/E8）：WZ 曲库浏览 + QQ 音源卡片。
 * - NDataTable：source 切换 wz/qq（GET /admin/music/tracks?source=），搜索 + 前端分页；
 * - QQ 卡：cookie 导入（POST .../sources/qq/cookie）、健康 NTag、启停 NSwitch
 *   （PUT .../sources/{name}；wz 为默认源不可停）。
 * Web 只管曲库/cookie/启停 —— 点歌控制权在设备（E8 定稿），本页无播放按钮。
 */
import { computed, h, onBeforeUnmount, onMounted, ref } from 'vue'
import {
  NCard, NDataTable, NInput, NRadioGroup, NRadioButton, NSpace, NTag, NSwitch,
  NButton, NSpin, NResult, NAlert, NTooltip, useMessage,
} from 'naive-ui'
import { musicTracks, listMusicSources, setMusicSourceEnabled, setQqCookie, getSettings } from '../api/client'
import { fmtBytes } from '../utils/format'

const message = useMessage()

// ── 曲库 ────────────────────────────────────────────────────────────────
const source = ref('wz')
const tracks = ref([])
const tracksLoading = ref(false)
const tracksError = ref('')
const trackCount = ref(0)
const query = ref('')

async function loadTracks() {
  tracksLoading.value = true
  tracksError.value = ''
  tracks.value = []
  try {
    const data = await musicTracks(source.value)
    tracks.value = data?.tracks ?? []
    trackCount.value = data?.count ?? tracks.value.length
  } catch (e) {
    tracksError.value = e?.serverError || '曲库加载失败'
  } finally {
    tracksLoading.value = false
  }
}

const filteredTracks = computed(() => {
  const q = query.value.trim().toLowerCase()
  if (!q) return tracks.value
  return tracks.value.filter((t) => (t.title || '').toLowerCase().includes(q) || (t.id || '').toLowerCase().includes(q))
})

const columns = [
  { title: '曲名', key: 'title', ellipsis: { tooltip: true }, render: (row) => row.title || '（未命名）' },
  { title: '分类', key: 'category', width: 140, render: (row) => row.category || '—' },
  { title: '曲目 ID', key: 'id', ellipsis: { tooltip: true } },
  { title: '大小', key: 'bytes', width: 110, render: (row) => h('span', fmtBytes(row.bytes)) },
]

function onSourceChange(v) {
  source.value = v
  query.value = ''
  loadTracks()
}

// ── 音源健康 / QQ 启停 / cookie ─────────────────────────────────────────
const sources = ref([])
const sourcesError = ref('')
const toggling = ref(false)
const cookieText = ref('')
const cookieSaving = ref(false)
const cookieHint = ref('')

const qq = computed(() => sources.value.find((s) => s.name === 'qq') ?? null)
const wzSrc = computed(() => sources.value.find((s) => s.name === 'wz') ?? null)

const STATE_TAG = {
  ok: { type: 'success', label: '正常' },
  degraded: { type: 'warning', label: '降级' },
  down: { type: 'error', label: '不可用' },
  disabled: { type: 'default', label: '已停用' },
}
function stateTag(s) {
  return STATE_TAG[s?.toLowerCase()] ?? { type: 'default', label: s || '未知' }
}

async function loadSources() {
  sourcesError.value = ''
  try {
    const data = await listMusicSources()
    sources.value = data?.sources ?? []
  } catch (e) {
    sourcesError.value = e?.serverError || '音源状态加载失败'
  }
}

async function toggleQq(enabled) {
  toggling.value = true
  try {
    const r = await setMusicSourceEnabled('qq', enabled)
    message.success(enabled ? 'QQ 音源已启用' : 'QQ 音源已停用')
    await loadSources()
    if (r?.health?.detail) cookieHint.value = r.health.detail
  } catch (e) {
    message.error(e?.serverError || '启停失败')
  } finally {
    toggling.value = false
  }
}

async function saveCookie() {
  cookieSaving.value = true
  try {
    const r = await setQqCookie(cookieText.value.trim())
    message.success(r?.imported ? 'cookie 已保存' : 'cookie 已清空')
    cookieHint.value = r?.note || r?.health?.detail || ''
    cookieText.value = ''
    loadSources()
  } catch (e) {
    message.error(e?.serverError || 'cookie 保存失败')
  } finally {
    cookieSaving.value = false
  }
}

// 设置页之外顺带取一次 cookie 是否已配置（只在本地提示「已导入」，不回显明文）
const cookieConfigured = ref(false)
async function probeCookie() {
  try {
    const s = await getSettings()
    cookieConfigured.value = !!(s?.config?.qqMusic?.cookie)
  } catch { /* 忽略：仅提示用 */ }
}

let refreshTimer = null
onMounted(() => {
  loadSources()
  probeCookie()
  loadTracks()
  refreshTimer = setInterval(loadSources, 30000) // 健康态 30s 静默刷新
})
onBeforeUnmount(() => clearInterval(refreshTimer))
</script>

<template>
  <div>
    <n-alert v-if="sourcesError" type="warning" closable class="mb12">{{ sourcesError }}</n-alert>

    <n-card size="small" class="mb12" title="QQ 音源">
      <template #header-extra>
        <n-space align="center">
          <n-tooltip trigger="hover">
            <template #trigger>
              <n-tag size="small" :bordered="false" :type="stateTag(qq?.health?.state).type">
                健康：{{ stateTag(qq?.health?.state).label }}
              </n-tag>
            </template>
            {{ qq?.health?.detail || '健康详情未知（源未注册）' }}
          </n-tooltip>
          <n-switch
            :value="!!qq?.enabled"
            :loading="toggling"
            :disabled="!qq"
            @update:value="toggleQq"
          >
            <template #checked>启用</template>
            <template #unchecked>停用</template>
          </n-switch>
        </n-space>
      </template>
      <n-space vertical size="small">
        <n-input
          v-model:value="cookieText"
          type="textarea"
          placeholder="粘贴网页版 QQ 音乐 cookie（uin / qm_keyst 等整串）；留空提交 = 清空"
          :rows="3"
          :disabled="cookieSaving"
        />
        <n-space align="center" justify="space-between" style="width: 100%">
          <span class="hint">
            {{ cookieConfigured ? '服务端已存有 cookie（此处不回显明文），重新粘贴可覆盖' : '尚未导入 cookie' }}
            <template v-if="cookieHint"> · {{ cookieHint }}</template>
          </span>
          <n-button size="small" type="primary" :loading="cookieSaving" @click="saveCookie">保存 cookie</n-button>
        </n-space>
      </n-space>
    </n-card>

    <n-card size="small">
      <template #header>
        <n-space align="center">
          <n-radio-group :value="source" size="small" @update:value="onSourceChange">
            <n-radio-button value="wz">WZ 曲库</n-radio-button>
            <n-radio-button value="qq" :disabled="qq ? !qq.enabled : true">QQ 音乐</n-radio-button>
          </n-radio-group>
          <n-tag size="small" :bordered="false" :type="stateTag(source === 'wz' ? wzSrc?.health?.state : qq?.health?.state).type">
            {{ stateTag(source === 'wz' ? wzSrc?.health?.state : qq?.health?.state).label }}
          </n-tag>
        </n-space>
      </template>
      <template #header-extra>
        <n-input
          v-model:value="query"
          size="small"
          clearable
          placeholder="按曲名 / ID 过滤（前端）"
          style="width: 220px"
        />
      </template>

      <n-spin v-if="tracksLoading" class="block-center" />
      <n-result
        v-else-if="tracksError"
        status="warning"
        title="曲库加载失败"
        :description="tracksError"
      >
        <template #footer>
          <n-button size="small" @click="loadTracks">重试</n-button>
        </template>
      </n-result>
      <n-data-table
        v-else
        size="small"
        :columns="columns"
        :data="filteredTracks"
        :bordered="false"
        :row-key="(row) => row.id"
        :pagination="{ pageSize: 20, showSizePicker: true, pageSizes: [10, 20, 50] }"
        :max-height="480"
      />
      <div class="hint" style="margin-top: 8px">
        共 {{ trackCount }} 首{{ query ? ` · 过滤后 ${filteredTracks.length} 首` : '' }}；播放控制在设备端（E8）
      </div>
    </n-card>
  </div>
</template>

<style scoped>
.mb12 { margin-bottom: 12px; }
.hint { font-size: 12px; opacity: 0.6; }
.block-center { display: flex; justify-content: center; padding: 40px 0; }
</style>
