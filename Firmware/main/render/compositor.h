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

/* 部件位图缓存上限（PSRAM；当前装扮全部引用件 + 25 表情变体典型 <1MB） */
#define RC_PART_CACHE_CAP   (2u * 1024u * 1024u)

/* IMU 倾角→条带视差：offset_px = deg * rx/100 * RC_TILT_PX_PER_DEG */
#define RC_TILT_PX_PER_DEG  4

/* 气泡位图上限（RGB565，不透明矩形） */
#define RC_BUBBLE_MAX_W     460
#define RC_BUBBLE_MAX_H     160

/* 未配网常驻横幅（问题4：POKER 态顶部 480×28 深色底白字，5x7 字体 ×2） */
#define RC_BANNER_H         28
#define RC_BANNER_SCALE     2
#define RC_BANNER_PAD_X     8
#define RC_BANNER_PAD_Y     7      /* (28 - 7*2)/2，垂直居中 */
#define RC_BANNER_BG        0x2104 /* 深色底（#102020） */
#define RC_BANNER_FG        0xFFFF /* 白字 */

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

#ifdef __cplusplus
}
#endif

#endif /* RENDER_COMPOSITOR_H */
