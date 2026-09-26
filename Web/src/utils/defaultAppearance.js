/**
 * 纸娃娃默认装扮：神子（2026-09-26 胶水定稿，从桌面版 savedPaperdolls.神子 迁移）。
 * 与 Server/seed/default-appearance.json、Firmware 固件默认三处同源
 * （槽位为 id 字符串形态；dyeHue/dyeEnabled/enableEffect 与 seed 逐字段一致）。
 * 槽位规则：overall（套服）与 coat+pants 互斥，穿套服时上/下为 null。
 */
export const SHENZI_DEFAULT = {
  name: '神子',
  gender: 0,
  skin: 0,
  bodyId: 2000,
  ear: 'humanEar',
  hair: '36633',
  face: '20094',
  cap: '1003953',
  cape: null,
  coat: null,
  overall: '1050863',
  pants: null,
  shoes: '1074299',
  weapon: '1572011',
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
