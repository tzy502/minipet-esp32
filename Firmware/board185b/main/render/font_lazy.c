/*
 * font_lazy.c — FONT 包 → lv_font_t 实现
 *
 * LVGL 9 接线：
 *   font->get_glyph_dsc   → 元数据（索引二分，无 IO）
 *   font->get_glyph_bitmap→ 位图指针（缓存 miss 时 TF 懒读 4bpp）
 * 字形位图行长 (w+1)/2 字节 == LVGL A4 行规则，无需转换直接透传。
 * ofs_y = -bearing_y（包内 bearing 向上为正；LVGL ofs_y 向下为正）。
 * line_height = size_px；base_line = max(bearing_y) 近似 ascent。
 */
#include "font_lazy.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "font";

#define FL_CACHE_SLOTS 16

typedef struct {
    bool     valid;   /* cp 匹配且位图已载入 */
    uint32_t cp;
    uint8_t *bmp;     /* 恒指向所属槽缓冲（init 时分配） */
} fl_slot_t;

typedef struct {
    mpak_t     mpk;
    bool       open;
    lv_font_t  font;
    fl_slot_t  slots[FL_CACHE_SLOTS];
    uint8_t   *slot_bufs;          /* FL_CACHE_SLOTS × max_bmp_bytes */
    uint32_t   slot_sz;
    uint32_t   next_slot;          /* 环形替换指针 */
    int32_t    base_line;
} fl_inst_t;

static fl_inst_t s_inst[FONT_ID_COUNT];

static fl_inst_t *fl_self(const lv_font_t *font)
{
    for (int i = 0; i < FONT_ID_COUNT; i++)
        if (&s_inst[i].font == font) return &s_inst[i];
    return NULL;
}

/* ---------------- LVGL 回调（v9.3 签名） ----------------
 * 字形 codepoint 存 dsc->gid.index（内置 fmt_txt 同款模式），
 * get_glyph_bitmap 阶段凭它反查缓存槽。
 * A4 原样返回（req_raw_bitmap 路径），draw_buf 不需要（无解压格式）。 */

static bool fl_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                         uint32_t letter, uint32_t letter_next)
{
    fl_inst_t *self = fl_self(font);
    if (!self || !self->open || !dsc) return false;
    (void)letter_next;

    const mpak_glyph_t *g;
    if (mpak_font_find_glyph(&self->mpk, letter, &g) != MPAK_OK) {
        /* 缺字：零宽占位，LVGL 按 placeholder 处理 */
        memset(dsc, 0, sizeof *dsc);
        dsc->adv_w        = self->mpk.u.font->size_px / 2;
        dsc->box_w        = 0;
        dsc->box_h        = 0;
        dsc->ofs_x        = 0;
        dsc->ofs_y        = 0;
        dsc->gid.index    = 0;
        dsc->format       = LV_FONT_GLYPH_FORMAT_NONE;
        dsc->is_placeholder = 1;
        dsc->resolved_font = font;
        return true;
    }
    memset(dsc, 0, sizeof *dsc);
    dsc->adv_w        = g->advance;
    dsc->box_w        = g->w;
    dsc->box_h        = g->h;
    dsc->ofs_x        = g->off_x;
    dsc->ofs_y        = -g->bearing_y; /* 向上为正 → LVGL 向下为正 */
    dsc->gid.index    = letter;         /* 反查键：0 保留为无效 */
    dsc->format       = LV_FONT_GLYPH_FORMAT_A4;
    dsc->stride       = (g->w + 1) / 2; /* 4bpp 紧行长 == LVGL A4 行规则 */
    dsc->is_placeholder = 0;
    dsc->resolved_font = font;
    return true;
}

static const void *fl_glyph_bmp(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *draw_buf)
{
    (void)draw_buf;                     /* A4 原样透传，无解码目标缓冲需求 */
    if (!dsc || dsc->gid.index == 0) return NULL;
    uint32_t letter = dsc->gid.index;
    fl_inst_t *self = fl_self(dsc->resolved_font);
    if (!self || !self->open) return NULL;

    const mpak_glyph_t *g;
    if (mpak_font_find_glyph(&self->mpk, letter, &g) != MPAK_OK) return NULL;

    /* 缓存命中 */
    for (int i = 0; i < FL_CACHE_SLOTS; i++)
        if (self->slots[i].valid && self->slots[i].cp == letter)
            return self->slots[i].bmp;

    /* miss：环形取槽 TF 懒读 */
    fl_slot_t *s = &self->slots[self->next_slot];
    self->next_slot = (self->next_slot + 1) % FL_CACHE_SLOTS;
    s->valid = false;
    s->cp    = letter;
    if (mpak_font_read_glyph_bmp(&self->mpk, g, s->bmp, self->slot_sz) != MPAK_OK) {
        ESP_LOGE(TAG, "glyph U+%04" PRIx32 " read failed", letter);
        return NULL;
    }
    s->valid = true;
    return s->bmp;
}

/* ---------------- 生命周期 ---------------- */

static void fl_clear_slots(fl_inst_t *self)
{
    memset(self->slots, 0, sizeof self->slots);
    if (self->slot_bufs && self->slot_sz) {
        uint8_t *p = self->slot_bufs;
        for (int i = 0; i < FL_CACHE_SLOTS; i++)
            self->slots[i].bmp = p + (size_t)i * self->slot_sz;
    }
    self->next_slot = 0;
}

void font_lazy_deinit(font_id_t id)
{
    if ((int)id < 0 || id >= FONT_ID_COUNT) return;
    fl_inst_t *self = &s_inst[id];
    if (self->open) {
        mpak_close(&self->mpk);
        self->open = false;
    }
    if (self->slot_bufs) {
        heap_caps_free(self->slot_bufs);
        self->slot_bufs = NULL;
    }
    memset(&self->font, 0, sizeof self->font);
}

int font_lazy_init(font_id_t id, const char *mpk_path)
{
    if ((int)id < 0 || id >= FONT_ID_COUNT || !mpk_path) return MPAK_ERR_ARG;
    fl_inst_t *self = &s_inst[id];
    font_lazy_deinit(id);

    ESP_LOGW("font", "font_lazy_init open %s", mpk_path);
    int rc = mpak_open(&self->mpk, mpk_path, 0 /* hash 比对由调用方决定 */,
                       MPAK_KIND_FONT);
    ESP_LOGW("font", "mpak_open rc=%d", rc);
    if (rc != MPAK_OK) return rc;

    const mpak_font_t *ft = self->mpk.u.font;
    self->slot_sz  = ft->max_bmp_bytes;
    self->slot_bufs = heap_caps_malloc((size_t)FL_CACHE_SLOTS * self->slot_sz,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!self->slot_bufs) {
        mpak_close(&self->mpk);
        return MPAK_ERR_NOMEM;
    }
    fl_clear_slots(self);

    /* ascent 近似 = max(bearing_y)，clamp 到 [1, size_px] */
    int32_t asc = 0;
    for (uint32_t i = 0; i < ft->glyph_count; i++)
        if (ft->glyphs[i].bearing_y > asc) asc = ft->glyphs[i].bearing_y;
    if (asc < 1) asc = 1;
    if (asc > ft->size_px) asc = ft->size_px;
    self->base_line = asc;

    memset(&self->font, 0, sizeof self->font);
    self->font.get_glyph_dsc    = fl_glyph_dsc;
    self->font.get_glyph_bitmap = fl_glyph_bmp;
    self->font.line_height      = ft->size_px;
    self->font.base_line        = self->base_line;

    self->open = true;
    ESP_LOGI(TAG, "font%d ready: %u glyphs, line=%u base=%" PRId32,
             (int)ft->size_px, ft->glyph_count, ft->size_px, self->base_line);
    return MPAK_OK;
}

const lv_font_t *font_lazy_get(font_id_t id)
{
    if ((int)id < 0 || id >= FONT_ID_COUNT || !s_inst[id].open) return NULL;
    return &s_inst[id].font;
}

bool font_lazy_ready(font_id_t id)
{
    return !((int)id < 0 || id >= FONT_ID_COUNT) && s_inst[id].open;
}

int font_lazy_measure(font_id_t id, uint32_t codepoint, uint32_t *adv_w)
{
    if ((int)id < 0 || id >= FONT_ID_COUNT || !s_inst[id].open || !adv_w)
        return MPAK_ERR_ARG;
    const mpak_glyph_t *g;
    if (mpak_font_find_glyph(&s_inst[id].mpk, codepoint, &g) != MPAK_OK)
        return MPAK_ERR_RANGE;
    *adv_w = g->advance;
    return MPAK_OK;
}
