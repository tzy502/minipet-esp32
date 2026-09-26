/*
 * lvgl_bridge.c — LVGL 合流桥实现
 *
 * 注意（LVGL 9.x API 假设点，联调时核对）：
 *   - flush：void (*)(lv_display_t*, const lv_area_t*, uint8_t*)
 *   - flush 末尾必须 lv_display_flush_ready(disp)（同步 flush 契约；
 *     缺失 = 下一次带失效区的刷新在 wait_for_flushing 死循环 → 卡死喂狗
 *     → watchdog.c 软件看门狗 esp_restart，表现为进菜单一帧后整机重启）
 *   - lv_font_get_glyph_bitmap_cb_t：(font, letter, glyph_dsc) 三参
 *   - lv_font_glyph_dsc_t 含 resolved_font 字段
 */
#include "lvgl_bridge.h"

#include <string.h>

#include <lvgl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "font_lazy.h"
#include "render.h" /* RENDER_* 错误码 */

static const char *TAG = "bridge";

#define BR_BUBBLE_PAD    8
#define BR_BUBBLE_BORDER 2
#define BR_BUBBLE_TEXT_MAX 384   /* 显示串缓冲（含插入的 \n） */

static struct {
    lv_display_t *disp;
    int32_t sw, sh;

    uint8_t *poker_buf[2];       /* 各 1/10 屏（PARTIAL 双缓冲） */
    uint32_t poker_buf_sz;
    uint8_t *menu_buf;           /* 整屏（DIRECT 单缓冲） */

    bool menu_mode;

    /* MENU 脏区 bbox（tick 内累积、take 清零；全在渲染任务） */
    bool     md_valid;
    int32_t  md_x1, md_y1, md_x2, md_y2;

    /* 气泡捕获 */
    struct {
        bool      active;
        uint16_t *dst;
        int32_t   stride, w, h;
    } cap;
} s_br;

/* ---------------- flush ---------------- */

static void bridge_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* 本 flush 为同步完成（像素已落在 LVGL 缓冲/或仅记录 bbox，无 DMA 在途）。
     * LVGL 9 契约：flush_cb 必须调用 lv_display_flush_ready() 终止本次 flush，
     * 否则 disp->flushing 恒为 1，下一次「带失效区」的刷新会在
     * wait_for_flushing() 的 while(disp->flushing) 处死循环（lv_refr.c:1500）
     * → render 任务卡死不再喂狗 → watchdog.c 软件看门狗 esp_restart()
     * （真机症状：菜单亮一帧后 ~5s 整机重启）。
     * 在入口即清标志：对同步 flush 与出口清除等价（该标志只会在下一次刷新
     * 被检查），且任何 early-return 分支都不可能漏标。 */
    if (disp) lv_display_flush_ready(disp);

    if (!area) return;

    if (s_br.menu_mode) {
        /* MENU：记录 bbox，拷贝与上屏由合成器完成（单写屏者）。
         * 探针：每次进菜单的首个 flush（md_valid 仍未置位）打印一次区域，
         * 真机日志可确认「第 1 帧已渲染完成」。 */
        if (!s_br.md_valid) {
            ESP_LOGI(TAG, "flush[menu] first (%d,%d)-(%d,%d)",
                     area->x1, area->y1, area->x2, area->y2);
            s_br.md_x1 = area->x1; s_br.md_y1 = area->y1;
            s_br.md_x2 = area->x2; s_br.md_y2 = area->y2;
            s_br.md_valid = true;
        } else {
            if (area->x1 < s_br.md_x1) s_br.md_x1 = area->x1;
            if (area->y1 < s_br.md_y1) s_br.md_y1 = area->y1;
            if (area->x2 > s_br.md_x2) s_br.md_x2 = area->x2;
            if (area->y2 > s_br.md_y2) s_br.md_y2 = area->y2;
        }
        return;
    }

    /* POKER：无屏可写——气泡捕获模式则收切片 */
    if (s_br.cap.active && px_map) {
        int32_t cx1 = area->x1, cy1 = area->y1;
        int32_t cx2 = area->x2, cy2 = area->y2;
        if (cx1 < 0) cx1 = 0;
        if (cy1 < 0) cy1 = 0;
        if (cx2 > s_br.cap.w - 1) cx2 = s_br.cap.w - 1;
        if (cy2 > s_br.cap.h - 1) cy2 = s_br.cap.h - 1;
        if (cx1 <= cx2 && cy1 <= cy2) {
            int32_t area_w = area->x2 - area->x1 + 1;
            for (int32_t y = cy1; y <= cy2; y++) {
                const uint8_t *src = px_map +
                    (size_t)(y - area->y1) * area_w * 2u + (size_t)(cx1 - area->x1) * 2u;
                uint16_t *dst = s_br.cap.dst + (size_t)y * s_br.cap.stride + cx1;
                memcpy(dst, src, (size_t)(cx2 - cx1 + 1) * 2u);
            }
        }
    }
}

/* ---------------- 生命周期 / 模式 ---------------- */

int bridge_init(int32_t screen_w, int32_t screen_h)
{
    memset(&s_br, 0, sizeof s_br);
    s_br.sw = screen_w;
    s_br.sh = screen_h;

    /* LVGL 核心初始化——必须在任何 lv_* 调用之前（缺失 = tlsf 空池崩溃） */
    lv_init();
    lv_tick_set_cb((lv_tick_get_cb_t)xTaskGetTickCount);

    s_br.disp = lv_display_create((uint32_t)screen_w, (uint32_t)screen_h);
    if (!s_br.disp) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return RENDER_ERR_NOMEM;
    }
    lv_display_set_flush_cb(s_br.disp, bridge_flush);
    lv_display_set_color_format(s_br.disp, LV_COLOR_FORMAT_RGB565);

    /* POKER：2 × 1/10 屏双缓冲 */
    s_br.poker_buf_sz = (uint32_t)screen_w * (uint32_t)(screen_h / 10) * 2u;
    s_br.poker_buf[0] = heap_caps_malloc(s_br.poker_buf_sz,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_br.poker_buf[1] = heap_caps_malloc(s_br.poker_buf_sz,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_br.poker_buf[0] || !s_br.poker_buf[1]) {
        ESP_LOGE(TAG, "poker bufs alloc failed (%u B each)", s_br.poker_buf_sz);
        return RENDER_ERR_NOMEM;
    }

    /* MENU：整屏离屏（450KB @480×480） */
    s_br.menu_buf = heap_caps_malloc((size_t)screen_w * screen_h * 2u,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_br.menu_buf) {
        ESP_LOGE(TAG, "menu buf alloc failed");
        return RENDER_ERR_NOMEM;
    }

    return bridge_mode_poker();
}

void bridge_deinit(void)
{
    if (s_br.disp) lv_display_delete(s_br.disp);
    if (s_br.poker_buf[0]) heap_caps_free(s_br.poker_buf[0]);
    if (s_br.poker_buf[1]) heap_caps_free(s_br.poker_buf[1]);
    if (s_br.menu_buf) heap_caps_free(s_br.menu_buf);
    memset(&s_br, 0, sizeof s_br);
}

int bridge_mode_poker(void)
{
    if (!lv_is_initialized() || !s_br.disp || !s_br.poker_buf[0]) {
        ESP_LOGE(TAG, "mode_poker: not ready (lv=%d disp=%p)",
                 lv_is_initialized(), (void *)s_br.disp);
        return RENDER_ERR_STATE;
    }
    if (s_br.menu_mode) ESP_LOGI(TAG, "mode_poker: exit menu -> PARTIAL");
    lv_display_set_buffers(s_br.disp, s_br.poker_buf[0], s_br.poker_buf[1],
                           s_br.poker_buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    s_br.menu_mode = false;
    s_br.md_valid = false;
    return RENDER_OK;
}

/* ---------------- MENU 深色菜单屏（问题5） ----------------
 * 旧实现 enter_menu 后从未构建任何 LVGL 控件，LVGL 默认屏幕底色为白 →
 * 整屏发白。现构建黑底菜单：标题 + 三行占位 + 底部退出提示。
 * 须与 render_tick 同任务（render_enter_menu → bridge_mode_menu）调用。 */
static void menu_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr) {
        ESP_LOGE(TAG, "menu_build: no active screen");
        return;
    }

    /* 幂等：进/出菜单多轮后默认屏上会残留上一轮的控件，重建前先清空，
     * 防控件树逐轮叠加（内存慢性泄漏 + 重叠绘制）。 */
    uint32_t stale = lv_obj_get_child_count(scr);
    if (stale) lv_obj_clean(scr);

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    const lv_font_t *f32 = font_lazy_get(FONT_ID_32);
    const lv_font_t *f24 = font_lazy_get(FONT_ID_24);
    const lv_font_t *f16 = font_lazy_get(FONT_ID_16);
    ESP_LOGI(TAG, "menu_build: cleared %u stale obj, fonts 32=%d 24=%d 16=%d",
             (unsigned)stale, f32 != NULL, f24 != NULL, f16 != NULL);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    if (f32) lv_obj_set_style_text_font(title, f32, 0);
    lv_label_set_text(title, "MiniPet");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 64);

    static const char *rows[3] = { "Maps", "Paperdoll", "BGM" };   /* 占位项（E7） */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *it = lv_label_create(scr);
        lv_obj_set_style_text_color(it, lv_color_hex(0xE8E8EC), 0);
        if (f24) lv_obj_set_style_text_font(it, f24, 0);
        lv_label_set_text_fmt(it, "> %s", rows[i]);
        lv_obj_align(it, LV_ALIGN_LEFT_MID, 110, -48 + i * 72);
    }

    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x909098), 0);
    if (f16) {
        lv_obj_set_style_text_font(hint, f16, 0);
        lv_label_set_text(hint, "长按退出");   /* [待真机验证] FONT 包需含这 4 个汉字字形，缺字时降级为下一分支同义 ASCII */
    } else {
        lv_label_set_text(hint, "LONG PRESS: EXIT");
    }
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    ESP_LOGI(TAG, "menu_build: %u widgets on screen", (unsigned)lv_obj_get_child_count(scr));
    lv_obj_invalidate(scr);   /* DIRECT 模式强制整屏重绘入 menu_buf */
}

int bridge_mode_menu(void)
{
    /* 前置验证：LVGL 未初始化/无 display/无整屏缓冲时绝不切换（防
     * 后续 lv_* 调用在空指针/LVGL 空池上炸机）——任何失败都原样返回，
     * render 层保持 POKER 态，不产生重启路径 */
    if (!lv_is_initialized() || !s_br.disp || !s_br.menu_buf) {
        ESP_LOGE(TAG, "mode_menu: not ready (lv=%d disp=%p buf=%p)",
                 lv_is_initialized(), (void *)s_br.disp, (void *)s_br.menu_buf);
        return RENDER_ERR_STATE;
    }
    /* 幂等：已在菜单态直接成功返回，不重复 set_buffers / 重建控件
     * （旧实现重复 enter 每次在默认屏上再叠 5 个控件） */
    if (s_br.menu_mode) {
        ESP_LOGW(TAG, "mode_menu: already in menu (idempotent no-op)");
        return RENDER_OK;
    }

    ESP_LOGI(TAG, "mode_menu: enter (%dx%d DIRECT buf=%u B)",
             (int)s_br.sw, (int)s_br.sh,
             (unsigned)((size_t)s_br.sw * s_br.sh * 2u));
    memset(s_br.menu_buf, 0, (size_t)s_br.sw * s_br.sh * 2u);
    lv_display_set_buffers(s_br.disp, s_br.menu_buf, NULL,
                           (uint32_t)s_br.sw * (uint32_t)s_br.sh * 2u,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    s_br.menu_mode = false;   /* 构建成功后才切菜单态：失败路径保持 POKER，绝不半切换 */
    s_br.md_valid = false;
    menu_build();             /* 问题5：进入菜单即构建深色 UI（防白屏/黑屏） */
    s_br.menu_mode = true;    /* 构建成功后才切换态，失败路径保持 POKER */
    ESP_LOGI(TAG, "mode_menu: built, first tick will render frame 1");
    return RENDER_OK;
}

bool bridge_is_menu(void) { return s_br.menu_mode; }

const uint16_t *bridge_menu_buf(void)
{
    return (const uint16_t *)s_br.menu_buf;
}

lv_display_t *bridge_display(void)
{
    return s_br.disp;
}

bool bridge_menu_take_dirty(int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    if (!s_br.menu_mode || !s_br.md_valid) return false;
    if (x) *x = s_br.md_x1;
    if (y) *y = s_br.md_y1;
    if (w) *w = s_br.md_x2 - s_br.md_x1 + 1;
    if (h) *h = s_br.md_y2 - s_br.md_y1 + 1;
    s_br.md_valid = false;
    return true;
}

/* ---------------- 气泡离屏渲染 ---------------- */

/* UTF-8 解码：返回消耗字节数（*cp 出码点）；串尾返回 0；非法返回 -1 */
static int utf8_step(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!*p) return 0;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) |
              ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    return -1;
}

/*
 * 排版：贪心换行（空格处优先断行，行满紧急断行——CJK 风格）。
 * 产出含显式 \n 的显示串；返回行数/最宽行宽；行数超 max_lines 截断。
 */
static int bubble_layout(const char *text, const lv_font_t *font,
                         int32_t max_text_w, int max_lines,
                         char *disp, size_t disp_cap,
                         int32_t *out_max_line_w, int *out_lines)
{
    uint32_t size_px = font->line_height ? (uint32_t)font->line_height : 16u;
    int32_t line_w = 0, max_line_w = 0;
    int lines = 1;
    size_t dl = 0;

    const char *s = text;
    while (*s) {
        uint32_t cp = 0xFFFDu;
        int step = utf8_step(s, &cp);
        if (step <= 0) { cp = 0xFFFDu; step = 1; }

        uint32_t adv = 0;
        lv_font_glyph_dsc_t dsc;
        if (lv_font_get_glyph_dsc(font, &dsc, cp, 0))   /* v9 签名：dsc 第 2 参 */
            adv = dsc.adv_w;
        else
            adv = size_px; /* 缺字按全宽占位（可见反馈） */

        bool brk = false;
        if (cp == (uint32_t)' ') {
            if (line_w + (int32_t)adv > max_text_w) brk = true; /* 断行并丢弃空格 */
        } else if (line_w + (int32_t)adv > max_text_w) {
            brk = true; /* 紧急断行 */
        }

        if (brk) {
            if (lines >= max_lines) break; /* 截断（丢弃剩余） */
            if (dl + 1 >= disp_cap) break;
            disp[dl++] = '\n';
            lines++;
            line_w = 0;
            if (cp == (uint32_t)' ') { s += step; continue; }
        }

        if (dl + (size_t)step >= disp_cap) break;
        memcpy(disp + dl, s, (size_t)step);
        dl += (size_t)step;
        line_w += (int32_t)adv;
        if (line_w > max_line_w) max_line_w = line_w;
        s += step;
    }
    disp[dl] = '\0';
    *out_max_line_w = max_line_w;
    *out_lines = lines;
    return 0;
}

int bridge_bubble_render(const char *text, int font_id,
                         uint16_t *dst, int32_t dst_stride,
                         int32_t dst_max_w, int32_t dst_max_h,
                         int32_t *out_w, int32_t *out_h)
{
    if (!text || !*text || !dst || !out_w || !out_h) return RENDER_ERR_ARG;
    if (!lv_is_initialized() || !s_br.disp) return RENDER_ERR_STATE;
    if (s_br.menu_mode) return RENDER_ERR_STATE;
    ESP_LOGI(TAG, "bubble_render: font=%d text_len=%u", font_id, (unsigned)strlen(text));

    const lv_font_t *font = font_lazy_get((font_id_t)font_id);
    if (!font) {
        ESP_LOGE(TAG, "font %d not loaded (render_set_font first)", font_id);
        return RENDER_ERR_STATE;
    }

    static char disp[BR_BUBBLE_TEXT_MAX + 8];
    int32_t inner_max_w = dst_max_w - 2 * (BR_BUBBLE_PAD + BR_BUBBLE_BORDER);
    int32_t inner_max_h = dst_max_h - 2 * (BR_BUBBLE_PAD + BR_BUBBLE_BORDER);
    uint32_t line_h = font->line_height ? (uint32_t)font->line_height : 16u;
    int max_lines = (int)(inner_max_h / line_h);
    if (inner_max_w < 8 || max_lines < 1) return RENDER_ERR_ARG;

    int32_t max_line_w;
    int lines;
    bubble_layout(text, font, inner_max_w, max_lines,
                  disp, sizeof disp, &max_line_w, &lines);

    int32_t bw = max_line_w + 2 * (BR_BUBBLE_PAD + BR_BUBBLE_BORDER);
    int32_t bh = (int32_t)lines * (int32_t)line_h + 2 * (BR_BUBBLE_PAD + BR_BUBBLE_BORDER);
    if (bw > dst_max_w || bh > dst_max_h) return RENDER_ERR_ARG;

    /* 离屏渲染：临时 screen + 气泡盒（不透明、直角），仅 invalidate 盒区域 */
    lv_obj_t *prev_scr = lv_screen_active();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_pos(box, 0, 0);
    lv_obj_set_size(box, (int32_t)bw, (int32_t)bh);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xF7F7F2), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, BR_BUBBLE_BORDER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x303030), 0);
    lv_obj_set_style_pad_all(box, BR_BUBBLE_PAD, 0);

    lv_obj_t *lbl = lv_label_create(box);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x101010), 0);
    lv_label_set_text(lbl, disp);
    lv_obj_set_width(lbl, inner_max_w);
    lv_obj_center(lbl);

    lv_screen_load(scr);
    s_br.cap.active = true;
    s_br.cap.dst    = dst;
    s_br.cap.stride = dst_stride;
    s_br.cap.w      = bw;
    s_br.cap.h      = bh;
    lv_obj_invalidate(box);
    lv_refr_now(NULL);           /* 同步渲染；partial 切片经 flush 捕获 */
    s_br.cap.active = false;

    lv_screen_load(prev_scr);
    lv_obj_delete(scr);

    *out_w = bw;
    *out_h = bh;
    ESP_LOGI(TAG, "bubble_render: done %dx%d", (int)bw, (int)bh);
    return RENDER_OK;
}
