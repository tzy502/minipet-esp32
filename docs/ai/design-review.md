# MiniPet-ESP32 Phase 2 设计评审

> 状态：评审完成（2026-09-25）
> 评审对象：`requirements-analysis.md`（E1–E14）/ `software-design.md` / `algorithm-asset-format.md`
> 交叉核对：`deployment-design.md` / `clock-display-spec.md` / `README.md`（F19 决策汇总）+ 桌面版仓库（mapleStoryMiniPet）符号实证
> 评审人：ZCode（实现 Agent，替代 Claude Code）
> 结论：**修订一轮后方可进 M1** —— 3 个阻断级问题 + 11 项设计级意见（二轮验证补 1 中 2 小）；E9 评审建议已被胶水否决（见 3.4，维持魔法值表定稿）

---

## 一、桌面版仓库符号实证（文档引用 vs 真实代码）

| 文档引用 | 核对结果 |
|---|---|
| `MapService.RenderViewport` | ✅ MapService.Render.cs:68，签名 `(MapInfo, camCenterX, camCenterY, zoom, viewTimeMs, screenW, screenH, target)`，与烘焙用法吻合 |
| `ClampCamera` / `ParseBacks` / `ExtractPng` | ✅ 分别在 MapService.cs:137 / MapService.cs:327（private）/ WzService.cs:430 |
| `CollectPiecesForFrame` / `MaterializePieces` | ✅ PaperdollService.cs 内 13 处引用 |
| `GetMeshBack`（F19 决策文档提到） | ❌ **grep 无命中，方法不存在**（可能已改名）。M1 迁移时需更正引用 |
| E12 气泡参数（wordWrap 90 / 行高 16 / SimSun 系） | ✅ BalloonService.cs:184-185（`wrapW=90`、`lineHeight=16f`）、:483-491（Windows SimSun/MingLiU 优先）逐字吻合 |
| EffectLayerService 恒 default stub（F19 断言） | ✅ :311 附近 `DefaultExpressionDriver.GetExpression() => ExpressionDefault` |
| WzService 解析链（clock spec 断言） | ✅ `LoadWz(wzLibPath, baseWzPath)` 两参签名（:74，探针用法 `LoadWz("", 数据目录)` 成立）；`GetOrigin`（:509）；:359/:386/:514 注释明确 outlink 链与「origin 在源节点、PNG 在 _Canvas」语义 |
| 条带公式（BGMAP 元数据可行性） | ✅ MapService.Render.cs:221-222 实证公式：ScrollH 自动滚动 `X += (rx*5*t) % cx`，非滚动视差 `X += floor(camCenter*(100+rx)/100)`；MapService.cs:799 `GetBackTileMode`（bit2=ScrollH / bit3=ScrollV）。算法文档 strip 的 `speed_x`/`rx_parallax` 字段有公式来源 |
| 表情维度（A2 修复路径可行性） | ✅ PaperdollService.cs:455 `CollectPiecesForFrame(hash, a, action, frame, expression)` 签名含 expression 维度；:79 注释「帧源缓存 key 已含 expression，换表情零额外失效成本」。算法文档十.3 的 (action, frame, expression) 组合导出成立 |

**迁移表 Dispatcher 数据不准（两处方向性错误）**。严格计数 `Dispatcher.UIThread`，桌面版 `MiniPet/Services/` 根目录实测 **20 处 / 9 文件**（E1 写的「17 处」已过期）：

| 迁移表声称 | 实测 |
|---|---|
| MapService「删 1 处 Dispatcher 封送」 | **0 处**（MapService.cs / MapService.Render.cs 均无） |
| PaperdollService「零 UI 依赖原样迁」 | **含 2 处 Dispatcher 引用**（非 UIThread 模式，类型级引用，仍需清理） |
| MusicCatalogService / MusicPlayerService / EventBus 迁入 | 分别含 **1 / 1 / 2** 处 `Dispatcher.UIThread`，表内未标注清理项 |

不阻断（清理总量仍很小），但迁移表是 M1 验收依据，数据必须更正。
**建议 M1 验收加机械门禁：迁入完成后 `grep -r Avalonia Server/` 零命中。**

---

## 二、阻断级问题（实现前必须解决）

### A1 部署形态「三容器 → 单容器」已按 E3 拍板，旧文档未同步

E3 定稿单容器（api 直接托管 Vue 产物 + QQ 网关为容器内 node 子进程），但以下三处仍写「三容器 + nginx 反代 + :8090」，全是过期残留：

- `deployment-design.md`（整篇按三容器写，且含 `README` 对照表 8090 旧端口）
- `README.md:19`（架构一句话）
- `docs/ai/README.md` 总体架构图

单容器化本身是净收益：上一轮针对 nginx 的风险项（BGM `proxy_buffering`、长轮询 `proxy_read_timeout`）整体作废，Kestrel 原生流式无缓冲问题。**但 `deployment-design.md` 必须重写为单容器版或加「已被 E3 取代」标注，README 两处同步**，否则实现时会拿错蓝本。

### A2 表情替换机制在格式层未定义（algorithm-asset-format.md 硬伤）

LAYOUT 的 piece 有 `expr_index(u8)`，但 **PARTS 索引没有任何表情关联字段**——固件拿到「当前帧 part 21 是 face 件、当前表情是 smile」之后，无法定位 smile 版部件图的 part_id。「导出器保证同位替换」没有落到字段上，M2 导出器动工前必须定稿。

建议方案（示意）：PARTS 索引 16B 中拆出 `expr_group(u16)`——同一 face 部件的 25 个表情变体共享 group 编号、group 内按 LAYOUT 的 expression 列表顺序排列；固件按「当前表情 index 在 group 内取第 N 项」。非 face 件 group=0。

### A3 体积预算三处算错（algorithm-asset-format.md 第九节）

| 项 | 文档估算 | 实算 | 差异原因 |
|---|---|---|---|
| BGMAP 单地图 | 300–500KB | **≈930KB 起** | static_back 480×480×2=450KB + tile_layer 2.125B/px≈478KB，文档自身公式即 ≥900KB |
| FONT 三档 | ~1.5MB | **≈3.2MB**（16px 448KB + 24px 1008KB + 32px 1792KB，按 3500 字全量） | 32px 若限标题字符集（~500 字）则 ≈1.7MB，需写明字符集策略 |
| 全量首拉 | 2.5–3MB | **≈4–5.5MB** | 上两项 + PARTS 未计入 25 表情 face 变体（约 8 件 × 25 变体 ≈ +200–400KB） |

局域网 1–2MB/s 均可承受（首拉 5s 内），但预算表是 TF 卡容量与淘汰策略的规划依据，必须修正。
可选优化（不阻断）：tile_layer 改稀疏/RLE（tile 层大面积透明，可省 60–70%），代价是固件 blit 复杂度，列为 P2。

---

## 三、设计级问题（建议修订）

### 3.1 端口双头冲突

`appsettings.Server.Port: 38090` 与 `.env MINIPET_PORT` 都能改端口，但容器形态下改 appsettings 端口不影响 Docker 端口映射，改了即失效——违反「双通道不分叉」原则。
**建议：端口只属部署层（.env），appsettings 删除 `Server.Port`，E4 设置页端口项改只读展示。**

### 3.2 单容器最终镜像缺 node 运行时

software-design 2.5 节多阶段构建只讲了构建期用 node 编 Web；QQ 网关是**运行期**子进程，final 层必须带 node（+~180MB，「镜像 ≤200MB」目标作废，需在部署文档更新）。
另需写明：容器 SIGTERM 时子进程终止传播、防僵尸 node 进程（Process.Exited 回收）。

### 3.3 E6 IMU 上报与 4.2 本地驱动矛盾

E6 写「角度 100ms 上报，服务端重算 layout 下发」，4.2 写「条带偏移 = IMU 倾角（本地）」。连续视差效果走服务端往返（上报→指令→下次 poll）延迟不可接受，且 10 事件/秒/设备浪费带宽。
**建议定稿：倾斜视觉全本地驱动；IMU 只上报状态变迁事件（倾斜进入/退出）。** 设备缓存清单上报（E7 `cached` 标记依赖）可搭 event 通道。

### 3.4 E9 时钟位置：维持魔法值表（胶水已拍板，原评审建议不采纳）

**评审原建议**（自动推导为主）**被否决**。胶水定稿（2026-09-25）：**维持「地图模板 → 时钟坐标」魔法值表**。

理由：魔法值表可覆盖 **WZ 中没有 clock 配置的任意地图**——26 张清单之外、或未来新增地图，手工加一个表项即可显示时钟；自动推导只能覆盖 WZ 内置 clock 的 26 张，覆盖面反而更小。表的增删改由 Web 配置页完成（E9 原文），manifest `clock_table` 下发。

可选 P2 便利项（非机制）：导出器为 26 张已知地图生成建议初值供一键填表；**表仍是唯一事实源**。

### 3.5 fontTime 时钟数字无包类型归属

待机时钟的 0–9 / am / pm / comma（`etc.img/clock/fontTime`，WZ 地图素材）在五个 kind 中没有位置（FONT kind 是宋体文本字体）。**导出器范围需补一项**：建议按 PARTS 包导出（保留 part_id），排布参数（26/22/17 宽、AMPM_GAP=12、起点 +18+3/+83 相对 clock_table 锚点、comma 偶显奇隐）按 clock-display-spec.md 定稿值固件硬编码。

### 3.6 manifest 缺选择器元数据

设备选择器三个 tab 要显示条目名称与缩略图，manifest 目前只有 hash+kind。**需加 `label` 字段与缩略图 asset 引用**；E7 `cached` 标记依赖设备上报缓存清单，上报通道需定义（可并入 event）。

### 3.7 LVGL 与自研合成器关系未定义

宠物场景是自研部件合成、菜单/气泡是 LVGL——谁拥有 framebuffer、flush 路径怎么合流（LVGL v9 custom draw unit？还是旁路直写后 LVGL 仅覆盖 UI 层？）是固件最关键的架构决策之一，software-design 4.2 节应补一节定稿。

### 3.8 RGBA5650 1bit alpha 风险应提前到 M2 出结论

1x 存储 + 2x nearest 放大会放大 alpha 阈值锯齿。开放问题 1 的 A/B（1bit vs 4bit alpha 变体）应作为 **M2 回放工具的验收项**（与 PaperdollService 直渲染对比时顺带评估边缘质量），不留到固件阶段返工。

### 3.9 IMusicPlayer 迁入口径不成立（中等）

software-design 2.1 迁移表写「MusicCatalogService / MusicPlayerService / MusicDecisions 迁入，`IMusicPlayer` 换成流式转发器实现」，但实测桌面版 `IMusicPlayer` 是**围绕本地 BASS 播放的两阶段协议**（`IPreparedStream.Prepare/Validate/Commit`，注释明言「提交段任一 BASS 调用失败」）——服务端不再播放，这个接口没有「换实现」的空间。而 2.2 节 BgmRouter 自己用的是 `IMusicSource(WZ|QQ)` 概念，两处口径不一致。
**建议定稿：统一为 `IMusicSource`（取流源抽象，服务端侧）——`IMusicPlayer`/`IPreparedStream`/`BassMusicPlayer` 全部不迁；`MusicDecisions`（选曲决策）保留迁入；`MusicPlayerService` 拆解后仅留决策相关逻辑。** E8「音量/当前源偏好存设备配置」与此一致（播放控制状态在设备端）。

### 3.10 零碎项

| # | 位置 | 问题 |
|---|---|---|
| 1 | software-design 4.4 | TF 目录 `/mnipet/` 拼写 → `/minipet` |
| 2 | software-design 风险表 | `ImageSmapeSharp` 拼写 → ImageSharp |
| 3 | algorithm 二节 | crc32c 需写明覆盖范围（建议 header+payload 全量） |
| 4 | algorithm 三节 | PARTS 索引建议加显式 `offset(u32)`（16B→20B），比累计偏移前向安全 |
| 5 | algorithm 七节 | AUDIO_META title 定长 64B 仅装 21 个汉字，建议 96B+ 或偏移式字符串 |
| 6 | software-design 2.4 | JSON 注释策略建议直接定 `_comment` 键（.NET 无 JSON5 原生支持），不留两案 |
| 7 | E3 vs 现有文档 | E3 新规「仓库禁止真实 IP/用户名」与 README、deployment-design 中 `192.168.3.46`、`homes/15080035319/...` 冲突，开源前需脱敏或私有细节外移 |
| 8 | E1 / F19「10.4K 行」口径 | 过期：实测迁入集 17 文件 **13,140 行**（扣除将重写的 CacheManager 720 + ConfigService 620，净迁入 ≈11.8K）。迁移工作量口径更新 |
| 9 | algorithm 五节 | BGMAP strip 头无循环周期字段——WZ 公式 `% cx` 的 cx（tile 周期宽）需导出器折叠进条带图宽（导出已按 cx 平铺好的循环图），此导出职责应在格式文档写明 |

### 3.11 出厂体验缺口（可选项）

未配对 + 首启无网 + TF 为空 = 黑屏 FATAL。可考虑固件分区内置一套默认素材包保底开箱体验（P2）。

---

## 四、确认项（评审通过，照此执行）

1. M2「解包回放逐像素对比 PaperdollService 直渲染」验收设计
2. 内容 hash 寻址（xxhash64 / System.IO.Hashing）+ 三条差量粒度原则
3. 表情作为 LAYOUT 独立维度（expression_count + 表情名列表）
4. 看门狗三振熔断 + NVS 计数持久化 + 稳定后清零
5. manifest `ETag=rev` 304 语义
6. 固件双核任务划分（PRO=网络/下载/BGM，APP=渲染/输入）与三条队列
7. M1–M5 服务端线与 M6 固件 bring-up 并行计划
8. 实体区 30fps 局部刷新带宽 ~3.1MB/s ≈ 32% QSPI 预算（与 E5「≤31%」一致，数值口径统一为 200×260×2×30fps 即可）
9. BGM 128kbps = 16KB/s 常量带宽评估
10. 单容器化本身（E3）——简化部署与流式路径，方向正确

---

## 五、修订动作顺序（建议）

1. `algorithm-asset-format.md` 出 **v2**：A2（表情字段定稿）+ A3（预算修正）+ 3.10 中 3/4/5/9 项
2. `software-design.md` 小修：迁移表按第一节实证更正（含 GetMeshBack）、统一 `IMusicSource` 口径并调整 Music 系迁移范围（3.9）、补 node 运行时与 SIGTERM 要点（3.2）、补 LVGL 合成架构（3.7）、修 3.10 之 1/2/6
3. `requirements-analysis.md` 微修：E6 定稿本地驱动（3.3）、E4 端口项只读（3.1）、E1「10.4K 行 / 17 处」更新为实测口径（3.10 之 8 与第一节）
4. `deployment-design.md` 重写为单容器版（或顶部加「已被 E3 取代」标注），README.md / docs-ai README 同步单容器 + 38090
5. 以上落完启动 M1，验收门禁加「`grep -r Avalonia Server/` 零命中」
