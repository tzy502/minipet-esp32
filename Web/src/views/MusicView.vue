<script setup>
/**
 * 曲库管理（E4/E8）：WZ 曲库浏览 + QQ 音源卡片 + 设备播放控制卡片。
 * - NDataTable：source 切换 wz/qq（GET /admin/music/tracks?source=），搜索 + 前端分页；
 * - QQ 卡（E4 告警 + E4 关键词入口）：
 *     · cookie 导入（POST .../sources/qq/cookie）、健康 NTag、启停 NSwitch
 *       （PUT .../sources/{name}；wz 为默认源不可停）；
 *     · cookie 有效期告警：消费服务端已算好的 `cookieStale` / `cookieAgeDays` / `cookieSavedAtUtc`
 *       （>7 天 → 黄色 NAlert「已保存 N 天…可能已失效」；未过期 → 「已保存 N 天」）；
 *     · 网关降级原因：`gateway` 对象（QqGatewayStatus）+ `health.detail`（服务端聚合原因）——
 *       网关本体不在仓库/镜像内，未配好时如实标注「需服务端配好网关」；
 *     · 搜索关键词 qqMusic.searchKeyword（QQ 无全库概念，空 = 曲库恒空）：读写走**既有**
 *       settings 通道（stores/settings.js load/save → PUT /admin/settings 全量同构回传）。
 * - 设备播放控制卡（本页顶部）：E8 定稿「控制入口在设备触摸屏，Web 只管曲库/歌单/cookie」，
 *   本卡是用户实测反馈「服务器页面也没有播放 bgm 的按钮」后的**超出需求的附加能力**，
 *   因此按「只读展示 + 探测到才启用的可选远程下发」做：
 *     · 只读：设备在线态 / BGM 偏好（source·volume）/ 设备事件日志（GET /admin/logs/{id}）；
 *     · 下发：POST /admin/devices/{id}/command {type:"bgm", value:"play|pause|resume|stop|next|prev"}
 *       与 {type:"bgm", value:"vol", n} —— 开卡先探测（哨兵值 __probe__ 对设备零副作用），
 *       端点未放行 bgm → 按钮全禁用 + 卡内贴出接口需求（与 DeviceDetailView 表情卡同一套模式）。
 *     · 音量另有已在位通道：PUT /admin/devices/{id} {bgm:{volume}}（服务端偏好，非即时下发）。
 */
import { computed, h, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import {
  NCard, NDataTable, NInput, NInputNumber, NRadioGroup, NRadioButton, NSpace, NTag, NSwitch,
  NButton, NSpin, NResult, NAlert, NTooltip, NSelect, NSlider, NDivider, useMessage,
} from 'naive-ui'
import {
  musicTracks, listMusicSources, setMusicSourceEnabled, setQqCookie,
  listDevices, getDevice, getDeviceLogs, sendBgmCommand, probeBgmCommand, setDeviceBgmPrefs,
  BGM_COMMAND, errText, musicStateKey, qqCookieInfo, qqGatewayInfo,
} from '../api/client'
import { useSettingsStore } from '../stores/settings'
import { fmtBytes } from '../utils/format'

const message = useMessage()
const settingsStore = useSettingsStore()

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
/**
 * 健康标签。⚠ 服务端按**数字**序列化 MusicSourceState（实测 wz=0 / qq 停用=3）——
 * 旧实现直接 `s.toLowerCase()` 会对 number 抛 TypeError（整页渲染失败）；这里先过
 * client.js 的 musicStateKey() 归一（数字与字符串枚举两种形态都兼容）。
 */
function stateTag(s) {
  const key = musicStateKey(s)
  return STATE_TAG[key] ?? { type: 'default', label: s == null ? '未知' : String(s) }
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
    // 服务端导入响应里就带新鲜度（cookieStale/cookieAgeDays）——直接用于即时提示
    cookieHint.value = r?.imported
      ? `cookieSavedAtUtc=${r?.cookieSavedAtUtc ?? '—'} · ${r?.cookieStale ? '已过期（>7 天）' : `已保存 ${r?.cookieAgeDays ?? 0} 天`}`
      : (r?.note || 'cookie 已清空')
    cookieText.value = ''
    loadSources()
    loadQqConfig() // cookie 状态变了，「已导入 N 天」提示同步刷新
  } catch (e) {
    message.error(e?.serverError || 'cookie 保存失败')
  } finally {
    cookieSaving.value = false
  }
}

// 设置页之外顺带取一次 QQ 相关配置（cookie 是否已配置 + 曲库搜索关键词），不回显 cookie 明文
const cookieConfigured = ref(false)
const keyword = ref('') // 表单值（qqMusic.searchKeyword）
const savedKeyword = ref('') // 服务端当前值（用于「未保存」提示）
const keywordSaving = ref(false)
const configNote = ref('')

/** 复用既有 settings 通道（stores/settings.js → GET /admin/settings），不新造读取方式。 */
async function loadQqConfig() {
  try {
    await settingsStore.load()
    const c = settingsStore.config
    cookieConfigured.value = !!c?.qqMusic?.cookie
    savedKeyword.value = c?.qqMusic?.searchKeyword ?? ''
    keyword.value = savedKeyword.value
    configNote.value = ''
  } catch (e) {
    configNote.value = errText(e, 'QQ 配置读取失败')
  }
}

/**
 * 保存搜索关键词 → 复用既有 settings 写通道（PUT /admin/settings 全量同构回传）。
 * 必须**先取最新全量配置**再改一个字段：服务端 `ConfigService.Replace` 是整段替换
 * （wz/qqMusic/bgm/device/clock/speech 全部覆盖），只发 {qqMusic:{...}} 会把 WZ 路径等清空 → 400。
 */
async function saveKeyword() {
  const next = keyword.value.trim()
  keywordSaving.value = true
  try {
    await settingsStore.load() // 取最新快照，避免覆盖其它会话/页面的改动
    const cfg = JSON.parse(JSON.stringify(settingsStore.config ?? {}))
    cfg.qqMusic = { ...(cfg.qqMusic ?? {}), searchKeyword: next }
    const r = await settingsStore.save(cfg)
    savedKeyword.value = r?.config?.qqMusic?.searchKeyword ?? next
    keyword.value = savedKeyword.value
    configNote.value = ''
    message.success(
      savedKeyword.value
        ? `搜索关键词已保存：${savedKeyword.value}`
        : '搜索关键词已清空（QQ 曲库将为空）'
    )
    if (source.value === 'qq') loadTracks() // 关键词变了，曲库立即可见地重拉
  } catch (e) {
    const errs = e?.response?.data?.errors
    const text = errs?.length ? errs.join('；') : errText(e, '保存失败')
    configNote.value = text
    message.error(text)
  } finally {
    keywordSaving.value = false
  }
}
const keywordDirty = computed(() => keyword.value.trim() !== savedKeyword.value)

// ── T1：cookie 有效期 + 网关降级原因（服务端已算好，前端只消费）─────────────
/** cookie 新鲜度：cookieStale / cookieAgeDays / cookieSavedAtUtc（null 字段被服务端省略）。 */
const cookieInfo = computed(() => qqCookieInfo(qq.value))
/** 网关状态：gateway 是对象（QqGatewayStatus），非 "Ok/Degraded" 字符串。 */
const gatewayInfo = computed(() => qqGatewayInfo(qq.value))
/** 服务端聚合原因（health.detail）：cookie 过期 / 网关未就绪 / 未启用 都会在这里给全文。 */
const healthDetail = computed(() => qq.value?.health?.detail || '')
/** 整体非 Ok（state 0=Ok）：用于决定是否亮降级说明。 */
const stateKey = computed(() => musicStateKey(qq.value?.health?.state))
const degraded = computed(() => !!qq.value && qq.value.health?.state != null && stateKey.value !== 'ok')

// ── 设备播放控制（E8 附加：只读展示 + 探测到才启用的远程下发）─────────────
const DEVICE_KEY = 'minipet.music.deviceId'
const devices = ref([])
const deviceId = ref('')
const deviceInfo = ref(null) // GET /admin/devices/{id} 的 device（含 bgm / online）
const devLogs = ref([])
const devLoadError = ref('')

const bgmSupport = ref('unknown') // unknown | ok | missing
const bgmProbeNote = ref('')
const bgmProbeBusy = ref(false)
const bgmBusy = ref('') // 正在下发的 bgm 值 / 'vol' / 'pref'
const volume = ref(50)
const lastResult = ref(null) // { type: 'success'|'error', text }

const deviceOptions = computed(() =>
  devices.value.map((d) => ({
    label: `${d.name || '未命名'} [${d.deviceId}]${d.online ? ' · 在线' : ' · 离线'}`,
    value: d.deviceId,
  })),
)
const selectedDevice = computed(() => devices.value.find((d) => d.deviceId === deviceId.value) ?? null)
const deviceOnline = computed(() => !!selectedDevice.value?.online)
const bgmDisabled = computed(() => bgmSupport.value === 'missing' || !deviceId.value)
const bgmPref = computed(() => deviceInfo.value?.bgm ?? null)

// 只有「放行 / 未放行 / 未判定」三态：bgm 的哨兵探针被 value 白名单拒绝（400）属
// 正确实现的预期响应 → 归「端点在位」（原文记在 bgmProbeNote 里），不设 mismatch 态。
const SUPPORT_TAG = {
  ok: { type: 'success', label: '端点在位' },
  missing: { type: 'error', label: '端点不支持 bgm' },
  unknown: { type: 'default', label: '未探测到' },
}
const supportTag = computed(() => SUPPORT_TAG[bgmSupport.value] ?? SUPPORT_TAG.unknown)

/** 命令端点路径（提示文案与真实请求同源）。 */
const cmdPath = computed(() => `/api/admin/devices/${deviceId.value || '{id}'}/command`)

async function loadDevices() {
  try {
    const data = await listDevices()
    devices.value = data?.devices ?? []
    const known = devices.value.some((d) => d.deviceId === deviceId.value)
    if (!known) {
      let stored = ''
      try { stored = localStorage.getItem(DEVICE_KEY) || '' } catch { /* 无 localStorage 权限时忽略 */ }
      const pick =
        devices.value.find((d) => d.deviceId === stored) ||
        devices.value.find((d) => d.online) ||
        devices.value[0]
      deviceId.value = pick?.deviceId ?? ''
    }
  } catch (e) {
    devLoadError.value = errText(e, '设备列表加载失败')
  }
}

async function loadDeviceInfo() {
  if (!deviceId.value) return
  devLoadError.value = ''
  try {
    const data = await getDevice(deviceId.value)
    deviceInfo.value = data?.device ?? null
    const v = deviceInfo.value?.bgm?.volume
    if (v != null) volume.value = v
  } catch (e) {
    deviceInfo.value = null
    devLoadError.value = errText(e, '设备详情加载失败')
  }
  loadDeviceLogs()
}

async function loadDeviceLogs() {
  if (!deviceId.value) return
  try {
    const data = await getDeviceLogs(deviceId.value)
    devLogs.value = (data?.lines ?? []).slice(0, 5)
  } catch { devLogs.value = [] }
}

/** 开卡探测（哨兵值 __probe__：固件不认它 → 设备零副作用，见 client.js 注释）。 */
async function probeBgm() {
  if (!deviceId.value) return
  bgmProbeBusy.value = true
  bgmSupport.value = 'unknown'
  bgmProbeNote.value = ''
  try {
    const r = await probeBgmCommand(deviceId.value)
    if (r.supported === false) {
      bgmSupport.value = 'missing'
      bgmProbeNote.value = `HTTP ${r.status}${r.error ? `：${r.error}` : ''}`
    } else if (r.supported === true) {
      bgmSupport.value = 'ok'
      // 哨兵值被 value 白名单拒（400）属正确实现的预期响应：记原文，不算失败
      if (r.probeRejected) bgmProbeNote.value = `哨兵值被拒（预期）：HTTP ${r.status}：${r.error || ''}`
    } else {
      bgmSupport.value = 'unknown'
      bgmProbeNote.value = r.error || '无法判定（网络不可达？）'
    }
  } finally {
    bgmProbeBusy.value = false
  }
}

function onDeviceChange(v) {
  deviceId.value = v
  try { localStorage.setItem(DEVICE_KEY, v) } catch { /* 忽略 */ }
}

/** 真发一条 bgm 指令；按真实响应落状态（400「type 非法：bgm」→ 判定端点未放行）。 */
async function sendBgm(value, label, opts = {}) {
  if (!deviceId.value) return
  bgmBusy.value = opts.busyKey || value
  lastResult.value = null
  const bodyText = opts.n != null
    ? `{"type":"bgm","value":"${value}","n":${opts.n}}`
    : `{"type":"bgm","value":"${value}"}`
  try {
    const r = await sendBgmCommand(deviceId.value, value, opts)
    bgmSupport.value = 'ok'
    const http = r?.seq != null ? 202 : 200
    lastResult.value = {
      type: 'success',
      text: `HTTP ${http} · POST ${cmdPath.value} body ${bodyText} → ${r?.seq != null ? `seq ${r.seq}（已入队，设备下次 poll 取走）` : JSON.stringify(r)}`,
    }
    message.success(`${label}已下发${r?.seq != null ? `（seq ${r.seq}）` : ''}`)
    loadDeviceLogs()
  } catch (e) {
    const status = e?.response?.status
    const text = errText(e)
    if (status === 404 || status === 405 || status === 501) bgmSupport.value = 'missing'
    else if (status === 400 && /bgm/i.test(text) && /type[\s=:：]*[^，,。;\s]{0,12}?(非法|不支持|未知|无效)/i.test(text)) bgmSupport.value = 'missing'
    else if (status === 400) bgmSupport.value = 'ok' // 端点认 bgm，只是拒了这次入参（原文见下）
    bgmProbeNote.value = `HTTP ${status ?? '—'}：${text}`
    lastResult.value = { type: 'error', text: `HTTP ${status ?? '—'} · POST ${cmdPath.value} body ${bodyText} → ${text}` }
    message.error(text)
  } finally {
    bgmBusy.value = ''
  }
}

const playBgm = (v, label) => sendBgm(v, label)

/** 音量下发（需服务端放行 + 固件支持数值型 payload；202 只代表入队）。 */
function sendVolume() {
  return sendBgm(BGM_COMMAND.VOLUME, '音量', { n: Number(volume.value), busyKey: 'vol' })
}

/** 音量写入服务端设备偏好（PUT /admin/devices/{id}，服务端已支持；非即时下发）。 */
async function saveVolumePref() {
  bgmBusy.value = 'pref'
  lastResult.value = null
  const bodyText = `{"bgm":{"volume":${Number(volume.value)}}}`
  try {
    await setDeviceBgmPrefs(deviceId.value, { volume: Number(volume.value) })
    lastResult.value = { type: 'success', text: `HTTP 200 · PUT /api/admin/devices/${deviceId.value} body ${bodyText} → 设备偏好已保存` }
    message.success('音量已写入设备偏好')
    loadDeviceInfo()
  } catch (e) {
    const text = errText(e)
    lastResult.value = { type: 'error', text: `HTTP ${e?.response?.status ?? '—'} · PUT /api/admin/devices/${deviceId.value} body ${bodyText} → ${text}` }
    message.error(text)
  } finally {
    bgmBusy.value = ''
  }
}

watch(deviceId, () => {
  deviceInfo.value = null
  devLogs.value = []
  loadDeviceInfo()
  probeBgm()
})

let refreshTimer = null
onMounted(async () => {
  loadSources()
  loadQqConfig()
  loadTracks()
  // deviceId 由 loadDevices 选定 → watch(deviceId) 负责首次 loadDeviceInfo + probeBgm
  await loadDevices()
  refreshTimer = setInterval(() => {
    loadSources() // 健康态 30s 静默刷新
    loadDevices().then(() => loadDeviceInfo())
  }, 30000)
})
onBeforeUnmount(() => clearInterval(refreshTimer))
</script>

<template>
  <div>
    <n-alert v-if="sourcesError" type="warning" closable class="mb12">{{ sourcesError }}</n-alert>

    <!-- 设备播放控制（E8 附加：控制入口本在设备触摸屏，这里是超出需求的 Web 远程通道） -->
    <n-card size="small" class="mb12" title="设备播放控制（E8 附加：只读展示 + 可选远程下发）">
      <template #header-extra>
        <n-space align="center">
          <n-select
            :value="deviceId"
            :options="deviceOptions"
            size="small"
            style="width: 240px"
            :disabled="!devices.length"
            placeholder="选择设备"
            @update:value="onDeviceChange"
          />
          <n-tag size="small" :bordered="false" :type="supportTag.type">{{ supportTag.label }}</n-tag>
          <n-button size="tiny" secondary :loading="bgmProbeBusy" :disabled="!deviceId" @click="probeBgm">重新检测</n-button>
        </n-space>
      </template>

      <n-space vertical :size="10">
        <n-alert v-if="devLoadError" type="warning" :show-icon="false" size="small">{{ devLoadError }}</n-alert>

        <!-- 只读展示：设备与 BGM 偏好来自 GET /admin/devices/{id}，事件来自 GET /admin/logs/{id} -->
        <n-space align="center" :size="8" wrap class="ro-line">
          <n-tag size="small" :bordered="false" :type="deviceOnline ? 'success' : 'default'">
            {{ deviceOnline ? '设备在线' : '设备离线' }}
          </n-tag>
          <span class="hint">
            BGM 偏好：源 <b>{{ bgmPref?.source ?? '—' }}</b> · 音量 <b>{{ bgmPref?.volume ?? '—' }}</b>
            （GET /api/admin/devices/{{ deviceId || '{id}' }} · 只读，设备端现场控制会回写此处）
          </span>
        </n-space>

        <n-space v-if="devLogs.length" vertical size="2">
          <span class="hint">设备事件（GET /api/admin/logs/{{ deviceId }} 最近 5 条）：</span>
          <div v-for="(l, i) in devLogs" :key="i" class="log-line">{{ l }}</div>
        </n-space>

        <n-divider style="margin: 2px 0" />

        <n-space align="center" :size="8" wrap>
          <n-button size="small" type="primary" class="bgm-btn" data-bgm="play" :disabled="bgmDisabled" :loading="bgmBusy === 'play'" @click="playBgm(BGM_COMMAND.PLAY, '播放')">▶ 播放</n-button>
          <n-button size="small" class="bgm-btn" data-bgm="pause" :disabled="bgmDisabled" :loading="bgmBusy === 'pause'" @click="playBgm(BGM_COMMAND.PAUSE, '暂停')">⏸ 暂停</n-button>
          <n-button size="small" class="bgm-btn" data-bgm="resume" :disabled="bgmDisabled" :loading="bgmBusy === 'resume'" @click="playBgm(BGM_COMMAND.RESUME, '续播')">⏯ 续播</n-button>
          <n-button size="small" class="bgm-btn" data-bgm="stop" :disabled="bgmDisabled" :loading="bgmBusy === 'stop'" @click="playBgm(BGM_COMMAND.STOP, '停止')">⏹ 停止</n-button>
          <n-divider vertical />
          <n-button size="small" class="bgm-btn" data-bgm="prev" :disabled="bgmDisabled" :loading="bgmBusy === 'prev'" @click="playBgm(BGM_COMMAND.PREV, '上一首')">⏮ 上一首</n-button>
          <n-button size="small" class="bgm-btn" data-bgm="next" :disabled="bgmDisabled" :loading="bgmBusy === 'next'" @click="playBgm(BGM_COMMAND.NEXT, '下一首')">⏭ 下一首</n-button>
        </n-space>

        <n-space align="center" :size="8" wrap>
          <span class="vol-label">音量</span>
          <n-slider v-model:value="volume" :min="0" :max="100" :step="1" style="width: 220px" />
          <n-input-number v-model:value="volume" size="small" :min="0" :max="100" style="width: 92px" />
          <n-button size="small" class="bgm-btn" data-bgm="vol" :disabled="bgmDisabled" :loading="bgmBusy === 'vol'" @click="sendVolume">下发音量</n-button>
          <n-button size="small" secondary class="bgm-btn" data-bgm="pref" :disabled="!deviceId" :loading="bgmBusy === 'pref'" @click="saveVolumePref">存为设备偏好</n-button>
        </n-space>

        <n-alert
          v-if="lastResult"
          :type="lastResult.type === 'success' ? 'success' : 'error'"
          :show-icon="false"
          size="small"
        >{{ lastResult.text }}</n-alert>

        <n-alert v-if="bgmSupport === 'missing'" type="warning" :show-icon="false" size="small">
          <b>服务端未放行 bgm 指令，播放/暂停/切歌按钮已禁用。</b>
          （探测结果：{{ bgmProbeNote || 'HTTP 400' }}）需要服务端在设备指令端点白名单里加 <code>bgm</code>：
          <div class="req">
            <div><code>POST {{ cmdPath }}</code></div>
            <div>播放控制 body <code>{ "type": "bgm", "value": "play" | "pause" | "resume" | "stop" | "next" | "prev" }</code></div>
            <div>音量 body <code>{ "type": "bgm", "value": "vol", "n": 0-100 }</code></div>
            <div>期望 <code>202 { "ok": true, "seq": &lt;n&gt;, "type": "bgm", "value": "..." }</code></div>
            <div>
              实现要点：入队 payload 必须是<b>裸 JSON 字符串</b>（<code>queue.Enqueue(id, "bgm", "play")</code>）——
              固件 <code>Firmware/main/net/poller.c:208-217</code> 的 bgm 分支要求 <code>payload</code> 是字符串；
              数值型音量 <code>{"n":50}</code> 会被固件静默忽略（<code>poller.c:171-173</code> 折算成 vitem=NULL），
              需固件同补 <code>pn</code> 分支（<code>MP_AUDIO_VOL</code>）。
            </div>
            <div>完整清单：<code>Web/docs/interfaces-needed-from-server.md</code> §T6</div>
          </div>
        </n-alert>
        <n-alert v-else-if="bgmSupport === 'unknown'" type="info" :show-icon="false" size="small">
          未能判定端点是否放行 bgm（{{ bgmProbeNote || '网络不可达' }}）：按钮未禁用，点击后按真实响应提示。
        </n-alert>
        <div v-else-if="bgmProbeNote" class="hint">探测：{{ bgmProbeNote }}</div>

        <div class="hint">
          E8 定稿：控制入口在<b>设备触摸屏</b>（菜单内 BGM 入口 → 半屏控制条），Web 的职责是曲库/歌单/cookie；
          本卡为超出需求的<b>只读展示 + 可选远程下发</b>。设置音量后不会自动同步设备实际音量
          （设备端现场控制会回传 <code>POST /api/device/bgm/cmd</code>）；「存为设备偏好」只改服务端偏好
          （设备下次 hello 才可能消费，固件当前只读阈值四件套）。
        </div>
      </n-space>
    </n-card>

    <n-card size="small" class="mb12" title="QQ 音源（E4：cookie 有效期告警 + 曲库关键词 + 网关状态）">
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
          <n-tag v-if="gatewayInfo.present" size="small" :bordered="false" :type="gatewayInfo.level === 'ok' ? 'success' : gatewayInfo.level === 'off' ? 'default' : 'warning'">
            网关：{{ gatewayInfo.label }}
          </n-tag>
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
        <!-- T1：cookie 有效期（服务端已算好 cookieStale / cookieAgeDays / cookieSavedAtUtc） -->
        <n-alert v-if="cookieInfo.stale" type="warning" :show-icon="false" size="small">
          <b v-if="!cookieInfo.unknownAge">cookie 已保存 {{ cookieInfo.ageDays }} 天（&gt;7 天）可能已失效，请重新导入</b>
          <b v-else>cookie 无导入时间（旧配置）——有效期未知，可能已失效，请重新导入</b>
          <div class="hint" style="margin-top: 4px">
            服务端判定：<code>cookieStale=true</code><template v-if="cookieInfo.ageDays != null"> · <code>cookieAgeDays={{ cookieInfo.ageDays }}</code></template>
            <template v-if="cookieInfo.savedAtUtc"> · 导入于 {{ cookieInfo.savedAtUtc }}</template>
            （阈值 7 天，<code>QqMusicSource.CookieFreshness</code>）
          </div>
        </n-alert>
        <n-alert v-else-if="cookieInfo.ageDays != null" type="default" :show-icon="false" size="small">
          cookie 已保存 {{ cookieInfo.ageDays }} 天（未超 7 天阈值）
          <template v-if="cookieInfo.savedAtUtc"> · 导入于 {{ cookieInfo.savedAtUtc }}</template>
        </n-alert>

        <!-- T1：整体非 Ok（含网关未就绪）→ 展示服务端聚合的降级原因 -->
        <n-alert v-if="degraded" :type="stateKey === 'disabled' ? 'default' : 'warning'" :show-icon="false" size="small">
          <b>QQ 音源当前{{ stateTag(qq?.health?.state).label }}。</b>{{ healthDetail }}
          <div v-if="gatewayInfo.present" class="hint" style="margin-top: 4px">
            网关细节：<code>enabled={{ qq?.gateway?.enabled }}</code> · <code>scriptFound={{ qq?.gateway?.scriptFound }}</code>
            · <code>running={{ qq?.gateway?.running }}</code> · <code>healthy={{ qq?.gateway?.healthy }}</code>
            · <code>cookieLoaded={{ qq?.gateway?.cookieLoaded }}</code>
            <template v-if="qq?.gateway?.scriptPath"> · <code>scriptPath={{ qq?.gateway?.scriptPath }}</code></template>
            <template v-if="gatewayInfo.reason"> · {{ gatewayInfo.reason }}</template>
          </div>
          <div v-if="gatewayInfo.present && gatewayInfo.level !== 'ok'" class="hint" style="margin-top: 4px">
            ⚠ QQ 网关本体（Rain120/qq-music-api + 适配层）<b>不在本仓库、镜像也未内置</b>：
            需服务端自行部署后放到 <code>data/qq-gateway/index.js</code>，或配 <code>QqMusic.GatewayScript</code> /
            环境变量 <code>MINIPET_QQ_GATEWAY</code>；未配好前该源恒为降级（不造假实现）。
          </div>
        </n-alert>

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

        <n-divider style="margin: 2px 0" />

        <!-- T2：QQ 曲库关键词（QQ 无全库概念；空 = /music/tracks?source=qq 恒空） -->
        <n-space align="center" :size="8" wrap style="width: 100%">
          <span style="font-size: 13px">曲库搜索关键词</span>
          <n-input
            v-model:value="keyword"
            size="small"
            clearable
            style="width: 260px"
            placeholder="如：久石让 / 天空之城（空 = 曲库为空）"
            :disabled="keywordSaving"
            @keyup.enter="saveKeyword"
          />
          <n-button size="small" type="primary" secondary :loading="keywordSaving" :disabled="!keywordDirty" @click="saveKeyword">
            保存关键词
          </n-button>
          <n-tag v-if="keywordDirty" size="small" :bordered="false" type="warning">未保存</n-tag>
          <span class="hint">
            写入 <code>qqMusic.searchKeyword</code>（走既有 <code>PUT /api/admin/settings</code> 全量同构回传，
            复用设置页同一通道）；当前服务端值：<b>{{ savedKeyword || '（空）' }}</b>
          </span>
        </n-space>
        <n-alert v-if="configNote" type="warning" :show-icon="false" size="small">{{ configNote }}</n-alert>
        <div class="hint">
          QQ 无「全库」概念：服务端 <code>QqMusicSource.ListTracksAsync</code> 用该关键词调网关
          <code>/search</code>（取前 30 首）；<b>关键词为空或网关未就绪 → QQ 曲库列表为空</b>（服务端不臆造默认歌单）。
          QQ 网关本体不在本仓库，需服务端按 <code>GET /api/admin/music/sources</code> 的
          <code>gateway</code> / <code>health.detail</code> 提示配好后本页才有曲目。
        </div>
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

      <n-alert v-if="source === 'qq' && !tracksLoading && !tracksError && !filteredTracks.length" type="info" :show-icon="false" size="small" style="margin-bottom: 8px">
        <b>QQ 曲库为空。</b>
        QQ 无「全库」概念，服务端用 <code>qqMusic.searchKeyword</code> 调网关搜索取曲：
        <template v-if="!savedKeyword">当前关键词为<b>空</b> → 请在上方「QQ 音源」卡填写关键词并保存。</template>
        <template v-else>当前关键词「<b>{{ savedKeyword }}</b>」，仍无结果 → 多为网关未就绪（{{ gatewayInfo.present ? gatewayInfo.label : '网关状态未返回' }}<template v-if="healthDetail">；{{ healthDetail }}</template>）。</template>
      </n-alert>

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
        共 {{ trackCount }} 首{{ query ? ` · 过滤后 ${filteredTracks.length} 首` : '' }}；
        播放控制在设备触摸屏（E8），上方控制卡为附加的 Web 远程通道
      </div>
    </n-card>
  </div>
</template>

<style scoped>
.mb12 { margin-bottom: 12px; }
.hint { font-size: 12px; opacity: 0.6; }
.block-center { display: flex; justify-content: center; padding: 40px 0; }
.ro-line { width: 100%; }
.log-line { font-size: 12px; opacity: 0.65; font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
.vol-label { font-size: 12px; opacity: 0.75; }
.req { margin-top: 6px; line-height: 1.8; }
.req code { background: rgba(128, 128, 140, 0.15); padding: 0 3px; border-radius: 3px; }
</style>
