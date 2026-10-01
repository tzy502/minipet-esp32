# 整图 BGMAP「分块（tile）布局」契约 v1（冻结）

> 2026-10-01 定稿。背景与动因见文末「为什么」。**这是服务端导出与固件读取之间的唯一接口**，
> 双方任何一方改动都必须先改本文档并同步另一方。

## 0. 一句话

整图 BGMAP 包在保持**信封、尺寸、地面表、条带语义**不变的前提下，把 **static / tile / 条带** 三类
像素层（以及 tile 层的 1bit 掩码）从「逐行存储」改为「**128×128 世界像素瓦片**存储」，
让固件按瓦片**连续读**（SD 顺序吞吐 1336 KB/s 可跑满），而不是按窗口**跨行距逐行读**（实测仅 ~130 KB/s）。

## 1. 与现有格式的关系（强约束）

- **信封**（32B map_id + 头 + 各层长度/偏移 + 尾扩展块 magic `0x4D504745`）**完全不变**。
- **flags 新增 bit1 = `FULLMAP_FLAG_TILED`**：置位表示本包的三类像素层按本文档的瓦片布局存储。
  **bit0（full_map）语义不变**；未置位 ⇒ 一切按旧「逐行」格式解析（固件必须两套都能读）。
- 尺寸字段 `vw/vh`（世界 1x 像素）、`strip_count`、strip 描述（世界 `y`、`h`、`speed`、`rx`、
  周期平铺 `wrap`）**语义不变**；条带只是"像素区的存储方式"变成瓦片。
- **地面表**（尾扩展块 `ground_len/ground_off`）**不变**。
- 旧包（无 bit1）必须继续可用：固件保留现有逐行路径。

## 2. 瓦片几何

- `TILE = 128`（世界像素，常量；服务端可只支持 128，固件按包里字段读，不硬编码）。
- 对某一层，其"层矩形"尺寸 = 该层自身宽高 `lw × lh`：
  - static：`lw = vw`, `lh = vh`
  - tile：`lw = vw`, `lh = vh`（掩码同尺寸）
  - 条带 i：`lw = 该条带源宽`, `lh = 该条带高`（包内已有的 strip 描述）
- 瓦片网格：`gx = ceil(lw / TILE)`, `gy = ceil(lh / TILE)`，行主序编号
  `idx = ty * gx + tx`（`tx ∈ [0,gx)`, `ty ∈ [0,gy)`）。
- **瓦片写满**：每块固定 `TILE*TILE*2` 字节（RGB565 小端），右/下越界的像素补 **0**。
  即层像素区大小 = `gx*gy*TILE*TILE*2`（不是 `lw*lh*2`）。
- 瓦片内像素行主序、每行 `TILE*2` 字节、无行对齐额外 padding。

## 3. 掩码（仅 tile 层需要；条带的 alpha 掩码见 §4）

- tile 层掩码按**同网格**分块，每块固定 `TILE*TILE/8 = 2048` 字节，1bit/px，
  **行按字节对齐**：块内第 `y` 行占 `[y*16, y*16+16)` 字节，位序 MSB-first（x=0 在最高位），
  与现有 tile 掩码位序一致（沿用 `mpak` 现有口径，不要新发明）。
- 越界像素补 0（= 透明）。

## 4. 条带（strips）

- 每条带像素区按 §2 单独分块（网格按该条带自身 `lw × lh` 算）。
- 条带 alpha 掩码：若该条带 `blend` 位要求掩码，则掩码同样**按同网格分块**，
  每块 `2048` 字节、口径同 §3（块内 `y*16` 起 16 字节/行）。无掩码的条带照旧不写掩码区。
- 条带的「时间滚动 / 周期平铺 / 世界对齐」全部是**运行时**行为（固件按 `speed/rx/wrap` 计算
  源坐标），与存储布局无关：固件取像素时用的仍是**条带自身坐标**（0..lw, 0..lh），
  由固件把世界坐标折算成条带内坐标后按瓦片取。

## 5. 层的字节布局（相对 payload 起点）

沿用现有头字段顺序，只是"层长度"的含义随 flags 变化：

```
static_back_off / static_back_len : 分块后的 static 像素区（§2/§3），长度 = gx*gy*TILE*TILE*2
tile_layer_off  / tile_layer_len  : 分块后的 tile 像素区，长度同上（tile 层）
tile_mask_off                     : tile 层掩码区 = gx_t*gy_t*2048（紧跟在 tile 像素区之后，
                                    或由现有掩码偏移字段给出：沿用现有字段，不要新增）
strips[]                          : 每条带 { px_off, cov_off, row_bytes/strip_w, strip_h, y, speed, rx, blend, wrap }
                                    px_off 指向该条带的分块像素区起点；cov_off 指向分块掩码区起点
```

> 现有实现在"条带描述"里带 `row_bytes`（= `align4(lw*2)`）用于逐行口径。分块后固件**不得**再用
> `row_bytes` 做像素寻址，只用 §2 的瓦片几何。为兼容旧包，`row_bytes` 字段**保留但忽略**（tiled 时）。

## 6. 固件读取口径（唯一正确算法）

```
取层 L 上世界坐标 (x, y) 的像素：
  tx = x / TILE; ty = y / TILE;              // 向下取整（x,y ≥ 0）
  in_x = x % TILE; in_y = y % TILE;
  tile_off = L.px_off + (ty * gx + tx) * TILE * TILE * 2;
  byte_off = tile_off + (in_y * TILE + in_x) * 2;
  // 掩码：tile_off_cov + (ty*gx+tx)*2048 + in_y*16 + (in_x >> 3)，位序 MSB-first
```

- 固件**只允许整块读**（一次 `pread` 读 `TILE*TILE*2` 或 `2048` 字节到 PSRAM 瓦片缓存），
  不允许再按行/按像素向 SD 发命令。
- 建议瓦片缓存 ≥ 3×3 块（含 margin 的可见窗口通常 3~4 列 × 3~4 行），LRU 淘汰；
  缓存命中时取像素是**纯 PSRAM memcpy**，不再碰 SD。
- 装载期一次性预取当前相机窗口覆盖的瓦片；相机移动时只补"新露出的一列/一行瓦片"。

## 7. 验收判据（双方都要过）

1. **等价性**：同一张图、同一相机位置，分块包与旧逐行包渲染出的 480×480 帧**逐像素一致**
   （服务端导出工具需提供 `--verify` 或离线对拍脚本；固件侧提供 `::shot` 抓帧对拍）。
2. **性能**：整窗填充读字节数不得高于旧格式的 1.2 倍，耗时 **≤1.0s**（旧实测 8.5~17.2s）；
   拖动补边（一列瓦片）**≤0.2s**。
3. **兼容**：旧包（无 bit1）在真机上仍 `地图装载 <id>（条带 N）rc=0`，渲染不回归。
4. **不回归**：条带绘制、tile 掩码、地面线、宠物/气泡/横幅、菜单中文全部照旧。

## 8. 为什么（动因数据，2026-10-01 真机实测）

| 项 | 实测 |
|---|---|
| SD 顺序读（32KB 块 POSIX pread） | **1336 KB/s** |
| 整图窗口填充（跨行距逐行/预读） | **~130 KB/s** |
| static 层一行 | stride 4540B，窗口只需 672B ⇒ 效率 15% |
| 8 / 14 条带图一次装载 | 8.5s / 17.2s（`窗口读 4700 行 / 1712KB / 8534ms`） |
| 单帧满层合成 | compose 128ms + blit 32ms |

结论：瓶颈是**读放大 + 每行一次命令**，不是卡、不是带宽、也不是算力。
分块后按块连续读：可见窗口 288×288 世界 px ⇒ 3~4 列 × 3~4 行瓦片 ≈ 9~16 块 ≈ 288~512KB，
按 1336 KB/s ⇒ **0.22~0.38s**，且拖动只补边缘一列瓦片（128×128×2 = 32KB ≈ 24ms）。

---

## 9. 实现注记（服务端落地 2026-10-01；**不改上文任何语义**，只登记落地时确认的事实与固件侧前置项）

### 9.1 服务端落在哪些字节上

- `Server/MinipetServer/Export/BgmapPackWriter.cs`：`BgmapInput.Tiled` ⇒ static / tile 两层按 §2/§3
  编码，尾扩展块 `flags |= 0x2`（bit1）。长度口径：
  `static_back_len = gx*gy*32768`；`tile_layer_len = gx*gy*32768 + gx*gy*2048`
  （掩码区紧跟像素区，**无额外 4B 补齐**——两值天然是 4 的倍数）。
  几何常量与 §6 算式的唯一实现点：`Server/MinipetServer/Export/TiledLayout.cs`。
- **条带像素不在 BGMAP 包内**（`strip.part_ref` 指向独立小 PARTS 包）⇒ bit1 同时是那些 PARTS 包的
  口径标志：分块导出时条带小包的位图记录也按 128×128 瓦片编码
  （`PartPackWriter.Build(..., tiled: true)`），记录 = `[瓦片像素区 gx*gy*32768][瓦片掩码区 gx*gy*2048]`，
  PARTS 索引字段（w/h/origin/offset，20B 条目）不变。**三类层同进同退，不存在"半 tiled"包。**
- 回退/对拍开关：Exporter `--legacy-rows`、push body `tiled:false` ⇒ 完全旧口径（bit1=0）。
- 导出清单（manifest-assets.json）为分块包额外写 `"layout":"tiled"` 字符串（**仅服务端幂等判定
  与排障用**，v:v 固件不读该字段；逐行包不写）。作用：让"已登记但仍是逐行口径"的整图条目被判为
  待重导，否则新固件永远拿不到分块包。

### 9.2 固件侧必须同时处理的三件事（服务端读 `Firmware/board216/main/render/mpak.c` 得出）

1. `parse_bgmap` 的 geometry 白名单
   （`static_tight` / `static_aligned` / `tile_expect` / `tile_expect_enc`）在**读尾扩展块之前**执行
   ⇒ 分块长度会被 `MPAK_ERR_FMT` 直接拒收。必须先取到 `flags`（或把 `gx*gy*32768` /
   `gx*gy*32768+gx*gy*2048` 纳入白名单）再判几何。
2. `parse_parts` 的 `has_alpha` 推断只认 `extent == pixel_bytes` 或
   `pixel_bytes + mask_raw .. +mask_al+3` ⇒ 分块记录（更长且非该两式）会让**条带小包**被拒
   （PARTS 包自身没有标志位，这是 §4 的直接后果）；需按"引用它的 BGMAP flags bit1"放行。
3. 分块包的掩码偏移不再是 `tile_layer_off + vh*align4(vw*2)`，而是
   `tile_layer_off + gx*gy*32768`；条带像素寻址不得再用 `row_bytes`（§5）；
   `mpak_bgmap_read_tile_mask_rect` 的"全局 bit 号 = y*vw + x"只适用于逐行包。

> 对拍与读放大统计脚本：`Server/tools/bgmap-tiled-verify.py`（整幅逐像素 + 随机 2000 窗口 §6
> 字面取值 + 条带包逐像素；并给出 288×288 窗口两种格式的读次数/字节数）。
