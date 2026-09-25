/**
 * 纸娃娃装扮静态占位数据（E4/M4）。
 * 后端素材目录 API（按分类列举部件 id）尚未提供，先静态占位：
 * id 取冒险岛经典编号段（与 Server/seed/default-appearance.json 同命名空间），
 * 后续接真实目录后仅需替换 OPTIONS 的取数来源。
 */

export const CATEGORIES = [
  { key: 'hair', label: '发型' },
  { key: 'face', label: '脸型' },
  { key: 'coat', label: '上衣' },
  { key: 'pants', label: '裤/裙' },
  { key: 'weapon', label: '武器' },
]

export const CATEGORY_KEYS = CATEGORIES.map((c) => c.key)

// 静态占位选项：value = 部件 id
export const OPTIONS = {
  hair: ['30000', '30030', '30060', '30120', '30230', '30310', '30450', '30560'],
  face: ['20000', '20001', '20004', '20012', '20021', '20035', '20049', '20056'],
  coat: ['1040002', '1040006', '1040018', '1040036', '1041044', '1041061', '1041083'],
  pants: ['1060002', '1060006', '1060020', '1061034', '1061085', '1061108'],
  weapon: ['1302000', '1302013', '1312005', '1322012', '1332005', '1372004', '1402009', '1452008'],
}

export function optionsOf(key) {
  return (OPTIONS[key] ?? []).map((id) => ({ label: `${id}`, value: id }))
}

/**
 * 拼装缩略图 id：按固定顺序「类目:部件」用 '|' 连接（未选类目跳过）。
 * 例：{ hair: '30000', face: '20000' } → "hair:30000|face:20000"
 * 服务端 /admin/thumb?type=paperdoll 用 id 决定确定性占位渲染（M4 接真实合成）。
 */
export function buildThumbId(selection) {
  return CATEGORY_KEYS.filter((k) => selection?.[k])
    .map((k) => `${k}:${selection[k]}`)
    .join('|')
}

/** 当前是否至少选了一个部件（预览/保存的前置条件）。 */
export function hasSelection(selection) {
  return CATEGORY_KEYS.some((k) => !!selection?.[k])
}

/** 转成设备 petConfig 子集（对齐 seed/default-appearance.json 的部件命名）。 */
export function toPetConfig(selection) {
  return {
    hair: selection.hair ?? null,
    face: selection.face ?? null,
    coat: selection.coat ?? null,
    pants: selection.pants ?? null,
    weapon: selection.weapon ?? null,
  }
}
