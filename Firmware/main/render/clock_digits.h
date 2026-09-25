/*
 * clock_digits.h — WZ 地图时钟（fontTime 部件按 PARTS 包导出，part_id 900..912）
 *
 * 排布参数按 clock-display-spec.md 定稿值固件硬编码：
 *   起点 = clock 锚点 + (18+3, 83)（世界 1x px，480 屏合成时 ×2）
 *   am|pm(22) → [AMPM_GAP=12px] → H1(26) → H2(26) → comma(17) → M1(26) → M2(26)
 * 时间规则（对齐官方客户端）：
 *   12h 制；0~11=AM、12~23=PM；13~23 显示减 12；午夜显示 AM 00:xx（不做 12 修正）
 *   comma 偶秒显示、奇秒隐藏（1s 周期）
 * 时间源：C 库 time()（RTC，断网走时）。
 * 每秒由合成器标记时钟小脏区，comma 闪烁与分进位经 16×16 网格 hash 过滤。
 */
#ifndef RENDER_CLOCK_DIGITS_H
#define RENDER_CLOCK_DIGITS_H

#include <stdint.h>
#include <stdbool.h>

#include "mpak.h"

#ifdef __cplusplus
extern "C" {
#endif

/* fontTime 保留段 part_id（algorithm-asset-format.md 七.5 / R6） */
#define CLOCK_PART_DIGIT_BASE 900u  /* 900..909 = '0'..'9' */
#define CLOCK_PART_AM         910u
#define CLOCK_PART_PM         911u
#define CLOCK_PART_COMMA      912u

#define CLOCK_OFF_X   21   /* 18 + 3（官方偏移 + 胶水右移微调），世界 1x px */
#define CLOCK_OFF_Y   83   /* 官方偏移，世界 1x px */
#define CLOCK_AMPM_GAP 12  /* AM/PM 与时间数字间距，世界 1x px */

/*
 * 载入 fontTime PARTS 包（13 张小图常驻 PSRAM）。
 * anchor 为 clock_table 的世界 1x 锚点（manifest 下发，R15：烘焙视口坐标）。
 * path 传 NULL 保持既有包仅改锚点/使能。
 */
int  clock_digits_configure(const char *parts_path, int16_t anchor_wx,
                            int16_t anchor_wy, bool enable);
void clock_digits_unload(void);

bool clock_digits_active(void);

/* 时钟屏幕矩形（合成器标脏用；未激活返回 false） */
bool clock_digits_get_rect(int32_t *x, int32_t *y, int32_t *w, int32_t *h);

/*
 * 合成进 framebuffer（仅写 clip 与时钟矩形相交区域；带 1bit 掩码跳透明）。
 * scale：2 = 480 屏（世界 1x ×2）。fb_w 为 framebuffer 行宽。
 */
void clock_digits_compose(uint16_t *fb, int32_t fb_w, int32_t scale,
                          int32_t clip_x, int32_t clip_y,
                          int32_t clip_w, int32_t clip_h);

#ifdef __cplusplus
}
#endif

#endif /* RENDER_CLOCK_DIGITS_H */
