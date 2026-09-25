/*
 * lvgl_bridge.h — LVGL 9 与自研合成器的合流桥（software-design.md 4.2 定稿）
 *
 * framebuffer 单一所有权归合成器；本桥持有唯一 lv_display：
 *   POKER：2 × 1/10 屏部分缓冲（PARTIAL）——LVGL 不写屏；气泡经
 *          lv_refr_now 同步离屏渲染，flush 切片捕获进气泡位图
 *   MENU ：整屏 450KB 离屏缓冲（DIRECT）——LVGL 整屏渲染，合成器
 *          让路，按 flush 区域直拷 framebuffer 后 display_blit
 * 同一时刻仅一个写屏者（display_blit 只出现在合成器）。
 */
#ifndef RENDER_LVGL_BRIDGE_H
#define RENDER_LVGL_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回码与 render.h 的 RENDER_ERR_* 一致 */
int  bridge_init(int32_t screen_w, int32_t screen_h);
void bridge_deinit(void);

/* MENU ⇄ POKER 切换（切换 draw buffer 与渲染模式） */
int  bridge_mode_menu(void);
int  bridge_mode_poker(void);
bool bridge_is_menu(void);

/* 菜单整屏离屏缓冲（合成器直拷源） */
const uint16_t *bridge_menu_buf(void);

/* 唯一 lv_display（app 菜单 UI 挂载；enter_menu 后使用） */
lv_display_t *bridge_display(void);

/*
 * 取走本 tick LVGL flush 过的菜单区域（bbox）；无脏区返回 false。
 * 仅 MENU 模式有效。
 */
bool bridge_menu_take_dirty(int32_t *x, int32_t *y, int32_t *w, int32_t *h);

/*
 * 气泡离屏渲染（POKER 专用，同步）：
 *   text/font_id → 自动换行排版 → lv_refr_now 渲染 → flush 切片捕获为
 *   RGB565 位图（不透明矩形；圆角/尾巴为 P2 项）。
 * dst 行宽 = dst_stride（= 最终 out_w，调用方按 max 尺寸分配）。
 * 返回 0 成功并给出 out_w/out_h。
 */
int  bridge_bubble_render(const char *text, int font_id,
                          uint16_t *dst, int32_t dst_stride,
                          int32_t dst_max_w, int32_t dst_max_h,
                          int32_t *out_w, int32_t *out_h);

#ifdef __cplusplus
}
#endif

#endif /* RENDER_LVGL_BRIDGE_H */
