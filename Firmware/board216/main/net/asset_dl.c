/**
 * asset_dl.c — manifest diff / 逐包下载校验 / TF 落盘 / LRU 淘汰 / 元数据查询
 *
 * 同步流程（M7）：
 *   1) GET /api/device/manifest → {proto, rev, assets:{hash:{kind,bytes,label,
 *      entity,action,map,selector}}, clock_table:{map_id:[x,y]}, firmware:{...}}
 *   2) rev == 本地 rev → 跳过（304 语义，software-design 六）
 *   3) 逐条目：登记/刷新元数据；本地缺 hash → 块化断点续传下载（2026-09-30）：
 *      断点 = <hash>.mpk.tmp 现大小（stat 即得，天然续传），32KB 块逐块
 *      Range 请求，206 切片追加写 tmp；单块抖动只废单块，连续 3 块零进展
 *      放弃本轮下轮续传。服务端不认 Range（200）自动回退：off==0 体即完整
 *      文件照常收尾，off>0 清断点转全量模式。全块完成 → 从文件重建 crc/
 *      信封头尾 → 校验（MPAK/crc/hash/长度）→ rename <hash>.mpk；
 *      FONT 包顺带读包内 size_px 归档字号
 *   4) 更新 /minipet/manifest.json（hash 集 + 元数据 + clock_table + active_map）
 *      → cmd_q MANIFEST_SYNCED（render 任务重绑素材）
 *   5) TF 用量 ≥85% → LRU 淘汰（收藏 + 每类最新受保护）清到 ≤80%
 */
#include "asset_dl.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sd_tf.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs.h"               /* per-map 隐藏标识（namespace "maphide"，§4.4） */
#include "cJSON.h"

#include "app_core.h"
#include "http_client.h"
#include "esp_http_client.h"   /* range_get：块化下载需自定义 Range 头 */
#include "input_dispatch.h"

static const char *TAG = "asset";

#define MAX_FILES          256
#define MAX_CLOCK_MAPS     32
#define WATERMARK_PCT      85     /* E11/4.4：85% 水位触发 */
#define STOP_PCT           80     /* 清到 80% 停手 */
/* 16K：rev26/54 条目实测 ~11KB。旧 48K 常驻占内部堆 1/3——本板内部堆
 * 143KB 已是万物枯竭的总根源（下载失败/任务创建失败多为连锁反应） */
/* 24K：rev32 实测 18.6KB（休塔尔克装扮+NPC 后）。旧 16K 在 collect 满
 * 时返回 false 中断流 → sync 静默失败（真机：进入但永无下载）。 */
/* 【2026-10-01 再爆一次：24K 又不够了】真机实证：整图导出 + 中文名 label 之后
 * 清单涨到 **27,461 B** > 24,576 → manifest_collect 拒绝续收 → 响应被截断 →
 * cJSON_Parse 失败 → sync_once **静默 return**（无任何 E 级日志）→ 新登记的地图
 * 永远下不来、本地清单停在旧 rev（真机表现：推了 5 张整图包，设备一张都不下）。
 * 教训与 16K→24K 那次同款：**清单体积只增不减，上限必须留足并显式报错**。
 * 64KB 缓冲走 PSRAM（内部堆此刻只剩 ~50KB，再吃 64KB 会把 SD/newlib 逼死；
 * PSRAM 有 6MB+，cJSON 解析只读它）。 */
#define MANIFEST_RESP_CAP  (64 * 1024)

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
    int16_t  origin_x, origin_y; /* LAYOUT：画布内 body 锚点（桌面 RenderFrame 同口径，1x） */
    uint8_t  font_px;         /* FONT：16/24/32 */
    char     label[32];       /* 服务端 manifest label（选择器显示名） */
    uint32_t bytes;
    bool     fav;
    int64_t  last_used_ms;
} local_file_t;

static local_file_t s_files[MAX_FILES];
static int          s_file_cnt;
static uint32_t     s_local_rev;
static char         s_active_map[32];          /* 当前地图 id（clock 锚点/LRU） */
static char         s_active_map_hash[20];     /* 当前地图内容 hash（隐藏判定用，见 map_is_active） */

/* clock_table（E9/R15：[x,y] = 烘焙视口内屏幕坐标，世界 1x 口径下发给渲染层） */
static struct { char map_id[32]; int16_t x, y; } s_clock_tab[MAX_CLOCK_MAPS];
static int s_clock_cnt;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_sync_req;

/* T4 单包下载请求（菜单点选未缓存条目）：s_one_hash 非空 = 待下单个包（s_lock
 * 保护），s_sync_req 兼作唤醒信号。s_sync_again = 唤醒批次里还欠一次全量同步
 * （单包优先处理时不能把同批的全量请求吞掉——跨任务读写，volatile 单字）。 */
static char          s_one_hash[20];
static volatile bool s_sync_again;

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
    /* 大小写不敏感（服务端 ManifestBuilder 写 "Parts"/"Layout"/"Bgmap"——
     * 原全大写比较永不匹配 → 设备从不下载素材包，2026-09-26 真机定稿） */
    if (strcasecmp(kind, "PARTS") == 0)      return MP_TF_MINIPET_DIR "/parts";
    if (strcasecmp(kind, "LAYOUT") == 0)     return MP_TF_MINIPET_DIR "/layout";
    if (strcasecmp(kind, "BGMAP") == 0)      return MP_TF_MINIPET_DIR "/bg";
    if (strcasecmp(kind, "FONT") == 0)       return MP_TF_MINIPET_DIR "/font";
    if (strcasecmp(kind, "AUDIO_META") == 0) return MP_TF_MINIPET_DIR "/audio";
    return NULL;
}

/* 本地 .mpk 是否在位（<kind_dir>/<hash>.mpk）。调用方须持 s_lock（或无并发写） */
static bool file_cached_row(const local_file_t *lf)
{
    const char *dir = kind_dir(lf->kind);
    if (!dir) return false;
    char path[MP_MPK_PATH_MAX];
    /* hash 字段定长 20B（16 hex + NUL），拼接后必然远小于 MP_MPK_PATH_MAX；
     * GCC 看不到 local_file_t.hash 的定长约束 → 局部关掉 format-truncation。 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(path, sizeof(path), "%s/%s.mpk", dir, lf->hash);
#pragma GCC diagnostic pop
    return (access(path, F_OK) == 0);
}

/* 纯可打印 ASCII？（map_id/entity/hash 字段的判据） */
static bool ascii_printable(const char *s)
{
    if (!s || !s[0]) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p > 0x7E) return false;
    }
    return true;
}

/* 【中文化 2026-10-01】可显示串判据：非空 + 无控制字符。
 * 与 ascii_printable 的区别 = **放行 UTF-8 多字节**：服务端 L1 修好后地图
 * label 就是中文原名（"射手村：射手村"），必须原样透传到菜单；
 * 是否缺字（烘焙子集覆盖不到）由菜单层判（lvgl_bridge 的 menu_label_font_safe）。 */
static bool displayable_label(const char *s)
{
    if (!s || !s[0]) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) return false;
    }
    return true;
}

/* §3.2 L2 兜底（用户已拍板）：push 端点没接 WZ 地图名时 label = "map_<纯数字id>"
 * → 剥掉 "map_" 前缀只显示阿拉伯数字原名（"map_000010000" → "000010000"）。
 * 严格 ^map_[0-9]+$ 才剥：真中文名/其它 label 原样返回（不误伤）。 */
static void strip_map_prefix(const char *in, char *out, size_t cap)
{
    if (strncmp(in, "map_", 4) == 0 && in[4] &&
        strspn(in + 4, "0123456789") == strlen(in + 4)) {
        strlcpy(out, in + 4, cap);
        return;
    }
    strlcpy(out, in, cap);
}

/* UTF-8 安全拷贝（截断只退到完整码点边界）：label 缓冲 32B ≈ 10 个汉字，
 * 服务端中文地图名可能更长——strlcpy 会在码点中间截断，留下半个序列，
 * LVGL 拿到非法 UTF-8 渲染成乱码/方块（观感 = "最后一个字是怪字"）。
 * 纯逻辑边界用例已自测（/tmp/menu_logic_test，见汇报）。 */
static void utf8_safe_copy(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    const char *p = in;
    if (!cap) return;
    while (*p && o + 1 < cap) {
        unsigned char c = (unsigned char)*p;
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (o + len + 1 > cap) break;              /* 放不下完整码点 → 到此为止 */
        for (size_t k = 0; k < len && p[k]; k++) out[o++] = p[k];
        p += len;
    }
    out[o] = 0;
}

/* 菜单显示串：① label（可显示：含中文原名；map_ 数字前缀剥除；UTF-8 整码点截断）
 *             → ② map_id/entity(ASCII) → ③ hash 前 8 位 */
static void pick_display_label(const local_file_t *lf, char *out, size_t cap)
{
    if (displayable_label(lf->label)) {
        char t[64];
        strip_map_prefix(lf->label, t, sizeof(t));
        utf8_safe_copy(t, out, cap);
    }
    else if (ascii_printable(lf->map_id))    strlcpy(out, lf->map_id, cap);
    else if (ascii_printable(lf->entity))    strlcpy(out, lf->entity, cap);
    else                                     snprintf(out, cap, "%.8s", lf->hash);
}

/* ------------------------------------------------------------------ */
/* 地图"删除" = 本地隐藏标识（NVS per-map，§4.4）                        */
/* ------------------------------------------------------------------ */
/* 键长上限 = NVS_KEY_NAME_MAX_SIZE-1 = 15 字符；hash 是 16 hex 装不下，
 * 故 map_id（真机为 9 位数字）优先，无 map_id 时用 "h"+hash 前 14 位。 */
#define MP_HIDE_NVS_NS  "maphide"

static void hide_key_of(const local_file_t *lf, char *out, size_t cap)
{
    if (lf->map_id[0] && strlen(lf->map_id) <= 15) {
        strlcpy(out, lf->map_id, cap);
    } else {
        char t[17];
        snprintf(t, sizeof(t), "h%.14s", lf->hash);
        strlcpy(out, t, cap);
    }
}

/* 双键行定位（内容 hash 或地图 id，与 asset_dl_map_path 同口径）。
 * 调用方须持 s_lock；返回 s_files 下标，-1 = 无此 BGMAP。 */
static int find_bgmap_locked(const char *hash_or_id)
{
    if (!hash_or_id || !hash_or_id[0]) return -1;
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcasecmp(s_files[i].kind, "BGMAP") != 0) continue;
        if (strcmp(s_files[i].hash, hash_or_id) == 0) return i;
        if (s_files[i].map_id[0] && strcmp(s_files[i].map_id, hash_or_id) == 0) return i;
    }
    return -1;
}

/* 【活动地图持久化 2026-10-01】hash/id → 地图 id（BGMAP 条目的 map_id）。
 * 用户口径："我设置成神之子神殿调整了位置，重启以后应该还是我选择的地图，
 * 不要重置成默认地图"。持久化必须存 **map_id 而不是 hash**：服务端重导
 * 同一张图会换 hash（同 map_id、不同包），存 hash 的话重启就找不到条目。 */
bool asset_dl_map_id_of(const char *hash_or_id, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_bgmap_locked(hash_or_id);
    if (idx >= 0 && s_files[idx].map_id[0]) {
        strlcpy(out, s_files[idx].map_id, cap);
    }
    xSemaphoreGive(s_lock);
    return out[0] != 0;
}

/* 地图 id 在当前清单里是否存在（启动时校验"上次用的图"是否还可用） */
bool asset_dl_map_exists(const char *map_id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = find_bgmap_locked(map_id) >= 0;
    xSemaphoreGive(s_lock);
    return ok;
}

/* 隐藏标识读取（NVS 单键 u8；键不存在/命名空间不存在 = 未隐藏） */
static bool map_hidden_locked(const local_file_t *lf)
{
    char key[16];
    hide_key_of(lf, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(MP_HIDE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    bool hid = (nvs_get_u8(h, key, &v) == ESP_OK && v != 0);
    nvs_close(h);
    return hid;
}

bool asset_dl_map_hidden(const char *hash_or_id)
{
    bool hid = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_bgmap_locked(hash_or_id);
    if (idx >= 0) hid = map_hidden_locked(&s_files[idx]);
    xSemaphoreGive(s_lock);
    return hid;
}

bool asset_dl_map_set_hidden(const char *hash_or_id, bool hidden)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_bgmap_locked(hash_or_id);
    if (idx >= 0) {
        char key[16];
        hide_key_of(&s_files[idx], key, sizeof(key));
        nvs_handle_t h;
        if (nvs_open(MP_HIDE_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            esp_err_t err = hidden ? nvs_set_u8(h, key, 1) : nvs_erase_key(h, key);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;   /* 解除不存在的标识 = 成功 */
            if (err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
            ok = (err == ESP_OK);
        }
        /* 取证锚点：菜单"删除"与 poller"推送解除"都经这里，一条日志看清置位/解除 */
        ESP_LOGW(TAG, "地图隐藏标识 %s：key=%s hash=%.16s map_id=%s → %s",
                 hidden ? "置位" : "解除", key, s_files[idx].hash,
                 s_files[idx].map_id[0] ? s_files[idx].map_id : "-",
                 ok ? "OK" : "FAIL");
    } else {
        ESP_LOGW(TAG, "地图隐藏标识：%s 无对应 BGMAP 条目（清单未登记？）",
                 hash_or_id ? hash_or_id : "(null)");
    }
    xSemaphoreGive(s_lock);
    return ok;
}

int asset_dl_bgmap_visible_count(void)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcasecmp(s_files[i].kind, "BGMAP") != 0) continue;
        if (map_hidden_locked(&s_files[i])) continue;
        n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}

bool asset_dl_map_is_active(const char *hash_or_id)
{
    bool act = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_bgmap_locked(hash_or_id);
    if (idx >= 0 && s_active_map_hash[0]) {
        act = (strcmp(s_files[idx].hash, s_active_map_hash) == 0);
    }
    xSemaphoreGive(s_lock);
    return act;
}

bool asset_dl_map_key(const char *hash_or_id, char *out, size_t cap)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_bgmap_locked(hash_or_id);
    if (idx >= 0) {
        hide_key_of(&s_files[idx], out, cap);   /* 键口径与隐藏标识/相机 NVS 一致 */
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/* kind 过滤 + cached 标记。调用方须持 s_lock */
static int kind_list_locked(const char *kind, char hashes[][20], char labels[][32],
                            bool *cached, int max)
{
    int n = 0;
    for (int i = 0; i < s_file_cnt && n < max; i++) {
        if (strcasecmp(s_files[i].kind, kind) != 0) continue;
        if (strcasecmp(kind, "PARTS") == 0) {
            /* fontTime 非装扮；地图条带小包（无 selector）也不进换装列表 */
            if (strcmp(s_files[i].selector, "clock") == 0) continue;
            if (!(strcmp(s_files[i].selector, "paperdoll") == 0 ||
                  strncmp(s_files[i].entity, "paperdoll", 9) == 0)) continue;
        }
        /* §4.4：用户隐藏（"删除"）的图不进列表——文件仍在 TF、LRU 照常管 */
        if (strcasecmp(kind, "BGMAP") == 0 && map_hidden_locked(&s_files[i])) continue;
        if (hashes) strlcpy(hashes[n], s_files[i].hash, 20);
        if (labels) pick_display_label(&s_files[i], labels[n], 32);
        if (cached) cached[n] = file_cached_row(&s_files[i]);
        n++;
    }
    return n;
}

int asset_dl_bgmap_list(char hashes[][20], char labels[][32], bool *cached, int max)
{
    if (max <= 0) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = kind_list_locked("BGMAP", hashes, labels, cached, max);
    xSemaphoreGive(s_lock);
    return n;
}

int asset_dl_parts_list(char hashes[][20], char labels[][32], bool *cached, int max)
{
    if (max <= 0) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = kind_list_locked("PARTS", hashes, labels, cached, max);
    xSemaphoreGive(s_lock);
    return n;
}

int asset_dl_npc_list(char entities[][40], char hashes[][20], char labels[][32],
                      bool *cached, int max)
{
    if (max <= 0) return 0;
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt && n < max; i++) {
        if (strcasecmp(s_files[i].kind, "PARTS") != 0) continue;
        if (!(strcmp(s_files[i].selector, "npc") == 0 ||
              strncmp(s_files[i].entity, "npc:", 4) == 0)) continue;
        if (!s_files[i].entity[0]) continue;

        bool dup = false;                       /* entity 去重（1 PARTS + N LAYOUT） */
        for (int k = 0; k < n; k++) {
            if (strcmp(entities[k], s_files[i].entity) == 0) { dup = true; break; }
        }
        if (dup) continue;

        strlcpy(entities[n], s_files[i].entity, 40);
        if (hashes) strlcpy(hashes[n], s_files[i].hash, 20);
        if (labels) pick_display_label(&s_files[i], labels[n], 32);
        if (cached) cached[n] = file_cached_row(&s_files[i]);
        n++;
    }
    xSemaphoreGive(s_lock);
    return n;
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
static uint8_t read_font_px(const char *path);   /* 定义在下；清单缺 px 时兜底读包内首字节 */

/* 【NAN 陷阱 2026-09-27 真机根因】cJSON_GetNumberValue(NULL/非数字) 返回 **NAN**，
 * 强转整数是未定义行为：xtensa 上 (uint8_t)NAN == 255。清单里 FONT 条目不带 px 字段
 * （服务端 ManifestBuilder 只写 hash/kind/bytes/label）→ font_px 被写成 255 →
 * `asset_dl_font_path(16/24/32)` 永不匹配 → **三档字体从未绑定**（真机日志无 set_font 行，
 * 汇总日志实证 `FONT 3[px=255,255,255]`）。所有数字字段一律走本函数取默认值。 */
static char s_render_root[20] = "/sdcard/minipet";  /* 渲染根（含 minipet 段）：TF 常态 / 降级时 /factory/minipet */

static const char *render_kind_dir(const char *kind)
{
    /* 与 kind_dir 同构，但前缀=渲染根（/factory 降级时读内部 Flash 快照） */
    if (strcasecmp(kind, "PARTS") == 0)      return "%s/parts";
    if (strcasecmp(kind, "LAYOUT") == 0)     return "%s/layout";
    if (strcasecmp(kind, "BGMAP") == 0)      return "%s/bg";
    if (strcasecmp(kind, "FONT") == 0)       return "%s/font";
    if (strcasecmp(kind, "AUDIO_META") == 0) return "%s/audio";
    return NULL;
}

/* 渲染侧完整目录（调用方仍需 snprintf 拼 hash）——kind_dir 的渲染根版 */
static char s_rdir_buf[5][32];
static const char *render_dir_of(const char *kind)
{
    static int rot;
    const char *fmt = render_kind_dir(kind);
    if (!fmt) return MP_TF_MINIPET_DIR;
    char *b = s_rdir_buf[rot = (rot + 1) & 3];
    snprintf(b, 32, fmt, s_render_root);
    return b;
}

static double jnum(const cJSON *o, const char *key, double dflt)
{
    if (!o) return dflt;
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return (v && cJSON_IsNumber(v)) ? cJSON_GetNumberValue(v) : dflt;
}

/* 数组元素取数（clock_table 的 [x,y]） */
static double jnum_at(const cJSON *arr, int idx, double dflt)
{
    if (!arr) return dflt;
    const cJSON *v = cJSON_GetArrayItem(arr, idx);
    return (v && cJSON_IsNumber(v)) ? cJSON_GetNumberValue(v) : dflt;
}

static void load_local_manifest(void)
{
    s_file_cnt = 0;
    s_local_rev = 0;
    s_active_map[0] = 0;
    s_active_map_hash[0] = 0;
    s_clock_cnt = 0;

    char mf_path[48];
    snprintf(mf_path, sizeof(mf_path), "%s/manifest.json", s_render_root);
    FILE *f = fopen(mf_path, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 128 * 1024) {
        ESP_LOGW(TAG, "本地清单大小异常 sz=%ld → 弃用", sz);
        fclose(f); return;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "本地清单 JSON 解析失败（读 %zu/%ld 字节）→ 弃用", rd, sz);
        return;
    }

    s_local_rev = (uint32_t)jnum(root, "rev", 0);

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
            if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(jf, "label"))))
                utf8_safe_copy(s, lf->label, sizeof(lf->label));   /* 整码点截断 */
            /* 【锚点 2026-09-27】LAYOUT 条目的 origin=[x,y]（画布内 body 锚点）。
             * 旧固件只拿到 bounds（画布尺寸），摆放只能退化成"画布左上角对齐屏心"，
             * 与"origin 与屏幕中点重合"差 origin×2 px，且换动作时锚点漂移 → 人物跳。
             * 缺字段/旧清单 → 0,0（= 旧行为，不会崩）。 */
            {
                const cJSON *jo = cJSON_GetObjectItem(jf, "origin");
                if (jo && cJSON_IsArray(jo)) {
                    lf->origin_x = (int16_t)jnum_at(jo, 0, 0);
                    lf->origin_y = (int16_t)jnum_at(jo, 1, 0);
                }
            }
            lf->font_px = (uint8_t)jnum(jf, "px", 0);
            /* 【字体档位兜底 2026-09-27】服务端清单的 FONT 条目**不带 px** 字段
             * （只有 hash/kind/bytes/url/label）→ 本地已有素材的 font_px 恒为 0 →
             * `asset_dl_font_path(16/24/32)` 一个都匹配不上 → 三档字体从未绑定
             * （真机日志里 "set_font px=… 开始" 一条都没有，气泡/菜单/时钟只能退化）。
             * 这里在清单没给 px 时直接读本地包内 size_px（payload 首字节）。 */
            /* 【以包内为准 2026-09-27】清单里的 px 可能是**历史坏值被固件自己持久化**回来的
             * （旧 read_font_px 读错偏移 + cJSON 缺字段 NAN→255，save_local_manifest 把
             * 255 写进清单 → 后续每次启动都读到 255，只判 ==0 兜底救不回来）。
             * FONT 条目的档位一律以本地包内 size_px 为准（包在 → 信包；包不在 → 保留清单值）。 */
            if (strcasecmp(lf->kind, "FONT") == 0) {
                const char *fdir = kind_dir("FONT");
                if (fdir) {
                    /* 【堆保护 2026-09-29】低堆期 fopen 会触发 newlib 锁分配
                     * 失败 → abort 启动循环（真机实证）。空闲不足则跳过 px
                     * 读取（回退清单值，纯装饰性字段）。
                     * 【板级配置 2026-09-30】门值改由 Kconfig MP_ASSET_HEAP_GATE_KB
                     * 决定（默认 24KB=216 板保护不变）。原字面量
                     * 8*108(=864B) 是 8*1024 的历史笔误，与下方日志"24KB"自相
                     * 矛盾，一并收敛到同一配置源。 */
                    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) <
                        CONFIG_MP_ASSET_HEAP_GATE_KB * 1024) {
                        ESP_LOGW(TAG, "内部堆 <%dKB，跳过 FONT px 读取（防 newlib abort）",
                                 CONFIG_MP_ASSET_HEAP_GATE_KB);
                    } else {
                    char fp[MP_MPK_PATH_MAX];
                    snprintf(fp, sizeof(fp), "%s/%s.mpk", fdir, lf->hash);
                    uint8_t px = read_font_px(fp);
                    if (px) lf->font_px = px;
                    ESP_LOGI(TAG, "FONT %s：包内 size_px=%u（清单值 %u）",
                             lf->hash, (unsigned)px, (unsigned)lf->font_px);
                    }
                }
            }
            lf->bytes = (uint32_t)jnum(jf, "bytes", 0);
            lf->fav = cJSON_IsTrue(cJSON_GetObjectItem(jf, "fav"));
            lf->last_used_ms = (int64_t)jnum(jf, "ts", 0);
            s_file_cnt++;
        }
    }

    /* 当前图 id（active_map）→ 反解内容 hash：隐藏标识/正在渲染判定要用
     * （清单里 active_map 只存 id，见 save_local_manifest_locked） */
    if (s_active_map[0]) {
        for (int i = 0; i < s_file_cnt; i++) {
            if (strcasecmp(s_files[i].kind, "BGMAP") == 0 &&
                strcmp(s_files[i].map_id, s_active_map) == 0) {
                strlcpy(s_active_map_hash, s_files[i].hash, sizeof(s_active_map_hash));
                break;
            }
        }
    }

    /* 【可观测性 2026-09-27】清单载入结果此前完全静默：条目数、FONT 档位识别情况
     * 都看不见，"字体从未绑定""地图按 id 查不到"这类问题只能靠猜。这里一条汇总。 */
    {
        int n_parts = 0, n_layout = 0, n_bgmap = 0, n_font = 0, n_audio = 0;
        char fx[64] = { 0 };
        for (int i = 0; i < s_file_cnt; i++) {
            const char *k = s_files[i].kind;
            if (!strcasecmp(k, "PARTS")) n_parts++;
            else if (!strcasecmp(k, "LAYOUT")) n_layout++;
            else if (!strcasecmp(k, "BGMAP")) n_bgmap++;
            else if (!strcasecmp(k, "FONT")) {
                n_font++;
                char t[16];
                snprintf(t, sizeof(t), "%s%u", n_font > 1 ? "," : "", (unsigned)s_files[i].font_px);
                strlcat(fx, t, sizeof(fx));
            } else if (!strcasecmp(k, "AUDIO_META")) n_audio++;
        }
        ESP_LOGW(TAG, "本地清单 rev=%u：%d 条（PARTS %d / LAYOUT %d / BGMAP %d / FONT %d[px=%s] / AUDIO_META %d）",
                 (unsigned)s_local_rev, s_file_cnt, n_parts, n_layout, n_bgmap, n_font,
                 n_font ? fx : "-", n_audio);
        /* 【对账 2026-09-30】文件大面积丢失（FAT 损坏/下载中断被清）而清单仍在：
         * 真机实证 54 条清单配 0 个文件 → dispatch 全灭 → 误降级。缺失>60%
         * 时把 s_local_rev 归零——sync 视为全量缺失重下（不再 304 短路）。 */
        {
            int on_disk = 0;
            for (int i = 0; i < s_file_cnt; i++) {
                char pchk[128];
                const char *d = kind_dir(s_files[i].kind);
                if (!d) { on_disk++; continue; }      /* 非文件类不罚 */
                snprintf(pchk, sizeof(pchk), "%s/%.20s.mpk", d, s_files[i].hash);
                if (access(pchk, F_OK) == 0) on_disk++;
            }
            if (s_file_cnt > 0 && on_disk * 10 < s_file_cnt * 4) {   /* <40% 在盘 */
                ESP_LOGW(TAG, "清单%d条但磁盘仅%d个（<40%%）：FAT丢失 → 清 rev 触发全量重下",
                         s_file_cnt, on_disk);
                s_local_rev = 0;
            }
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
                    (int16_t)jnum_at(arr, 0, 0);
                s_clock_tab[s_clock_cnt].y =
                    (int16_t)jnum_at(arr, 1, 0);
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
        if (s_files[i].label[0]) cJSON_AddStringToObject(jf, "label", s_files[i].label);
        if (s_files[i].action[0])   cJSON_AddStringToObject(jf, "action", s_files[i].action);
        if (s_files[i].entity[0])   cJSON_AddStringToObject(jf, "entity", s_files[i].entity);
        if (s_files[i].map_id[0])   cJSON_AddStringToObject(jf, "map", s_files[i].map_id);
        if (s_files[i].selector[0]) cJSON_AddStringToObject(jf, "selector", s_files[i].selector);
        if (s_files[i].font_px)     cJSON_AddNumberToObject(jf, "px", s_files[i].font_px);
        /* 【origin 必须回写 2026-09-27】LAYOUT 的 origin（画布内 body 锚点）由清单下发；
         * 固件在同步后会**重写本地清单**，此前不写 origin ⇒ 字段被抹掉、下次开机
         * 摆放退回"画布左上角对齐屏心"（真机实测：锚点探针打印 origin=(0,0)）。
         * 只在非零时写（(0,0) 即默认值，写了也不影响，但少一个字段更干净）。 */
        if (s_files[i].origin_x || s_files[i].origin_y) {
            cJSON *jo = cJSON_AddArrayToObject(jf, "origin");
            cJSON_AddItemToArray(jo, cJSON_CreateNumber(s_files[i].origin_x));
            cJSON_AddItemToArray(jo, cJSON_CreateNumber(s_files[i].origin_y));
        }
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
/* 单包下载（全量流式：块化模式的回退路径 + 边下边校验）                    */
/* ------------------------------------------------------------------ */
typedef struct {
    FILE    *f;
    uint64_t want_hash;      /* manifest 键（信封 content_hash 必须相等） */
    uint32_t crc;
    uint64_t total;

    /* 【头 40B 2026-09-30】算法规格 §二 / 服务端 Mpak.cs（HeaderSize=40）/
     * 出厂 .mpk 实测三方一致：magic块16(MAGIC4+ver2+flags2+零填充8)
     * + kind8 + content_hash8 + payload_len4 + reserved4。旧 32B 头解析
     * 把 kind/hash/payload_len 各提前 8B 读进零填充/hash/kind → verify 永败。 */
    uint8_t  head[40];
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

    /* 1) 攒头 40 字节（MAGIC/版本/kind/content_hash/payload_len——规格 §二布局） */
    if (c->head_len < sizeof(c->head)) {
        size_t need = sizeof(c->head) - c->head_len;
        size_t take = (len < need) ? len : need;
        memcpy(c->head + c->head_len, data, take);
        c->head_len += take;
        if (c->head_len == sizeof(c->head)) {
            c->head_ok = (memcmp(c->head, "MPAK", 4) == 0);
            c->env_kind = rd_le64(c->head + 16);
            c->env_hash = rd_le64(c->head + 24);
            c->payload_len = rd_le32(c->head + 32);
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

/* FONT 包内 size_px（payload 首字节；**文件偏移 = 信封头 40B** + payload 0）。
 * 【2026-09-27 真机修复】旧代码读文件偏移 32 —— 那是信封里的 payload_len 低字节
 * （16px 包读到 164、32px 读到 0、24px 读到 84）→ s_files[].font_px 恒为垃圾 →
 * `asset_dl_font_path(16/24/32)` 全部匹配不上 → **三档字体从未被绑定**
 * （真机日志里 "set_font px=… 开始" 一条都没有），气泡/列表/时钟只能退化。
 * 校验（主机端对同一 mpk）：offset40 = 16 / 32 / 24 三个包各自正确。 */
static uint8_t read_font_px(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t b[2] = { 0, 0 };
    fseek(f, 40, SEEK_SET);               /* 40 字节信封头之后 = payload 首字节 */
    size_t r = fread(b, 1, 2, f);
    fclose(f);
    return (r == 2) ? b[0] : 0;
}

static bool verify_and_commit(dl_ctx_t *c, const char *dir, const char *hash)
{
    if (!c->head_ok) return false;
    if (c->total != (uint64_t)c->payload_len + 48) return false;   /* 40 头+8 尾 */
    if (c->tail_len != 8) return false;

    uint32_t want_crc = rd_le32(c->tail);
    uint32_t want_zero = rd_le32(c->tail + 4);
    if (want_zero != 0) return false;
    if (want_crc != c->crc) return false;
    if (c->env_hash != c->want_hash) return false;                 /* hash 校验 */

    char final_path[96], tmp_path[104];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s.mpk.tmp", dir, hash);
    snprintf(final_path, sizeof(final_path), "%s/%s.mpk", dir, hash);

    /* 尾部 8B（crc+zero）落盘：流式路径的 trailer 还在滑窗未写盘（盘上=total-8）；
     * 文件级路径（Range 块化收尾）盘上已是完整文件（=total）→ 跳过。
     * 【2026-09-30】mpak.h MPAK_ERR_LEN 要求盘上 = 40+payload+8 —— 旧实现不落
     * 尾部 8B，提交出的 .mpk 渲染侧长度校验必拒。 */
    long disk = -1;
    if (fseek(c->f, 0, SEEK_END) != 0 || (disk = ftell(c->f)) < 0) return false;
    if (disk == (long)c->total - 8) {
        if (fwrite(c->tail, 1, 8, c->f) != 8 || fflush(c->f) != 0) return false;
    } else if (disk != (long)c->total) {
        return false;                     /* 盘上既非 total-8 也非 total：拒绝提交 */
    }

    fclose(c->f);
    c->f = NULL;
    unlink(final_path);
    return rename(tmp_path, final_path) == 0;
}

/* 【陈旧条目剪除 2026-10-01】s_files 必须与服务端清单对账。旧实现只增量
 * upsert：本地清单（出厂 rev1）条目在服务端重新登记/淘汰后 hash 不再被服务，
 * 文件被本地 LRU 淘汰后也永不再下（下载循环只遍历清单），但 parts_path(NULL)
 * 按"第一个 paperdoll 匹配"仍选中它 → open errno=2 → 实体 0x0（真机：rev33
 * 同步后宠物从屏幕消失，critical_ready 却=1，"后台补齐"反复绑死包空转）。
 * 每次拿到完整清单（含 rev 相等早退前）都以服务端 assets 为准剪除不在清单内
 * 的行。须持 s_lock 调用；返回剪除数。 */
static int prune_stale_locked(const cJSON *assets)
{
    int kept = 0, removed = 0;
    for (int i = 0; i < s_file_cnt; i++) {
        if (cJSON_GetObjectItem(assets, s_files[i].hash)) {
            s_files[kept++] = s_files[i];
        } else {
            removed++;
        }
    }
    s_file_cnt = kept;
    if (removed > 0)
        ESP_LOGW(TAG, "清单对账：剪除 %d 个已不被服务的陈旧条目（保留 %d）",
                 removed, kept);
    return removed;
}

/* 元数据登记/刷新（不下载）；返回该 hash 本地是否已有文件。
 * 【label 落库 2026-10-01 中文化前置】服务端 manifest 的 label 此前**从未进设备**
 * （旧签名只有 action/entity/map/selector）→ s_files[].label 恒空 → 菜单只能走
 * ②map_id 兜底显示 "000010000"，服务端 L1（接 WZ 地图名）改了也**到不了屏上**。
 * 现按 upsert 语义落库：服务端给了就覆盖（rev bump 后 L1 修复即生效），
 * 没给则保留本地值（不退化成空）。UTF-8 整码点截断，防中文名截半个字。 */
static bool upsert_meta(const char *hash, const char *kind, const char *action,
                        const char *entity, const char *map_id, const char *selector,
                        const char *label)
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
        if (label && label[0] &&
            strcmp(slot->label, label) != 0) {          /* 变化才写（防无谓重写清单） */
            utf8_safe_copy(label, slot->label, sizeof(slot->label));
        }
        if (found_file && slot->last_used_ms == 0) slot->last_used_ms = mp_now_ms();
    }
    return found_file;
}

/* ------------------------------------------------------------------ */
/* 块化断点续传下载（2026-09-30）                                        */
/* ------------------------------------------------------------------ */
/* 现状痛点：整包一次 GET，链路抖动 20s 超时即整包作废，大文件（765KB 纸娃娃、
 * 1MB 地图）永远下不完。服务端 Range（206+Content-Range+Accept-Ranges）已就绪
 * 但生产部署时间未知——固件必须两种服务端都能跑，状态机如下：
 *
 *   Range 服务端：32KB 块逐块 Range GET，206 切片追加写 tmp（只追加不校验，
 *     dl_ctx 的 crc/尾滑窗口径只对完整流有效）；单块抖动只废单块（零进展重试
 *     ×2，连续 3 次零进展才放弃本轮）——断点 = tmp 现大小，下轮 sync 续传。
 *   无 Range 服务端：off==0 收到 200 → 体即完整文件，照常追加、收尾文件级
 *     校验收工；off>0 收到 200 → 体与断点前缀叠加即垃圾 → 中止读体、清 tmp、
 *     告警、本次转全量模式（整包 GET + dl_chunk 边下边校验，原路径原语义）。
 *
 * 断点无需 sidecar：.mpk.tmp 的现大小就是断点（老固件遗留 tmp 同为干净内容
 * 前缀——dl_chunk 尾滑窗 ≤8B 未落盘，可直接续）。
 * 内存纪律：全程流式——块请求 2KB 读缓冲（与 http_txn 同规格）、收尾校验
 * 1KB 读缓冲，均 malloc/free 即用即还；无任何整块/整包进内存。 */
#define DL_BLK           (32u * 1024u)   /* 块大小：单次 Range 请求窗口 */
#define DL_BLK_TIMEOUT   20000           /* 单块超时：与整包时代一致 20s */
#define DL_BLK_MAX_FAIL  3               /* 连续 3 次零进展 → 放弃本轮 */
#define DL_BAR_CELLS     20              /* 进度条格数（每格 5%） */
#define DL_BLK_ITERS_MAX 4096            /* 保险丝：防病态服务端死循环 */

/* 一次下载任务的静态描述（download_one 构建，逐层传递免长参数表） */
typedef struct {
    const char *hash;
    const char *dir;
    char        api_path[64];
    char        tmp_path[104];
    uint32_t    total;            /* manifest 元数据 bytes；0=未知总量模式 */
    int         seq, seq_total;   /* 进度 [i/N]（sync_once 预统计传入） */
    int         last_status;      /* 最近一次 HTTP 状态码（事件上报口径） */
} dl_job_t;

/* 单次块请求的落盘上下文（range_get 回调） */
typedef struct {
    FILE    *f;
    int      status;      /* range_get 在读体前写回：回调按状态决定落盘/中止 */
    uint32_t start_off;   /* 本次请求起始偏移（200 全量回退判定） */
    uint32_t appended;    /* 本次已追加字节数 */
    bool     clean_eof;   /* 体以 0（EOF）而非 -1（超时/掐断）收束 */
    bool     io_err;      /* 写盘失败 */
    bool     abort;       /* 非 206/合法 200：中止读体（错误页不碰 tmp） */
} blk_get_t;

/* 块落盘回调：只追加、不校验。Range 体是切片，crc/hash 统一挂到全块完成后
 * 的文件级重建（verify_file_and_commit）；200 全量模式走 dl_chunk 现有校验。 */
static bool blk_append_cb(void *ctx_, const char *data, size_t len)
{
    blk_get_t *b = ctx_;
    if (b->abort) return false;
    /* 只落两类体：206 切片；200@off==0（服务端不认 Range → 体=完整文件）。
     * 其余（4xx 错误页 / 200@off>0 叠加垃圾）立即中止，不浪费写盘与磨损。 */
    if (!(b->status == 206 || (b->status == 200 && b->start_off == 0))) {
        b->abort = true;
        return false;
    }
    if (fwrite(data, 1, len, b->f) != len) { b->io_err = true; return false; }
    b->appended += (uint32_t)len;
    return true;
}

/* 带 Range 头的单次 GET。mp_http_get 无自定义头入口（http_client.* 不在本次
 * 改动范围），用 esp_http_client 原地实现同构最小封装：2KB 流式读、即用即还；
 * 返回语义与 mp_http_get 一致（HTTP 状态码 / -1 网络错误）。 */
static int range_get(const char *api_path, uint32_t off, uint32_t end_incl,
                     bool open_ended, uint32_t timeout_ms, blk_get_t *bc)
{
    const char *base = mp_http_server_url();
    if (!base || !base[0]) return -1;

    char url[192];
    snprintf(url, sizeof(url), "%s%s", base, api_path);
    char rng[32];
    if (open_ended) snprintf(rng, sizeof(rng), "bytes=%u-", (unsigned)off);
    else            snprintf(rng, sizeof(rng), "bytes=%u-%u",
                             (unsigned)off, (unsigned)end_incl);

    bc->status    = -1;
    bc->appended  = 0;
    bc->clean_eof = false;
    bc->io_err    = false;
    bc->abort     = false;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = (int)timeout_ms,
        .buffer_size = 2048,
        .keep_alive_enable = true,          /* 与 http_txn 的 GET 同口径 */
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return -1;

    char *chunk = malloc(2048);
    if (!chunk) { esp_http_client_cleanup(h); return -1; }

    int status = -1;
    esp_http_client_set_header(h, "Range", rng);
    if (esp_http_client_open(h, 0) == ESP_OK &&
        esp_http_client_fetch_headers(h) >= 0) {
        status = esp_http_client_get_status_code(h);
        bc->status = status;                /* 体读取前写回（回调按状态落盘） */
        for (;;) {
            int r = esp_http_client_read(h, chunk, 2048);
            if (r == 0) { bc->clean_eof = true; break; }   /* 0=正常 EOF */
            if (r < 0) break;                              /* 超时/中途掐断 */
            if (!blk_append_cb(bc, chunk, (size_t)r)) break;
        }
        esp_http_client_close(h);
    }
    free(chunk);
    esp_http_client_cleanup(h);
    return status;
}

/* 全块完成 → 从 tmp 文件重建 dl_ctx（头 32B/尾 8B/crc 全文件口径）→
 * 走现有 verify_and_commit（MPAK/crc/hash/长度 + 改名，零改动）。
 * 调用方持有 ctx：返回后以 c->f 是否为 NULL 判句柄归属（verify_and_commit
 * 关闭后置 NULL——含"改名失败"这种已关闭却返回 false 的路径，防双重 fclose）。
 * 返回 false 时 tmp 未失效，调用方负责 fclose（若仍开）并清 tmp。 */
static bool verify_file_and_commit(dl_ctx_t *c, const char *dir, const char *hash)
{
    FILE *f = c->f;

    fflush(f);                            /* 写读切换：先冲 stdio 缓冲 */
    if (fseek(f, 0, SEEK_END) != 0) return false;
    long sz = ftell(f);
    if (sz < 48) return false;            /* 至少 40B 信封头 + 8B 尾 */
    c->total = (uint64_t)sz;

    uint8_t head[40];
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fread(head, 1, sizeof(head), f) != sizeof(head)) return false;
    memcpy(c->head, head, sizeof(head));
    c->head_len    = sizeof(head);
    c->head_ok     = (memcmp(head, "MPAK", 4) == 0);
    c->env_kind    = rd_le64(head + 16);
    c->env_hash    = rd_le64(head + 24);
    c->payload_len = rd_le32(head + 32);

    uint8_t tail[8];
    if (fseek(f, sz - 8, SEEK_SET) != 0) return false;
    if (fread(tail, 1, sizeof(tail), f) != sizeof(tail)) return false;
    memcpy(c->tail, tail, sizeof(tail));
    c->tail_len = sizeof(tail);

    /* crc 覆盖 [0, size-8)：与 dl_chunk 流式口径一致（尾部 8B crc 字段不进 crc） */
    uint32_t crc = 0;
    uint8_t *buf = malloc(1024);
    if (!buf) return false;
    if (fseek(f, 0, SEEK_SET) != 0) { free(buf); return false; }
    uint64_t left = (uint64_t)sz - 8;
    while (left > 0) {
        size_t want = (left > 1024) ? 1024 : (size_t)left;
        size_t r = fread(buf, 1, want, f);
        if (r == 0) { free(buf); return false; }
        crc = crc32c_update(crc, buf, r);
        left -= r;
    }
    free(buf);
    c->crc = crc;

    return verify_and_commit(c, dir, hash);
}

/* 全量模式：off>0 收到 200（服务端不认 Range）的回退 + E11 损坏重拉共用。
 * 整包流式 GET + dl_chunk 边下边算 crc（现有回调校验路径）+ verify_and_commit。
 * 返回 1=已 commit；0=失败（网络失败保留 tmp 作断点前缀，其余清 tmp）；
 * -1=损坏已清 tmp（可立即重拉）。 */
static int download_full(dl_job_t *j)
{
    dl_ctx_t c = { 0 };
    c.want_hash = strtoull(j->hash, NULL, 16);
    c.f = fopen(j->tmp_path, "wb");
    if (!c.f) {
        ESP_LOGI(TAG, "✗ %s tmp 打开失败 errno=%d", j->hash, errno);
        return 0;
    }
    int status = mp_http_get(j->api_path, DL_BLK_TIMEOUT, dl_chunk, &c);
    if (status == 200 && verify_and_commit(&c, j->dir, j->hash)) return 1;
    if (c.f) fclose(c.f);
    j->last_status = status;
    if (status == 200) {                  /* 校验失败：损坏（E11） */
        unlink(j->tmp_path);
        return -1;
    }
    if (status != -1) unlink(j->tmp_path);   /* 404 等确定失败：tmp 一并清 */
    /* 网络失败（-1）：tmp 保留（尾滑窗 ≤8B 未落盘，仍是干净内容前缀） */
    return 0;
}

/* manifest 元数据快照：该 hash 声明的字节数（0=未知/未登记） */
static uint32_t lookup_manifest_bytes(const char *hash)
{
    uint32_t b = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].hash, hash) == 0) { b = s_files[i].bytes; break; }
    }
    xSemaphoreGive(s_lock);
    return b;
}

/* 块化主体。返回 1=已 commit；0=失败断点保留（下轮 sync 续传）；
 * -1=损坏 tmp 已清（E11 重拉）。j 可变字段（last_status）在途中回写。 */
static int download_blocks(dl_job_t *j, uint32_t resumed)
{
    uint64_t want_hash = strtoull(j->hash, NULL, 16);
    uint32_t blk_total = j->total ? (j->total + DL_BLK - 1) / DL_BLK : 0;
    uint32_t off = resumed;
    int      fails = 0;
    int      st = -1;

    /* 断点续传：追加写（IDF FATFS VFS 对 O_APPEND 逐次 write 前 seek 到尾，
     * 已核 vfs_fat.c fat_mode_conv/vfs_fat_write）"+" 允许读（收尾校验） */
    FILE *f = fopen(j->tmp_path, resumed ? "ab+" : "wb+");
    if (!f) {
        ESP_LOGI(TAG, "✗ %s tmp 打开失败 errno=%d", j->hash, errno);
        return 0;
    }

    for (int iters = 0; ; iters++) {
        if (j->total && off >= j->total) break;           /* 收尾 → 文件级校验 */
        if (iters >= DL_BLK_ITERS_MAX) {
            ESP_LOGI(TAG, "✗ %s 块迭代超限 status=%d 已下%uKB，下轮续传",
                     j->hash, st, (unsigned)(off / 1024));
            fclose(f);
            return 0;
        }

        bool open_ended = (j->total == 0);
        uint32_t remain = j->total - off;                 /* ≥1（off<total） */
        uint32_t end = open_ended ? 0
                     : ((remain < DL_BLK) ? (j->total - 1) : (off + DL_BLK - 1));

        blk_get_t bc = { .f = f, .start_off = off };
        st = range_get(j->api_path, off, end, open_ended, DL_BLK_TIMEOUT, &bc);
        j->last_status = st;

        if (bc.io_err) {                                  /* 卡满/坏卡写盘失败 */
            ESP_LOGI(TAG, "✗ %s 写盘失败 status=%d 已下%uKB",
                     j->hash, st, (unsigned)(off / 1024));
            fclose(f);
            return 0;                                     /* tmp 保留 */
        }
        if (bc.status == 200 && bc.start_off > 0) {
            /* 服务端不认 Range：读体已中止（未落盘）→ 清断点、告警、转全量 */
            fclose(f);
            unlink(j->tmp_path);
            ESP_LOGW(TAG, "%s 服务端无 Range（200 @断点%uKB）→ 清断点转全量",
                     j->hash, (unsigned)(bc.start_off / 1024));
            return download_full(j);
        }

        if (bc.appended > 0) {
            off += bc.appended;
            fails = 0;
            /* 显眼进度：已知总量 → 20 格进度条（每 4 块/末块一拍）；
             * 未知总量 → 定期拍已下字节 */
            uint32_t blk_idx = (off + DL_BLK - 1) / DL_BLK;
            if (j->total) {
                if (off >= j->total || (blk_idx % 4) == 0) {
                    int pct = (int)((uint64_t)off * 100 / j->total);
                    int filled = (int)((uint64_t)off * DL_BAR_CELLS / j->total);
                    if (filled > DL_BAR_CELLS) filled = DL_BAR_CELLS;
                    char bar[DL_BAR_CELLS * 3 + 1];
                    char *p = bar;
                    for (int i = 0; i < DL_BAR_CELLS; i++) {
                        memcpy(p, (i < filled) ? "█" : "░", 3);
                        p += 3;
                    }
                    *p = 0;
                    ESP_LOGI(TAG, "⬇ %s %s %d%% %u/%u块", j->hash, bar, pct,
                             (unsigned)blk_idx, (unsigned)blk_total);
                }
            } else if ((iters % 4) == 3) {
                ESP_LOGI(TAG, "⬇ %s 已下%uKB（总量未知）",
                         j->hash, (unsigned)(off / 1024));
            }
        } else if (++fails >= DL_BLK_MAX_FAIL) {
            /* 连续 3 次零进展 → 放弃本轮：断点保留，下轮 sync 从断点续 */
            if (j->total)
                ESP_LOGI(TAG, "✗ %s 块%u/%u status=%d 已下%uKB，下轮续传",
                         j->hash, (unsigned)(off / DL_BLK + 1),
                         (unsigned)blk_total, st, (unsigned)(off / 1024));
            else
                ESP_LOGI(TAG, "✗ %s status=%d 已下%uKB，下轮续传",
                         j->hash, st, (unsigned)(off / 1024));
            fclose(f);
            return 0;
        }

        /* 收尾判定：已知总量下满；未知总量下 206 开尾（或 200 全量）干净走完。
         * 206 零体（恰在 EOF 续传）也算到站 → 一并交文件级校验裁决。 */
        bool complete;
        if (j->total) complete = (off >= j->total);
        else complete = (bc.clean_eof &&
                         (bc.status == 206 ||
                          (bc.status == 200 && bc.start_off == 0)));
        if (complete) break;
    }

    dl_ctx_t vc = { 0 };
    vc.want_hash = want_hash;
    vc.f = f;
    if (verify_file_and_commit(&vc, j->dir, j->hash)) return 1;
    if (vc.f) fclose(vc.f);        /* 校验失败句柄仍开；改名失败已关闭（NULL） */
    unlink(j->tmp_path);
    return -1;                     /* 损坏（MAGIC/crc/hash/长度）→ E11 重拉 */
}

static bool download_one(const char *hash, const char *kind, int seq, int seq_total)
{
    const char *dir = kind_dir(kind);
    if (!dir) return false;   /* 未知 kind：登记元数据但跳过（proto 前向兼容） */

    dl_job_t j = { .hash = hash, .dir = dir, .seq = seq, .seq_total = seq_total,
                   .last_status = -1 };
    snprintf(j.api_path, sizeof(j.api_path), "/api/device/asset/%s", hash);
    snprintf(j.tmp_path, sizeof(j.tmp_path), "%s/%s.mpk.tmp", dir, hash);
    j.total = lookup_manifest_bytes(hash);

    /* 断点 = tmp 文件现大小（stat 即得，天然续传） */
    struct stat st;
    uint32_t resumed = 0;
    if (stat(j.tmp_path, &st) == 0 && st.st_size > 0) {
        resumed = (uint32_t)st.st_size;
        if (j.total && resumed > j.total) {   /* 前缀比声明还长：清单换血，弃 */
            ESP_LOGW(TAG, "⬇ %s 断点 %uKB 超过声明 %uKB → 弃前缀重下",
                     hash, (unsigned)(resumed / 1024), (unsigned)(j.total / 1024));
            unlink(j.tmp_path);
            resumed = 0;
        }
    }

    char note[40] = "";
    if (resumed) snprintf(note, sizeof(note), " 断点=%uKB", (unsigned)(resumed / 1024));
    if (j.total)
        ESP_LOGI(TAG, "⬇ [%d/%d] %s (%uKB)%s", seq, seq_total, hash,
                 (unsigned)(j.total / 1024), note);
    else
        ESP_LOGI(TAG, "⬇ [%d/%d] %s (?)%s", seq, seq_total, hash, note);

    int64_t t0 = esp_timer_get_time();
    for (int attempt = 0; attempt < 2; attempt++) {   /* 损坏重拉一次（E11） */
        int r = download_blocks(&j, resumed);
        if (r == 1) {
            uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
            char fin[104];
            snprintf(fin, sizeof(fin), "%s/%s.mpk", dir, hash);
            uint32_t fsz = (stat(fin, &st) == 0) ? (uint32_t)st.st_size : j.total;
            char dn[40] = "";
            if (resumed) snprintf(dn, sizeof(dn), " 续传%uKB", (unsigned)(resumed / 1024));
            ESP_LOGI(TAG, "✅ %s %uKB %u.%us%s", hash, (unsigned)(fsz / 1024),
                     (unsigned)(ms / 1000), (unsigned)((ms % 1000) / 100), dn);
            return true;                         /* 字号/时间戳在锁内登记（sync_once） */
        }
        if (r == 0) {
            /* 网络/写盘：断点已保留，下轮 sync 从断点续（事件/表情口径同整包时代） */
            mp_post_event_simple(MP_EVT_ASSET_ERROR, j.last_status, 0, hash);
            input_trigger_expression(MP_EXPR_DAM, 2000);
            return false;
        }
        /* r == -1：损坏（crc/hash/MAGIC）已清 tmp → 事件 + dam 表情 + 重拉一次 */
        mp_post_event_simple(MP_EVT_ASSET_ERROR, j.last_status, 0, hash);
        input_trigger_expression(MP_EXPR_DAM, 2000);
        ESP_LOGW(TAG, "asset %s corrupt (status=%d), retry", hash, j.last_status);
        resumed = 0;                             /* tmp 已清：重拉从零开始 */
    }
    return false;
}

/* 下载成功后登记（不下载）：最近使用时间 + FONT 包内字号。
 * 调用方须持 s_lock（与 sync_once 的登记块同口径，抽出供单包下载复用） */
static void note_download_locked(const char *hash, const char *kind)
{
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].hash, hash) != 0) continue;
        s_files[i].last_used_ms = mp_now_ms();
        if (strcasecmp(kind, "FONT") == 0) {
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
            /* 【E11 修复 2026-09-27】需求「TF 卡满：按淘汰策略清」+「所有降级事件
             * 上报服务端（Web 可见设备健康状态）」——此前淘汰只打串口日志，
             * 服务端/Web 完全看不到。现上报一条事件（type=error, data.s=tf_evict,
             * data.a=被淘汰文件的 kind 名哈希长度无关，借 a 传剩余空间水位）。 */
            mp_post_event_simple(MP_EVT_ERROR, (int32_t)s_file_cnt, 0, "tf_evict");
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
    bool   truncated;      /* 超限截断标记（上层据此明确报错，不再静默解析失败） */
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
    /* 【禁止静默】截断 = 本轮清单作废（JSON 必不完整）。旧实现只 return false，
     * 上层见 status=200 仍去 parse → 失败 → 静默 return，故障完全不可见。 */
    if (!r->truncated) {
        r->truncated = true;
        ESP_LOGE(TAG, "清单响应超限（已收 %u B ≥ 上限 %u B）→ 本轮作废；"
                      "请抬高 MANIFEST_RESP_CAP（清单只增不减）",
                 (unsigned)(r->len + len), (unsigned)r->cap);
    }
    return false;
}

/* 【出厂素材模式（TF 卡缺失）下不下载 2026-09-27】真机根因：TF 卡损坏时
 * 每次 sync_once 都会把服务端 manifest 与本地对表 → 发现缺包 → 逐个下载 →
 * 每次都因无卡写盘失败 → 60s 周期 + poll 触发反复重试。每次失败都占用
 * socket 与 HTTP 内部缓冲，而本板运行期内部堆只剩 ~2.5KB →
 * `probe: socket 失败 errno=105 (No ENOBUFS)` → 心跳/指令/播放全部断续。
 * 出厂素材模式下的正确语义是：**只用内部 Flash 出厂快照，不尝试写盘**。 */
static bool asset_dl_download_allowed(void)
{
    /* 【死结修复 2026-09-30】旧判据=出厂模式即禁下载——空卡降级后 rev 永不
     * 拉新（预设推送全灭，真机实证 allowed=0 死循环）。新判据=TF 物理在位：
     * 下载写 TF（/sdcard 仍挂 TF），渲染暂用出厂分区不受影响；TF 真不在
     * （挂载失败回退）才禁——素材无处可写。 */
    extern bool sd_tf_tf_present(void);
    return sd_tf_tf_present();
}

static volatile bool s_sync_attempted;   /* 首次 sync_once 已出结果（成败均算） */
static volatile int  s_missing;         /* 本轮同步下载失败数（关键文件校验用） */
static volatile bool s_sync_busy;       /* sync_once 全程（含下载落盘）在飞：
                                          * 空卡降级切分区必须等它空闲，否则卸载
                                          * TF 会撕裂在途下载（真机实证竞态） */

bool asset_dl_sync_idle(void)
{
    return !s_sync_busy;
}

bool asset_dl_critical_ready(void)
{
    /* 关键素材以【下载根 /sdcard】落盘为准（与 sync 门同口径） */
    char pdir[64], ldir[64], ph[128], lh[128];
    snprintf(pdir, sizeof(pdir), "%s/parts", MP_TF_MINIPET_DIR);
    snprintf(ldir, sizeof(ldir), "%s/layout", MP_TF_MINIPET_DIR);
    bool parts_ok = false, stand1_ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcasecmp(s_files[i].kind, "PARTS") == 0 &&
            strcmp(s_files[i].selector, "paperdoll") == 0) {
            snprintf(ph, sizeof(ph), "%s/%.20s.mpk", pdir, s_files[i].hash);
            parts_ok = (access(ph, F_OK) == 0);
        }
        if (strcasecmp(s_files[i].kind, "LAYOUT") == 0 &&
            strcmp(s_files[i].action, "stand1") == 0 &&
            strncmp(s_files[i].entity, "paperdoll", 9) == 0) {
            snprintf(lh, sizeof(lh), "%s/%.20s.mpk", ldir, s_files[i].hash);
            stand1_ok = (access(lh, F_OK) == 0);
        }
    }
    xSemaphoreGive(s_lock);
    /* 【诊断】降级判定为什么失败：parts/stand1 查到的路径与文件是否存在 */
    ESP_LOGW(TAG, "critical_ready=%d parts=[%s]%d stand1=[%s]%d",
             (int)(parts_ok && stand1_ok),
             ph, (int)(access(ph, F_OK) == 0),
             lh, (int)(access(lh, F_OK) == 0));
    return parts_ok && stand1_ok;
}

static void sync_once(void)
{
    s_sync_busy = true;
    s_missing = 0;
    ESP_LOGW(TAG, "sync_once 进入（allowed=%d）", (int)asset_dl_download_allowed());
    if (!asset_dl_download_allowed()) {
        static int64_t s_last_log_ms;
        int64_t now = esp_timer_get_time() / 1000;
        if (now - s_last_log_ms > 60000) {      /* 60s 限频，避免刷屏 */
            s_last_log_ms = now;
            ESP_LOGW(TAG, "出厂素材模式（无 TF 卡）：跳过清单同步与下载，只用内部 Flash 快照");
        }
        s_sync_attempted = true;
        return;
    }
    /* 【堆纪律 2026-09-29】缓冲改为 sync 窗口内临时分配、解析后立刻释放。
     * 旧 static 常驻 16K：本板内部堆 143KB，联网+同步窗口期常驻占用直接把
     * 堆压到 2.4KB——SD 读扇区 0x101、mpak 全灭、降级切分区失败连环炸。 */
    char *resp = heap_caps_malloc(MANIFEST_RESP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) resp = malloc(MANIFEST_RESP_CAP);      /* PSRAM 失败才退内部堆 */
    if (!resp) { ESP_LOGE(TAG, "manifest 缓冲分配失败 %u", (unsigned)MANIFEST_RESP_CAP); s_sync_busy = false; s_sync_attempted = true; return; }
    manifest_ctx_t ctx = { .buf = resp, .cap = MANIFEST_RESP_CAP, .truncated = false };

    /* manifest 端点服务端必填 deviceId（DeviceEndpoints.cs HandleManifest 签名
     * string deviceId；缺失即 400）——真机实证：不带参永久 400 → 素材包一个
     * 都下不到、rev 永不跟随。deviceId 由 hello 返回并缓存在 http_client。 */
    char mpath[128];
    snprintf(mpath, sizeof(mpath), "/api/device/manifest?deviceId=%s",
             mp_http_device_id());
    int status = mp_http_get(mpath, 10000, manifest_collect, &ctx);
    s_sync_attempted = true;              /* 供 boot 的空卡降级等待判停 */
    if (status != 200) {
        free(resp);
        ESP_LOGW(TAG, "manifest 拉取失败 status=%d（deviceId=%s）→ 空卡将降级出厂素材",
                 status, mp_http_device_id());
        s_sync_busy = false;
        return;
    }

    cJSON *root = cJSON_Parse(resp);
    heap_caps_free(resp);                  /* cJSON 树自持数据，大缓冲即刻归还堆 */
    resp = NULL;
    if (!root) {
        ESP_LOGE(TAG, "清单 JSON 解析失败（响应 %u B%s）→ 本轮放弃，保持本地 rev=%u",
                 (unsigned)ctx.len, ctx.truncated ? "，已截断" : "", (unsigned)s_local_rev);
        s_sync_busy = false;
        return;
    }

    uint32_t rev = (uint32_t)jnum(root, "rev", 0);

    xSemaphoreTake(s_lock, portMAX_DELAY);

    cJSON *a0 = cJSON_GetObjectItem(root, "assets");
    if (rev == s_local_rev && s_local_rev != 0 && s_file_cnt > 0) {
        /* rev 相等也要对账：TF manifest.json 可能是旧固件写入的混入陈旧条目
         * 的版本，不清则 parts_path 永远选中已被服务的死 hash（真机实证）。 */
        bool pruned = false;
        if (cJSON_IsObject(a0)) {
            pruned = prune_stale_locked(a0) > 0;
            if (pruned) save_local_manifest_locked();
        }
        if (!pruned) {
            xSemaphoreGive(s_lock);
            cJSON_Delete(root);
            s_sync_busy = false;
            return;                              /* 304 语义：无变化 */
        }
        /* 剪掉了陈旧条目：不早退，继续走完整路径（关键文件门+热切回+
         * MANIFEST_SYNCED 重绑通知）——否则绑死在死 hash 上的渲染永远没人
         * 纠正（真机：宠物消失→全黑屏，rev 不变就永远好不了）。 */
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
                    (int16_t)jnum_at(xy, 0, 0);
                s_clock_tab[s_clock_cnt].y =
                    (int16_t)jnum_at(xy, 1, 0);
                s_clock_cnt++;
            }
        }
    }

    /* 条目元数据 + 缺包下载（持锁登记元数据；下载在锁外做 IO） */
    cJSON *assets = cJSON_IsObject(a0) ? a0 : cJSON_GetObjectItem(root, "assets");
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
                        cJSON_GetStringValue(cJSON_GetObjectItem(ja, "selector")),
                        cJSON_GetStringValue(cJSON_GetObjectItem(ja, "label")));
        }
        prune_stale_locked(assets);              /* 以本轮清单为准对账剪除 */
        xSemaphoreGive(s_lock);

        /* 【进度口径 2026-09-30】预统计本轮待下载条数作 [i/N] 显示。
         * 第二次 access(F_OK) 扫描代价：FAT 每条 μs 级，可忽略。 */
        int dl_total = 0;
        cJSON_ArrayForEach(ja, assets) {
            const char *hash = ja->string;
            const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(ja, "kind"));
            if (!hash || !kind) continue;
            const char *d = kind_dir(kind);
            if (!d) continue;
            char p[MP_MPK_PATH_MAX];
            snprintf(p, sizeof(p), "%s/%s.mpk", d, hash);
            if (access(p, F_OK) != 0) dl_total++;
        }
        int dl_seq = 0;

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
            /* 板上链路时通时断（真机实证）：单次失败重试至多 2 次。
             * download_one 内部块级断点保留 → 这里的重试天然从断点续。 */
            bool got = false;
            dl_seq++;
            for (int att = 0; att < 3 && !got; att++) {
                if (download_one(hash, kind, dl_seq, dl_total)) {
                    got = true;
                } else if (att < 2) {
                    vTaskDelay(pdMS_TO_TICKS(300));
                }
            }
            if (!got) { s_missing++; continue; }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            note_download_locked(hash, kind);
            xSemaphoreGive(s_lock);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }

    s_local_rev = rev;
    save_local_manifest_locked();
    xSemaphoreGive(s_lock);

    cJSON_Delete(root);

    /* 【关键文件落盘校验 2026-09-29】元数据登记 ≠ 文件在盘。旧逻辑无条件
     * post MANIFEST_SYNCED → dispatch 查表命中但开文件失败 → FATAL
     * （真机实证：清单 GET 秒过、资产 GET 时通时断，765KB 纸娃娃下不动）。
     * 纸娃娃 PARTS 或 stand1 LAYOUT 缺失 → 不 post，留给下一轮同步重试。 */
    /* 门走【下载根 /sdcard】（TF 落盘为准），与渲染根解耦 */
    char pchk[MP_MPK_PATH_MAX], pdir[64], ldir[64];
    snprintf(pdir, sizeof(pdir), "%s/parts", MP_TF_MINIPET_DIR);
    snprintf(ldir, sizeof(ldir), "%s/layout", MP_TF_MINIPET_DIR);
    char ph[128] = "", lh[128] = "";
    bool parts_ok = false, stand1_ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcasecmp(s_files[i].kind, "PARTS") == 0 &&
            strcmp(s_files[i].selector, "paperdoll") == 0) {
            snprintf(ph, sizeof(ph), "%s/%.20s.mpk", pdir, s_files[i].hash);
            parts_ok = (access(ph, F_OK) == 0);
        }
        if (strcasecmp(s_files[i].kind, "LAYOUT") == 0 &&
            strcmp(s_files[i].action, "stand1") == 0 &&
            strncmp(s_files[i].entity, "paperdoll", 9) == 0) {
            snprintf(lh, sizeof(lh), "%s/%.20s.mpk", ldir, s_files[i].hash);
            stand1_ok = (access(lh, F_OK) == 0);
        }
    }
    xSemaphoreGive(s_lock);
    if (!parts_ok || !stand1_ok) {
        ESP_LOGW(TAG, "关键素材缺失（parts=%d stand1=%d missing=%d）→ 本轮不通知渲染，等下轮同步",
                 (int)parts_ok, (int)stand1_ok, s_missing);
        s_sync_busy = false;
        return;
        s_sync_busy = false;
        return;
    }
    if (s_missing > 0) {
        ESP_LOGW(TAG, "本轮缺 %d 个非关键资产（地图条带/缩略图等），已通知渲染先起播", s_missing);
    }

    /* 【热切回 2026-09-30】关键素材已在 TF 落盘：渲染根从出厂分区切回 /sdcard
     * （重读 TF manifest 重建文件表），随后的 MANIFEST_SYNCED 重绑即用新素材
     * ——预设推送→下载→自动换装的完整闭环。 */
    if (asset_dl_render_root_is_factory()) {
        strlcpy(s_render_root, "/sdcard/minipet", sizeof(s_render_root));
        asset_dl_reload_local();
        ESP_LOGW(TAG, "TF 素材齐备 → 渲染根切回 /sdcard（rev=%lu file_cnt=%d）",
                 (unsigned long)s_local_rev, s_file_cnt);
    }

    /* 素材就绪 → 渲染层重读/重绑（render 任务执行：fonts/parts/stand1） */
    mp_cmd_t c = { .type = MP_CMD_MANIFEST_SYNCED };
    mp_post_cmd(&c);
    s_sync_busy = false;
}

/* ---------------- T4 单包下载（菜单点选未缓存条目 → 下载 → 上层切换）-------
 * 复用既有资产任务与 download_one（不新造任务/队列）：请求写 s_one_hash 后
 * give(s_sync_req) 唤醒任务；任务侧单包优先于全量 sync。 */

/* 取出待下单个包（取出即置空）；调用方给缓冲 ≥20 字节 */
static bool take_one_request(char *out, size_t cap)
{
    bool got = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_one_hash[0]) {
        strlcpy(out, s_one_hash, cap);
        s_one_hash[0] = 0;
        got = true;
    }
    xSemaphoreGive(s_lock);
    return got;
}

bool asset_dl_request_one(const char *hash)
{
    if (!hash || strlen(hash) < 8) return false;
    if (!s_sync_req || !s_lock) return false;      /* 任务未启动（asset_dl_start 之前） */

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].hash, hash) == 0) { idx = i; break; }
    }
    if (idx < 0 || !kind_dir(s_files[idx].kind)) {
        xSemaphoreGive(s_lock);
        return false;                              /* 未登记 / THUMB 等无目录 kind */
    }
    if (file_cached_row(&s_files[idx])) {          /* 本地已有：视为已完成 */
        xSemaphoreGive(s_lock);
        return true;
    }
    strlcpy(s_one_hash, s_files[idx].hash, sizeof(s_one_hash));
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "request_one %s 入队", hash);
    xSemaphoreGive(s_sync_req);
    return true;
}

/* 单包下载执行体（资产任务上下文）：查 kind → 水位检查 → download_one → 登记 */
static void one_request_run(const char *hash)
{
    if (!asset_dl_download_allowed()) return;   /* 出厂素材模式：不写盘 */
    char kind[12] = { 0 };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].hash, hash) == 0) {
            strlcpy(kind, s_files[i].kind, sizeof(kind));
            break;
        }
    }
    xSemaphoreGive(s_lock);
    if (!kind[0]) return;                          /* 清单变更/已淘汰：静默放弃 */

    evict_if_needed();
    if (!download_one(hash, kind, 1, 1)) {   /* [1/1]：单包请求无队列序号口径 */
        ESP_LOGW(TAG, "request_one %s 下载失败（菜单侧将轮询超时）", hash);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    note_download_locked(hash, kind);
    save_local_manifest_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "request_one %s 落盘完成", hash);
}

static void asset_dl_task(void *arg)
{
    (void)arg;
    char one[20];
    for (;;) {
        if (xSemaphoreTake(s_sync_req, pdMS_TO_TICKS(60000)) == pdTRUE) {
            while (xSemaphoreTake(s_sync_req, 0) == pdTRUE) {}   /* 合并重复请求 */
            s_sync_again = false;                                /* 本次唤醒消费掉同步需求 */
            if (take_one_request(one, sizeof(one))) {
                one_request_run(one);        /* T4：菜单点选的单包优先（不吃掉同批 sync） */
                if (s_sync_again) sync_once();
                continue;
            }
            sync_once();
        } else {
            sync_once();      /* 周期兜底：60s 无触发也对表一次服务端 rev */
        }
    }
}

/* ------------------------------------------------------------------ */
/* 查询面（app_cmd_dispatch 于 render 任务内调用；只读内存表）           */
/* ------------------------------------------------------------------ */
static esp_timer_handle_t s_task_retry_timer;
static void asset_dl_task_retry_cb(void *arg);
static bool asset_dl_spawn_ladder(void);

void asset_dl_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_sync_req = xSemaphoreCreateBinary();
    /* 清单同步加载：state_machine_boot 紧随其后查 have_local_manifest，
     * 必须在返回前就绪（否则自检竞态 → 该起播却判空） */
    ensure_dirs();
    crc32c_init_table();
    load_local_manifest();
    /* 【诊断】TF 素材目录实况：文件数 + 前 5 个文件名（对账清单 vs 磁盘） */
    {
        int n = 0;
        const char *dirs[] = { "parts", "layout", "bg", "font" };
        for (int d = 0; d < 4; d++) {
            char dp[48];
            snprintf(dp, sizeof(dp), "/sdcard/minipet/%s", dirs[d]);
            DIR *dr = opendir(dp);
            if (!dr) { ESP_LOGW(TAG, "dir[%s]: 无", dirs[d]); continue; }
            struct dirent *e; int c = 0; char head[96] = "";
            while ((e = readdir(dr)) != NULL) {
                if (e->d_name[0] == '.') continue;
                if (c < 3) { strlcat(head, e->d_name, sizeof(head)); strlcat(head, " ", sizeof(head)); }
                c++; n++;
            }
            closedir(dr);
            ESP_LOGW(TAG, "dir[%s]: %d 个：%s", dirs[d], c, head);
        }
        ESP_LOGW(TAG, "TF 素材总文件数=%d", n);
    }
    /* 任务创建失败=同步/下载全灭（真机实证 2026-09-29：内部堆启动挤压窗口
     * 8K 栈静默失败 → sync_once 永不执行 → 黑屏 60s 后误降级）。栈阶梯
     * （同 render 的降档思路）+ 10s 周期自愈定时器双保险。 */
    if (!asset_dl_spawn_ladder()) {
        ESP_LOGW(TAG, "asset_dl 任务全阶梯首建失败（内部空闲=%u 最大块=%u）→ 10s 周期自愈",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        const esp_timer_create_args_t t = {
            .callback = asset_dl_task_retry_cb,
            .name = "asset_dl_retry",
        };
        if (esp_timer_create(&t, &s_task_retry_timer) == ESP_OK) {
            esp_timer_start_periodic(s_task_retry_timer, 10ULL * 1000000ULL);
        }
    }
}

/* 栈阶梯：4096（1.85B 小堆板首选：内部堆须留给 RAMless 刷新+绑定门）→
 * 8192（宽裕）→ 6144 → 5120（勉强）。任务只做 fopen/fread+cJSON 轻解析；
 * 大堆板首轮 8192 命中行为不变。 */
static bool asset_dl_spawn_ladder(void)
{
    static const uint32_t stacks[] = { 8192, 6144, 5120 };
    for (int i = 0; i < (int)(sizeof(stacks) / sizeof(stacks[0])); i++) {
        if (xTaskCreatePinnedToCore(asset_dl_task, "asset_dl", stacks[i], NULL,
                                    2 /* 低于 poller——4.1 */, NULL,
                                    0 /* PRO */) == pdPASS) {
            ESP_LOGI(TAG, "asset_dl 任务就绪（栈 %u）", (unsigned)stacks[i]);
            return true;
        }
    }
    return false;
}

static void asset_dl_task_retry_cb(void *arg)
{
    (void)arg;
    if (asset_dl_spawn_ladder()) {
        ESP_LOGI(TAG, "asset_dl 任务延迟创建成功（内部堆已回稳）");
        esp_timer_stop(s_task_retry_timer);
        esp_timer_delete(s_task_retry_timer);
        s_task_retry_timer = NULL;
    }
}

bool asset_dl_sync_attempted(void)
{
    return s_sync_attempted;
}

bool asset_dl_render_use_factory(void)
{
    if (!sd_factory_mount_secondary()) return false;   /* Flash 回退态：渲染根已是工厂 */
    strlcpy(s_render_root, "/factory/minipet", sizeof(s_render_root));
    asset_dl_reload_local();
    ESP_LOGW(TAG, "渲染根切至 /factory（file_cnt=%d）；下载继续写 /sdcard", s_file_cnt);
    return true;
}

bool asset_dl_render_root_is_factory(void)
{
    return strcmp(s_render_root, "/factory/minipet") == 0;
}

void asset_dl_reload_local(void)
{
    /* 【空卡降级配套】切到内部 Flash 出厂素材分区后重读磁盘 manifest.json，
     * 重建内存文件表（asset_dl_start 只在启动时读一次）。持锁防并发查询。 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    load_local_manifest();
    ESP_LOGW(TAG, "reload: 装载后 file_cnt=%d rev=%lu", s_file_cnt, (unsigned long)s_local_rev);
    xSemaphoreGive(s_lock);
}

void asset_dl_request_sync(void)
{
    if (!s_sync_req) return;
    s_sync_again = true;      /* 单包请求与全量同步同批到达时不丢全量（见 asset_dl_task） */
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

bool asset_dl_file_cached(const char *hash)
{
    if (!hash || !hash[0]) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcmp(s_files[i].hash, hash) == 0) {
            ok = file_cached_row(&s_files[i]);
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool asset_dl_layout_cached(const char *action)
{
    if (!action || !action[0]) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* 选取口径与 asset_dl_layout_path 一致：paperdoll 实体优先，否则任一 */
    int pd = -1, fb = -1;
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcasecmp(s_files[i].kind, "LAYOUT") != 0) continue;
        if (strcmp(s_files[i].action, action) != 0) continue;
        if (strncmp(s_files[i].entity, "paperdoll", 9) == 0) { pd = i; break; }
        if (fb < 0) fb = i;
    }
    int pick = (pd >= 0) ? pd : fb;
    if (pick >= 0) ok = file_cached_row(&s_files[pick]);
    xSemaphoreGive(s_lock);
    return ok;
}

bool asset_dl_layout_path(const char *action, char *path, size_t cap)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int fallback = -1;
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcasecmp(s_files[i].kind, "LAYOUT") != 0) continue;
        if (strcmp(s_files[i].action, action) != 0) continue;
        if (strncmp(s_files[i].entity, "paperdoll", 9) == 0) {
            const char *dir = render_dir_of("LAYOUT");
            snprintf(path, cap, "%s/%s.mpk", dir, s_files[i].hash);
            ok = true;
            break;
        }
        if (fallback < 0) fallback = i;
    }
    if (!ok && fallback >= 0) {
        const char *dir = render_dir_of("LAYOUT");
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
    /* hash 直查优先（SET_PARTS 命令传 16 hex hash） */
    if (entity_or_null && strlen(entity_or_null) >= 8) {
        for (int i = 0; i < s_file_cnt; i++) {
            if (strcasecmp(s_files[i].kind, "PARTS") != 0) continue;
            if (strcasecmp(s_files[i].hash, entity_or_null) == 0) {
                snprintf(path, cap, "%s/%s.mpk", render_dir_of("PARTS"),
                         s_files[i].hash);
                xSemaphoreGive(s_lock);
                return true;
            }
        }
    }
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcasecmp(s_files[i].kind, "PARTS") != 0) continue;
        if (strcmp(s_files[i].selector, "clock") == 0) continue;   /* fontTime 非装扮 */
        if (entity_or_null && s_files[i].entity[0] &&
            strcmp(s_files[i].entity, entity_or_null) != 0) continue;
        if (entity_or_null == NULL) {
            /* 默认：paperdoll 系实体优先 */
            if (strncmp(s_files[i].entity, "paperdoll", 9) == 0 ||
                strcmp(s_files[i].selector, "paperdoll") == 0) {
                const char *dir = render_dir_of("PARTS");
                snprintf(path, cap, "%s/%s.mpk", dir, s_files[i].hash);
                ok = true;
                break;
            }
            if (fallback < 0) fallback = i;
        } else {
            const char *dir = render_dir_of("PARTS");
            snprintf(path, cap, "%s/%s.mpk", dir, s_files[i].hash);
            ok = true;
            break;
        }
    }
    if (!ok && fallback >= 0) {
        const char *dir = render_dir_of("PARTS");
        snprintf(path, cap, "%s/%s.mpk", dir, s_files[fallback].hash);
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/* LAYOUT 条目的画布内 body 锚点（origin）：按动作名查询。
 * 供合成器摆放用（画布左上角 = 屏心 - origin×scale ⇒ origin 恒在屏心）。
 * 返回 false = 清单无该动作/无 origin 字段（回退 0,0 = 旧摆放行为）。 */
bool asset_dl_layout_origin(const char *action, int16_t *out_x, int16_t *out_y)
{
    if (!action || !action[0]) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcasecmp(s_files[i].kind, "LAYOUT") != 0) continue;
        if (strcmp(s_files[i].action, action) != 0) continue;
        if (out_x) *out_x = s_files[i].origin_x;
        if (out_y) *out_y = s_files[i].origin_y;
        ok = true;
        break;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool asset_dl_font_path(int size_px, char *path, size_t cap)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_file_cnt; i++) {
        if (strcasecmp(s_files[i].kind, "FONT") != 0) continue;
        if (s_files[i].font_px != (uint8_t)size_px) continue;
        const char *dir = render_dir_of("FONT");
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
        if (strcasecmp(s_files[i].kind, "PARTS") != 0) continue;
        if (strcmp(s_files[i].selector, "clock") != 0) continue;
        const char *dir = render_dir_of("PARTS");
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
        if (strcasecmp(s_files[i].kind, "BGMAP") != 0) continue;
        if (hash_or_null) {
            /* 【按地图 id 也能查到 2026-09-27】调用方有两种口径：
             *   · state_machine 的 MP_CMD_SET_MAP 下发的是**地图 id**（"000010000"）；
             *   · 菜单点选下发的是**内容 hash**。
             * 旧实现只比 hash → 按 id 的查询恒失败（真机：无 TF 卡启动时
             * "地图 000010000 路径查询失败" → 需求"没有 TF 卡就渲染默认地图"
             * 静默不落地，屏幕永远纯黑底）。两种键都认。 */
            if (strcmp(s_files[i].hash, hash_or_null) == 0 ||
                (s_files[i].map_id[0] && strcmp(s_files[i].map_id, hash_or_null) == 0)) {
                best = i;
                break;
            }
            continue;
        }
        /* NULL = 最近使用的 selector==map 地图（E7 最近+收藏） */
        if (s_files[i].selector[0] && strcmp(s_files[i].selector, "map") != 0) continue;
        if (best < 0 || s_files[i].last_used_ms > s_files[best].last_used_ms) best = i;
    }
    if (best >= 0) {
        const char *dir = render_dir_of("BGMAP");
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

    /* 【偏移修正 2026-09-27】BGMAP payload 头 56B：map_id[32] vw/vh[4]
     * static len/off[8] tile len/off[8] strip_count[4] → strip_count @ payload+52、
     * strip[14B] @ payload+56。**文件偏移 = 信封头 40B + payload 偏移**
     * （错算成 payload+52=84 时读到的是 tile_layer_len=489600 → 被 clamp 成
     * max_strips=8 条，再按错位置解析出 8 个垃圾 part_ref）→ render_set_map 收到
     * strip_count 不匹配直接返回 RENDER_ERR_ARG，默认地图永远装不上（真机：
     * "strip count mismatch: bgmap=2 given=8"）。 */
    uint8_t sc[4] = { 0 };
    fseek(f, 40 + 52, SEEK_SET);          /* strip_count @ payload+52 = 文件 92 */
    if (fread(sc, 1, 4, f) != 4) { fclose(f); return -1; }
    int count = (int)rd_le32(sc);
    if (count <= 0) { fclose(f); return 0; }
    if (count > max_strips) count = max_strips;

    uint8_t sh[14];
    int got = 0;
    fseek(f, 40 + 56, SEEK_SET);          /* strip[0] @ payload+56 = 文件 96 */
    for (int i = 0; i < count; i++) {
        if (fread(sh, 1, sizeof(sh), f) != sizeof(sh)) break;
        uint64_t part_ref = rd_le64(sh);
        if (part_ref == 0) continue;

        /* 以本地清单登记的 hash 字符串为准（大小写与 manifest 键一致），
         * 找不到则回落小写 16 位 hex */
        bool named = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int k = 0; k < s_file_cnt; k++) {
            if (strcasecmp(s_files[k].kind, "PARTS") == 0 &&
                strtoull(s_files[k].hash, NULL, 16) == part_ref) {
                const char *d = render_dir_of("PARTS");
                if (d) {
                    snprintf(strip_paths[got], MP_MPK_PATH_MAX,
                             "%s/%.64s.mpk", d, s_files[k].hash);  /* hash≤16 hex，%.64s 消截断告警 */
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
    if (got != count)
        ESP_LOGW(TAG, "BGMAP 条带解析：声明 %d 条，解析出 %d 条", count, got);
    return got;
}

void asset_dl_set_active_map(const char *hash)
{
    if (!hash || !hash[0]) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* 【双键 2026-10-01】调用方两种口径：菜单点选=内容 hash、状态机/服务端指令=
     * 地图 id（见 asset_dl_map_path 同款说明）。旧实现只认 hash → 按 id 下发时
     * s_active_map 不更新（clock 锚点停在上一张图）。现统一走 find_bgmap_locked，
     * 顺带记录当前图的**内容 hash**（"隐藏当前图"判定要用，见 map_is_active）。 */
    int idx = find_bgmap_locked(hash);
    if (idx >= 0) {
        if (s_files[idx].map_id[0]) {
            strlcpy(s_active_map, s_files[idx].map_id, sizeof(s_active_map));
        }
        strlcpy(s_active_map_hash, s_files[idx].hash, sizeof(s_active_map_hash));
        s_files[idx].last_used_ms = mp_now_ms();
    } else {
        ESP_LOGW(TAG, "set_active_map：%s 无对应 BGMAP 条目（清单未就绪？）", hash);
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
