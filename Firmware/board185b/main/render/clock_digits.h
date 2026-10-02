/*
 * clock_digits.h — WZ 地图时钟（fontTime 部件按 PARTS 包导出，part_id 900..912）
 *
 * 排布参数按 clock-display-spec.md 定稿值固件硬编码：
 *   起点 = clock 锚点 + (18+3, 83)（世界 1x px，上屏时 ×scale）
 *   am|pm(22) → [AMPM_GAP=12px] → H1(26) → H2(26) → comma(17) → M1(26) → M2(26)
 *   【A1 定稿】185B 合成器 = 360（面板原生）且 RC_SCALE=1 → 世界 1x 上屏即 1:1，
 *   时钟整块 = 155×35 屏 px（原来 480 空间 ×2 = 310×70）。
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

/* 无地图锚点默认（问题3）：anchor 传 CLOCK_ANCHOR_AUTO → 时钟整块居中于
 * **屏幕中心**（sw/2, sh/2，屏 px，来自 clock_digits_set_screen；忽略官方
 * 地图偏移 CLOCK_OFF_X/OFF_Y）。
 *
 * 【A1 修复 2026-10-02】原实现把该中心硬编码成 (240,120)（= 480 合成器空间
 * 口径；A1 之前 185B 合成器是 480×480）。A1 把空间收到面板原生 360×360 且
 * RC_SCALE=1 后，屏幕中心是 (180,180)，硬编码值就变成「右偏 60px、上偏 60px」
 * —— 真机报障「待机时钟不固定在中间」的根因。现按屏幕尺寸实时算，与空间尺寸
 * 解耦（360/480 都对）。 */
#define CLOCK_ANCHOR_AUTO       ((int16_t)-32768)

/* clock_digits_set_screen 未告知尺寸时的兜底屏（正常路径不会走到：render_init
 * 先调 set_screen 再 configure）。仅防"零尺寸 → 中心算成 (0,0)"的退化。 */
#define CLOCK_CENTER_FALLBACK_W 360
#define CLOCK_CENTER_FALLBACK_H 360

/*
 * 告知屏幕尺寸（render_init 调用一次）：居中布局与 get_rect 标脏矩形
 * 都依赖 scale/屏宽；旧实现 scale 首次 compose 才赋值 → enable 后矩形
 * 恒 0、时钟永不标脏（问题3 根因之一）。
 */
void clock_digits_set_screen(int32_t w, int32_t h);

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
 * scale：世界 1x → 屏 px 的整数倍率（185B A1 后 = RC_SCALE = 1，1:1）。
 * fb_w 为 framebuffer 行宽。
 */
void clock_digits_compose(uint16_t *fb, int32_t fb_w, int32_t scale,
                          int32_t clip_x, int32_t clip_y,
                          int32_t clip_w, int32_t clip_h);

#ifdef __cplusplus
}
#endif

#endif /* RENDER_CLOCK_DIGITS_H */
