/*
 * tools/test_mob_pack.c — 怪物/NPC 实体包（PARTS + LAYOUT）的 host 侧离线对拍
 *
 * 背景（2026-10-02「怪物资产可用」）：服务端把 Mob.wz 导出成**与纸娃娃逐字节同构**的
 * PARTS + LAYOUT 包（selector=mob / entity="mob:<id>"），固件走同一条渲染通道
 * （render_set_parts + render_set_layout）。本工具不进程编译链，gcc 直接编译
 * main/render/mpak.c（配 tools/stubs 的 ESP 替身头），用**服务端真实导出的 .mpak**
 * 验证三件事：
 *   ① 信封/索引可解析（mpak_open：MAGIC/长度/CRC/hash/kind）；
 *   ② LAYOUT 里每个 piece 的 part_id 都能在 PARTS 里找到，且位图 + 1bit 掩码可读；
 *   ③ 按**合成器同一套摆放算术**（画布联合包围盒 + origin 贴屏心 + 2x 最近邻 + 掩码）
 *      把每个动作的每一帧真正合成出来 → 写 BMP 供人眼复核（"包能读"≠"画得对"）。
 *
 * 编译/运行（仓库根 = Firmware/board216）：
 *   gcc -std=c11 -O2 -Wall -Wextra -I tools/stubs -I main/render -I main/app \
 *       tools/test_mob_pack.c main/render/mpak.c -o /tmp/test_mob_pack
 *   （include 顺序有讲究：tools/stubs 必须排在 main/app 之前，让 mpak.c 取
 *     watchdog.h 的 host 替身；再补 -I main/app 才能找到 mp_psram.h）
 *   /tmp/test_mob_pack <PARTS.mpak> <LAYOUT.mpak> [<LAYOUT.mpak> ...]
 * 输出：/tmp/mob_<action>_f<帧>.bmp（480×480，16 位 RGB565 → 24 位 BGR，同固件截图口径）
 *
 * 摆放口径与 compositor.c 对齐（ent_canvas_update + ent_screen_pos_at）：
 *   · 画布 = 全帧全 piece 的矩形联合（min/max），超出 RC_ENT_W/H(480×440) 时
 *     横向取"以锚点为中心"的窗口、纵向取底部窗口；
 *   · origin = manifest LAYOUT 条目 extra.origin（实体包恒 (0,0)：piece x/y 已是
 *     相对锚点坐标 ⇒ 画布坐标 0 点 = 实体锚点）；
 *   · 缓冲左上角屏幕坐标 = 屏心 − (origin − 画布左上)×2；
 *   · 画布内 piece 落点 = (piece.x − cx0)*2，2×2 块展开、掩码 0 处不覆盖。
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpak.h"

unsigned g_test_watchdog_kicks;   /* watchdog.h 替身要的符号 */

#define SW 480
#define SH 480
#define SCALE 2
#define SCALE_SHIFT 1
#define ENT_W 480
#define ENT_H 440

static int g_fail, g_check;
static void ck(bool cond, const char *fmt, ...)
{
    g_check++;
    if (cond) return;
    g_fail++;
    va_list ap;
    va_start(ap, fmt);
    fputs("  x ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

/* 1bit 掩码位序（MSB first）——与 compositor.h 的 rc_mask_bit 同口径 */
static inline bool mask_bit(const uint8_t *m, uint32_t idx)
{
    return (m[idx >> 3] >> (7 - (idx & 7))) & 1u;
}

/* ── 实体缓冲（屏幕像素，内容 = 世界 1x × 2 最近邻）── */
static uint16_t g_ent[ENT_W * ENT_H];
static uint8_t  g_cov[(ENT_W * ENT_H + 7) / 8];

static void ent_reset(void)
{
    memset(g_ent, 0, sizeof g_ent);
    memset(g_cov, 0, sizeof g_cov);
}

static void blit_2x(const mpak_part_t *meta, const uint16_t *px, const uint8_t *mask,
                    int32_t bx, int32_t by)
{
    uint32_t stride_el = ((uint32_t)meta->w * 2u + 3u) / 4u / 2u;   /* align4(w*2)/2 */
    for (uint32_t sy = 0; sy < meta->h; sy++) {
        int32_t Y0 = by + (int32_t)(sy << SCALE_SHIFT);
        if (Y0 + 1 < 0 || Y0 >= ENT_H) continue;
        for (uint32_t sx = 0; sx < meta->w; sx++) {
            if (mask && !mask_bit(mask, sy * meta->w + sx)) continue;
            uint16_t c = px[(size_t)sy * stride_el + sx];
            int32_t X0 = bx + (int32_t)(sx << SCALE_SHIFT);
            for (int dy = 0; dy < SCALE; dy++) {
                int32_t Y = Y0 + dy;
                if (Y < 0 || Y >= ENT_H) continue;
                for (int dx = 0; dx < SCALE; dx++) {
                    int32_t X = X0 + dx;
                    if (X < 0 || X >= ENT_W) continue;
                    uint32_t idx = (uint32_t)Y * ENT_W + (uint32_t)X;
                    g_ent[idx] = c;
                    g_cov[idx >> 3] |= (uint8_t)(1u << (7 - (idx & 7)));
                }
            }
        }
    }
}

/* ── BMP 落盘（24 位 BGR，自下而上；同固件截图口径，仅便于人眼复核）── */
static void write_bmp(const char *path, const uint16_t *fb)
{
    const int W = SW, H = SH, row = (W * 3 + 3) & ~3;
    const uint32_t data_len = (uint32_t)row * H;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t file_len = 54 + data_len;
    memcpy(hdr + 2, &file_len, 4);
    uint32_t off = 54; memcpy(hdr + 10, &off, 4);
    uint32_t ih = 40; memcpy(hdr + 14, &ih, 4);
    memcpy(hdr + 18, &W, 4); memcpy(hdr + 22, &H, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data_len, 4);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite(hdr, 1, sizeof hdr, f);
    uint8_t *line = malloc((size_t)row);
    for (int y = H - 1; y >= 0; y--) {
        memset(line, 0, (size_t)row);
        for (int x = 0; x < W; x++) {
            uint16_t v = fb[(size_t)y * W + x];
            int r = (int)((v >> 11) & 0x1Fu) << 3;
            int g = (int)((v >> 5) & 0x3Fu) << 2;
            int b = (int)(v & 0x1Fu) << 3;
            line[x * 3 + 0] = (uint8_t)b;
            line[x * 3 + 1] = (uint8_t)g;
            line[x * 3 + 2] = (uint8_t)r;
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
    printf("    → %s\n", path);
}

/* ── 主流程：解析 PARTS，逐 LAYOUT 校验并合成 ── */
static mpak_t g_parts;

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <PARTS.mpak> <LAYOUT.mpak> [<LAYOUT.mpak> ...]\n", argv[0]);
        return 2;
    }

    printf("== PARTS: %s\n", argv[1]);
    int rc = mpak_open(&g_parts, argv[1], 0, MPAK_KIND_PARTS);
    ck(rc == MPAK_OK, "PARTS mpak_open rc=%d（信封/CRC/kind 校验）", rc);
    if (rc != MPAK_OK) { printf("FAIL %d/%d\n", g_fail, g_check); return 1; }
    printf("    parts=%u content_hash=%016" PRIx64 "\n",
           g_parts.parts_count, mpak_content_hash(&g_parts));
    ck(g_parts.parts_count > 0, "PARTS 非空（parts=%u）", g_parts.parts_count);

    /* 每个部件都要能读像素；掩码存在时必须能读掩码（实体件全是带 alpha 的 PNG） */
    uint32_t decoded = 0, with_mask = 0, bad = 0;
    for (uint32_t i = 0; i < g_parts.parts_count; i++) {
        const mpak_part_t *p = &g_parts.parts_tab[i];
        size_t need = (size_t)((p->w * 2u + 3u) & ~3u) * p->h;
        uint8_t *buf = malloc(need + 16);
        if (mpak_part_read_pixels(&g_parts, p, buf, need) != MPAK_OK) { bad++; free(buf); continue; }
        decoded++;
        if (p->has_alpha) {
            size_t mneed = ((size_t)p->w * p->h + 7u) / 8u;
            uint8_t *mb = malloc(mneed + 16);
            if (mpak_part_read_mask(&g_parts, p, mb, mneed) == MPAK_OK) with_mask++;
            else bad++;
            free(mb);
        }
        free(buf);
    }
    ck(bad == 0, "全部部件位图/掩码可读（成功 %u，失败 %u）", decoded, bad);
    printf("    位图解码 %u/%u，含 alpha 掩码 %u\n", decoded, g_parts.parts_count, with_mask);

    int total_frames = 0, total_pieces = 0, total_rendered = 0;

    for (int a = 2; a < argc; a++) {
        mpak_t lt;
        rc = mpak_open(&lt, argv[a], 0, MPAK_KIND_LAYOUT);
        ck(rc == MPAK_OK, "LAYOUT mpak_open(%s) rc=%d", argv[a], rc);
        if (rc != MPAK_OK) continue;
        const mpak_layout_t *L = lt.u.layout;
        ck(L && L->frame_count > 0, "LAYOUT '%s' 帧数 %u", L ? L->action : "?",
           L ? L->frame_count : 0u);
        if (!L || L->frame_count == 0) { mpak_close(&lt); continue; }
        printf("    LAYOUT %s: frames=%u pieces=%u exprs=%u\n",
               L->action, L->frame_count, L->pieces_total, L->expression_count);
        total_frames += (int)L->frame_count;

        /* 画布联合包围盒（compositor.ent_canvas_update 同式） */
        int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
        bool any = false;
        for (uint32_t f = 0; f < L->frame_count; f++) {
            const mpak_frame_t *fr = &L->frames[f];
            for (uint32_t k = 0; k < fr->piece_count; k++) {
                const mpak_piece_t *pc = &L->pieces[fr->piece_off + k];
                const mpak_part_t *m = mpak_parts_find(&g_parts, pc->part_id);
                ck(m != NULL, "piece part_id=%u 在 PARTS 中存在（%s 帧 %u）",
                   pc->part_id, L->action, f);
                if (!m) continue;
                int32_t x0 = pc->x, y0 = pc->y, x1 = x0 + (int32_t)m->w, y1 = y0 + (int32_t)m->h;
                if (!any) { minX = x0; minY = y0; maxX = x1; maxY = y1; any = true; }
                else {
                    if (x0 < minX) minX = x0;
                    if (y0 < minY) minY = y0;
                    if (x1 > maxX) maxX = x1;
                    if (y1 > maxY) maxY = y1;
                }
                total_pieces++;
            }
        }
        if (!any) { mpak_close(&lt); continue; }
        int32_t cw = maxX - minX, ch = maxY - minY, cx0 = minX, cy0 = minY;
        int32_t max_w = ENT_W / SCALE, max_h = ENT_H / SCALE;
        if (cw > max_w) { cw = max_w; cx0 = -max_w / 2; }
        if (ch > max_h) { ch = max_h; cy0 = maxY - max_h; }
        int16_t ox = 0, oy = 0;   /* 实体包 origin 恒 (0,0)：锚点在画布坐标 0 点 */
        int32_t bx = SW / 2 - (ox - cx0) * SCALE;
        int32_t by = SH / 2 - (oy - cy0) * SCALE;
        printf("    画布=(%d,%d %dx%d) 缓冲落点=(%d,%d)\n",
               (int)cx0, (int)cy0, (int)cw, (int)ch, (int)bx, (int)by);

        for (uint32_t f = 0; f < L->frame_count; f++) {
            const mpak_frame_t *fr = &L->frames[f];
            ent_reset();
            for (uint32_t k = 0; k < fr->piece_count; k++) {
                const mpak_piece_t *pc = &L->pieces[fr->piece_off + k];
                const mpak_part_t *m = mpak_parts_find(&g_parts, pc->part_id);
                if (!m) continue;
                size_t need = (size_t)((m->w * 2u + 3u) & ~3u) * m->h;
                uint8_t *buf = calloc(1, need + 16);
                uint8_t *mb = NULL;
                if (mpak_part_read_pixels(&g_parts, m, buf, need) == MPAK_OK) {
                    if (m->has_alpha) {
                        size_t mneed = ((size_t)m->w * m->h + 7u) / 8u;
                        mb = calloc(1, mneed + 16);
                        if (mpak_part_read_mask(&g_parts, m, mb, mneed) != MPAK_OK) {
                            free(mb); mb = NULL;
                        }
                    }
                    blit_2x(m, (const uint16_t *)buf, mb,
                            ((int32_t)pc->x - cx0) << SCALE_SHIFT,
                            ((int32_t)pc->y - cy0) << SCALE_SHIFT);
                }
                free(mb);
                free(buf);
            }
            /* 合成到 480×480 画布（黑底），检查非空覆盖率 */
            static uint16_t fb[SW * SH];
            memset(fb, 0, sizeof fb);
            uint32_t cover = 0;
            for (int y = 0; y < SH; y++) {
                for (int x = 0; x < SW; x++) {
                    int32_t ex = x - bx, ey = y - by;
                    if (ex < 0 || ey < 0 || ex >= ENT_W || ey >= ENT_H) continue;
                    uint32_t idx = (uint32_t)ey * ENT_W + (uint32_t)ex;
                    if (!((g_cov[idx >> 3] >> (7 - (idx & 7))) & 1u)) continue;
                    fb[(size_t)y * SW + x] = g_ent[idx];
                    cover++;
                }
            }
            ck(cover > 0, "%s 帧 %u 合成非空（覆盖 %u px）", L->action, f, cover);
            if (cover > 0) total_rendered++;
            char path[256];
            snprintf(path, sizeof path, "/tmp/mob_%s_f%u.bmp", L->action, f);
            write_bmp(path, fb);
        }
        mpak_close(&lt);
    }

    /* 默认动作（stand/move）必须存在：固件 idle 就靠它循环 */
    bool has_default = false;
    for (int a = 2; a < argc; a++) {
        mpak_t lt;
        if (mpak_open(&lt, argv[a], 0, MPAK_KIND_LAYOUT) == MPAK_OK) {
            const char *act = lt.u.layout ? lt.u.layout->action : "";
            if (strcmp(act, "stand") == 0 || strcmp(act, "move") == 0) has_default = true;
            mpak_close(&lt);
        }
    }
    ck(has_default, "存在默认动作包（stand/move）");

    mpak_close(&g_parts);
    printf("\n结果：%d 项断言，失败 %d；LAYOUT 帧 %d / piece %d / 实际合成 %d 帧\n",
           g_check, g_fail, total_frames, total_pieces, total_rendered);
    printf("%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
