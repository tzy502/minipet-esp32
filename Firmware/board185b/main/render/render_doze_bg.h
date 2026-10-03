/* render_doze_bg.h — 待机背景（固件内置，见 render_doze_bg.c） */
#ifndef RENDER_DOZE_BG_H
#define RENDER_DOZE_BG_H

#include <stdint.h>

extern const int g_doze_bg_w;
extern const int g_doze_bg_h;
extern const int16_t g_doze_clock_anchor_x;
extern const int16_t g_doze_clock_anchor_y;
extern const uint16_t g_doze_bg[];

#endif
