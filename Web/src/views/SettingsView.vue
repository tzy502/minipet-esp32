<script setup>
/**
 * 设置页（E3/E4）：WZ 路径（校验）/ 阈值（IMU 死区·灵敏度·轻拍·重拍·待机分钟）/
 * 随机台词气泡（E12）/ 地图时钟坐标表（高级，JSON 编辑）→ PUT /admin/settings 全量同构回传。
 * 端口只读展示（部署层 .env 的 MINIPET_PORT 管理，R2 定稿）。
 *
 * 字段在位实况（2026-09-27 真实实例 `GET /api/admin/settings` 实测）：
 *   · `config.device.imuSensitivity = 1` **在位**（ConfigService.cs:58-73 DeviceThresholdsConfig）；
 *   · `config.speech = {enabled:false, idleSec:300, lines:[]}` **在位**，且
 *     `ConfigService.Replace` 显式搬运 `c.Speech = incoming.Speech ?? new SpeechConfig()`；
 *   · 设备指令端点 `POST /api/admin/devices/{id}/command` **已上线**（实测 202 {"ok":true,"seq":47,…}），
 *     E12 的 bubble 入队通道与 `Services/SpeechScheduler.cs`（静置调度）服务端均已具备。
 * 两处仍保留**运行时探测**（`config.device.imuSensitivity != null` / `config.speech != null`）：
 * 探测不到只可能是老部署实例 → 退回禁用占位并贴出需求，绝不硬塞发不出去的字段。
 * 接口需求历史记录见 Web/docs/interfaces-needed-from-server.md（§T4/§T5 已标记服务端已实现）。
 */
import { computed, onMounted, reactive, ref } from 'vue'
import {
  NCard, NForm, NFormItem, NInput, NInputNumber, NButton, NTag, NSpace, NSwitch,
  NSpin, NResult, NAlert, NTooltip, NCollapse, NCollapseItem, useMessage,
} from 'naive-ui'
import { useSettingsStore } from '../stores/settings'
import { BUBBLE_MAX_BYTES, bubbleByteLength } from '../utils/expressions'

const message = useMessage()
const store = useSettingsStore()

const form = reactive({
  dataPath: '',
  imuDeadzoneDeg: 8,
  tapLightG: 2,
  tapHardG: 4,
  idleToClockMin: 5,
  imuSensitivity: 1,
})
const mapOffsetsText = ref('{}')
const mapOffsetsError = ref('')
const saving = ref(false)

// ── T4/T5 字段存在性探测（服务端模型有没有这些字段，决定启用还是占位）─────────
// 实测两份服务端都已带这些字段（见文件头注释）；探测只为兼容老部署实例。
const imuSensitivitySupported = computed(() => store.config?.device?.imuSensitivity != null)
const speechSupported = computed(() => store.config?.speech != null)
// 默认值与服务端 SpeechConfig 对齐（enabled=false / idleSec=300）——探测失败时不会被误保存
const speech = reactive({ enabled: false, idleSec: 300 })
const speechLinesText = ref('')
const speechLinesError = ref('')

/** 台词逐行校验：非空行数 ≤ 50，单行 UTF-8 ≤ 95 字节（固件 mp_cmd_t.s = char[96]）。 */
function parseSpeechLines() {
  const lines = speechLinesText.value
    .split('\n')
    .map((s) => s.trim())
    .filter((s) => s.length > 0)
  if (lines.length > 50) return { ok: false, error: `台词条数 ${lines.length} 条，超过上限 50 条` }
  for (const l of lines) {
    const bytes = bubbleByteLength(l)
    if (bytes > BUBBLE_MAX_BYTES) {
      return { ok: false, error: `「${l}」${bytes} 字节，超出固件单条上限 ${BUBBLE_MAX_BYTES} 字节（≈31 汉字）` }
    }
  }
  return { ok: true, value: lines }
}
const speechBytesHint = computed(() => {
  const p = parseSpeechLines()
  return p.ok ? `共 ${p.value.length} 条` : p.error
})

const loaded = computed(() => !!store.config)
const wzTagType = computed(() => (store.wzPathExists ? 'success' : 'error'))
const wzTagText = computed(() => (store.wzPathExists ? '当前配置的 WZ 路径存在' : '当前配置的 WZ 路径不存在'))

const validateTag = computed(() => {
  const v = store.validate
  switch (v.status) {
    case 'checking': return { type: 'info', text: '校验中…' }
    case 'ok': return { type: 'success', text: v.message || '路径存在' }
    case 'fail': return { type: 'error', text: v.message || '校验失败' }
    case 'unsupported': return { type: 'warning', text: v.message }
    default: return null
  }
})

async function initialLoad() {
  try {
    await store.load()
    const c = store.config
    if (c) {
      form.dataPath = c.wz?.dataPath ?? ''
      form.imuDeadzoneDeg = c.device?.imuDeadzoneDeg ?? 8
      form.tapLightG = c.device?.tapLightG ?? 2
      form.tapHardG = c.device?.tapHardG ?? 4
      form.idleToClockMin = c.device?.idleToClockMin ?? 5
      // T4：探测到才回填（否则保持占位默认值 1.0，不参与下发）
      form.imuSensitivity = c.device?.imuSensitivity ?? 1
      // T5：台词段存在才回填（服务端实测在位）
      if (c.speech) {
        speech.enabled = c.speech.enabled ?? false
        speech.idleSec = c.speech.idleSec ?? 300
        speechLinesText.value = Array.isArray(c.speech.lines) ? c.speech.lines.join('\n') : ''
      }
      speechLinesError.value = ''
      mapOffsetsText.value = JSON.stringify(c.clock?.mapOffsets ?? {}, null, 2)
      mapOffsetsError.value = ''
    }
  } catch {
    /* store.error 已置，NResult 兜底 */
  }
}
onMounted(initialLoad)

async function onValidate() {
  if (!form.dataPath.trim()) {
    message.warning('请先填写 WZ 路径')
    return
  }
  try {
    await store.validatePath(form.dataPath.trim())
  } catch {
    /* validate.status 已置 fail */
  }
}

function parseMapOffsets() {
  try {
    const obj = JSON.parse(mapOffsetsText.value || '{}')
    if (obj === null || typeof obj !== 'object' || Array.isArray(obj)) throw new Error('顶层必须是对象')
    for (const [k, v] of Object.entries(obj)) {
      if (!Array.isArray(v) || v.length !== 2 || !v.every((n) => Number.isFinite(n))) {
        throw new Error(`「${k}」的值必须是 [x, y] 双元素数组`)
      }
    }
    return { ok: true, value: obj }
  } catch (e) {
    return { ok: false, error: `地图时钟坐标 JSON 不合法：${e.message}` }
  }
}

async function onSave() {
  const mo = parseMapOffsets()
  mapOffsetsError.value = mo.ok ? '' : mo.error
  if (!mo.ok) {
    message.error(mo.error)
    return
  }
  if (!form.dataPath.trim()) {
    message.error('WZ 路径不能为空')
    return
  }
  const sl = parseSpeechLines()
  speechLinesError.value = sl.ok ? '' : sl.error
  if (speechSupported.value && !sl.ok) {
    message.error(sl.error)
    return
  }

  // 同构回传：未在页面编辑的段（qqMusic/bgm/_comment）原样带回，不丢字段
  const next = JSON.parse(JSON.stringify(store.config))
  next.wz.dataPath = form.dataPath.trim()
  next.device = {
    ...next.device,
    imuDeadzoneDeg: Number(form.imuDeadzoneDeg),
    tapLightG: Number(form.tapLightG),
    tapHardG: Number(form.tapHardG),
    idleToClockMin: Number(form.idleToClockMin),
  }
  // T4：服务端模型无 ImuSensitivity 字段时不带该 key（带了也会被 Replace 静默丢弃）
  if (imuSensitivitySupported.value) next.device.imuSensitivity = Number(form.imuSensitivity)
  // 探测不到 speech 段（老部署实例）时不带该 key：同 ImuSensitivity，不硬塞发不出去的字段
  if (speechSupported.value) {
    next.speech = { ...next.speech, enabled: speech.enabled, idleSec: Number(speech.idleSec), lines: sl.value }
  }
  next.clock = { ...next.clock, mapOffsets: mo.value }

  saving.value = true
  try {
    const r = await store.save(next)
    message.success(
      r?.wzPathExists
        ? '设置已保存（WZ 路径校验通过，服务端热重载生效）'
        : '设置已保存（注意：WZ 路径当前不存在）'
    )
  } catch (e) {
    const errs = e?.response?.data?.errors
    message.error(errs?.length ? errs.join('；') : e?.serverError || '保存失败')
  } finally {
    saving.value = false
  }
}
</script>

<template>
  <div>
    <n-spin v-if="store.loading" class="block-center" />

    <n-result
      v-else-if="store.error && !loaded"
      status="error"
      title="设置加载失败"
      :description="store.error"
    >
      <template #footer>
        <n-button type="primary" @click="initialLoad">重试</n-button>
      </template>
    </n-result>

    <template v-else-if="loaded">
      <n-alert v-if="store.error" type="warning" closable class="mb12">{{ store.error }}</n-alert>

      <n-space vertical :size="12">
        <n-card title="WZ 数据" size="small">
          <n-form label-placement="top">
            <n-form-item label="WZ Data 目录路径（容器内默认 /wz/Data，开源用户自定义）" required>
              <n-space align="center" style="width: 100%" item-style="flex:1">
                <n-input v-model:value="form.dataPath" placeholder="/wz/Data" :disabled="saving" />
                <n-button secondary :loading="store.validate.status === 'checking'" @click="onValidate">校验</n-button>
                <n-tag v-if="validateTag" :bordered="false" :type="validateTag.type">{{ validateTag.text }}</n-tag>
                <n-tag v-else :bordered="false" :type="wzTagType">{{ wzTagText }}</n-tag>
              </n-space>
            </n-form-item>
            <n-form-item label="服务端口（只读）">
              <n-tooltip trigger="hover" placement="top-start">
                <template #trigger>
                  <n-input value="由 .env 管理（MINIPET_PORT，默认 38090）" disabled style="max-width: 380px" />
                </template>
                端口属部署层，只在 .env 管理，不入 appsettings（E3 定稿）—— 修改端口请编辑部署机 .env 后重启容器
              </n-tooltip>
            </n-form-item>
          </n-form>
        </n-card>

        <n-card title="设备阈值（IMU / 力度分级 / 待机，E6）" size="small">
          <n-form label-placement="top">
            <div class="grid4">
              <n-form-item label="IMU 死区（±°，超阈值才算倾斜）">
                <n-input-number v-model:value="form.imuDeadzoneDeg" :min="0" :max="45" :step="0.5" :disabled="saving" style="width: 100%" />
              </n-form-item>
              <n-form-item label="轻拍阈值（g，低于算抚摸）">
                <n-input-number v-model:value="form.tapLightG" :min="0.5" :max="16" :step="0.1" :disabled="saving" style="width: 100%" />
              </n-form-item>
              <n-form-item label="重拍阈值（g，达到算 hit）">
                <n-input-number v-model:value="form.tapHardG" :min="0.5" :max="16" :step="0.1" :disabled="saving" style="width: 100%" />
              </n-form-item>
              <n-form-item label="待机转时钟（分钟，无交互后睡）">
                <n-input-number v-model:value="form.idleToClockMin" :min="1" :max="240" :step="1" :disabled="saving" style="width: 100%" />
              </n-form-item>
              <!-- IMU 灵敏度（E4）：服务端 DeviceThresholdsConfig.ImuSensitivity 已在上位并随 PUT 下发；
                   探测只为兼容老部署实例（探测不到 → 禁用占位，不硬塞发不出去的字段） -->
              <n-form-item>
                <template #label>
                  <n-tooltip trigger="hover" :disabled="imuSensitivitySupported">
                    <template #trigger>
                      <span>IMU 灵敏度（倍率，越大越灵敏）</span>
                    </template>
                    当前服务端未返回 device.imuSensitivity（老部署实例？）——此项暂不可下发，
                    接口需求见 Web/docs/interfaces-needed-from-server.md §T4
                  </n-tooltip>
                </template>
                <n-input-number
                  v-model:value="form.imuSensitivity"
                  :min="0.2"
                  :max="3"
                  :step="0.1"
                  :disabled="saving || !imuSensitivitySupported"
                  style="width: 100%"
                />
              </n-form-item>
            </div>
          </n-form>
          <n-alert v-if="!imuSensitivitySupported" type="info" :show-icon="false" size="small" class="mt8">
            <b>「IMU 灵敏度」为占位（禁用态）。</b>
            本次 <code>GET /api/admin/settings</code> 的响应里没有 <code>device.imuSensitivity</code>，
            故不下发该字段（避免被 <code>ConfigService.Replace</code> 静默丢弃）。
            该字段已由服务端实现在位（<code>DeviceThresholdsConfig.ImuSensitivity</code>，默认 1.0，Normalize 夹取 0.2–3.0），
            此处出现即说明连的是旧部署实例 —— 重启/更新服务端后本项自动启用。
            规格见 <code>Web/docs/interfaces-needed-from-server.md</code> §T4。
          </n-alert>
        </n-card>

        <!-- E12：随机台词气泡（文本 Web 配置）——服务端 speech 段 + SpeechScheduler 均已就位 -->
        <n-card title="随机台词气泡（E12，静置久了冒预设台词）" size="small">
          <template #header-extra>
            <n-tag size="small" :bordered="false" :type="speechSupported ? 'success' : 'warning'">
              {{ speechSupported ? '配置段在位（真实读写）' : '服务端未返回 speech 段' }}
            </n-tag>
          </template>
          <n-space vertical :size="10">
            <n-alert v-if="!speechSupported" type="info" :show-icon="false" size="small">
              <b>本次响应没有 <code>config.speech</code> 段（禁用态占位）。</b>
              服务端工作区已实现在位（<code>MinipetConfig.Speech</code> + <code>ConfigService.Replace</code> 显式搬运
              <code>c.Speech = incoming.Speech ?? new SpeechConfig()</code>，<code>SettingsView</code> 保存即真实往返），
              此处出现即说明连的是旧部署实例。仅当段缺失时才需要服务端支持：
              <div class="req">
                <div>① 配置段 <code>speech</code>：<code>{ "enabled": true, "idleSec": 300, "lines": ["…", "…"] }</code>（随 PUT /admin/settings 增删改）</div>
                <div>② 静置调度：<code>Services/SpeechScheduler.cs</code>（静置 ≥ idleSec → 随机挑一条 → <code>CommandQueue.Enqueue(id,"bubble",line)</code>，payload 为裸字符串）</div>
                <div>③ 手动测试端点：<code>POST /api/admin/devices/{id}/command { "type": "bubble", "value": "…" }</code>（已在位，实测 202）</div>
              </div>
              完整规格见 <code>Web/docs/interfaces-needed-from-server.md</code> §T5。
              固件侧已就绪：<code>poller.c</code> 消费 <code>bubble</code> 指令 → <code>MP_CMD_BUBBLE</code>。
            </n-alert>
            <n-form label-placement="top">
              <div class="grid4">
                <n-form-item label="启用静置台词">
                  <n-switch v-model:value="speech.enabled" :disabled="saving || !speechSupported" />
                </n-form-item>
                <n-form-item label="静置多久冒一句（秒，服务端下限 30）">
                  <n-input-number v-model:value="speech.idleSec" :min="30" :max="3600" :step="10" :disabled="saving || !speechSupported" style="width: 100%" />
                </n-form-item>
              </div>
              <n-form-item label="台词库（一行一条）">
                <n-input
                  v-model:value="speechLinesText"
                  type="textarea"
                  :rows="6"
                  :disabled="saving || !speechSupported"
                  :status="speechLinesError ? 'error' : undefined"
                  placeholder="今天也要加油哦&#10;摸摸头～&#10;休息一下眼睛吧"
                />
                <div v-if="speechLinesError" class="err">{{ speechLinesError }}</div>
                <div v-else class="hint">
                  {{ speechBytesHint }} · 单条 ≤ {{ BUBBLE_MAX_BYTES }} 字节（UTF-8，≈31 汉字，固件
                  <code>mp_cmd_t.s = char[96]</code>）· 最多 50 条（此处为前端校验；服务端
                  <code>SpeechScheduler.MaxUtf8Bytes=95</code> 还会再卡一次，超长条目跳过并记日志）
                </div>
              </n-form-item>
            </n-form>
            <div class="hint" style="margin-top: 6px">
              生效链路（服务端工作区已实现）：<code>SpeechScheduler</code> 每 5s 一拍 →
              仅<b>在线</b>设备、静置 ≥ <code>idleSec</code>（夹取 ≥30s）、两次台词间隔 ≥120s →
              随机挑一条 → <code>CommandQueue.Enqueue(id,"bubble",line)</code> → 设备 poll 取走 →
              固件 <code>MP_CMD_BUBBLE</code>。保存即随 <code>PUT /admin/settings</code> 落盘并热重载，
              无需重启（是否已部署到目标实例以服务端实际版本为准）。
            </div>
          </n-space>
        </n-card>

        <n-collapse>
          <n-collapse-item title="高级：地图时钟坐标表（clock.mapOffsets，E9 魔法值）" name="mo">
            <n-input
              v-model:value="mapOffsetsText"
              type="textarea"
              :rows="6"
              placeholder='{ "200000100": [123, 240] }'
              style="font-family: monospace"
              :status="mapOffsetsError ? 'error' : undefined"
            />
            <div v-if="mapOffsetsError" class="err">{{ mapOffsetsError }}</div>
            <div v-else class="hint">格式：{ "地图id": [x, y] }——烘焙视口内屏幕坐标；保存时随配置一并下发（manifest rev+1）</div>
          </n-collapse-item>
        </n-collapse>

        <n-space justify="end">
          <n-button @click="initialLoad" :disabled="saving">放弃修改</n-button>
          <n-button type="primary" :loading="saving" @click="onSave">保存设置</n-button>
        </n-space>
      </n-space>
    </template>
  </div>
</template>

<style scoped>
.mb12 { margin-bottom: 12px; }
.grid4 {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
  gap: 0 16px;
}
.hint { font-size: 12px; opacity: 0.6; margin-top: 4px; }
.err { font-size: 12px; color: #d03050; margin-top: 4px; }
.mt8 { margin-top: 8px; }
.req { margin-top: 6px; line-height: 1.9; }
.req code, .hint code { background: rgba(128, 128, 140, 0.15); padding: 0 3px; border-radius: 3px; }
.block-center { display: flex; justify-content: center; padding: 48px 0; }
</style>
