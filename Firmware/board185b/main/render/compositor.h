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
#define RC_SCALE            2
#define RC_SCALE_SHIFT      1
/* ══ 【世界→屏 比例 · 可调（2026-10-01）】════════════════════════════════════
 * 语义：世界 1x 像素 → 合成器空间像素。Q16 定点，便于表达 1.5 这类非整数倍。
 *   2.0（默认）  = 与 216 板同一契约（世界 1x → 屏 2x）
 *   1.5          = 整体缩到 2.0 的 0.75（用户口径"展示内容缩小 1.33 倍"）
 * 影响面：娃娃实体展开/落点、条带展开、地面/站位换算——凡是世界坐标上屏的地方。
 * 注意：地图 static/tile 的 240→480 是**独立**缩放（layer_rgb_load 内部按
 * vw→g_sw 最近邻），要一起缩需同步改那里，否则地图与娃娃比例会不一致。 */
#ifndef RC_ENTITY_SCALE_Q16
/* 【定稿 2026-10-01：保持 2.0x = 与 216 同契约】曾试过 1.5x（用户口径"整体缩
 * 0.75"），真机实测体验更差（娃娃变小但地图没跟着缩、画面留白、底被裁），
 * 用户拍板撤销 → 回到 1:1 观感（世界 1x → 屏 2x，再由驱动 0.75x 上 360 面板）。
 * 旋钮保留：需要再试别的比例时只改这里（例如 1.5x 写 (3*65536/2)）。 */
#define RC_ENTITY_SCALE_Q16 (2 * 65536)         /* 2.0x（定稿） */
#endif
#define RC_ESCALE(v)        ((int32_t)(((int64_t)(v) * RC_ENTITY_SCALE_Q16) >> 16))
/* 世界长度 → 屏长度（四舍五入，避免 1.5x 下累积欠一像素） */
#define RC_ESCALE_R(v)      ((int32_t)((((int64_t)(v) * RC_ENTITY_SCALE_Q16) + 32768) >> 16))

/*
 * 实体缓冲（像素 = 屏幕像素；内容为世界 1x 经 2x nearest 展开后的图）。
 * 尺寸取「摆放规则的理论上限」：显示宽 = 画布宽*2 clamp ≤ 屏宽 480 → 世界宽 ≤240；
 * 底部对齐留 40px 边距 → 显示高 ≤ 480-40=440 → 世界高 ≤220。
 * 超出此世界的画布（极端特效件）在缓冲边缘裁剪（与旧 200×260 窗口同策略，窗口更大）。
 */
#define RC_ENT_W            480
#define RC_ENT_H            440
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
#define RC_BUBBLE_MAX_W     460
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
#define RC_CAM_CACHE_W      336
#define RC_CAM_CACHE_H      336

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

#ifdef __cplusplus
}
#endif

#endif /* RENDER_COMPOSITOR_H */
