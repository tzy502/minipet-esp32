/*
 * compositor.c — 自研合成器（POKER 场景全权写屏）+ render.h 公共 API 实现
 *
 * 帧循环（render_tick，30fps 由 app 定时器驱动）：
 *   1) 实体帧 delay 到 → 重合成实体层（piece 按 z 序 blit；表情件替换：
 *      piece.expr_index≠255 → 取 face 件 expr_group 组内第 active_expr 个变体）
 *   2) 条带 offset_x = (speed_x*ms/1000 + tilt*rx*K) mod 图宽（1x 世界）
 *   3) 增量合成候选区域：static_back → 条带 → tile → 时钟 → 实体 → 气泡
 *   4) 16×16 网格 hash diff → 行程合并 → display_blit（唯一写屏出口）
 *
 * 内存（PSRAM，heap_caps）：见文件尾 RENDER_PSRAM_BUDGET 注释。
 */
#include "compositor.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lvgl.h>

#include "drivers.h"
#include "entity_anim.h"
#include "clock_digits.h"
#include "font_lazy.h"
#include "font5x7.h"
#include "lvgl_bridge.h"
#include "render.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "rc";

/* ================= 静态状态 ================= */

static bool g_inited, g_menu;
static int32_t g_sw, g_sh;                 /* 屏幕尺寸（480×480） */

static uint16_t *g_fb;                     /* framebuffer（单一所有权） */

/* 地图场景 */
static bool g_map_ok;
static uint16_t *g_static;                 /* 屏幕尺寸（装载时按需 2x 展开） */
static uint16_t *g_tile;
static uint8_t  *g_tile_mask;              /* 屏幕尺寸 1bit */
static int64_t   g_map_epoch_us;

typedef struct {
    bool      ok;
    uint16_t *px;                          /* 1x 存储 */
    uint8_t  *mask;                        /* NULL=不透明 */
    uint16_t  w, h;
    uint32_t  stride_b;
    int16_t   y, speed_x;
    uint8_t   rx, blend;
    int32_t   last_off;
} rc_strip_t;
static rc_strip_t *g_strips;
static int         g_strip_n;

/* 实体 */
static uint16_t *g_ent_px;                 /* RC_ENT_W×RC_ENT_H（内容=2x 展开图，像素=屏幕像素） */
static uint8_t  *g_ent_cov;                /* 1bit 覆盖 */
static int32_t   g_ent_base_wx, g_ent_base_wy;   /* 世界 1x 附加偏移（默认 0,0） */

/* 实体画布（对齐桌面版 GetBounds 联合画布，见 LayoutPackWriter 语义注释）：
 * 导出 x/y = FinalX - body锚点（FinalX 已含 part origin，即位图左上角相对 body 锚点
 * 的偏移；不含帧位移 move）。设备画布 = 该动作全部帧 piece 矩形的联合包围盒，
 * 原点 = min(x,y)（等价桌面 bounds.Left/Top 画布原点，manifest 不带 bounds 时按
 * piece 联合包围盒现算）。绑定 parts+layout 后懒计算一次。 */
static bool    g_ent_cbox_ok;
static int32_t g_ent_cx0, g_ent_cy0;       /* 联合包围盒左上（世界 1x） */
static int32_t g_ent_cw,  g_ent_ch;        /* 联合包围盒宽高（世界 1x，已 clamp 到缓冲） */

static mpak_t g_parts;   static bool g_parts_ok;
static mpak_t g_lt_loop; static bool g_lt_loop_ok;   /* stand1 等循环动作 */
static mpak_t g_lt_once; static bool g_lt_once_ok;   /* 单次动作 */
static rc_anim_t g_anim;

/* 部件位图缓存（当前装扮；TF 懒读） */
typedef struct rc_part_img {
    const mpak_part_t *meta;
    uint16_t *px;
    uint8_t  *mask;
    struct rc_part_img *next;
} rc_part_img_t;
static rc_part_img_t *g_pc;
static uint32_t g_pc_bytes;
static bool g_pc_cap_logged;

/* 气泡 */
static struct { bool active; uint16_t *px; int32_t w, h, x, y; } g_bub;

/* 未配网常驻横幅（问题4：POKER 态顶部深色底白字，compose 最顶层） */
static bool g_banner_on;
static char g_banner_text[48];

/* IMU 视差（input 任务异步写；对齐 int32 写原子） */
static volatile int32_t g_tilt_mdeg;
static int32_t s_last_ent_tilt;            /* 实体已按此 tilt 值摆放（问题7 跟随标脏） */

/* 脏区网格（16×16 标记；问题1：合帧后合并为单一包围盒上屏） */
static uint8_t  g_mark[RC_GRID_MAX];
static int      g_gw, g_gh;
static time_t   g_last_clock_t;

/* ================= 小工具 ================= */

static void *psram(size_t n);

/* LE framebuffer → 驱动大端 RGB565 的上屏边界（定义见脏区节） */
static void blit_be(int32_t x, int32_t y, int32_t w, int32_t h,
                    const uint16_t *src, int32_t src_stride);

static void *psram(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) ESP_LOGE(TAG, "PSRAM alloc %zu failed", n);
    return p;
}

/* 实体显示尺寸：画布宽高 ×2（clamp 到缓冲与屏幕；画布未算出前为 0 → 无显示区） */
static void ent_disp_size(int32_t *dw, int32_t *dh)
{
    *dw = g_ent_cw * RC_SCALE;
    *dh = g_ent_ch * RC_SCALE;
    if (*dw > RC_ENT_W) *dw = RC_ENT_W;
    if (*dh > RC_ENT_H) *dh = RC_ENT_H;
    if (*dw > g_sw) *dw = g_sw;
}

/* 倾斜/拖拽 → 实体 x 偏移可见反馈（问题6/7）：±8° ↔ ±8px */
static int32_t ent_tilt_off_px(int32_t tilt_mdeg)
{
    int32_t px = (tilt_mdeg / 1000) * RC_TILT_ENT_PX_PER_DEG;
    if (px > RC_TILT_ENT_MAX_PX) px = RC_TILT_ENT_MAX_PX;
    if (px < -RC_TILT_ENT_MAX_PX) px = -RC_TILT_ENT_MAX_PX;
    return px;
}

/* 实体缓冲 → 屏幕摆放（问题2 修复）：
 * 世界 1x body 锚点 (0,0)（= 人物脚底基准，piece x/y 的原点）2x 后——
 *   水平钉在屏幕中心（+调参偏移 + tilt 可见偏移），
 *   垂直钉在 屏底-40px。
 * 不再按画布联合包围盒居中/贴底（武器/翅膀大件撑大包围盒会把人物挤偏左上）。
 * g_ent_base_wx/wy 为世界 1x 附加偏移（render_set_entity_pos，默认 0,0）。 */
static void ent_screen_pos_at(int32_t tilt_mdeg, int32_t *sx, int32_t *sy)
{
    *sx = g_sw / 2 + g_ent_cx0 * RC_SCALE + RC_ENT_CENTER_OFF_X
          + (g_ent_base_wx << RC_SCALE_SHIFT) + ent_tilt_off_px(tilt_mdeg);
    *sy = g_sh - RC_ENT_MARGIN_B + g_ent_cy0 * RC_SCALE + RC_ENT_CENTER_OFF_Y
          + (g_ent_base_wy << RC_SCALE_SHIFT);
}

static void ent_screen_pos(int32_t *sx, int32_t *sy)
{
    ent_screen_pos_at(g_tilt_mdeg, sx, sy);
}

static void mark_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!g_inited) return;
    int32_t x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_sw) x1 = g_sw;
    if (y1 > g_sh) y1 = g_sh;
    if (x >= x1 || y >= y1) return;
    int cx0 = (int)(x / RC_CELL), cy0 = (int)(y / RC_CELL);
    int cx1 = (int)((x1 - 1) / RC_CELL), cy1 = (int)((y1 - 1) / RC_CELL);
    if (cx1 >= g_gw) cx1 = g_gw - 1;
    if (cy1 >= g_gh) cy1 = g_gh - 1;
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++)
            g_mark[cy * g_gw + cx] = 1;
}

static void mark_ent_at(int32_t tilt_mdeg)
{
    int32_t ex, ey, dw, dh;
    ent_screen_pos_at(tilt_mdeg, &ex, &ey);
    ent_disp_size(&dw, &dh);
    mark_rect(ex, ey, dw, dh);
}

static void mark_ent(void)
{
    mark_ent_at(g_tilt_mdeg);
}

/* ================= 部件缓存（懒加载） ================= */

static const rc_part_img_t *pc_get(const mpak_part_t *meta)
{
    for (rc_part_img_t *e = g_pc; e; e = e->next)
        if (e->meta->id == meta->id) return e;

    uint32_t pb = (uint32_t)meta->h * rc_align4((uint32_t)meta->w * 2u);
    uint32_t mb = meta->has_alpha ? meta->mask_bytes : 0;
    if (g_pc_bytes + pb + mb > RC_PART_CACHE_CAP) {
        if (!g_pc_cap_logged) {
            ESP_LOGE(TAG, "part cache cap %" PRIu32 " exceeded", (uint32_t)RC_PART_CACHE_CAP);
            g_pc_cap_logged = true;
        }
        return NULL;
    }

    rc_part_img_t *e = malloc(sizeof *e);
    if (!e) return NULL;
    e->meta = meta;
    e->px   = psram(pb);
    e->mask = mb ? psram(mb) : NULL;
    if (!e->px || (mb && !e->mask)) {
        if (e->px) heap_caps_free(e->px);
        if (e->mask) heap_caps_free(e->mask);
        free(e);
        return NULL;
    }
    if (mpak_part_read_pixels(&g_parts, meta, (uint8_t *)e->px, pb) != MPAK_OK ||
        (mb && mpak_part_read_mask(&g_parts, meta, e->mask, mb) != MPAK_OK)) {
        ESP_LOGE(TAG, "part %u read failed", meta->id);
        heap_caps_free(e->px);
        if (e->mask) heap_caps_free(e->mask);
        free(e);
        return NULL;
    }
    e->next = g_pc;
    g_pc = e;
    g_pc_bytes += pb + mb;
    return e;
}

static void pc_flush(void)
{
    rc_part_img_t *e = g_pc;
    while (e) {
        rc_part_img_t *n = e->next;
        heap_caps_free(e->px);
        if (e->mask) heap_caps_free(e->mask);
        free(e);
        e = n;
    }
    g_pc = NULL;
    g_pc_bytes = 0;
    g_pc_cap_logged = false;
}

/* ================= 实体画布/布局接线 ================= */

static const mpak_layout_t *active_layout(void)
{
    if (g_lt_once_ok) return g_lt_once.u.layout;
    if (g_lt_loop_ok) return g_lt_loop.u.layout;
    return NULL;
}

/* parts/layout 任一变化后失效，下次重合成前按新数据现算 */
static void ent_canvas_invalidate(void)
{
    g_ent_cbox_ok = false;
}

/* 扫描当前布局全部帧的 piece 矩形（x/y 与 part w/h），求联合包围盒。
 * 语义（LayoutPackWriter）：设备画布 = 全 piece 矩形联合，起点 = min(x/y)；
 * 画布尺寸不落 manifest 时按此联合包围盒取（对齐桌面 GetBounds 的 union 画布）。 */
static void ent_canvas_update(void)
{
    if (g_ent_cbox_ok) return;
    g_ent_cw = 0; g_ent_ch = 0;
    const mpak_layout_t *lt = active_layout();
    if (!lt || !g_parts_ok) return;

    int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool any = false;
    for (uint32_t f = 0; f < lt->frame_count; f++) {
        const mpak_frame_t *fr = &lt->frames[f];
        for (uint32_t k = 0; k < fr->piece_count; k++) {
            const mpak_piece_t *pc = &lt->pieces[fr->piece_off + k];
            const mpak_part_t *meta = mpak_parts_find(&g_parts, pc->part_id);
            if (!meta) continue;
            int32_t x0 = pc->x, y0 = pc->y;
            int32_t x1 = x0 + (int32_t)meta->w, y1 = y0 + (int32_t)meta->h;
            if (!any) {
                minX = x0; minY = y0; maxX = x1; maxY = y1;
                any = true;
            } else {
                if (x0 < minX) minX = x0;
                if (y0 < minY) minY = y0;
                if (x1 > maxX) maxX = x1;
                if (y1 > maxY) maxY = y1;
            }
        }
    }
    if (!any) return;
    int32_t cw = maxX - minX, ch = maxY - minY;
    int32_t cx0 = minX, cy0 = minY;
    /* 画布超出缓冲窗口的退化情形：横向取以 body 锚点(0,0)为中心的窗口
     * （人物保持居中，武器/翅膀大件外溢裁剪）；纵向取底部窗口（保脚底对齐） */
    int32_t max_w = RC_ENT_W / RC_SCALE, max_h = RC_ENT_H / RC_SCALE;
    if (cw > max_w) { cw = max_w; cx0 = -max_w / 2; }
    if (ch > max_h) { ch = max_h; cy0 = maxY - max_h; }
    g_ent_cx0 = cx0;
    g_ent_cy0 = cy0;
    g_ent_cw = cw;
    g_ent_ch = ch;
    g_ent_cbox_ok = true;
    ESP_LOGI(TAG, "ent canvas union origin(%" PRId32 ",%" PRId32 ") %"
             PRId32 "x%" PRId32, g_ent_cx0, g_ent_cy0, g_ent_cw, g_ent_ch);
}

static void bind_active_layout(int64_t now_us, bool reset_expr)
{
    const mpak_layout_t *lt = active_layout();
    if (!lt) return;
    rc_anim_bind(&g_anim, lt, !g_lt_once_ok, reset_expr, now_us);
    ent_canvas_invalidate();
}

/* ================= 实体层合成 ================= */

static const rc_part_img_t *resolve_piece(const mpak_piece_t *piece)
{
    const mpak_part_t *meta = mpak_parts_find(&g_parts, piece->part_id);
    if (!meta) {
        ESP_LOGW(TAG, "piece part %u not in PARTS pkg", piece->part_id);
        return NULL;
    }
    if (piece->expr_index != MPAK_EXPR_NONE && meta->expr_group != 0) {
        const mpak_part_t *v = mpak_parts_variant(
            &g_parts, meta, (uint32_t)rc_anim_active_expr(&g_anim));
        if (v) meta = v;
    }
    return pc_get(meta);
}

/* 2x nearest blit 进实体缓冲（含 1bit 掩码、水平翻转、2×2 块展开、覆盖位） */
static void blit_ent_2x(const rc_part_img_t *img, bool hflip, int32_t bx, int32_t by)
{
    const uint16_t w = img->meta->w, h = img->meta->h;
    const uint32_t stride_el = rc_align4((uint32_t)w * 2u) / 2u;

    for (uint32_t sy = 0; sy < h; sy++) {
        const uint16_t *srow = img->px + (size_t)sy * stride_el;
        int32_t Y0 = by + (int32_t)(sy << RC_SCALE_SHIFT);
        if (Y0 + 1 < 0 || Y0 >= RC_ENT_H) continue;
        for (uint32_t sx = 0; sx < w; sx++) {
            if (img->mask && !rc_mask_bit(img->mask, sy * w + sx)) continue;
            uint32_t sxx = hflip ? (uint32_t)(w - 1 - sx) : sx;
            uint16_t c = srow[sxx];
            int32_t X0 = bx + (int32_t)(sx << RC_SCALE_SHIFT);
            for (int32_t dy = 0; dy < RC_SCALE; dy++) {
                int32_t Y = Y0 + dy;
                if (Y < 0 || Y >= RC_ENT_H) continue;
                for (int32_t dx = 0; dx < RC_SCALE; dx++) {
                    int32_t X = X0 + dx;
                    if (X < 0 || X >= RC_ENT_W) continue;
                    uint32_t idx = (uint32_t)Y * RC_ENT_W + (uint32_t)X;
                    g_ent_px[idx] = c;
                    rc_mask_set(g_ent_cov, idx);
                }
            }
        }
    }
}

static void recompose_entity(void)
{
    memset(g_ent_px, 0, (size_t)RC_ENT_W * RC_ENT_H * 2u);
    memset(g_ent_cov, 0, RC_ENT_COV_BYTES);

    const mpak_layout_t *lt = active_layout();
    if (!lt || !g_parts_ok) return;
    if (g_anim.frame_idx >= lt->frame_count) return;
    ent_canvas_update();

    const mpak_frame_t *fr = &lt->frames[g_anim.frame_idx];
    /* 帧内 piece 列表顺序 = 权威绘制序（导出端按桌面 RenderFrame 底→顶排列：
     * OrderByDescending(ZIndex)，z 字段仅诊断参考）→ 顺序画，不再排序 */
    for (uint32_t k = 0; k < fr->piece_count; k++) {
        const mpak_piece_t *piece = &lt->pieces[fr->piece_off + k];
        const rc_part_img_t *img = resolve_piece(piece);
        if (!img) continue;
        /* 导出 x/y = 位图左上角相对 body 锚点坐标（FinalX 已含 part origin，
         * 不再减 origin）；+帧位移 move（桌面同轴：画布内绝对位移，非累计），
         * -联合画布原点 → 实体缓冲内位置（世界 1x → 2x 移位展开） */
        /* LAYOUT x/y 已含帧位移（导出契约：画布绝对坐标），move 字段仅参考，勿重复叠加 */
        int32_t bx = ((int32_t)piece->x - g_ent_cx0) << RC_SCALE_SHIFT;
        int32_t by = ((int32_t)piece->y - g_ent_cy0) << RC_SCALE_SHIFT;
        blit_ent_2x(img, piece->flip & 1u, bx, by);
    }
}

/* ================= 条带 ================= */

/* 世界 1x 偏移（已折叠周期：mod 图宽；speed + IMU 视差合成） */
static int32_t strip_offset(const rc_strip_t *s, int64_t now_us)
{
    int64_t ms = (now_us - g_map_epoch_us) / 1000;
    int64_t t_off = (int64_t)s->speed_x * ms / 1000;
    int64_t v_off = ((int64_t)g_tilt_mdeg * s->rx * RC_TILT_PX_PER_DEG) /
                    (100 * 1000);
    int64_t w = s->w;
    int64_t off = (t_off + v_off) % w;
    if (off < 0) off += w;
    return (int32_t)off;
}

static void strip_blit(const rc_strip_t *s, int32_t rx0, int32_t ry0,
                       int32_t rw, int32_t rh)
{
    if (!s->ok) return;
    int32_t band_y = (int32_t)s->y << RC_SCALE_SHIFT;
    int32_t band_h = (int32_t)s->h << RC_SCALE_SHIFT;
    int32_t y0 = band_y > ry0 ? band_y : ry0;
    int32_t y1 = (band_y + band_h < ry0 + rh) ? band_y + band_h : ry0 + rh;
    if (y0 >= y1) return;

    int32_t period = (int32_t)s->w << RC_SCALE_SHIFT;
    if (period <= 0) return;
    int32_t off2 = s->last_off << RC_SCALE_SHIFT;
    const uint32_t stride_el = s->stride_b / 2u;

    for (int32_t sy = y0; sy < y1; sy++) {
        int32_t src_y = (sy - band_y) >> RC_SCALE_SHIFT;
        const uint16_t *srow = s->px + (size_t)src_y * stride_el;
        uint16_t *drow = g_fb + (size_t)sy * g_sw;
        for (int32_t sx = rx0; sx < rx0 + rw; sx++) {
            int32_t m = (sx + off2) % period;
            if (m < 0) m += period;
            int32_t src_x = m >> RC_SCALE_SHIFT;
            if (s->mask && !rc_mask_bit(s->mask, (uint32_t)src_y * s->w + (uint32_t)src_x))
                continue;
            drow[sx] = srow[src_x];
        }
    }
}

/* ================= 区域合成（全层重算，幂等） ================= */

/* 横幅字形像素块填充（问题3 描边用）：scaled 块向四周外扩 grow px 涂 color，
 * 严格裁剪到本合成区域列范围 [clip_x, clip_x+clip_w) 与横幅带行 [by0, by1)
 * ——g_fb 常驻，绝不可写出区域外（会污染后续增量合成的残留画面） */
static void banner_fill_block(int32_t px0, int32_t py0, int32_t grow,
                              uint16_t color, int32_t clip_x, int32_t clip_w,
                              int32_t by0, int32_t by1)
{
    for (int32_t py = py0 - grow; py < py0 + RC_BANNER_SCALE + grow; py++) {
        if (py < by0 || py >= by1) continue;
        uint16_t *drow = g_fb + (size_t)py * g_sw;
        for (int32_t px = px0 - grow; px < px0 + RC_BANNER_SCALE + grow; px++) {
            if (px < clip_x || px >= clip_x + clip_w || px >= g_sw) continue;
            drow[px] = color;
        }
    }
}

static void compose_region(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_sw) w = g_sw - x;
    if (y + h > g_sh) h = g_sh - y;
    if (w <= 0 || h <= 0) return;

    /* 0) CLOCK_DOZE（问题3/E9）：AMOLED 纯黑背景只数字发光——
     * 时钟激活即 doze 语义（enable 仅由 CLOCK_DOZE 进出指令驱动），
     * 黑底 + 时钟，跳过条带/tile/实体/气泡/横幅 */
    if (clock_digits_active()) {
        for (int32_t r = y; r < y + h; r++)
            memset(g_fb + (size_t)r * g_sw + x, 0, (size_t)w * 2u);
        clock_digits_compose(g_fb, g_sw, RC_SCALE, x, y, w, h);
        return;
    }

    /* 1) static_back（不动底；无地图 → 黑底） */
    for (int32_t r = y; r < y + h; r++) {
        uint16_t *drow = g_fb + (size_t)r * g_sw;
        if (g_static) memcpy(drow, g_static + (size_t)r * g_sw, (size_t)w * 2u);
        else memset(drow, 0, (size_t)w * 2u);
    }

    /* 2) 条带（x 向循环平铺） */
    for (int i = 0; i < g_strip_n; i++)
        strip_blit(&g_strips[i], x, y, w, h);

    /* 3) tile_layer（1bit alpha 叠加） */
    if (g_tile) {
        for (int32_t r = y; r < y + h; r++) {
            const uint16_t *srow = g_tile + (size_t)r * g_sw;
            const uint8_t  *mrow = g_tile_mask ?
                g_tile_mask + (size_t)r * ((g_sw + 7) / 8) : NULL;
            uint16_t *drow = g_fb + (size_t)r * g_sw;
            if (!mrow) {
                memcpy(drow, srow, (size_t)w * 2u);
            } else {
                const uint8_t *mseg = mrow + (x / 8);
                for (int32_t c = 0; c < w; c++) {
                    if (rc_mask_bit(mseg, (uint32_t)(x + c) - (x & ~7)))
                        drow[c] = srow[c];
                }
            }
        }
    }

    /* 4) 地图时钟（场景层，实体之下） */
    clock_digits_compose(g_fb, g_sw, RC_SCALE, x, y, w, h);

    /* 5) 实体缓冲（1bit 覆盖；显示区 = 画布尺寸×2 的摆放矩形） */
    {
        int32_t ex, ey, dw, dh;
        ent_screen_pos(&ex, &ey);
        ent_disp_size(&dw, &dh);
        int32_t X0 = ex > x ? ex : x, Y0 = ey > y ? ey : y;
        int32_t X1 = (ex + dw < x + w) ? ex + dw : x + w;
        int32_t Y1 = (ey + dh < y + h) ? ey + dh : y + h;
        for (int32_t sy = Y0; sy < Y1; sy++) {
            uint32_t erow = (uint32_t)(sy - ey) * RC_ENT_W;
            uint16_t *drow = g_fb + (size_t)sy * g_sw;
            for (int32_t sx = X0; sx < X1; sx++) {
                uint32_t eidx = erow + (uint32_t)(sx - ex);
                if (rc_mask_bit(g_ent_cov, eidx))
                    drow[sx] = g_ent_px[eidx];
            }
        }
    }

    /* 6) 气泡（不透明矩形位图） */
    if (g_bub.active) {
        int32_t X0 = g_bub.x > x ? g_bub.x : x, Y0 = g_bub.y > y ? g_bub.y : y;
        int32_t X1 = (g_bub.x + g_bub.w < x + w) ? g_bub.x + g_bub.w : x + w;
        int32_t Y1 = (g_bub.y + g_bub.h < y + h) ? g_bub.y + g_bub.h : y + h;
        for (int32_t r = Y0; r < Y1; r++) {
            memcpy(g_fb + (size_t)r * g_sw + X0,
                   g_bub.px + (size_t)(r - g_bub.y) * RC_BUBBLE_MAX_W +
                       (X0 - g_bub.x),
                   (size_t)(X1 - X0) * 2u);
        }
    }

    /* 7) 未配网常驻横幅（问题4：顶部 480×28 深色底白字，compose 最顶层）
     * 取字模（与 font5x7.h 数据格式核对一致）：下标=字符 ASCII 值（指定初始化器，
     * 等价 字符-' ' 起点式表），每字符 5 列字节、无 stride，bit0=顶行。
     * 问题3 可读性加固：先整趟 1px 黑描边（块外扩 1px）再整趟白字填充。 */
    if (g_banner_on) {
        int32_t by0 = y > 0 ? y : 0;
        int32_t by1 = (y + h < RC_BANNER_H) ? y + h : RC_BANNER_H;
        if (by0 < by1) {
            for (int32_t r = by0; r < by1; r++) {
                uint16_t *drow = g_fb + (size_t)r * g_sw;
                for (int32_t c = x; c < x + w; c++) drow[c] = RC_BANNER_BG;
            }
            for (int pass = 0; pass < 2; pass++) {
                int32_t gx = RC_BANNER_PAD_X;
                for (const char *p = g_banner_text; *p && gx < g_sw; p++) {
                    unsigned char u = (unsigned char)*p;
                    if (u >= 128) u = '?';
                    const uint8_t *cols = MP_FONT5X7[u];
                    for (int col = 0; col < MP_FONT_GLYPH_W; col++) {
                        for (int row = 0; row < MP_FONT_GLYPH_H; row++) {
                            if (!(cols[col] & (1u << row))) continue;
                            int32_t px0 = gx + col * RC_BANNER_SCALE;
                            int32_t py0 = RC_BANNER_PAD_Y + row * RC_BANNER_SCALE;
                            banner_fill_block(px0, py0,
                                              pass == 0 ? 1 : 0,
                                              pass == 0 ? RC_BANNER_OUTLINE
                                                        : RC_BANNER_FG,
                                              x, w, by0, by1);
                        }
                    }
                    gx += (MP_FONT_GLYPH_W + 1) * RC_BANNER_SCALE;
                }
            }
        }
    }
}

/* ================= 脏区：标脏 16×16 块 → 单一包围盒（问题1） =================
 * 旧实现：逐行行程分别 compose+blit，且 blit 前按 hash 过滤把行内连续变化段
 * 拆成多段——相邻矩形之间的间隙像素漏刷，叠加 display_blit 内部 2px 向外取偶
 * 的外扩错位，真机表现为行/列裂纹与残影。现改为：本帧所有标脏块合并为一个
 * 包围盒，一次 compose_region + 一次 blit（480 宽全帧重绘成本可接受，
 * 先正确后优化）；实体/条带每帧整体重绘各自包围盒由 mark_ent/mark_rect 保证。 */
static void flush_dirty(void)
{
    int32_t cx0 = -1, cy0 = -1, cx1 = -1, cy1 = -1;
    for (int32_t cy = 0; cy < g_gh; cy++) {
        for (int32_t cx = 0; cx < g_gw; cx++) {
            if (!g_mark[cy * g_gw + cx]) continue;
            g_mark[cy * g_gw + cx] = 0;
            if (cx0 < 0 || cx < cx0) cx0 = cx;
            if (cy0 < 0 || cy < cy0) cy0 = cy;
            if (cx > cx1) cx1 = cx;
            if (cy > cy1) cy1 = cy;
        }
    }
    if (cx0 < 0) return;

    int32_t x = cx0 * RC_CELL, y = cy0 * RC_CELL;
    int32_t w = (cx1 - cx0 + 1) * RC_CELL, h = (cy1 - cy0 + 1) * RC_CELL;
    if (x + w > g_sw) w = g_sw - x;
    if (y + h > g_sh) h = g_sh - y;
    compose_region(x, y, w, h);
    blit_be(x, y, w, h, g_fb + (size_t)y * g_sw + x, g_sw);
}

/* ================= 上屏边界：LE framebuffer → 驱动大端 RGB565 =================
 * 全管线按小端 u16 处理（资产小端 + LVGL 小端一致）；
 * display_blit 要求大端字节序，故在此唯一边界做逐像素字节交换，
 * 分块经内部 RAM 暂存（8KB → 480 宽行 × 8 行），避免 PSRAM 二份帧缓冲。 */
static uint8_t s_blit_stage[8192];

static void blit_be(int32_t x, int32_t y, int32_t w, int32_t h,
                    const uint16_t *src, int32_t src_stride)
{
    const int32_t chunk_px = (int32_t)sizeof s_blit_stage / 2;
    int32_t rows_per = (w > 0) ? chunk_px / w : 0;
    if (rows_per < 1) rows_per = 1;

    for (int32_t r0 = 0; r0 < h; r0 += rows_per) {
        int32_t hh = (r0 + rows_per < h) ? rows_per : (h - r0);
        uint8_t *d = s_blit_stage;
        for (int32_t r = 0; r < hh; r++) {
            const uint16_t *s = src + (size_t)(r0 + r) * src_stride;
            for (int32_t c = 0; c < w; c++) {
                uint16_t v = s[c];
                *d++ = (uint8_t)(v >> 8);
                *d++ = (uint8_t)v;
            }
        }
        display_blit((int)x, (int)(y + r0), (int)w, (int)hh, s_blit_stage);
    }
}

static void full_recompose(void)
{
    compose_region(0, 0, g_sw, g_sh);
    memset(g_mark, 0, (size_t)g_gw * g_gh);
    blit_be(0, 0, g_sw, g_sh, g_fb, g_sw);
}

/* ================= 地图装载 ================= */

static void scene_free(void)
{
    if (g_static)    { heap_caps_free(g_static);    g_static = NULL; }
    if (g_tile)      { heap_caps_free(g_tile);      g_tile = NULL; }
    if (g_tile_mask) { heap_caps_free(g_tile_mask); g_tile_mask = NULL; }
    if (g_strips) {
        for (int i = 0; i < g_strip_n; i++) {
            if (g_strips[i].px)   heap_caps_free(g_strips[i].px);
            if (g_strips[i].mask) heap_caps_free(g_strips[i].mask);
        }
        heap_caps_free(g_strips);
        g_strips = NULL;
    }
    g_strip_n = 0;
    g_map_ok = false;
}

/* 读整幅 RGB565 层进屏幕尺寸缓冲（预烘焙直接用；1x 存储则 2x 展开） */
static uint16_t *layer_rgb_load(const mpak_t *m, uint32_t off, uint32_t len,
                                uint16_t vw, uint16_t vh)
{
    uint32_t stride_b = rc_align4((uint32_t)vw * 2u);
    if (len != (uint32_t)vh * stride_b) return NULL;

    uint16_t *dst = psram((size_t)g_sw * g_sh * 2u);
    if (!dst) return NULL;

    if (vw == (uint16_t)g_sw && vh == (uint16_t)g_sh &&
        stride_b == (uint32_t)g_sw * 2u) {
        if (mpak_read_at((mpak_t *)m, m->payload_off + off, dst, len) != MPAK_OK) {
            heap_caps_free(dst);
            return NULL;
        }
        return dst;
    }

    if ((int32_t)vw * RC_SCALE != g_sw || (int32_t)vh * RC_SCALE != g_sh) {
        heap_caps_free(dst);
        return NULL;
    }
    uint8_t *raw = psram(len);
    if (!raw) { heap_caps_free(dst); return NULL; }
    if (mpak_read_at((mpak_t *)m, m->payload_off + off, raw, len) != MPAK_OK) {
        heap_caps_free(raw); heap_caps_free(dst);
        return NULL;
    }
    uint32_t stride_el = stride_b / 2u;
    for (int32_t dy = 0; dy < g_sh; dy++) {
        const uint16_t *srow = (const uint16_t *)raw + (size_t)(dy >> 1) * stride_el;
        uint16_t *drow = dst + (size_t)dy * g_sw;
        for (int32_t dx = 0; dx < g_sw; dx++) drow[dx] = srow[dx >> 1];
    }
    heap_caps_free(raw);
    return dst;
}

static uint8_t *tile_mask_load(const mpak_t *m, uint32_t off, uint32_t len,
                               uint16_t vw, uint16_t vh)
{
    if (len == 0) return NULL;
    size_t raw_bits = (size_t)vw * vh;
    if (len != (raw_bits + 7) / 8) return NULL;
    uint8_t *raw = psram(len);
    if (!raw) return NULL;
    if (mpak_read_at((mpak_t *)m, m->payload_off + off, raw, len) != MPAK_OK) {
        heap_caps_free(raw);
        return NULL;
    }
    uint8_t *dst = psram(((size_t)g_sw * g_sh + 7) / 8);
    if (!dst) { heap_caps_free(raw); return NULL; }
    for (int32_t dy = 0; dy < g_sh; dy++)
        for (int32_t dx = 0; dx < g_sw; dx++) {
            uint32_t sidx = (uint32_t)(dy >> 1) * vw + (uint32_t)(dx >> 1);
            if (rc_mask_bit(raw, sidx))
                rc_mask_set(dst, (uint32_t)dy * g_sw + dx);
        }
    heap_caps_free(raw);
    return dst;
}

static int strip_load(rc_strip_t *s, const char *path, const mpak_strip_t *hdr)
{
    memset(s, 0, sizeof *s);
    mpak_t pm;
    if (mpak_open(&pm, path, 0, MPAK_KIND_PARTS) != MPAK_OK) return MPAK_ERR_IO;
    if (pm.parts_count < 1 || !pm.parts_tab) { mpak_close(&pm); return MPAK_ERR_FMT; }
    const mpak_part_t *p = &pm.parts_tab[0];

    s->stride_b = rc_align4((uint32_t)p->w * 2u);
    s->w = p->w; s->h = p->h;
    s->px = psram((size_t)p->h * s->stride_b);
    if (!s->px) { mpak_close(&pm); return MPAK_ERR_NOMEM; }
    if (mpak_part_read_pixels(&pm, p, (uint8_t *)s->px,
                              (size_t)p->h * s->stride_b) != MPAK_OK) {
        heap_caps_free(s->px); s->px = NULL;
        mpak_close(&pm);
        return MPAK_ERR_IO;
    }
    if (p->has_alpha && (hdr->blend & 1u)) {
        s->mask = psram(p->mask_bytes);
        if (s->mask)
            mpak_part_read_mask(&pm, p, s->mask, p->mask_bytes);
    }
    mpak_close(&pm);   /* 条带小图已全量入 RAM */

    s->y       = hdr->y;
    s->speed_x = hdr->speed_x;
    s->rx      = hdr->rx_parallax;
    s->blend   = hdr->blend;
    s->ok      = true;
    return MPAK_OK;
}

/* ================= render.h 公共 API ================= */

int render_init(const minipet_profile_t *profile)
{
    if (!profile) return RENDER_ERR_ARG;
    if (g_inited) return RENDER_OK;

    g_sw = profile->width;
    g_sh = profile->height;
    if (g_sw <= 0 || g_sh <= 0 || g_sw > 512 || g_sh > 512)
        return RENDER_ERR_ARG;

    g_gw = (int)((g_sw + RC_CELL - 1) / RC_CELL);
    g_gh = (int)((g_sh + RC_CELL - 1) / RC_CELL);
    if (g_gw * g_gh > RC_GRID_MAX) return RENDER_ERR_ARG;

    if (display_init() != 0) {
        ESP_LOGE(TAG, "display_init failed");
        return RENDER_ERR_STATE;
    }

    g_fb     = psram((size_t)g_sw * g_sh * 2u);
    g_ent_px = psram((size_t)RC_ENT_W * RC_ENT_H * 2u);
    g_ent_cov = psram(RC_ENT_COV_BYTES);
    if (!g_fb || !g_ent_px || !g_ent_cov) return RENDER_ERR_NOMEM;
    /* PSRAM 不保证清零：覆盖位/像素必须先清空，否则首次 full_recompose 会把
     * 未初始化覆盖位当已画像素上屏（真机表现为黑色竖条 + 随机竖条纹残留） */
    memset(g_ent_px, 0, (size_t)RC_ENT_W * RC_ENT_H * 2u);
    memset(g_ent_cov, 0, RC_ENT_COV_BYTES);

    rc_anim_init(&g_anim);

    /* 问题3：屏尺寸/比例先行告知时钟模块（默认居中锚点与 get_rect 标脏依赖；
     * 旧实现 scale 在首次 compose 才赋值 → enable 后时钟矩形恒 0 永不标脏） */
    clock_digits_set_screen(g_sw, g_sh);

    int rc = bridge_init(g_sw, g_sh);
    if (rc != RENDER_OK) return rc;

    g_inited = true;
    full_recompose();   /* 黑底首帧 + hash 基线 + 全幅上屏 */
    ESP_LOGI(TAG, "render_init ok %dx%d", (int)g_sw, (int)g_sh);
    return RENDER_OK;
}

void render_tick(void)
{
    if (!g_inited) return;
    int64_t now_us = esp_timer_get_time();

    if (g_menu) {
        /* MENU：LVGL 整屏离屏 → 直拷 framebuffer → 上屏（合成器让路）。
         * 问题4 加固：每帧全屏重绘。DIRECT 模式下 LVGL 只重绘失效区，
         * menu_buf 常驻持有完整画面；若按脏 bbox 增量上屏，LVGL「本帧无
         * 失效区」时无 flush → 不 blit，未刷新区域与残留叠加会闪烁。 */
        lv_timer_handler();
        const uint16_t *mb = bridge_menu_buf();
        if (mb) {
            for (int32_t r = 0; r < g_sh; r++)
                memcpy(g_fb + (size_t)r * g_sw, mb + (size_t)r * g_sw,
                       (size_t)g_sw * 2u);
            blit_be(0, 0, g_sw, g_sh, g_fb, g_sw);
        }
        return;
    }

    bool any = false;

    /* 1) 实体动画帧/表情/blink */
    rc_anim_ev_t ev;
    if (rc_anim_advance(&g_anim, now_us, &ev)) {
        if (ev.finished) {
            /* 单次动作播完 → 回退 standby 循环布局（stand1） */
            mark_ent();
            mpak_close(&g_lt_once);
            g_lt_once_ok = false;
            bind_active_layout(now_us, false);
            recompose_entity();
            mark_ent();
            any = true;
        } else {
            mark_ent();                          /* 旧位置 */
            /* 帧位移 move 已在实体画布内逐帧绝对叠加（recompose_entity，
             * 对齐桌面 +mv 语义），不再累计到屏幕锚点 */
            recompose_entity();
            mark_ent();                          /* 新位置 */
            any = true;
        }
    }

    /* 2) 条带偏移（时间驱动 + IMU 视差；offset 不变则零成本） */
    for (int i = 0; i < g_strip_n; i++) {
        if (!g_strips[i].ok) continue;
        int32_t off = strip_offset(&g_strips[i], now_us);
        if (off != g_strips[i].last_off) {
            g_strips[i].last_off = off;
            mark_rect(0, (int32_t)g_strips[i].y << RC_SCALE_SHIFT,
                      g_sw, (int32_t)g_strips[i].h << RC_SCALE_SHIFT);
            any = true;
        }
    }

    /* 3) 地图时钟（每秒标脏；comma 偶显奇隐） */
    if (clock_digits_active()) {
        time_t t = time(NULL);
        if (t != g_last_clock_t) {
            g_last_clock_t = t;
            int32_t cx, cy, cw, ch;
            if (clock_digits_get_rect(&cx, &cy, &cw, &ch))
                mark_rect(cx, cy, cw, ch);
            any = true;
        }
    }

    /* 4) 倾斜/拖拽视差 → 实体 x 偏移跟随（问题6/7 可见反馈：±8° ↔ ±8px；
     * 条带偏移变化已在步骤 2 标脏，实体需另行以新旧位置标脏防残影） */
    if (g_tilt_mdeg != s_last_ent_tilt) {
        mark_ent_at(s_last_ent_tilt);        /* 旧位置 */
        s_last_ent_tilt = g_tilt_mdeg;
        mark_ent();                          /* 新位置 */
        any = true;
    }

    if (any) flush_dirty();
}

int render_set_parts(const char *mpk_path)
{
    if (!g_inited || !mpk_path) return RENDER_ERR_ARG;
    mpak_t tmp;
    int rc = mpak_open(&tmp, mpk_path, 0, MPAK_KIND_PARTS);
    if (rc != MPAK_OK) return rc;

    pc_flush();
    if (g_parts_ok) mpak_close(&g_parts);
    g_parts = tmp;             /* FILE* 所有权转移 */
    g_parts_ok = true;
    ent_canvas_invalidate();   /* part 尺寸可能变化 → 画布联合包围盒重算 */

    mark_ent();                /* 下一 tick 以新部件重合成实体层 */
    return RENDER_OK;
}

int render_set_layout(const char *mpk_path, bool loop)
{
    if (!g_inited || !mpk_path) return RENDER_ERR_ARG;
    mpak_t tmp;
    int rc = mpak_open(&tmp, mpk_path, 0, MPAK_KIND_LAYOUT);
    if (rc != MPAK_OK) return rc;

    mark_ent();
    if (loop) {
        if (g_lt_loop_ok) mpak_close(&g_lt_loop);
        g_lt_loop = tmp;
        g_lt_loop_ok = true;
    } else {
        if (g_lt_once_ok) mpak_close(&g_lt_once);
        g_lt_once = tmp;
        g_lt_once_ok = true;
    }
    bind_active_layout(esp_timer_get_time(), false);
    recompose_entity();
    mark_ent();
    return RENDER_OK;
}

int render_set_expression(const char *name)
{
    if (!g_inited || !name) return RENDER_ERR_ARG;
    int rc = rc_anim_set_expression(&g_anim, name);
    if (rc == 0) {
        mark_ent();            /* 旧覆盖区（表情件形状可能缩小） */
        recompose_entity();
        mark_ent();
    }
    return rc;
}

/* 强制一次全屏重合成 + 全幅上屏（脏区基线同步重建）。
 * 用于外部直写面板（面板自检色块等）或素材全量重绑后清除残留：
 * 无 BGMAP → 全屏填黑；有 BGMAP → static_back+条带+tile 一次铺满。 */
void render_force_redraw(void)
{
    if (!g_inited) return;
    full_recompose();
}

int render_set_map(const char *bgmap_path,
                   const char *strip_parts_paths[], int strip_count)
{
    if (!g_inited || !bgmap_path) return RENDER_ERR_ARG;

    mpak_t bm;
    int rc = mpak_open(&bm, bgmap_path, 0, MPAK_KIND_BGMAP);
    if (rc != MPAK_OK) return rc;
    const mpak_bgmap_t *bg = bm.u.bgmap;

    if ((int)bg->strip_count != strip_count) {
        ESP_LOGE(TAG, "strip count mismatch: bgmap=%u given=%d",
                 bg->strip_count, strip_count);
        mpak_close(&bm);
        return RENDER_ERR_ARG;
    }

    scene_free();

    g_static = layer_rgb_load(&bm, bg->static_back_off, bg->static_back_len,
                              bg->vw, bg->vh);
    if (!g_static) {
        ESP_LOGE(TAG, "static_back load failed (vw=%u vh=%u)", bg->vw, bg->vh);
        mpak_close(&bm);
        full_recompose();
        return MPAK_ERR_FMT;
    }
    if (bg->tile_layer_len) {
        uint32_t stride_b = rc_align4((uint32_t)bg->vw * 2u);
        g_tile = layer_rgb_load(&bm, bg->tile_layer_off,
                                (uint32_t)bg->vh * stride_b, bg->vw, bg->vh);
        g_tile_mask = tile_mask_load(&bm,
                                     bg->tile_layer_off + (uint32_t)bg->vh * stride_b,
                                     bg->tile_layer_len - (uint32_t)bg->vh * stride_b,
                                     bg->vw, bg->vh);
    }

    if (strip_count > 0) {
        g_strips = psram((size_t)strip_count * sizeof(rc_strip_t));
        if (!g_strips) { strip_count = 0; }
        g_strip_n = strip_count;
        for (int i = 0; i < strip_count; i++) {
            rc = strip_load(&g_strips[i], strip_parts_paths[i], &bg->strips[i]);
            if (rc != MPAK_OK) {
                ESP_LOGE(TAG, "strip %d load failed (%s)", i, strip_parts_paths[i]);
                g_strips[i].ok = false;   /* 跳过该条带，其余照常 */
            }
        }
    }

    g_map_epoch_us = esp_timer_get_time();
    g_map_ok = true;
    mpak_close(&bm);           /* 场景已全量入 PSRAM */

    full_recompose();
    return RENDER_OK;
}

void render_set_entity_pos(int16_t world_x, int16_t world_y)
{
    if (!g_inited) return;
    mark_ent();
    g_ent_base_wx = world_x;
    g_ent_base_wy = world_y;
    mark_ent();
}

int render_set_clock(const char *fonttime_parts_path,
                     int16_t anchor_world_x, int16_t anchor_world_y, bool enable)
{
    if (!g_inited) return RENDER_ERR_ARG;
    int rc = clock_digits_configure(fonttime_parts_path, anchor_world_x,
                                    anchor_world_y, enable);
    if (rc != MPAK_OK) return rc;
    g_last_clock_t = 0;        /* 下一 tick 立即标脏 */
    /* 问题3：enable/disable 都全幅重合成——doze 语义是「纯黑底+时钟」，
     * 必须清掉进 doze 前的场景残留（局部标脏会留下旧画面） */
    full_recompose();
    return RENDER_OK;
}

int render_set_font(render_font_t id, const char *mpk_path)
{
    if (!g_inited || !mpk_path) return RENDER_ERR_ARG;
    return font_lazy_init((font_id_t)id, mpk_path);
}

const lv_font_t *render_get_font(render_font_t id)
{
    return font_lazy_get((font_id_t)id);
}

int render_enter_menu(void)
{
    if (!g_inited) return RENDER_ERR_STATE;
    if (g_menu) return RENDER_OK;
    int rc = bridge_mode_menu();
    if (rc != RENDER_OK) return rc;
    g_menu = true;
    return RENDER_OK;
}

int render_exit_menu(void)
{
    if (!g_inited) return RENDER_ERR_STATE;
    if (!g_menu) return RENDER_OK;
    int rc = bridge_mode_poker();
    if (rc != RENDER_OK) return rc;
    g_menu = false;
    bind_active_layout(esp_timer_get_time(), false);  /* 重启帧时钟，防快进 */
    rc_anim_kick_blink(&g_anim, esp_timer_get_time());
    full_recompose();
    return RENDER_OK;
}

lv_display_t *render_lvgl_display(void)
{
    return bridge_display();
}

int render_bubble_show(const char *text, render_font_t font)
{
    if (!g_inited) return RENDER_ERR_STATE;
    if (g_menu) return RENDER_ERR_STATE;
    if (!text || !text[0]) { render_bubble_hide(); return RENDER_OK; }

    uint16_t *buf = psram((size_t)RC_BUBBLE_MAX_W * RC_BUBBLE_MAX_H * 2u);
    if (!buf) return RENDER_ERR_NOMEM;

    int32_t bw = 0, bh = 0;
    int rc = bridge_bubble_render(text, (int)font, buf, RC_BUBBLE_MAX_W,
                                  RC_BUBBLE_MAX_W, RC_BUBBLE_MAX_H, &bw, &bh);
    if (rc != RENDER_OK) {
        heap_caps_free(buf);
        return rc;
    }

    if (g_bub.active) mark_rect(g_bub.x, g_bub.y, g_bub.w, g_bub.h);
    if (g_bub.px) heap_caps_free(g_bub.px);
    g_bub.px = buf;
    g_bub.w = bw;
    g_bub.h = bh;
    g_bub.active = true;

    /* 锚在实体上方（实体显示区顶部居中；越界落到实体下方） */
    int32_t ex, ey, dw, dh;
    ent_screen_pos(&ex, &ey);
    ent_disp_size(&dw, &dh);
    int32_t bx = ex + dw / 2 - bw / 2;
    if (bx < 0) bx = 0;
    if (bx + bw > g_sw) bx = g_sw - bw;
    int32_t by = ey - bh - 8;
    if (by < 0) by = ey + dh + 8;
    if (by + bh > g_sh) by = g_sh - bh;
    if (by < 0) by = 0;
    g_bub.x = bx;
    g_bub.y = by;

    mark_rect(bx, by, bw, bh);
    return RENDER_OK;
}

void render_bubble_hide(void)
{
    if (!g_inited || !g_bub.active) return;
    mark_rect(g_bub.x, g_bub.y, g_bub.w, g_bub.h);
    heap_caps_free(g_bub.px);
    g_bub.px = NULL;
    g_bub.active = false;
}

/* ---------------- 未配网常驻横幅（问题4） ----------------
 * POKER 态顶部 480×28 深色底白字（5x7 内嵌字体 ×2，文本需 ASCII 大写），
 * compose_region 最顶层绘制；CLOCK_DOZE（时钟激活）态自动让位不画。 */

int render_banner_show(const char *text)
{
    if (!g_inited) return RENDER_ERR_STATE;
    if (g_banner_on) mark_rect(0, 0, g_sw, RC_BANNER_H);
    g_banner_on = true;
    g_banner_text[0] = 0;
    if (text) strlcpy(g_banner_text, text, sizeof(g_banner_text));
    mark_rect(0, 0, g_sw, RC_BANNER_H);
    return RENDER_OK;
}

void render_banner_hide(void)
{
    if (!g_inited || !g_banner_on) return;
    g_banner_on = false;
    mark_rect(0, 0, g_sw, RC_BANNER_H);
}

void render_input_tilt(float tilt_deg)
{
    if (tilt_deg > 8.0f) tilt_deg = 8.0f;
    if (tilt_deg < -8.0f) tilt_deg = -8.0f;
    g_tilt_mdeg = (int32_t)(tilt_deg * 1000.0f);   /* 对齐 32bit 原子写 */
}

/*
 * ================= PSRAM 预算（480×480，软件设计 4.2 口径） =================
 * RENDER_PSRAM_BUDGET:
 *   framebuffer            460,800 B   合成器独占（单一所有权）
 *   static_back            460,800 B   屏幕尺寸（装载时按需 2x 展开）
 *   tile_layer             460,800 B   屏幕尺寸
 *   tile_mask               28,800 B   (480*480+7)/8
 *   实体缓冲 480×440×2     422,400 B   摆放上限（画布 240×220 世界 @2x，底部留 40px）
 *   实体覆盖 1bit           26,400 B
 *   LVGL MENU 整屏         460,800 B   render_mode_menu 离屏（DIRECT）
 *   LVGL POKER 双缓冲       92,160 B   2 × 1/10 屏（46,080 B each）
 *   气泡位图 460×160×2      147,200 B   显示时分配，隐藏即释放
 *   条带图（每条）           ~70 KB    480×64×2.25B（按条带实际尺寸）
 *   部件缓存（懒加载）       ≤2 MB cap 典型 ~700KB（整装扮+25 表情变体）
 *   时钟 13 小图              ~24 KB
 *   字形缓存 3×16 槽          ~40 KB   16/24/32px 各 max_glyph×16
 *   LAYOUT/PARTS 索引        ~120 KB   头+索引常驻
 *   ------------------------------------------------------------------
 *   固定峰值（无气泡）     ≈ 2.41 MB；典型全负载（含缓存/条带）≈ 3.4-4.0 MB
 *   （8MB PSRAM，与网络/音频共享；menu 与 poker 的 LVGL 缓冲常驻不切换释放）
 */
