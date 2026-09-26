/**
 * 纸娃娃外观工具层（对齐桌面版，规范：docs/ai/web-paperdoll-alignment.md 第一/四/五章）。
 * - 16 类目与桌面版 SettingsWindow.Paperdoll 逐字一致，中文标签用两字紧凑无空格版
 * - 素材选项一律走服务端 catalog API（utils/api getCatalog），禁止静态占位
 * - 草稿（draft）= 扁平 id 字符串形态；appearance JSON（存预设/下发设备）= 槽位 { id } 对象形态
 */
import { SHENZI_DEFAULT } from './defaultAppearance'

/** 16 类目（顺序/中文标签/图标/WZ 目录与桌面版一致）；2026-09-26 起目录不分性别全量展示。 */
export const CATEGORIES = [
  { key: 'hair', label: '发型', icon: '💇', folder: 'Hair' },
  { key: 'face', label: '脸型', icon: '😊', folder: 'Face' },
  { key: 'cap', label: '帽子', icon: '⛑️', folder: 'Cap' },
  { key: 'cape', label: '披风', icon: '🧣', folder: 'Cape' },
  { key: 'coat', label: '上衣', icon: '🛡️', folder: 'Coat' },
  { key: 'overall', label: '套服', icon: '🧥', folder: 'Longcoat' },
  { key: 'pants', label: '裤子', icon: '👖', folder: 'Pants' },
  { key: 'shoes', label: '鞋子', icon: '👢', folder: 'Shoes' },
  { key: 'weapon', label: '武器', icon: '🗡️', folder: 'Weapon' },
  { key: 'shield', label: '盾牌', icon: '🛡️', folder: 'Shield' },
  { key: 'glove', label: '手套', icon: '🧤', folder: 'Glove' },
  { key: 'faceAccessory', label: '面饰', icon: '🎭', folder: 'Accessory' },
  { key: 'eyeAccessory', label: '眼饰', icon: '👁️', folder: 'Accessory' },
  { key: 'earring', label: '耳环', icon: '💍', folder: 'Accessory' },
  { key: 'mount', label: '坐骑', icon: '🏍️', folder: 'TamingMob' },
  { key: 'chair', label: '椅子', icon: '🪑', folder: 'Install' },
]

/** 性别（0 男 / 1 女）。 */
export const GENDERS = [
  { value: 0, label: '男' },
  { value: 1, label: '女' },
]

/** 耳型选项（对齐桌面版 body/ear 渲染分支）。 */
export const EARS = [
  { value: 'humanEar', label: '人类' },
  { value: 'ear', label: '精灵' },
  { value: 'lefEar', label: '精灵(Lef)' },
  { value: 'highlefEar', label: '高等精灵' },
]

/** 发型回退名前缀：String.wz 读不到中文名时用 "{分类}_{id}"，回退名不参与同名折叠。 */
export const HAIR_FALLBACK_PREFIX = '发型_'

/** 空草稿：16 槽全 null（键序同 CATEGORIES）+ 基础字段 + 染色/特效开关。 */
export function newDraft() {
  return {
    gender: 0,
    skin: 0,
    bodyId: 2000,
    ear: 'humanEar',
    hair: null,
    face: null,
    cap: null,
    cape: null,
    coat: null,
    overall: null,
    pants: null,
    shoes: null,
    weapon: null,
    shield: null,
    glove: null,
    faceAccessory: null,
    eyeAccessory: null,
    earring: null,
    mount: null,
    chair: null,
    dyeHue: 0,
    dyeEnabled: false,
    enableEffect: true,
  }
}

/**
 * 默认装扮（神子）→ 完整草稿。
 * SHENZI_DEFAULT 本身是 id 字符串形态，复用 appearanceToDraft 的宽容转换并深拷贝，
 * 缺失字段（如旧数据无 dyeHue）回落 newDraft 默认。
 */
export function draftFromShenzi() {
  return appearanceToDraft(SHENZI_DEFAULT)
}

/**
 * 草稿 → 纸娃娃拼串 id（服务端 /admin/thumb?type=paperdoll 按此解析合成）。
 * 头部 g/ear/body + 16 槽全显式 `key:{id}`，`-` 表示显式清空（null）。
 * 例：g:0|ear:humanEar|body:2000|hair:36633|face:20094|cap:-|…|chair:-
 */
export function buildPaperdollId(draft) {
  const d = draft ?? {}
  const head = `g:${d.gender ?? 0}|ear:${d.ear ?? 'humanEar'}|body:${d.bodyId ?? 2000}`
  const slots = CATEGORIES.map((c) => {
    const v = d[c.key]
    return `${c.key}:${v != null && v !== '' ? v : '-'}`
  })
  return `${head}|${slots.join('|')}`
}

/**
 * 草稿 → 完整 appearance JSON（存预设/下发设备，形态对齐桌面 CharacterAppearance）。
 * 槽位字符串包成 { id } 对象，空槽显式 null；skin（皮肤索引）与 bodyId（2000 系）都输出。
 */
export function draftToAppearance(draft) {
  const d = draft ?? {}
  const out = {}
  if (d.name != null && d.name !== '') out.name = d.name
  out.gender = d.gender ?? 0
  out.skin = d.skin ?? 0
  out.bodyId = d.bodyId ?? 2000
  out.ear = d.ear ?? 'humanEar'
  for (const c of CATEGORIES) {
    const v = d[c.key]
    out[c.key] = v != null && v !== '' ? { id: String(v) } : null
  }
  out.dyeHue = d.dyeHue ?? 0
  out.dyeEnabled = d.dyeEnabled ?? false
  out.enableEffect = d.enableEffect ?? true
  return out
}

/**
 * appearance JSON → 草稿（{ id } → 字符串、null → null、缺字段回落 newDraft 默认）。
 * 兼容纯 id 字符串槽位形态（defaultAppearance.js）；name 参数优先，否则取 appearance.name。
 */
export function appearanceToDraft(appearance, name) {
  const draft = newDraft()
  if (!appearance) {
    if (name !== undefined) draft.name = name
    return draft
  }
  for (const key of ['gender', 'skin', 'bodyId', 'ear', 'dyeHue', 'dyeEnabled', 'enableEffect']) {
    const v = appearance[key]
    if (v !== undefined && v !== null) draft[key] = v
  }
  for (const c of CATEGORIES) {
    const v = appearance[c.key]
    draft[c.key] = v == null ? null : typeof v === 'object' ? (v.id != null ? String(v.id) : null) : String(v)
  }
  draft.name = name !== undefined ? name : appearance.name
  return draft
}

/** 是否发型回退名（"{分类}_{id}" 形态，不参与同名折叠）。 */
export function isHairFallbackName(name) {
  return typeof name === 'string' && name.startsWith(HAIR_FALLBACK_PREFIX)
}

/**
 * 发型同名折叠分组（纯函数，对齐桌面 GroupHairByName）：
 * 过滤回退名后按 name 分组，只保留 variants.length > 1 的组，组内按数值 id 升序。
 * 返回 [{ name, variants }]；组间顺序 = 首次出现顺序（入参已按 id 升序时即稳定）。
 */
export function groupHairItems(items) {
  const groups = new Map()
  for (const item of items ?? []) {
    if (!item || isHairFallbackName(item.name)) continue
    if (!groups.has(item.name)) groups.set(item.name, [])
    groups.get(item.name).push(item)
  }
  const result = []
  for (const [name, variants] of groups) {
    if (variants.length < 2) continue
    variants.sort((a, b) => numericId(a.id) - numericId(b.id))
    result.push({ name, variants })
  }
  return result
}

/** 部件 id → 数值（排序用）；解析失败排最后（Number.MAX_SAFE_INTEGER）。 */
export function numericId(id) {
  const n = parseInt(id, 10)
  return Number.isNaN(n) ? Number.MAX_SAFE_INTEGER : n
}
