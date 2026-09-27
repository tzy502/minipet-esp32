<script setup>
/**
 * 设备宠物缩略图（T1）：按「该设备真实装扮」请求服务端合成 PNG。
 *
 * 背景（修复前）：DashboardView 传 thumbUrl('paperdoll', d.deviceId)，而 deviceId 形如
 * dev-693ea4 —— ThumbService.BuildAppearance（Server/.../ThumbService.cs:276-324）只解析
 * `k:v|...` 外观拼串，解析不出的串 → 恒落 seed/default-appearance.json（神子），
 * 于是所有设备卡片长得一样。
 *
 * 现在：设备列表只有 hasPetConfig 布尔（无 petConfig 本体），故按设备懒加载
 * GET /api/admin/devices/{id} 取 petConfig → appearanceToDraft → buildPaperdollId
 * （Web/src/utils/appearance.js:89）→ paperdollThumbUrl（id 即外观拼串，天然区分缓存）。
 * - hasPetConfig=false：设备就是默认宠物 → 仍用 id=deviceId（服务端落 seed 神子，语义正确）；
 * - 详情拉取失败：回落 seed 缩略图（不空图、不报错，卡片仍可点进详情页重试）。
 */
import { computed, onMounted, ref, watch } from 'vue'
import { NAvatar } from 'naive-ui'
import { paperdollThumbUrl, thumbUrl } from '../api/client'
import { appearanceToDraft, buildPaperdollId } from '../utils/appearance'
import { useDevicesStore } from '../stores/devices'

const props = defineProps({
  device: { type: Object, required: true },
  size: { type: Number, default: 64 },
  /** 合成尺寸（服务端仅 64/128/192/256，缺省 192 与原卡片一致）。 */
  renderSize: { type: Number, default: 192 },
  round: { type: Boolean, default: true },
})

const store = useDevicesStore()
const petConfig = ref(null)
const loadFailed = ref(false)

async function ensure() {
  const id = props.device?.deviceId
  loadFailed.value = false
  if (!id || !props.device?.hasPetConfig) {
    petConfig.value = null
    return
  }
  try {
    petConfig.value = await store.ensurePetConfig(id)
  } catch {
    petConfig.value = null
    loadFailed.value = true
  }
}

onMounted(ensure)
watch(() => [props.device?.deviceId, props.device?.hasPetConfig], ensure)

const src = computed(() => {
  const id = props.device?.deviceId || ''
  if (petConfig.value) {
    return paperdollThumbUrl(buildPaperdollId(appearanceToDraft(petConfig.value)), props.renderSize)
  }
  return thumbUrl('paperdoll', id) // 默认宠物 = seed 外观；详情失败时同样回落这里
})

defineExpose({ loadFailed, petConfig })
</script>

<template>
  <n-avatar
    :size="size"
    :src="src"
    :round="round"
    object-fit="cover"
    :title="loadFailed ? '装扮详情加载失败，暂显示默认外观（进详情页可重试）' : '按该设备真实装扮合成'"
  />
</template>
