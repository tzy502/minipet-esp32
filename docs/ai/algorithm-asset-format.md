# 素材包二进制格式（算法级规格）

> 状态：**Phase 2 设计稿 v1**（2026-09-25）
> 定位：实现级细节文档——布局表、部件包、背景包、字体包的二进制结构
> 评审：Phase 2 完成后随 software-design.md 一并过 3 agent 评审（含对抗验证 agent）
> 作者说明：C# 侧实现细节由 Agent 定稿，胶水开独立 agent 对抗验证

---

## 一、设计原则

1. **设备端零解析成本**：所有结构按「顺序读、定长头、偏移索引」设计，固件拿到就能用，不需要 JSON 解析器、不需要 WZ 知识
2. **hash 寻址**：每个素材以内容 hash（xxhash64，快且短）为身份，manifest 按 hash 去 diff；URL 即 `GET /api/device/asset/{hash}`
3. **差量友好**：部件按「部件图」独立成包（换一件衣服只拉一个包）；布局表按「动作」独立；背景按「地图」独立——粒度 = 差量粒度
4. **端序与对齐**：统一小端、4 字节对齐、所有字符串 UTF-8 定长区 + 偏移（不用 C 字符串）

## 二、包的通用信封

```
[16B]  MAGIC     "MPAK"（4B）+ version(u16=1) + flags(u16)
[ 8B]  kind      u64：PARTS=1 / LAYOUT=2 / BGMAP=3 / FONT=4 / AUDIO_META=5
[ 8B]  content_hash  u64  本包内容 hash（manifest 比对用）
[ 4B]  payload_len   u32
[ 4B]  reserved  u32（0）
[... ]  payload（按 kind 各自结构）
[ 8B]  尾部 crc32c + zero（校验完整性）
```

固件校验流程：MAGIC → crc → 长度 → hash 与 manifest 比对，任一失败按 E11 损坏处理（弃用重拉 + dam 表情）。

## 三、kind=1 PARTS（部件图包）

一个纸娃娃装扮的全部部件位图（一次换装 = 一个包）。

```
payload:
[u32] part_count
[part_count × 16B] 索引：part_id(u32) | w(u16) | h(u16) | origin_x(i16) | origin_y(i16) | z_base(i16) | flags(u16)
                    flags: bit0=is_face（表情切换只重画 face 类）
[...] 位图数据区（连续，索引按顺序偏移累计；每图 RGB565 行对齐 4B）
```

- 格式 **RGB565**（无 alpha 的部件直接不透明）；带 alpha 的部件用 **RGBA5650 变体**：1bit alpha 掩码位图附在像素后（每 8 像素 1 字节）——比 RGBA8888 省 40%，像素风 alpha 边缘 1bit 足够
- part_id 用稳定编号（导出器分配：body=1, head=2, hair=10.., face=20.., coat=30.....）
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
[track_count × N]：id(u32) | title(定长64B UTF-8) | source(u8: 0=WZ 1=QQ) | duration_s(u32)
```

音频本体不打包（流式拉取，E8）；此包仅列表（设备端选择器/控制条显示用）。

## 八、manifest（版本总表，JSON，非二进制）

```json
{
  "proto": 1,
  "rev": 184,
  "assets": {
    "<hash>": { "kind": "PARTS", "bytes": 51200, "url": "/api/device/asset/<hash>" },
    "...": { "kind": "LAYOUT", "entity": "paperdoll:default", "action": "walk1" },
    "...": { "kind": "BGMAP", "map": "200000100" }
  },
  "firmware": { "ver": "0.3.1", "url": "/api/device/firmware/0.3.1.bin" },
  "clock_table": { "200000100": [123, 240], "220000100": [98, 258] }
}
```

- `clock_table`：E9 的地图时钟魔法值表（地图→时钟坐标），Web 可改 → rev+1 → 设备拉新 manifest 生效
- 设备 diff：本地已有 hash 集 vs manifest → 拉缺失/更新的

## 九、体积预算（480 屏基准）

| 包 | 估算 | 说明 |
|---|---|---|
| PARTS（一整套装扮） | ~200-400KB | 10-14 部件 × 1x RGB565 |
| LAYOUT（一动作） | 5-30KB | 纯索引数据 |
| 全动作 LAYOUT（30 动作） | ~300KB | walk1/stand1/fly/... |
| BGMAP（一地图） | 300-500KB | 480×480×2 + alpha 层 + 条带 |
| FONT 三档 | ~1.5MB | 24px 主体 + 16px + 32px |
| **一设备全量首拉** | **~2.5-3MB ≈ 局域网 2-3 秒** | 换装/换图只拉差量（KB 级） |

## 十、导出器（服务端侧）实现要点

1. 复用 `PaperdollService.CollectPiecesForFrame/MaterializePieces` 拿每帧部件清单与坐标 → 写 LAYOUT
2. 部件位图走 `WzService.ExtractPng` + PngEncoder 解码 → BGRA→RGB565 量化（带 1bit alpha 判定阈值 ≥128）
3. 表情维度：对 25 表情 × 各动作跑 (action, frame, expression) 组合，face 类部件按 expr_index 替换
4. BGMAP：`MapService.RenderViewport` 出 static_back（视口快照）+ tile 层（关 back 只渲染 tile/obj）分两趟；条带元数据从 `ParseBacks` 的 ScrollH/V 项生成
5. hash 用 xxhash64（服务端 System.IO.Hashing），manifest rev 单调递增
6. 导出 CLI：`dotnet run --project Server/tools/Exporter -- --appearance <json> --profile amoled216`（CI/手动皆可）

## 十一、开放问题（留给评审）

1. RGBA5650 的 1bit alpha 在半透明特效（如 glow）上是否可接受？备选：face/特效类部件允许 4bit alpha 变体（flag 位）
2. LAYOUT 的 z 用 i8 是否够（WZ zmap 183 层是桌面合成顺序，导出时已折叠为相对序，预计 <127）
3. 条带 speed 单位 px/s 与 IMU 倾角的映射系数放 Web 配置还是包内——倾向 Web（可调）
