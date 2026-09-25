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

/* 实体缓冲：世界 100×130 窗口 × 2 */
#define RC_ENT_W            200
#define RC_ENT_H            260
#define RC_ENT_WIN_W        100
#define RC_ENT_WIN_H        130
#define RC_ENT_COV_BYTES    ((RC_ENT_W * RC_ENT_H + 7) / 8)

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
