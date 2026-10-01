/*
 * tools/test_tiled_bgmap.c — 整图 BGMAP「瓦片(tile)布局」读取路径的 host 侧离线对拍
 *
 * 依据契约 docs/ai/map-tiled-format-contract.md（§2 几何 / §5 布局 / §6 读取口径）。
 * 这里**不进程编译链**：gcc 直接编译 main/render/mpak.c（配 tools/stubs 下的 ESP 替身头），
 * 用构造出来的假包验证三件事：
 *   ① 网格/偏移：按契约 §6 的公式**自己算文件字节偏移**直读文件，与固件读出的像素
 *      逐点对比（校验的是真实盘上布局，不是"自己跟自己一致"）；
 *   ② 等价性：同一个逻辑图分别按「瓦片」和「逐行」两种布局落盘，288×288 世界窗口
 *      读出**逐像素一致**（像素 + tile 掩码），越界补 0 行为也一致；
 *   ③ 统计：整窗填充 / 拖动补一列各读了几块、命中几块、多少 KB（判据口径）。
 *
 * 编译/运行（仓库根 = Firmware/board216）：
 *   gcc -std=c11 -O2 -Wall -Wextra -I main/render -I tools/stubs \
 *       tools/test_tiled_bgmap.c main/render/mpak.c -o /tmp/test_tiled_bgmap
 *   /tmp/test_tiled_bgmap
 */
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mpak.h"

unsigned g_test_watchdog_kicks;

/* ───────────────────────── 断言 ───────────────────────── */
static int g_fail, g_check;
static void ck(bool cond, const char *fmt, ...)
{
    g_check++;
    if (cond) return;
    g_fail++;
    va_list ap;
    va_start(ap, fmt);
    fputs("  ✗ ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

/* ───────────────────────── 逻辑图模型（唯一真值） ─────────────────────────
 * 尺寸故意选"非 128 整数倍 + 非偶数宽"：奇数宽能同时压到行 4B 对齐与瓦片补 0
 * 两条边角路径（偶数宽下两式恰好等值，压不出分叉）。 */
#define MVW 517
#define MVH 390

static uint16_t model_static(int32_t x, int32_t y)
{
    return (uint16_t)(0x1000u + (uint32_t)((x * 7 + y * 13) & 0x0FFFu));
}
static uint16_t model_tile(int32_t x, int32_t y)
{
    return (uint16_t)(model_static(x, y) ^ ((x & 1) ? 0x0F0Fu : 0x00F0u));
}
static uint8_t model_mask(int32_t x, int32_t y)
{
    return (uint8_t)(((x * 3 + y * 5) % 7) < 3);
}
static uint16_t model_ground(int32_t x)
{
    if ((x % 7) == 0) return MPAK_BGMAP_GROUND_NONE;
    return (uint16_t)(120 + (x % 50));
}

/* ───────────────────────── 可增长写缓冲 ───────────────────────── */
static uint8_t *g_b;
static size_t   g_n, g_cap;

static void w_need(size_t n)
{
    if (g_n + n <= g_cap) return;
    g_cap = (g_n + n) * 2u + 4096u;
    g_b = realloc(g_b, g_cap);
    if (!g_b) { fputs("OOM\n", stderr); exit(2); }
}
static void w_u8(uint8_t v)   { w_need(1); g_b[g_n++] = v; }
static void w_u16(uint16_t v) { w_need(2); g_b[g_n++] = (uint8_t)(v & 0xFF); g_b[g_n++] = (uint8_t)(v >> 8); }
static void w_u32(uint32_t v) { w_need(4); for (int i = 0; i < 4; i++) g_b[g_n++] = (uint8_t)(v >> (8 * i)); }
static void w_bytes(const void *p, size_t n) { w_need(n); memcpy(g_b + g_n, p, n); g_n += n; }

static void w_reset(void) { g_n = 0; }

/* 写一个 MPAK 信封（40B 头 + payload + 8B 尾），CRC32C 走 mpak 自己的实现 */
static void write_mpak_kind(const char *path, const uint8_t *payload, uint32_t payload_len,
                            uint64_t kind)
{
    uint8_t hdr[MPAK_HEADER_LEN];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "MPAK", 4);
    uint16_t ver = MPAK_VERSION;
    memcpy(hdr + 4, &ver, 2);
    memcpy(hdr + 16, &kind, 8);
    uint64_t hash = 0x0123456789ABCDEFull;
    memcpy(hdr + 24, &hash, 8);
    memcpy(hdr + 32, &payload_len, 4);

    uint32_t crc = mpak_crc32c(0, hdr, sizeof hdr);
    crc = mpak_crc32c(crc, payload, payload_len);

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(hdr, 1, sizeof hdr, f);
    fwrite(payload, 1, payload_len, f);
    uint8_t tr[MPAK_TRAILER_LEN];
    memset(tr, 0, sizeof tr);
    memcpy(tr, &crc, 4);
    fwrite(tr, 1, sizeof tr, f);
    fclose(f);
}
static void write_mpak(const char *path, const uint8_t *payload, uint32_t payload_len)
{
    write_mpak_kind(path, payload, payload_len, MPAK_KIND_BGMAP);
}

static uint32_t align4u(uint32_t n) { return (n + 3u) & ~3u; }

/* ───────── 造包 A：瓦片布局（契约 §2/§3/§5）───────── */
static uint8_t *build_tiled(uint32_t *len_out, int32_t T, int32_t vw, int32_t vh)
{
    const int32_t gx = (vw + T - 1) / T, gy = (vh + T - 1) / T;
    const uint32_t px_len   = (uint32_t)gx * gy * (uint32_t)T * (uint32_t)T * 2u;
    const uint32_t mask_len = (uint32_t)gx * gy * ((uint32_t)T * (uint32_t)T / 8u);
    const uint32_t static_off = 56u;
    const uint32_t tile_off   = static_off + px_len;
    const uint32_t ext_off    = align4u(tile_off + px_len + mask_len);

    w_reset();
    /* ── BGMAP payload 头 56B ── */
    char mapid[MPAK_NAME_LEN];
    memset(mapid, 0, sizeof mapid);
    snprintf(mapid, sizeof mapid, "TILED%d", (int)T);
    w_bytes(mapid, MPAK_NAME_LEN);
    w_u16((uint16_t)vw);
    w_u16((uint16_t)vh);
    w_u32(px_len);                    /* static_back_len = gx*gy*T*T*2 */
    w_u32(static_off);
    w_u32(px_len + mask_len);         /* tile_layer_len = 像素 + 掩码 */
    w_u32(tile_off);
    w_u32(0);                         /* strip_count */
    /* ── static 像素区（每块 T*T*2，右/下补 0）── */
    for (int32_t ty = 0; ty < gy; ty++)
        for (int32_t tx = 0; tx < gx; tx++)
            for (int32_t y = 0; y < T; y++)
                for (int32_t x = 0; x < T; x++) {
                    int32_t wx = tx * T + x, wy = ty * T + y;
                    w_u16((wx < vw && wy < vh) ? model_static(wx, wy) : 0);
                }
    /* ── tile 像素区 ── */
    for (int32_t ty = 0; ty < gy; ty++)
        for (int32_t tx = 0; tx < gx; tx++)
            for (int32_t y = 0; y < T; y++)
                for (int32_t x = 0; x < T; x++) {
                    int32_t wx = tx * T + x, wy = ty * T + y;
                    w_u16((wx < vw && wy < vh) ? model_tile(wx, wy) : 0);
                }
    /* ── tile 掩码区（块内 y*T/8 起 T/8 字节/行，MSB first）── */
    for (int32_t ty = 0; ty < gy; ty++)
        for (int32_t tx = 0; tx < gx; tx++)
            for (int32_t y = 0; y < T; y++)
                for (int32_t xb = 0; xb < T / 8; xb++) {
                    uint8_t v = 0;
                    for (int32_t k = 0; k < 8; k++) {
                        int32_t wx = tx * T + xb * 8 + k, wy = ty * T + y;
                        bool on = (wx < vw && wy < vh) && model_mask(wx, wy);
                        if (on) v |= (uint8_t)(1u << (7 - k));
                    }
                    w_u8(v);
                }
    /* ── 尾扩展块（bit0 整图 + bit1 TILED）+ 地面表 ── */
    while (g_n < ext_off) w_u8(0);
    uint32_t gnd_off = ext_off + MPAK_BGMAP_EXT_HDR_LEN;
    w_u32(MPAK_BGMAP_EXT_MAGIC);
    w_u32((uint32_t)vw * 2u);
    w_u32(gnd_off);
    w_u32(MPAK_BGMAP_FLAG_FULL_MAP | MPAK_BGMAP_FLAG_TILED);
    for (int32_t x = 0; x < vw; x++) w_u16(model_ground(x));

    *len_out = (uint32_t)g_n;
    uint8_t *p = malloc(g_n);
    memcpy(p, g_b, g_n);
    return p;
}

/* ───────── 造包 B：逐行布局（整图口径：行 4B 对齐 + 全局 tight 掩码）───────── */
static uint8_t *build_row(uint32_t *len_out, int32_t vw, int32_t vh)
{
    const uint32_t stride    = align4u((uint32_t)vw * 2u);
    const uint32_t px_len    = (uint32_t)vh * stride;
    const uint32_t mask_len  = align4u(((uint32_t)vw * (uint32_t)vh + 7u) / 8u);
    const uint32_t static_off = 56u;
    const uint32_t tile_off   = static_off + px_len;
    const uint32_t ext_off    = align4u(tile_off + px_len + mask_len);

    w_reset();
    char mapid[MPAK_NAME_LEN];
    memset(mapid, 0, sizeof mapid);
    snprintf(mapid, sizeof mapid, "ROW");
    w_bytes(mapid, MPAK_NAME_LEN);
    w_u16((uint16_t)vw);
    w_u16((uint16_t)vh);
    w_u32(px_len);
    w_u32(static_off);
    w_u32(px_len + mask_len);
    w_u32(tile_off);
    w_u32(0);
    for (int32_t y = 0; y < vh; y++) {
        for (int32_t x = 0; x < vw; x++) w_u16(model_static(x, y));
        for (uint32_t k = (uint32_t)vw * 2u; k < stride; k++) w_u8(0);   /* 行 4B 对齐填充 */
    }
    for (int32_t y = 0; y < vh; y++) {
        for (int32_t x = 0; x < vw; x++) w_u16(model_tile(x, y));
        for (uint32_t k = (uint32_t)vw * 2u; k < stride; k++) w_u8(0);
    }
    /* 掩码：全局 tight 位流（bit = y*vw + x，MSB first，行间不补位） */
    {
        uint32_t bits = (uint32_t)vw * (uint32_t)vh;
        uint8_t cur = 0;
        for (uint32_t i = 0; i < bits; i++) {
            int32_t x = (int32_t)(i % (uint32_t)vw), y = (int32_t)(i / (uint32_t)vw);
            if (model_mask(x, y)) cur |= (uint8_t)(1u << (7 - (i & 7u)));
            if ((i & 7u) == 7u) { w_u8(cur); cur = 0; }
        }
        if (bits & 7u) w_u8(cur);
        while ((g_n - 0) % 4u) w_u8(0);      /* align4(mask) */
    }
    while (g_n < ext_off) w_u8(0);
    uint32_t gnd_off = ext_off + MPAK_BGMAP_EXT_HDR_LEN;
    w_u32(MPAK_BGMAP_EXT_MAGIC);
    w_u32((uint32_t)vw * 2u);
    w_u32(gnd_off);
    w_u32(MPAK_BGMAP_FLAG_FULL_MAP);
    for (int32_t x = 0; x < vw; x++) w_u16(model_ground(x));

    *len_out = (uint32_t)g_n;
    uint8_t *p = malloc(g_n);
    memcpy(p, g_b, g_n);
    return p;
}


/* ───────── 造包 C：瓦片条带（PARTS 包，part 0 = 带图，契约 §4）─────────
 * 条带像素在独立小包里，BGMAP 的 bit1 管不到它 ⇒ 包自身必须能自证瓦片布局
 * （长度 == gx*gy*T*T*2 [+ gx*gy*(T*T/8)]）。这里分别造"不透明"与"带掩码"两版。 */
static uint8_t *build_strip_tiled(uint32_t *len_out, int32_t T, int32_t w, int32_t h, bool alpha)
{
    const int32_t gx = (w + T - 1) / T, gy = (h + T - 1) / T;
    const uint32_t px_len = (uint32_t)gx * gy * (uint32_t)T * (uint32_t)T * 2u;
    const uint32_t mk_len = alpha ? (uint32_t)gx * gy * ((uint32_t)T * (uint32_t)T / 8u) : 0u;

    w_reset();
    w_u32(1);                                   /* part 数量 */
    w_u32(1001);                                /* part_id */
    w_u16(0);                                   /* expr_group */
    w_u16((uint16_t)w);
    w_u16((uint16_t)h);
    w_u16(0); w_u16(0);                         /* origin */
    w_u32(0);                                   /* offset（位图区相对） */
    w_u16(0);                                   /* 20B 尾填充 */
    for (int32_t ty = 0; ty < gy; ty++)
        for (int32_t tx = 0; tx < gx; tx++)
            for (int32_t y = 0; y < T; y++)
                for (int32_t x = 0; x < T; x++) {
                    int32_t wx = tx * T + x, wy = ty * T + y;
                    w_u16((wx < w && wy < h) ? model_static(wx, wy) : 0);
                }
    if (alpha) {
        for (int32_t ty = 0; ty < gy; ty++)
            for (int32_t tx = 0; tx < gx; tx++)
                for (int32_t y = 0; y < T; y++)
                    for (int32_t xb = 0; xb < T / 8; xb++) {
                        uint8_t v = 0;
                        for (int32_t k = 0; k < 8; k++) {
                            int32_t wx = tx * T + xb * 8 + k, wy = ty * T + y;
                            if ((wx < w && wy < h) && model_mask(wx, wy)) v |= (uint8_t)(1u << (7 - k));
                        }
                        w_u8(v);
                    }
    }
    *len_out = (uint32_t)g_n;
    uint8_t *p = malloc(g_n);
    memcpy(p, g_b, g_n);
    (void)px_len; (void)mk_len;
    return p;
}

/* ───────── 造包 D：逐行条带（旧口径，同一张图）───────── */
static uint8_t *build_strip_row(uint32_t *len_out, int32_t w, int32_t h, bool alpha)
{
    const uint32_t stride = align4u((uint32_t)w * 2u);
    const uint32_t px_len = (uint32_t)h * stride;
    const uint32_t mk_len = alpha ? align4u(((uint32_t)w * (uint32_t)h + 7u) / 8u) : 0u;
    w_reset();
    w_u32(1);
    w_u32(1001);
    w_u16(0);
    w_u16((uint16_t)w);
    w_u16((uint16_t)h);
    w_u16(0); w_u16(0);
    w_u32(0);
    w_u16(0);
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) w_u16(model_static(x, y));
        for (uint32_t k = (uint32_t)w * 2u; k < stride; k++) w_u8(0);
    }
    if (alpha) {
        uint32_t bits = (uint32_t)w * (uint32_t)h;
        uint8_t cur = 0;
        for (uint32_t i = 0; i < bits; i++) {
            int32_t x = (int32_t)(i % (uint32_t)w), y = (int32_t)(i / (uint32_t)w);
            if (model_mask(x, y)) cur |= (uint8_t)(1u << (7 - (i & 7u)));
            if ((i & 7u) == 7u) { w_u8(cur); cur = 0; }
        }
        if (bits & 7u) w_u8(cur);
        while ((g_n - 0) % 4u) w_u8(0);
    }
    *len_out = (uint32_t)g_n;
    uint8_t *p = malloc(g_n);
    memcpy(p, g_b, g_n);
    (void)px_len; (void)mk_len;
    return p;
}

/* ───────── 契约 §6 的偏移算法（测试自己实现一份，用来直读文件字节）───────── */
static uint32_t contract_px_byte_off(uint32_t layer_off, int32_t T, int32_t gx, int32_t x, int32_t y)
{
    int32_t tx = x / T, ty = y / T, in_x = x % T, in_y = y % T;
    return layer_off + (uint32_t)(ty * gx + tx) * (uint32_t)T * (uint32_t)T * 2u
           + (uint32_t)(in_y * T + in_x) * 2u;
}
static uint32_t contract_mask_byte_off(uint32_t cov_off, int32_t T, int32_t gx, int32_t x, int32_t y)
{
    int32_t tx = x / T, ty = y / T, in_x = x % T, in_y = y % T;
    return cov_off + (uint32_t)(ty * gx + tx) * ((uint32_t)T * (uint32_t)T / 8u)
           + (uint32_t)in_y * ((uint32_t)T / 8u) + (uint32_t)(in_x >> 3);
}
static int32_t contract_mask_bit(int32_t T, int32_t x) { return 7 - (x % T) % 8; }

int main(void)
{
    const char *path_t = "/tmp/minipet_test_tiled.mpk";
    const char *path_r = "/tmp/minipet_test_row.mpk";
    const char *path_t64 = "/tmp/minipet_test_tiled64.mpk";
    const int32_t T = MPAK_BGMAP_TILE_DEFAULT;
    const int32_t gx = (MVW + T - 1) / T, gy = (MVH + T - 1) / T;

    const char *path_st = "/tmp/minipet_test_strip_tiled.mpk";
    const char *path_sr = "/tmp/minipet_test_strip_row.mpk";
    const char *path_stn = "/tmp/minipet_test_strip_tiled_nomask.mpk";
    uint32_t lst = 0, lsr = 0, lstn = 0;
    uint8_t *pst = build_strip_tiled(&lst, T, 300, 200, true);
    uint8_t *psr = build_strip_row(&lsr, 300, 200, true);
    uint8_t *pstn = build_strip_tiled(&lstn, T, 300, 200, false);
    write_mpak_kind(path_st, pst, lst, MPAK_KIND_PARTS);
    write_mpak_kind(path_sr, psr, lsr, MPAK_KIND_PARTS);
    write_mpak_kind(path_stn, pstn, lstn, MPAK_KIND_PARTS);

    uint32_t lt = 0, lr = 0, l64 = 0;
    uint8_t *pt = build_tiled(&lt, T, MVW, MVH);
    uint8_t *pr = build_row(&lr, MVW, MVH);
    uint8_t *p64 = build_tiled(&l64, 64, 300, 200);
    write_mpak(path_t, pt, lt);
    write_mpak(path_r, pr, lr);
    write_mpak(path_t64, p64, l64);
    printf("假包：瓦片 %s（payload %u B）· 逐行 %s（payload %u B）· 瓦片64 %s（payload %u B）\n"
           "     逻辑图 %dx%d，tile=%d ⇒ 网格 %dx%d（%d 块）\n",
           path_t, lt, path_r, lr, path_t64, l64, MVW, MVH, T, gx, gy, gx * gy);

    /* ══ 打开两个包（走固件同一条解析路径）══ */
    mpak_t mt, mr, m64;
    ck(mpak_open(&mt, path_t, 0, MPAK_KIND_BGMAP) == MPAK_OK, "瓦片包 open 失败");
    ck(mpak_open(&mr, path_r, 0, MPAK_KIND_BGMAP) == MPAK_OK, "逐行包 open 失败");
    ck(mpak_open(&m64, path_t64, 0, MPAK_KIND_BGMAP) == MPAK_OK, "瓦片64 包 open 失败");
    const mpak_bgmap_t *bt = mt.u.bgmap, *br = mr.u.bgmap, *b64 = m64.u.bgmap;
    if (!bt || !br || !b64) { fputs("解析结果为空\n", stderr); return 2; }

    /* ── ① 解析：tiled 标志 / tile 边长 / 网格（**从包里读，不硬编码**）── */
    puts("\n① 解析与网格");
    ck(bt->tiled && !br->tiled, "tiled 标志判定错：瓦片=%d 逐行=%d", (int)bt->tiled, (int)br->tiled);
    ck(bt->tile == T && bt->gx == gx && bt->gy == gy,
       "瓦片包网格错：tile=%d gx=%d gy=%d（期望 %d/%d/%d）", bt->tile, bt->gx, bt->gy, T, gx, gy);
    ck(bt->tile_px_bytes == (uint32_t)gx * gy * T * T * 2u,
       "瓦片像素区长度错：%u", bt->tile_px_bytes);
    ck(bt->tile_mask_off == bt->tile_layer_off + bt->tile_px_bytes,
       "瓦片掩码偏移错：%u", bt->tile_mask_off);
    ck(b64->tiled && b64->tile == 64 && b64->gx == 5 && b64->gy == 4,
       "tile=64 包未被识别（tiled=%d tile=%d 网格 %dx%d）——边长必须从包内长度反解",
       (int)b64->tiled, b64->tile, b64->gx, b64->gy);
    ck(br->tile == 0 && br->gx == 0, "逐行包不该带瓦片字段（tile=%d gx=%d）", br->tile, br->gx);
    ck(bt->full_map && br->full_map, "full_map 标志判定错");
    printf("   瓦片包：tile=%d 网格 %dx%d 像素区 %u B/层 掩码 %u B@%u flags=0x%08" PRIx32 "\n",
           bt->tile, bt->gx, bt->gy, bt->tile_px_bytes, bt->tile_mask_bytes, bt->tile_mask_off,
           bt->ext_flags);
    printf("   逐行包：tile=%d（=0 表示走原逐行路径）static=%u B\n", br->tile, br->static_back_len);

    /* ── ② 契约 §6 偏移：按公式自己算盘上字节偏移，直读文件比对 ── */
    puts("\n② 契约 §6 偏移算法（直读文件字节 vs 固件读出）");
    {
        int fd = open(path_t, O_RDONLY);
        ck(fd >= 0, "打开瓦片包失败");
        int bad = 0, n = 0;
        for (int32_t y = 0; y < MVH && bad < 3; y += 17)
            for (int32_t x = 0; x < MVW && bad < 3; x += 23) {
                uint16_t want = 0;
                uint32_t off = (uint32_t)mt.payload_off +
                               contract_px_byte_off(bt->static_back_off, T, gx, x, y);
                if (pread(fd, &want, 2, (off_t)off) != 2) { bad++; break; }
                uint8_t mb = 0;
                uint32_t moff = (uint32_t)mt.payload_off +
                                contract_mask_byte_off(bt->tile_mask_off, T, gx, x, y);
                if (pread(fd, &mb, 1, (off_t)moff) != 1) { bad++; break; }
                uint8_t wantbit = (uint8_t)((mb >> contract_mask_bit(T, x)) & 1u);

                uint16_t gpx = 0;
                uint8_t gmk = 0xFF;
                if (mpak_bgmap_read_static_rect(&mt, x, y, 1, 1, &gpx, 1) != MPAK_OK ||
                    mpak_bgmap_read_tile_mask_rect(&mt, x, y, 1, 1, &gmk, 1) != MPAK_OK) { bad++; break; }
                n++;
                if (gpx != want) {
                    printf("   ✗ (%d,%d) 文件字节 %04x ≠ 固件 %04x（off=%u）\n", x, y, want, gpx, off);
                    bad++;
                }
                if (wantbit != gmk) {
                    printf("   ✗ (%d,%d) 掩码位 文件 %u ≠ 固件 %u（off=%u）\n", x, y, wantbit, gmk, moff);
                    bad++;
                }
            }
        close(fd);
        ck(bad == 0, "契约 §6 偏移比对失败 %d 处", bad);
        printf("   抽样 %d 点：盘上字节 == 固件读出（static 像素 + tile 掩码）\n", n);
    }

    /* ── ③ 288×288 窗口：瓦片包 vs 逐行包 逐像素/逐位一致 ── */
    puts("\n③ 288×288 窗口等价性（瓦片 vs 逐行，逐像素逐位）");
    {
        const int32_t W = 288, H = 288;
        const int32_t xs[] = { 0, 137, MVW - W, -40, MVW - 100 };   /* 含越界窗口 */
        const int32_t ys[] = { 0, 89, MVH - H, -30, MVH - 60 };
        uint16_t *dt = malloc((size_t)W * H * 2u);
        uint16_t *dr = malloc((size_t)W * H * 2u);
        uint8_t  *kt = malloc((size_t)W * H);
        uint8_t  *kr = malloc((size_t)W * H);
        int bad = 0;
        for (int w = 0; w < 5; w++) {
            int32_t x0 = xs[w], y0 = ys[w];
            memset(dt, 0xAA, (size_t)W * H * 2u);
            memset(dr, 0x55, (size_t)W * H * 2u);
            memset(kt, 0xAA, (size_t)W * H);
            memset(kr, 0x55, (size_t)W * H);
            int rc1 = mpak_bgmap_read_static_rect(&mt, x0, y0, W, H, dt, W);
            int rc2 = mpak_bgmap_read_static_rect(&mr, x0, y0, W, H, dr, W);
            int rc3 = mpak_bgmap_read_tile_rect(&mt, x0, y0, W, H, dt, W);
            int rc4 = mpak_bgmap_read_tile_rect(&mr, x0, y0, W, H, dr, W);
            ck(rc1 == MPAK_OK && rc2 == MPAK_OK && rc3 == MPAK_OK && rc4 == MPAK_OK,
               "窗口 (%d,%d) 读失败 rc=%d/%d/%d/%d", x0, y0, rc1, rc2, rc3, rc4);
            if (memcmp(dt, dr, (size_t)W * H * 2u) != 0) {
                for (int i = 0; i < W * H; i++)
                    if (dt[i] != dr[i]) {
                        printf("   ✗ 窗口(%d,%d) tile 层第一处不同：第 %d 像素 瓦片=%04x 逐行=%04x\n",
                               x0, y0, i, dt[i], dr[i]);
                        break;
                    }
                bad++;
            }
            mpak_bgmap_read_tile_mask_rect(&mt, x0, y0, W, H, kt, W);
            mpak_bgmap_read_tile_mask_rect(&mr, x0, y0, W, H, kr, W);
            if (memcmp(kt, kr, (size_t)W * H) != 0) {
                for (int i = 0; i < W * H; i++)
                    if (kt[i] != kr[i]) {
                        printf("   ✗ 窗口(%d,%d) 掩码第一处不同：第 %d 位 瓦片=%u 逐行=%u\n",
                               x0, y0, i, kt[i], kr[i]);
                        break;
                    }
                bad++;
            }
            printf("   窗口 (%4d,%4d) %dx%d：像素/掩码 %s\n", x0, y0, W, H, "一致 ✓");
        }
        ck(bad == 0, "窗口等价性失败 %d 个", bad);

        /* static 层也顺带比一遍（同一口径的另一个分流点） */
        memset(dt, 0, (size_t)W * H * 2u);
        memset(dr, 0, (size_t)W * H * 2u);
        mpak_bgmap_read_static_rect(&mt, 137, 89, W, H, dt, W);
        mpak_bgmap_read_static_rect(&mr, 137, 89, W, H, dr, W);
        ck(memcmp(dt, dr, (size_t)W * H * 2u) == 0, "static 层 288×288 窗口不一致");
        printf("   static 层 288×288 窗口：一致 ✓\n");
        free(dt); free(dr); free(kt); free(kr);
    }

    /* ── ④ 统计：整窗填充（32 行一带，compositor 的真实调用形态）与拖动补一列 ── */
    puts("\n④ 读块统计（口径：瓦片读 N 块 / 命中 M 块 / N KB）");
    {
        const int32_t W = 288, H = 288;
        uint16_t *dst = malloc((size_t)W * H * 2u);
        uint8_t  *kdst = malloc((size_t)W * H);
        uint32_t b0, h0, by0, b1, h1, by1, b2, h2, by2;

        /* 整窗填充：与 wc_fill_cache 同形态（每带 32 行，先像素后掩码） */
        mpak_tile_cache_flush();
        mpak_tile_stat_reset();
        for (int32_t y = 0; y < H; y += 32) {
            int32_t rows = (y + 32 <= H) ? 32 : H - y;
            mpak_bgmap_read_static_rect(&mt, 137, 89 + y, W, rows, dst + (size_t)y * W, W);
            for (int32_t r = 0; r < rows; r++)
                mpak_bgmap_read_tile_mask_rect(&mt, 137, 89 + y + r, W, 1, kdst, W);
        }
        mpak_tile_stat_get(&b0, &h0, &by0);
        printf("   ① static 整窗 %dx%d（32 行/带）：瓦片读 %u 块 / 命中 %u 块 / %u KB\n",
               W, H, b0, h0, by0 / 1024u);
        ck(b0 > 0 && by0 >= b0 * ((uint32_t)T * (uint32_t)T / 8u) &&
                   by0 <= b0 * (uint32_t)(T * T * 2u),
           "整窗读字节数与块数不成整块关系：%u 块 %u B（像素块 32KB / 掩码块 2KB）", b0, by0);
        ck(b0 <= (uint32_t)(gx * gy), "整窗读块数 %u > 总块数 %d（读了窗口外的块）", b0, gx * gy);

        /* 拖动补一列：窗口右缘新露出的 24 列（x=425..448，落在第 3 块瓦片列 384..511） */
        mpak_tile_stat_reset();
        for (int32_t y = 0; y < H; y += 32) {
            int32_t rows = (y + 32 <= H) ? 32 : H - y;
            mpak_bgmap_read_static_rect(&mt, 425, 89 + y, 24, rows, dst, 24);
        }
        mpak_tile_stat_get(&b1, &h1, &by1);
        printf("   ② 拖动补一列（跨块，24×288）：瓦片读 %u 块 / 命中 %u 块 / %u KB\n",
               b1, h1, by1 / 1024u);
        /* 同一列再补一次（相机每 24px 动一次，多数步不跨块）⇒ 应全命中 */
        mpak_tile_stat_reset();
        for (int32_t y = 0; y < H; y += 32) {
            int32_t rows = (y + 32 <= H) ? 32 : H - y;
            mpak_bgmap_read_static_rect(&mt, 425, 89 + y, 24, rows, dst, 24);
        }
        mpak_tile_stat_get(&b2, &h2, &by2);
        printf("   ③ 同列再补一次（未跨块）：瓦片读 %u 块 / 命中 %u 块 / %u KB\n",
               b2, h2, by2 / 1024u);
        ck(b2 == 0, "同一列重复补边仍读了 %u 块（缓存没起作用）", b2);
        ck(h2 > 0, "同一列重复补边没有命中（缓存没起作用）");
        ck(b1 * (uint32_t)(T * T * 2u) <= 4u * (uint32_t)(T * T * 2u),
           "补一列读了 %u 块（>4 块 = 读了整窗而非一列）", b1);

        /* ②b 真"跨块"补边：窗口右缘推到 x=490（图内），tile 列 4（512..516）首次进入
         * ⇒ 这一拍必须发 SD 命令，且只读"新露出的那一列块"（1 列 × 覆盖到的瓦片行）。 */
        mpak_tile_stat_reset();
        for (int32_t y = 0; y < H; y += 32) {
            int32_t rows = (y + 32 <= H) ? 32 : H - y;
            mpak_bgmap_read_static_rect(&mt, 490, 89 + y, 24, rows, dst, 24);
        }
        mpak_tile_stat_get(&b1, &h1, &by1);
        printf("   ②b 拖动补边（首次跨入新瓦片列，24×288）：瓦片读 %u 块 / 命中 %u 块 / %u KB\n",
               b1, h1, by1 / 1024u);
        ck(b1 >= 1 && b1 <= 4, "跨块补边读了 %u 块（应只读新露出的一列 = 1~4 块）", b1);

        /* 旧包（逐行）路径：统计应保持 0（不走瓦片缓存） */
        mpak_tile_stat_reset();
        mpak_bgmap_read_static_rect(&mr, 137, 89, W, H, dst, W);
        mpak_tile_stat_get(&b0, &h0, &by0);
        ck(b0 == 0 && h0 == 0, "逐行包居然走了瓦片缓存（读 %u 块/命中 %u）", b0, h0);
        printf("   ④ 逐行包同窗口：瓦片读 0 块 / 命中 0 块（走原逐行路径 ✓）\n");

        /* 喂狗计数：确认瓦片读循环里调了 watchdog_kick（真机 5s 不喂 = E14 熔断） */
        ck(g_test_watchdog_kicks > 0, "瓦片整块读循环里没有喂狗");
        printf("   ⑤ watchdog_kick 调用计数 = %u（瓦片读循环内每块一次）\n",
               g_test_watchdog_kicks);
        free(dst); free(kdst);
    }

    /* ── ⑤ 条带口径：直接调低层瓦片 API（compositor 的条带路径用法）── */
    puts("\n⑤ 条带口径（mpak_tile_read_px / _mask 直调，模拟 strip 的裸 fd 路径）");
    {
        int fd = open(path_t, O_RDONLY);
        ck(fd >= 0, "打开瓦片包失败");
        mpak_tile_src_t ts;
        mpak_tile_src_init(&ts, mpak_path_id(path_t), fd,
                           (uint32_t)mt.payload_off + bt->static_back_off, MVW, MVH, T);
        ck(ts.gx == gx && ts.gy == gy, "条带瓦片源网格错 %dx%d", ts.gx, ts.gy);
        const int32_t W = 288, H = 288;
        uint16_t *dst = malloc((size_t)W * H * 2u);
        uint16_t *ref = malloc((size_t)W * H * 2u);
        mpak_bgmap_read_static_rect(&mt, 100, 50, W, H, ref, W);
        mpak_tile_read_px(&ts, 100, 50, W, H, dst, W);
        ck(memcmp(dst, ref, (size_t)W * H * 2u) == 0, "条带瓦片读与 BGMAP 矩形读不一致");
        /* 越界矩形（条带包边缘）：补 0 不报错 */
        uint16_t one[4] = { 0x1111, 0x2222, 0x3333, 0x4444 };
        int rc = mpak_tile_read_px(&ts, MVW - 2, MVH - 2, 4, 1, one, 4);
        ck(rc == MPAK_OK && one[2] == 0 && one[3] == 0, "越界补 0 行为错 rc=%d %04x %04x",
           rc, one[2], one[3]);
        /* 条带掩码：低层 API 取掩码区（strip 的 cov_off 口径）与 tile 掩码矩形读对拍 */
        uint8_t *kt = malloc((size_t)W * H), *kr = malloc((size_t)W * H);
        mpak_tile_src_t tsm;
        mpak_tile_src_init(&tsm, mpak_path_id(path_t), fd,
                           (uint32_t)mt.payload_off + bt->tile_mask_off, MVW, MVH, T);
        memset(kt, 0xAA, (size_t)W * H);
        memset(kr, 0x55, (size_t)W * H);
        mpak_tile_read_mask(&tsm, 100, 50, W, H, kt, W);
        mpak_bgmap_read_tile_mask_rect(&mt, 100, 50, W, H, kr, W);
        ck(memcmp(kt, kr, (size_t)W * H) == 0, "条带瓦片掩码读与 BGMAP 掩码矩形读不一致");
        printf("   条带瓦片读 288×288：与 BGMAP 矩形读一致 ✓；掩码一致 ✓；越界补 0 ✓\n");
        free(kt); free(kr);
        close(fd);
        free(dst); free(ref);
    }


    /* ── ⑥ 瓦片条带包（PARTS，契约 §4）：包自身自证瓦片布局 ── */
    puts("\n⑥ 瓦片条带包（PARTS part0 = 带图，300×200）");
    {
        mpak_t ms, msr, msn;
        ck(mpak_open(&ms, path_st, 0, MPAK_KIND_PARTS) == MPAK_OK, "瓦片条带包 open 失败");
        ck(mpak_open(&msr, path_sr, 0, MPAK_KIND_PARTS) == MPAK_OK, "逐行条带包 open 失败");
        ck(mpak_open(&msn, path_stn, 0, MPAK_KIND_PARTS) == MPAK_OK, "瓦片条带包(无掩码) open 失败");
        const mpak_part_t *q = ms.parts_tab ? &ms.parts_tab[0] : NULL;
        const mpak_part_t *qr = msr.parts_tab ? &msr.parts_tab[0] : NULL;
        const mpak_part_t *qn = msn.parts_tab ? &msn.parts_tab[0] : NULL;
        ck(q && qr && qn, "条带包解析为空");
        if (q && qr && qn) {
            ck(q->tiled && q->tile == T && q->gx == 3 && q->gy == 2,
               "瓦片条带识别错：tiled=%d tile=%d 网格 %dx%d", (int)q->tiled, q->tile, q->gx, q->gy);
            ck(q->has_alpha && q->mask_bytes == (uint32_t)3 * 2 * (T * T / 8u),
               "瓦片条带掩码：has_alpha=%d mask_bytes=%u", (int)q->has_alpha, q->mask_bytes);
            ck(q->pixel_bytes == (uint32_t)3 * 2 * T * T * 2u,
               "瓦片条带像素字节 = %u", q->pixel_bytes);
            ck(!qr->tiled && qr->has_alpha, "逐行条带被误判为瓦片（tiled=%d）", (int)qr->tiled);
            ck(qn->tiled && !qn->has_alpha, "无掩码瓦片条带识别错（tiled=%d alpha=%d）",
               (int)qn->tiled, (int)qn->has_alpha);
            printf("   瓦片条带：tile=%d 网格 %dx%d 像素 %u B 掩码 %u B（has_alpha=%d）\n",
                   q->tile, q->gx, q->gy, q->pixel_bytes, q->mask_bytes, (int)q->has_alpha);
            printf("   逐行条带：tiled=%d（=0 → 原逐行路径）像素 %u B 掩码 %u B\n",
                   (int)qr->tiled, qr->pixel_bytes, qr->mask_bytes);

            /* 用 compositor 条带路径的方式取像素：裸 fd + 瓦片源 + 整块读，
             * 与逐行包读出的同一世界矩形逐像素对拍。 */
            int fd = open(path_st, O_RDONLY);
            int fdr = open(path_sr, O_RDONLY);
            ck(fd >= 0 && fdr >= 0, "条带包打开 fd 失败");
            const int32_t W = 288, H = 200;
            uint16_t *dt = malloc((size_t)W * H * 2u);
            uint16_t *dr = malloc((size_t)W * H * 2u);
            for (int32_t x0 = 0; x0 <= 60; x0 += 30) {
                mpak_tile_src_t ts;
                mpak_tile_src_init(&ts, mpak_path_id(path_st), fd,
                                   (uint32_t)ms.payload_off + ms.parts_bmp_base + q->offset,
                                   q->w, q->h, q->tile);
                mpak_tile_read_px(&ts, x0, 0, 300 - x0, H, dt, 300 - x0);
                /* 逐行包：按 row_bytes 直读（模拟旧口径） */
                uint32_t stride = align4u((uint32_t)300 * 2u);
                for (int32_t y = 0; y < H; y++) {
                    uint32_t off = (uint32_t)msr.payload_off + msr.parts_bmp_base + qr->offset
                                   + (uint32_t)y * stride + (uint32_t)x0 * 2u;
                    ck(pread(fdr, dr + (size_t)y * (300 - x0), (size_t)(300 - x0) * 2u,
                             (off_t)off) == (ssize_t)((size_t)(300 - x0) * 2u), "逐行条带读失败");
                }
                int bad = 0;
                for (int32_t i = 0; i < (300 - x0) * H; i++)
                    if (dt[i] != dr[i]) { bad = i; break; }
                ck(bad == 0, "瓦片条带 vs 逐行条带第 %d 像素不同（x0=%d）", bad, x0);
                printf("   条带 x0=%d 宽 %d：瓦片/逐行 逐像素一致 ✓\n", x0, 300 - x0);
            }
            free(dt); free(dr);
            close(fd); close(fdr);
        }
        mpak_close(&ms); mpak_close(&msr); mpak_close(&msn);
    }

    mpak_close(&mt);
    mpak_close(&mr);
    mpak_close(&m64);
    free(pt); free(pr); free(p64); free(g_b);
    free(pst); free(psr); free(pstn);
    unlink(path_t); unlink(path_r); unlink(path_t64);
    unlink(path_st); unlink(path_sr); unlink(path_stn);

    printf("\n===== %s：%d 项检查，%d 项失败 =====\n",
           g_fail ? "FAIL" : "PASS", g_check, g_fail);
    return g_fail ? 1 : 0;
}
