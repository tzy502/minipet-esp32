/*
 * clock_digits.c — fontTime 地图时钟实现
 *
 * 13 张小图（0-9/am/pm/comma）在 configure 时一次性读入 PSRAM（共 ~24KB），
 * 合成零 IO。世界 1x 坐标 ×scale 进入屏幕（480 屏 scale=2）。
 */
#include "clock_digits.h"

#include "compositor.h"   /* rc_mask_bit（tight bitpack：bit = y*w+x） */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "clock";

#define CLOCK_GLYPHS 13   /* 0..9, am, pm, comma */

typedef struct {
    bool     loaded;
    uint16_t id;
    uint16_t w, h;
    uint16_t *px;      /* RGB565，行 stride = align4(w*2) 字节 → 行元素数 stride/2 */
    uint8_t  *mask;    /* 1bit，MSB first；NULL = 不透明 */
    uint32_t stride_b; /* 行字节数（4 对齐） */
} cg_t;

static struct {
    bool     enabled;
    bool     loaded;
    bool     centered;          /* 问题3：无地图锚点 → 整块居中屏幕 (240,120) */
    mpak_t   mpk;
    cg_t     g[CLOCK_GLYPHS];
    int16_t  anchor_wx, anchor_wy;
    int32_t  scale;
    int32_t  sw, sh;            /* 屏幕尺寸（clock_digits_set_screen，居中用） */
    /* 屏幕矩形缓存（compose 时按当前时间刷新） */
    int32_t  rx, ry, rw, rh;
    bool     rect_valid;
    /* 上次绘制状态（分/秒奇偶变化才标脏由合成器管理） */
    int      last_min, last_parity;
} s_ck;

void clock_digits_set_screen(int32_t w, int32_t h)
{
    s_ck.sw = w;
    s_ck.sh = h;
    if (s_ck.scale <= 0) s_ck.scale = RC_SCALE;
}

static uint32_t cg_stride(uint16_t w) { return ((uint32_t)w * 2u + 3u) & ~3u; }

static const cg_t *cg_load(uint16_t part_id)
{
    const mpak_part_t *p = mpak_parts_find(&s_ck.mpk, part_id);
    if (!p) {
        ESP_LOGE(TAG, "fontTime part %u missing", part_id);
        return NULL;
    }
    int idx = -1;
    if (part_id >= CLOCK_PART_DIGIT_BASE && part_id < CLOCK_PART_DIGIT_BASE + 10)
        idx = (int)(part_id - CLOCK_PART_DIGIT_BASE);
    else if (part_id == CLOCK_PART_AM)    idx = 10;
    else if (part_id == CLOCK_PART_PM)    idx = 11;
    else if (part_id == CLOCK_PART_COMMA) idx = 12;
    if (idx < 0) return NULL;

    cg_t *g = &s_ck.g[idx];
    g->id = part_id;
    g->w  = p->w;
    g->h  = p->h;
    g->stride_b = cg_stride(p->w);
    g->px = heap_caps_malloc((size_t)p->h * g->stride_b,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g->px) return NULL;
    if (mpak_part_read_pixels(&s_ck.mpk, p, (uint8_t *)g->px,
                              (size_t)p->h * g->stride_b) != MPAK_OK) {
        heap_caps_free(g->px); g->px = NULL;
        return NULL;
    }
    if (p->has_alpha) {
        g->mask = heap_caps_malloc(p->mask_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g->mask &&
            mpak_part_read_mask(&s_ck.mpk, p, g->mask, p->mask_bytes) != MPAK_OK) {
            heap_caps_free(g->mask); g->mask = NULL;
        }
    } else {
        g->mask = NULL;
    }
    return g;
}

static void cg_free(cg_t *g)
{
    if (g->px)   heap_caps_free(g->px);
    if (g->mask) heap_caps_free(g->mask);
    memset(g, 0, sizeof *g);
}

void clock_digits_unload(void)
{
    for (int i = 0; i < CLOCK_GLYPHS; i++) cg_free(&s_ck.g[i]);
    if (s_ck.loaded) mpak_close(&s_ck.mpk);
    memset(&s_ck, 0, sizeof s_ck);
}

int clock_digits_configure(const char *parts_path, int16_t anchor_wx,
                           int16_t anchor_wy, bool enable)
{
    if (parts_path) {
        clock_digits_unload();
        int rc = mpak_open(&s_ck.mpk, parts_path, 0, MPAK_KIND_PARTS);
        if (rc != MPAK_OK) return rc;
        bool ok = true;
        for (uint32_t id = CLOCK_PART_DIGIT_BASE; id <= CLOCK_PART_COMMA; id++)
            if (!cg_load((uint16_t)id)) ok = false;
        s_ck.loaded = ok;
        if (!ok) ESP_LOGW(TAG, "fontTime package incomplete (continuing)");
    }
    s_ck.anchor_wx = anchor_wx;
    s_ck.anchor_wy = anchor_wy;
    /* 问题3：无地图锚点（AUTO 哨兵）→ 整块居中屏幕 (240,120)，忽略官方地图偏移 */
    s_ck.centered = (anchor_wx == CLOCK_ANCHOR_AUTO && anchor_wy == CLOCK_ANCHOR_AUTO);
    if (s_ck.scale <= 0) s_ck.scale = RC_SCALE;
    s_ck.enabled   = enable && s_ck.loaded;
    s_ck.rect_valid = false;
    return MPAK_OK;
}

bool clock_digits_active(void) { return s_ck.enabled; }

/* ---- 时间规则（clock-display-spec.md 三） ---- */
static void time_split(int *disp_h, bool *is_am, int *minute, int *parity)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    int h = tmv.tm_hour % 24;
    *is_am   = (h < 12);          /* 0~11 = AM、12~23 = PM */
    if (h >= 13) h -= 12;         /* 13..23 → 1..11；12 保持 12；0 保持 0（午夜 AM 00） */
    *disp_h  = h;
    *minute  = tmv.tm_min;
    *parity  = tmv.tm_sec & 1;    /* 偶秒显 comma、奇秒隐 */
}

/*
 * 当前时间的字形序列（7 槽；comma 奇秒 → NULL 跳过但保留间距位）。
 * 返回序列长度；out_x 各槽世界 1x 左缘；out_y0 块顶世界 1x y（居中/锚点统一）。
 */
static int glyph_seq(const cg_t *out_g[7], int32_t out_x[7], bool *comma_on,
                     int32_t *out_y0)
{
    int dh, mm, parity;
    bool am;
    time_split(&dh, &am, &mm, &parity);
    *comma_on = (parity == 0);

    const cg_t *g_am = &s_ck.g[am ? 10 : 11];
    const cg_t *d1 = &s_ck.g[dh / 10];
    const cg_t *d2 = &s_ck.g[dh % 10];
    const cg_t *m1 = &s_ck.g[mm / 10];
    const cg_t *m2 = &s_ck.g[mm % 10];
    const cg_t *comma = &s_ck.g[12];

    int32_t scale = (s_ck.scale > 0) ? s_ck.scale : RC_SCALE;
    int32_t x, y0;
    if (s_ck.centered) {
        /* 问题3 默认锚点：整块（含 comma 恒占宽）居中于屏幕 (240,120)（屏 px）。
         * 屏 px → 世界 1x（除 scale），comma 常驻宽度参与计算 → 闪烁不移位。 */
        int32_t w_world = g_am->w + CLOCK_AMPM_GAP +
                          d1->w + d2->w + comma->w + m1->w + m2->w;
        int32_t h_world = d2->h;
        int32_t cx_world = (int32_t)CLOCK_CENTER_SCREEN_X / scale;
        int32_t cy_world = (int32_t)CLOCK_CENTER_SCREEN_Y / scale;
        x  = cx_world - w_world / 2;
        y0 = cy_world - h_world / 2;
    } else {
        /* 地图 clock_table 锚点 + 官方偏移（clock-display-spec） */
        x  = (int32_t)s_ck.anchor_wx + CLOCK_OFF_X;
        y0 = (int32_t)s_ck.anchor_wy + CLOCK_OFF_Y;
    }

    /* am|pm → GAP=12 → H1 → H2 → comma → M1 → M2（数字间无额外间距） */
    int n = 0;
    if (g_am->px) {
        out_g[n] = g_am; out_x[n] = x; n++;
        x += g_am->w;
    }
    x += CLOCK_AMPM_GAP;

    out_g[n] = d1; out_x[n] = x; n++; x += d1->w;
    out_g[n] = d2; out_x[n] = x; n++; x += d2->w;
    out_x[n] = x;               /* comma 槽位恒占宽（闪烁不移位） */
    out_g[n] = *comma_on ? comma : NULL; n++;
    x += comma->w;
    out_g[n] = m1; out_x[n] = x; n++; x += m1->w;
    out_g[n] = m2; out_x[n] = x; n++;
    if (out_y0) *out_y0 = y0;
    return n;
}

bool clock_digits_get_rect(int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    if (!s_ck.enabled) return false;
    int32_t scale = (s_ck.scale > 0) ? s_ck.scale : RC_SCALE;
    const cg_t *seq[7];
    int32_t xs[7];
    bool comma_on;
    int32_t y0;
    int n = glyph_seq(seq, xs, &comma_on, &y0);
    if (n <= 0) return false;
    int32_t x0 = INT32_MAX, x1 = INT32_MIN;
    int32_t yy0 = INT32_MAX, yy1 = INT32_MIN;
    for (int i = 0; i < n; i++) {
        if (!seq[i]) continue;
        if (y0 < yy0) yy0 = y0;
        if (y0 + seq[i]->h > yy1) yy1 = y0 + seq[i]->h;
        if (xs[i] < x0) x0 = xs[i];
        if (xs[i] + seq[i]->w > x1) x1 = xs[i] + seq[i]->w;
    }
    if (x0 == INT32_MAX || yy0 == INT32_MAX) return false;
    *x = x0 * scale; *y = yy0 * scale;
    *w = (x1 - x0) * scale;
    *h = (yy1 - yy0) * scale;
    return true;
}

void clock_digits_compose(uint16_t *fb, int32_t fb_w, int32_t scale,
                          int32_t clip_x, int32_t clip_y,
                          int32_t clip_w, int32_t clip_h)
{
    if (!s_ck.enabled || !fb) return;
    s_ck.scale = scale;

    const cg_t *seq[7];
    int32_t xs[7];
    bool comma_on;
    int32_t y0;
    int n = glyph_seq(seq, xs, &comma_on, &y0);
    int32_t sy = y0 * scale;

    for (int i = 0; i < n; i++) {
        const cg_t *g = seq[i];
        if (!g || !g->px) continue;                 /* comma 奇秒 → NULL */
        int32_t dx = xs[i] * scale;
        int32_t dw = (int32_t)g->w * scale;
        int32_t dh = (int32_t)g->h * scale;

        int32_t x0 = dx > clip_x ? dx : clip_x;
        int32_t y0 = sy > clip_y ? sy : clip_y;
        int32_t x1 = (dx + dw < clip_x + clip_w) ? dx + dw : clip_x + clip_w;
        int32_t y1 = (sy + dh < clip_y + clip_h) ? sy + dh : clip_y + clip_h;
        if (x0 >= x1 || y0 >= y1) continue;

        uint32_t stride_el = g->stride_b / 2u;      /* RGB565 元素 */
        for (int32_t syy = y0; syy < y1; syy++) {
            int32_t src_y = (syy - sy) / scale;     /* nearest */
            const uint16_t *srow = g->px + (size_t)src_y * stride_el;
            uint16_t *drow = fb + (size_t)syy * fb_w;
            for (int32_t sxx = x0; sxx < x1; sxx++) {
                int32_t src_x = (sxx - dx) / scale;
                if (g->mask && !rc_mask_bit(g->mask,
                        (uint32_t)src_y * g->w + (uint32_t)src_x))
                    continue;
                drow[sxx] = srow[src_x];
            }
        }
    }
}
