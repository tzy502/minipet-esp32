<script setup>
import { onMounted, onBeforeUnmount, ref } from 'vue'
import { useRouter } from 'vue-router'
import {
  NGrid, NGridItem, NCard, NButton, NBadge, NEmpty, NResult, NSpin, NAlert,
  NModal, NForm, NFormItem, NInput, NTag, NSpace, NPopconfirm, useMessage,
} from 'naive-ui'
import { useDevicesStore } from '../stores/devices'
import { deleteDevice } from '../api/client'
import DevicePetThumb from '../components/DevicePetThumb.vue'
import { fmtAgo } from '../utils/format'

const router = useRouter()
const message = useMessage()
const store = useDevicesStore()

// ── 配对码入册（E13：屏显 6 位码 → Web 输入绑定命名，10 分钟有效）──────────
/* 删除设备（用户口径："服务端运行删除链接过的硬件"）：
 * 走 DELETE /admin/devices/{id} → 成功后刷新列表（失败把服务端错误原样提示）。 */
async function removeDevice(d) {
  try {
    await deleteDevice(d.deviceId)
    message.success(`已删除设备 ${d.name || d.deviceId}`)
    await store.fetchAll({ silent: true })
  } catch (e) {
    message.error(e?.serverError || '删除失败')
  }
}

const pairVisible = ref(false)
const pairCode = ref('')
const pairName = ref('')
const pairing = ref(false)

function openPair() {
  pairCode.value = ''
  pairName.value = ''
  pairVisible.value = true
}

async function submitPair() {
  const code = pairCode.value.trim()
  if (!/^\d{6}$/.test(code)) {
    message.warning('配对码为 6 位数字')
    return
  }
  pairing.value = true
  try {
    const dev = await store.pairDevice(code, pairName.value.trim())
    message.success(`配对成功：${dev?.name || dev?.deviceId || ''}`)
    pairVisible.value = false
  } catch (e) {
    message.error(e?.serverError || '配对失败')
  } finally {
    pairing.value = false
  }
}

// ── 列表 + 5s 轮询在线态 ─────────────────────────────────────────────────
const firstLoadError = ref('')

async function initialLoad() {
  firstLoadError.value = ''
  try {
    await store.fetchAll()
    store.startPolling()
  } catch (e) {
    firstLoadError.value = e?.serverError || '设备列表请求失败'
  }
}

onMounted(initialLoad)
onBeforeUnmount(() => store.stopPolling())

function goDetail(id) {
  router.push(`/device/${encodeURIComponent(id)}`)
}
</script>

<template>
  <div>
    <n-alert v-if="store.devices.length && store.error" type="warning" closable class="mb12">
      轮询失败：{{ store.error }}（保留上次列表，自动重试中）
    </n-alert>

    <n-card size="small" class="mb12">
      <div class="bar">
        <n-space align="center">
          <span>设备 <b>{{ store.devices.length }}</b> 台 · 在线 <b>{{ store.onlineCount }}</b> 台</span>
          <n-tag size="small" :type="store.lastOkAt ? 'success' : 'default'" :bordered="false">5s 轮询</n-tag>
        </n-space>
        <n-space>
          <n-button size="small" @click="initialLoad">刷新</n-button>
          <n-button size="small" type="primary" @click="openPair">配对新设备</n-button>
        </n-space>
      </div>
    </n-card>

    <n-spin v-if="store.loading" class="block-center" />

    <n-result
      v-else-if="firstLoadError"
      status="error"
      title="设备列表加载失败"
      :description="firstLoadError"
    >
      <template #footer>
        <n-button type="primary" @click="initialLoad">重试</n-button>
      </template>
    </n-result>

    <n-empty
      v-else-if="!store.devices.length"
      description="暂无设备 —— 设备首次开机上报后会出现在这里；点右上角「配对新设备」输入屏显 6 位码完成绑定"
      class="block-center"
    />

    <n-grid v-else :cols="'1 s:2 m:3 l:4 xl:5'" responsive="screen" :x-gap="12" :y-gap="12">
      <n-grid-item v-for="d in store.devices" :key="d.deviceId">
        <n-card size="small" hoverable @click="goDetail(d.deviceId)">
          <template #header>
            <n-badge dot :type="d.online ? 'success' : 'default'">
              <span class="dev-name">{{ d.name || '未命名设备' }}</span>
            </n-badge>
          </template>
          <template #header-extra>
            <n-tag size="tiny" :type="d.paired ? 'info' : 'warning'" :bordered="false">
              {{ d.paired ? '已配对' : '匿名' }}
            </n-tag>
          </template>
          <div class="card-body">
            <!-- 缩略图 = 该设备真实装扮（petConfig → buildPaperdollId → /admin/thumb?type=paperdoll）；
                 无自定义装扮时回落 seed 神子外观（语义即「默认宠物」） -->
            <n-badge value="宠" :offset="[-4, 4]" color="#c2c2c2">
              <DevicePetThumb :device="d" :size="64" />
            </n-badge>
            <div class="meta">
              <div>{{ d.deviceId }}</div>
              <div>固件 {{ d.firmware || '—' }}</div>
              <div>音源 {{ d.bgm?.source || 'wz' }} · 音量 {{ d.bgm?.volume ?? '—' }}</div>
              <div :class="{ dim: !d.online }">{{ d.online ? '在线' : '离线' }} · {{ fmtAgo(d.lastSeenUtc) }}</div>
              <div v-if="d.health?.batteryPercent != null">电量 {{ d.health.batteryPercent }}%</div>
              <div v-if="d.health?.lastError" class="err">最近异常：{{ d.health.lastError }}</div>
            </div>
          </div>
          <template #action>
            <n-space justify="end">
              <n-button size="small" secondary @click.stop="goDetail(d.deviceId)">换宠换装</n-button>
              <n-popconfirm @positive-click="removeDevice(d)" @click.stop>
                <template #trigger>
                  <n-button size="small" tertiary type="error" @click.stop>删除</n-button>
                </template>
                从服务端删除「{{ d.name || d.deviceId }}」？<br />
                设备若在线会**立刻掉线**，需重新 hello 才能再接入（素材缓存保留）。
              </n-popconfirm>
            </n-space>
          </template>
        </n-card>
      </n-grid-item>
    </n-grid>

    <n-modal
      v-model:show="pairVisible"
      preset="card"
      title="配对新设备"
      style="width: 380px"
      :mask-closable="!pairing"
    >
      <n-form label-placement="top" @keyup.enter="submitPair">
        <n-form-item label="配对码（设备屏幕显示的 6 位数字）" required>
          <n-input
            v-model:value="pairCode"
            :maxlength="6"
            placeholder="如 384201"
            :input-props="{ inputmode: 'numeric' }"
            :disabled="pairing"
          />
        </n-form-item>
        <n-form-item label="设备名称（可选，如「书桌小宠」）">
          <n-input v-model:value="pairName" :maxlength="32" placeholder="留空则保持匿名" :disabled="pairing" />
        </n-form-item>
        <n-space justify="end">
          <n-button :disabled="pairing" @click="pairVisible = false">取消</n-button>
          <n-button type="primary" :loading="pairing" @click="submitPair">提交配对</n-button>
        </n-space>
      </n-form>
    </n-modal>
  </div>
</template>

<style scoped>
.mb12 { margin-bottom: 12px; }
.bar { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 8px; }
.card-body { display: flex; gap: 12px; }
.meta { font-size: 12px; line-height: 1.7; opacity: 0.85; min-width: 0; }
.dev-name { max-width: 140px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; display: inline-block; }
.dim { opacity: 0.55; }
.err { color: #d03050; }
.block-center { display: flex; justify-content: center; padding: 48px 0; }
</style>
