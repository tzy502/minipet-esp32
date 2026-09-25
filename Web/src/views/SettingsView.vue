<script setup>
/**
 * 设置页（E3/E4）：WZ 路径（校验）/ 阈值（IMU 死区·轻拍·重拍·待机分钟）/
 * 地图时钟坐标表（高级，JSON 编辑）→ PUT /admin/settings 全量同构回传。
 * 端口只读展示（部署层 .env 的 MINIPET_PORT 管理，R2 定稿）。
 */
import { computed, onMounted, reactive, ref } from 'vue'
import {
  NCard, NForm, NFormItem, NInput, NInputNumber, NButton, NTag, NSpace,
  NSpin, NResult, NAlert, NTooltip, NCollapse, NCollapseItem, useMessage,
} from 'naive-ui'
import { useSettingsStore } from '../stores/settings'

const message = useMessage()
const store = useSettingsStore()

const form = reactive({
  dataPath: '',
  imuDeadzoneDeg: 8,
  tapLightG: 2,
  tapHardG: 4,
  idleToClockMin: 5,
})
const mapOffsetsText = ref('{}')
const mapOffsetsError = ref('')
const saving = ref(false)

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
            </div>
          </n-form>
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
.block-center { display: flex; justify-content: center; padding: 48px 0; }
</style>
