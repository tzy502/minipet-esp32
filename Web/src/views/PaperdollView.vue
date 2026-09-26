<script setup>
/**
 * 纸娃娃编辑器（完整 16 槽版，对齐桌面版 SettingsWindow.Paperdoll.cs）：
 * 草稿 draft（16 槽 + 性别/皮肤/耳朵/特效/染发）→ 服务端真实合成预览
 * （paperdollThumbUrl(buildPaperdollId(draft), 256)）→ 存为预设（完整 appearance JSON）。
 * 草稿语义（docs/ai/web-paperdoll-alignment.md 3.9）：selection 即草稿——
 * 任何单槽选择都是对同一 draft 对象的整体修改并整体提交预览/预设，不做局部状态。
 */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import {
  NButton, NCard, NEmpty, NForm, NFormItem, NImage, NInput, NList, NListItem, NModal,
  NPopconfirm, NResult, NSelect, NSlider, NSpace, NSpin, NSwitch, NTag, useMessage,
} from 'naive-ui'
import AppearancePicker from '../components/AppearancePicker.vue'
import {
  CATEGORIES, GENDERS, EARS, appearanceToDraft, buildPaperdollId, draftFromShenzi,
  draftToAppearance, newDraft,
} from '../utils/appearance'
import { createPreset, deletePreset, getCatalog, listPresets, paperdollThumbUrl } from '../api/client'
import { fmtTime } from '../utils/format'

const message = useMessage()

// ── 草稿（selection 即草稿）：首次进入即用神子草稿出图 ─────────────────────
const draft = ref(draftFromShenzi())

/** 读槽位 id（草稿槽位为 id 字符串；宽限兼容 { id } 对象与数字形态）。null = 未穿戴。 */
function slotId(key) {
  const v = draft.value?.[key]
  if (v == null) return null
  if (typeof v === 'object') return v.id == null ? null : String(v.id)
  return String(v)
}
function hasSlot(cat) {
  return slotId(cat.key) != null
}

// ── 部件显示名「名字 [id]」：按需拉目录建 part:id → name 缓存，回退「分类_id」──
// 槽位行要显示中文名，而 Picker 契约只 emit id，名字由页面自行从 catalog 解析；
// 目录按（类目 + 性别过滤标记）缓存一次，17k 条的发型目录每会话最多拉一次。
const slotNames = ref({})
const namesFetched = new Set()
const namesFailed = new Set()
const namesFetching = new Set()

function ensureNames() {
  for (const cat of CATEGORIES) {
    const id = slotId(cat.key)
    if (id == null || slotNames.value[`${cat.key}:${id}`]) continue
    fetchPartNames(cat)
  }
}

async function fetchPartNames(cat) {
  // 发型/脸型目录按性别过滤（CATEGORIES.genderFilter 标记），其余类目与性别无关
  const gender = cat.genderFilter ? draft.value.gender : undefined
  const cacheKey = cat.genderFilter ? `${cat.key}|${gender}` : cat.key
  if (namesFetched.has(cacheKey) || namesFailed.has(cacheKey) || namesFetching.has(cacheKey)) return
  namesFetching.add(cacheKey)
  try {
    const data = await getCatalog(cat.key, gender)
    namesFetched.add(cacheKey)
    const patch = {}
    for (const it of data?.items ?? []) {
      if (it?.id != null && it.name) patch[`${cat.key}:${it.id}`] = it.name
    }
    slotNames.value = { ...slotNames.value, ...patch }
  } catch (e) {
    namesFailed.add(cacheKey)
    console.warn(`[Paperdoll] 部件名目录拉取失败（${cat.label}）：`, e?.serverError || e)
  } finally {
    namesFetching.delete(cacheKey)
  }
}

/** 槽位行显示文案：未穿戴（灰）/ 名字 [id]（名字缺失回退「分类_id」，对齐文档二章）。 */
function slotText(cat) {
  const id = slotId(cat.key)
  if (id == null) return '未穿戴'
  const name = slotNames.value[`${cat.key}:${id}`] || `${cat.label}_${id}`
  return `${name} [${id}]`
}

watch(draft, ensureNames, { deep: true })

// ── 基础项：性别 / 皮肤 / 耳朵 ────────────────────────────────────────────
const skinOptions = ref([])
const skinLoading = ref(false)
const skinError = ref('')

async function loadSkins() {
  skinLoading.value = true
  skinError.value = ''
  try {
    const data = await getCatalog('skin')
    skinOptions.value = (data?.items ?? [])
      .filter((it) => it?.id != null)
      .map((it) => {
        const n = Number(it.id)
        return { label: `${it.name} (${it.id})`, value: Number.isFinite(n) ? n : it.id }
      })
  } catch (e) {
    skinError.value = `皮肤列表加载失败：${e?.serverError || e?.message || '未知错误'}`
  } finally {
    skinLoading.value = false
  }
}

/** 选中皮肤：bodyId = 该 id、skin 保持 0（head=body+10000 成对由服务端解析）。 */
function onSkinPick(v) {
  draft.value.bodyId = v
  draft.value.skin = 0
}

// ── 槽位选择（Picker 契约：行点击 emit id 并自动关闭；底部清空按钮 emit null）─
const pickerShow = ref(false)
const pickerPart = ref('')
const pickerModel = computed({
  get: () => slotId(pickerPart.value),
  set: (v) => applyPick(v),
})

function openPicker(cat) {
  pickerPart.value = cat.key
  pickerShow.value = true
}

function applyPick(v) {
  const key = pickerPart.value
  if (!key) return
  if (v == null) {
    draft.value[key] = null
    return
  }
  if (typeof v === 'object') {
    // 契约外的宽限：若 Picker emit 了整个 item 对象，取 id 并顺手记下名字
    if (v.id == null) return
    if (v.name) slotNames.value = { ...slotNames.value, [`${key}:${v.id}`]: v.name }
    draft.value[key] = String(v.id)
  } else {
    draft.value[key] = String(v)
  }
}

/** 「×」清空：该槽位置 null（对齐桌面版 ClearEquipPart 语义）。 */
function clearSlot(cat) {
  draft.value[cat.key] = null
}

// ── 操作：载入默认（神子）/ 清空全部 / 已选计数 ───────────────────────────
const pickedCount = computed(() => CATEGORIES.filter((c) => hasSlot(c)).length)

/** 保存预设前置校验：至少穿戴一件（hair/face/16 槽任一非 null）。 */
function hasWorn() {
  return CATEGORIES.some((c) => hasSlot(c))
}

function loadDefault() {
  draft.value = draftFromShenzi()
  message.success('已载入默认装扮「神子」')
}

function clearAll() {
  draft.value = newDraft()
  message.success('已清空全部槽位')
}

// ── 合成预览：拼串 → paperdollThumbUrl；draft 任何变化 300ms 防抖刷新 ──────
const paperdollId = computed(() => buildPaperdollId(draft.value))
const previewUrl = ref('')
const previewLoading = ref(false)
const previewError = ref(false)
let previewTimer = null
let previewFallbackTimer = null

function refreshPreview() {
  previewLoading.value = true
  previewError.value = false
  previewUrl.value = paperdollThumbUrl(paperdollId.value, 256)
  // 兜底超时：正常由 <n-image> 的 load/error 事件关闭 loading
  clearTimeout(previewFallbackTimer)
  previewFallbackTimer = setTimeout(() => { previewLoading.value = false }, 8000)
}

watch(draft, () => {
  clearTimeout(previewTimer)
  previewTimer = setTimeout(refreshPreview, 300)
}, { deep: true })

function onPreviewLoad() {
  clearTimeout(previewFallbackTimer)
  previewLoading.value = false
}

function onPreviewError() {
  clearTimeout(previewFallbackTimer)
  previewLoading.value = false
  previewError.value = true
}

// ── 预设：保存（完整 appearance）/ 列表 / 载入 / 删除 ────────────────────────
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

function openSave() {
  if (!hasWorn()) {
    message.warning('先至少穿戴一件（发型/脸型或任意装备槽）')
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
    // 保存完整 appearance（16 槽 + 基础项 + 染色参数），不再是 5 槽 selection
    await createPreset(name, 'paperdoll', draftToAppearance(draft.value))
    message.success(`预设「${name}」已保存`)
    saveVisible.value = false
    loadPresets()
  } catch (e) {
    message.error(e?.serverError || '保存失败')
  } finally {
    saving.value = false
  }
}

/** 载入预设：appearance JSON → 草稿整体回填（预览随 draft watcher 自动刷新）。 */
function loadPreset(p) {
  if (p.type !== 'paperdoll' || !p.data) {
    message.warning('该预设不是纸娃娃装扮，无法载入')
    return
  }
  try {
    draft.value = appearanceToDraft(p.data, p.name)
    message.success(`已载入预设「${p.name}」`)
  } catch (e) {
    console.warn('[Paperdoll] 载入预设失败：', e)
    message.error('预设数据解析失败')
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

onMounted(() => {
  refreshPreview() // 首次进入即用神子草稿出图
  ensureNames()
  loadSkins()
  loadPresets()
})

onBeforeUnmount(() => {
  clearTimeout(previewTimer)
  clearTimeout(previewFallbackTimer)
})
</script>

<template>
  <n-space :size="12" align="stretch" item-style="flex:1 1 380px">
    <!-- 卡片一：换装组合（基础项 + 16 槽位 + 特效/染发 + 操作） -->
    <n-card title="换装组合" size="small">
      <div class="section-title">基础</div>
      <div class="form-row">
        <span class="row-label">性别</span>
        <n-select v-model:value="draft.gender" :options="GENDERS" size="small" class="row-control" />
        <span class="hint">切换后发型/脸型目录按性别重拉，已选不清空</span>
      </div>
      <div class="form-row">
        <span class="row-label">皮肤</span>
        <n-select
          :value="draft.bodyId"
          :options="skinOptions"
          :loading="skinLoading"
          size="small"
          class="row-control"
          @update:value="onSkinPick"
        />
      </div>
      <div v-if="skinError" class="hint skin-error">
        {{ skinError }}
        <n-button size="tiny" quaternary type="primary" @click="loadSkins">重试</n-button>
      </div>
      <div class="form-row">
        <span class="row-label">耳朵</span>
        <n-select v-model:value="draft.ear" :options="EARS" size="small" class="row-control" />
      </div>

      <div class="section-title">装备槽位</div>
      <div class="slot-grid">
        <div v-for="cat in CATEGORIES" :key="cat.key" class="slot-row">
          <span class="slot-icon">{{ cat.icon }}</span>
          <span class="slot-label">{{ cat.label }}</span>
          <span class="slot-name" :class="{ none: !hasSlot(cat) }" :title="slotText(cat)">{{ slotText(cat) }}</span>
          <n-button size="tiny" secondary @click="openPicker(cat)">选择…</n-button>
          <n-button size="tiny" quaternary type="error" :disabled="!hasSlot(cat)" @click="clearSlot(cat)">×</n-button>
        </div>
      </div>

      <div class="section-title">特效 / 染发</div>
      <div class="form-row">
        <span class="row-label">特效</span>
        <n-switch v-model:value="draft.enableEffect" size="small" />
        <span class="hint">装备特效（披风/武器等帧内特效）</span>
      </div>
      <div class="form-row">
        <span class="row-label">染发</span>
        <n-switch v-model:value="draft.dyeEnabled" size="small" />
        <span class="hint">染色参数随预设保存，设备端合成时生效</span>
      </div>
      <div class="form-row">
        <span class="row-label">色相</span>
        <n-slider
          v-model:value="draft.dyeHue"
          :min="0"
          :max="360"
          :step="1"
          :disabled="!draft.dyeEnabled"
          class="hue-slider"
        />
        <span class="hue-val">{{ draft.dyeHue }}°</span>
      </div>

      <template #action>
        <n-space justify="space-between" align="center">
          <span class="hint">已选 {{ pickedCount }}/16</span>
          <n-space>
            <n-button size="small" secondary @click="loadDefault">载入默认（神子）</n-button>
            <n-button size="small" quaternary type="error" @click="clearAll">清空全部</n-button>
          </n-space>
        </n-space>
      </template>
    </n-card>

    <!-- 卡片二：合成预览（draft 变化 300ms 防抖刷新，:key 强制重载） -->
    <n-card title="合成预览" size="small">
      <div class="preview-box">
        <n-spin :show="previewLoading">
          <n-image
            v-if="previewUrl"
            :key="previewUrl"
            :src="previewUrl"
            width="256"
            height="256"
            object-fit="contain"
            class="preview-img"
            @load="onPreviewLoad"
            @error="onPreviewError"
          />
          <n-empty v-else description="正在生成预览…" style="padding: 60px 0" />
        </n-spin>
      </div>
      <div class="hint id-line" :title="paperdollId">拼串：{{ paperdollId }}</div>
      <div v-if="previewError" class="err">预览加载失败（服务端未启动或合成出错）</div>
    </n-card>

    <!-- 卡片三：已存预设（保存完整 appearance；纸娃娃预设可一键载入回填草稿） -->
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
            <n-space :size="4">
              <n-button v-if="p.type === 'paperdoll'" size="tiny" secondary @click="loadPreset(p)">载入</n-button>
              <n-popconfirm @positive-click="removePreset(p)">
                <template #trigger>
                  <n-button size="tiny" quaternary type="error">删除</n-button>
                </template>
                删除预设「{{ p.name }}」？
              </n-popconfirm>
            </n-space>
          </n-space>
        </n-list-item>
      </n-list>
      <template #action>
        <n-space justify="end">
          <n-button size="small" type="primary" @click="openSave">保存为预设</n-button>
        </n-space>
      </template>
    </n-card>
  </n-space>

  <!-- 部件选择器：打开时按 part 拉目录（gender 变化由 Picker 自行重拉） -->
  <AppearancePicker v-model:show="pickerShow" v-model="pickerModel" :part="pickerPart" :gender="draft.gender" />

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
.section-title { font-size: 12px; font-weight: 600; opacity: 0.55; margin: 10px 0 8px; }
.form-row { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; }
.row-label { width: 34px; text-align: right; font-size: 12px; opacity: 0.75; flex-shrink: 0; }
.row-control { flex: 1; }
.skin-error { margin: -4px 0 8px; }
.slot-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 6px 16px; margin: 4px 0 6px; }
.slot-row { display: flex; align-items: center; gap: 6px; min-width: 0; }
.slot-icon { width: 20px; text-align: center; flex-shrink: 0; }
.slot-label { width: 30px; font-size: 12px; opacity: 0.75; flex-shrink: 0; }
.slot-name { flex: 1; min-width: 0; font-size: 12px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.slot-name.none { opacity: 0.4; }
.hue-slider { flex: 1; }
.hue-val { width: 38px; text-align: right; font-size: 12px; opacity: 0.6; flex-shrink: 0; }
.preview-box { display: flex; justify-content: center; align-items: center; min-height: 280px; }
.preview-img { border-radius: 8px; background: #f7f7fa; display: block; }
.id-line { margin-top: 8px; word-break: break-all; }
.err { font-size: 12px; color: #d03050; margin-top: 4px; }
.block-center { display: flex; justify-content: center; padding: 32px 0; }
</style>
