/*
 * font_lazy.h — FONT 包 → LVGL lv_font_t 懒加载字体
 *
 * 三实例（16/24/32px，各自独立 FONT 包）。字形元数据随 mpak 索引常驻
 * PSRAM；字形位图 4bpp（行按字节对齐 = LVGL A4 兼容）按需 TF 懒读进
 * 每实例 16 槽环形缓存。LVGL 绘制是同步消费 bitmap 指针，环形缓存安全。
 *
 * 约定：与 render_tick 同任务调用。
 */
#ifndef RENDER_FONT_LAZY_H
#define RENDER_FONT_LAZY_H

#include <stdint.h>
#include <stdbool.h>

#include <lvgl.h>

#include "mpak.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FONT_ID_16 = 0,
    FONT_ID_24 = 1,
    FONT_ID_32 = 2,
    FONT_ID_COUNT
} font_id_t;

/* 打开 FONT 包并装配 lv_font_t 实例（重复 init 会先关闭旧的） */
int  font_lazy_init(font_id_t id, const char *mpk_path);
void font_lazy_deinit(font_id_t id);

/* 实例指针（未 init 返回 NULL；气泡/菜单可用） */
const lv_font_t *font_lazy_get(font_id_t id);
bool font_lazy_ready(font_id_t id);

/* 供气泡排版测量：返回字前进宽度（px）；缺字返回 0 */
int font_lazy_measure(font_id_t id, uint32_t codepoint, uint32_t *adv_w);

#ifdef __cplusplus
}
#endif

#endif /* RENDER_FONT_LAZY_H */
