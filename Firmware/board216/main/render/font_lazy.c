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
#include "mp_psram.h"
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
    uint8_t  *src_scratch;   /* 4bpp 源位图暂存（A8 展开用） */
} fl_inst_t;

/* 【内部 RAM 腾挪 2026-10-02】3 档字体的实例表（3×320B=960B）原为内部 .bss。
 * 内容是 LVGL 字体回调用的句柄/指针/尺寸（位图缓存 slot_bufs 与 src_scratch
 * **本来就在 PSRAM**）→ 表本身放 PSRAM 完全等价，且这些字段只在 CPU 侧
 * 被回调读，无 DMA 约束。
 * 形状：单槽兜底 + 唯一取槽入口 fl_at()。兜底槽故意只有 1 个，所以**任何**
 * 按下标取槽都必须走 fl_at()（未就绪时它一律返回兜底槽），否则越界。
 * 兜底槽全 0 ⇒ open=false ⇒ 所有字形回调安全退化为"无此字"（与"字体未装载"
 * 同语义，不崩）。 */
static fl_inst_t  s_inst_zero;
static fl_inst_t *s_inst;                 /* NULL = 未就绪（见 fl_inst_ensure） */
static bool       s_inst_ready;

static void fl_inst_ensure(void)
{
    if (s_inst_ready) return;
    s_inst_ready = true;                  /* 先置位：失败也不再重试（防抖） */
    s_inst = mp_psram_calloc(FONT_ID_COUNT, sizeof *s_inst);
    if (!s_inst) ESP_LOGE(TAG, "字体实例表分配失败 → 字体功能不可用（降级，不崩）");
}

/* 唯一取槽入口；调用方须先做 0<=id<FONT_ID_COUNT 校验 */
static fl_inst_t *fl_at(int id)
{
    fl_inst_ensure();
    return s_inst ? &s_inst[id] : &s_inst_zero;
}

static fl_inst_t *fl_self(const lv_font_t *font)
{
    for (int i = 0; i < FONT_ID_COUNT; i++) {
        fl_inst_t *c = fl_at(i);
        if (&c->font == font) return c;
    }
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
    /* 【必须是 A8 + static_bitmap】LVGL 9.6 draw_letter 只有
     * `static_bitmap && format == A8` 才走"原始位图指针"路径；
     * 否则它把 get_glyph_bitmap 的返回值当 lv_draw_buf_t* 解引用
     * （draw_buf->data / ->header.stride 全是位图字节）→ 野指针崩溃
     * （真机 EXCVADDR=0xaaddccbb/0x0c000000，位置 lv_draw_sw_blend_color_to_rgb565）。
     * 故此处报 A8（1B/px），位图在 fl_glyph_bmp 里由 4bpp 展开成 8bpp。 */
    dsc->format       = LV_FONT_GLYPH_FORMAT_A8;
    dsc->stride       = g->w;           /* A8 行距 = 宽（字节/行） */
    dsc->is_placeholder = 0;
    dsc->resolved_font = font;
    return true;
}

static const void *fl_glyph_bmp(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *draw_buf)
{
    /* 契约：本字体 static_bitmap=1 + format=A8 ⇒ LVGL 必以 draw_buf == NULL 调用，
     * 返回值按"原始 A8 掩码指针"消费（环形缓存保证同步消费期内有效）。 */
    (void)draw_buf;
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
    uint32_t w = g->w, h = g->h;
    uint32_t src_sz = ((w + 1u) / 2u) * h;
    if (src_sz > self->mpk.u.font->max_bmp_bytes || w * h > self->slot_sz) {
        ESP_LOGE(TAG, "glyph U+%04" PRIx32 " 尺寸越界 %ux%u (src %u>%u, a8 %u>%u)", letter,
                 (unsigned)w, (unsigned)h, (unsigned)src_sz,
                 (unsigned)self->mpk.u.font->max_bmp_bytes,
                 (unsigned)(w * h), (unsigned)self->slot_sz);
        return NULL;
    }
    if (w == 0 || h == 0) return NULL;      /* 零尺寸字形：不画（LVGL 不会为此调位图） */
    if (mpak_font_read_glyph_bmp(&self->mpk, g, self->src_scratch, src_sz) != MPAK_OK) {
        ESP_LOGE(TAG, "glyph U+%04" PRIx32 " read failed", letter);
        return NULL;
    }
    /* 4bpp(A4) → 8bpp(A8)：LVGL static 原始位图路径按 1B/px 取掩码 */
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *srow = self->src_scratch + (size_t)y * ((w + 1u) / 2u);
        uint8_t *drow = s->bmp + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t v = (x & 1u) ? (uint8_t)(srow[x >> 1] & 0x0Fu)
                                 : (uint8_t)(srow[x >> 1] >> 4);
            drow[x] = (uint8_t)(v * 17u);        /* 0..15 → 0..255 */
        }
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
    fl_inst_t *self = fl_at((int)id);
    if (self->open) {
        mpak_close(&self->mpk);
        self->open = false;
    }
    if (self->slot_bufs) {
        heap_caps_free(self->slot_bufs);
        self->slot_bufs = NULL;
    }
    if (self->src_scratch) {
        heap_caps_free(self->src_scratch);
        self->src_scratch = NULL;
    }
    memset(&self->font, 0, sizeof self->font);
}

int font_lazy_init(font_id_t id, const char *mpk_path)
{
    if ((int)id < 0 || id >= FONT_ID_COUNT || !mpk_path) return MPAK_ERR_ARG;
    fl_inst_t *self = fl_at((int)id);
    font_lazy_deinit(id);

    ESP_LOGW("font", "font_lazy_init open %s", mpk_path);
    int rc = mpak_open(&self->mpk, mpk_path, 0 /* hash 比对由调用方决定 */,
                       MPAK_KIND_FONT);
    ESP_LOGW("font", "mpak_open rc=%d", rc);
    if (rc != MPAK_OK) return rc;

    const mpak_font_t *ft = self->mpk.u.font;
    /* 【A8 展开 2026-10-01】LVGL 9.3+ 自定义字体必须给出 .static_bitmap 标记，
     * 否则 draw_letter 会把 get_glyph_bitmap 的返回值当 lv_draw_buf_t* 解析
     * （真机崩溃：EXCVADDR=0x0c000000 出现在 lv_draw_sw_blend_color_to_rgb565，
     * 中文位图头 8 字节被当地址；ASCII 侥幸没崩）。走 static 原始位图路径时
     * LVGL 要求 **A8（1B/px）**，所以这里按 w*h 算槽位，并把包里的 4bpp
     * 展开成 A8 再交给 LVGL。 */
    uint32_t a8_max = 0;
    for (uint32_t i = 0; i < ft->glyph_count; i++) {
        uint32_t sz = (uint32_t)ft->glyphs[i].w * (uint32_t)ft->glyphs[i].h;
        if (sz > a8_max) a8_max = sz;
    }
    self->slot_sz  = a8_max ? a8_max : 1;
    self->slot_bufs = heap_caps_malloc((size_t)FL_CACHE_SLOTS * self->slot_sz,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!self->slot_bufs) {
        mpak_close(&self->mpk);
        return MPAK_ERR_NOMEM;
    }
    self->src_scratch = heap_caps_malloc(ft->max_bmp_bytes ? ft->max_bmp_bytes : 1,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!self->src_scratch) {
        mpak_close(&self->mpk);
        heap_caps_free(self->slot_bufs); self->slot_bufs = NULL;
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
#if LV_VERSION_CHECK(9, 3, 0) || LVGL_VERSION_MAJOR >= 10
    /* 告诉 LVGL：位图是"原始静态"指针（环形缓存保证同步消费有效），
     * 走 lv_font_get_glyph_static_bitmap_internal 的 A8 mask 路径。 */
    self->font.static_bitmap    = 1;
#endif
    self->font.line_height      = ft->size_px;
    self->font.base_line        = self->base_line;

    self->open = true;
    ESP_LOGI(TAG, "font%d ready: %u glyphs, line=%u base=%" PRId32,
             (int)ft->size_px, ft->glyph_count, ft->size_px, self->base_line);
    return MPAK_OK;
}

const lv_font_t *font_lazy_get(font_id_t id)
{
    if ((int)id < 0 || id >= FONT_ID_COUNT) return NULL;
    fl_inst_t *c = fl_at((int)id);
    return c->open ? &c->font : NULL;
}

bool font_lazy_ready(font_id_t id)
{
    if ((int)id < 0 || id >= FONT_ID_COUNT) return false;
    return fl_at((int)id)->open;
}

int font_lazy_measure(font_id_t id, uint32_t codepoint, uint32_t *adv_w)
{
    if ((int)id < 0 || id >= FONT_ID_COUNT || !adv_w) return MPAK_ERR_ARG;
    fl_inst_t *c = fl_at((int)id);
    if (!c->open) return MPAK_ERR_ARG;
    const mpak_glyph_t *g;
    if (mpak_font_find_glyph(&c->mpk, codepoint, &g) != MPAK_OK)
        return MPAK_ERR_RANGE;
    *adv_w = g->advance;
    return MPAK_OK;
}
