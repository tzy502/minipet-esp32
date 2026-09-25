<script setup>
/**
 * 单设备详情（E4/E13）：信息卡 + 换宠换装（AppearancePicker 复用）+ 阈值覆盖 + OTA 触发。
 * 换装 = PUT devices/{id} 显式传 petConfig（按设备隔离，manifest rev+1）；
 * 阈值覆盖 = 传 thresholds 对象；显式传 petConfig:null = 清空回默认宠物。
 */
import { computed, onMounted, reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import {
  NCard, NSpace, NButton, NTag, NDescriptions, NDescriptionsItem, NInput,
  NInputNumber, NCheckbox, NForm, NFormItem, NResult, NSpin, NPopconfirm,
  NImage, NDivider, useMessage,
} from 'naive-ui'
import { getDevice, updateDevice, triggerOta, thumbUrl } from '../api/client'
import { useDevicesStore } from '../stores/devices'
import AppearancePicker from '../components/AppearancePicker.vue'
import { buildThumbId, hasSelection, toPetConfig } from '../utils/appearance'
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
      // 从 petConfig 回填选择（设备配置与静态占位 id 同命名空间时直接回显）
      const pc = device.value.petConfig
      selection.value = {
        hair: pc?.hair ?? null,
        face: pc?.face ?? null,
        coat: pc?.coat ?? null,
        pants: pc?.pants ?? null,
        weapon: pc?.weapon ?? null,
      }
      const th = device.value.thresholds ?? {}
      thresholdForm.deadzone = th.imuDeadzoneDeg ?? 8
      thresholdForm.light = th.tapLightG ?? 2
      thresholdForm.hard = th.tapHardG ?? 4
      thresholdForm.idle = th.idleToClockMin ?? 5
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

// ── 换宠换装 ────────────────────────────────────────────────────────────
const selection = ref({ hair: null, face: null, coat: null, pants: null, weapon: null })
const applying = ref(false)
const previewUrl = ref('')
const hasPetConfig = computed(() => !!device.value?.petConfig)

function refreshPreview() {
  previewUrl.value = hasSelection(selection.value) ? thumbUrl('paperdoll', buildThumbId(selection.value)) : ''
}

async function applyPet(clear = false) {
  applying.value = true
  try {
    // 显式传 null = 清空回默认宠物（后端以「字段出现且为 null」判定）
    await updateDevice(props.id, { petConfig: clear ? null : toPetConfig(selection.value) })
    message.success(clear ? '已恢复默认宠物（设备下次 poll 生效）' : '装扮已下发（设备下次 poll 生效）')
    previewUrl.value = ''
    await load()
    devicesStore.fetchAll({ silent: true }).catch(() => {})
  } catch (e) {
    message.error(e?.serverError || '装扮下发失败')
  } finally {
    applying.value = false
  }
}

// ── 阈值覆盖 ────────────────────────────────────────────────────────────
const overrideThresholds = ref(false)
const thresholdForm = reactive({ deadzone: 8, light: 2, hard: 4, idle: 5 })
const savingTh = ref(false)
const thresholdsSource = computed(() => device.value?.thresholdsSource || 'global')

async function saveThresholds() {
  savingTh.value = true
  try {
    await updateDevice(props.id, {
      thresholds: overrideThresholds.value
        ? {
            imuDeadzoneDeg: Number(thresholdForm.deadzone),
            tapLightG: Number(thresholdForm.light),
            tapHardG: Number(thresholdForm.hard),
            idleToClockMin: Number(thresholdForm.idle),
          }
        : null,
    })
    message.success(overrideThresholds.value ? '已按设备覆盖阈值' : '已保存（未勾选覆盖，不改变阈值）')
    load()
  } catch (e) {
    message.error(e?.serverError || '阈值保存失败')
  } finally {
    savingTh.value = false
  }
}

// ── OTA（E11：WiFi 拉包自更新，双分区回滚）──────────────────────────────
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
          <div style="flex: 1; min-width: 280px">
            <AppearancePicker v-model="selection" />
          </div>
          <div class="preview-col">
            <div class="preview-box">
              <n-image
                v-if="previewUrl || hasPetConfig"
                :src="previewUrl || thumbUrl('paperdoll', device.deviceId)"
                width="160"
                height="160"
                object-fit="contain"
                style="border-radius: 8px; background: #f7f7fa"
              />
              <span v-else class="hint">无预览</span>
            </div>
            <n-tag size="small" :bordered="false">{{ hasPetConfig ? '当前有自定义装扮' : '当前为默认宠物' }}</n-tag>
          </div>
        </n-space>
        <template #action>
          <n-space justify="space-between">
            <n-popconfirm @positive-click="applyPet(true)">
              <template #trigger>
                <n-button quaternary type="warning" :loading="applying">清空装扮（回默认）</n-button>
              </template>
              清空该设备的自定义装扮，恢复默认宠物？
            </n-popconfirm>
            <n-space>
              <n-button secondary @click="refreshPreview" :disabled="!hasSelection(selection)">预览组合</n-button>
              <n-button type="primary" :loading="applying" :disabled="!hasSelection(selection)" @click="applyPet(false)">
                应用到设备
              </n-button>
            </n-space>
          </n-space>
        </template>
      </n-card>

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
          </div>
        </n-form>
        <template #action>
          <n-space justify="end">
            <n-button type="primary" size="small" :loading="savingTh" @click="saveThresholds">保存阈值</n-button>
          </n-space>
        </template>
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
.preview-col { display: flex; flex-direction: column; align-items: center; gap: 8px; }
.preview-box { width: 160px; height: 160px; display: flex; align-items: center; justify-content: center; }
.block-center { display: flex; justify-content: center; padding: 48px 0; }
</style>
