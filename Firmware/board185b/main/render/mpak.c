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
#include <unistd.h>      /* pread / fileno：瓦片整块读（契约 §6 只允许整块 pread） */

#include "esp_heap_caps.h"
#include "mp_psram.h"
#include "esp_log.h"
#include "watchdog.h"    /* 瓦片整块读循环喂狗（渲染任务 5s 不喂 = E14 熔断黑屏） */

static const char *TAG = "mpak";

/* 【长任务让帧钩子】默认未注册（NULL）= 行为与从前逐字节一致；上层（合成器）
 * 在地图装载窗口内注册，让全量 CRC32C 这段秒级同步读里渲染任务仍能出帧
 * （真机 185B"推图后人物卡死"根因之一）。声明见 mpak.h。 */
void (*mpak_long_op_hook)(void) = NULL;

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
/* 瓦片几何（契约 docs/ai/map-tiled-format-contract.md §2/§6）          */
/* ------------------------------------------------------------------ */

/* 瓦片边长候选表（**从包内长度反解，不硬编码**；128 优先 = 契约 §2 现档）。
 * 为什么可以反解：逐行口径与瓦片口径的层长度集合不相交 ——
 *   行口径 static = vh*align4(vw*2)（或旧紧打包 vw*vh*2）
 *   瓦片口径 static = ceil(vw/T)*ceil(vh/T)*T*T*2
 * 唯一重合的退化情形是"整层恰好一块瓦片"（vw=vh=T）：此时两种布局逐字节相同，
 * 取哪一种都读到同样的像素，故无歧义风险。
 * 未来服务端换档（64/256/512）时固件无需改代码，只要长度自洽就能认。 */
static const int32_t k_tile_cand[] = { 128, 64, 256, 512, 32, 16 };
#define MPAK_TILE_CAND_N ((int)(sizeof k_tile_cand / sizeof k_tile_cand[0]))

static int32_t tile_grid_axis(int32_t len, int32_t tile)
{
    return (len + tile - 1) / tile;
}

/* 层像素区长度 == gx*gy*T*T*2 → 返回该 T（否则 0）。gx/gy 可空。 */
static int32_t tile_probe_px_len(uint32_t len, int32_t lw, int32_t lh,
                                 int32_t *gx_out, int32_t *gy_out)
{
    if (len == 0 || lw <= 0 || lh <= 0) return 0;
    for (int i = 0; i < MPAK_TILE_CAND_N; i++) {
        int32_t T = k_tile_cand[i];
        int32_t gx = tile_grid_axis(lw, T), gy = tile_grid_axis(lh, T);
        uint64_t need = (uint64_t)gx * (uint64_t)gy * (uint64_t)T * (uint64_t)T * 2u;
        if (need == len) {
            if (gx_out) *gx_out = gx;
            if (gy_out) *gy_out = gy;
            return T;
        }
    }
    return 0;
}

/* 瓦片掩码区长度 = gx*gy*(T*T/8)（T%8==0 才有定义） */
static uint32_t tile_mask_len_of(int32_t T, int32_t gx, int32_t gy)
{
    if (T <= 0 || (T & 7) != 0) return 0;
    return (uint32_t)((uint64_t)gx * (uint64_t)gy * (uint64_t)(T / 8) * (uint64_t)T);
}

/* 单件（PARTS part）瓦片布局探测：像素区 = gx*gy*T*T*2（右/下补 0），
 * 掩码区 = gx*gy*(T*T/8)。extent 是"到下一件起点"的字节数（单件包 = 位图区尾）。
 * 返回 T（命中）或 0；*alpha 报告是否带掩码。 */
static int32_t part_tile_probe(uint32_t extent, int32_t w, int32_t h,
                               int32_t *gx_out, int32_t *gy_out, bool *alpha)
{
    for (int i = 0; i < MPAK_TILE_CAND_N; i++) {
        int32_t T = k_tile_cand[i];
        int32_t gx = tile_grid_axis(w, T), gy = tile_grid_axis(h, T);
        uint32_t pxb = (uint32_t)((uint64_t)gx * gy * T * T * 2u);
        uint32_t mkb = tile_mask_len_of(T, gx, gy);
        if (extent == pxb) {
            if (gx_out) *gx_out = gx;
            if (gy_out) *gy_out = gy;
            if (alpha) *alpha = false;
            return T;
        }
        if (mkb && (extent == pxb + mkb || extent == (uint32_t)align4((size_t)pxb + mkb))) {
            if (gx_out) *gx_out = gx;
            if (gy_out) *gy_out = gy;
            if (alpha) *alpha = true;
            return T;
        }
    }
    return 0;
}

/* 路径 → 文件身份（FNV-1a 32）。与 mpak_open 的自增 file_id 分处两个取值空间：
 * 自增 id 从小整数开始，这里**顶位置 1** ⇒ 两套身份永不相撞（瓦片缓存键的一部分）。
 * 为什么条带要用路径身份而不是每次 open 的句柄身份：条带补读是"按需 open → pread
 * → close"，若身份随句柄走，同一文件两次打开之间缓存全失效（每次补边都要重读整列
 * 瓦片）；用路径身份则"内容不变 ⇒ 缓存有效"，且路径是内容哈希命名的（见 asset_dl）。 */
uint32_t mpak_path_id(const char *path)
{
    uint32_t h = 2166136261u;
    if (!path) return 0;
    for (const char *p = path; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return h | 0x80000000u;
}

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

    /* ══════════════ 尾部 CRC32C：大包策略（2026-10-01）══════════════
     * 【问题】旧实现无条件对 MAGIC..payload 全量算 CRC32C。整图 BGMAP
     * payload=16,924,968 B ⇒ 每次 open 要把 16.9MB 从 TF 读一遍。
     * 真机实测基线（同一台 216 板，SDSPI/FATFS，日志时间戳直读）：
     *   font 287,569B → 259/265/260 ms；526,049B → 512/514 ms；
     *   1,070,401B → 982/1010 ms（`font_lazy_init open` → `mpak: opened` 两条日志之差）
     *   ⇒ 拟合 ≈ 9ms 固定 + 0.94 ms/KB ≈ **1.06 MB/s**
     *   ⇒ 16.9MB 全量 CRC ≈ **16 秒/次 open**（地图每次切换/重载都要付一次）。
     *
     * 【策略】payload_len > MPAK_CRC_SKIP_BYTES(4MB) 时**跳过全量 CRC**：
     *   · 仍然校验：magic / version / kind / 文件长度恰为 40+payload_len+8 /
     *     尾部 zero 字段，并读入 payload 头 4KB 算出头段 CRC32C 作为**指纹**打日志；
     *   · 头段指纹**不能**与尾部 crc 比对（尾部是覆盖全 payload 的**单个**滚动值，
     *     任何前缀/抽样 CRC 都没有可比的存储值）——所以这里诚实地说：它不是校验，
     *     只是给"怀疑包损坏"时留着与导出端/服务端对表用的一枚指纹，同时把尾部
     *     存储 crc 一并打出来供人工核对。
     *
     * 【为什么跳过是安全的】（不是"省事"，是有替代保障）
     *   1. 落盘前已全量校验：net/asset_dl.c `verify_and_commit()`（asset_dl.c:814-846）
     *      在 rename 提交前做 **流式全量 crc32c == 尾部 crc** + `env_hash == manifest hash`
     *      + `total == payload_len+48` + 尾部 zero==0，四道全过才提交，且 rename 原子。
     *      即：TF 上出现过的 .mpk 都已经被逐字节验过一次；open 再验只能发现"提交之后"
     *      的介质损坏/截断，而这由文件长度校验 + 严格的 payload 结构校验兜住。
     *   2. 解析期结构校验很硬：BGMAP 的 static/tile 偏移必须**恰好等于**由
     *      「56+14n 补 4B」「static 长度」「tile 长度」推出的实际布局，否则告警并按实际
     *      布局校正；长度必须是合法集合之一；扩展块 magic+ground 偏移必须自洽。
     *      头部/索引任一字节被破坏 → 偏移恒等式几乎必然崩 → MPAK_ERR_FMT。
     *   3. 阈值 4MB：现网所有非整图包的 payload 都远小于它（PARTS ~237KB、
     *      LAYOUT ≤512KB、FONT 32px ~1.8MB、AUDIO_META ~126KB、窗口 BGMAP ~237KB）
     *      ⇒ **旧包 CRC 行为逐字节不变**（照旧全量算、照旧比对）。
     *   4. 逃生阀：-DMPAK_CRC_SKIP_BYTES=0 即恢复"永远全量 CRC"（取证/恢复用）。 */
    uint8_t chunk[2048];
    bool skip_full_crc = (MPAK_CRC_SKIP_BYTES != 0u) && (payload_len > MPAK_CRC_SKIP_BYTES);
    if (skip_full_crc) {
        uint32_t head_n = payload_len < MPAK_CRC_HEAD_BYTES ? payload_len : MPAK_CRC_HEAD_BYTES;
        if (fseek(m->f, (long)sizeof hdr, SEEK_SET) != 0) return MPAK_ERR_IO;
        uint32_t head_crc = mpak_crc32c(0, hdr, sizeof hdr);
        uint32_t got = 0;
        while (got < head_n) {
            uint32_t n = head_n - got;
            if (n > sizeof chunk) n = (uint32_t)sizeof chunk;
            if (rd_exact(m->f, chunk, n)) return MPAK_ERR_IO;
            head_crc = mpak_crc32c(head_crc, chunk, n);
            got += n;
        }
        uint8_t trailer0[MPAK_TRAILER_LEN];
        if (fseek(m->f, (long)MPAK_HEADER_LEN + (long)payload_len, SEEK_SET) != 0)
            return MPAK_ERR_IO;
        if (rd_exact(m->f, trailer0, sizeof trailer0)) return MPAK_ERR_IO;
        cur_t tc0 = { trailer0, sizeof trailer0, 0 };
        uint32_t crc_stored0 = 0, zero0 = 0;
        cur_u32(&tc0, &crc_stored0);
        cur_u32(&tc0, &zero0);
        if (zero0 != 0) {
            ESP_LOGE(TAG, "trailer zero field != 0");
            return MPAK_ERR_FMT;
        }
        ESP_LOGW(TAG, "大包 payload=%" PRIu32 "B > %uB：跳过全量 CRC32C"
                      "（下载侧 asset_dl 已校验 content_hash+全量 crc）；"
                      "仅信封+头 %" PRIu32 "B 指纹 head_crc=%08" PRIx32
                      " 尾部存储 crc=%08" PRIx32 "（未比对，供人工对表）",
                 payload_len, (unsigned)MPAK_CRC_SKIP_BYTES, head_n, head_crc, crc_stored0);
    } else {
        /* crc32c 流式覆盖 header+payload（MAGIC 至 payload 全量） */
        uint32_t crc = mpak_crc32c(0, hdr, sizeof hdr);
        if (fseek(m->f, (long)sizeof hdr, SEEK_SET) != 0) return MPAK_ERR_IO;
        uint32_t remain = payload_len;
        uint32_t nchunk = 0;
        while (remain) {
            uint32_t n = remain > sizeof chunk ? (uint32_t)sizeof chunk : remain;
            if (rd_exact(m->f, chunk, n)) return MPAK_ERR_IO;
            crc = mpak_crc32c(crc, chunk, n);
            remain -= n;
            /* 【长任务让帧】每 64 块（= 128KB ≈ 120ms @1.06MB/s）给上层一次机会：
             * 上层注册了 mpak_long_op_hook 时，渲染任务能在这段秒级同步 CRC 里
             * 继续出帧（宠物动画不停）。未注册 = 空操作，行为与从前一致。 */
            if (mpak_long_op_hook && ((++nchunk & 63u) == 0)) mpak_long_op_hook();
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

        /* 【PARTS 单件尺寸上限 1024 → 8192（R2 整图配套，2026-10-01）】
         * 契约 §1 的 000010000 里，**带宽 == vw 的两条整幅带**（丘陵段带
         * 2270×260、远景段带 2270×508）是独立 PARTS 小包；旧上限 1024 让它们
         * 全被这里拒（MPAK_ERR_FMT，真机日志实证 "part 1 size 2270x508 invalid"、
         * 且 200000000 那版地图的 1300x392 / 1100x222 带同样一直在被拒）。
         * 契约 §3.2 明确要求「带宽==vw 的条带按世界对齐层整幅绘制」——不放开
         * 这一条，4 条带里就有 2 条永远装不上。
         * 旧包不受影响：判据是上界，w/h ≤ 1024 的件走完全相同的分支（所有
         * 现网宠物件 ≤1024：实测 /tmp 真包 850×222、613×125 等）；此处只放宽
         * 「接受更大」。
         * 内存：位图不进解析器（本解析器只建索引 + 懒读）。消费侧
         * （compositor 的整图条带加载）按窗口缓存切片持有，不整幅常驻；
         * 若将来有消费侧整幅分配（2270×508 → 508×4540 ≈ 2.2MB PSRAM），
         * 分配失败时既有代码路径已是"跳过该条带"降级。
         * 乘积不溢出：w*2 ≤ 16384、w*h ≤ 8192×8192，u32 内（×2 = 134MB）。 */
        if (w == 0 || h == 0 || w > MPAK_MAX_BGMAP_DIM || h > MPAK_MAX_BGMAP_DIM) {
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
    /* 【内部 RAM 腾挪 2026-10-02】extent 推导用的 offset 排序副本：纯粹是
     * qsort/upper_bound 的 CPU 侧中间数组（打开后循环用完即 free），
     * 与 SD 读路径无关（SD 读走 payload_read/psram_alloc）→ PSRAM。
     * 每个包省 n×4B 内部堆；PARTS 包 n 可达数百（包一多就是几 KB）。 */
    uint32_t *offs = mp_psram_malloc(n * sizeof(uint32_t));
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
            /* ── 瓦片口径（契约 §2；2026-10-01）──────────────────────────────
             * 【为什么必须在这里认】整图条带的像素在独立小 PARTS 包里，BGMAP 的
             * bit1 管不到它；包自身唯一的自证就是"长度 == gx*gy*T*T*2（+掩码）"。
             * 认不出的后果不是"少画一条带"，而是 mpak_open 直接 MPAK_ERR_FMT →
             * strip_load_world 整条失败 → 真机表现为"整图只有底图、条带全灭"。
             * 逐行口径先判（老包走原分支，行为逐字节不变），只有逐行两式都不匹配
             * 时才试瓦片口径 ⇒ ≤1024 的老宠物件/老条带完全不受影响。 */
            int32_t tgx = 0, tgy = 0;
            bool t_alpha = false;
            int32_t T = part_tile_probe(ext, tab[i].w, tab[i].h, &tgx, &tgy, &t_alpha);
            if (T) {
                tab[i].tiled      = true;
                tab[i].tile       = (uint16_t)T;
                tab[i].gx         = (uint16_t)tgx;
                tab[i].gy         = (uint16_t)tgy;
                tab[i].has_alpha  = t_alpha;
                /* pixel_bytes/mask_bytes 改写成**盘上真实**字节数：消费侧
                 * （compositor 的条带装载）用它们算掩码区起点与尺寸。 */
                tab[i].pixel_bytes = (uint32_t)((uint64_t)tgx * tgy * T * T * 2u);
                tab[i].mask_bytes  = tile_mask_len_of(T, tgx, tgy);
                ESP_LOGI(TAG, "part %" PRIu32 " %ux%u 为瓦片存储（tile=%d 网格 %dx%d%s）",
                         tab[i].id, tab[i].w, tab[i].h, (int)T, (int)tgx, (int)tgy,
                         t_alpha ? " +掩码" : "");
            } else {
                ESP_LOGE(TAG, "part %" PRIu32 " extent %u matches neither opaque nor alpha"
                              "（逐行 %" PRIu32 "/掩码 %" PRIu32 "；瓦片口径亦不匹配）",
                         tab[i].id, ext, tab[i].pixel_bytes, mask_raw);
                free(offs);
                heap_caps_free(tab);
                return MPAK_ERR_FMT;
            }
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

    /* 【expr_count == 0 合法 2026-10-02】表情是**纸娃娃专属维度**：怪物/NPC 实体包
     * （selector=mob|npc）的 piece 全是 expr_index=255（无 face 变体）→ 导出器按
     * 契约写 expression_count=0。旧校验 `expr_count == 0 → MPAK_ERR_FMT` 会把这类
     * 包**整包拒收**（真机后果：LAYOUT 永远打不开 ⇒ 怪物/NPC 资产"下得下来、用不了"，
     * 也正是当年 NPC 页"只下载不渲染"长期没人发现的原因）。帧区非空校验照旧。 */
    if (frame_count == 0 || frame_count > MPAK_LAYOUT_MAX_FRAMES ||
        expr_count > MPAK_LAYOUT_MAX_EXPR) {
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

    lt->expr_names = NULL;
    if (expr_count > 0) {                       /* 实体包 = 0：不分配、不读名字区 */
        lt->expr_names = psram_alloc((size_t)expr_count * (MPAK_NAME_LEN + 1));
        if (!lt->expr_names) goto nomem;
        /* 读表情名区（44 + e*32） */
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
    bg->ext_off = -1;                 /* <0 = 无扩展块（旧包） */

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

    /* 先卡尺寸上限，再算 px（上限卡死后 vw*vh*2 ≤ 134MB，u32 内安全） */
    if (bg->vw == 0 || bg->vh == 0 ||
        bg->vw > MPAK_MAX_BGMAP_DIM || bg->vh > MPAK_MAX_BGMAP_DIM) {
        ESP_LOGE(TAG, "bgmap %s dim invalid (vw=%u vh=%u max=%u)",
                 bg->map_id, bg->vw, bg->vh, (unsigned)MPAK_MAX_BGMAP_DIM);
        heap_caps_free(bg);
        return MPAK_ERR_FMT;
    }

    uint64_t px = (uint64_t)bg->vw * bg->vh;
    /* 【行距口径 2026-10-01】整图包（R2）的 static/tile 按**行 4B 对齐**写：
     *   static = vh × align4(vw×2)；tile  = vh × align4(vw×2)（RGB565）+ align4(掩码)
     * 旧窗口包是紧打包：static = vw*vh*2。两者在**偶数宽**下数值完全相同
     * （vw 偶 ⇒ vw*2 已 4B 对齐 ⇒ align4(vw*2) == vw*2）——
     * 240×240（480B 行）、180×180（360B 行）、360 档全部命中"两式相等"，
     * 故旧包的接受集合**逐字节不变**；放宽只新增"奇数宽 + 行对齐"这一种以前
     * 必被拒的组合。tile 的旧公式（先算 px*2+掩码再整体补 4B）与编码端公式
     * （RGB 行对齐 + 掩码单独补 4B）同理：偶数宽等值，奇数宽才分叉，两个值都收。 */
    uint32_t stride_al = (uint32_t)align4((size_t)bg->vw * 2u);   /* 行 4B 对齐 */
    uint32_t static_tight   = (uint32_t)(px * 2u);                /* 旧：紧打包 */
    uint32_t static_aligned = (uint32_t)bg->vh * stride_al;       /* 整图：行对齐 */
    uint32_t tile_rgb       = (uint32_t)bg->vh * stride_al;
    uint32_t tile_mask_al   = (uint32_t)align4((size_t)((px + 7u) / 8u));
    uint32_t tile_expect    = (uint32_t)(px * 2u) + (uint32_t)((px + 7u) / 8u);
    tile_expect = (tile_expect + 3u) & ~3u;                       /* 旧公式 */
    uint32_t tile_expect_enc = tile_rgb + tile_mask_al;           /* 编码端公式 */

    /* ── 瓦片口径（契约 docs/ai/map-tiled-format-contract.md §2/§5；2026-10-01）──
     * 【为什么靠长度反解而不是只看 flags bit1】
     *   1. 逐行口径与瓦片口径的层长度集合不相交（见 tile_probe_px_len 注释），
     *      长度本身就是硬证据：静态层长度 == gx*gy*T*T*2 时，它**不可能**是逐行打包；
     *   2. bit1 要等尾扩展块读出来才知道，而扩展块位置又依赖 tile_layer_len 的
     *      口径 —— 先判长度再交叉校验 bit1，才不会拿"逐行口径的偏移"去找瓦片包的
     *      扩展块（两者差着 padding 的倍数）；
     *   3. 服务端若忘置 bit1，固件仍能按瓦片正确读（只告警）；服务端置了 bit1 而
     *      文件其实是逐行打包，则退回逐行读（只告警）—— 两种半升级状态都不黑屏。 */
    int32_t tgx = 0, tgy = 0;
    int32_t tile_t = tile_probe_px_len(bg->static_back_len, bg->vw, bg->vh, &tgx, &tgy);
    uint32_t tiled_rgb_len  = 0;
    uint32_t tiled_mask_len = 0;
    if (tile_t > 0) {
        tiled_rgb_len  = (uint32_t)((uint64_t)tgx * tgy * tile_t * tile_t * 2u);
        tiled_mask_len = tile_mask_len_of(tile_t, tgx, tgy);
    }
    bool tiled = (tile_t > 0);

    /* tile 层长度必须与 static 同一 TILE 口径（不同 = 包自相矛盾，硬失败：
     * 半瓦片半逐行的包没有正确读法，静默猜只会画出错位图）。 */
    if (tiled && bg->tile_layer_len != 0 &&
        bg->tile_layer_len != tiled_rgb_len &&
        bg->tile_layer_len != tiled_rgb_len + tiled_mask_len) {
        ESP_LOGE(TAG, "bgmap %s 瓦片口径自相矛盾：static=%" PRIu32 "B(→tile=%d 网格 %dx%d)"
                      " 但 tile_layer=%" PRIu32 "B（期望 %" PRIu32 "±掩码 %" PRIu32 "）",
                 bg->map_id, bg->static_back_len, (int)tile_t, (int)tgx, (int)tgy,
                 bg->tile_layer_len, tiled_rgb_len, tiled_mask_len);
        heap_caps_free(bg);
        return MPAK_ERR_FMT;
    }

    if ((bg->static_back_len != static_tight &&
         bg->static_back_len != static_aligned &&
         bg->static_back_len != tiled_rgb_len) ||
        (bg->tile_layer_len != 0 &&
         bg->tile_layer_len != tile_expect &&
         bg->tile_layer_len != tile_expect_enc &&
         bg->tile_layer_len != tiled_rgb_len &&
         bg->tile_layer_len != tiled_rgb_len + tiled_mask_len) ||
        bg->strip_count > MPAK_BGMAP_MAX_STRIPS) {
        ESP_LOGE(TAG, "bgmap %s geometry invalid (vw=%u vh=%u static=%" PRIu32
                      " tile=%" PRIu32 ")",
                 bg->map_id, bg->vw, bg->vh, bg->static_back_len, bg->tile_layer_len);
        heap_caps_free(bg);
        return MPAK_ERR_FMT;
    }

    bg->tiled = tiled;
    if (tiled) {
        bg->tile            = tile_t;
        bg->gx              = tgx;
        bg->gy              = tgy;
        bg->tile_px_bytes   = tiled_rgb_len;
        bg->tile_mask_bytes = (bg->tile_layer_len > tiled_rgb_len)
                              ? (bg->tile_layer_len - tiled_rgb_len) : 0;
    }

    /* 【BGMAP 层偏移自愈 2026-09-27】真凶级布局 bug：BgmapPackWriter 估 headerLen 时
     * 按 **16B/条** 计（`input.Strips.Count * 16`），但每条实际只写 14B
     * （part_ref u64 + y i16 + speed i16 + rx u8 + blend u8）。于是声明的
     * static_back_off/tile_layer_off 比真实数据位置大 8B：
     *   · static_back 整体左移 4 源像素（每行首 4 像素来自下一行行尾）；
     *   · tile_layer 颜色同样移 4 像素，而**掩码**是从 tile_off+px 读的 —— 掩码是
     *     tight 位打包、行距 240 bit，8B = 64 bit ⇒ 掩码整体错位 64 像素（含跨行
     *     回卷）⇒ 掩码与颜色完全对不上：真机表现为地图对象错位、块状黑斑、边缘
     *     锯齿状竖缝（用户"地图没渲染好"照片）。
     * 判据：真实布局必然是「56B 头 + 14B×条数（补 4B 对齐）+ static + tile」，
     * 声明值与之不符即自愈（对已修好的新包是 no-op）。 */
    uint32_t strips_end = (56u + (uint32_t)bg->strip_count * 14u + 3u) & ~3u;
    if (bg->static_back_off != strips_end ||
        bg->tile_layer_off != strips_end + bg->static_back_len ||
        strips_end + bg->static_back_len + bg->tile_layer_len > m->payload_len) {
        ESP_LOGW(TAG, "BGMAP %s 层偏移不符实际布局（声明 static_off=%u tile_off=%u，"
                      "实际 %u/%u，payload=%u）→ 已按实际布局校正",
                 bg->map_id, (unsigned)bg->static_back_off, (unsigned)bg->tile_layer_off,
                 (unsigned)strips_end, (unsigned)(strips_end + bg->static_back_len),
                 (unsigned)m->payload_len);
        bg->static_back_off = strips_end;
        bg->tile_layer_off   = strips_end + bg->static_back_len;
    }
    /* tile 掩码区起点（**用自愈后的 tile_layer_off**）：瓦片口径 = 像素区之后
     * （像素区 = gx*gy*T*T*2，右/下补 0 也算在区内），行口径 = vh*align4(vw*2)。
     * 两个口径的数值**不可互换**（差着瓦片 padding 的倍数），所以由 mpak 统一算好
     * 交给 compositor，避免第二处再推一遍公式推错。 */
    bg->tile_mask_off = bg->tiled
        ? bg->tile_layer_off + bg->tile_px_bytes
        : bg->tile_layer_off + (uint32_t)bg->vh * stride_al;

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

    /* ══════════ 整图扩展块（R2，可选；2026-10-01）══════════
     * 位置：ext_off = align4(tile_layer_off + tile_layer_len)（**用自愈后的**偏移）。
     * 旧包此处 ext_off 已 ≥ payload_len（旧 payload 正好在 tile 末尾结束，最多几个
     * 4B 填充字节）⇒ 直接判"无扩展块"，full_map=false，一切照旧；
     * 即便尾部填充里凑巧出现 magic（概率 ≈2^-32，且填充通常为 0），后面 ground 偏移
     * 自洽性校验也会把它否掉——不会把旧包误判成整图包。 */
    {
        uint32_t ext_off = (uint32_t)align4((size_t)bg->tile_layer_off + bg->tile_layer_len);
        if (ext_off <= m->payload_len &&
            m->payload_len - ext_off >= MPAK_BGMAP_EXT_HDR_LEN) {
            uint8_t *eb = payload_read(m, ext_off, MPAK_BGMAP_EXT_HDR_LEN);
            if (!eb) { if (bg->strips) heap_caps_free(bg->strips);
                       heap_caps_free(bg); return MPAK_ERR_IO; }
            cur_t ec = { eb, MPAK_BGMAP_EXT_HDR_LEN, 0 };
            uint32_t magic = 0, glen = 0, goff = 0, eflags = 0;
            cur_u32(&ec, &magic);
            cur_u32(&ec, &glen);
            cur_u32(&ec, &goff);
            cur_u32(&ec, &eflags);
            heap_caps_free(eb);

            if (magic == MPAK_BGMAP_EXT_MAGIC) {
                /* magic 命中 = 新格式。字段必须自洽，否则包确实坏了 → 硬失败
                 * （整图包 16.9MB，下载侧已全量校验，出现坏字段只可能是介质损坏）。 */
                uint64_t g_end = (uint64_t)goff + glen;
                if (glen == 0 || glen != (uint32_t)bg->vw * 2u ||
                    goff < ext_off + MPAK_BGMAP_EXT_HDR_LEN ||
                    g_end > m->payload_len) {
                    ESP_LOGE(TAG, "bgmap %s 扩展块非法（ground_len=%" PRIu32
                                  " 期望=%u ground_off=%" PRIu32 " ext_off=%" PRIu32
                                  " payload=%" PRIu32 ")",
                             bg->map_id, glen, (unsigned)((uint32_t)bg->vw * 2u), goff,
                             ext_off, m->payload_len);
                    if (bg->strips) heap_caps_free(bg->strips);
                    heap_caps_free(bg);
                    return MPAK_ERR_FMT;
                }
                bg->ext_off    = (int32_t)ext_off;
                bg->ext_flags  = eflags;
                bg->ground_off = goff;
                bg->ground_len = glen;
                bg->full_map   = (eflags & MPAK_BGMAP_FLAG_FULL_MAP) != 0;

                /* ── 瓦片标志交叉校验（契约 §1：flags bit1 = TILED）──────────────
                 * 布局口径以"层长度恒等式"为准（见上面 tile_probe_px_len），bit1 只做
                 * 一致性告警：两种半升级状态（服务端忘置位 / 置了位但文件还是逐行）
                 * 都要能读出正确画面，绝不能黑屏。 */
                bool flag_tiled = (eflags & MPAK_BGMAP_FLAG_TILED) != 0;
                if (flag_tiled && !bg->tiled) {
                    ESP_LOGW(TAG, "bgmap %s flags bit1(TILED)=1 但层长度是逐行口径"
                                  "（static=%" PRIu32 "B）→ 按逐行读（服务端半升级）",
                             bg->map_id, bg->static_back_len);
                } else if (!flag_tiled && bg->tiled) {
                    ESP_LOGW(TAG, "bgmap %s 层长度=瓦片口径（tile=%d 网格 %dx%d）但 flags"
                                  " bit1=0 → 仍按瓦片读（服务端忘置位）",
                             bg->map_id, (int)bg->tile, (int)bg->gx, (int)bg->gy);
                }

                /* 地面表缓存取舍（契约 §3.1 允许"整块读"或"按需 2B 读"）：
                 * 选**open 时整块缓存**，理由：
                 *  1) 调用频次：地面线在相机每次移动/角色每次换地图都要重算，
                 *     按需读 = 每列一次 TF fseek+fread（真机随机读 ≈ms 级），
                 *     而缓存后是一次数组访存；
                 *  2) 体量可控：上限 MPAK_BGMAP_GROUND_CACHE_MAX = 16KB
                 *     （8192 列 × 2B），相对 1.2MB PSRAM 预算可忽略；
                 *  3) 失败面收敛：读失败发生在 open（可判可拒），而不是渲染任务
                 *     深处的每个调用点；LE→host 也只需转一次。
                 * 超过 16KB（vw > 8192，已被维度上限挡住）时不缓存，
                 * mpak_bgmap_ground_y 自动退回"按需读 2B"路径。 */
                if (glen <= MPAK_BGMAP_GROUND_CACHE_MAX) {
                    uint8_t *raw = payload_read(m, goff, glen);
                    if (!raw) { if (bg->strips) heap_caps_free(bg->strips);
                                heap_caps_free(bg); return MPAK_ERR_IO; }
                    uint16_t *tbl = psram_alloc(glen);
                    if (tbl) {
                        uint32_t n = glen / 2u;
                        for (uint32_t i = 0; i < n; i++)   /* u16 小端 → host 序 */
                            tbl[i] = (uint16_t)(raw[i * 2u] | ((uint16_t)raw[i * 2u + 1u] << 8));
                        bg->ground = tbl;
                    } else {
                        ESP_LOGW(TAG, "地面表缓存分配失败（%uB）→ 退回按需读", glen);
                    }
                    heap_caps_free(raw);
                }
                if (!bg->full_map)
                    ESP_LOGW(TAG, "bgmap %s 有扩展块但 flags=0x%08" PRIx32
                                  "（bit0=0）→ 不按整图包处理", bg->map_id, eflags);
            }
        }
    }

    /* 装载日志（验收锚点）：整图包 `full_map=1 ground=NNNNB`；旧包 `full_map=0 ground=0B`。
     * 瓦片包额外打 `tiled=1 tile=128 网格 18x15`（真机判读"这条包到底走哪条路"）。 */
    ESP_LOGI(TAG, "bgmap %s vw=%u vh=%u full_map=%d ground=%" PRIu32 "B"
                  " ext_off=%" PRId32 " static=%" PRIu32 "@%" PRIu32
                  " tile=%" PRIu32 "@%" PRIu32 " strips=%" PRIu32,
             bg->map_id, bg->vw, bg->vh, bg->full_map ? 1 : 0, bg->ground_len,
             bg->ext_off, bg->static_back_len, bg->static_back_off,
             bg->tile_layer_len, bg->tile_layer_off, bg->strip_count);
    if (bg->tiled) {
        ESP_LOGI(TAG, "bgmap %s 瓦片布局：tile=%d 网格 %dx%d 像素区 %" PRIu32 "B/层"
                      "（行口径同图需 %" PRIu32 "B）+ 掩码 %" PRIu32 "B@%" PRIu32
                      " flags=0x%08" PRIx32,
                 bg->map_id, (int)bg->tile, (int)bg->gx, (int)bg->gy, bg->tile_px_bytes,
                 (uint32_t)bg->vh * stride_al, bg->tile_mask_bytes, bg->tile_mask_off,
                 bg->ext_flags);
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
/* BGMAP 分块读（整图 R2，契约 §3.1）                                   */
/*                                                                     */
/* 三件事必须钉死，否则"看起来对、图上错位"：                            */
/*  1) 源行距按**包内真实布局**反推，不是凭窗口宽：static 可能紧打包     */
/*     也可能行 4B 对齐；tile RGB 区固定行 4B 对齐（编码端 Align4）。    */
/*  2) tile 掩码是**全局逐行 tight 位打包**（bit = y*vw + x，MSB 先出）， */
/*     行与行之间**不**做字节对齐 ⇒ vw%8 != 0 时一个字节会同时装下上一行 */
/*     结尾和下一行开头。按"整块线性"或"每行各自对齐"都会错位；这里按    */
/*     全局 bit 号算字节区间，天然跨行正确。                             */
/*  3) 世界矩形 → 文件偏移用 **vw**（整图宽）算行距，跟窗口宽 w 无关；    */
/*     窗口越界部分补 0（调用方自己按 vw/vh 裁剪），不报错。             */
/*                                                                     */
/* ⚠️ 上面 2) 只适用于**逐行包**。瓦片包（tiled，契约 §3）的掩码是       */
/*    **块内逐行字节对齐**（块内第 y 行占 [y*T/8, y*T/8+T/8)），两套位序   */
/*    不可混用 —— 分流点在 mpak_bgmap_read_*_rect，见下面的瓦片读模块。  */
/* ------------------------------------------------------------------ */

/* static_back 源行距（字节）：由 static_back_len 反推实际布局（见 parse_bgmap）。
 * 偶数宽两式等值（旧包）→ 结果与旧口径一致；奇数宽才分叉。 */
static uint32_t bg_static_stride(const mpak_bgmap_t *bg)
{
    uint32_t al = (uint32_t)align4((size_t)bg->vw * 2u);
    if (bg->static_back_len == (uint32_t)bg->vh * al) return al;   /* 行 4B 对齐（整图） */
    return (uint32_t)bg->vw * 2u;                                  /* 旧：紧打包 */
}

/* tile 层 RGB565 源行距：编码端固定 Align4(vw*2)（PartPackWriter.EncodeCore） */
static uint32_t bg_tile_stride(const mpak_bgmap_t *bg)
{
    return (uint32_t)align4((size_t)bg->vw * 2u);
}

/* 掩码区首字节（payload 相对）= tile RGB 区之后 */
static uint32_t bg_tile_mask_off(const mpak_bgmap_t *bg)
{
    return bg->tile_layer_off + (uint32_t)bg->vh * bg_tile_stride(bg);
}

static int bg_rect_args(const mpak_t *m, int32_t w, int32_t h,
                        const void *dst, int32_t dst_stride_px)
{
    if (!m || !m->u.bgmap || !dst) return MPAK_ERR_ARG;
    if (w < 0 || h < 0 || dst_stride_px < w) return MPAK_ERR_ARG;
    if (w > (int32_t)MPAK_MAX_BGMAP_DIM || h > (int32_t)MPAK_MAX_BGMAP_DIM)
        return MPAK_ERR_ARG;
    return MPAK_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 瓦片(tile)整块读 + PSRAM 瓦片缓存（契约 §2/§6；2026-10-01）
 *
 * 【要解决的问题（真机实测根因）】整图窗口填充原来按"跨行距逐行读"：static 层
 * 行距 4540B 而每行只需 672B ⇒ 读效率 15%，SD 顺序吞吐 1336 KB/s 只跑出
 * ~130KB/s，一次装载 8.5~17.2s。瓦片布局下同一窗口只需读"覆盖它的那几块"
 * （288×288 窗口 @128 瓦片 ≈ 9~16 块 ≈ 288~512KB），且**每块一次 pread**。
 *
 * 【缓存策略】像素块与掩码块各一张定容槽表，全局 LRU、按需懒分配：
 *   · 只读"被请求到的那一块" —— 窗口/条带没覆盖到的瓦片既不读也不驻留
 *     （条带未进视野时连窗口缓存都不分配，见 compositor 的 strip_wc_alloc）；
 *   · 命中 = 纯 PSRAM memcpy，一次 SD 命令都不发（契约 §6 硬要求）；
 *   · 槽位缓冲按需分配（第一张瓦片读进来时才 malloc 32KB/2KB），所以旧包
 *     （非瓦片）与"没有一条带可见"的场景下这套缓存**一个字节都不占**。
 * 【容量取舍】16 槽 = 512KB 像素 + 32KB 掩码（槽数可用 -D 覆盖）。
 *   为什么不是把所有可见层都装下：288×288 世界窗口 @128 瓦片对**每一层**
 *   就要 9~16 块，static + tile 两层叠加已 >16 ⇒ 再大也装不下"所有层"，只是
 *   线性吃 PSRAM。16 槽的实际效果是"最近走过的那一层基本全命中"，跨层切换时
 *   按 LRU 补最近的一列/一行块（每次边界跨越约 1 列 = 3~4 块 ≈ 128KB）。
 * 【线程契约】只在渲染任务（持 rc_lock 的合成/同步路径）里访问，无锁。
 * ══════════════════════════════════════════════════════════════════════════ */

#ifndef MPAK_TILE_PX_SLOTS
#define MPAK_TILE_PX_SLOTS 16       /* 像素块槽数（每槽 tile*tile*2 = 32KB @128） */
#endif
#ifndef MPAK_TILE_CV_SLOTS
#define MPAK_TILE_CV_SLOTS 16       /* 掩码块槽数（每槽 tile*tile/8 = 2KB @128）  */
#endif

typedef struct {
    uint8_t *data;                  /* 槽缓冲（懒分配；PSRAM） */
    size_t   cap;                   /* 已分配字节数（块大小变了要重分配） */
    uint32_t fid;                   /* 文件身份（mpak_t.file_id / mpak_path_id） */
    uint32_t base;                  /* 层基址（同一文件里的不同层不串） */
    uint32_t idx;                   /* 瓦片序号（行主序 ty*gx+tx） */
    uint32_t stamp;                 /* LRU 时间戳（单调递增） */
    bool     valid;
} mpak_tile_slot_t;

static mpak_tile_slot_t s_ts_px[MPAK_TILE_PX_SLOTS];
static mpak_tile_slot_t s_ts_cv[MPAK_TILE_CV_SLOTS];
static uint32_t s_ts_stamp;         /* LRU 时钟 */
static uint32_t s_ts_blocks;        /* 统计：实际整块读次数 */
static uint32_t s_ts_hits;          /* 统计：命中次数 */
static uint32_t s_ts_bytes;         /* 统计：整块读字节数 */

void mpak_tile_stat_reset(void) { s_ts_blocks = s_ts_hits = s_ts_bytes = 0; }

void mpak_tile_stat_get(uint32_t *blocks, uint32_t *hits, uint32_t *bytes)
{
    if (blocks) *blocks = s_ts_blocks;
    if (hits)   *hits   = s_ts_hits;
    if (bytes)  *bytes  = s_ts_bytes;
}

void mpak_tile_cache_flush(void)
{
    for (int i = 0; i < MPAK_TILE_PX_SLOTS; i++) s_ts_px[i].valid = false;
    for (int i = 0; i < MPAK_TILE_CV_SLOTS; i++) s_ts_cv[i].valid = false;
    /* 只清键不 free：缓冲下次直接复用（反复 malloc/free 大块是 PSRAM 碎片源） */
}

void mpak_tile_src_init(mpak_tile_src_t *s, uint32_t file_id, int fd, uint32_t base_off,
                        int32_t lw, int32_t lh, int32_t tile)
{
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->file_id = file_id;
    s->fd      = fd;
    s->base_off = base_off;
    s->lw = lw;
    s->lh = lh;
    s->tile = tile;
    if (tile > 0 && lw > 0 && lh > 0) {
        s->gx = tile_grid_axis(lw, tile);
        s->gy = tile_grid_axis(lh, tile);
    }
}

/* 取一块（idx）到缓存并返回块数据指针；未命中时整块 pread（**唯一**允许的读法）。
 * 返回 NULL = 读失败/无槽可分配（调用方报 IO/NOMEM，不静默画错）。 */
static const uint8_t *ts_fetch(mpak_tile_slot_t *tab, int n, size_t block,
                               const mpak_tile_src_t *s, uint32_t idx)
{
    if (s->fd < 0 || block == 0) return NULL;
    uint32_t abs = s->base_off + (uint32_t)((uint64_t)idx * block);

    int slot = -1;
    for (int i = 0; i < n; i++) {
        mpak_tile_slot_t *t = &tab[i];
        if (t->valid && t->fid == s->file_id && t->base == s->base_off && t->idx == idx) {
            t->stamp = ++s_ts_stamp;
            s_ts_hits++;
            return t->data;
        }
        if (!t->valid && slot < 0) slot = i;                 /* 优先空槽 */
    }
    /* 全满：淘汰最久未用（LRU） */
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < n; i++)
            if (tab[i].stamp < tab[slot].stamp) slot = i;
    }
    mpak_tile_slot_t *t = &tab[slot];
    if (!t->data || t->cap < block) {
        uint8_t *p = heap_caps_malloc(block, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            ESP_LOGE(TAG, "瓦片缓存槽分配失败（%zuB，PSRAM 空闲 %uB）",
                     block, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            t->valid = false;
            return NULL;
        }
        if (t->data) heap_caps_free(t->data);                /* 块大小换档（tile 变了） */
        t->data = p;
        t->cap  = block;
    }
    /* 每块喂一次狗：一次装载要读几十块，单块 32KB 在 SD 上 ≈ 24ms，
     * 累加可超渲染任务的 5s TWDT 预算（历史事故 E14 熔断黑屏）。 */
    watchdog_kick();
    ssize_t got = pread(s->fd, t->data, block, (off_t)abs);
    if (got != (ssize_t)block) {
        ESP_LOGE(TAG, "瓦片整块读失败 fd=%d off=%u len=%zu got=%d",
                 s->fd, (unsigned)abs, block, (int)got);
        t->valid = false;
        return NULL;
    }
    t->valid = true;
    t->fid = s->file_id;
    t->base = s->base_off;
    t->idx = idx;
    t->stamp = ++s_ts_stamp;
    s_ts_blocks++;
    s_ts_bytes += (uint32_t)block;
    return t->data;
}

/* 层上 (x,y) 所在瓦片的块数据（像素/掩码共用；块大小由调用方给） */
static const uint8_t *ts_tile_at(mpak_tile_slot_t *tab, int n, size_t block,
                                 const mpak_tile_src_t *s, int32_t tx, int32_t ty)
{
    if (tx < 0 || ty < 0 || tx >= s->gx || ty >= s->gy) return NULL;
    return ts_fetch(tab, n, block, s, (uint32_t)(ty * s->gx + tx));
}

/* 契约 §6：tx=x/TILE, ty=y/TILE, in_x=x%TILE, in_y=y%TILE；
 * tile_off = px_off + (ty*gx+tx)*TILE*TILE*2；byte_off = tile_off + (in_y*TILE+in_x)*2。
 * 掩码：tile_off_cov + (ty*gx+tx)*2048 + in_y*(TILE/8)，位序 MSB-first（x=0 在最高位）。 */
int mpak_tile_read_px(const mpak_tile_src_t *s, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint16_t *dst, int32_t dst_stride_px)
{
    if (!s || !dst) return MPAK_ERR_ARG;
    if (w < 0 || h < 0 || dst_stride_px < w) return MPAK_ERR_ARG;
    if (w == 0 || h == 0) return MPAK_OK;
    if (s->tile <= 0 || s->gx <= 0 || s->gy <= 0 || s->fd < 0) return MPAK_ERR_FMT;
    const int32_t  T  = s->tile;
    const size_t  blk = (size_t)T * (size_t)T * 2u;
    for (int32_t dy = 0; dy < h; dy++) {
        const int32_t wy = y + dy;
        uint16_t *drow = dst + (size_t)dy * dst_stride_px;
        if (wy < 0 || wy >= s->lh) { memset(drow, 0, (size_t)w * 2u); continue; }
        const int32_t ty = wy / T, iny = wy % T;
        int32_t dx = 0;
        while (dx < w) {
            const int32_t wx = x + dx;
            if (wx < 0 || wx >= s->lw) { drow[dx] = 0; dx++; continue; }  /* 层外：补 0 */
            const int32_t tx = wx / T, inx = wx % T;
            int32_t run = T - inx;            /* 本段不跨块：wx+run ≤ (tx+1)*T */
            if (run > w - dx) run = w - dx;
            const uint8_t *b = ts_tile_at(s_ts_px, MPAK_TILE_PX_SLOTS, blk, s, tx, ty);
            if (!b) return MPAK_ERR_IO;
            memcpy(drow + dx, b + ((size_t)iny * T + inx) * 2u, (size_t)run * 2u);
            dx += run;
        }
    }
    return MPAK_OK;
}

int mpak_tile_read_mask(const mpak_tile_src_t *s, int32_t x, int32_t y, int32_t w, int32_t h,
                        uint8_t *dst, int32_t dst_stride_px)
{
    if (!s || !dst) return MPAK_ERR_ARG;
    if (w < 0 || h < 0 || dst_stride_px < w) return MPAK_ERR_ARG;
    if (w == 0 || h == 0) return MPAK_OK;
    if (s->tile <= 0 || s->gx <= 0 || s->gy <= 0 || s->fd < 0) return MPAK_ERR_FMT;
    const int32_t T = s->tile;
    if ((T & 7) != 0) return MPAK_ERR_FMT;           /* 块内行按 T/8 字节对齐 ⇒ T 必须 8 的倍数 */
    const size_t blk = (size_t)T * (size_t)T / 8u;
    const int32_t rb = T / 8;                        /* 块内每行字节 */
    for (int32_t dy = 0; dy < h; dy++) {
        const int32_t wy = y + dy;
        uint8_t *drow = dst + (size_t)dy * dst_stride_px;
        if (wy < 0 || wy >= s->lh) { memset(drow, 0, (size_t)w); continue; }
        const int32_t ty = wy / T, iny = wy % T;
        int32_t dx = 0;
        while (dx < w) {
            const int32_t wx = x + dx;
            if (wx < 0 || wx >= s->lw) { drow[dx] = 0; dx++; continue; }
            const int32_t tx = wx / T, inx = wx % T;
            int32_t run = T - inx;
            if (run > w - dx) run = w - dx;
            const uint8_t *b = ts_tile_at(s_ts_cv, MPAK_TILE_CV_SLOTS, blk, s, tx, ty);
            if (!b) return MPAK_ERR_IO;
            const uint8_t *brow = b + (size_t)iny * rb;
            for (int32_t i = 0; i < run; i++) {
                const int32_t bx = inx + i;
                drow[dx + i] = (uint8_t)((brow[bx >> 3] >> (7 - (bx & 7))) & 1u);
            }
            dx += run;
        }
    }
    return MPAK_OK;
}

/* 预取：只把"被请求矩形覆盖到的块"拉进缓存（不碰矩形外的块）。
 * 用途：给 `::shot`/真机排障做"窗口预热"对比；装载期**不**单跑一遍——
 * 见 cam_scene_load 的说明（缓存装不下两层窗口，单独预取反而多读一遍 SD）。 */
static int ts_prefetch(mpak_tile_slot_t *tab, int n, size_t block,
                       const mpak_tile_src_t *s, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!s) return MPAK_ERR_ARG;
    if (w <= 0 || h <= 0) return MPAK_OK;
    if (s->tile <= 0 || s->gx <= 0 || s->gy <= 0 || s->fd < 0) return MPAK_ERR_FMT;
    const int32_t T = s->tile;
    int32_t tx0 = x / T, tx1 = (x + w - 1) / T;
    int32_t ty0 = y / T, ty1 = (y + h - 1) / T;
    if (x < 0) tx0 = 0;
    if (y < 0) ty0 = 0;
    if (tx1 >= s->gx) tx1 = s->gx - 1;
    if (ty1 >= s->gy) ty1 = s->gy - 1;
    for (int32_t ty = ty0; ty <= ty1; ty++)
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            if (!ts_tile_at(tab, n, block, s, tx, ty)) return MPAK_ERR_IO;
        }
    return MPAK_OK;
}

int mpak_tile_prefetch(const mpak_tile_src_t *s, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!s) return MPAK_ERR_ARG;
    if (s->tile <= 0) return MPAK_ERR_FMT;
    return ts_prefetch(s_ts_px, MPAK_TILE_PX_SLOTS, (size_t)s->tile * s->tile * 2u, s, x, y, w, h);
}

/* BGMAP 层 → 瓦片源（fd 走 FILE* 的 fd：pread 不改文件位置，与 fseek/fread 共存安全）。
 * ⚠️ layer_off 是 **payload 相对**（bg->static_back_off 等的口径），而
 * mpak_tile_src_t.base_off 要的是**绝对文件偏移** ⇒ 这里必须加 m->payload_off。
 * （漏加 = 每次整块读都前移 40B = 画面整体错位 + 头部读到 map_id 的 0 字节；
 * host 对拍 tools/test_tiled_bgmap.c 的 ② 就是专门钉这一条的。） */
static int bg_tiled_src(const mpak_t *m, uint32_t layer_off, mpak_tile_src_t *s)
{
    const mpak_bgmap_t *bg = m->u.bgmap;
    int fd = m->f ? fileno(m->f) : -1;
    if (fd < 0) return MPAK_ERR_IO;
    mpak_tile_src_init(s, m->file_id, fd, m->payload_off + layer_off,
                       bg->vw, bg->vh, bg->tile);
    return (s->tile > 0 && s->gx > 0 && s->gy > 0) ? MPAK_OK : MPAK_ERR_FMT;
}

/* 瓦片口径的层矩形读（static / tile 像素层共用） */
static int bg_tiled_read_rgb(const mpak_t *m, uint32_t layer_off,
                             int32_t x, int32_t y, int32_t w, int32_t h,
                             uint16_t *dst, int32_t dst_stride_px)
{
    mpak_tile_src_t s;
    int rc = bg_tiled_src(m, layer_off, &s);
    if (rc) return rc;
    return mpak_tile_read_px(&s, x, y, w, h, dst, dst_stride_px);
}

int mpak_bgmap_prefetch_static(const mpak_t *m, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!m || !m->u.bgmap) return MPAK_ERR_ARG;
    const mpak_bgmap_t *bg = m->u.bgmap;
    if (!bg->tiled) return MPAK_OK;                    /* 旧包：无瓦片概念，no-op */
    mpak_tile_src_t s;
    int rc = bg_tiled_src(m, bg->static_back_off, &s);
    if (rc) return rc;
    return ts_prefetch(s_ts_px, MPAK_TILE_PX_SLOTS, (size_t)s.tile * s.tile * 2u, &s, x, y, w, h);
}

int mpak_bgmap_prefetch_tile(const mpak_t *m, int32_t x, int32_t y, int32_t w, int32_t h,
                             bool with_mask)
{
    if (!m || !m->u.bgmap) return MPAK_ERR_ARG;
    const mpak_bgmap_t *bg = m->u.bgmap;
    if (!bg->tiled) return MPAK_OK;
    mpak_tile_src_t s;
    int rc = bg_tiled_src(m, bg->tile_layer_off, &s);
    if (rc) return rc;
    rc = ts_prefetch(s_ts_px, MPAK_TILE_PX_SLOTS, (size_t)s.tile * s.tile * 2u, &s, x, y, w, h);
    if (rc || !with_mask || bg->tile_mask_bytes == 0) return rc;
    rc = bg_tiled_src(m, bg->tile_mask_off, &s);
    if (rc) return rc;
    return ts_prefetch(s_ts_cv, MPAK_TILE_CV_SLOTS, (size_t)s.tile * s.tile / 8u, &s, x, y, w, h);
}


/* 通用 RGB565 世界矩形读（static 与 tile 共用；stride_b = 源行距字节） */
/* 整图窗口读的预读块（PSRAM 单例，懒分配）。64KB 在"命令数下降"与
 * "读放大浪费"之间取平衡：static 层 stride 4540 ⇒ 覆盖 14 行；条带层
 * stride 1700 ⇒ 覆盖 38 行。 */
#define BG_RA_CAP_DEFAULT (64u * 1024u)
#define BG_RA_CAP_MIN     (8u * 1024u)
static uint8_t *s_bg_ra;
static size_t   s_bg_ra_cap;

static uint8_t *bg_ra_buf(void)
{
    if (s_bg_ra) return s_bg_ra;
    size_t cap = BG_RA_CAP_DEFAULT;
    uint8_t *p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) { cap = BG_RA_CAP_MIN; p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
    if (!p) return NULL;
    s_bg_ra = p;
    s_bg_ra_cap = cap;
    return s_bg_ra;
}
static size_t bg_ra_cap(void) { return s_bg_ra_cap; }

static int bg_read_rect_rgb(const mpak_t *m, uint32_t layer_off, uint32_t layer_len,
                            uint32_t stride_b, int32_t x, int32_t y, int32_t w, int32_t h,
                            uint16_t *dst, int32_t dst_stride_px)
{
    const mpak_bgmap_t *bg = m->u.bgmap;
    const int32_t vw = (int32_t)bg->vw, vh = (int32_t)bg->vh;

    if ((uint64_t)layer_len < (uint64_t)vh * stride_b) return MPAK_ERR_FMT;

    /* 目标整体清零：越界区域 = 0（不报错，调用方按 vw/vh 自行裁剪） */
    for (int32_t dy = 0; dy < h; dy++)
        memset(dst + (size_t)dy * dst_stride_px, 0, (size_t)w * 2u);
    if (w == 0 || h == 0) return MPAK_OK;

    /* 世界矩形 ∩ [0,vw)×[0,vh)（64 位中间量，避免 x+w 溢出） */
    const int64_t wx0 = x, wy0 = y;
    const int64_t cx0 = wx0 < 0 ? 0 : (wx0 > vw ? vw : wx0);
    const int64_t cx1 = (wx0 + w) > vw ? vw : (wx0 + w);
    const int64_t cy0 = wy0 < 0 ? 0 : (wy0 > vh ? vh : wy0);
    const int64_t cy1 = (wy0 + h) > vh ? vh : (wy0 + h);
    if (cx0 >= cx1 || cy0 >= cy1) return MPAK_OK;      /* 完全越界 → 全 0 */

    const uint32_t row_bytes = (uint32_t)(cx1 - cx0) * 2u;
    const int32_t  row_px    = (int32_t)(cx1 - cx0);
    const int32_t  dy0       = (int32_t)(cy0 - wy0);
    const int32_t  dx0       = (int32_t)(cx0 - wx0);

    /* 快路径：源相邻行首尾相接（行距 == 需求宽度）且目标行也连续
     * ⇒ 一次 mpak_read_at 读完，避免 h 次 seek+fread（整幅/整列带读时很值）。 */
    if (stride_b == row_bytes && dst_stride_px == row_px) {
        uint8_t *d0 = (uint8_t *)(dst + (size_t)dy0 * dst_stride_px + dx0);
        return mpak_read_at((mpak_t *)m,
                            m->payload_off + layer_off + (uint32_t)cy0 * stride_b, d0,
                            (size_t)(cy1 - cy0) * row_bytes);
    }

    /* ── 慢路径：行跨度 ≠ 需求宽度（窗口读）─────────────────────────────
     * 【2026-10-01 真机性能根因】原实现"每行一次 fseek+fread"：整图装载时
     * `窗口读 3144 行 / 1112 KB（9409 ms）`＝每行 3ms（每行才 672~800 B）。
     * 开销不在带宽（1-bit SDMMC@20MHz 理论 2.5MB/s），而在**每次读的
     * FATFS+SD 命令往返**（fseek → 簇链定位 → 2 个 512B 扇区事务）。
     * 修法：**预读块**——一次连读 RA 字节（覆盖若干整行），再从块内
     * memcpy 出每行需要的列窗口。读放大（读进来的行距部分丢弃）换来命令数
     * 下降 1~2 个数量级：典型 static 层 stride 4540 / 行需 672B，64KB 覆盖
     * 14 行 ⇒ 336 行从 336 次读降到 24 次。
     * 缓冲走 PSRAM 单例（懒分配；失败则退回逐行，不影响正确性）。 */
    {
        uint8_t *ra = bg_ra_buf();
        if (ra) {
            int64_t r = cy0;
            while (r < cy1) {
                uint32_t base = (uint32_t)r * stride_b;
                /* 覆盖上限：缓冲大小 / 剩余行跨度 / 该层剩余字节 */
                size_t span = bg_ra_cap();
                size_t remain_span = (size_t)(cy1 - r) * stride_b;
                if (span > remain_span) span = remain_span;
                uint32_t col_off = (uint32_t)cx0 * 2u;
                size_t need = (size_t)row_bytes + col_off;      /* 至少覆盖一行 */
                if (span < need) span = need;
                if (layer_len > base) {
                    size_t avail = layer_len - base;
                    if (span > avail) span = avail;
                } else {
                    break;                                      /* 越界：剩余行按 0 处理 */
                }
                int rc = mpak_read_at((mpak_t *)m,
                                      m->payload_off + layer_off + base, ra, span);
                if (rc) return rc;
                int64_t nrows = 0;
                while (r + nrows < cy1) {
                    size_t in_off = (size_t)nrows * stride_b + col_off;
                    if (in_off + row_bytes > span) break;
                    uint16_t *drow = dst +
                        (size_t)(int32_t)(r + nrows - wy0) * dst_stride_px + dx0;
                    memcpy(drow, ra + in_off, row_bytes);
                    nrows++;
                }
                if (nrows == 0) {          /* 缓冲异常小：保底逐行，绝不死循环 */
                    uint16_t *drow = dst +
                        (size_t)(int32_t)(r - wy0) * dst_stride_px + dx0;
                    int rc2 = mpak_read_at((mpak_t *)m,
                                           m->payload_off + layer_off + base + col_off,
                                           drow, row_bytes);
                    if (rc2) return rc2;
                    r++;
                } else {
                    r += nrows;
                }
            }
            return MPAK_OK;
        }
        /* 预读缓冲分配失败：退回逐行（慢但正确） */
        for (int64_t r = cy0; r < cy1; r++) {
            uint16_t *drow = dst + (size_t)(int32_t)(r - wy0) * dst_stride_px + dx0;
            uint32_t off = m->payload_off + layer_off + (uint32_t)r * stride_b +
                           (uint32_t)cx0 * 2u;
            int rc = mpak_read_at((mpak_t *)m, off, drow, row_bytes);
            if (rc) return rc;
        }
        return MPAK_OK;
    }
}

/* ══ 分流点：tiled=true 走瓦片缓存整块读，false 走原逐行（预读块）路径 ══
 * 两条路径的**入参与语义完全一致**（世界矩形 + dst 行距 + 越界补 0），所以
 * compositor 侧一行都不用改分流：旧包（无 bit1 / 长度是逐行口径）逐字节不变。 */
int mpak_bgmap_read_static_rect(const mpak_t *m, int32_t x, int32_t y,
                                int32_t w, int32_t h, uint16_t *dst, int32_t dst_stride_px)
{
    int rc = bg_rect_args(m, w, h, dst, dst_stride_px);
    if (rc) return rc;
    const mpak_bgmap_t *bg = m->u.bgmap;
    if (bg->tiled)
        return bg_tiled_read_rgb(m, bg->static_back_off, x, y, w, h, dst, dst_stride_px);
    return bg_read_rect_rgb(m, bg->static_back_off, bg->static_back_len,
                            bg_static_stride(bg), x, y, w, h, dst, dst_stride_px);
}

int mpak_bgmap_read_tile_rect(const mpak_t *m, int32_t x, int32_t y,
                              int32_t w, int32_t h, uint16_t *dst, int32_t dst_stride_px)
{
    int rc = bg_rect_args(m, w, h, dst, dst_stride_px);
    if (rc) return rc;
    const mpak_bgmap_t *bg = m->u.bgmap;
    if (bg->tile_layer_len == 0) return MPAK_ERR_RANGE;     /* 该图无 tile 层 */
    if (bg->tiled)
        return bg_tiled_read_rgb(m, bg->tile_layer_off, x, y, w, h, dst, dst_stride_px);
    uint32_t stride_b = bg_tile_stride(bg);
    /* layer_len 只给 RGB 区（= vh×行距），掩码区不会被误读进来 */
    return bg_read_rect_rgb(m, bg->tile_layer_off, (uint32_t)bg->vh * stride_b,
                            stride_b, x, y, w, h, dst, dst_stride_px);
}

int mpak_bgmap_read_tile_mask_rect(const mpak_t *m, int32_t x, int32_t y,
                                   int32_t w, int32_t h, uint8_t *dst,
                                   int32_t dst_stride_px)
{
    int rc = bg_rect_args(m, w, h, dst, dst_stride_px);
    if (rc) return rc;
    const mpak_bgmap_t *bg = m->u.bgmap;
    if (bg->tile_layer_len == 0) return MPAK_ERR_RANGE;

    const int32_t vw = (int32_t)bg->vw, vh = (int32_t)bg->vh;
    const uint32_t stride_b = bg_tile_stride(bg);

    /* 瓦片口径：掩码按**同网格**分块，每块 T*T/8 字节（契约 §3），
     * 与像素区长度一起由 parse_bgmap 反解（tile_mask_off / tile_mask_bytes）。 */
    if (bg->tiled) {
        if (bg->tile_mask_bytes < tile_mask_len_of(bg->tile, bg->gx, bg->gy))
            return MPAK_ERR_FMT;                        /* 包损坏：掩码区长度不足 */
        mpak_tile_src_t s;
        rc = bg_tiled_src(m, bg->tile_mask_off, &s);
        if (rc) return rc;
        return mpak_tile_read_mask(&s, x, y, w, h, dst, dst_stride_px);
    }

    const uint32_t rgb_bytes = (uint32_t)vh * stride_b;
    if (bg->tile_layer_len < rgb_bytes) return MPAK_ERR_FMT;
    const uint32_t mask_off   = bg_tile_mask_off(bg);
    const uint32_t mask_bytes = bg->tile_layer_len - rgb_bytes;
    const uint32_t mask_need  = (uint32_t)(((uint64_t)vw * vh + 7u) / 8u);
    if (mask_bytes < mask_need) return MPAK_ERR_FMT;   /* 掩码区长度不足（包损坏） */

    /* 目标清零：越界/未覆盖 = 0（"无 tile"，与掩码位 0 同语义） */
    for (int32_t dy = 0; dy < h; dy++)
        memset(dst + (size_t)dy * dst_stride_px, 0, (size_t)w);
    if (w == 0 || h == 0) return MPAK_OK;

    const int64_t wx0 = x, wy0 = y;
    const int64_t cx0 = wx0 < 0 ? 0 : (wx0 > vw ? vw : wx0);
    const int64_t cx1 = (wx0 + w) > vw ? vw : (wx0 + w);
    const int64_t cy0 = wy0 < 0 ? 0 : (wy0 > vh ? vh : wy0);
    const int64_t cy1 = (wy0 + h) > vh ? vh : (wy0 + h);
    if (cx0 >= cx1 || cy0 >= cy1) return MPAK_OK;

    const int32_t dx0 = (int32_t)(cx0 - wx0);
    const uint32_t nbits = (uint32_t)(cx1 - cx0);
    /* 每行最多涉及 (nbits+7)/8 + 1 字节（首位/末位可能落在同一字节、也可能跨行） */
    uint8_t *tmp = psram_alloc((size_t)((nbits + 7u) / 8u) + 2u);
    if (!tmp) return MPAK_ERR_NOMEM;

    for (int64_t r = cy0; r < cy1; r++) {
        /* 全局 bit 号（行间不补位）⇒ 跨行字节自动正确 */
        uint64_t bit0 = (uint64_t)r * (uint64_t)vw + (uint64_t)cx0;
        uint32_t fb = (uint32_t)(bit0 >> 3);
        uint32_t lb = (uint32_t)((bit0 + nbits - 1u) >> 3);
        uint32_t n  = lb - fb + 1u;
        int rc2 = mpak_read_at((mpak_t *)m, m->payload_off + mask_off + fb, tmp, n);
        if (rc2) { heap_caps_free(tmp); return rc2; }
        uint8_t *drow = dst + (size_t)(int32_t)(r - wy0) * dst_stride_px + dx0;
        for (uint32_t i = 0; i < nbits; i++) {
            uint64_t bit = bit0 + i;
            uint32_t bi  = (uint32_t)((bit >> 3) - fb);
            drow[i] = (uint8_t)((tmp[bi] >> (7u - (uint32_t)(bit & 7u))) & 1u);
        }
    }
    heap_caps_free(tmp);
    return MPAK_OK;
}

int32_t mpak_bgmap_ground_y(const mpak_t *m, int32_t world_x)
{
    if (!m || !m->u.bgmap) return INT32_MIN;
    const mpak_bgmap_t *bg = m->u.bgmap;
    if (bg->ground_len == 0) return INT32_MIN;                     /* 旧包：无地面表 */
    if (world_x < 0 || world_x >= (int32_t)bg->vw) return INT32_MIN;
    uint32_t byte_off = bg->ground_off + (uint32_t)world_x * 2u;
    uint16_t gy;
    if (bg->ground) {
        gy = bg->ground[world_x];                                  /* 常驻缓存（host 序） */
    } else {
        /* 未缓存（>16KB，已被维度上限挡住，理论上到不了）→ 按需读 2B，u16 小端 */
        uint8_t b[2];
        if (mpak_read_at((mpak_t *)m, m->payload_off + byte_off, b, 2) != MPAK_OK)
            return INT32_MIN;
        gy = (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
    }
    if (gy == MPAK_BGMAP_GROUND_NONE) return INT32_MIN;            /* 该列无 foothold */
    return (int32_t)gy;
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

/* 【曲库上限 2026-09-27 真机根因】WZ 曲库实测 **1167 首**（本机 AUDIO_META
 * 8d618b5d337f2818：track_count=1167，4+108×1167=126040 = payload 全长）。
 * 旧上限 1024 → parse_audio 直接 MPAK_ERR_FMT → mpak_open 失败 →
 * 设备侧 "audio pack unusable, skipped" → `曲目表构建：命中 0 首` →
 * 曲目表恒空：next/prev 无表可走、点播无曲可播（用户报障"设了 BGM 没声"）。
 * 表本体在 PSRAM（1167×108B≈123KB 索引 + 设备侧 ids/titles≈41KB），放宽到 4096
 * 仍只占 PSRAM，不动内部堆。 */
#define MPAK_AUDIO_MAX_TRACKS 4096u

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
            /* 地面表常驻缓存（R2 整图包；旧包恒 NULL） */
            if (m->u.bgmap->ground) heap_caps_free((void *)m->u.bgmap->ground);
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

    /* 文件身份（瓦片缓存键）：全局自增，永不复用 —— fd 会被 close/reopen 复用，
     * 而"打开同一路径的两次 open"在这套缓存里必须是同一个身份（条带每次补读都
     * open/close，靠身份相同才能命中上次读进来的瓦片）。 */
    {
        static uint32_t s_file_seq;
        m->file_id = ++s_file_seq;
        if (m->file_id == 0) m->file_id = ++s_file_seq;      /* 回绕保护（0 = 无身份） */
    }

    m->f = fopen(path, "rb");
    if (!m->f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return MPAK_ERR_IO;
    }
    /* 【stdio 缓冲实测结论 2026-10-02 —— 查过并**刻意不动**】
     * 曾考虑 `setvbuf(_IONBF)` 省掉每个 FILE 的 stdio 缓冲，实测否决：
     *   · 本工具链是 newlib（CONFIG_LIBC_NEWLIB=y）且 **BUFSIZ = 128**（不是 1024）
     *     → 每句柄那点缓冲只有 128B，6~10 个常开句柄合计 ~1KB，收益太小；
     *   · 而 `_IONBF` 在 newlib 里会把 `_bf._size` 设成 **1**
     *     （libc_a-makebuf.o：0xa7 处 `_bf._base = &_nbuf; _bf._size = 1`），
     *     且 `_fread_r`（libc_a-fread.o）**没有**"大请求直读用户缓冲"的快路径
     *     （其重定位表里只有 __srefill_r/memcpy，没有 _read_r）
     *     → `fread` 会退化成**一次 1 字节**的 read()，17MB 校验直接灾难。
     * 结论：保持默认缓冲。顺带记录一个**性能**线索（本次不动，供后续单独评估）：
     * 既然 BUFSIZ=128，`fread` 每 1KB 就要 8 次 read()（17MB 的 crc 校验
     * ≈13.6 万次系统调用）。要提速应走 `setvbuf(fp, NULL, _IOFBF, 8192)`
     * —— 8192 > CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL(4096)，缓冲会落到 PSRAM，
     * 既不占内部堆又能把 read() 次数降 64 倍；但随机读会多读前一扇区，
     * 属行为变更，须真机按"地图装载/瓦片读"回归后再上。 */

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

    /* 【内部堆取证 2026-10-02】每次成功打开都带上当时的内部堆水位/最大块：
     * 真机现象是"@联网后 空闲=32819 → 6 个 mpak opened → @@素材全绑后 只剩
     * 1307"，但逐个包究竟各吃多少内部 RAM 一直只能靠猜。这一行让"谁吃的"
     * 直接可读（本条日志本身就是交付的验证判据之一，不是临时调试件）：
     * 每个包的增量 = 本行空闲值 - 上一行空闲值；增量大的包就是嫌疑包。
     * 只读两个计数（heap_caps_get_*），对打开耗时无影响。 */
    ESP_LOGI(TAG, "opened %s kind=%llu hash=%016llx payload=%" PRIu32
                  " 内部堆(此包后) 空闲=%u 最大块=%u",
             path, (unsigned long long)m->kind,
             (unsigned long long)m->content_hash, m->payload_len,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
