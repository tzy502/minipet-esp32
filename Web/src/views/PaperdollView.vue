<script setup>
/**
 * 纸娃娃编辑器（E4）：分类选择 → 服务端合成预览（GET /admin/thumb?type=paperdoll&id=拼接）
 * → 存为预设（POST /admin/presets，喂设备选择器「纸娃娃 tab」E7）。
 * 预览为服务端确定性占位渲染（M4 接 PaperdollService 真实合成后自动出真图）。
 */
import { computed, onMounted, ref } from 'vue'
import {
  NCard, NButton, NImage, NModal, NForm, NFormItem, NInput, NSpace, NTag,
  NEmpty, NSpin, NResult, NList, NListItem, NPopconfirm, useMessage,
} from 'naive-ui'
import AppearancePicker from '../components/AppearancePicker.vue'
import { buildThumbId, hasSelection, toPetConfig, CATEGORY_KEYS } from '../utils/appearance'
import { thumbUrl, listPresets, createPreset, deletePreset } from '../api/client'
import { fmtTime } from '../utils/format'

const message = useMessage()

const selection = ref({ hair: null, face: null, coat: null, pants: null, weapon: null })
const previewUrl = ref('')
const previewing = ref(false)

const pickedCount = computed(() => CATEGORY_KEYS.filter((k) => selection.value[k]).length)

function doPreview() {
  if (!hasSelection(selection.value)) {
    message.warning('先至少选择一个部件')
    return
  }
  previewing.value = true
  previewUrl.value = thumbUrl('paperdoll', buildThumbId(selection.value))
  // 占位渲染无耗时；previewing 状态留给真实合成期
  setTimeout(() => (previewing.value = false), 150)
}

// ── 预设：保存 / 列表 / 删除 ─────────────────────────────────────────────
const presets = ref([])
const presetsLoading = ref(false)
const presetsError = ref('')

const saveVisible = ref(false)
const presetName = ref('')
const saving = ref(false)

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
onMounted(loadPresets)

function openSave() {
  if (!hasSelection(selection.value)) {
    message.warning('先至少选择一个部件')
    return
  }
  presetName.value = ''
  saveVisible.value = true
}

async function submitSave() {
  const name = presetName.value.trim()
  if (!name) {
    message.warning('请输入预设名称')
    return
  }
  saving.value = true
  try {
    await createPreset(name, 'paperdoll', { selection: toPetConfig(selection.value) })
    message.success(`预设「${name}」已保存`)
    saveVisible.value = false
    loadPresets()
  } catch (e) {
    message.error(e?.serverError || '保存失败')
  } finally {
    saving.value = false
  }
}

async function removePreset(p) {
  try {
    await deletePreset(p.id)
    message.success(`已删除「${p.name}」`)
    loadPresets()
  } catch (e) {
    message.error(e?.serverError || '删除失败')
  }
}
</script>

<template>
  <n-space :size="12" align="stretch" item-style="flex:1 1 380px">
    <n-card title="换装组合" size="small">
      <AppearancePicker v-model="selection" />
      <template #action>
        <n-space justify="space-between" align="center">
          <span class="hint">已选 {{ pickedCount }}/5 个部位（不选 = 用默认）</span>
          <n-space>
            <n-button secondary @click="doPreview" :loading="previewing">预览</n-button>
            <n-button type="primary" @click="openSave">保存为预设</n-button>
          </n-space>
        </n-space>
      </template>
    </n-card>

    <n-card title="合成预览" size="small">
      <div class="preview-box">
        <n-spin v-if="previewing" />
        <n-image
          v-else-if="previewUrl"
          :src="previewUrl"
          width="192"
          height="192"
          object-fit="contain"
          style="border-radius: 8px; background: #f7f7fa"
        />
        <n-empty v-else description="选择部件后点「预览」——服务端按组合 id 渲染缩略图" style="padding: 40px 0" />
      </div>
      <div v-if="previewUrl" class="hint">id 拼接：{{ previewUrl.split('id=')[1] }}</div>
      <template #action>
        <n-tag size="small" :bordered="false">M4 占位渲染：同组合同图，接真实合成后即出换装效果</n-tag>
      </template>
    </n-card>

    <n-card title="已存预设" size="small" style="flex: 1 1 100%">
      <n-spin v-if="presetsLoading" class="block-center" />
      <n-result
        v-else-if="presetsError"
        status="warning"
        title="预设列表加载失败"
        :description="presetsError"
      >
        <template #footer>
          <n-button size="small" @click="loadPresets">重试</n-button>
        </template>
      </n-result>
      <n-empty v-else-if="!presets.length" description="暂无预设 —— 保存后出现在设备选择器「纸娃娃 tab」" style="padding: 40px 0" />
      <n-list v-else bordered size="small" style="max-height: 360px; overflow: auto">
        <n-list-item v-for="p in presets" :key="p.id">
          <n-space justify="space-between" style="width: 100%">
            <div>
              <b>{{ p.name }}</b>
              <n-tag size="tiny" style="margin-left: 8px" :bordered="false">{{ p.type }}</n-tag>
              <div class="hint">{{ fmtTime(p.updatedAtUtc) }}</div>
            </div>
            <n-popconfirm @positive-click="removePreset(p)">
              <template #trigger>
                <n-button size="tiny" quaternary type="error">删除</n-button>
              </template>
              删除预设「{{ p.name }}」？
            </n-popconfirm>
          </n-space>
        </n-list-item>
      </n-list>
    </n-card>
  </n-space>

  <n-modal v-model:show="saveVisible" preset="card" title="保存纸娃娃预设" style="width: 380px" :mask-closable="!saving">
    <n-form label-placement="top" @keyup.enter="submitSave">
      <n-form-item label="预设名称" required>
        <n-input v-model:value="presetName" :maxlength="32" placeholder="如「初始冒险家」" :disabled="saving" />
      </n-form-item>
      <n-space justify="end">
        <n-button :disabled="saving" @click="saveVisible = false">取消</n-button>
        <n-button type="primary" :loading="saving" @click="submitSave">保存</n-button>
      </n-space>
    </n-form>
  </n-modal>
</template>

<style scoped>
.hint { font-size: 12px; opacity: 0.6; }
.preview-box { display: flex; justify-content: center; align-items: center; min-height: 220px; }
.block-center { display: flex; justify-content: center; padding: 32px 0; }
</style>
