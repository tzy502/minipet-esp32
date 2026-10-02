/*
 * compositor.h — 自研合成器内部接口（render/ 模块间使用）
 *
 * 对外 API 见 render.h（本模块实现之）。合成管线（software-design 4.2）：
 *   compose 顺序：static_back → 条带(循环平铺) → tile_layer → 时钟 → 实体 → 气泡
 *   脏区：16×16 网格 hash diff → 行程合并 → display_blit
 * framebuffer 单一所有权归本模块；display_blit 仅在此发出（单写屏者）。
 */
#ifndef RENDER_COMPOSITOR_H
#define RENDER_COMPOSITOR_H

#include <stdint.h>
#include <stdbool.h>

#include "mpak.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 480 屏 = 世界 1x × 2（PARTS 包内存 1x，设备端 nearest 放大） */
/* 【A1 定稿 2026-10-01】世界 1x → 合成器 1x（原为 2x）。
 * 与 profile 360 空间配合 = 资产像素 ↔ 面板像素**真 1:1**（驱动层恒等映射）。
 * ⚠️ 本值必须是整数（全代码用 <<RC_SCALE_SHIFT / >>RC_SCALE_SHIFT 位移），
 * 所以"1.5x 保持观感"这条路在本工程里不存在——要么 1x（1:1）要么 2x（丢样）。
 * 改动连带：世界视场 = 屏/RC_SCALE（240→360）、实体展开 1:1、条带 1:1、
 * 站位/拖拽/时钟锚点换算全部随之变化（均为统一口径，不需逐处改）。 */
#define RC_SCALE            1
#define RC_SCALE_SHIFT      0

/*
 * 实体缓冲（像素 = 屏幕像素；内容为世界 1x 经 2x nearest 展开后的图）。
 * 尺寸取「摆放规则的理论上限」：显示宽 = 画布宽*2 clamp ≤ 屏宽 480 → 世界宽 ≤240；
 * 底部对齐留 40px 边距 → 显示高 ≤ 480-40=440 → 世界高 ≤220。
 * 超出此世界的画布（极端特效件）在缓冲边缘裁剪（与旧 200×260 窗口同策略，窗口更大）。
 */
/* 实体（娃娃）暂存画布：合成器空间大小。360 空间下改为 360×360
 * （原 480×440 是为 480 空间留的余量；1:1 后不再需要放大余量）。 */
#define RC_ENT_W            360
#define RC_ENT_H            360
#define RC_ENT_COV_BYTES    ((RC_ENT_W * RC_ENT_H + 7) / 8)

/* ------------------------------------------------------------------------
 * 实体摆放调参区（问题2 集中调参）
 * 摆放语义：世界 1x 的 body 锚点 (0,0)（LAYOUT piece x/y 的原点 = 人物脚底
 * 基准）经 2x nearest 后——
 *   水平 → 屏幕中心 + RC_ENT_CENTER_OFF_X（正=右移）
 *   垂直 → (屏底 - RC_ENT_MARGIN_B) + RC_ENT_CENTER_OFF_Y（正=下移）
 * 画布联合包围盒只决定缓冲窗口；人物定位不再按包围盒居中/贴底，
 * 武器/翅膀等大件撑大包围盒不再把人物挤偏（旧症状：偏左上）。
 * ------------------------------------------------------------------------ */
#define RC_ENT_MARGIN_B        40   /* 脚底距屏底（屏幕 px） */
#define RC_ENT_CENTER_OFF_X    0    /* 水平居中微调（屏幕 px） */
#define RC_ENT_CENTER_OFF_Y    0    /* 垂直微调（屏幕 px） */

/* 倾斜（IMU 与触摸拖拽共用通路）→ 实体 x 偏移可见反馈（问题6/7）：
 * offset_px = tilt_deg × RC_TILT_ENT_PX_PER_DEG，clamp ±RC_TILT_ENT_MAX_PX */
#define RC_TILT_ENT_PX_PER_DEG 1
#define RC_TILT_ENT_MAX_PX     8

/* 脏区网格 */
#define RC_CELL             16
#define RC_GRID_MAX         (32 * 32)   /* 支持屏 ≤ 512×512 */
#define RC_MAX_W            512         /* 屏宽上限（render_init 同口径；屏幕列映射表用） */

/* 部件位图缓存上限（PSRAM；当前装扮全部引用件 + 25 表情变体典型 <1MB） */
#define RC_PART_CACHE_CAP   (2u * 1024u * 1024u)

/* IMU 倾角→条带视差：offset_px = deg * rx/100 * RC_TILT_PX_PER_DEG */
#define RC_TILT_PX_PER_DEG  4

/* 气泡位图上限（RGB565，不透明矩形） */
#define RC_BUBBLE_MAX_W     340   /* 360 空间下留边距（原 460 配 480 空间） */
#define RC_BUBBLE_MAX_H     160

/* 未配网常驻横幅（问题4：POKER 态顶部 480×28 深色底白字，5x7 字体 ×2）
 * 字形加 1px 黑描边（问题3：浅色地图/条带上白字可读性） */
#define RC_BANNER_H         28
/* 【用户反馈 2026-09-27】横幅原先贴 y=0 画，真机被 AMOLED 圆角/边框切掉
 * （照片里只剩一条发光边，"肉眼看不到"）。下移到圆角安全区。 */
#define RC_BANNER_Y         46
#define RC_BANNER_SCALE     2
#define RC_BANNER_PAD_X     8
#define RC_BANNER_PAD_Y     7      /* (28 - 7*2)/2，垂直居中 */
#define RC_BANNER_BG        0x2104 /* 深色底（#102020） */
#define RC_BANNER_FG        0xFFFF /* 白字 */
#define RC_BANNER_OUTLINE   0x0000 /* 1px 黑描边 */

/* 【E9】CLOCK_DOZE 睡眠态宠物亮度（百分比）：需求「宠物睡眠态 + 数字时钟 +
 * AMOLED 纯黑背景只数字发光（省电）」——时钟态把宠物降亮合成，既看得见宠物
 * 睡着，又保持黑底省电口径。 */
#define RC_SLEEP_DARKEN     35

/* 1bit 掩码位序：MSB first（字节内 bit7 为首像素） */
static inline bool rc_mask_bit(const uint8_t *mask, uint32_t idx)
{
    return (mask[idx >> 3] >> (7 - (idx & 7))) & 1u;
}
static inline void rc_mask_set(uint8_t *mask, uint32_t idx)
{
    mask[idx >> 3] |= (uint8_t)(1u << (7 - (idx & 7)));
}
static inline uint32_t rc_align4(uint32_t n) { return (n + 3u) & ~3u; }

/* ══════════════════════════════════════════════════════════════════════════
 * R2 整图相机（契约 docs/ai/map-fullmap-firmware-contract.md §3.2）
 *
 * 语义：整图包（vw/vh ≫ 屏）下相机可任意平移；相机 = **可见窗口左上角的世界坐标**
 * （整图世界系，原点 = bbox 左上角）。窗口尺寸 = 屏/RC_SCALE（480 屏 → 240×240 世界 px）。
 * 渲染硬约束：世界 1x → 屏 2x **整倍最近邻**（零插值、零半像素、比例恒定）；
 * static/tile 由 PSRAM 窗口缓存流式供给（拖动只补新露出的边条，相机静止零 TF 读）。
 * 旧包（非整图）= 不支持相机（render_cam_supported()==false），原路径视觉不变。
 *
 * ⚠️ 相机 UX 层（F3）请 #include "compositor.h" —— render.h 不转出本组接口。
 * ⚠️ render_set_map() 装载整图包后相机置中（= 服务端导出参考相机）；
 *    NVS 相机必须在 render_set_map 返回之后再 render_cam_set() 应用。
 * ══════════════════════════════════════════════════════════════════════════ */

/* 窗口缓存尺寸（世界 px）：可见窗口 240×240 + 两侧各 48 余量。
 * 余量内的小幅拖动 = 纯命中（零 TF 读）；超出后只补新露出的边条。 */
/* ══ 【缓存尺寸 A1 修正 2026-10-01：288 → 360（= 屏宽）】══════════════════════
 * ⚠️ 硬约束：**RC_CAM_CACHE_W/H 必须 ≥ 可见窗口 g_cam_fov_w/h = 屏宽/RC_SCALE**，
 * 否则 `cam_scene_load` 直接 `return RENDER_ERR_UNSUPPORTED`（真机表现：地图装载
 * rc=-103，**背景全黑、地图完全不显示**）。
 * 历史：本常量原是 336→288（为 480 空间下"可见窗口 240 + 余量"省装载行数，见原注释：
 * 装载/切图时间 ∝ 缓存行数，8 条带图 5892 行 ≈10.2s，SD 有效吞吐 ~230KB/s 是硬顶）。
 * 但 A1 把合成器收到 360 且 RC_SCALE=1 → 可见窗口从 240 变成 **360** > 288
 * → 触发上面那条硬约束 → 地图再也装不上（用户报障"地图无法显示了"的真因）。
 * 现取 360×360：满足约束且与屏同宽（余量 0：1:1 后没有放大余量可留，拖动靠补边条）。
 * 内存：每层像素 253KB + 掩码 15.8KB ≈ 269KB（原 172KB/层）。 */
#define RC_CAM_CACHE_W      360
#define RC_CAM_CACHE_H      360

/* 整图包（含可平移余量）→ true；旧窗口包/无地图 → false */
bool render_cam_supported(void);
/* 相机可平移范围（世界 px，含 0）：max = 图尺寸 − 可见窗口（不足则 0） */
void render_cam_range(int32_t *max_dx, int32_t *max_dy);
/* 设置相机（越界自动夹取；图小于窗口时居中）。变化 → 整屏标脏，下一帧整屏重合成。
 * 读回实际值用 render_cam_get()。 */
void render_cam_set(int32_t world_x, int32_t world_y);
void render_cam_get(int32_t *world_x, int32_t *world_y);
/* 相机置中（= 服务端整图导出参考相机；与 _ref 参考图同相位） */
void render_cam_center(void);

/* 站位地面线联动：用整图地面表（mpak_bgmap_ground_y）算 screen_x 处宠物脚踩的
 * 屏幕 y（已按 2× 与当前相机换算，并夹进画面 [0, 屏底-RC_GROUND_UP_PX]）。
 * 返回 -1 = 无地面表/越界/非整图（调用方回落通用线 ground_line_y_at 的口径不变）。 */
int32_t render_ground_screen_y(int32_t screen_x);

/* ══════════════════════════════════════════════════════════════════════════
 * 【相机调参性能模式（两级降级）2026-10-01 二次定稿】
 *
 * 用户口径（原话）："调整摄像头的时候卡顿很严重很严重"；第一版只做了
 * "跳条带层"（level 1）后**仍然卡**。真机耗时构成（见 compose_region/flush_dirty
 * 的哨兵日志）决定了单靠跳条带不够——拖动每一拍仍要付：
 *   · static 全屏 480×480 采样（窗口缓存未命中处还要补边条 = 每 48 世界 px
 *     一次 336 行 × 逐行 fseek+fread 的小块读，SD 上 ≈ 100ms 级尖峰）；
 *   · tile 层逐像素 1bit 掩码判定（230K 次位测试）；
 *   · 上屏 460KB 分块字节交换 + SPI（这一项无论怎么降级都省不掉）。
 * 所以再加一档"极限档"，把**拖动进行中**的那几帧压到只剩 static 底图 + 上屏：
 *
 *   level 0 = 正常：static → 条带 → tile（逐像素掩码）→ 时钟 → 实体 → 气泡 → 横幅。
 *             **与加本功能之前的整图渲染逐像素一致**（不许动这条路径）。
 *   level 1 = 跳条带：整图条带层合成与窗口缓存同步都跳过（= 上一版行为，
 *             条带是最贵的一层：每条带一份 336×N 缓存 + 掩码 + 相位）；
 *             static + tile 照常。语义：整段调参态的**静止**底色。
 *   level 2 = 极限：只画 static 底图（tile/条带全跳），且**窗口缓存零 TF 读**
 *             （cam_scene_sync 整段跳过）——缓存没覆盖到的边缘改用"最近列延展"
 *             而不是黑边（纯黑带比"拉伸的边缘像素"刺眼得多，且松手后 200ms
 *             就回 level 1 补回真内容）。相机位移由 UX 层按大步长吸附（见
 *             lvgl_bridge.c 的 CAM_PAN_SNAP_PX），避免每 1 世界 px 一次整屏重合成。
 *
 * 自动回落（"松手出全图"）：拖动停止 200ms → 自动回 level 1（条带仍未画，
 * 但 tile/条带缓存补齐内容）；再静止 300ms → 自动回 level 0 并整屏重合成一次，
 * 条带层重新出现 = 用户看到的"松手出全图"。确认/取消/被打断（cam_finish_core）
 * 直接 set(0)。回落由 render_tick 内的 cam_adjust_tick() 驱动（渲染任务上下文），
 * 不需要任何 input 侧定时器。
 *
 * 语义：纯渲染降级，不动任何持久化状态；每次 level 变化都整屏重合成一次
 * （level 2 除外：进 level 2 必然伴随一次相机平移 = 整屏已标脏，不必重复合成）。
 *
 * 【2026-10-01 扁平缓冲（R3）后的档位口径 —— 与上面"不降级"口径一致】
 * 地图扁平缓冲（static+条带+tile 一次压平成一张 480×480 背景图，相机移动只
 * 平移 + 重算新露出的边条，见 compositor.c 的「地图扁平缓冲」模块）落地后，
 * 整图路径上 **level 0 与 level 1 已经等价**（都是全层渲染；条带不再是"每帧
 * 重算全屏"的成本项，只剩相位推进时"该带所在行"的一次重算）⇒ 调参路径请固定
 * level 0；level 1/2 仅作排障兜底保留。level 2 会主动把扁平缓冲置失效
 * （它只画 static，内容不完整），退出后下一帧整幅重建一次。
 * ══════════════════════════════════════════════════════════════════════════ */
#define RC_CAM_ADJ_OFF      0
#define RC_CAM_ADJ_NO_STRIP 1   /* 跳条带层 */
#define RC_CAM_ADJ_MINIMAL  2   /* 极限：只画 static 底图 + 零 TF 读 */
#define RC_CAM_ADJ_L2_IDLE_MS 200   /* 拖动停止 → 2 回 1 */
#define RC_CAM_ADJ_L1_IDLE_MS 300   /* 再静止 → 1 回 0（松手出全图） */

/* 设置降级档（>0 时进调参性能模式；越界自动夹到 [0,2]）。
 * 注意：level 0 与"进调参态之前"的渲染路径完全一致。 */
/* 【装载期直落相机 2026-10-01】用户口径"启动渲染卡住 / 切图很久"。
 * 真机取数（000010000 整图包）：装载后先在**置中相机**填一遍窗口缓存（10.7s），
 * 紧接着 NVS 相机一应用，锚点跨度过大 → rc_shift_plan 判 RELOAD → **整窗重填**
 * （14.2s）⇒ 开机光缓存填充就 ~25s。
 * 用法：装载前调 render_cam_set_pending(x,y)（无记忆则不调），cam_scene_load
 * 会直接以该相机为初始位置，只填一次；随后 render_cam_set 到同一位置时
 * 命中缓存（零 IO）。 */
void render_cam_set_pending(int32_t x, int32_t y);
void render_cam_clear_pending(void);

void render_cam_adjust_set(int level);
int  render_cam_adjust_get(void);
/* UX 层每下发一次相机位移就调一次：进/续 level 2（拖动期极限档）。
 * 由渲染任务按 RC_CAM_ADJ_L2_IDLE_MS 自动回落到 level 1 → level 0。 */
void render_cam_adjust_motion(void);
/* 【调参期不降级】只记录"手指在动"的时戳（供渲染层做松手判定/忙窗），
 * **不改变渲染内容**——用户定稿：调参时背景必须正常全层渲染。 */
void render_cam_adjust_motion_notify(void);

/* ── 忙判定（输入侧丢弃按键/触摸的判据）─────────────────────────────────
 * 用户口径："正在做整屏重合成/相机吸附/地图装载的窗口内丢弃按键与触摸，
 * 不要排队、不要延后执行"。render_busy() 为真的三种来源：
 *   ① 重活深度：地图装载 / 整屏重合成 / 半屏以上脏区 flush 期间（可嵌套计数）；
 *   ② 忙尾：最后一次重活结束后的 RC_BUSY_TAIL_MS 内（卡顿刚过的那一下也丢，
 *      否则用户"卡完再按"的那一下正好落进队列形成积压）；
 *   ③ 相机调参极限档（= 正在拖动/吸附）或显式 loading 窗口（相机收尾重派发地图）。
 * 只读、可从任意任务调用（内部全是 volatile 标量）。 */
#define RC_BUSY_TAIL_MS     200
bool render_busy(void);
/* 重活窗口（可嵌套；只在渲染任务/持有 rc_lock 的路径调用）。why = 横幅文案前缀。 */
void render_busy_enter(const char *why);
void render_busy_leave(void);
/* 显式 loading 横幅（相机收尾/地图装载窗口；UX 层开关，on=false 立即结束忙窗）。
 * 文案 = 5x7 ASCII 大写（横幅字库只有 ASCII，中文会渲染成 '?'）。 */
void render_busy_banner(const char *why, bool on);

#ifdef __cplusplus
}
#endif

#endif /* RENDER_COMPOSITOR_H */
