<script setup>
/**
 * 装扮部件选择器（E4）：发型/脸型/上衣/裤/武器 分类 NSelect。
 * 纸娃娃编辑器与设备详情「换宠换装」共用；选项为静态占位（见 utils/appearance.js）。
 */
import { NSelect, NFormItem } from 'naive-ui'
import { CATEGORIES, optionsOf } from '../utils/appearance'

const props = defineProps({
  /** { hair: '30000', face: null, ... } —— null/未选 = 该部位用默认 */
  modelValue: { type: Object, default: () => ({}) },
})
const emit = defineEmits(['update:modelValue'])

function onPick(key, value) {
  emit('update:modelValue', { ...props.modelValue, [key]: value ?? null })
}
</script>

<template>
  <div class="picker-grid">
    <n-form-item v-for="cat in CATEGORIES" :key="cat.key" :label="cat.label" :show-feedback="false" label-placement="top">
      <n-select
        :value="modelValue[cat.key] ?? null"
        :options="optionsOf(cat.key)"
        :placeholder="`选择${cat.label}（占位数据）`"
        clearable
        filterable
        @update:value="(v) => onPick(cat.key, v)"
      />
    </n-form-item>
  </div>
</template>

<style scoped>
.picker-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
  gap: 12px 16px;
  align-items: end;
}
</style>
