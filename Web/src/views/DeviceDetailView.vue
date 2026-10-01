<script setup>
/**
 * 单设备详情（E4/E13）：信息卡 + 换宠换装（AppearancePicker 复用）+ 阈值覆盖 + OTA 触发
 * + 素材推送（T3/E7）+ 动作/表情/气泡调试（T2/E4，25 表情手动指定）。
 * 换装 = PUT devices/{id} 显式传 petConfig（按设备隔离，manifest rev+1）；
 * 阈值覆盖 = 传 thresholds 对象；显式传 petConfig:null = 清空回默认宠物。
 * 从预设推送 = 纸娃娃编辑器「已存预设」（服务端 GET /admin/presets，与 PaperdollView 同一份）
 *   的 appearance 经 appearanceToDraft→draftToAppearance 归一化后走同一 PUT 链路（同一端点/同一 payload）。
 * T2 指令端点服务端**已上线**（实测 202 {"ok":true,"seq":47,...}）：仍保留开卡探测，
 * 老部署实例上端点缺失 → 25 个表情按钮禁用 + 卡片内给出接口需求（见
 * Web/docs/interfaces-needed-from-server.md）；端点在线时探测转 ok，无需改前端。
 * 素材选择器按 E4 分组「★收藏 / 🕘最近 / 全部目录」（读 utils/favorites.js）。
 */
import { computed, onMounted, onUnmounted, reactive, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import {
  NCard, NSpace, NButton, NTag, NDescriptions, NDescriptionsItem, NInput,
  NInputNumber, NCheckbox, NForm, NFormItem, NResult, NSpin, NPopconfirm,
  NImage, NDivider, NSelect, NAlert, NTooltip, NEmpty, useMessage,
} from 'naive-ui'
import {
  getDevice, updateDevice, triggerOta, getCatalog, getMaterials, paperdollThumbUrl,
  pushMaterial, errText, sendDeviceCommand, probeDeviceCommand, deviceCommandPath,
  DEVICE_COMMAND_TYPE, getDeviceLogs, listPresets,
} from '../api/client'
import { useDevicesStore } from '../stores/devices'
import AppearancePicker from '../components/AppearancePicker.vue'
import CameraPicker from '../components/CameraPicker.vue'
import {
  CATEGORIES, GENDERS, newDraft, appearanceToDraft, draftToAppearance, buildPaperdollId,
} from '../utils/appearance'
import { EXPRESSIONS, BUBBLE_MAX_BYTES, bubbleByteLength } from '../utils/expressions'
import { favoriteIds, recentIds, recordRecent } from '../utils/favorites'
import { fmtTime, fmtAgo } from '../utils/format'

const props = defineProps({ id: { type: String, required: true } })
const router = useRouter()
const message = useMessage()
const devicesStore = useDevicesStore()

const device = ref(null)
const health = ref(null)
const loading = ref(false)
const loadError = ref('')

async function load() {
  loading.value = true
  loadError.value = ''
  try {
    const data = await getDevice(props.id)
    device.value = data?.device ?? null
    health.value = data?.health ?? null
    if (device.value) {
      // 从 petConfig 回填草稿（完整 appearance；旧 5 槽字符串形态由 appearanceToDraft 宽容兼容）
      draft.value = appearanceToDraft(device.value.petConfig ?? {})
      const th = device.value.thresholds ?? {}
      thresholdForm.deadzone = th.imuDeadzoneDeg ?? 8
      thresholdForm.light = th.tapLightG ?? 2
      thresholdForm.hard = th.tapHardG ?? 4
      thresholdForm.idle = th.idleToClockMin ?? 5
      // T4：服务端 DeviceThresholdsConfig 目前无 ImuSensitivity 字段（ConfigService.cs:38-47）
      // → 未探测到就禁用输入且不随 PUT 下发（不硬塞发不出去的字段）
      thresholdForm.sensitivity = th.imuSensitivity ?? 1
      overrideThresholds.value = device.value.thresholdsSource === 'device'
      nameText.value = device.value.name || ''
    }
  } catch (e) {
    loadError.value = e?.serverError || '设备详情加载失败'
  } finally {
    loading.value = false
  }
}
onMounted(load)

// ── 重命名 ──────────────────────────────────────────────────────────────
const nameText = ref('')
const savingName = ref(false)
async function saveName() {
  savingName.value = true
  try {
    await updateDevice(props.id, { name: nameText.value })
    message.success('设备名已保存')
    load()
  } catch (e) {
    message.error(e?.serverError || '重命名失败')
  } finally {
    savingName.value = false
  }
}

// ── 换宠换装（完整 16 槽草稿，与纸娃娃编辑器同一套选择器/合成预览）───────
const draft = ref(newDraft())
const applying = ref(false)
const previewUrl = ref('')
const hasPetConfig = computed(() => !!device.value?.petConfig)
const hasAnyWorn = computed(() => CATEGORIES.some((c) => !!draft.value[c.key]))

// 部件选择器（单类目弹窗）
const pickerShow = ref(false)
const pickerPart = ref('hair')
function openPicker(key) {
  pickerPart.value = key
  pickerShow.value = true
}
function onPick(v) {
  draft.value = { ...draft.value, [pickerPart.value]: v ?? null }
}
function clearSlot(key) {
  draft.value = { ...draft.value, [key]: null }
}
function clearAll() {
  draft.value = newDraft()
}

// 槽位中文名缓存（part → Map(id → name)），失败静默回退显示 id
const partNames = ref({})
async function ensureNames(key) {
  if (partNames.value[key]) return
  try {
    const cat = CATEGORIES.find((c) => c.key === key)
    const data = await getCatalog(key)
    partNames.value[key] = Object.fromEntries(data?.items?.map((it) => [it.id, it.name]) ?? [])
  } catch { partNames.value[key] = {} }
}
CATEGORIES.forEach((c) => ensureNames(c.key))
function slotLabel(key) {
  const id = draft.value[key]
  if (!id) return null
  return partNames.value[key]?.[id] ? `${partNames.value[key][id]} [${id}]` : id
}

// 预览：草稿任何变化 300ms 防抖刷新（服务端真实合成）
let previewTimer = null
watch(
  draft,
  () => {
    clearTimeout(previewTimer)
    previewTimer = setTimeout(() => {
      previewUrl.value = paperdollThumbUrl(buildPaperdollId(draft.value), 192)
    }, 300)
  },
  { deep: true },
)

async function applyPet(clear = false) {
  applying.value = true
  try {
    // 显式传 null = 清空回默认宠物（后端以「字段出现且为 null」判定）
    await updateDevice(props.id, { petConfig: clear ? null : draftToAppearance(draft.value) })
    message.success(clear ? '已恢复默认宠物，设备数秒内自动生效' : '装扮已下发，设备数秒内自动换装')
    previewUrl.value = ''
    await load()
    devicesStore.fetchAll({ silent: true }).catch(() => {})
    // 首页卡片缩略图用的 petConfig 缓存置失效重取（T1：换装后卡片立刻反映新装扮）
    devicesStore.ensurePetConfig(props.id, { force: true }).catch(() => {})
  } catch (e) {
    message.error(e?.serverError || '装扮下发失败')
  } finally {
    applying.value = false
  }
}

// ── 从预设推送（纸娃娃编辑器「已存预设」→ 本设备，同一 E13 提交链路）─────────
// 预设存服务端（GET /admin/presets，与 PaperdollView 同一份，无本机副本），此处只读列出
// type==='paperdoll' 的条目。推送 = 预设 data（appearance JSON，槽位 { id } 对象形态；
// 旧数据可能是 5 槽/纯字符串形态）→ appearanceToDraft 宽容转草稿 → draftToAppearance 归一化
// → PUT /admin/devices/{id} { petConfig }（与逐槽「应用到设备」同端点同 payload），
// 服务端 bump manifest rev → 设备数秒内拉取换装；成功后反馈链与 applyPet 相同（load 回填 + 首页缓存刷新）。
const presets = ref([])
const presetsLoading = ref(false)
const presetsError = ref('')
const pushingPresetId = ref('') // 正在推送的预设 id；非空即「任一推送中」，行间互斥防重复点击

async function loadPresets() {
  presetsLoading.value = true
  presetsError.value = ''
  try {
    const data = await listPresets()
    presets.value = data?.presets ?? []
  } catch (e) {
    presetsError.value = e?.serverError || '预设列表加载失败'
  } finally {
    presetsLoading.value = false
  }
}

/** 展示行：仅 paperdoll 预设；顺手把 appearance 转草稿拼 64px 合成缩略图（数据坏损时仅无图，不影响推送）。 */
const presetRows = computed(() => presets.value
  .filter((p) => p.type === 'paperdoll')
  .map((p) => {
    let thumb = ''
    try {
      thumb = paperdollThumbUrl(buildPaperdollId(appearanceToDraft(p.data, p.name)), 64)
    } catch { /* keep thumb = '' */ }
    return { ...p, thumb }
  }))

async function pushPreset(p) {
  if (pushingPresetId.value) return
  const d = appearanceToDraft(p.data, p.name) // appearance 槽位串 → 草稿（宽容兼容旧形态）
  // 与逐槽 apply 的 hasAnyWorn 同口径：全空预设等于「清空装扮」，不静默下发
  if (!CATEGORIES.some((c) => !!d[c.key])) {
    message.warning(`预设「${p.name}」没有任何穿戴槽位，已取消推送`)
    return
  }
  pushingPresetId.value = p.id
  try {
    // 与 E13 逐槽「应用到设备」同一端点/同一 payload（draftToAppearance 归一化出完整 appearance）
    await updateDevice(props.id, { petConfig: draftToAppearance(d) })
    message.success(`已推送预设「${p.name}」到设备，等待设备拉取`)
    await load() // petConfig 回填面板草稿（与 applyPet 相同的 manifest 反馈链）
    devicesStore.fetchAll({ silent: true }).catch(() => {})
    devicesStore.ensurePetConfig(props.id, { force: true }).catch(() => {})
  } catch (e) {
    message.error(e?.serverError || `推送预设「${p.name}」失败`)
  } finally {
    pushingPresetId.value = ''
  }
}
onMounted(loadPresets)

// ── 阈值覆盖 ────────────────────────────────────────────────────────────
const overrideThresholds = ref(false)
const thresholdForm = reactive({ deadzone: 8, light: 2, hard: 4, idle: 5, sensitivity: 1 })
const savingTh = ref(false)
const thresholdsSource = computed(() => device.value?.thresholdsSource || 'global')
/** E4：设备表阈值是否已含灵敏度字段（运行时探测；有则启用并下发，无则禁用+说明）。 */
const imuSensitivitySupported = computed(() => device.value?.thresholds?.imuSensitivity != null)

async function saveThresholds() {
  savingTh.value = true
  try {
    const thresholds = {
      imuDeadzoneDeg: Number(thresholdForm.deadzone),
      tapLightG: Number(thresholdForm.light),
      tapHardG: Number(thresholdForm.hard),
      idleToClockMin: Number(thresholdForm.idle),
    }
    // 仅在服务端确有该字段时才带上（否则会被 ConfigService 反序列化静默丢弃，属"发不出去"）
    if (imuSensitivitySupported.value) thresholds.imuSensitivity = Number(thresholdForm.sensitivity)
    await updateDevice(props.id, { thresholds: overrideThresholds.value ? thresholds : null })
    message.success(overrideThresholds.value ? '已按设备覆盖阈值' : '已保存（未勾选覆盖，不改变阈值）')
    load()
  } catch (e) {
    message.error(e?.serverError || '阈值保存失败')
  } finally {
    savingTh.value = false
  }
}

// ── OTA（E11：WiFi 拉包自更新，双分区回滚）──────────────────────────────
// ── 设备端日志（E14）────────────────────────────────────────────────────
const logBusy = ref(false)
const logAuto = ref(false)
const logLevel = ref('')
const logTag = ref('')
const logItems = ref([])
const logMeta = ref(null)
const logNote = ref('')
let logTimer = null

function fmtLogTs(l, clockSynced) {
  // clockSynced=false 时设备 ts 是开机毫秒，用接收时刻兜底（契约 §7.2）
  if (clockSynced && l.tsUtc) return fmtTime(l.tsUtc)
  return l.receivedUtc ? `${fmtTime(l.receivedUtc)}(收)` : `t=${l.t}`
}

async function loadLogs(silent = false) {
  if (!deviceId.value) return
  logBusy.value = true
  try {
    const d = await getDeviceLogs(deviceId.value, {
      sinceSeq: 0, limit: 200, level: logLevel.value, tag: logTag.value,
    })
    logMeta.value = { total: d.total ?? 0, lastSeq: d.lastSeq ?? 0, clockSynced: !!d.clockSynced }
    logNote.value = ''
    logItems.value = (d.items ?? []).slice(-200).reverse().map((l) => ({
      ...l,
      tsText: fmtLogTs(l, !!d.clockSynced),
    }))
  } catch (e) {
    // 404/405 → 服务端还没实现该端点（旧镜像）；不要当成错误弹窗
    const st = e?.response?.status
    logItems.value = []
    logMeta.value = null
    logNote.value = (st === 404 || st === 405)
      ? '服务端未实现 GET /api/admin/device-logs/{id}（旧镜像）→ 需要重新构建部署 Server 后可用；设备侧上报通道已在跑。'
      : `拉取失败：${errText(e)}`
    if (!silent && st !== 404 && st !== 405) message.error(logNote.value)
  } finally {
    logBusy.value = false
  }
}

function syncLogAuto(on) {
  if (logTimer) { clearInterval(logTimer); logTimer = null }
  if (on) logTimer = setInterval(() => loadLogs(true), 5000)
}
watch(logAuto, syncLogAuto)
watch([logLevel, logTag], () => loadLogs(true))
onUnmounted(() => { if (logTimer) clearInterval(logTimer) })

const otaVer = ref('')
const otaBusy = ref(false)
async function doOta() {
  const ver = otaVer.value.trim()
  if (!/^\d+(\.\d+){0,3}$/.test(ver)) {
    message.warning('固件版本格式如 0.3.1（对应 data/firmware/<ver>.bin）')
    return
  }
  otaBusy.value = true
  try {
    const r = await triggerOta(props.id, ver)
    message.success(`升级指令已入队（seq ${r?.seq}，设备 WiFi 拉取 ${r?.url} 自更新）`)
    otaVer.value = ''
  } catch (e) {
    message.error(e?.serverError || 'OTA 下发失败')
  } finally {
    otaBusy.value = false
  }
}

// ── 素材推送到设备（T3/E7：地图/NPC 资产登记进该设备 manifest）─────────────
// E4「地图选择含收藏（喂给设备选择器的「最近+收藏」）」：选项按 ★收藏 / 🕘最近 / 全部目录 分组，
// 收藏与「最近」读自 utils/favorites.js（与素材页星标同一份 localStorage 单例；服务端收藏端点缺失时
// 仍照常工作，端点上线后素材页会把它同步上去）。成功推送即记一次「最近使用」。
const pushKind = ref('map')
const pushId = ref('')
const pushSwitch = ref(true)
const pushBusy = ref(false)
const pushResult = ref(null) // { type, text }
const matAll = ref([]) // 当前 kind 的目录全量 [{ id, name }]
const matLoading = ref(false)
const matLoaded = reactive({}) // kind → 已加载过

function matLabel(id) {
  const hit = matAll.value.find((it) => it.id === id)
  return hit ? `${hit.name || '—'} [${id}]` : `[${id}]`
}

/**
 * 设备选择器选项（分组）：★ 收藏 → 🕘 最近 → 全部目录。
 * 同一 id 只出现一次（上面出现过的从「全部目录」里剔除）；收藏/最近里的 id 即使不在目录中
 * （手工输入过 / 目录未加载完）也保留，值仍是可下发的编号字符串。
 */
const matOptions = computed(() => {
  const shown = new Set()
  const take = (ids) => {
    const out = []
    for (const raw of ids) {
      const id = String(raw)
      if (shown.has(id)) continue
      shown.add(id)
      out.push({ label: matLabel(id), value: id })
    }
    return out
  }
  const groups = []
  const favs = take(favoriteIds(pushKind.value))
  if (favs.length) groups.push({ type: 'group', label: '★ 收藏', key: 'g-fav', children: favs })
  const rec = take(recentIds(pushKind.value))
  if (rec.length) groups.push({ type: 'group', label: '🕘 最近使用', key: 'g-recent', children: rec })
  const rest = take(matAll.value.map((it) => it.id))
  if (rest.length) groups.push({ type: 'group', label: '全部目录', key: 'g-all', children: rest })
  return groups
})

async function loadMatOptions() {
  const k = pushKind.value
  if (matLoaded[k] || matLoading.value) return
  matLoading.value = true
  try {
    const data = await getMaterials(k)
    matAll.value = (data?.items ?? []).map((it) => ({ id: String(it.id), name: it.name ?? '' }))
    matLoaded[k] = true
  } catch (e) {
    matAll.value = []
    pushResult.value = { type: 'error', text: `素材目录加载失败：${errText(e)}（仍可直接输入编号）` }
  } finally {
    matLoading.value = false
  }
}
watch(pushKind, () => {
  pushId.value = ''
  matAll.value = []
  loadMatOptions()
})

function pushErrorHint(status) {
  switch (status) {
    case 400: return '请求参数被拒：kind 必须是 map|npc，id 不能为空'
    case 404: return '设备不存在（该设备可能已被移除）'
    case 503: return 'WZ 未加载：到「设置」页配置 WZ 路径后重试'
    case 500: return '服务端资产打包异常（详见服务端日志）'
    default: return ''
  }
}

async function doPushMaterial() {
  const id = pushId.value.trim()
  if (!id) {
    message.warning('请选择或输入素材编号（如地图 200000100）')
    return
  }
  pushBusy.value = true
  pushResult.value = null
  try {
    const r = await pushMaterial(props.id, pushKind.value, id, pushSwitch.value)
    const text = `已受理（HTTP 202）：${r?.note || '后台打包中'}`
    pushResult.value = { type: 'success', text }
    recordRecent(pushKind.value, id) // E4「最近」：成功推送即记一次（设备选择器分组置顶）
    message.success(text)
  } catch (e) {
    const status = e?.response?.status
    const hint = pushErrorHint(status)
    const text = `${errText(e)}${status ? `（HTTP ${status}）` : ''}${hint ? ` · ${hint}` : ''}`
    pushResult.value = { type: 'error', text }
    message.error(text)
  } finally {
    pushBusy.value = false
  }
}

// ── 动作 / 表情 / 气泡调试（T2/E4：25 表情手动指定）────────────────────────
// 服务端缺 admin 入队端点（AdminEndpoints.cs 只有 manifest/ota/map/push）→ 开卡先探测：
// supported=false(404/405) → 按钮全禁用 + 卡片内贴出接口需求；端点上线后探测自动转 ok。
const cmdPath = computed(() => deviceCommandPath(props.id))
const cmdSupport = ref('unknown') // unknown | ok | missing | mismatch
const cmdProbeNote = ref('')
const cmdProbeBusy = ref(false)
const cmdBusy = ref('') // 正在下发的表情 key / 'bubble'
const bubbleText = ref('')

const bubbleBytes = computed(() => bubbleByteLength(bubbleText.value))
const bubbleTooLong = computed(() => bubbleBytes.value > BUBBLE_MAX_BYTES)
const cmdDisabled = computed(() => cmdSupport.value === 'missing')

async function probeCmd() {
  cmdProbeBusy.value = true
  cmdSupport.value = 'unknown'
  cmdProbeNote.value = ''
  try {
    const r = await probeDeviceCommand(props.id)
    if (r.supported === false) {
      cmdSupport.value = 'missing'
      cmdProbeNote.value = `HTTP ${r.status}`
    } else if (r.supported === true && r.status >= 400) {
      cmdSupport.value = 'mismatch'
      cmdProbeNote.value = `HTTP ${r.status}：${r.error || ''}`
    } else if (r.supported === true) {
      cmdSupport.value = 'ok'
    } else {
      cmdSupport.value = 'unknown'
      cmdProbeNote.value = r.error || '无法判定（网络不可达？）'
    }
  } finally {
    cmdProbeBusy.value = false
  }
}
onMounted(probeCmd)

/** 发一条指令；404/405 → 判定端点缺失并禁用按钮（该端点服务端已上线，此分支只对老部署实例生效）。 */
async function sendCmd(type, value, busyKey) {
  cmdBusy.value = busyKey
  try {
    const r = await sendDeviceCommand(props.id, type, value)
    cmdSupport.value = 'ok'
    message.success(`指令已入队（${type}=${value}${r?.seq != null ? `，seq ${r.seq}` : ''}）`)
    return true
  } catch (e) {
    const status = e?.response?.status
    if (status === 404 || status === 405 || status === 501) {
      cmdSupport.value = 'missing'
      cmdProbeNote.value = `HTTP ${status}`
      message.error(`服务端未提供 ${cmdPath.value}（HTTP ${status}）—— 见卡片内接口需求`)
    } else {
      message.error(errText(e))
    }
    return false
  } finally {
    cmdBusy.value = ''
  }
}
function playExpression(ex) {
  sendCmd(DEVICE_COMMAND_TYPE.EXPRESSION, ex, ex)
}
async function sendBubble() {
  const t = bubbleText.value.trim()
  if (!t) return
  if (bubbleTooLong.value) {
    message.warning(`气泡文本 ${bubbleBytes.value} 字节，超出固件上限 ${BUBBLE_MAX_BYTES} 字节（≈31 汉字）`)
    return
  }
  if (await sendCmd(DEVICE_COMMAND_TYPE.BUBBLE, t, 'bubble')) bubbleText.value = ''
}
</script>

<template>
  <div>
    <n-spin v-if="loading" class="block-center" />

    <n-result
      v-else-if="loadError"
      status="404"
      title="设备不存在或加载失败"
      :description="loadError"
    >
      <template #footer>
        <n-space justify="center">
          <n-button @click="load">重试</n-button>
          <n-button type="primary" @click="router.push('/')">返回设备总览</n-button>
        </n-space>
      </template>
    </n-result>

    <n-space v-else-if="device" vertical :size="12">
      <n-card size="small">
        <template #header>
          <n-space align="center">
            <b>{{ device.name || '未命名设备' }}</b>
            <n-tag :bordered="false" :type="device.online ? 'success' : 'default'">
              {{ device.online ? '在线' : `离线 · ${fmtAgo(device.lastSeenUtc)}` }}
            </n-tag>
            <n-tag size="small" :bordered="false" :type="device.paired ? 'info' : 'warning'">
              {{ device.paired ? '已配对' : '匿名' }}
            </n-tag>
          </n-space>
        </template>
        <n-descriptions size="small" :column="3" label-placement="left" bordered>
          <n-descriptions-item label="deviceId">{{ device.deviceId }}</n-descriptions-item>
          <n-descriptions-item label="UUID">{{ device.uuid }}</n-descriptions-item>
          <n-descriptions-item label="固件版本">{{ device.firmware || '—' }}</n-descriptions-item>
          <n-descriptions-item label="屏幕">{{ device.profile ? `${device.profile.w}×${device.profile.h} ${device.profile.shape}` : '—' }}</n-descriptions-item>
          <n-descriptions-item label="PSRAM">{{ device.profile ? `${device.profile.psram} MB` : '—' }}</n-descriptions-item>
          <n-descriptions-item label="音频">{{ device.profile?.audio ? '有' : '无' }}</n-descriptions-item>
          <n-descriptions-item label="BGM 偏好">{{ device.bgm?.source || 'wz' }} · 音量 {{ device.bgm?.volume ?? '—' }}</n-descriptions-item>
          <n-descriptions-item label="最近心跳">{{ fmtTime(device.lastSeenUtc) }}</n-descriptions-item>
          <n-descriptions-item label="入册时间">{{ fmtTime(device.createdAtUtc) }}</n-descriptions-item>
          <n-descriptions-item v-if="health?.batteryPercent != null" label="电量">{{ health.batteryPercent }}%</n-descriptions-item>
          <n-descriptions-item v-if="health?.lastError" label="最近异常" :span="2">
            {{ health.lastError }}（{{ fmtTime(health.lastErrorUtc) }}）
          </n-descriptions-item>
        </n-descriptions>
        <n-divider style="margin: 12px 0" />
        <n-space align="center">
          <n-input v-model:value="nameText" placeholder="设备名称" :maxlength="32" style="width: 220px" :disabled="savingName" />
          <n-button size="small" secondary :loading="savingName" @click="saveName">重命名</n-button>
        </n-space>
      </n-card>

      <n-card title="换宠换装（按设备隔离，E13）" size="small">
        <n-space align="flex-start" :size="20">
          <div style="flex: 1; min-width: 320px">
            <div class="gender-row">
              <span class="slot-label">性别</span>
              <n-select
                :value="draft.gender"
                :options="GENDERS"
                size="small"
                style="width: 120px"
                @update:value="(v) => (draft = { ...draft, gender: v })"
              />
              <span class="hint">仅影响外观草稿的性别字段</span>
            </div>
            <div class="slot-grid">
              <div v-for="cat in CATEGORIES" :key="cat.key" class="slot-row">
                <span class="slot-icon">{{ cat.icon }}</span>
                <span class="slot-label">{{ cat.label }}</span>
                <span class="slot-value" :class="{ worn: !!draft[cat.key] }" :title="slotLabel(cat.key)">
                  {{ slotLabel(cat.key) || '未穿戴' }}
                </span>
                <n-button size="tiny" secondary @click="openPicker(cat.key)">选择…</n-button>
                <n-button size="tiny" quaternary :disabled="!draft[cat.key]" @click="clearSlot(cat.key)">×</n-button>
              </div>
            </div>
          </div>
          <div class="preview-col">
            <div class="preview-box">
              <n-image
                v-if="previewUrl"
                :src="previewUrl"
                :key="previewUrl"
                width="160"
                height="160"
                object-fit="contain"
                style="border-radius: 8px; background: #f7f7fa"
              />
              <span v-else class="hint">装扮后出合成预览</span>
            </div>
            <n-tag size="small" :bordered="false">{{ hasPetConfig ? '当前有自定义装扮' : '当前为默认宠物' }}</n-tag>
          </div>
        </n-space>

        <!-- 从预设推送：纸娃娃编辑器保存的预设一键下发到本设备（与「应用到设备」同一端点/payload） -->
        <n-divider style="margin: 14px 0 10px" />
        <div class="preset-push">
          <div class="preset-head">
            <span class="preset-title">从预设推送</span>
            <span class="hint">选一个纸娃娃编辑器保存的装扮预设，一键推送到该设备（服务端预设，与「纸娃娃」页同一份）</span>
            <n-button size="tiny" secondary :loading="presetsLoading" @click="loadPresets">刷新</n-button>
          </div>
          <n-spin v-if="presetsLoading" size="small" class="preset-empty" />
          <n-alert v-else-if="presetsError" type="warning" :show-icon="false" size="small">
            预设列表加载失败：{{ presetsError }}
            <n-button size="tiny" secondary style="margin-left: 8px" @click="loadPresets">重试</n-button>
          </n-alert>
          <n-empty
            v-else-if="!presetRows.length"
            size="small"
            description="暂无预设 —— 先到「纸娃娃」编辑器保存一个装扮预设"
            class="preset-empty"
          />
          <div v-else class="preset-list">
            <div v-for="p in presetRows" :key="p.id" class="preset-row">
              <n-image
                v-if="p.thumb"
                :src="p.thumb"
                :key="p.thumb"
                width="40"
                height="40"
                object-fit="contain"
                class="preset-thumb"
              />
              <span v-else class="preset-thumb preset-thumb-none">👗</span>
              <div class="preset-info">
                <span class="preset-name">{{ p.name }}</span>
                <span class="hint">保存于 {{ fmtTime(p.updatedAtUtc) }}</span>
              </div>
              <n-button
                size="tiny"
                type="primary"
                secondary
                :loading="pushingPresetId === p.id"
                :disabled="!!pushingPresetId"
                @click="pushPreset(p)"
              >
                推送到此设备
              </n-button>
            </div>
          </div>
        </div>
        <template #action>
          <n-space justify="space-between">
            <n-popconfirm @positive-click="applyPet(true)">
              <template #trigger>
                <n-button quaternary type="warning" :loading="applying">清空装扮（回默认）</n-button>
              </template>
              清空该设备的自定义装扮，恢复默认宠物？
            </n-popconfirm>
            <n-space>
              <n-button secondary @click="clearAll">全部清空</n-button>
              <n-button type="primary" :loading="applying" :disabled="!hasAnyWorn" @click="applyPet(false)">
                应用到设备
              </n-button>
            </n-space>
          </n-space>
        </template>
      </n-card>

      <AppearancePicker
        v-model:show="pickerShow"
        :model-value="draft[pickerPart]"
        :part="pickerPart"
        
        @update:model-value="onPick"
      />

      <n-card title="阈值（IMU / 力度 / 待机）" size="small">
        <template #header-extra>
          <n-space align="center">
            <n-tag size="small" :bordered="false" :type="thresholdsSource === 'device' ? 'warning' : 'default'">
              {{ thresholdsSource === 'device' ? '本设备覆盖' : '跟随全局（设置页）' }}
            </n-tag>
            <n-checkbox v-model:checked="overrideThresholds">覆盖全局阈值</n-checkbox>
          </n-space>
        </template>
        <n-form label-placement="top">
          <div class="grid4">
            <n-form-item label="IMU 死区（±°）">
              <n-input-number v-model:value="thresholdForm.deadzone" :min="0" :max="45" :step="0.5" :disabled="!overrideThresholds || savingTh" style="width: 100%" />
            </n-form-item>
            <n-form-item label="轻拍阈值（g）">
              <n-input-number v-model:value="thresholdForm.light" :min="0.5" :max="16" :step="0.1" :disabled="!overrideThresholds || savingTh" style="width: 100%" />
            </n-form-item>
            <n-form-item label="重拍阈值（g）">
              <n-input-number v-model:value="thresholdForm.hard" :min="0.5" :max="16" :step="0.1" :disabled="!overrideThresholds || savingTh" style="width: 100%" />
            </n-form-item>
            <n-form-item label="待机转时钟（分钟）">
              <n-input-number v-model:value="thresholdForm.idle" :min="1" :max="240" :disabled="!overrideThresholds || savingTh" style="width: 100%" />
            </n-form-item>
            <!-- E4「IMU 灵敏度」：服务端 DeviceThresholdsConfig.ImuSensitivity 已在上位（随 thresholds 覆盖下发）；
                 探测只为兼容老部署实例（探测不到 → 禁用且保存时不带该字段，不硬塞发不出去的字段） -->
            <n-form-item>
              <template #label>
                <n-tooltip trigger="hover" :disabled="imuSensitivitySupported">
                  <template #trigger>
                    <span>IMU 灵敏度（倍率，越大越灵敏）</span>
                  </template>
                  当前服务端未返回 thresholds.imuSensitivity（老部署实例？），暂不可下发 ——
                  接口需求见 Web/docs/interfaces-needed-from-server.md §T4
                </n-tooltip>
              </template>
              <n-input-number
                v-model:value="thresholdForm.sensitivity"
                :min="0.2"
                :max="3"
                :step="0.1"
                :disabled="!overrideThresholds || savingTh || !imuSensitivitySupported"
                style="width: 100%"
              />
            </n-form-item>
          </div>
        </n-form>
        <n-alert v-if="!imuSensitivitySupported" type="info" :show-icon="false" size="small" class="mt8">
          「IMU 灵敏度」当前为占位（禁用态）：本次设备响应的 <code>thresholds</code> 里没有
          <code>imuSensitivity</code>，因此不下发该字段。该字段已由服务端实现在位
          （<code>DeviceThresholdsConfig.ImuSensitivity</code>，默认 1.0），此处出现即说明连的是旧部署实例。
          规格见 <code>Web/docs/interfaces-needed-from-server.md</code> §T4。
        </n-alert>
        <template #action>
          <n-space justify="end">
            <n-button type="primary" size="small" :loading="savingTh" @click="saveThresholds">保存阈值</n-button>
          </n-space>
        </template>
      </n-card>

      <!-- 设备串口日志（E14「排障不用插线」）：数据来自 POST /api/device/log，
           是设备端环形缓冲的服务端副本；端点缺失时给出可读提示而非报错 -->
      <n-card title="设备日志（E14 排障不用插线）" size="small">
        <n-space vertical :size="10">
          <n-space align="center" :size="8">
            <n-select v-model:value="logLevel" size="small" style="width: 120px"
              :options="[{ label: '全部级别', value: '' }, { label: '仅 E 错误', value: 'E' }, { label: '仅 W 警告', value: 'W' }, { label: '仅 I 信息', value: 'I' }]" />
            <n-input v-model:value="logTag" size="small" placeholder="按 TAG 过滤（如 poller）" style="width: 200px" />
            <n-button size="small" :loading="logBusy" @click="loadLogs()">刷新</n-button>
            <n-switch v-model:value="logAuto" size="small"><template #checked>自动 5s</template><template #unchecked>手动</template></n-switch>
            <span class="hint" v-if="logMeta">共 {{ logMeta.total }} 条 · lastSeq {{ logMeta.lastSeq }} · 设备校时{{ logMeta.clockSynced ? '正常' : '未校准（时间用接收时刻兜底）' }}</span>
          </n-space>
          <n-alert v-if="logNote" type="warning" :show-icon="false" size="small">{{ logNote }}</n-alert>
          <div class="logbox" v-if="logItems.length">
            <div v-for="l in logItems" :key="l.seq" class="logline" :class="'lv-' + l.lvl">
              <span class="lt">{{ l.tsText }}</span>
              <span class="ll">{{ l.lvl }}</span>
              <span class="lg">{{ l.tag }}</span>
              <span class="lm">{{ l.msg }}</span>
            </div>
          </div>
          <div v-else class="hint">{{ logMeta ? '（该设备暂无可展示日志）' : '点「刷新」拉取设备端最近日志' }}</div>
        </n-space>
      </n-card>

      <n-card title="固件升级（OTA，E11）" size="small">
        <n-space align="center">
          <n-input v-model:value="otaVer" placeholder="目标版本，如 0.3.1" style="width: 200px" :disabled="otaBusy" />
          <n-popconfirm @positive-click="doOta">
            <template #trigger>
              <n-button type="primary" secondary :loading="otaBusy">下发升级指令</n-button>
            </template>
            下发 OTA {{ otaVer || '(未填版本)' }} ？设备将从服务端拉取固件包自更新（双分区，失败自动回滚）。
          </n-popconfirm>
          <span class="hint">bin 需先放到服务端 data/firmware/&lt;ver&gt;.bin</span>
        </n-space>
      </n-card>

      <!-- 素材推送（T3/E7）：选择器 = E4「最近+收藏」，推送是真正让素材上机的动作 -->
      <n-card title="素材推送（地图 / NPC 上机，E7）" size="small">
        <n-space vertical :size="10">
          <n-space align="center" :size="8" style="width: 100%">
            <n-select v-model:value="pushKind" :options="[{ label: '地图 map', value: 'map' }, { label: 'NPC npc', value: 'npc' }]" size="small" style="width: 130px" :disabled="pushBusy" />
            <n-select
              v-model:value="pushId"
              filterable
              tag
              clearable
              size="small"
              style="min-width: 320px; flex: 1"
              :options="matOptions"
              :loading="matLoading"
              :disabled="pushBusy"
              placeholder="搜索目录或直接输入素材编号（如 200000100）"
              @update:show="(v) => v && loadMatOptions()"
            />
            <n-checkbox v-model:checked="pushSwitch" :disabled="pushBusy">登记后立即切换（仅地图）</n-checkbox>
            <n-popconfirm @positive-click="doPushMaterial">
              <template #trigger>
                <n-button type="primary" size="small" :loading="pushBusy" :disabled="!pushId">推送到本设备</n-button>
              </template>
              把 {{ pushKind === 'map' ? '地图' : 'NPC' }} {{ pushId }} 推送到 {{ device.name || device.deviceId }} ？
              服务端后台打包（HTTP 202 受理）→ 设备拉到新 manifest → 完成后自动切换。
            </n-popconfirm>
          </n-space>
          <n-alert
            v-if="pushResult"
            :type="pushResult.type === 'success' ? 'success' : 'error'"
            :show-icon="false"
            size="small"
          >
            {{ pushResult.text }}
          </n-alert>
          <div class="hint">
            选择器分组：<b>★ 收藏</b> / <b>🕘 最近使用</b>（E4「地图选择含收藏」，读素材页星标同一份
            本机存储 <code>minipet.materials.favorites</code> / <code>.recent</code>；服务端收藏端点缺失时
            仍照常工作）/ <b>全部目录</b>。端点：
            <code>POST /api/admin/devices/{{ device.deviceId }}/push</code> body
            <code>{ kind: "map"|"npc", id, switch }</code> → 202 受理（后台打包，数秒）；
            失败分支：400 参数非法 / 404 设备不存在 / 503 WZ 未加载 / 500 打包异常。
          </div>
        </n-space>
      </n-card>

      <!-- 选镜头（服务端选相机机位）：整图预览 + 框外半黑取景框 + 机位落盘 + 一键下发 cam。
           放在「素材推送」之后——先有 BGMAP 地图上机，这里才有图可选。 -->
      <CameraPicker :device-id="device.deviceId" />

      <!-- 动作 / 表情 / 气泡调试（T2/E4）：25 表情手动指定 -->
      <n-card title="表情 / 气泡调试（E4 25 表情手动指定）" size="small">
        <template #header-extra>
          <n-space align="center">
            <n-tag size="small" :bordered="false" :type="cmdSupport === 'ok' ? 'success' : cmdSupport === 'missing' ? 'error' : 'default'">
              {{ cmdSupport === 'ok' ? '端点在位' : cmdSupport === 'missing' ? '端点缺失' : cmdSupport === 'mismatch' ? '端点入参不符' : '未探测到' }}
            </n-tag>
            <n-button size="tiny" secondary :loading="cmdProbeBusy" @click="probeCmd">重新检测</n-button>
          </n-space>
        </template>

        <n-space vertical :size="10">
          <n-alert v-if="cmdDisabled" type="warning" :show-icon="false" size="small">
            <b>本次探测判定设备指令端点不可用，25 个表情按钮已禁用。</b>
            （探测结果：{{ cmdProbeNote || 'HTTP 404/405' }}）该端点服务端<b>已上线</b>（实测
            <code>202 {"ok":true,"seq":47,…}</code>）；出现此提示说明连的是旧部署实例，
            或探测请求被网络/依赖问题挡下。端点规格：
            <div class="req">
              <div><code>POST {{ cmdPath }}</code></div>
              <div>body <code>{ "type": "expression" | "action" | "bubble", "value": "&lt;表情名/动作名/气泡文本&gt;", "durationMs"?: number }</code></div>
              <div>期望 <code>202 { "ok": true, "seq": &lt;n&gt;, "type": "...", "value": "..." }</code>；表达式名限清单内 25 个，气泡 ≤ 95 字节（UTF-8）</div>
              <div>
                实现要点：<code>CommandQueue.Enqueue(id, type, value)</code> —— payload 必须是
                <b>裸 JSON 字符串</b>或 <code>{ "id": "..." }</code>；固件
                （<code>Firmware/main/net/poller.c:141-210</code>）读 <code>commands[].type</code> +
                <code>payload</code>，形如 <code>{"value":"..."}</code> 会被静默忽略。
              </div>
              <div>完整清单：<code>Web/docs/interfaces-needed-from-server.md</code> §T2</div>
            </div>
          </n-alert>
          <n-alert v-else-if="cmdSupport === 'mismatch'" type="warning" :show-icon="false" size="small">
            端点存在但拒绝了约定入参（{{ cmdProbeNote }}）—— 请对照 <code>Web/docs/interfaces-needed-from-server.md</code> §T2 核对请求体字段名。
          </n-alert>
          <n-alert v-else-if="cmdSupport === 'unknown'" type="info" :show-icon="false" size="small">
            未能判定端点是否可用（{{ cmdProbeNote || '网络不可达' }}）：按钮未禁用，点击后按真实响应提示。
          </n-alert>

          <div class="expr-grid">
            <n-button
              v-for="ex in EXPRESSIONS"
              :key="ex.key"
              size="small"
              secondary
              :type="ex.friendly ? 'primary' : 'default'"
              :disabled="cmdDisabled"
              :loading="cmdBusy === ex.key"
              :title="`${ex.cn}（${ex.key}）${ex.friendly ? ' · 随机池' : ''}`"
              @click="playExpression(ex.key)"
            >
              {{ ex.cn }}<span class="expr-key">{{ ex.key }}</span>
            </n-button>
          </div>
          <div class="hint">
            25 个表情名逐字取自 <code>PaperdollService.KnownExpressions</code>（服务端 WZ 实测清单）/ <code>docs/ai/README.md:93</code>；
            蓝色为主角「随机表情」池（Friendly），其余为手动触发；<code>blink</code> 由渲染层本地插播，仅调试用。
          </div>

          <n-divider style="margin: 4px 0" />

          <n-space align="center" :size="8" style="width: 100%">
            <n-input
              v-model:value="bubbleText"
              size="small"
              style="flex: 1; min-width: 260px"
              :status="bubbleTooLong ? 'error' : undefined"
              placeholder="气泡文本（同端点下发；固件上限 95 字节 ≈ 31 汉字）"
              :disabled="cmdDisabled"
              @keyup.enter="sendBubble"
            />
            <n-tag size="small" :bordered="false" :type="bubbleTooLong ? 'error' : 'default'">
              {{ bubbleBytes }} / {{ BUBBLE_MAX_BYTES }} 字节
            </n-tag>
            <n-button size="small" secondary :disabled="cmdDisabled || !bubbleText.trim() || bubbleTooLong" :loading="cmdBusy === 'bubble'" @click="sendBubble">
              发气泡
            </n-button>
          </n-space>
        </n-space>
      </n-card>
    </n-space>
  </div>
</template>

<style scoped>
.grid4 {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
  gap: 0 16px;
}
.hint { font-size: 12px; opacity: 0.6; }
.mt8 { margin-top: 8px; }
.expr-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(112px, 1fr)); gap: 8px; }
.expr-key { font-size: 10px; opacity: 0.55; margin-left: 4px; }
.req { margin-top: 6px; line-height: 1.8; }
.req code { background: rgba(128, 128, 140, 0.15); padding: 0 3px; border-radius: 3px; }
.gender-row { display: flex; align-items: center; gap: 8px; margin-bottom: 10px; }
.slot-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 4px 16px; }
.slot-row { display: flex; align-items: center; gap: 6px; min-height: 30px; }
.slot-icon { width: 18px; text-align: center; }
.slot-label { font-size: 12px; opacity: 0.75; width: 34px; flex: none; }
.slot-value {
  flex: 1; min-width: 0; font-size: 12px; opacity: 0.45;
  white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
}
.slot-value.worn { opacity: 1; }
.preview-col { display: flex; flex-direction: column; align-items: center; gap: 8px; }
.preview-box { width: 160px; height: 160px; display: flex; align-items: center; justify-content: center; }
.block-center { display: flex; justify-content: center; padding: 48px 0; }

.preset-push { display: flex; flex-direction: column; gap: 8px; }
.preset-head { display: flex; align-items: center; gap: 10px; }
.preset-title { font-size: 13px; font-weight: 600; flex: none; }
.preset-head .hint { flex: 1; }
.preset-empty { padding: 12px 0; }
.preset-list { display: flex; flex-direction: column; }
.preset-row {
  display: flex; align-items: center; gap: 10px; padding: 6px 0;
  border-bottom: 1px dashed rgba(128, 128, 140, 0.25);
}
.preset-row:last-child { border-bottom: none; }
.preset-thumb { border-radius: 6px; background: #f7f7fa; flex: none; }
.preset-thumb-none { width: 40px; height: 40px; display: inline-flex; align-items: center; justify-content: center; }
.preset-info { flex: 1; min-width: 0; display: flex; flex-direction: column; }
.preset-name { font-weight: 600; font-size: 13px; }

.logbox {
  max-height: 300px;
  overflow: auto;
  background: #0f1014;
  border: 1px solid #2a2a32;
  border-radius: 8px;
  padding: 8px 10px;
  font-family: ui-monospace, Menlo, Consolas, monospace;
  font-size: 12px;
  line-height: 1.55;
}
.logline { display: flex; gap: 8px; white-space: pre-wrap; word-break: break-all; }
.logline .lt { color: #6b7280; flex: 0 0 132px; }
.logline .ll { flex: 0 0 12px; font-weight: 700; }
.logline .lg { color: #7dd3fc; flex: 0 0 92px; overflow: hidden; text-overflow: ellipsis; }
.logline .lm { flex: 1; color: #d8dee9; }
.logline.lv-E .ll, .logline.lv-E .lm { color: #ff6b81; }
.logline.lv-W .ll, .logline.lv-W .lm { color: #fbbf24; }
</style>
