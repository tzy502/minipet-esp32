/**
 * asset_dl.c — manifest diff / 逐包下载校验 / TF 落盘 / LRU 淘汰 / 元数据查询
 *
 * 同步流程（M7）：
 *   1) GET /api/device/manifest → {proto, rev, assets:{hash:{kind,bytes,label,
 *      entity,action,map,selector}}, clock_table:{map_id:[x,y]}, firmware:{...}}
 *   2) rev == 本地 rev → 跳过（304 语义，software-design 六）
 *   3) 逐条目：登记/刷新元数据；本地缺 hash → GET /api/device/asset/{hash}
 *      → .tmp 流式落盘 + 边下边算 crc32c → 校验信封（MPAK/crc/hash/长度）
 *      → rename <hash>.mpk；FONT 包顺带读包内 size_px 归档字号
 *   4) 更新 /minipet/manifest.json（hash 集 + 元数据 + clock_table + active_map）
 *      → cmd_q MANIFEST_SYNCED（render 任务重绑素材）
 *   5) TF 用量 ≥85% → LRU 淘汰（收藏 + 每类最新受保护）清到 ≤80%
 */
#include "asset_dl.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "cJSON.h"

#include "app_core.h"
#include "http_client.h"
#include "input_dispatch.h"

static const char *TAG = "asset";

#define MAX_FILES          256
#define MAX_CLOCK_MAPS     32
#define WATERMARK_PCT      85     /* E11/4.4：85% 水位触发 */
#define STOP_PCT           80     /* 清到 80% 停手 */
#define MANIFEST_RESP_CAP  (48 * 1024)

/* ------------------------------------------------------------------ */
/* 本地清单模型（内存 + TF manifest.json 双写）                          */
/* ------------------------------------------------------------------ */
typedef struct {
    char     hash[20];
    char     kind[12];        /* PARTS/LAYOUT/BGMAP/FONT/AUDIO_META */
    char     action[32];      /* LAYOUT：动作名 */
    char     entity[40];      /* LAYOUT/PARTS：如 "paperdoll:default" */
    char     map_id[32];      /* BGMAP：地图 id */
    char     selector[12];    /* map/paperdoll/npc/clock（无则空） */
    uint8_t  font_px;         /* FONT：16/24/32 */
    uint32_t bytes;
    bool     fav;
    int64_t  last_used_ms;
} local_file_t;

static local_file_t s_files[MAX_FILES];
static int          s_file_cnt;
static uint32_t     s_local_rev;
static char         s_active_map[32];          /* 当前地图（clock 锚点/LRU） */

/* clock_table（E9/R15：[x,y] = 烘焙视口内屏幕坐标，世界 1x 口径下发给渲染层） */
static struct { char map_id[32]; int16_t x, y; } s_clock_tab[MAX_CLOCK_MAPS];
static int s_clock_cnt;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_sync_req;

/* ------------------------------------------------------------------ */
/* crc32c（Castagnoli，表运行期生成）                                    */
/* ------------------------------------------------------------------ */
static uint32_t s_crc_table[256];

static void crc32c_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        }
        s_crc_table[i] = c;
    }
}

static uint32_t crc32c_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc = s_crc_table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_le64(const uint8_t *p)
{
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

/* ------------------------------------------------------------------ */
/* 目录/路径                                                            */
/* ------------------------------------------------------------------ */
static const char *kind_dir(const char *kind)
{
    if (strcmp(kind, "PARTS") == 0)      return MP_TF_MINIPET_DIR "/parts";
    if (strcmp(kind, "LAYOUT") == 0)     return MP_TF_MINIPET_DIR "/layout";
    if (strcmp(kind, "BGMAP") == 0)      return MP_TF_MINIPET_DIR "/bg";
    if (strcmp(kind, "FONT") == 0)       return MP_TF_MINIPET_DIR "/font";
    if (strcmp(kind, "AUDIO_META") == 0) return MP_TF_MINIPET_DIR "/audio";
    return NULL;
}

static void ensure_dirs(void)
{
    mkdir(MP_TF_MINIPET_DIR, 0775);
    const char *dirs[] = { "parts", "layout", "bg", "font", "audio", "firmware" };
    char path[64];
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", MP_TF_MINIPET_DIR, dirs[i]);
        mkdir(path, 0775);
    }
}

/* ------------------------------------------------------------------ */
/* 本地清单持久化                                                        */
/* ------------------------------------------------------------------ */
static void load_local_manifest(void)
{
    s_file_cnt = 0;
    s_local_rev = 0;
    s_active_map[0] = 0;
    s_clock_cnt = 0;

    FILE *f = fopen(MP_TF_MANIFEST, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 128 * 1024) { fclose(f); return; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    s_local_rev = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "rev"));

    const char *am = cJSON_GetStringValue(cJSON_GetObjectItem(root, "active_map"));
    if (am) strlcpy(s_active_map, am, sizeof(s_active_map));

    cJSON *files = cJSON_GetObjectItem(root, "files");
    if (cJSON_IsArray(files)) {
        cJSON *jf;
        cJSON_ArrayForEach(jf, files) {
            if (s_file_cnt >= MAX_FILES) break;
            local_file_t *lf = &s_files[s_file_cnt];
            const char *h = cJSON_GetStringValue(cJSON_GetObjectItem(jf, "hash"));
            const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(jf, "kind"));
            if (!h || !k) continue;
            strlcpy(lf->hash, h, sizeof(lf->hash));
            strlcpy(lf->kind, k, sizeof(lf->kind));
            const char *s;
            if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(jf, "action"))))
                strlcpy(lf->action, s, sizeof(lf->action));
            if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(jf, "entity"))))
                strlcpy(lf->entity, s, sizeof(lf->entity));
            if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(jf, "map"))))
                strlcpy(lf->map_id, s, sizeof(lf->map_id));
            if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(jf, "selector"))))
                strlcpy(lf->selector, s, sizeof(lf->selector));
            lf->font_px = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(jf, "px"));
            lf->bytes = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(jf, "bytes"));
            lf->fav = cJSON_IsTrue(cJSON_GetObjectItem(jf, "fav"));
            lf->last_used_ms = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(jf, "ts"));
            s_file_cnt++;
        }
    }

    cJSON *ct = cJSON_GetObjectItem(root, "clock_table");
    if (cJSON_IsObject(ct)) {
        cJSON *jm;
        cJSON_ArrayForEach(jm, ct) {
            if (s_clock_cnt >= MAX_CLOCK_MAPS) break;
            cJSON *arr = jm->child;
            if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 2) {
                strlcpy(s_clock_tab[s_clock_cnt].map_id, jm->string,
                        sizeof(s_clock_tab[0].map_id));
                s_clock_tab[s_clock_cnt].x =
                    (int16_t)cJSON_GetNumberValue(cJSON_GetArrayItem(arr, 0));
                s_clock_tab[s_clock_cnt].y =
                    (int16_t)cJSON_GetNumberValue(cJSON_GetArrayItem(arr, 1));
                s_clock_cnt++;
            }
        }
    }
    cJSON_Delete(root);
}

/* 调用方须持 s_lock */
static void save_local_manifest_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "proto", MP_PROTO_VER);
    cJSON_AddNumberToObject(root, "rev", s_local_rev);
    if (s_active_map[0]) cJSON_AddStringToObject(root, "active_map", s_active_map);

    cJSON *files = cJSON_AddArrayToObject(root, "files");
    for (int i = 0; i < s_file_cnt; i++) {
        cJSON *jf = cJSON_CreateObject();
        cJSON_AddStringToObject(jf, "hash", s_files[i].hash);
        cJSON_AddStringToObject(jf, "kind", s_files[i].kind);
        if (s_files[i].action[0])   cJSON_AddStringToObject(jf, "action", s_files[i].action);
        if (s_files[i].entity[0])   cJSON_AddStringToObject(jf, "entity", s_files[i].entity);
        if (s_files[i].map_id[0])   cJSON_AddStringToObject(jf, "map", s_files[i].map_id);
        if (s_files[i].selector[0]) cJSON_AddStringToObject(jf, "selector", s_files[i].selector);
        if (s_files[i].font_px)     cJSON_AddNumberToObject(jf, "px", s_files[i].font_px);
        cJSON_AddNumberToObject(jf, "bytes", s_files[i].bytes);
        cJSON_AddBoolToObject(jf, "fav", s_files[i].fav);
        cJSON_AddNumberToObject(jf, "ts", (double)s_files[i].last_used_ms);
        cJSON_AddItemToArray(files, jf);
    }

    if (s_clock_cnt > 0) {
        cJSON *ct = cJSON_AddObjectToObject(root, "clock_table");
        for (int i = 0; i < s_clock_cnt; i++) {
            cJSON *xy = cJSON_CreateArray();
            cJSON_AddItemToArray(xy, cJSON_CreateNumber(s_clock_tab[i].x));
            cJSON_AddItemToArray(xy, cJSON_CreateNumber(s_clock_tab[i].y));
            cJSON_AddItemToObject(ct, s_clock_tab[i].map_id, xy);
        }
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return;

    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.tmp", MP_TF_MANIFEST);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fwrite(body, 1, strlen(body), f);
        fclose(f);
        unlink(MP_TF_MANIFEST);
        rename(tmp, MP_TF_MANIFEST);
    }
    free(body);
}

/* ------------------------------------------------------------------ */
/* 单包下载（流式 + 边下边校验）                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    FILE    *f;
    uint64_t want_hash;      /* manifest 键（信封 content_hash 必须相等） */
    uint32_t crc;
    uint64_t total;

    uint8_t  head[32];
    size_t   head_len;
    bool     head_ok;

    uint8_t  tail[8];        /* 末 8 字节滑窗（尾部 crc 字段不进 crc） */
    size_t   tail_len;

    uint64_t env_kind, env_hash;
    uint32_t payload_len;
} dl_ctx_t;

static bool dl_chunk(void *ctx_, const char *data, size_t len)
{
    dl_ctx_t *c = ctx_;

    /* 1) 攒头 32 字节（MAGIC/版本/kind/content_hash/payload_len） */
    if (c->head_len < sizeof(c->head)) {
        size_t need = sizeof(c->head) - c->head_len;
        size_t take = (len < need) ? len : need;
        memcpy(c->head + c->head_len, data, take);
        c->head_len += take;
        if (c->head_len == sizeof(c->head)) {
            c->head_ok = (memcmp(c->head, "MPAK", 4) == 0);
            c->env_kind = rd_le64(c->head + 8);
            c->env_hash = rd_le64(c->head + 16);
            c->payload_len = rd_le32(c->head + 24);
        }
    }

    /* 2) 流序 = [旧 pending][本块]：前 (n-8) 字节写盘+进 crc，
     *    最后 ≤8 字节滑入 pending */
    size_t n = c->tail_len + len;
    size_t crc_len = (n > 8) ? (n - 8) : 0;

    size_t from_pend = (crc_len < c->tail_len) ? crc_len : c->tail_len;
    if (from_pend > 0) {
        if (fwrite(c->tail, 1, from_pend, c->f) != from_pend) return false;
        c->crc = crc32c_update(c->crc, c->tail, from_pend);
    }
    size_t from_chunk = crc_len - from_pend;
    if (from_chunk > 0) {
        if (fwrite(data, 1, from_chunk, c->f) != from_chunk) return false;
        c->crc = crc32c_update(c->crc, data, from_chunk);
    }

    size_t rem_pend = c->tail_len - from_pend;
    memmove(c->tail, c->tail + from_pend, rem_pend);
    memcpy(c->tail + rem_pend, (const uint8_t *)data + from_chunk, len - from_chunk);
    c->tail_len = rem_pend + (len - from_chunk);

    c->total += len;
    return c->head_ok || (c->head_len < sizeof(c->head));
}

/* FONT 包内 size_px（payload 首字节；文件偏移 = 32 头） */
static uint8_t read_font_px(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t b[2] = { 0, 0 };
    fseek(f, 32, SEEK_SET);
    size_t r = fread(b, 1, 2, f);
    fclose(f);
    return (r == 2) ? b[0] : 0;
}

static bool verify_and_commit(dl_ctx_t *c, const char *dir, const char *hash)
{
    if (!c->head_ok) return false;
    if (c->total != (uint64_t)c->payload_len + 40) return false;   /* 32 头+8 尾 */
    if (c->tail_len != 8) return false;

    uint32_t want_crc = rd_le32(c->tail);
    uint32_t want_zero = rd_le32(c->tail + 4);
    if (want_zero != 0) return false;
    if (want_crc != c->crc) return false;
    if (c->env_hash != c->want_hash) return false;                 /* hash 校验 */

    char final_path[96], tmp_path[104];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s.mpk.tmp", dir, hash);
    snprintf(final_path, sizeof(final_path), "%s/%s.mpk", dir, hash);

    fclose(c->f);
    c->f = NULL;
    unlink(final_path);
    return rename(tmp_path, final_path) == 0;
}

/* 元数据登记/刷新（不下载）；返回该 hash 本地是否已有文件 */
static bool upsert_meta(const char *hash, const char *kind, const char *action,
                        const char *entity, const char *map_id, const char *selector)
{
    bool found_file = false;
    char path[MP_MPK_PATH_MAX];
    const char *dir = kind_dir(kind);
    if (dir) {
        snprintf(path, sizeof(path), "%s/%s.mpk", dir, hash);
        found_file = (access(path, F_OK) == 0);
    }

    local_file_t *slot = NULL;
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].hash, hash) == 0) { slot = &s_files[i]; break; }
    }
    if (!slot && s_file_cnt < MAX_FILES) {
        slot = &s_files[s_file_cnt++];
        memset(slot, 0, sizeof(*slot));
        strlcpy(slot->hash, hash, sizeof(slot->hash));
    }
    if (slot) {
        strlcpy(slot->kind, kind, sizeof(slot->kind));
        if (action)   strlcpy(slot->action, action, sizeof(slot->action));
        if (entity)   strlcpy(slot->entity, entity, sizeof(slot->entity));
        if (map_id)   strlcpy(slot->map_id, map_id, sizeof(slot->map_id));
        if (selector) strlcpy(slot->selector, selector, sizeof(slot->selector));
        if (found_file && slot->last_used_ms == 0) slot->last_used_ms = mp_now_ms();
    }
    return found_file;
}

static bool download_one(const char *hash, const char *kind)
{
    const char *dir = kind_dir(kind);
    if (!dir) return false;   /* 未知 kind：登记元数据但跳过（proto 前向兼容） */

    char api_path[96], tmp_path[104];
    snprintf(api_path, sizeof(api_path), "/api/device/asset/%s", hash);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s.mpk.tmp", dir, hash);

    for (int attempt = 0; attempt < 2; attempt++) {   /* 损坏重拉一次（E11） */
        dl_ctx_t c = { 0 };
        c.want_hash = strtoull(hash, NULL, 16);
        c.f = fopen(tmp_path, "wb");
        if (!c.f) return false;

        int status = mp_http_get(api_path, 20000, dl_chunk, &c);
        if (status == 200 && verify_and_commit(&c, dir, hash)) {
            return true;                         /* 字号/时间戳在锁内登记（sync_once） */
        }

        if (c.f) fclose(c.f);
        unlink(tmp_path);

        if (status != 200 && status != -1) return false;   /* 404 等：不重试 */

        /* 损坏（crc/hash/MAGIC）→ 事件 + dam 表情 + 重拉一次（E11） */
        mp_post_event_simple(MP_EVT_ASSET_ERROR, status, 0, hash);
        input_trigger_expression(MP_EXPR_DAM, 2000);
        ESP_LOGW(TAG, "asset %s corrupt (status=%d), retry", hash, status);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* TF 水位 + LRU 淘汰                                                    */
/* ------------------------------------------------------------------ */
static bool tf_usage_pct(int *pct)
{
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(MP_TF_ROOT, &total, &free_b) != ESP_OK || total == 0) {
        return false;
    }
    *pct = (int)((total - free_b) * 100 / total);
    return true;
}

static uint32_t kind_bucket(const char *kind)
{
    uint32_t h = 0;
    for (const char *p = kind; *p; p++) h = h * 31 + (uint8_t)*p;
    return h % 16;
}

static void evict_if_needed(void)
{
    int pct = 0;
    if (!tf_usage_pct(&pct) || pct < WATERMARK_PCT) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    while (s_file_cnt > 0) {
        int now = 0;
        if (!tf_usage_pct(&now) || now <= STOP_PCT) break;

        /* 每类（kind 分桶）最近使用受保护 + 收藏保护（E7：保最近 N + 收藏） */
        int64_t newest[16] = { 0 };
        for (int i = 0; i < s_file_cnt; i++) {
            int b = (int)kind_bucket(s_files[i].kind);
            if (s_files[i].last_used_ms > newest[b]) newest[b] = s_files[i].last_used_ms;
        }

        int victim = -1;
        for (int i = 0; i < s_file_cnt; i++) {
            if (s_files[i].fav) continue;
            int b = (int)kind_bucket(s_files[i].kind);
            if (s_files[i].last_used_ms == newest[b]) continue;
            if (victim < 0 || s_files[i].last_used_ms < s_files[victim].last_used_ms) {
                victim = i;
            }
        }
        if (victim < 0) break;   /* 只剩保护集：宁可水位高，不误删 */

        const char *dir = kind_dir(s_files[victim].kind);
        if (dir) {
            char path[96];
            snprintf(path, sizeof(path), "%s/%s.mpk", dir, s_files[victim].hash);
            ESP_LOGI(TAG, "evict LRU %s", path);
            unlink(path);
        }
        s_files[victim] = s_files[s_file_cnt - 1];
        s_file_cnt--;
    }

    save_local_manifest_locked();
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* manifest 同步                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    char  *buf;
    size_t len, cap;
} manifest_ctx_t;

static bool manifest_collect(void *ctx_, const char *data, size_t len)
{
    manifest_ctx_t *r = ctx_;
    if (r->len + len < r->cap) {
        memcpy(r->buf + r->len, data, len);
        r->len += len;
        r->buf[r->len] = 0;
        return true;
    }
    return false;
}

static void sync_once(void)
{
    static char *resp = NULL;
    if (!resp) {
        resp = malloc(MANIFEST_RESP_CAP);       /* 常驻复用（任务栈外） */
        if (!resp) return;
    }
    manifest_ctx_t ctx = { .buf = resp, .cap = MANIFEST_RESP_CAP };

    int status = mp_http_get("/api/device/manifest", 10000, manifest_collect, &ctx);
    if (status != 200) return;

    cJSON *root = cJSON_Parse(resp);
    if (!root) return;

    uint32_t rev = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "rev"));

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (rev == s_local_rev && s_local_rev != 0 && s_file_cnt > 0) {
        xSemaphoreGive(s_lock);
        cJSON_Delete(root);
        return;                                  /* 304 语义：无变化 */
    }

    /* clock_table（E9/R15：地图时钟锚点表，Web 可改 → rev+1 生效） */
    cJSON *ct = cJSON_GetObjectItem(root, "clock_table");
    if (cJSON_IsObject(ct)) {
        s_clock_cnt = 0;
        cJSON *jm;
        cJSON_ArrayForEach(jm, ct) {
            if (s_clock_cnt >= MAX_CLOCK_MAPS) break;
            cJSON *xy = jm->child;
            if (cJSON_IsArray(xy) && cJSON_GetArraySize(xy) == 2) {
                strlcpy(s_clock_tab[s_clock_cnt].map_id, jm->string,
                        sizeof(s_clock_tab[0].map_id));
                s_clock_tab[s_clock_cnt].x =
                    (int16_t)cJSON_GetNumberValue(cJSON_GetArrayItem(xy, 0));
                s_clock_tab[s_clock_cnt].y =
                    (int16_t)cJSON_GetNumberValue(cJSON_GetArrayItem(xy, 1));
                s_clock_cnt++;
            }
        }
    }

    /* 条目元数据 + 缺包下载（持锁登记元数据；下载在锁外做 IO） */
    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (cJSON_IsObject(assets)) {
        cJSON *ja;
        cJSON_ArrayForEach(ja, assets) {
            const char *hash = ja->string;
            const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(ja, "kind"));
            if (!hash || !kind) continue;
            upsert_meta(hash, kind,
                        cJSON_GetStringValue(cJSON_GetObjectItem(ja, "action")),
                        cJSON_GetStringValue(cJSON_GetObjectItem(ja, "entity")),
                        cJSON_GetStringValue(cJSON_GetObjectItem(ja, "map")),
                        cJSON_GetStringValue(cJSON_GetObjectItem(ja, "selector")));
        }
        xSemaphoreGive(s_lock);

        cJSON_ArrayForEach(ja, assets) {
            const char *hash = ja->string;
            const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(ja, "kind"));
            if (!hash || !kind) continue;

            bool have = false;
            const char *dir = kind_dir(kind);
            char path[MP_MPK_PATH_MAX];
            if (dir) {
                snprintf(path, sizeof(path), "%s/%s.mpk", dir, hash);
                have = (access(path, F_OK) == 0);
            }
            if (have) continue;                     /* hash 即内容身份：无差量 */

            evict_if_needed();                      /* 下载前查水位 */
            if (!download_one(hash, kind)) continue;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < s_file_cnt; i++) {
                if (strcmp(s_files[i].hash, hash) == 0) {
                    s_files[i].last_used_ms = mp_now_ms();
                    if (strcmp(kind, "FONT") == 0) {
                        char p[MP_MPK_PATH_MAX];
                        const char *d = kind_dir("FONT");
                        if (d) {
                            snprintf(p, sizeof(p), "%s/%s.mpk", d, hash);
                            s_files[i].font_px = read_font_px(p);   /* 16/24/32 */
                        }
                    }
                    break;
                }
            }
            xSemaphoreGive(s_lock);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }

    s_local_rev = rev;
    save_local_manifest_locked();
    xSemaphoreGive(s_lock);

    cJSON_Delete(root);

    /* 素材就绪 → 渲染层重读/重绑（render 任务执行：fonts/parts/stand1） */
    mp_cmd_t c = { .type = MP_CMD_MANIFEST_SYNCED };
    mp_post_cmd(&c);
}

static void asset_dl_task(void *arg)
{
    (void)arg;
    ensure_dirs();
    crc32c_init_table();
    load_local_manifest();

    for (;;) {
        if (xSemaphoreTake(s_sync_req, pdMS_TO_TICKS(60000)) == pdTRUE) {
            while (xSemaphoreTake(s_sync_req, 0) == pdTRUE) {}   /* 合并重复请求 */
            sync_once();
        } else {
            sync_once();      /* 周期兜底：60s 无触发也对表一次服务端 rev */
        }
    }
}

/* ------------------------------------------------------------------ */
/* 查询面（app_cmd_dispatch 于 render 任务内调用；只读内存表）           */
/* ------------------------------------------------------------------ */
void asset_dl_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_sync_req = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(asset_dl_task, "asset_dl", 10240, NULL,
                            2 /* 低于 poller——4.1 */, NULL, 0 /* PRO */);
}

void asset_dl_request_sync(void)
{
    if (!s_sync_req) return;
    xSemaphoreGive(s_sync_req);
}

bool asset_dl_have_local_manifest(void)
{
    return s_file_cnt > 0 || (access(MP_TF_MANIFEST, F_OK) == 0);
}

size_t asset_dl_collect_hashes(char *buf, size_t cap)
{
    if (!buf || cap < 4) return 0;
    buf[0] = 0;
    size_t off = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        size_t L = strlen(s_files[i].hash);
        if (off + L + 2 >= cap) break;
        if (off > 0) buf[off++] = ',';
        memcpy(buf + off, s_files[i].hash, L);
        off += L;
    }
    xSemaphoreGive(s_lock);

    buf[off] = 0;
    return off;
}

bool asset_dl_layout_path(const char *action, char *path, size_t cap)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int fallback = -1;
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].kind, "LAYOUT") != 0) continue;
        if (strcmp(s_files[i].action, action) != 0) continue;
        if (strncmp(s_files[i].entity, "paperdoll", 9) == 0) {
            const char *dir = kind_dir("LAYOUT");
            snprintf(path, cap, "%s/%s.mpk", dir, s_files[i].hash);
            ok = true;
            break;
        }
        if (fallback < 0) fallback = i;
    }
    if (!ok && fallback >= 0) {
        const char *dir = kind_dir("LAYOUT");
        snprintf(path, cap, "%s/%s.mpk", dir, s_files[fallback].hash);
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool asset_dl_parts_path(const char *entity_or_null, char *path, size_t cap)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int fallback = -1;
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].kind, "PARTS") != 0) continue;
        if (strcmp(s_files[i].selector, "clock") == 0) continue;   /* fontTime 非装扮 */
        if (entity_or_null && s_files[i].entity[0] &&
            strcmp(s_files[i].entity, entity_or_null) != 0) continue;
        if (entity_or_null == NULL) {
            /* 默认：paperdoll 系实体优先 */
            if (strncmp(s_files[i].entity, "paperdoll", 9) == 0 ||
                strcmp(s_files[i].selector, "paperdoll") == 0) {
                const char *dir = kind_dir("PARTS");
                snprintf(path, cap, "%s/%s.mpk", dir, s_files[i].hash);
                ok = true;
                break;
            }
            if (fallback < 0) fallback = i;
        } else {
            const char *dir = kind_dir("PARTS");
            snprintf(path, cap, "%s/%s.mpk", dir, s_files[i].hash);
            ok = true;
            break;
        }
    }
    if (!ok && fallback >= 0) {
        const char *dir = kind_dir("PARTS");
        snprintf(path, cap, "%s/%s.mpk", dir, s_files[fallback].hash);
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool asset_dl_font_path(int size_px, char *path, size_t cap)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].kind, "FONT") != 0) continue;
        if (s_files[i].font_px != (uint8_t)size_px) continue;
        const char *dir = kind_dir("FONT");
        snprintf(path, cap, "%s/%s.mpk", dir, s_files[i].hash);
        ok = true;
        break;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool asset_dl_fonttime_path(char *path, size_t cap)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].kind, "PARTS") != 0) continue;
        if (strcmp(s_files[i].selector, "clock") != 0) continue;
        const char *dir = kind_dir("PARTS");
        snprintf(path, cap, "%s/%s.mpk", dir, s_files[i].hash);
        ok = true;
        break;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool asset_dl_map_path(const char *hash_or_null, char *path, size_t cap)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int best = -1;
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].kind, "BGMAP") != 0) continue;
        if (hash_or_null) {
            if (strcmp(s_files[i].hash, hash_or_null) == 0) { best = i; break; }
            continue;
        }
        /* NULL = 最近使用的 selector==map 地图（E7 最近+收藏） */
        if (s_files[i].selector[0] && strcmp(s_files[i].selector, "map") != 0) continue;
        if (best < 0 || s_files[i].last_used_ms > s_files[best].last_used_ms) best = i;
    }
    if (best >= 0) {
        const char *dir = kind_dir("BGMAP");
        snprintf(path, cap, "%s/%s.mpk", dir, s_files[best].hash);
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/* BGMAP 条带头解析（asset-format §五）：
 * payload: map_id[32] vw/vh[4] static len/off[8] tile len/off[8] strip_count[4]
 *          → strip_count @payload+52（文件 84）；strip[14B] @payload+56（文件 88）
 *          strip: part_ref(u64) y(i16) speed_x(i16) rx(u8) blend(u8) */
int asset_dl_map_strips(const char *bg_path,
                        char (*strip_paths)[MP_MPK_PATH_MAX], int max_strips)
{
    FILE *f = fopen(bg_path, "rb");
    if (!f) return -1;

    uint8_t sc[4] = { 0 };
    fseek(f, 84, SEEK_SET);               /* strip_count @ payload+52 = 文件 84 */
    if (fread(sc, 1, 4, f) != 4) { fclose(f); return -1; }
    int count = (int)rd_le32(sc);
    if (count <= 0) { fclose(f); return 0; }
    if (count > max_strips) count = max_strips;

    uint8_t sh[14];
    int got = 0;
    fseek(f, 88, SEEK_SET);               /* strip[0] @ payload+56 = 文件 88 */
    for (int i = 0; i < count; i++) {
        if (fread(sh, 1, sizeof(sh), f) != sizeof(sh)) break;
        uint64_t part_ref = rd_le64(sh);
        if (part_ref == 0) continue;

        /* 以本地清单登记的 hash 字符串为准（大小写与 manifest 键一致），
         * 找不到则回落小写 16 位 hex */
        bool named = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int k = 0; k < s_file_cnt; k++) {
            if (strcmp(s_files[k].kind, "PARTS") == 0 &&
                strtoull(s_files[k].hash, NULL, 16) == part_ref) {
                const char *d = kind_dir("PARTS");
                if (d) {
                    snprintf(strip_paths[got], MP_MPK_PATH_MAX,
                             "%s/%s.mpk", d, s_files[k].hash);
                    named = true;
                }
                break;
            }
        }
        xSemaphoreGive(s_lock);
        if (!named) {
            snprintf(strip_paths[got], MP_MPK_PATH_MAX,
                     MP_TF_MINIPET_DIR "/parts/%016llx.mpk",
                     (unsigned long long)part_ref);
        }
        got++;
    }
    fclose(f);
    return got;
}

void asset_dl_set_active_map(const char *hash)
{
    if (!hash || !hash[0]) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].kind, "BGMAP") == 0 &&
            strcmp(s_files[i].hash, hash) == 0) {
            if (s_files[i].map_id[0]) {
                strlcpy(s_active_map, s_files[i].map_id, sizeof(s_active_map));
            }
            s_files[i].last_used_ms = mp_now_ms();
            break;
        }
    }
    save_local_manifest_locked();
    xSemaphoreGive(s_lock);
}

bool asset_dl_clock_anchor(int16_t *world_x, int16_t *world_y)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active_map[0]) {
        for (int i = 0; i < s_clock_cnt; i++) {
            if (strcmp(s_clock_tab[i].map_id, s_active_map) == 0) {
                *world_x = s_clock_tab[i].x;
                *world_y = s_clock_tab[i].y;
                ok = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void asset_dl_set_favorite(const char *hash, bool fav)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].hash, hash) == 0) {
            s_files[i].fav = fav;
            save_local_manifest_locked();
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

void asset_dl_touch(const char *hash)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].hash, hash) == 0) {
            s_files[i].last_used_ms = mp_now_ms();
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

uint32_t asset_dl_local_rev(void)
{
    return s_local_rev;
}
