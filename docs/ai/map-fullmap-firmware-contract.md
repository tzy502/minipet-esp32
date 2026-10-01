# 整图地图（R2）固件实现契约（2026-10-01）

> 目的：把「服务端整图 BGMAP」落到 board216 固件的**冻结接口**，供三个并行 agent
> （mpak 层 / compositor 层 / 相机 UX 层）同时施工，避免接口漂移。
> 上游依据：`docs/ai/map-feature-requirements.md` §5、服务端 agent 实测报告（本文 §1/§2）。
> 状态：**接口冻结**（要改必须由主线程改本文件）。

---

## 一、wire 事实（服务端已实现并实测，000010000）

| 项 | 值 |
|---|---|
| 包体 | BGMAP **16,925,016 B**（40B 信封 + payload 16,924,968 + 8B 尾） |
| `vw`/`vh` | **2270 × 1807**（整图世界尺寸，1x，零缩放） |
| `static_back` | 8,203,780 B @ payload+112（= vh × align4(vw×2) = 1807×4540） |
| `tile_layer` | 8,716,520 B @ payload+8,203,892（RGB565 行对齐 + 1bit mask 整块补 4B） |
| 扩展块（**tile 之后**，4B 对齐） | `magic u32=0x4D504745`（文件字节 `45 47 50 4D`）`ground_len u32=4540` `ground_off u32` `flags u32=0x1` |
| 地面表 | vw × **u16 小端**（世界系 y，`0xFFFF`=该列无 foothold）@ payload+16,920,428 |
| 条带 | 仍 4 条、仍是「一个循环周期宽 × 带高」的独立 PARTS 包；**`y` 改为带图顶边在整图世界系的 y** |
| 世界系原点 | **bbox 左上角 = (0,0)**（服务端导出相机=整图中心裁 bbox；设备只有 vw/vh，无 MinX/MinY） |

**条带清单（000010000，参考）**：枫树 850×222 y=681；丘陵段带 2270×260 y=735 rx=5；
白云 613×125 y=771；远景段带 2270×508 y=787 rx=12。
**带宽 == vw 的条带必须按「世界对齐层」整幅定位绘制**（沿用周期平铺会相位错）。

**旧包（240×240 窗口口径）行为必须逐字节不变**：`flags` 缺位（无扩展块）、`vw/vh=240`、
`strip.y` 仍是视口内 y。

---

## 二、固件侧四道硬门槛（服务端 agent 读 mpak.c 得出，必须全部处理）

1. `envelope_check`：`payload_len > MPAK_MAX_PAYLOAD`（当前 8MB）→ 16.9MB 直接 `MPAK_ERR_LEN`。
2. `parse_bgmap`：`vw>512 || vh>512` → `MPAK_ERR_FMT`。
3. `parse_bgmap`：`static_back_len != vw*vh*2` 严格相等（整图行 4B 对齐后 = `vh*align4(vw*2)`，奇数宽地图不等）。
4. `mpak_bgmap_read_static/read_tile` 是「一次给一整层缓冲」的语义 → 8.2/8.7MB，PSRAM 装不下，**必须分块流式**。
   （另：`envelope_check` 会做**全 payload CRC32C**，17MB 每次 open 全读一遍 ≈ 数秒级，必须能跳过。）

---

## 三、冻结接口

### 3.1 mpak 层（`main/render/mpak.h` 声明，`mpak.c` 实现）

```c
/* 包能力上限（#define，放 mpak.h）：
 *   MPAK_MAX_PAYLOAD        8MB → 64MB（整图 BGMAP 16.9MB）
 *   MPAK_MAX_BGMAP_DIM      512 → 8192（vw/vh 上限）
 * static_back_len 合法值集合 = { vw*vh*2（旧，紧打包）,
 *                                vh*align4(vw*2)（整图，行 4B 对齐）} */

/* 大包 CRC 跳过：> MPAK_CRC_SKIP_BYTES（建议 4MB）时不做全量 CRC32C，
 * 改为「信封+payload 头 4KB 抽样 CRC」或直接跳过并在日志标注（下载侧 asset_dl 已校验）。
 * 语义变化必须写进 mpak.c 注释（为什么安全）。 */

/* —— 整图扩展（解析期填充）—— */
typedef struct {
    bool     full_map;       /* flags bit0 */
    uint32_t ground_len;     /* vw*2 */
    uint32_t ground_off;     /* payload 相对 */
} mpak_bgmap_ext_t;          /* 挂到 mpak_bgmap_t：ext + ground 指针 */

/* 世界系地面 Y：x 越界或 0xFFFF → 返回 INT32_MIN（调用方回落通用线） */
int32_t mpak_bgmap_ground_y(const mpak_t *m, int32_t world_x);

/* —— 分块读（任意窗口；唯一新增 IO 原语，内部走 mpak_read_at）——
 * layer: 0=static_back(RGB565)  1=tile_layer(RGB565 + 1bit mask 分块交错，见下)
 * 语义：读世界矩形 [x, x+w) × [y, y+h) 到 dst；
 *   · 越界部分补 0（调用方按 vw/vh 自行裁剪，越界容错不报错）；
 *   · dst 行距由调用方给出（px 计，不是字节）；
 *   · tile 掩码单独走 mpak_bgmap_read_tile_mask()（避免一次读两层）。
 * 返回 0=OK，负=MPAK_ERR_*。 */
int mpak_bgmap_read_static_rect(const mpak_t *m, int32_t x, int32_t y,
                                int32_t w, int32_t h, uint16_t *dst, int32_t dst_stride_px);
int mpak_bgmap_read_tile_rect(const mpak_t *m, int32_t x, int32_t y,
                              int32_t w, int32_t h, uint16_t *dst, int32_t dst_stride_px);
int mpak_bgmap_read_tile_mask_rect(const mpak_t *m, int32_t x, int32_t y,
                                   int32_t w, int32_t h, uint8_t *dst /*每像素 1B，0/1*/,
                                   int32_t dst_stride_px);
```

`mpak_bgmap_t` 新增字段（**只增不改**，避免破坏现有读者）：
`bool full_map; uint32_t ground_off, ground_len; int32_t ext_off;`

### 3.2 compositor 层（`main/render/compositor.h` 声明，`compositor.c` 实现）

```c
/* 相机能力查询（相机 UI 用它决定入口是否可用/滑杆范围）：
 *   返回 false = 当前图不是整图包（无可平移余量）→ 相机子页提示"此图不支持相机" */
bool render_cam_supported(void);
void render_cam_range(int32_t *max_dx, int32_t *max_dy);   /* 世界 px（含 0） */

/* 相机状态 = 可见窗口左上角的世界坐标（世界系见 §1）。
 * 越界由实现夹取；窗口尺寸 = 屏宽/2 × 屏高/2（480 屏 → 240×240 世界 px）。
 * set 立即重合成（整屏脏区）；返回夹取后的实际值。 */
void render_cam_set(int32_t world_x, int32_t world_y);
void render_cam_get(int32_t *world_x, int32_t *world_y);
void render_cam_center(void);          /* 置中（= 服务端导出参考相机） */

/* 站位地面线联动：相机移动后渲染层用整图地面表重算「宠物脚踩的 y」。
 * 返回屏幕 y（px，已按 2× 缩放与当前相机换算）；无地面表/越界 → -1（调用方回落通用线）。 */
int32_t render_ground_screen_y(int32_t screen_x);
```

**渲染语义（硬约束）**
1. 世界像素比例恒定：地图 1x → 屏 2×（整倍，零插值；与桌宠 RenderViewport(zoom=2) 同口径）。
2. static/tile 都从 TF **分块**取（PSRAM 常驻窗口缓存，建议 ≥ 屏可视窗口 + 余量，例如 336×336 世界 px）；
   拖动时只补新露出的边条（增量读），相机静止时**零 TF 读**。
3. 条带：`y` 按世界系定位（`screen_y = (y - cam_y) * 2`），水平按 `speed_x`（时间轴自走）与
   `rx_parallax`（相机偏移×系数）合成；**带宽==vw 的带整幅世界对齐绘制**。
4. 旧包（无 ext/`vw==240`）走原路径，视觉与现在完全一致。
5. 内存预算：新增 PSRAM 常驻 ≤ 1.2MB（整图方案首期），不得挤爆 8MB（现有 LVGL 菜单缓冲 460KB + 帧缓冲 460KB）。

### 3.3 相机 UX 层（`main/render/lvgl_bridge.c` + `main/app/input_dispatch.c` + NVS）

- 入口：`menu_map_fn_camera_hook()`（lvgl_bridge.c:882，菜单 agent 已留 hook）。
- 调参态语义（需求 §5.4）：进入 → 进"相机模式"（宠物隐藏或冻结，防拖拽歧义）；
  拖动=平移相机（跟手）；中键短按=确认保存（NVS + 重合成）、长按=取消（恢复进入前状态）；
  收尾 → 宠物固定回归屏幕正中间、脚踩 `render_ground_screen_y(屏心)` 的地面线。
- NVS：namespace **`cam`**，key = per-map 键（与菜单 agent 的 `hide_key_of()` 同口径，≤15 字符），
  value = 两个 int32（x,y）——建议 `nvs_set_blob` 8B 或两键 `x`/`y` 前缀；实现自选，写进注释。
- 装载地图（`state_machine` 的 `dispatch_map` 成功路径）→ 读 NVS 应用 = "全局加载"。
- 旧包不支持相机 → 子页提示「此图不支持相机（需整图包）」，不进入调参态。

---

## 四、施工分工（文件独占，禁止越界）

| agent | 独占文件 | 交付 |
|---|---|---|
| **F1 mpak** | `main/render/mpak.c` `main/render/mpak.h` | §3.1 全部；旧包回归不变 |
| **F2 compositor** | `main/render/compositor.c` `main/render/compositor.h` | §3.2 全部；旧包视觉不变 |
| **F3 相机 UX** | `main/render/lvgl_bridge.c` `main/app/input_dispatch.c` `main/app/state_machine.c` | §3.3 全部 |

**共同纪律**
- 编译隔离：只用 `idf.py -B build-<自己的名字> build`（主线程在用 `build` / `build-bgm`，菜单 agent 用 `build-menu`）。
- 串口/烧录归主线程：`/dev/cu.usbmodem21201` 不要占用；要真机验证先汇报。
- 不许改 `Firmware/board185b/**`；不许 git commit。
- 真机证据优先：每条结论要么来自你跑过的命令/日志，要么标注「未实测」。

---

## 五、验收锚点（真机可 grep）

| 场景 | 期望日志 |
|---|---|
| 整图包装载 | `mpak: bgmap 000010000 vw=2270 vh=1807 full_map=1 ground=4540B`（示例，TAG/格式自定但必须可判） |
| 旧包装载 | 与现状同（无 full_map 字样 / full_map=0） |
| 相机可用性 | `rc: 相机支持=1 范围 dx[0,2030] dy[0,1567]`（示例） |
| 平移 | `rc: 相机 → (x,y) 窗口读 %d 行（%d ms）`（增量读证据） |
| NVS | `sm: 地图 000010000 应用相机 (x,y)` / 确认保存 / 取消恢复 |
| 地面线 | `rc: 地面线来源：整图地面表 world_x=%d → world_y=%d`（现有日志的扩展） |
