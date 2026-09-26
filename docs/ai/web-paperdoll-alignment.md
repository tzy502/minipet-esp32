# Web 纸娃娃编辑器对齐桌面版（问题④ 落档）

> 状态：待开发（P1）
> 来源：2026-09-26 胶水拍板「Web 编辑器不完全复刻桌面的，补齐」+「神子当默认配置写进板子」

## 1. 现状 vs 桌面版差距

| 层面 | 桌面版（mapleStoryMiniPet） | Web 现状 | 要做 |
|---|---|---|---|
| 类目槽位 | 14 部件槽 + gender/body/skin/ear | 仅 5 个（hair/face/coat/pants/weapon） | 扩全 |
| 选项数据 | WZ 真实目录枚举 | `appearance.js` 硬编码 8 个 id/类 | 服务端 catalog API |
| 预览渲染 | PaperdollService 真实合成 | `RenderPlaceholder` 色块占位 | thumb type=paperdoll 接真实合成 |
| 染色系统 | dye/hue/saturation/brightness 每槽位 | 无 | P2（设备端 RGB565 合成时考虑） |

## 2. 改动清单

### 2.1 前端（Web/src）

- `utils/appearance.js`：
  - `CATEGORIES` 扩到全槽位：cap(帽子)/cape(披风)/overall(套服)/glove(手套)/shield(盾)/shoes(鞋)/faceAccessory(脸饰)/eyeAccessory(眼饰)/earring(耳环)/weapon(武器)/mount(骑宠)/chair(椅子)
  - 规则对齐桌面版：overall 与 coat+pants 互斥（穿套服时上/下置灰）
  - `toPetConfig` 输出全槽位字段（null=不穿）
- `views/PaperdollView.vue`：按新 CATEGORIES 动态渲染下拉（不再写死 5 个）

### 2.2 服务端（Server）

- **catalog API**（新）：`GET /api/admin/catalog?part=hair` → 从 WZ 枚举该类目全部部件 id + 名称（Character/{Hair|Face|Cap|...}.img，名字取 name 节点，与桌面版 AppearanceService 同源逻辑）
- **真实合成预览**：`ThumbService` 的 `type=paperdoll` 从占位渲染改为调 PaperdollService（服务端已迁移的 2511 行合成器）按 selection 出 64×64 真图；磁盘缓存 key 换成 selection hash
- `GET /api/admin/thumb?type=paperdoll&id=hair:36633|face:20094|...` 协议不变（前端零改动）

## 3. 神子 = 默认配置（写进板子/服务端种子）

### 数据（已从桌面版 settings.json savedPaperdolls.神子 挖出，逐字段核对）

```json
{
  "name": "神子",
  "gender": 0,
  "skin": 0,
  "bodyId": 2000,
  "ear": "humanEar",
  "hair":   { "id": "36633" },
  "face":   { "id": "20094" },
  "cap":    { "id": "1003953" },
  "overall":{ "id": "1050863" },
  "shoes":  { "id": "1074299" },
  "weapon": { "id": "1572011" },
  "cape": null, "coat": null, "pants": null, "glove": null,
  "shield": null, "faceAccessory": null, "eyeAccessory": null, "earring": null
}
```

（overall 套服替代 coat+pants；桌面版各槽位 dye/visible/effect 字段全 0/false，迁移时省略）

### 落点（三处）

1. **服务端种子**：`Server/MinipetServer/seed/default-appearance.json` ← 上面这份（首启/无预设时自动注册为纸娃娃预设「神子」，presets API 可见可编辑）
2. **固件默认**：`Firmware/main/profiles/default_profile.h`（或 assets）内嵌同一份——设备离线/首启未配对时的兜底装扮
3. **前端**：`appearance.js` 导出 `SHENZI_DEFAULT`，编辑器加「载入默认（神子）」快捷按钮

## 4. 验收

- [ ] Web 页面能选全 14 槽位；overall/coat+pants 互斥生效
- [ ] catalog API 返回真实 WZ 部件列表（hair 36633 在列）
- [ ] 预览出 PaperdollService 真实合成图（非色块）
- [ ] 保存预设「神子」→ 设备选择器纸娃娃 tab 可见
- [ ] 板子未配对时按默认神子渲染
- [ ] 全链路黑盒：页面改一件 → 设备 manifestation 换装生效
