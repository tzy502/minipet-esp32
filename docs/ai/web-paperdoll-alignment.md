# Web 纸娃娃编辑器完整对齐桌面版（问题④ 定稿）

> 状态：**定稿待开发**（2026-09-26）
> 铁律：**桌面版的类目、中文名、素材选择逻辑一条都不能少**（胶水原话）
> 一切数据从 WZ 走，禁止静态占位/硬编码选项

---

## 一、类目表（16 项，顺序/中文名/WZ 目录/图标 与桌面版逐字一致）

来源：`MiniPet/Views/SettingsWindow.Paperdoll.cs:332-360` + `MaterialBrowserWindow.Logic.cs:1139-1174`

| # | 中文标签 | WZ 目录 | 图标 | 字段（JSON key） | 必选 |
|---|---|---|---|---|---|
| 1 | 发型 | `Character/Hair` | 💇 | `hair` | ✅（默认必有） |
| 2 | 脸型 | `Character/Face` | 😊 | `face` | ✅（默认必有） |
| 3 | 帽子 | `Character/Cap` | ⛑️ | `cap` | 可空 |
| 4 | 披风 | `Character/Cape` | 🧣 | `cape` | 可空 |
| 5 | 上衣 | `Character/Coat` | 🛡️ | `coat` | 可空 |
| 6 | 套服 | `Character/Longcoat` | 🧥 | `overall` | 可空 |
| 7 | 裤子 | `Character/Pants` | 👖 | `pants` | 可空 |
| 8 | 鞋子 | `Character/Shoes` | 👢 | `shoes` | 可空 |
| 9 | 武器 | `Character/Weapon` | 🗡️ | `weapon` | 可空 |
| 10 | 盾牌 | `Character/Shield` | 🛡️ | `shield` | 可空 |
| 11 | 手套 | `Character/Glove` | 🧤 | `glove` | 可空 |
| 12 | 面饰 | `Character/Accessory`（id/10000==101） | 🎭 | `faceAccessory` | 可空 |
| 13 | 眼饰 | `Character/Accessory`（id/10000==102） | 👁️ | `eyeAccessory` | 可空 |
| 14 | 耳环 | `Character/Accessory`（id/10000==103） | 💍 | `earring` | 可空 |
| 15 | 坐骑 | `Character/TamingMob` | 🏍️ | `mount` | 可空 |
| 16 | 椅子 | `Item/Install`（3010000–3020999） | 🪑 | `chair` | 可空 |
| — | 皮肤 | `Character/{2000-2999}` | 🧑 | `skin`+`bodyId` | 单选 |

**另外**（非装备槽，但配置必须一致）：`gender`(0男/1女) `ear`(humanEar/elfEar/…) `bodyId`(2000起) `dyeHue` `dyeEnabled` `enableEffect`

---

## 二、中文名解析（必须一致，禁止用 id 当名字）

- **来源**：`String.wz` → `Item.img/{folder}/{id}/name`（服务端 `WzService.GetItemName(folder, id)`，已迁移）
- **回退**：读不到时 `"{分类}_{id}"`（如 `发型_30000`）——桌面版同规则，**此回退名参与折叠判断**
- folder 取值：`Hair/Face/Cap/Cape/Coat/Longcoat/Pants/Shoes/Weapon/Shield/Glove/Accessory/TamingMob/Body/Install`

## 三、素材选择逻辑（逐条，一条都不能少）

### 3.1 枚举（服务端 catalog API）

- `Character/{folder}` 的 `.img` 子节点 = 该类目全部部件 id（服务端 `GetDirectoryChildren`，已迁移；**注意不能用只读 `_Canvas` 的老写法**，桌面版实测会缺件：Hair 少 1451、Face 少 2338、Weapon 少 3436）
- 椅子特殊：`Item/Install/{分组}.img` → 需 `GetImgChildren`（`Wz_Image.TryExtract` 后遍历数字子节点），**不可用 `GetDirectoryChildren`**（返回空，桌面版踩过「椅子没有内容」）
- 椅子 id 区间硬过滤：`3010000 ≤ id < 3021000`

### 3.2 饰品拆分（Accessory 一个目录拆 3 类）

```
id / 10000 == 101 → 面饰（faceAccessory）
id / 10000 == 102 → 眼饰（eyeAccessory）
id / 10000 == 103 → 耳环（earring）
其余                → 丢弃
```
三类可**同时穿戴**（各自独立槽位）。

### 3.3 性别过滤（发型/脸型专用，其余类目不过滤）

按 id 千位（`(n/1000)%10`）：

| 类目 | 男(0) | 女(1) | 通用(2/9) |
|---|---|---|---|
| 发型 | 0/3/5/6 | 1/4/7/8 | 其余 |
| 脸型 | 0/3/5/7 | 1/4/6/8 | 其余 |

规则：`genderTag == 2 || genderTag == 当前gender` 才显示。
**踩坑记录**：桌面版曾用 `n>=31000 = 女`，导致男发 36633（千位 6 = 男）被过滤 → 用户搜不到。**新实现必须用千位表**。

### 3.4 发型同名折叠（仅发型）

- 同名发型折叠成一个组行，展开显示变体（id 不同、名字相同，如染色/版本款）
- **回退名不折叠**：名字以 `发型_` 开头 → 不参与分组（判定函数 `IsFallbackHairName`）
- 展开状态可切换

### 3.5 排序与搜索

- 排序：数字 id 升序（`int.Parse(id)`，解析失败排最后）
- 搜索：匹配**中文名**（忽略大小写）或 **id 字符串**；发型组行同时匹配组名与变体名/id
- 搜索防抖（桌面版用 timer，Web 用 debounce）

### 3.6 缩略图（列表内每行）

- 优先部件 **icon**（`Character/{folder}/{id}.img/info/icon`，快）
- 无 icon → 回退**默认动画 stand 首帧**（`stand1/0`）
- 后台懒加载 + 缓存；窗口不可见时不加载（防并发/闪退）

### 3.7 预览（右侧详情）

- 选中条目 → 服务端真实合成（PaperdollService，非占位色块）
- 合成 64×64（列表）/ 大图（详情）两种尺寸
- 用当前草稿外观 + 该部件临时替换对应槽位

### 3.8 槽位清空

每个类目都有「清空」操作 → 该槽位置 null（对应桌面版 `SettingsWindow.Paperdoll.cs:507-516` 的 case 清空）。

### 3.9 草稿语义（重要）

- 编辑器维护**草稿外观对象**（`_draftAppearance`），打开时从当前配置克隆
- 选完某类目 → **整体应用草稿**（不是只改一个槽位）
- 原因（桌面版踩坑）：从「帽子」打开切到「上衣」选件时，若只改单槽会被主窗口旧外观覆盖 → 用户反馈「默认内容不停覆盖前面已选内容」

---

## 四、配置一致性（三处同源，改一处必须三处同步）

神子（默认装扮，2026-09-26 定稿）：

```json
{ "name": "神子", "gender": 0, "skin": 0, "bodyId": 2000, "ear": "humanEar",
  "hair": {"id":"36633"}, "face": {"id":"20094"}, "cap": {"id":"1003953"},
  "overall": {"id":"1050863"}, "shoes": {"id":"1074299"}, "weapon": {"id":"1572011"},
  "cape": null, "coat": null, "pants": null, "shield": null, "glove": null,
  "faceAccessory": null, "eyeAccessory": null, "earring": null, "mount": null, "chair": null,
  "dyeHue": 0, "dyeEnabled": false, "enableEffect": true }
```

| 落点 | 路径 | 状态 |
|---|---|---|
| 服务端种子 | `Server/seed/default-appearance.json` | ✅ 已写（含 name） |
| 前端常量 | `Web/src/utils/defaultAppearance.js` | ✅ 已写 |
| 固件板载 | `Firmware/main/app/default_appearance.h` | ✅ 已写（宏定义，含中文名） |

槽位 JSON 键名与桌面版 `CharacterAppearance.cs` 一一对应（`faceAccessory`/`eyeAccessory`/`earring`/`overall`/`shoes`…），**中文名与桌面版 UI 标签逐字一致**（帽 子/套 服/裤 子/坐 骑/椅 子 等，Web 端用两字紧凑无空格版：帽子/套服/裤子/坐骑/椅子）。

### ItemInfo 完整字段（每个槽位，与桌面版一致）

```json
{ "id": "36633", "enableEffect": 0, "visible": false,
  "dye": 0, "hue": 0, "saturation": 0, "brightness": 0 }
```
`visible` 桌面版语义 = 该槽位在列表中「已选」标记；`dye/hue/saturation/brightness` = 染色参数（Web 端先存不渲染，设备端 RGB565 合成时考虑）。

---

## 五、改动清单（开发用）

### 服务端（Server）

- [ ] **catalog API**：`GET /api/admin/catalog?part=hair&gender=0&q=黄` → `{items:[{id,name,icon_url,fallback_name}]}`
  - 复用 `WzService.GetDirectoryChildren` + `GetItemName`（两者都已迁移，直接调）
  - 实现 3.1 椅子特例 / 3.2 饰品拆分 / 3.3 性别过滤 / 3.4 折叠标记 / 3.5 搜索排序
  - 性别参数来自编辑器当前 gender
- [ ] `ThumbService` 的 `type=paperdoll` 从 `RenderPlaceholder` 色块 → **调 PaperdollService 真实合成**
  - 缓存 key 换成 selection 的 hash（当前是 `SafeFileId(id)`）
  - 详情大图：加 `&size=256` 参数（或新端点）
- [ ] 预设种子：首启无预设时把 `seed/default-appearance.json` 注册为纸娃娃预设「神子」（presets API 可见可编辑）

### 前端（Web/src）

- [ ] `utils/appearance.js`：`CATEGORIES` 换成上面 16 项（中文标签 + 图标 + WZ folder + 字段名）
- [ ] `AppearancePicker.vue`：从 catalog API 取数（替换 `OPTIONS` 静态表），加搜索框 + 防抖 + 发型折叠组 + 分页/虚拟滚动（Hair 1.7 万条必须有）
- [ ] `PaperdollView.vue`：16 槽位动态渲染 + 每槽「清空」+「载入默认（神子）」按钮（已加好）
- [ ] 草稿语义：selection 即草稿，整体提交（对齐 3.9）
- [ ] 性别切换时重拉发型/脸型列表

### 固件（Firmware）

- [ ] `default_appearance.h` 已在（身份展示/兜底）；后续与 manifest 的 petConfig 字段核对

---

## 六、验收（黑盒，逐条）

- [ ] 16 类目全部可见、中文名与桌面版一致
- [ ] 素材列表条数与桌面版素材浏览器**同类目同数量**（抽查 hair/face/cap/椅子）
- [ ] 发型搜索「黄色电」能搜到 36633（性别过滤不误杀）
- [ ] 同名发型折叠成组、展开正常
- [ ] 饰品三类可同时穿；id 前缀拆分类正确
- [ ] 椅子列表非空且都在 3010000–3020999
- [ ] 预览是真实合成图（非色块）
- [ ] 页面配出神子 → 保存预设「神子」→ 选择器可见
- [ ] 板子首启/离线按神子渲染
- [ ] 全链路：页面换一件 → 设备换装生效
