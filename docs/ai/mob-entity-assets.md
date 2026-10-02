# 怪物 / NPC 实体资产（「怪物资产可用」全链路）

> 定稿：2026-10-02 ｜ 用户口径：**「菜单怪物页选中 = 宠物形象变成这只怪物」**，且
> **board216（AMOLED-2.16 480×480）与 board185b（LCD-1.85B 360×360）两板都要能用**。
> 本文 = 该功能的契约 + 真机/离线取证 + 踩过的坑，接手人先读这一篇。

## 1. 一句话架构

**不新增包格式、不新增渲染分支**：服务端把 Mob.wz / Npc.wz 导出成**与纸娃娃逐字节同构**的
`PARTS（整套形象）+ LAYOUT（每动作一包）`，固件把它当"另一套装扮"绑上
（`render_set_parts` + `render_set_layout`）⇒ 合成器/动画器/包解析器全部复用。
差别只有三处：**selector/entity 元数据**、**动作名映射**、**切换与持久化**。

```
Web 素材页「怪物」tab 📤
  └─ POST /api/admin/devices/{id}/push {kind:"mob", id:"100100", switch:true}
       ├─ AssetExporter.ExportMobAssets(id)         Mob.wz → PARTS + N×LAYOUT
       ├─ DeviceAssetService.EnsureMobAsync(id)     写 {hash}.mpak + 合并 manifest-assets.json
       ├─ DeviceManifestService.BumpRev(id)         设备长轮询被唤醒 → 拉到新 manifest
       └─ CommandQueue.Enqueue(id,"entity",{entity:"mob:100100"})   （15s 后重发一次兜底）
            └─ 固件 poller「entity」→ MP_CMD_SET_ENTITY → state_machine.dispatch_entity()
                 ├─ 按 entity 找 PARTS/LAYOUT 路径（asset_dl 实体查询面）
                 ├─ render_set_parts + render_set_layout（默认动作，循环）
                 └─ 意图写 NVS（键 "entity"）→ 重启自动恢复
菜单「怪物」页点选 = 同一条 MP_CMD_SET_ENTITY（未缓存先下载，落盘后自动切换）
```

## 2. 数据契约（服务端 → 固件）

`manifest-assets.json` / `/api/device/manifest` 里每个实体 = 1 个 PARTS + N 个 LAYOUT：

| 字段 | PARTS | LAYOUT | 说明 |
|---|---|---|---|
| `kind` | `PARTS` | `LAYOUT` | 固件 `kind_dir` 白名单内，无新 kind |
| `selector` | `mob` / `npc` | 同左 | 菜单按它 + entity 前缀圈定实体 |
| `entity` | `mob:<id>` / `npc:<id>` | 同左 | **实体的唯一键**（切换指令载荷就是它） |
| `action` | — | `stand`/`move`/`die1`… | WZ 真实动作名（不翻译） |
| `defaultAction` | ✔ | ✔ | 固件 idle 循环用；缺省 `stand` → `move` → 首个 |
| `origin` | — | **恒 `[0,0]`** | 见 §4「摆放口径」 |
| `bounds` | — | `[w,h]` = cell | 诊断/Web 预览用，不参与固件定位 |
| `cellOrigin` | — | `[ox,oy]` | 条带 cell 内锚点（原始 SpriteStrip 元数据，仅供排查） |

固件 `local_file_t` 为此新增 `default_action[32]`（manifest 解析 + 本地清单回写都要带上，
否则下次开机动作名丢失）。

## 3. 固件侧改动清单（两板逐文件同改）

| 文件 | 改动 |
|---|---|
| `net/asset_dl.h/.c` | 实体查询面：`asset_dl_entity_list`（mob+npc，entity 去重）、`asset_dl_entity_exists`、`asset_dl_entity_parts_path`、`asset_dl_entity_layout_path`、`asset_dl_entity_layout_cached`、`asset_dl_entity_default_action`、`asset_dl_entity_actions`、`asset_dl_layout_origin_of`（按**包 hash** 查锚点）；`defaultAction` 解析/回写 |
| `app/app_core.h` | 新指令 `MP_CMD_SET_ENTITY`（s = entity；空/"paperdoll" = 回纸娃娃） |
| `app/state_machine.c` | `s_entity` + NVS `"entity"` 持久化；`dispatch_entity()`；`entity_action_of()` 动作名映射；`dispatch_action()` 实体分支；开机 `entity_restore()`；实体态忽略表情指令；服务端换装（SET_PARTS）时退出实体态并重绑 stand1 |
| `render/lvgl_bridge.c` | 「怪物」页列 mob+npc；点选 → `menu_dispatch_entity()`（未缓存先下载）；`>` 当前形象标记 |
| `render/compositor.c` | 画布锚点改为**按当前 LAYOUT 包 content hash** 查（不再按动作名——跨实体动作重名会串台） |
| `render/mpak.c` | LAYOUT `expression_count == 0` 合法（实体包没有表情维度） |
| `net/poller.c` | 现代/传统两路都认 `entity` 指令 |

> **两板隔离纪律**：以上改动在 `Firmware/board216/` 与 `Firmware/board185b/` 各存一份
> （无共享文件）。本次用 `git diff board216 → patch board185b` 移植，随后**两侧各自
> `idf.py build` 0 error** 才算过。

## 4. 摆放口径（为什么 origin 必须是 0,0）

- 导出器写的 LAYOUT `piece.x/y = 位图左上角 − 实体锚点`（= `-strip.Origin`）⇒ **画布坐标
  0 点就是实体锚点**（脚底/中心，WZ 的 origin 语义）。
- 合成器摆放：`缓冲左上 = 屏心 − (origin − 画布左上) × 2`，即"清单里的 origin 落在屏心"。
  要让**锚点**落屏心，就必须下发 `origin = [0,0]`。
- 历史坑：NPC 旧导出下发的是**条带 cell 内锚点** `(OriginX,OriginY)`，而 piece 又是
  `-Origin` 起算 ⇒ `(origin − cx0)` 把原点算了两遍，实体整体偏 `2×Origin`。
  NPC 当年"只下载不渲染"，这个错误一直没暴露。**现在统一 0,0**。
- 锚点查询必须**按包 hash**（`asset_dl_layout_origin_of`）：怪物与纸娃娃动作名会重名
  （都有 `fly`/`hit`），按动作名查清单会取到另一实体的 origin ⇒ 形象跳位。

## 5. 排障：三个真因（都已修，别再踩）

1. **`expression_count == 0` 被固件拒收**（`mpak.c`）
   LAYOUT 解析旧校验 `expr_count == 0 → MPAK_ERR_FMT` ⇒ **实体 LAYOUT 整包打不开**，
   表现是"包下得下来、形象起不来"。表情是纸娃娃专属维度，实体包 0 是合法值。
2. **`SKCanvas.DrawBitmap(bmp, x, y)` 在 x>0 时一个像素都不画**（`AssetExporter`）
   SkiaSharp 实测：同一条带 PNG，`f=0` 画得出 712 px，`f≥1` 全 0 ⇒ 每个动作只有第 0 帧
   有内容，其余帧是空部件（"能动但只有一帧/动作发僵"）。改用显式 source/dest 矩形
   （`DrawBitmap(bmp, srcRect, dstRect)`）后逐帧像素数与条带 PNG 各 cell 完全一致。
   证据：`tools/test_mob_pack.c`（host 侧对拍，见 §6）。
3. **动作名不是纸娃娃那套**：怪物/NPC 用 `stand/move/fly/hit1/die1`，纸娃娃是
   `stand1/walk1/fly/alert/hit`。固件 `entity_action_of()` 做映射，映射不到就回落
   **该实体的默认动作**（绝不去查纸娃娃的 LAYOUT，否则会出现跨实体部件错配）。

## 6. 取证（都是真跑过的）

| 证据 | 做法 | 结果 |
|---|---|---|
| 服务端导出/推送 | `POST /api/admin/devices/dev-8a628a/push {"kind":"mob","id":"100100"}` | 日志 `资产登记完成（新打包）`；`切换实体 → mob:100100（PARTS dfa7f36730c61355，默认动作 stand）` |
| 设备清单契约 | `GET /api/device/manifest?deviceId=…` | PARTS `{selector:mob, entity:mob:100100, defaultAction:stand}`；LAYOUT `{action:stand/move/hit1/die1, origin:[0,0], bounds:[37,26]…}` |
| 指令链路 | 读 `data/queues/dev-8a628a.json` | `{"seq":15,"type":"entity","payload":{"entity":"mob:100100","action":"stand"}}` |
| 包能否解析/能否画 | `tools/test_mob_pack.c`（gcc 直编 `mpak.c` + 真实 .mpak） | **44 项断言全过**：16 部件位图+掩码全可读；4 个 LAYOUT 共 16 帧逐帧合成非空（`/tmp/mob_<action>_f<n>.bmp`） |
| 渲染颜色对不对 | 与 `GET /api/admin/thumb?type=mob&id=100100` 参考图对比 | 一致（绿壳/蓝白头/紫足） |
| 两板编译 | `idf.py -B build build`（board216 / board185b） | 均 **0 error** |

复跑 host 对拍：

```bash
cd Firmware/board216
gcc -std=c11 -O2 -Wall -Wextra -I tools/stubs -I main/render -I main/app \
    tools/test_mob_pack.c main/render/mpak.c -o /tmp/test_mob_pack
D=<服务端>/data/cache/export/<deviceId>
/tmp/test_mob_pack $D/<PARTS>.mpak $D/<LAYOUT-stand>.mpak $D/<LAYOUT-move>.mpak …
```

## 7. 界面文案与交互（设备端）

- 根页第 4 行「怪物」→ 实体页：`>` = **当前形象**，`[v]` = 包已缓存，`[ ]`/`[x]` = 未缓存
  （离线时置灰不可点）。
- 点选：已缓存 → 立刻切换；未缓存 → 下载（状态行「下载中，请稍候」）→ 落盘后自动切换。
- 切回纸娃娃：菜单「纸娃娃」页任选一套装扮（服务端换装指令同效）。
- 选项持久化：NVS `minipet/entity`；重启/断网后仍用上次的怪物（包被摘除则自动回纸娃娃）。

## 8. 未做（明确边界）

- 怪物**漫游/战斗**（随机出现、走近、hit1/die1 交互）不在本次范围 —— 本次只做
  「资产可用 = 形象可切换、动画可播」。
- 实体专属表情：怪物/NPC 没有表情维度，实体态下表情指令被忽略（设计如此）。
