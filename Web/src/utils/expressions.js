/**
 * 25 表情清单（E4「25 表情手动指定」入口的唯一数据源）。
 *
 * 来源（逐字照抄，禁止自创表情名——需求 docs/ai/requirements-analysis.md:147「25 表情之外
 * 不做任何自创表情；某表情素材缺失 → 回退 default」）：
 *   - Server/MinipetServer/Services/PaperdollService.cs:63-74 `KnownExpressions`
 *     （WZ Face_000.wz 实测 25 个；key 顺序 = 导出器 LAYOUT expression 列表顺序）
 *   - 同清单的文档口径：docs/ai/README.md:93（default/blink/hit/…/dam/qBlue）
 *   - 固件侧同名宏：Firmware/main/app/app_core.h:52-77（MP_EXPR_*，18 个具名 + 渲染层插播）
 *
 * 字段：
 *   key      — 下发给设备的表情名（poller.c 指令 {"type":"expression","payload":"<key>"} → render_set_expression）
 *   cn       — 中文标签（PaperdollService.KnownExpressions 的 Cn 列，逐字一致）
 *   friendly — 是否进「随机表情」池（PaperdollService 的 Friendly 列；负面表情只能手动播）
 *
 * ⚠ blink 由渲染层本地 3~8s 随机插播（app_core.h 注释），应用层一般不主动发；
 *   此处保留在清单内（E4 要求 25 个全量入口），仅作调试用。
 */
export const EXPRESSIONS = [
  { key: 'blink', cn: '眨眼', friendly: false },
  { key: 'hit', cn: '受击', friendly: false },
  { key: 'smile', cn: '微笑', friendly: true },
  { key: 'troubled', cn: '烦恼', friendly: false },
  { key: 'cry', cn: '哭', friendly: false },
  { key: 'angry', cn: '生气', friendly: false },
  { key: 'bewildered', cn: '慌张', friendly: false },
  { key: 'stunned', cn: '发晕', friendly: false },
  { key: 'vomit', cn: '呕吐', friendly: false },
  { key: 'oops', cn: '哎呀', friendly: true },
  { key: 'cheers', cn: '欢呼', friendly: true },
  { key: 'chu', cn: '亲亲', friendly: true },
  { key: 'wink', cn: '眨眼（俏皮）', friendly: true },
  { key: 'pain', cn: '痛苦', friendly: false },
  { key: 'glitter', cn: '闪亮', friendly: true },
  { key: 'despair', cn: '绝望', friendly: false },
  { key: 'love', cn: '爱心', friendly: true },
  { key: 'shine', cn: '闪耀', friendly: false },
  { key: 'blaze', cn: '怒火', friendly: false },
  { key: 'hum', cn: '哼歌', friendly: true },
  { key: 'bowing', cn: '鞠躬', friendly: false },
  { key: 'hot', cn: '热', friendly: false },
  { key: 'dam', cn: '郁闷', friendly: false },
  { key: 'default', cn: '默认', friendly: false },
  { key: 'qBlue', cn: '蓝色问号', friendly: false },
]

/** 表情名集合（校验用：只允许清单内 25 个）。 */
export const EXPRESSION_KEYS = new Set(EXPRESSIONS.map((e) => e.key))

/**
 * 气泡文本字节上限：固件 mp_cmd_t.s 为 char[96]（Firmware/main/app/app_core.h:150，
 * 注释「action/expression/hash/气泡文本（≈31 汉字）」）→ UTF-8 编码后必须 ≤ 95 字节。
 */
export const BUBBLE_MAX_BYTES = 95

/** 气泡文本的 UTF-8 字节数（浏览器 TextEncoder；非浏览器环境回退长度估算）。 */
export function bubbleByteLength(text) {
  const s = String(text ?? '')
  if (typeof TextEncoder !== 'undefined') return new TextEncoder().encode(s).length
  return s.length * 3
}
