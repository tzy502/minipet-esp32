# 素材包二进制格式（算法级规格）

> 状态：**v2.2**（三轮 R4-R9：差量口径定稿/offset寻址/part_id保留段/manifest selector·cached·按设备隔离/导出器出处/fontTime项/开放问题全定稿）
> 定位：实现级细节文档——布局表、部件包、背景包、字体包的二进制结构
> 评审：Phase 2 完成后随 software-design.md 一并过 3 agent 评审（含对抗验证 agent）
> 作者说明：C# 侧实现细节由 Agent 定稿，胶水开独立 agent 对抗验证

---

## 一、设计原则

1. **设备端零解析成本**：所有结构按「顺序读、定长头、偏移索引」设计，固件拿到就能用，不需要 JSON 解析器、不需要 WZ 知识
2. **hash 寻址**：每个素材以内容 hash（xxhash64，快且短）为身份，manifest 按 hash 去 diff；URL 即 `GET /api/device/asset/{hash}`
3. **差量友好**：**PARTS 以整套装扮为传输单元**（换任意一件 = 拉新装扮包 400-700KB，局域网 <1s；part_id 跨包稳定，未来可平滑引入单件级差量——R4 定稿）；布局表按「动作」独立；背景按「地图」独立
4. **端序与对齐**：统一小端、4 字节对齐、所有字符串 UTF-8 定长区 + 偏移（不用 C 字符串）

## 二、包的通用信封

```
[16B]  MAGIC     "MPAK"（4B）+ version(u16=1) + flags(u16)
[ 8B]  kind      u64：PARTS=1 / LAYOUT=2 / BGMAP=3 / FONT=4 / AUDIO_META=5
[ 8B]  content_hash  u64  本包内容 hash（manifest 比对用）
[ 4B]  payload_len   u32
[ 4B]  reserved  u32（0）
[... ]  payload（按 kind 各自结构）
[ 8B]  尾部 crc32c（**覆盖 header+payload 全量**）+ zero
```

固件校验流程：MAGIC → crc → 长度 → hash 与 manifest 比对，任一失败按 E11 损坏处理（弃用重拉 + dam 表情）。

## 三、kind=1 PARTS（部件图包）

一个纸娃娃装扮的全部部件位图（一次换装 = 一个包）。

```
payload:
[u32] part_count
[part_count × 20B] 索引：part_id(u32) | expr_group(u16) | w(u16) | h(u16) | origin_x(i16) | origin_y(i16) | offset(u32)
                    expr_group：face 类部件的表情变体组号——同一 face 部件的 25 个表情变体共享 group、组内按 LAYOUT 的 expression 列表顺序排列；非 face 件 group=0
                    offset：位图数据区内该图的显式偏移（u32；不再按累计推算，前向安全）
                    ⚠️ z_base/flags 移除：z 属于布局表（每帧可变），PARTS 只存图
[...] 位图数据区（连续，索引 offset 显式寻址；每图 RGB565 行对齐 4B）
```

- 格式 **RGB565**（无 alpha 的部件直接不透明）；带 alpha 的部件用 **RGBA5650 变体**：1bit alpha 掩码位图附在像素后（每 8 像素 1 字节）——比 RGBA8888 省 40%，像素风 alpha 边缘 1bit 足够
- part_id 用稳定编号（导出器分配：body=1, head=2, hair=10.., face=20.., coat=30..；**fontTime 时钟数字保留段 900..（见七.5）**）
- **表情替换闭环（A2 定稿）**：渲染时 piece.expr_index ≠ 255 → 找到该 piece 引用的 face 件 part → 取其 expr_group → 在组内按 expr_index 偏移取变体 part_id → blit 变体图。导出器保证：每组变体数 == LAYOUT.expression_count，尺寸/origin 同构（仅像素不同）
- 480 屏 2x 缩放在设备端做（nearest），包里存 1x

## 四、kind=2 LAYOUT（布局表包）

一个实体（纸娃娃/怪物/NPC）一个动作的布局。**表情是独立维度**：

```
payload:
[u32] entity_id
[char32] action 名（定长 32B，如 "walk1"）
[u32] frame_count
[u32] expression_count          // 本动作支持的表情数（face 部件位图变体）
[expression_count × char32]     // 表情名列表（"default","blink","smile",...）
[frame_count × frame_header]:
    [u32] delay_ms
    [u32] move_dx(i16), move_dy(i16), pad
    [u32] piece_count
    [piece_count × 12B]:
        part_id(u32) | expr_index(u8，255=非表情件) | x(i16) | y(i16) | flip(u8) | z(i8)
[...] 无额外数据（全部引用 PARTS 包的 part_id）
```

- 渲染一帧 = 遍历 piece 列表按 z 排序 blit（设备端）
- 表情切换 = 同 action 下把 expr_index 匹配的 face 件换成目标表情的对应件（导出器保证每个表情在每帧有同位替换）
- **entities[] 的落点**：屏幕多实体 = 多个 LAYOUT 同时激活，互不引用

## 五、kind=3 BGMAP（地图背景包）

```
payload:
[char32] map_id
[u16] vw, vh                    // 设备视口（profile 相关，导出时按设备 profile 定制）
[u32] static_back_len, static_back_off
[u32] tile_layer_len, tile_layer_off
[u32] strip_count
[strip_count × strip_header]:
    part_ref(u64 hash，指向独立小 PARTS 包) | y(i16) | speed_x(i16,px/s) | rx_parallax(u8) | blend(u8)
[...] static_back：整幅 RGB565（视口大小，不透明底）
[...] tile_layer：RGBA5650（含 alpha，叠加在 static_back 上）
```

- 条带动画：`offset_x = f(time) × speed_x` 或 IMU 倾角驱动（E6），条带图 x 方向平铺循环
- **循环周期已折叠进图宽（评审 3.10-9）**：WZ 公式的取模周期 cx（tile 间隔）由**导出器**负责——导出的条带图已按 cx 平铺为「一个完整循环周期宽」的图，固件按 `offset_x mod 图宽` 循环 blit 即可，无需知道 cx
- 冰箱贴 profile：导出时 strip_count=0（无条带，省电）

## 六、kind=4 FONT（字体包）

```
payload:
[u8] size_px（16/24/32）
[u8] bpp（4）
[u32] glyph_count
[glyph_count × 12B]：unicode(u32) | w(u16) | h(u16) | advance(u8) | off_x(i8) | bearing_y(i8)
[...] 位图数据区（4bpp 行对齐）
```

- 与 LVGL `lv_font_bin` 格式兼容字段命名，但头自成体系（不直接用 LVGL 官方 bin——我们要能 hash 寻址与差量）
- 宋体源（E12），三档字号 = 三个包

## 七、kind=5 AUDIO_META（BGM 元数据）

```
payload:
[u32] track_count
[track_count × N]：id(u32) | title(定长96B UTF-8，约32汉字) | source(u8: 0=WZ 1=QQ) | duration_s(u32)
```

音频本体不打包（流式拉取，E8）；此包仅列表（设备端选择器/控制条显示用）。

## 七.5、fontTime 时钟数字素材（3.5 评审项）

待机/地图时钟的 0-9/am/pm/comma（`Map/Obj/etc.img/clock/fontTime`）**按 PARTS 包导出**（保留 part_id，设备端与普通部件同路 blit）；
排布参数不进包——起点 `clock_table 锚点 + (18+3, 83)`、`AMPM_GAP=12px`、comma 偶显奇隐，按 clock-display-spec.md 定稿值**固件硬编码**。

## 八、manifest（版本总表，JSON，非二进制）

```json
{
  "proto": 1,
  "rev": 184,
  "assets": {
    "<hash>": { "kind": "PARTS", "bytes": 51200, "url": "/api/device/asset/<hash>", "label": "默认装扮" },
    "...": { "kind": "LAYOUT", "entity": "paperdoll:default", "action": "walk1" },
    "...": { "kind": "BGMAP", "map": "200000100", "label": "天空之城售票处", "thumb": "<hash>", "selector": "map" }
  },
  （`selector`: map|paperdoll|npc|clock——无 selector 字段的条目不进设备选择器（R7）；
   `cached` 标记 = 设备经 POST /api/device/event 附本地 hash 集上报、服务端计算回写（E7）；
   **manifest 按设备隔离（E13），rev 每设备单调递增**）
  "firmware": { "ver": "0.3.1", "url": "/api/device/firmware/0.3.1.bin" },
  "clock_table": { "200000100": [123, 240], "220000100": [98, 258] }
}
```

- `clock_table`：E9 的地图时钟魔法值表（地图→时钟坐标），Web 可改 → rev+1 → 设备拉新 manifest 生效
- 设备 diff：本地已有 hash 集 vs manifest → 拉缺失/更新的

## 九、体积预算（480 屏基准）

| 包 | 估算（修正 2026-09-25 评审） | 说明 |
|---|---|---|
| PARTS（一整套装扮） | ~400-700KB | 10-14 部件 1x RGB565 + **face 类 25 表情变体**（约 8 件 × 25 ≈ +200-400KB） |
| LAYOUT（一动作） | 5-30KB | 纯索引数据 |
| 全动作 LAYOUT（30 动作） | ~300KB | walk1/stand1/fly/... |
| BGMAP（一地图） | **≈930KB 起** | static_back 450KB + tile_layer ≈478KB（2.125B/px）+ 条带；tile 层 RLE 稀疏化为 P2 优化（可省 60-70%） |
| FONT | **≈1.7-3.2MB** | 16px 448KB + 24px 1008KB（3500 字全量）+ 32px（限标题字符集 ~500 字则 256KB；全量则 1792KB——**32px 默认限字符集**） |
| **一设备全量首拉** | **≈4-5.5MB ≈ 局域网 3-6 秒** | 换装/换图只拉差量（KB 级）；TF 规划按 6MB/设备 + 淘汰水位

## 十、导出器（服务端侧）实现要点

1. 复用 `PaperdollService.CollectPiecesForFrame/MaterializePieces` 拿每帧部件清单与坐标 → 写 LAYOUT
2. 部件位图走 `WzService.ExtractPng` + PngEncoder 解码 → BGRA→RGB565 量化（带 1bit alpha 判定阈值 ≥128）
3. 表情维度：对 25 表情 × 各动作跑 (action, frame, expression) 组合，face 类部件按 expr_index 替换
4. BGMAP：`MapService.RenderViewport` 出 static_back（视口快照）+ tile 层（关 back 只渲染 tile/obj）分两趟；条带元数据从 `ParseBacks` 的 ScrollH/V 项生成
   - 公式实证（R8）：ScrollH `X += (rx*5*t) % cx`、非滚动 `X += floor(camCenter*(100+rx)/100)`（MapService.Render.cs:221-222）；ScrollH/V 判定位 `GetBackTileMode`（MapService.cs:799，bit2/bit3）；条带图按 cx 预平铺（第五节）
5. fontTime 全套（0-9/am/pm/comma）按 PARTS 导出，part_id 900.. 保留段（R6/R8）
5. hash 用 xxhash64（服务端 System.IO.Hashing），manifest rev 单调递增
7. 导出 CLI：`dotnet run --project Server/tools/Exporter -- --appearance <json> --profile amoled216`（CI/手动皆可）
8. manifest rev **每设备单调递增**（R7/R8，与按设备隔离一致）

## 十一、开放问题（R9 全部定稿）

1. **1bit alpha 保持默认**；M2 回放工具加 1bit vs 4bit 边缘质量 A/B 验收（与逐像素对比同场），结论 M2 出——届时若需 4bit 变体，用索引 flag 位扩展，格式不破坏
2. **z 用 i8 定稿**：帧内相对序实测 <127（WZ zmap 183 层是桌面合成序，导出时已折叠）
3. **条带 speed 与 IMU 倾角映射系数定稿放 Web 配置**（可调，不进包）
