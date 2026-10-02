<script setup>
/**
 * 选镜头（服务端选相机机位）—— 设备详情页卡片。
 *
 * 为什么要有这个界面：设备渲染的是"整图地图"，相机 = 可见窗口左上角的**整图世界坐标**
 * （世界 1x、屏 2x ⇒ 可见窗口 = 屏宽/2 世界像素，480 屏 = 240×240），可平移范围
 * x∈[0,vw-240]、y∈[0,vh-240]。设备上只能用手指拖，看不见"整张图里选的是哪一角"，
 * 所以：整图预览 + 取景框在服务端 Web 上选，坐标记在服务端，再一键下发给设备。
 *
 * 界面口径（两处画面，别混）：
 *   · 左侧「整图预览」= 服务端按该图 BGMAP 的 vw/vh 重渲的整图参考图（Back+Tile+Obj，
 *     相机=地图中心，零缩放）→ 图内像素 = 整图世界坐标。取景框画在上面：
 *     **框外半黑、框内原色**（框的巨型 box-shadow 铺满整块舞台 + 舞台 overflow:hidden），
 *     一眼看出"选的是哪里"。
 *   · 右侧「设备视角」= 服务端按当前机位重渲的 240×240（1x），页面按 **2x 就近放大**显示
 *     = 设备实机观感（设备就是世界 1x → 屏 2x nearest）。视差层按真实机位算，与设备一致；
 *     整图预览里的视差层是用"整图中心相机"烘的，两者可能差 ≤ rx%（卡片里有说明）。
 *
 * 服务端为主口径：坐标落 data/camera-positions.json（重新打开页面自动回填）；
 * 设备 NVS 只是断网辅助。下发走 POST /admin/devices/{id}/command {type:"cam",value:"x,y"}。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import {
  NAlert, NButton, NCard, NEmpty, NInputNumber, NPopconfirm, NSelect, NSpace, NSpin, NTag, NTooltip,
  useDialog, useMessage,
} from 'naive-ui'
import {
  cameraPreviewUrl, cameraViewportUrl, deleteCameraMap, errText, getCameraMaps, saveCameraPosition, sendCameraCommand,
} from '../api/client'
import { fmtTime } from '../utils/format'

const props = defineProps({ deviceId: { type: String, required: true } })
const message = useMessage()
// 删除正在使用的地图（服务端 409）时用对话框做二次确认（App.vue 已挂 NDialogProvider）
const dialog = useDialog()

// ── 状态 ────────────────────────────────────────────────────────────────
const loading = ref(false)
const loadError = ref('')
const unsupported = ref(false)      // 服务端没这组端点（旧部署实例）→ 卡片给可读提示
const maps = ref([])
const mapId = ref('')
const lastMapId = ref('')           // 服务端口径的"当前图"（最后设过机位/最后推送切图的那张）
const x = ref(0)
const y = ref(0)
const busy = ref('')                // '' | 'save' | 'apply'
const deleting = ref('')            // 正在删除的 mapId（按钮 loading）
const savedAt = ref(null)           // 服务端已记录时间（当前图）
const lastSeq = ref(null)
const statusText = ref('')

const map = computed(() => maps.value.find((m) => m.mapId === mapId.value) || null)
const winW = computed(() => map.value?.winW ?? 240)
const winH = computed(() => map.value?.winH ?? 240)
const maxX = computed(() => map.value?.maxX ?? 0)
const maxY = computed(() => map.value?.maxY ?? 0)
const pannable = computed(() => !!map.value?.pannable)

const selectOptions = computed(() => maps.value.map((m) => ({
  label: `${m.label} [${m.mapId}] · ${m.vw}×${m.vh}`
    + (m.pannable ? '' : '（窗口包·不可平移）')
    + (m.saved ? ` · 已记录 ${m.x},${m.y}` : ''),
  value: m.mapId,
})))

// ── 加载清单（页面打开/刷新）─────────────────────────────────────────────
async function load(keepMap = true) {
  loading.value = true
  loadError.value = ''
  try {
    const data = await getCameraMaps(props.deviceId)
    maps.value = data?.maps ?? []
    lastMapId.value = data?.lastMapId ?? ''
    unsupported.value = false
    if (!maps.value.length) {
      mapId.value = ''
      return
    }
    // 选图优先级：当前已选（刷新时保持）→ 服务端 lastMapId（上次操作过的图）→ 第一张
    const want = (keepMap && maps.value.some((m) => m.mapId === mapId.value)) ? mapId.value
      : (maps.value.some((m) => m.mapId === data?.lastMapId) ? data.lastMapId : maps.value[0].mapId)
    selectMap(want)
  } catch (e) {
    const st = e?.response?.status
    if (st === 404 || st === 405 || st === 501) {
      unsupported.value = true
      loadError.value = `服务端未实现 GET /api/admin/devices/{id}/camera/maps（HTTP ${st}，旧部署实例）`
    } else {
      loadError.value = errText(e)
    }
  } finally {
    loading.value = false
  }
}

/** 选图：回填服务端已记录机位；没记录则给一个"能看出内容"的默认机位（地图中心）。 */
function selectMap(id) {
  mapId.value = id
  const m = maps.value.find((mm) => mm.mapId === id)
  if (!m) return
  savedAt.value = m.updatedUtc ?? null
  if (m.saved && m.x != null && m.y != null) {
    x.value = m.x
    y.value = m.y
  } else {
    x.value = Math.round((m.maxX ?? 0) / 2)
    y.value = Math.round((m.maxY ?? 0) / 2)
  }
  lastSeq.value = null
  statusText.value = m.saved ? '' : '该图还没有记录过机位（当前为默认：地图中心）'
  refreshViewport(true)
}

// ── 取景框几何：世界坐标 ↔ 预览图显示像素 ────────────────────────────────
const stageRef = ref(null)
const imgRef = ref(null)
const imgW = ref(0)      // 预览图**显示**宽度（CSS px）；世界 → 显示比例 = imgW / vw
const imgLoaded = ref(false)

/** 世界 → 显示 比例（预览图等比缩放到容器内，用实测显示宽算，避免取整误差）。 */
const scale = computed(() => (map.value && map.value.vw > 0 && imgW.value > 0 ? imgW.value / map.value.vw : 0))

const boxStyle = computed(() => {
  const s = scale.value
  if (!s) return { display: 'none' }
  return {
    left: `${x.value * s}px`,
    top: `${y.value * s}px`,
    width: `${Math.min(winW.value, map.value?.vw ?? winW.value) * s}px`,
    height: `${Math.min(winH.value, map.value?.vh ?? winH.value) * s}px`,
  }
})

function measure() {
  const el = imgRef.value
  if (!el) return
  const w = el.getBoundingClientRect().width
  if (w > 0) imgW.value = w
}

function refreshViewport(immediate = false) {
  clearTimeout(vpTimer)
  const run = () => { if (map.value) viewportUrl.value = cameraViewportUrl(props.deviceId, map.value.mapId, x.value, y.value, map.value.hash) }
  if (immediate) run()
  else vpTimer = setTimeout(run, 150)   // 拖动时别把渲染请求打爆（服务端有内存缓存兜底）
}

// 设备视角图：先用 Image 预载成功再换 src，避免拖动时闪白
const viewportUrl = ref('')
let vpTimer = null
const viewportImgOk = ref(true)
watch(viewportUrl, (url) => {
  if (!url) return
  const im = new Image()
  im.onload = () => { viewportImgOk.value = true }
  im.onerror = () => { viewportImgOk.value = false }
  im.src = url
})

// ── 拖动 / 键盘 / 输入 ──────────────────────────────────────────────────
const dragging = ref(false)
let grabDx = 0
let grabDy = 0

function clampX(v) { return Math.max(0, Math.min(maxX.value, Math.round(v))) }
function clampY(v) { return Math.max(0, Math.min(maxY.value, Math.round(v))) }

function setXY(nx, ny) {
  x.value = clampX(nx)
  y.value = clampY(ny)
  refreshViewport()
}
watch([x, y], () => refreshViewport())

/** 指针位置 → 世界坐标（相对预览图左上角）。 */
function pointerToWorld(ev) {
  const img = imgRef.value
  if (!img || !scale.value) return null
  const r = img.getBoundingClientRect()
  return { wx: (ev.clientX - r.left) / scale.value, wy: (ev.clientY - r.top) / scale.value }
}

function onStageDown(ev) {
  if (!map.value || !pannable.value) return
  const p = pointerToWorld(ev)
  if (!p) return
  ev.preventDefault()
  // preventDefault 会连带取消"点击聚焦"这个默认行为 → 方向键微调会永远收不到事件。
  // 所以手动把焦点给舞台（tabindex=0），键盘微调才真正可用（浏览器实测踩过）。
  stageRef.value?.focus?.({ preventScroll: true })
  const onBox = ev.target?.classList?.contains('cam-box')
  if (onBox) {
    // 抓住框内任意点：保持抓取偏移，拖动手感跟手
    grabDx = p.wx - x.value
    grabDy = p.wy - y.value
  } else {
    // 点图面任意处 = 把框中心挪到该点
    grabDx = winW.value / 2
    grabDy = winH.value / 2
  }
  dragging.value = true
  setXY(p.wx - grabDx, p.wy - grabDy)
  stageRef.value?.setPointerCapture?.(ev.pointerId)
}

function onStageMove(ev) {
  if (!dragging.value) return
  const p = pointerToWorld(ev)
  if (!p) return
  setXY(p.wx - grabDx, p.wy - grabDy)
}

function onStageUp(ev) {
  if (!dragging.value) return
  dragging.value = false
  stageRef.value?.releasePointerCapture?.(ev.pointerId)
  refreshViewport(true)
}

/** 方向键微调 1px，Shift = 10px（手指拖不准时的精修；也是"直接输入 x,y"的补充）。 */
function onKeydown(ev) {
  if (!map.value) return
  const step = ev.shiftKey ? 10 : 1
  const map1 = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] }
  const d = map1[ev.key]
  if (!d) return
  ev.preventDefault()
  setXY(x.value + d[0], y.value + d[1])
}

function centerCamera() {
  setXY(maxX.value / 2, maxY.value / 2)
}

// ── 保存 / 下发 ─────────────────────────────────────────────────────────
async function doSave(silent = false) {
  busy.value = 'save'
  try {
    const r = await saveCameraPosition(props.deviceId, mapId.value, x.value, y.value)
    // 以服务端夹取后的返回值为准（不乐观看待自己发出去的数）
    x.value = r?.x ?? x.value
    y.value = r?.y ?? y.value
    savedAt.value = r?.updatedUtc ?? null
    statusText.value = r?.clamped
      ? `服务端按图尺寸夹取：请求 ${r?.requested?.x},${r?.requested?.y} → 记录 ${r?.x},${r?.y}`
      : `已记录到服务端（${fmtTime(r?.updatedUtc)}）`
    if (!silent) message.success(`机位已记录：${r?.x},${r?.y}（服务端为主口径）`)
    const m = maps.value.find((mm) => mm.mapId === mapId.value)
    if (m) { m.saved = true; m.x = r?.x; m.y = r?.y; m.updatedUtc = r?.updatedUtc }
    return true
  } catch (e) {
    message.error(e?.serverError || '机位记录失败')
    return false
  } finally {
    busy.value = ''
  }
}

async function doApply(saveFirst = false) {
  if (saveFirst && !(await doSave(true))) return
  busy.value = 'apply'
  try {
    // 带上当前选中的图：否则服务端只能回退 lastMapId，可能把机位应用到别的图
    const r = await sendCameraCommand(props.deviceId, x.value, y.value, mapId.value)
    lastSeq.value = r?.seq ?? null
    statusText.value = `已下发设备：cam=${r?.value ?? `${x.value},${y.value}`}`
      + (r?.seq != null ? `（seq ${r.seq}）` : '')
    message.success(`已下发设备（seq ${r?.seq ?? '?'}）：设备数秒内切到该机位`)
  } catch (e) {
    const st = e?.response?.status
    if (st === 400) {
      message.error(`${e?.serverError || 'cam 指令被拒'} —— 服务端可能还是旧镜像（command 白名单没有 cam）`)
    } else {
      message.error(errText(e))
    }
  } finally {
    busy.value = ''
  }
}

/**
 * 删除一张地图（从该设备素材清单移除）。
 *
 * 服务端 DELETE …/camera/maps/{mapId}：摘掉该图 BGMAP 主条目 + 仅它引用的派生素材
 * （条带 PARTS / 缩略图，引用计数保护共享素材）→ BumpRev → 设备下次同步对账剪除本地条目。
 * 失败路径全中文：409 = 服务端判定这张图正被设备使用（口径见端点注释），直接把服务端
 * 返回的原因吐给用户，不自己造文案；用户若确实要删，再走一次二次确认带 force=true
 * （留给"清单里只剩这一张、又不想要它"的场景：设备回落到其它图，清单空了背景为黑）。
 * 成功后重新拉清单（load）——面板上的张数/下拉选项/剩余地图都要看到删掉后的结果。
 *
 * @param {object} m 地图条目（GET …/camera/maps 的 maps[i]）
 * @param {boolean} force 已确认要强制删除正在使用的图
 */
async function doDelete(m, force = false) {
  if (!m || deleting.value) return
  deleting.value = m.mapId
  try {
    const r = await deleteCameraMap(props.deviceId, m.mapId, force)
    const left = r?.remainingMaps ?? 0
    if (r?.removed) {
      /* 【2026-10-02 用户口径"页面上删除地图要把设备的资源删掉"】服务端现在连**包文件**
       * 一起物理删除（导出目录），设备下次同步对账时也会删掉自己 TF 上的同名包 ——
       * 文案把释放量说清楚，别让人以为只摘了索引。 */
      const freed = r?.bytesFreed ? `，服务端已删除 ${r.filesDeleted ?? 0} 个包文件、释放 ${(r.bytesFreed / 1048576).toFixed(1)}MB` : ''
      message.success(
        `已删除「${m.label}」：移除 BGMAP 主包 + ${r?.removedCount ?? 0} 个专用素材${freed}，`
        + `清单还剩 ${left} 张图（设备下次同步时对账并删除本地包）`,
        { duration: 8000 }
      )
    } else {
      message.info(`「${m.label}」本来就不在清单里（幂等）：清单还剩 ${left} 张图`)
    }
    if (r?.keptCount) {
      message.warning(`有 ${r.keptCount} 个候选素材因仍被其它条目引用而保留（共享内容，不删）`, { duration: 6000 })
    }
    if (r?.warning) message.warning(r.warning, { duration: 8000 })
    await load(true)     // 刷新列表（保持当前选图，若被删则回落到 lastMapId/首张）
  } catch (e) {
    const st = e?.response?.status
    if (st === 409 && !force) {
      // 服务端判定"正在使用"：把中文原因原样给用户，并给一条明确的强制出口（二次确认）
      dialog.warning({
        title: '这张图正被设备使用',
        content: `${e?.serverError || '服务端拒绝删除'}\n\n仍要强制删除「${m.label}」吗？`
          + '设备会回落到清单里的其它图；清单里若没有别的图，背景会是黑的。',
        positiveText: '仍要删除（强制）',
        negativeText: '取消',
        onPositiveClick: () => { doDelete(m, true) },
      })
    } else {
      message.error(errText(e), { duration: 9000 })
    }
    console.warn(`[camera] 删除地图 ${m.mapId} 失败:`, st, e?.serverError)
  } finally {
    deleting.value = ''
  }
}

// 预览图随窗口尺寸变化重新量（取景框位置按显示宽度换算）
let ro = null
onMounted(() => {
  load(false)
  window.addEventListener('resize', measure)
  ro = typeof ResizeObserver !== 'undefined' ? new ResizeObserver(measure) : null
  if (ro && imgRef.value) ro.observe(imgRef.value)
})
onUnmounted(() => {
  window.removeEventListener('resize', measure)
  ro?.disconnect()
  clearTimeout(vpTimer)
})

function onPreviewLoad() {
  imgLoaded.value = true
  measure()
  if (ro && imgRef.value) ro.observe(imgRef.value)
}

/** 换图时预览图重载（宽高比变了 → 重新量）。 */
watch(mapId, () => { imgLoaded.value = false; imgW.value = 0 })
</script>

<template>
  <n-card title="选镜头（服务端选相机机位）" size="small">
    <template #header-extra>
      <n-space align="center" :size="8">
        <n-tag v-if="map" size="small" :bordered="false" :type="map.viewport === 'full' ? 'success' : 'warning'">
          {{ map.viewport === 'full' ? '整图包（可平移）' : '窗口包（不可平移）' }}
        </n-tag>
        <n-tag v-if="map" size="small" :bordered="false">布局 {{ map.layout }}</n-tag>
        <n-button size="tiny" secondary :loading="loading" @click="load(true)">刷新</n-button>
      </n-space>
    </template>

    <n-spin v-if="loading" size="small" class="cam-center" />

    <n-alert v-else-if="unsupported" type="warning" :show-icon="false" size="small">
      {{ loadError }}<br>
      需要的端点：<code>GET /api/admin/devices/{id}/camera/maps</code>、
      <code>GET …/camera/maps/{mapId}/preview</code>、<code>GET …/camera/maps/{mapId}/viewport</code>、
      <code>PUT …/camera</code>（服务端 CameraEndpoints.cs）。重新构建部署 Server 后本卡片自动可用。
    </n-alert>

    <n-alert v-else-if="loadError" type="error" :show-icon="false" size="small">{{ loadError }}</n-alert>

    <n-empty
      v-else-if="!maps.length"
      size="small"
      description="该设备还没有 BGMAP 地图 —— 先到下面「素材推送」推一张地图（缺省就是整图口径，推完这里会出现）"
      class="cam-center"
    />

    <n-space v-else vertical :size="10">
      <n-space align="center" :size="8" style="width: 100%">
        <n-select
          v-model:value="mapId"
          size="small"
          style="min-width: 340px; flex: 1; max-width: 640px"
          :options="selectOptions"
          :disabled="!!busy"
          placeholder="选择该设备上的地图"
          @update:value="selectMap"
        />
        <span class="hint">
          共 {{ maps.length }} 张（清单来自设备 manifest 的 BGMAP 条目，尺寸读 BGMAP 包头）
        </span>
      </n-space>

      <!-- 地图清单（每行一个「删除」）：删 = 从该设备 manifest 摘掉这张图的 BGMAP 主包 +
           仅它引用的派生素材（条带 PARTS / 缩略图），并 BumpRev 让设备下次同步剪除本地条目
           （设备菜单里随之消失）。"当前使用"的那行标出来、按钮照样可点：由服务端判定并回中文
           原因（409），页面原样展示；确实要删可在随后对话框里二次确认强制删除。 -->
      <div class="map-manage">
        <div class="map-manage-head">
          <span class="map-manage-title">地图清单（{{ maps.length }} 张）</span>
          <span class="hint">
            删除 = 从设备清单移除该地图及其专用素材，<b>设备下次同步后生效</b>
            （服务端只摘清单条目，磁盘包保留，设备侧按清单对账剪除）
          </span>
        </div>
        <div class="map-rows">
          <div
            v-for="m in maps"
            :key="m.mapId"
            class="map-row"
            :class="{ current: m.mapId === lastMapId }"
          >
            <img
              v-if="m.thumbUrl"
              class="map-row-thumb"
              :src="m.thumbUrl"
              alt=""
              @error="(ev) => { ev.target.style.visibility = 'hidden' }"
            >
            <div class="map-row-main">
              <div class="map-row-name">
                {{ m.label }}<span class="map-row-id">{{ m.mapId }}</span>
              </div>
              <div class="hint">
                {{ m.vw }}×{{ m.vh }} ·
                {{ m.viewport === 'full' ? '整图包' : '窗口包' }} ·
                布局 {{ m.layout }} ·
                {{ m.saved ? `机位 ${m.x},${m.y}` : '未记录机位' }}
                · {{ Math.round((m.bytes ?? 0) / 1024) }} KB
              </div>
            </div>
            <n-tag v-if="m.mapId === lastMapId" size="small" type="warning" :bordered="false">当前使用</n-tag>
            <!-- 每行一个删除按钮：当前使用的那张**照样可点**——由服务端判定并给中文原因
                 （409），页面再把原因显示出来（要求：删除失败要显示服务端返回的中文原因）；
                 用户若确实要删，随后的对话框里可以二次确认强制删除。 -->
            <n-popconfirm
              :positive-button-props="{ type: 'error', size: 'small' }"
              :negative-button-props="{ size: 'small' }"
              @positive-click="doDelete(m)"
            >
              <template #trigger>
                <n-button size="tiny" tertiary type="error" :loading="deleting === m.mapId" :disabled="!!deleting">
                  删除
                </n-button>
              </template>
              将从设备清单移除「{{ m.label }}」（{{ m.mapId }}）及其专用素材：<br>
              BGMAP 主包 + 只被它引用的条带 PARTS / 缩略图（纸娃娃、字体等共享素材不受影响）。<br>
              <b>设备下次同步（≤55s）后生效</b>，之后设备菜单里不再出现这张图。
              <span v-if="m.mapId === lastMapId">
                <br><b>注意：这张是设备「当前使用」的图</b>——服务端会拒绝并给出中文原因；
                确实要删时可在随后弹出的对话框里二次确认强制删除。
              </span>
            </n-popconfirm>
          </div>
        </div>
      </div>

      <n-alert v-if="map && !pannable" type="warning" :show-icon="false" size="small">
        该图是 <b>窗口包</b>（vw×vh = {{ map.vw }}×{{ map.vh }}，没有平移余量）：设备上无法平移相机。
        要用「选镜头」请先把这张图按<b>整图口径</b>重推一次（下面「素材推送」的 switch 保持勾选即可，
        缺省就是整图 + 分块）。此刻右侧显示的是设备实际那一屏（导出相机取景）。
      </n-alert>

      <div v-if="map" class="cam-layout">
        <!-- 左：整图预览 + 取景框（框外半黑 / 框内原色） -->
        <div class="cam-left">
          <div
            ref="stageRef"
            class="cam-stage"
            :class="{ dragging, disabled: !pannable }"
            tabindex="0"
            @pointerdown="onStageDown"
            @pointermove="onStageMove"
            @pointerup="onStageUp"
            @pointercancel="onStageUp"
            @keydown="onKeydown"
          >
            <img
              ref="imgRef"
              class="cam-img"
              :src="cameraPreviewUrl(deviceId, map.mapId)"
              alt="整图预览"
              draggable="false"
              @load="onPreviewLoad"
            >
            <!-- 取景框：自身原色显示，靠巨型 box-shadow 把**框外**压成半黑（舞台 overflow:hidden 裁掉） -->
            <div class="cam-box" :style="boxStyle">
              <span class="cam-box-label">{{ x }},{{ y }}</span>
              <span class="cam-cross" />
            </div>
            <div v-if="!imgLoaded" class="cam-img-loading">整图渲染中（首次要几秒，之后走磁盘缓存）…</div>
          </div>
          <div class="hint cam-tip">
            <b>框外半黑 = 取景框的巨型投影</b>（<code>box-shadow: 0 0 0 9999px rgba(0,0,0,.62)</code> +
            舞台 <code>overflow:hidden</code>）：亮框 = 设备可见的 {{ winW }}×{{ winH }} 世界像素。
            操作：<b>拖动框</b> / <b>点图面任意处</b>把框挪过去 / <b>方向键</b>微调 1px（Shift=10px）/
            右侧直接输入 x,y。框内是整图参考渲染的**原始像素**（相机=地图中心，零缩放），
            与右侧「设备视角」是同一坐标系的两种口径。
          </div>
        </div>

        <!-- 右：设备视角（240×240 1x → 页面 2x 就近放大 = 实机观感）+ 坐标与按钮 -->
        <div class="cam-right">
          <div class="cam-vp-title">
            设备视角 <span class="hint">（{{ winW }}×{{ winH }} 世界像素 ×2 就近放大 = 实机观感）</span>
          </div>
          <div class="cam-vp-frame">
            <img
              v-if="viewportUrl && viewportImgOk"
              class="cam-vp-img"
              :src="viewportUrl"
              :width="winW * 2"
              :height="winH * 2"
              alt="设备视角"
            >
            <div v-else class="cam-vp-empty hint">渲染中…</div>
          </div>

          <n-space align="center" :size="8" class="cam-coord">
            <span class="cam-coord-label">x</span>
            <n-input-number
              v-model:value="x"
              size="small"
              style="width: 96px"
              :min="0"
              :max="maxX"
              :disabled="!!busy"
              @update:value="(v) => setXY(v ?? 0, y)"
            />
            <span class="cam-coord-label">y</span>
            <n-input-number
              v-model:value="y"
              size="small"
              style="width: 96px"
              :min="0"
              :max="maxY"
              :disabled="!!busy"
              @update:value="(v) => setXY(x, v ?? 0)"
            />
            <n-button size="tiny" secondary :disabled="!!busy" @click="centerCamera">居中</n-button>
          </n-space>

          <div class="hint cam-range">
            设备可见范围 <code>dx[0,{{ maxX }}]</code> <code>dy[0,{{ maxY }}]</code>
            （= vw−{{ winW }} / vh−{{ winH }}；图 {{ map.vw }}×{{ map.vh }}，机位越界服务端与设备都会夹取）
          </div>

          <n-space align="center" :size="8" class="cam-actions">
            <n-button size="small" :loading="busy === 'save'" :disabled="!!busy" @click="doSave()">保存坐标</n-button>
            <n-button
              size="small"
              type="primary"
              :loading="busy === 'apply'"
              :disabled="!!busy"
              @click="doApply(false)"
            >
              上送设备
            </n-button>
            <n-button size="small" type="primary" secondary :loading="!!busy" :disabled="!!busy" @click="doApply(true)">
              保存并上送
            </n-button>
          </n-space>

          <div class="hint cam-status">
            <div v-if="savedAt">服务端已记录于 {{ fmtTime(savedAt) }}（<code>data/camera-positions.json</code>，为主口径）</div>
            <div v-else>服务端尚无该图机位记录（保存后即可回填）</div>
            <div v-if="lastSeq != null">最近下发：<code>cam={{ x }},{{ y }}</code> · seq {{ lastSeq }}</div>
            <div v-if="statusText">{{ statusText }}</div>
            <div class="cam-note">
              下发口径：<code>POST /api/admin/devices/{{ deviceId }}/command</code>
              <code>{"type":"cam","value":"{{ x }},{{ y }}"}</code>
              → 服务端转固件旧口径 <code>{"t":"cam","v":"{{ x }},{{ y }}"}</code> → 设备
              <code>render_cam_set(x,y)</code> 并写 NVS（服务端是主口径，设备本地只是断网辅助）。
            </div>
            <n-tooltip trigger="hover">
              <template #trigger>
                <span class="cam-note-dot">口径说明</span>
              </template>
              左侧整图预览的视差背景层是按"整图中心相机"烘的；右侧设备视角是按当前机位重渲的，
              含正确的视差偏移（rx≠0 的层可能有 ≤rx% 的差异）。设备端最终画面以右侧为准。
            </n-tooltip>
          </div>
        </div>
      </div>
    </n-space>
  </n-card>
</template>

<style scoped>
.cam-center { display: flex; justify-content: center; padding: 16px 0; }
.hint { font-size: 12px; opacity: 0.62; }
.cam-layout { display: flex; flex-wrap: wrap; gap: 14px; align-items: flex-start; }
.cam-left { flex: 1 1 420px; min-width: 320px; }
.cam-right { flex: 0 0 500px; max-width: 100%; display: flex; flex-direction: column; gap: 8px; }

/* 舞台：内联块包裹图片，overflow:hidden 负责裁掉取景框铺满全场的半黑投影 */
.cam-stage {
  position: relative;
  display: inline-block;
  max-width: 100%;
  line-height: 0;
  overflow: hidden;
  border-radius: 8px;
  background: #14161c;   /* 地图透明区在深底上更易分辨"图外" */
  outline: none;
  touch-action: none;
  user-select: none;
  cursor: crosshair;
}
.cam-stage.dragging { cursor: grabbing; }
.cam-stage.disabled { cursor: not-allowed; }
.cam-stage:focus-visible { box-shadow: 0 0 0 2px #ffd166 inset; }
.cam-img { display: block; max-width: 100%; height: auto; -webkit-user-drag: none; }

/* 取景框：框内保持原色（不被任何遮罩盖住），框外靠这层 9999px 投影压成半黑 */
.cam-box {
  position: absolute;
  box-sizing: border-box;
  border: 2px solid #ffd166;
  box-shadow: 0 0 0 9999px rgba(0, 0, 0, 0.62), 0 0 8px rgba(0, 0, 0, 0.6);
  cursor: grab;
}
.cam-stage.dragging .cam-box { cursor: grabbing; }
.cam-box-label {
  position: absolute;
  top: 0;
  left: 0;
  padding: 1px 6px;
  font-size: 11px;
  line-height: 16px;
  color: #ffd166;
  background: rgba(20, 22, 28, 0.72);
  border-radius: 0 0 4px 0;
  white-space: nowrap;
  pointer-events: none;
}
.cam-cross,
.cam-cross::before {
  position: absolute;
  background: rgba(255, 209, 102, 0.75);
  content: '';
}
.cam-cross { left: 50%; top: 50%; width: 12px; height: 1px; margin-left: -6px; }
.cam-cross::before { left: 50%; top: -6px; width: 1px; height: 12px; margin-left: -0.5px; }
.cam-img-loading {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 12px;
  color: #d8dee9;
  line-height: 1.6;
}
.cam-tip { margin-top: 8px; line-height: 1.75; }
.cam-tip code, .cam-note code, .cam-range code { background: rgba(128, 128, 140, 0.16); padding: 0 3px; border-radius: 3px; }

.cam-vp-title { font-size: 13px; font-weight: 600; }

.cam-vp-frame {
  position: relative;
  width: 100%;
  min-height: 120px;
  display: flex;
  align-items: center;
  justify-content: center;
  background: #14161c;
  border-radius: 8px;
  padding: 6px;
}
/* 设备是 2x 最近邻展开：这里也必须 pixelated，否则看到的不是实机观感 */
.cam-vp-img { image-rendering: pixelated; display: block; max-width: 100%; height: auto; }
.cam-vp-empty { padding: 40px 0; }

.cam-coord { margin-top: 2px; }
.cam-coord-label { font-size: 12px; opacity: 0.7; width: 12px; }

/* 地图清单（删除入口）：行 = 缩略图 + 名称/尺寸口径 + 当前使用标记 + 删除按钮 */
.map-manage { border: 1px solid rgba(128, 128, 140, 0.22); border-radius: 8px; padding: 8px 10px; }
.map-manage-head { display: flex; flex-wrap: wrap; align-items: baseline; gap: 10px; margin-bottom: 6px; }
.map-manage-title { font-size: 13px; font-weight: 600; }
.map-rows { max-height: 260px; overflow: auto; }
.map-row {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 6px 4px;
  border-top: 1px solid rgba(128, 128, 140, 0.14);
}
.map-row:first-child { border-top: none; }
.map-row.current { background: rgba(255, 209, 102, 0.08); border-radius: 6px; }
.map-row-thumb {
  width: 40px;
  height: 40px;
  flex: 0 0 40px;
  object-fit: cover;
  border-radius: 6px;
  background: #14161c;
  image-rendering: pixelated;
}
.map-row-main { flex: 1 1 auto; min-width: 0; }
.map-row-name { font-size: 13px; line-height: 1.5; }
.map-row-id { margin-left: 8px; font-size: 12px; opacity: 0.6; font-family: ui-monospace, monospace; }

.cam-range { line-height: 1.7; }
.cam-actions { margin-top: 2px; }
.cam-status { line-height: 1.7; }
.cam-note { margin-top: 4px; opacity: 0.85; word-break: break-all; }
.cam-note-dot { text-decoration: underline dotted; cursor: help; }
</style>
