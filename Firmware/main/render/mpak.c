/*
 * mpak.c — MPAK 素材包流式解析实现
 *
 * 校验序（algorithm-asset-format.md 第二节）：
 *   MAGIC → version → 文件长度 → 尾部 crc32c（覆盖 header+payload 全量）
 *   → content_hash 比对 → kind。
 * 解析策略：头 + 索引一次读入（临时内部 RAM 缓冲，逐字段游标解析后拷入宿主结构），
 * 位图数据区不读；后续 mpak_part_read_* / mpak_font_read_glyph_bmp 以
 * fseek+fread 懒加载（TF 直读流式，包不整载内存）。
 */
#include "mpak.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "mpak";

static void mpak_layout_free(mpak_layout_t *lt); /* 前置声明 */

static int u32_cmp(const void *a, const void *b)
{
    uint32_t ua = *(const uint32_t *)a, ub = *(const uint32_t *)b;
    return (ua > ub) - (ua < ub);
}

/* 首个 > key 的元素指针（无则 NULL） */
static const uint32_t *upper_bound(const uint32_t *arr, uint32_t n, uint32_t key)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (arr[mid] <= key) lo = mid + 1;
        else hi = mid;
    }
    return (lo < n) ? &arr[lo] : NULL;
}

/* ------------------------------------------------------------------ */
/* 小端字节游标（主机为小端，读出的 u16/u32/u64 直接有效）              */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         pos;
} cur_t;

static int cur_u8(cur_t *c, uint8_t *out)
{
    if (c->pos + 1 > c->len) return MPAK_ERR_FMT;
    *out = c->p[c->pos++];
    return MPAK_OK;
}

static int cur_i8(cur_t *c, int8_t *out)
{
    if (c->pos + 1 > c->len) return MPAK_ERR_FMT;
    *out = (int8_t)c->p[c->pos++];
    return MPAK_OK;
}

static int cur_u16(cur_t *c, uint16_t *out)
{
    if (c->pos + 2 > c->len) return MPAK_ERR_FMT;
    *out = (uint16_t)(c->p[c->pos] | ((uint16_t)c->p[c->pos + 1] << 8));
    c->pos += 2;
    return MPAK_OK;
}

static int cur_i16(cur_t *c, int16_t *out)
{
    uint16_t u;
    int rc = cur_u16(c, &u);
    if (rc) return rc;
    *out = (int16_t)u;
    return MPAK_OK;
}

static int cur_u32(cur_t *c, uint32_t *out)
{
    if (c->pos + 4 > c->len) return MPAK_ERR_FMT;
    *out = (uint32_t)c->p[c->pos] |
           ((uint32_t)c->p[c->pos + 1] << 8) |
           ((uint32_t)c->p[c->pos + 2] << 16) |
           ((uint32_t)c->p[c->pos + 3] << 24);
    c->pos += 4;
    return MPAK_OK;
}

static int cur_u64(cur_t *c, uint64_t *out)
{
    uint32_t lo, hi;
    if (cur_u32(c, &lo) || cur_u32(c, &hi)) return MPAK_ERR_FMT;
    *out = (uint64_t)lo | ((uint64_t)hi << 32);
    return MPAK_OK;
}

static int cur_bytes(cur_t *c, void *out, size_t n)
{
    if (c->pos + n > c->len) return MPAK_ERR_FMT;
    memcpy(out, c->p + c->pos, n);
    c->pos += n;
    return MPAK_OK;
}

/* 定长 UTF-8 名字区 → NUL 终止宿主串（不检查 UTF-8 合法性） */
static int cur_name(cur_t *c, char *out, size_t out_sz /* 含 NUL，=33 */)
{
    if (out_sz < MPAK_NAME_LEN + 1) return MPAK_ERR_ARG;
    int rc = cur_bytes(c, out, MPAK_NAME_LEN);
    if (rc) return rc;
    out[MPAK_NAME_LEN] = '\0';
    return MPAK_OK;
}

static size_t align4(size_t n) { return (n + 3u) & ~3u; }

/* ------------------------------------------------------------------ */
/* crc32c（软件实现；表运行期生成一次，仅渲染任务访问，无锁）            */
/* ------------------------------------------------------------------ */

static uint32_t s_crc_table[256];
static bool     s_crc_ready;

static void crc32c_init_table(void)
{
    if (s_crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        s_crc_table[i] = c;
    }
    s_crc_ready = true;
}

uint32_t mpak_crc32c(uint32_t crc, const uint8_t *data, size_t len)
{
    crc32c_init_table();
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = s_crc_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------------ */
/* 内部小工具                                                          */
/* ------------------------------------------------------------------ */

static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) ESP_LOGE(TAG, "PSRAM alloc %zu failed", n);
    return p;
}

static int rd_exact(FILE *f, void *dst, size_t n)
{
    return (fread(dst, 1, n, f) == n) ? MPAK_OK : MPAK_ERR_IO;
}

uint64_t mpak_content_hash(const mpak_t *m)
{
    return m ? m->content_hash : 0;
}

int mpak_read_at(mpak_t *m, uint32_t file_off, void *dst, size_t len)
{
    if (!m || !m->f) return MPAK_ERR_ARG;
    if (fseek(m->f, (long)file_off, SEEK_SET) != 0) return MPAK_ERR_IO;
    return rd_exact(m->f, dst, len);
}

/* ------------------------------------------------------------------ */
/* 信封校验 + 头读取                                                    */
/* ------------------------------------------------------------------ */

static int envelope_check(mpak_t *m, uint64_t expect_hash, uint64_t expect_kind)
{
    uint8_t hdr[MPAK_HEADER_LEN];
    if (rd_exact(m->f, hdr, sizeof hdr)) return MPAK_ERR_IO;

    cur_t c = { hdr, sizeof hdr, 0 };
    uint32_t magic = 0;
    uint16_t version = 0, flags = 0;

    cur_u32(&c, &magic);
    cur_u16(&c, &version);
    cur_u16(&c, &flags);
    c.pos += 8;                       /* magic 块 16B 的 8B 0 填充 */
    if (magic != MPAK_MAGIC0) {
        ESP_LOGE(TAG, "bad magic 0x%08" PRIx32, magic);
        return MPAK_ERR_MAGIC;
    }
    if (version != MPAK_VERSION) {
        ESP_LOGE(TAG, "bad version %u", version);
        return MPAK_ERR_VERSION;
    }
    (void)flags;

    uint64_t kind = 0, hash = 0;
    uint32_t payload_len = 0, reserved = 0;
    cur_u64(&c, &kind);
    cur_u64(&c, &hash);
    cur_u32(&c, &payload_len);
    cur_u32(&c, &reserved);

    if (kind < MPAK_KIND_PARTS || kind > MPAK_KIND_AUDIO_META) {
        ESP_LOGE(TAG, "bad kind %llu", (unsigned long long)kind);
        return MPAK_ERR_KIND;
    }
    if (expect_kind != 0 && expect_kind != kind) {
        ESP_LOGE(TAG, "kind %llu != expect %llu",
                 (unsigned long long)kind, (unsigned long long)expect_kind);
        return MPAK_ERR_KIND;
    }
    if (payload_len == 0 || payload_len > MPAK_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "payload_len %" PRIu32 " out of range", payload_len);
        return MPAK_ERR_LEN;
    }

    /* 长度校验：文件必须恰为 40 + payload_len + 8 */
    long file_sz = 0;
    if (fseek(m->f, 0, SEEK_END) != 0) return MPAK_ERR_IO;
    file_sz = ftell(m->f);
    if (file_sz < 0) return MPAK_ERR_IO;
    long expect_sz = (long)MPAK_HEADER_LEN + (long)payload_len + (long)MPAK_TRAILER_LEN;
    if (file_sz != expect_sz) {
        ESP_LOGE(TAG, "file size %ld != envelope %ld", file_sz, expect_sz);
        return MPAK_ERR_LEN;
    }

    /* crc32c 流式覆盖 header+payload（MAGIC 至 payload 全量） */
    uint32_t crc = mpak_crc32c(0, hdr, sizeof hdr);
    if (fseek(m->f, (long)sizeof hdr, SEEK_SET) != 0) return MPAK_ERR_IO;
    uint8_t chunk[2048];
    uint32_t remain = payload_len;
    while (remain) {
        uint32_t n = remain > sizeof chunk ? (uint32_t)sizeof chunk : remain;
        if (rd_exact(m->f, chunk, n)) return MPAK_ERR_IO;
        crc = mpak_crc32c(crc, chunk, n);
        remain -= n;
    }
    uint8_t trailer[MPAK_TRAILER_LEN];
    if (rd_exact(m->f, trailer, sizeof trailer)) return MPAK_ERR_IO;
    cur_t tc = { trailer, sizeof trailer, 0 };
    uint32_t crc_stored = 0, zero = 0;
    cur_u32(&tc, &crc_stored);
    cur_u32(&tc, &zero);
    if (crc != crc_stored) {
        ESP_LOGE(TAG, "crc32c mismatch stored=%08" PRIx32 " calc=%08" PRIx32,
                 crc_stored, crc);
        return MPAK_ERR_CRC;
    }
    if (zero != 0) {
        ESP_LOGE(TAG, "trailer zero field != 0");
        return MPAK_ERR_FMT;
    }

    /* hash 比对（manifest 期望值） */
    if (expect_hash != 0 && expect_hash != hash) {
        ESP_LOGE(TAG, "content_hash %llu != manifest %llu",
                 (unsigned long long)hash, (unsigned long long)expect_hash);
        return MPAK_ERR_HASH;
    }

    m->kind         = kind;
    m->content_hash = hash;
    m->payload_len  = payload_len;
    m->payload_off  = MPAK_HEADER_LEN;
    (void)reserved;
    return MPAK_OK;
}

/* payload 区一次性读入（头+索引区，≤ 数百 KB） */
static uint8_t *payload_read(const mpak_t *m, uint32_t off, uint32_t len)
{
    if ((uint64_t)off + len > m->payload_len) return NULL;
    uint8_t *buf = psram_alloc(len ? len : 1);
    if (!buf) return NULL;
    if (mpak_read_at((mpak_t *)m, m->payload_off + off, buf, len)) {
        heap_caps_free(buf);
        return NULL;
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* kind=1 PARTS                                                        */
/* ------------------------------------------------------------------ */

#define MPAK_PARTS_MAX 4096u

static int parse_parts(mpak_t *m)
{
    uint8_t *buf = payload_read(m, 0, 4);
    if (!buf) return MPAK_ERR_IO;
    cur_t c = { buf, 4, 0 };
    uint32_t n = 0;
    cur_u32(&c, &n);
    heap_caps_free(buf);
    if (n == 0 || n > MPAK_PARTS_MAX) {
        ESP_LOGE(TAG, "parts count %" PRIu32 " invalid", n);
        return MPAK_ERR_FMT;
    }

    uint32_t idx_len = 4u + 20u * n;
    buf = payload_read(m, 0, idx_len);
    if (!buf) return MPAK_ERR_IO;
    c = (cur_t){ buf, idx_len, 0 };
    cur_u32(&c, &n); /* 重复读 count */

    mpak_part_t *tab = psram_alloc(n * sizeof(mpak_part_t));
    if (!tab) { heap_caps_free(buf); return MPAK_ERR_NOMEM; }
    memset(tab, 0, n * sizeof(mpak_part_t));

    int rc = MPAK_OK;
    for (uint32_t i = 0; i < n && !rc; i++) {
        uint32_t part_id = 0, offset = 0;
        uint16_t expr_group = 0, w = 0, h = 0;
        int16_t origin_x = 0, origin_y = 0;
        rc |= cur_u32(&c, &part_id);
        rc |= cur_u16(&c, &expr_group);
        rc |= cur_u16(&c, &w);
        rc |= cur_u16(&c, &h);
        rc |= cur_i16(&c, &origin_x);
        rc |= cur_i16(&c, &origin_y);
        rc |= cur_u32(&c, &offset);
        uint16_t pad20 = 0;
        rc |= cur_u16(&c, &pad20);   /* 索引项 20B 尾填充 */
        if (rc) break;

        if (w == 0 || h == 0 || w > 1024 || h > 1024) {
            ESP_LOGE(TAG, "part %" PRIu32 " size %ux%u invalid", part_id, w, h);
            rc = MPAK_ERR_FMT;
            break;
        }
        tab[i].id         = part_id;
        tab[i].expr_group = expr_group;
        tab[i].w          = w;
        tab[i].h          = h;
        tab[i].origin_x   = origin_x;
        tab[i].origin_y   = origin_y;
        tab[i].offset     = offset;
        tab[i].pixel_bytes = (uint32_t)h * (uint32_t)align4((size_t)w * 2u);
        tab[i].mask_bytes  = (((uint32_t)w * h + 7u) / 8u);
        tab[i].has_alpha  = false;
        tab[i].extent     = 0;
    }
    heap_caps_free(buf);
    if (rc) { heap_caps_free(tab); return rc; }

    /*
     * extent 推导（判定 1bit alpha 掩码是否存在——格式删除了 flags 字段）：
     * 对每个 part 取位图区内「下一个更大 offset」（或位图区末尾）为本图 extent；
     *   extent == pixel_bytes          → 不透明 RGB565
     *   extent == pixel_bytes+mask_raw → RGBA5650（像素后附 1bit 掩码）
     * 其余视为损坏。
     * O(n log n)：对 offset 排序副本做后继查找。
     */
    uint32_t bmp_base  = idx_len;                 /* payload 相对 */
    uint32_t bmp_area  = m->payload_len - bmp_base;
    uint32_t *offs = malloc(n * sizeof(uint32_t));
    if (!offs) { heap_caps_free(tab); return MPAK_ERR_NOMEM; }
    for (uint32_t i = 0; i < n; i++) offs[i] = tab[i].offset;
    qsort(offs, n, sizeof(uint32_t), u32_cmp);

    for (uint32_t i = 0; i < n; i++) {
        /* 后继：offs 中第一个 > tab[i].offset 的值 */
        uint32_t end = bmp_area;
        const uint32_t *found = upper_bound(offs, n, tab[i].offset);
        if (found) end = *found;
        if (end < tab[i].offset ||
            end - tab[i].offset < tab[i].pixel_bytes) {
            ESP_LOGE(TAG, "part %" PRIu32 " extent (%u..%u) < pixels (%" PRIu32 ")",
                     tab[i].id, tab[i].offset, end, tab[i].pixel_bytes);
            free(offs);
            heap_caps_free(tab);
            return MPAK_ERR_FMT;
        }
        uint32_t ext = end - tab[i].offset;
        uint32_t mask_raw = tab[i].mask_bytes;
        uint32_t mask_al  = (uint32_t)align4(mask_raw); /* 掩码区亦按 4 对齐裁量 */
        if (ext == tab[i].pixel_bytes) {
            tab[i].has_alpha = false;
        } else if (ext >= tab[i].pixel_bytes + mask_raw &&
                   ext <= tab[i].pixel_bytes + mask_al + 3u) {
            tab[i].has_alpha = true;
        } else {
            ESP_LOGE(TAG, "part %" PRIu32 " extent %u matches neither opaque nor alpha",
                     tab[i].id, ext);
            free(offs);
            heap_caps_free(tab);
            return MPAK_ERR_FMT;
        }
        tab[i].extent = ext;
    }
    free(offs);

    m->parts_tab    = tab;
    m->parts_count  = n;
    m->parts_bmp_base = bmp_base;
    return MPAK_OK;
}

const mpak_part_t *mpak_parts_find(const mpak_t *m, uint32_t part_id)
{
    if (!m || !m->parts_tab) return NULL;
    for (uint32_t i = 0; i < m->parts_count; i++)
        if (m->parts_tab[i].id == part_id) return &m->parts_tab[i];
    return NULL;
}

const mpak_part_t *mpak_parts_variant(const mpak_t *m, const mpak_part_t *p, uint32_t idx)
{
    if (!m || !m->parts_tab || !p || p->expr_group == 0) return NULL;
    const mpak_part_t *first = NULL;
    uint32_t count = 0;
    for (uint32_t i = 0; i < m->parts_count; i++) {
        if (m->parts_tab[i].expr_group == p->expr_group) {
            if (!first) first = &m->parts_tab[i];
            count++;
        }
    }
    if (!first || idx >= count) return NULL;
    /* 组内顺序 = 索引顺序；first 已是组内第一个 */
    const mpak_part_t *v = first;
    for (uint32_t k = 0; k < idx; k++) {
        do { v++; } while (v < m->parts_tab + m->parts_count &&
                          v->expr_group != p->expr_group);
    }
    return (v < m->parts_tab + m->parts_count) ? v : NULL;
}

static int part_read(const mpak_t *m, const mpak_part_t *p, bool mask,
                     uint8_t *dst, size_t cap)
{
    if (!m || !p || !dst) return MPAK_ERR_ARG;
    uint32_t off = m->payload_off + m->parts_bmp_base + p->offset;
    uint32_t len;
    if (mask) {
        if (!p->has_alpha) return MPAK_ERR_RANGE;
        off += p->pixel_bytes;
        len = p->mask_bytes;
    } else {
        len = p->pixel_bytes;
    }
    if (cap < len) return MPAK_ERR_ARG;
    return mpak_read_at((mpak_t *)m, off, dst, len);
}

int mpak_part_read_pixels(const mpak_t *m, const mpak_part_t *p, uint8_t *dst, size_t cap)
{
    return part_read(m, p, false, dst, cap);
}

int mpak_part_read_mask(const mpak_t *m, const mpak_part_t *p, uint8_t *dst, size_t cap)
{
    return part_read(m, p, true, dst, cap);
}

/* ------------------------------------------------------------------ */
/* kind=2 LAYOUT                                                       */
/* ------------------------------------------------------------------ */

#define MPAK_LAYOUT_MAX_FRAMES 512u
#define MPAK_LAYOUT_MAX_EXPR   64u
#define MPAK_LAYOUT_MAX_BUF    (512u * 1024u)

static int parse_layout(mpak_t *m)
{
    /* 先读定长前段：entity_id + action32 + frame_count + expression_count = 44B */
    uint8_t *buf = payload_read(m, 0, 44);
    if (!buf) return MPAK_ERR_IO;
    cur_t c = { buf, 44, 0 };
    uint32_t entity_id = 0, frame_count = 0, expr_count = 0;
    cur_u32(&c, &entity_id);
    char action[MPAK_NAME_LEN + 1];
    cur_name(&c, action, sizeof action);
    cur_u32(&c, &frame_count);
    cur_u32(&c, &expr_count);
    heap_caps_free(buf);

    if (frame_count == 0 || frame_count > MPAK_LAYOUT_MAX_FRAMES ||
        expr_count == 0 || expr_count > MPAK_LAYOUT_MAX_EXPR) {
        ESP_LOGE(TAG, "layout %s counts f=%" PRIu32 " e=%" PRIu32 " invalid",
                 action, frame_count, expr_count);
        return MPAK_ERR_FMT;
    }

    mpak_layout_t *lt = psram_alloc(sizeof *lt);
    if (!lt) return MPAK_ERR_NOMEM;
    memset(lt, 0, sizeof *lt);
    lt->entity_id        = entity_id;
    memcpy(lt->action, action, sizeof action);
    lt->frame_count      = frame_count;
    lt->expression_count = expr_count;

    lt->expr_names = psram_alloc((size_t)expr_count * (MPAK_NAME_LEN + 1));
    if (!lt->expr_names) goto nomem;
    /* 读表情名区（44 + e*32） */
    {
        uint8_t *nb = payload_read(m, 44, (uint32_t)(expr_count * MPAK_NAME_LEN));
        if (!nb) goto io;
        cur_t nc = { nb, (size_t)expr_count * MPAK_NAME_LEN, 0 };
        for (uint32_t i = 0; i < expr_count; i++)
            cur_name(&nc, lt->expr_names[i], MPAK_NAME_LEN + 1);
        heap_caps_free(nb);
    }

    /* 剩余帧区长度 = payload_len - 44 - e*32；帧+piece 全读 */
    uint32_t frames_off = 44 + expr_count * MPAK_NAME_LEN;
    uint32_t frames_len = m->payload_len - frames_off;
    if (frames_len == 0 || frames_len > MPAK_LAYOUT_MAX_BUF) {
        ESP_LOGE(TAG, "layout frames area %" PRIu32 " invalid", frames_len);
        goto fmt;
    }
    buf = payload_read(m, frames_off, frames_len);
    if (!buf) goto io;
    c = (cur_t){ buf, frames_len, 0 };

    lt->frames = psram_alloc((size_t)frame_count * sizeof(mpak_frame_t));
    if (!lt->frames) { heap_caps_free(buf); goto nomem; }

    /* 第一遍：帧头，累计 piece 总数（帧头 12B：delay + dx/dy + piece_count） */
    uint32_t pieces_total = 0;
    for (uint32_t f = 0; f < frame_count; f++) {
        uint32_t d = 0, pc = 0;
        int16_t dx = 0, dy = 0;
        if (cur_u32(&c, &d) || cur_i16(&c, &dx) || cur_i16(&c, &dy) ||
            cur_u32(&c, &pc)) { heap_caps_free(buf); goto fmt; }
        if (pc > 256) { heap_caps_free(buf); goto fmt; }
        lt->frames[f].delay_ms    = d ? d : 50u; /* delay=0 防死循环 → 50ms */
        lt->frames[f].move_dx     = dx;
        lt->frames[f].move_dy     = dy;
        lt->frames[f].piece_count = pc;
        lt->frames[f].piece_off   = pieces_total;
        pieces_total += pc;
        /* 跳过 piece 区（第二遍再读） */
        if (c.pos + (size_t)pc * 12u > c.len) { heap_caps_free(buf); goto fmt; }
        c.pos += (size_t)pc * 12u;
    }
    if (pieces_total == 0) { heap_caps_free(buf); goto fmt; }

    lt->pieces = psram_alloc((size_t)pieces_total * sizeof(mpak_piece_t));
    if (!lt->pieces) { heap_caps_free(buf); goto nomem; }
    lt->pieces_total = pieces_total;

    /* 第二遍：piece（游标复位） */
    c.pos = 0;
    for (uint32_t f = 0; f < frame_count; f++) {
        c.pos += 12; /* 帧头 */
        mpak_frame_t *fr = &lt->frames[f];
        for (uint32_t k = 0; k < fr->piece_count; k++) {
            mpak_piece_t *pc = &lt->pieces[fr->piece_off + k];
            uint8_t expr = 0, flip = 0, pad12 = 0;
            int8_t z = 0;
            if (cur_u32(&c, &pc->part_id) || cur_u8(&c, &expr) ||
                cur_i16(&c, &pc->x) || cur_i16(&c, &pc->y) ||
                cur_u8(&c, &flip) || cur_i8(&c, &z) ||
                cur_u8(&c, &pad12)) { heap_caps_free(buf); goto fmt; } /* 12B 尾填充 */
            pc->expr_index = expr;
            pc->flip       = flip;
            pc->z          = z;
        }
    }
    heap_caps_free(buf);

    m->u.layout = lt;
    return MPAK_OK;

nomem:
    mpak_layout_free(lt);
    return MPAK_ERR_NOMEM;
io:
    mpak_layout_free(lt);
    return MPAK_ERR_IO;
fmt:
    mpak_layout_free(lt);
    return MPAK_ERR_FMT;
}

/* ------------------------------------------------------------------ */
/* kind=3 BGMAP                                                        */
/* ------------------------------------------------------------------ */

#define MPAK_BGMAP_MAX_STRIPS 16u

static int parse_bgmap(mpak_t *m)
{
    uint32_t hdr_len = 56;
    uint8_t *buf = payload_read(m, 0, hdr_len);
    if (!buf) return MPAK_ERR_IO;
    cur_t c = { buf, hdr_len, 0 };

    mpak_bgmap_t *bg = psram_alloc(sizeof *bg);
    if (!bg) { heap_caps_free(buf); return MPAK_ERR_NOMEM; }
    memset(bg, 0, sizeof *bg);

    int rc = MPAK_OK;
    rc |= cur_name(&c, bg->map_id, sizeof bg->map_id);
    rc |= cur_u16(&c, &bg->vw);
    rc |= cur_u16(&c, &bg->vh);
    rc |= cur_u32(&c, &bg->static_back_len);
    rc |= cur_u32(&c, &bg->static_back_off);
    rc |= cur_u32(&c, &bg->tile_layer_len);
    rc |= cur_u32(&c, &bg->tile_layer_off);
    rc |= cur_u32(&c, &bg->strip_count);
    heap_caps_free(buf);
    if (rc) { heap_caps_free(bg); return MPAK_ERR_FMT; }

    uint64_t px = (uint64_t)bg->vw * bg->vh;
    if (bg->vw == 0 || bg->vh == 0 || bg->vw > 512 || bg->vh > 512 ||
        bg->static_back_len != (uint32_t)(px * 2u) ||
        (bg->tile_layer_len != 0 &&
         bg->tile_layer_len != (uint32_t)(px * 2u + (px + 7u) / 8u)) ||
        bg->strip_count > MPAK_BGMAP_MAX_STRIPS) {
        ESP_LOGE(TAG, "bgmap %s geometry invalid (vw=%u vh=%u)", bg->map_id, bg->vw, bg->vh);
        heap_caps_free(bg);
        return MPAK_ERR_FMT;
    }

    if (bg->strip_count) {
        bg->strips = psram_alloc(bg->strip_count * sizeof(mpak_strip_t));
        if (!bg->strips) { heap_caps_free(bg); return MPAK_ERR_NOMEM; }
        buf = payload_read(m, hdr_len, bg->strip_count * (uint32_t)sizeof(mpak_wire_strip_t));
        if (!buf) { heap_caps_free(bg->strips); heap_caps_free(bg); return MPAK_ERR_IO; }
        c = (cur_t){ buf, (size_t)bg->strip_count * sizeof(mpak_wire_strip_t), 0 };
        for (uint32_t i = 0; i < bg->strip_count; i++) {
            uint64_t ref = 0;
            int16_t y = 0, sx = 0;
            uint8_t rx = 0, bl = 0;
            if (cur_u64(&c, &ref) || cur_i16(&c, &y) || cur_i16(&c, &sx) ||
                cur_u8(&c, &rx) || cur_u8(&c, &bl)) {
                heap_caps_free(buf);
                heap_caps_free(bg->strips);
                heap_caps_free(bg);
                return MPAK_ERR_FMT;
            }
            bg->strips[i].part_ref   = ref;
            bg->strips[i].y          = y;
            bg->strips[i].speed_x    = sx;
            bg->strips[i].rx_parallax = rx;
            bg->strips[i].blend      = bl;
        }
        heap_caps_free(buf);
    }

    m->u.bgmap = bg;
    return MPAK_OK;
}

int mpak_bgmap_read_static(const mpak_t *m, uint8_t *dst, size_t cap)
{
    if (!m || !m->u.bgmap) return MPAK_ERR_ARG;
    if (cap < m->u.bgmap->static_back_len) return MPAK_ERR_ARG;
    return mpak_read_at((mpak_t *)m, m->payload_off + m->u.bgmap->static_back_off,
                        dst, m->u.bgmap->static_back_len);
}

int mpak_bgmap_read_tile(const mpak_t *m, uint8_t *dst, size_t cap)
{
    if (!m || !m->u.bgmap) return MPAK_ERR_ARG;
    if (m->u.bgmap->tile_layer_len == 0) return MPAK_ERR_RANGE;
    if (cap < m->u.bgmap->tile_layer_len) return MPAK_ERR_ARG;
    return mpak_read_at((mpak_t *)m, m->payload_off + m->u.bgmap->tile_layer_off,
                        dst, m->u.bgmap->tile_layer_len);
}

/* ------------------------------------------------------------------ */
/* kind=4 FONT                                                         */
/* ------------------------------------------------------------------ */

#define MPAK_FONT_MAX_GLYPHS 8192u

static int glyph_cmp(const void *a, const void *b)
{
    uint32_t ua = ((const mpak_glyph_t *)a)->unicode;
    uint32_t ub = ((const mpak_glyph_t *)b)->unicode;
    return (ua > ub) - (ua < ub);
}

static int parse_font(mpak_t *m)
{
    uint8_t *buf = payload_read(m, 0, 8);
    if (!buf) return MPAK_ERR_IO;
    cur_t c = { buf, 8, 0 };
    uint8_t size_px = 0, bpp = 0;
    uint16_t pad = 0;
    uint32_t n = 0;
    cur_u8(&c, &size_px);
    cur_u8(&c, &bpp);
    cur_u16(&c, &pad);
    cur_u32(&c, &n);
    heap_caps_free(buf);

    if (bpp != 4) { /* 本实现仅支持 4bpp→LVGL A4 */
        ESP_LOGE(TAG, "font bpp %u != 4", bpp);
        return MPAK_ERR_FMT;
    }
    if (n == 0 || n > MPAK_FONT_MAX_GLYPHS) {
        ESP_LOGE(TAG, "font glyph_count %" PRIu32 " invalid", n);
        return MPAK_ERR_FMT;
    }

    mpak_font_t *ft = psram_alloc(sizeof *ft);
    if (!ft) return MPAK_ERR_NOMEM;
    memset(ft, 0, sizeof *ft);
    ft->size_px = size_px;
    ft->bpp     = bpp;

    ft->glyphs = psram_alloc((size_t)n * sizeof(mpak_glyph_t));
    ft->bmp_prefix = psram_alloc(((size_t)n + 1) * sizeof(uint32_t));
    if (!ft->glyphs || !ft->bmp_prefix) {
        heap_caps_free(ft->glyphs); heap_caps_free(ft->bmp_prefix);
        heap_caps_free(ft); return MPAK_ERR_NOMEM;
    }

    uint32_t idx_len = 8u + 12u * n;
    buf = payload_read(m, 0, idx_len);
    if (!buf) {
        heap_caps_free(ft->glyphs); heap_caps_free(ft->bmp_prefix);
        heap_caps_free(ft); return MPAK_ERR_IO;
    }
    c = (cur_t){ buf, idx_len, 0 };
    cur_u8(&c, &size_px); cur_u8(&c, &bpp); cur_u16(&c, &pad); cur_u32(&c, &n);

    for (uint32_t i = 0; i < n; i++) {
        mpak_glyph_t *g = &ft->glyphs[i];
        uint16_t w = 0, h = 0;
        uint8_t adv = 0, pad12 = 0;
        int8_t ox = 0, by = 0;
        if (cur_u32(&c, &g->unicode) || cur_u16(&c, &w) || cur_u16(&c, &h) ||
            cur_u8(&c, &adv) || cur_i8(&c, &ox) || cur_i8(&c, &by) ||
            cur_u8(&c, &pad12)) { /* 12B 尾填充 */
            heap_caps_free(buf);
            heap_caps_free(ft->glyphs); heap_caps_free(ft->bmp_prefix);
            heap_caps_free(ft); return MPAK_ERR_FMT;
        }
        if (w > 128 || h > 128) {
            heap_caps_free(buf);
            heap_caps_free(ft->glyphs); heap_caps_free(ft->bmp_prefix);
            heap_caps_free(ft); return MPAK_ERR_FMT;
        }
        g->w = w; g->h = h; g->advance = adv; g->off_x = ox; g->bearing_y = by;
    }
    heap_caps_free(buf);

    /* unicode 升序排序（二分查找前提） */
    qsort(ft->glyphs, n, sizeof(mpak_glyph_t), glyph_cmp);

    /* 位图区前缀和：glyph 依索引序紧凑存放，行长 = (w+1)/2 字节 */
    ft->bmp_base_off = idx_len;
    ft->bmp_prefix[0] = 0;
    uint32_t maxb = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sz = (uint32_t)(((uint32_t)ft->glyphs[i].w + 1u) / 2u) * ft->glyphs[i].h;
        ft->bmp_prefix[i + 1] = ft->bmp_prefix[i] + sz;
        if (sz > maxb) maxb = sz;
    }
    if (m->payload_len < ft->bmp_base_off + ft->bmp_prefix[n]) {
        heap_caps_free(ft->glyphs); heap_caps_free(ft->bmp_prefix);
        heap_caps_free(ft); return MPAK_ERR_FMT;
    }
    ft->glyph_count = n;
    ft->max_bmp_bytes = maxb ? maxb : 1;

    m->u.font = ft;
    return MPAK_OK;
}

static const mpak_glyph_t *font_find(const mpak_font_t *ft, uint32_t unicode)
{
    uint32_t lo = 0, hi = ft->glyph_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (ft->glyphs[mid].unicode < unicode) lo = mid + 1;
        else hi = mid;
    }
    if (lo < ft->glyph_count && ft->glyphs[lo].unicode == unicode)
        return &ft->glyphs[lo];
    return NULL;
}

int mpak_font_find_glyph(const mpak_t *m, uint32_t unicode, const mpak_glyph_t **g_out)
{
    if (!m || !m->u.font || !g_out) return MPAK_ERR_ARG;
    const mpak_glyph_t *g = font_find(m->u.font, unicode);
    if (!g) return MPAK_ERR_RANGE;
    *g_out = g;
    return MPAK_OK;
}

int mpak_font_read_glyph_bmp(const mpak_t *m, const mpak_glyph_t *g, uint8_t *dst, size_t cap)
{
    if (!m || !m->u.font || !g || !dst) return MPAK_ERR_ARG;
    const mpak_font_t *ft = m->u.font;
    uint32_t idx = (uint32_t)(g - ft->glyphs);
    uint32_t off = ft->bmp_base_off + ft->bmp_prefix[idx];
    uint32_t sz  = ft->bmp_prefix[idx + 1] - ft->bmp_prefix[idx];
    if (cap < sz) return MPAK_ERR_ARG;
    return mpak_read_at((mpak_t *)m, m->payload_off + off, dst, sz);
}

/* ------------------------------------------------------------------ */
/* kind=5 AUDIO_META                                                   */
/* ------------------------------------------------------------------ */

#define MPAK_AUDIO_MAX_TRACKS 1024u

static int parse_audio(mpak_t *m)
{
    uint8_t *buf = payload_read(m, 0, 4);
    if (!buf) return MPAK_ERR_IO;
    cur_t c = { buf, 4, 0 };
    uint32_t n = 0;
    cur_u32(&c, &n);
    heap_caps_free(buf);
    if (n > MPAK_AUDIO_MAX_TRACKS) return MPAK_ERR_FMT;

    mpak_audio_t *au = psram_alloc(sizeof *au);
    if (!au) return MPAK_ERR_NOMEM;
    memset(au, 0, sizeof *au);
    au->track_count = n;
    if (n == 0) { m->u.audio = au; return MPAK_OK; }

    au->tracks = psram_alloc((size_t)n * sizeof(mpak_track_t));
    if (!au->tracks) { heap_caps_free(au); return MPAK_ERR_NOMEM; }

    uint32_t area = 4u + 108u * n;
    if (area > m->payload_len) { heap_caps_free(au->tracks); heap_caps_free(au); return MPAK_ERR_FMT; }
    buf = payload_read(m, 0, area);
    if (!buf) { heap_caps_free(au->tracks); heap_caps_free(au); return MPAK_ERR_IO; }
    c = (cur_t){ buf, area, 0 };
    cur_u32(&c, &n);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t src = 0;
        uint8_t pad3[3];
        if (cur_u32(&c, &au->tracks[i].id) ||
            cur_bytes(&c, au->tracks[i].title, 96) ||
            cur_u8(&c, &src) ||
            cur_bytes(&c, pad3, 3) ||           /* 3B 填充 */
            cur_u32(&c, &au->tracks[i].duration_s)) {
            heap_caps_free(buf);
            heap_caps_free(au->tracks); heap_caps_free(au);
            return MPAK_ERR_FMT;
        }
        au->tracks[i].title[96] = '\0';
        au->tracks[i].source = src;
    }
    heap_caps_free(buf);
    m->u.audio = au;
    return MPAK_OK;
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

static void free_kind_data(mpak_t *m)
{
    switch (m->kind) {
    case MPAK_KIND_PARTS:
        if (m->parts_tab) heap_caps_free(m->parts_tab);
        m->parts_tab = NULL;
        break;
    case MPAK_KIND_LAYOUT:
        if (m->u.layout) mpak_layout_free(m->u.layout);
        m->u.layout = NULL;
        break;
    case MPAK_KIND_BGMAP:
        if (m->u.bgmap) {
            if (m->u.bgmap->strips) heap_caps_free(m->u.bgmap->strips);
            heap_caps_free(m->u.bgmap);
        }
        m->u.bgmap = NULL;
        break;
    case MPAK_KIND_FONT:
        if (m->u.font) {
            if (m->u.font->glyphs) heap_caps_free(m->u.font->glyphs);
            if (m->u.font->bmp_prefix) heap_caps_free(m->u.font->bmp_prefix);
            heap_caps_free(m->u.font);
        }
        m->u.font = NULL;
        break;
    case MPAK_KIND_AUDIO_META:
        if (m->u.audio) {
            if (m->u.audio->tracks) heap_caps_free(m->u.audio->tracks);
            heap_caps_free(m->u.audio);
        }
        m->u.audio = NULL;
        break;
    default:
        break;
    }
}

static void mpak_layout_free(mpak_layout_t *lt)
{
    if (!lt) return;
    if (lt->expr_names) heap_caps_free(lt->expr_names);
    if (lt->frames)     heap_caps_free(lt->frames);
    if (lt->pieces)     heap_caps_free(lt->pieces);
    heap_caps_free(lt);
}

int mpak_open(mpak_t *m, const char *path, uint64_t expect_hash, uint64_t expect_kind)
{
    if (!m || !path) return MPAK_ERR_ARG;
    memset(m, 0, sizeof *m);

    m->f = fopen(path, "rb");
    if (!m->f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return MPAK_ERR_IO;
    }

    int rc = envelope_check(m, expect_hash, expect_kind);
    if (rc) goto fail;

    /* 回卷到 payload 起点供各 kind 解析 */
    if (fseek(m->f, (long)m->payload_off, SEEK_SET) != 0) { rc = MPAK_ERR_IO; goto fail; }

    switch (m->kind) {
    case MPAK_KIND_PARTS:      rc = parse_parts(m); break;
    case MPAK_KIND_LAYOUT:     rc = parse_layout(m); break;
    case MPAK_KIND_BGMAP:      rc = parse_bgmap(m); break;
    case MPAK_KIND_FONT:       rc = parse_font(m); break;
    case MPAK_KIND_AUDIO_META: rc = parse_audio(m); break;
    default:                   rc = MPAK_ERR_KIND; break;
    }
    if (rc) goto fail;

    ESP_LOGI(TAG, "opened %s kind=%llu hash=%016llx payload=%" PRIu32,
             path, (unsigned long long)m->kind,
             (unsigned long long)m->content_hash, m->payload_len);
    return MPAK_OK;

fail:
    free_kind_data(m);
    if (m->f) fclose(m->f);
    memset(m, 0, sizeof *m);
    return rc;
}

void mpak_close(mpak_t *m)
{
    if (!m) return;
    free_kind_data(m);
    if (m->f) fclose(m->f);
    memset(m, 0, sizeof *m);
}
