/**
 * bgm.c — BGM 双源流式播放（E8）
 *
 * 数据通路（4.1）：
 *   [bgm 任务 PRO 核]
 *     GET /api/device/bgm/stream?id=N&source=wz → 16KB MP3 输入缓冲
 *     → mp3dec_decode_frame → 单声道 short[] → 双声道交织
 *     → pcm_ring（PSRAM 128KB） ——满则背压（HTTP 读流自然暂停）
 *   [feeder 任务 PRO 核]
 *     ring_read(1152 帧, 100ms 超时) → 线性音量缩放
 *     → codec_write（I2S DMA，48k/16bit 档随 MP3 帧采样率重配）
 *     → PA_CTRL 有声才开（pa_ctrl_enable）
 *
 * E8 短路状态机（同源内）：
 *   曲内失败重试 2 → 跳下一首（仍是同源）→ 连续 3 曲失败
 *   → 源置灰（入口 greyed）+ failover 事件 + despair 表情 + 停播。
 *   手动 MP_AUDIO_SOURCE 才换源（清灰显）。
 */
#include "bgm.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>         /* audio/ 目录扫描（曲目表构建） */
#include <strings.h>        /* strcasecmp（.mpk 后缀） */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"   /* xTaskCreatePinnedToCoreWithCaps（PSRAM 栈） */
#include "cJSON.h"

#include "app_core.h"
#include "hal_contract.h"
#include "http_client.h"
#include "input_dispatch.h"
#include "pcm_ring.h"
#include "minimp3/minimp3.h"
#include "mpak.h"           /* AUDIO_META 包解析（复用格式实现，不另写） */

static const char *TAG = "bgm";

/* ------------------------------------------------------------------ */
/* 参数                                                                 */
/* ------------------------------------------------------------------ */
#define MP3_INBUF_LEN      (16 * 1024)      /* MP3 码流缓冲（bit reservoir 够用） */
#define RING_BYTES         (128 * 1024)     /* E8/任务书：PSRAM 128KB */
#define FEED_FRAMES        1152             /* 单次喂 I2S 的帧数 */
#define RETRY_SAME_TRACK   2                /* 同曲重试 2 次 */
#define TRACK_FAIL_LIMIT   3                /* 连续 3 曲失败 → 源置灰 */
#define STALL_NOFRAME_MAX  64               /* 连续空帧判失联（喂错流/占位解码器） */
#define UNDERRUN_PA_OFF    20               /* 静音 2s → 关 PA */

/* ------------------------------------------------------------------ */
/* 状态（feeder/外部读）                                                 */
/* ------------------------------------------------------------------ */
static volatile uint32_t s_rate;            /* 当前流采样率（0=未定） */
static volatile bool     s_rate_dirty;      /* 采样率变更待 feeder 重配 */
static volatile uint8_t  s_vol = 50;        /* 线性音量 0..100（立即生效） */
static volatile bool     s_playing;         /* feeder 出声许可 */
static volatile mp_bgm_state_t s_state = MP_BGM_IDLE;
static volatile mp_bgm_source_t s_source = MP_BGM_SRC_WZ;   /* NVS 偏好 */
static volatile bool     s_greyed[2] = { false, false };     /* E8 源置灰 */
static volatile bool     s_offline;

static volatile uint32_t s_cur_id;   /* 最近起播的流 id（暂停恢复重开流用） */
static pcm_ring_t *s_ring;

/* ------------------------------------------------------------------ */
/* 曲目表（AUDIO_META 包懒加载缓存）                                     */
/* ------------------------------------------------------------------ */
/* asset_dl.h 只有 hash→路径 查询面、无 audio 列表接口 → 此处直接扫
 * /sdcard/minipet/audio/<hash>.mpk，逐包 mpak_open(expect_kind=AUDIO_META)
 * 解析（复用 render/mpak 格式实现；损坏包 MPAK_ERR_* 静默跳过，E11 归
 * asset_dl 管）。表按当前源过滤（E8 同源口径，流 URL 的 source 也同源），
 * 源切换后按需重建；bgm 任务与表读者共一把锁（表构建持锁做 TF IO，
 * 读者短临界区——bgm_list 首调可能令 bgm 任务阻塞百 ms 级，可接受）。 */

#define TBL_TITLE_LEN      32      /* 对外 title 定宽（bgm_list 契约：32B 含 NUL） */
#define TBL_PATH_LEN       64      /* /sdcard/minipet/audio/<16hex>.mpk */
#define TBL_RESCAN_MS      30000   /* 空表扫描结果缓存 30s（play session 曲末查表，防逐曲扫 TF） */

static SemaphoreHandle_t s_tbl_lock;
static uint32_t *s_tids;                       /* PSRAM [s_tcount]（已按源过滤+去重） */
static char (*s_titles)[TBL_TITLE_LEN];        /* PSRAM [s_tcount] */
static int   s_tcount;                         /* 0=无表（未加载/空/失败） */
static int   s_tcur = -1;                      /* 当前曲表内 index；-1=未锚定 */
static uint8_t s_tbl_src = 0xFF;               /* 表对应源（0xFF=未建） */
static int64_t s_tbl_failed_ms;                /* 上次空表扫描时刻（重扫节流） */

static const char *source_str(mp_bgm_source_t s);   /* 定义见 bgm/cmd 回传节 */

/* 定宽 title 拷贝：截 31 字节并回退 UTF-8 续字节，避免截半个汉字 */
static void tbl_title_copy(char dst[TBL_TITLE_LEN], const char *src)
{
    int n = 0;
    while (n < TBL_TITLE_LEN - 1 && src[n]) n++;
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;  /* 0b10xxxxxx=截断残体 */
    memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
}

/* 构建期去重（多包/重复 id 只收一次；n≤~千级，线性扫一次性成本可忽略） */
static bool tbl_build_has(const uint32_t *ids, int n, uint32_t id)
{
    for (int i = 0; i < n; i++) {
        if (ids[i] == id) return true;
    }
    return false;
}

/* 扫 audio/ 全部 .mpk 构建（须持 s_tbl_lock）。结果发布前旧表保持可读：
 * 中途 OOM 弃新保旧/置空，调用方按 s_tcount 语义自然降级服务端兜底。 */
static void tbl_load_locked(void)
{
    mp_bgm_source_t src = (mp_bgm_source_t)s_source;
    uint32_t *ids = NULL;
    char (*titles)[TBL_TITLE_LEN] = NULL;
    int cnt = 0, cap = 0;
    bool oom = false;

    DIR *d = opendir(MP_TF_MINIPET_DIR "/audio");
    if (d) {
        struct dirent *e;
        while (!oom && (e = readdir(d)) != NULL) {
            size_t nlen = strlen(e->d_name);
            if (nlen < 5 || strcasecmp(e->d_name + nlen - 4, ".mpk") != 0) continue;

            char path[TBL_PATH_LEN];
            snprintf(path, sizeof(path), MP_TF_MINIPET_DIR "/audio/%s", e->d_name);

            mpak_t m;
            if (mpak_open(&m, path, 0 /*hash 免比（asset_dl 落盘已校验）*/,
                          MPAK_KIND_AUDIO_META) != MPAK_OK) {
                ESP_LOGW(TAG, "audio pack %s unusable, skipped", e->d_name);
                continue;                        /* 空/截断/坏包：跳过 */
            }
            const mpak_audio_t *au = m.u.audio;
            for (uint32_t i = 0; au && i < au->track_count; i++) {
                if ((mp_bgm_source_t)au->tracks[i].source != src) continue;
                if (tbl_build_has(ids, cnt, au->tracks[i].id)) continue;
                if (cnt == cap) {                /* 倍增扩容（PSRAM） */
                    int ncap = cap ? cap * 2 : 256;
                    uint32_t *nid = heap_caps_realloc(
                        ids, (size_t)ncap * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
                    if (!nid) { oom = true; break; }
                    ids = nid;
                    char (*ntl)[TBL_TITLE_LEN] = heap_caps_realloc(
                        titles, (size_t)ncap * TBL_TITLE_LEN, MALLOC_CAP_SPIRAM);
                    if (!ntl) { oom = true; break; }
                    titles = ntl;
                    cap = ncap;
                }
                ids[cnt] = au->tracks[i].id;
                tbl_title_copy(titles[cnt], au->tracks[i].title);   /* 96B→31B 安全截断 */
                cnt++;
            }
            mpak_close(&m);
        }
        closedir(d);
    }

    /* 【可观测性 2026-09-27】此前构建结果完全静默：包不存在/坏包/source 过滤不匹配
     * （AUDIO_META 里 tracks[].source 与当前音源不符）都会得到空表，用户侧只看到
     * "设了 BGM 没声音"，无从判断卡在哪一步。这里把关键计数打出来。 */
    ESP_LOGW(TAG, "曲目表构建：src=%d 命中 %d 首（audio 目录%s）",
             (int)src, cnt, d ? "存在" : "不存在");
    if (oom) {
        ESP_LOGE(TAG, "track table build OOM @%d", cnt);
        heap_caps_free(ids);
        heap_caps_free(titles);
        ids = NULL; titles = NULL; cnt = 0;      /* 弃半成品，旧表/空表语义不变 */
    }

    heap_caps_free(s_tids);
    heap_caps_free(s_titles);
    s_tids = ids;
    s_titles = titles;
    s_tcount = cnt;
    s_tcur = -1;                                 /* 表换血，播放锚点作废 */
    s_tbl_src = (uint8_t)src;
    s_tbl_failed_ms = cnt ? 0 : mp_now_ms();     /* 空表 30s 内不重扫 */
    ESP_LOGI(TAG, "track table: %d tracks (source=%s)", cnt, source_str(src));
}

/* 表与当前源一致且可用 → 直接用；否则（重）构建 */
static void tbl_ensure_locked(void)
{
    if (s_tbl_src == (uint8_t)s_source && s_tcount > 0) return;
    if (s_tbl_src == (uint8_t)s_source && mp_now_ms() - s_tbl_failed_ms < TBL_RESCAN_MS) return;
    tbl_load_locked();
}

/* idx 处曲目 id（越界/空表=0）。短临界区，不触发构建。
 * 【E8 无声根因修复】按 u32 原值返回：AUDIO_META 的 id 是 uint32，>2^31 的曲目
 * 占 49.7%（本机实测 1167 曲中 580 首；且设备曲目表**首条** SleepyWood=3036740071）。
 * 此前返回 int → 高位 id 变负数 → bgm_play_session 的 by_table 判 false 且 fb_id=0
 * → 会话开头直接 return：不发 HTTP、不报错、状态仍停在 PLAYING（界面像在播），
 * 且 s_tcur 锚点不前移 → 每次「下一首」都重挑同一首，表现为永久无声。 */
static uint32_t tbl_id_at(int idx)
{
    xSemaphoreTake(s_tbl_lock, portMAX_DELAY);
    uint32_t id = (idx >= 0 && idx < s_tcount) ? s_tids[idx] : 0;
    xSemaphoreGive(s_tbl_lock);
    return id;
}

static int tbl_find_locked(uint32_t id)
{
    for (int i = 0; i < s_tcount; i++) {
        if (s_tids[i] == id) return i;
    }
    return -1;
}

/* 表内步进（dir=+1/-1），尾↔首循环；空表=-1；锚点无效时 dir>0 从 0 起 */
static int tbl_step_locked(int dir)
{
    if (s_tcount <= 0) return -1;
    if (s_tcur < 0 || s_tcur >= s_tcount) return (dir > 0) ? 0 : s_tcount - 1;
    int i = s_tcur + dir;
    if (i < 0) i = s_tcount - 1;
    if (i >= s_tcount) i = 0;
    return i;
}

/* ------------------------------------------------------------------ */
/* bgm/cmd 回传（E8：设备端现场控制）                                    */
/* ------------------------------------------------------------------ */
static const char *source_str(mp_bgm_source_t s) { return s ? "qq" : "wz"; }

/* 菜单标签用：当前源显示名（大写便于 5x7/拉丁字体判读） */
const char *bgm_source_name(void)
{
    return (s_source == MP_BGM_SRC_QQ) ? "QQ" : "WZ";
}
/* 发控制命令并取回应答 id（next/prev/play 用）；纯回传可忽略应答 */
static int bgm_cmd(const char *cmd, int n, int *out_id)
{
    if (out_id) *out_id = 0;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "proto", MP_PROTO_VER);
    cJSON_AddStringToObject(root, "deviceId", mp_http_device_id());
    cJSON_AddStringToObject(root, "cmd", cmd);
    if (n) cJSON_AddNumberToObject(root, "n", n);
    cJSON_AddStringToObject(root, "source", source_str((mp_bgm_source_t)s_source));
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return -1;

    char resp[512];
    int status = mp_http_post_json("/api/device/bgm/cmd", body, resp, sizeof(resp), 8000);
    free(body);
    if (status != 200) return -1;

    if (out_id) {
        cJSON *r = cJSON_Parse(resp);
        if (r) {
            cJSON *jid = cJSON_GetObjectItem(r, "id");
            if (cJSON_IsNumber(jid)) *out_id = (int)cJSON_GetNumberValue(jid);
            cJSON_Delete(r);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 流会话（bgm 任务上下文执行）                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  *in;
    size_t    in_len;
    mp3dec_t *dec;
    int16_t   pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int16_t   stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    int       frames;          /* 本会话解出的帧数 */
    int       stall;           /* 连续无帧计数 */
} stream_ctx_t;

static stream_ctx_t s_sc;

static mp3dec_t *s_dec;   /* 解码器常驻（bgm 任务启始分配；play_track 每曲 mp3dec_init 复位） */

/* 前向：流中控制队列排空（定义见「控制消息」节） */
static void drain_audio_q_nonblock(void);

/* 解码 in 缓冲内所有完整帧 → 环形缓冲；false=外部要求停 */
static bool decode_pending(stream_ctx_t *c)
{
    while (c->in_len > 0) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(c->dec, c->in, (int)c->in_len,
                                          c->pcm, &info);
        if (info.frame_bytes <= 0) break;              /* 数据不足 */

        /* 采样率变化：feeder 冲缓冲后重配 I2S（44.1k/48k 都走 16bit 档） */
        if (info.hz > 0 && (uint32_t)info.hz != s_rate) {
            s_rate = (uint32_t)info.hz;
            s_rate_dirty = true;
        }

        if (samples > 0 && info.channels >= 1) {
            c->frames++;
            c->stall = 0;
            int ch = (info.channels == 1) ? 1 : 2;
            for (int i = 0; i < samples; i++) {
                if (ch == 1) {
                    c->stereo[i * 2]     = c->pcm[i];  /* 单声道 → 双声道复制 */
                    c->stereo[i * 2 + 1] = c->pcm[i];
                } else {
                    c->stereo[i * 2]     = c->pcm[i * 2];
                    c->stereo[i * 2 + 1] = c->pcm[i * 2 + 1];
                }
            }
            size_t total = (size_t)samples * 2;
            size_t done = 0;
            while (done < total) {
                if (!s_playing || s_offline) return false;   /* 暂停/断网：中止 */
                size_t w = pcm_ring_write(s_ring, c->stereo + done, total - done);
                done += w;
                if (w == 0) vTaskDelay(pdMS_TO_TICKS(20));   /* 背压等 feeder */
            }
        } else {
            /* 无 PCM 帧（头/标签帧）：正常，但连续过多判失联 */
            if (++c->stall > STALL_NOFRAME_MAX) return false;
        }

        /* 消费掉已解码输入 */
        size_t used = (size_t)info.frame_bytes;
        if (used >= c->in_len) {
            c->in_len = 0;
        } else {
            memmove(c->in, c->in + used, c->in_len - used);
            c->in_len -= used;
        }
    }
    return true;
}

static bool stream_chunk(void *ctx, const char *data, size_t len)
{
    stream_ctx_t *c = ctx;
    drain_audio_q_nonblock();                    /* 流中控制：暂停/切歌即时生效 */
    if (!s_playing || s_offline) return false;   /* 控制中止流 */

    /* 追加进码流缓冲（满则先解码腾空） */
    if (c->in_len + len > MP3_INBUF_LEN) {
        if (!decode_pending(c)) return false;
    }
    size_t take = len;
    if (c->in_len + take > MP3_INBUF_LEN) {
        /* 缓冲仍满且解不出帧（16KB 无同步头：坏流/超长垃圾前缀）。
         * 上游 find_frame 已证明整缓冲无可解码帧 → 整段丢弃重同步，
         * 保证流的前向推进（否则后续数据会被永久丢弃）。 */
        c->in_len = 0;
        take = len;
    }
    memcpy(c->in + c->in_len, data, take);
    c->in_len += take;

    return decode_pending(c);
}

/* 播放一首：返回 true=自然播完（可续下一首），false=失败/中止 */
static bool play_track(uint32_t track_id)
{
    char url[160];
    /* bgm/stream 端点服务端必填 deviceId（DeviceEndpoints.cs HandleBgmStream
     * 签名 string deviceId）——不带参实测 400，带参后实测 200 + audio/mpeg。
     * 【E8 无声根因修复】id 必须按 u32（%u）发出：服务端 WzMusicSource 只认
     * 「纯 ASCII 数字」做 u32→trackKey 反查，用 %d 发高位 id（3036740071 →
     * -1258227225）会带上负号 → 服务端当字符串 key → 503「曲目不存在」（实测）。 */
    snprintf(url, sizeof(url), "/api/device/bgm/stream?deviceId=%s&id=%u&source=%s",
             mp_http_device_id(), (unsigned)track_id, source_str((mp_bgm_source_t)s_source));

    memset(&s_sc, 0, sizeof(s_sc));
    mp3dec_init(s_dec);                         /* 每曲复位解码器（清 bit reservoir/合成
                                                 * 残态；上游 mp3dec_init 仅清头缓存，开销极小。
                                                 * 流内逐帧调用间则必须保持状态，勿在此之外重置） */
    s_sc.dec = s_dec;
    s_sc.in = heap_caps_malloc(MP3_INBUF_LEN, MALLOC_CAP_SPIRAM);
    if (!s_sc.in) return false;

    s_playing = true;
    int status = mp_http_get(url, 15000, stream_chunk, &s_sc);

    bool natural = (status == 200);              /* 200+读尽 = 服务端结束本曲 */
    free(s_sc.in);
    s_sc.in = NULL;

    if (!natural && !s_playing) {
        return true;                             /* 控制中止不算失败 */
    }
    if (natural && s_sc.frames == 0) {
        return false;                            /* 200 但一帧没解：占位/坏流 */
    }
    return natural;
}

/* ------------------------------------------------------------------ */
/* 播放循环（含 E8 短路状态机）                                          */
/* ------------------------------------------------------------------ */
static void source_failover(void)
{
    s_greyed[s_source] = true;                   /* 该源入口置灰（E8） */
    s_state = MP_BGM_FAILED;
    s_playing = false;

    /* failover 事件上报 + despair 表情（E8/E10） */
    mp_post_event_simple(MP_EVT_BGM_FAILOVER, (int32_t)s_source, 0,
                         source_str((mp_bgm_source_t)s_source));
    input_trigger_expression(MP_EXPR_DESPAIR, 2500);
    ESP_LOGW(TAG, "source %s failed over (greyed)",
             source_str((mp_bgm_source_t)s_source));
}

/* 播放会话（bgm 任务上下文）：
 *   start_idx >= 0        → 曲目表驱动：自然播完/曲废均走 tbl_step 循环推进
 *                           （E8 同源内循环；表构建时已按源过滤）
 *   start_idx <0 且 fb_id>0 → 服务端驱动兜底（audio/ 无表）：bgm_cmd("next") 取下一首
 *   两皆无效               → 直接返回（无可播）
 * s_tcur 只在真正起播前提交：暂停/流中止退出时不前移，恢复续播仍在当前曲。 */
static void bgm_play_session(int start_idx, int fb_id)
{
    bool by_table = (tbl_id_at(start_idx) != 0);         /* u32 判 0（见 tbl_id_at 注释） */
    uint32_t track = by_table ? 0 : (uint32_t)fb_id;     /* 服务端模式：int32 位型即 u32 id */
    int idx = by_table ? start_idx : -1;
    if (!by_table && fb_id == 0) return;

    int track_fails = 0;

    while (track_fails < TRACK_FAIL_LIMIT) {
        drain_audio_q_nonblock();                       /* 曲间也收控制 */
        if (!s_playing || s_offline) return;            /* 中止（锚点不前移） */
        if (s_greyed[s_source]) return;

        uint32_t id;
        if (by_table) {
            id = tbl_id_at(idx);
            if (id == 0) {
                /* 表中途失效（重建为空/缩水）：走循环尾统一收尾
                 * （s_state=PLAYING 时回 IDLE），不可裸 return 漏状态 */
                break;
            }
            s_tcur = idx;                               /* 真正起播前锚定 */
        } else {
            id = track;
        }
        s_cur_id = id;                                  /* 恢复续播/上报用 */

        int retries = 0;
        bool ok = false;
        while (retries <= RETRY_SAME_TRACK) {
            if (!s_playing || s_offline) return;
            if (play_track(id)) { ok = true; break; }
            retries++;
        }
        if (!ok) {
            track_fails++;
            ESP_LOGW(TAG, "track %u failed (streak %d), skip", (unsigned)id, track_fails);
            /* 同曲重试耗尽 → 跳下一首（仍同源——E8 禁跨源自动换歌） */
            if (by_table) {
                xSemaphoreTake(s_tbl_lock, portMAX_DELAY);
                idx = tbl_step_locked(+1);
                xSemaphoreGive(s_tbl_lock);
                if (idx < 0) break;                     /* 空表（异常）：会话结束 */
            } else {
                int next = 0;
                if (bgm_cmd("next", 0, &next) != 0 || next == 0) {
                    track_fails++;                      /* 取下一首也失败：加速三振 */
                    break;                              /* id 判 0：int32 位型可为负 */
                }
                track = (uint32_t)next;
            }
            continue;
        }

        track_fails = 0;                         /* 有成功播放即复位 */

        if (!s_playing) return;                  /* 暂停/切歌中止：不前移锚点 */

        /* 自然播完 → 下一首（同源内循环） */
        if (by_table) {
            xSemaphoreTake(s_tbl_lock, portMAX_DELAY);
            idx = tbl_step_locked(+1);
            xSemaphoreGive(s_tbl_lock);
            if (idx < 0) break;
        } else {
            int next = 0;
            if (bgm_cmd("next", 0, &next) != 0 || next == 0) {
                track_fails = 1;
                break;                               /* 拿不到下一首：会话结束 */
            }
            track = (uint32_t)next;
        }
    }

    if (track_fails >= TRACK_FAIL_LIMIT) {
        source_failover();
    }
    if (s_state == MP_BGM_PLAYING) {
        s_state = MP_BGM_IDLE;
        s_playing = false;
    }
}

/* ------------------------------------------------------------------ */
/* 控制消息                                                              */
/* ------------------------------------------------------------------ */

/* 流播放期间（bgm 任务阻塞在 HTTP 读流）非阻塞排空控制队列：
 * 暂停/停止/切歌立即中止流；音量即时生效；切歌/选曲动作记账延后执行 */
static volatile int s_pending_op;      /* 0=无 1=next 2=prev */
static volatile bool s_pending_play;   /* 流中收到选曲播放待执行 */
static volatile int32_t s_pending_play_id;

/* 音量统一落点（仅 bgm 任务上下文调用；s_vol 为 volatile u8 单写者）：
 * clamp 0..100 → 立即生效（feeder 出口读 s_vol）→ NVS 偏好 */
static void vol_apply(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if ((uint8_t)v == s_vol) return;
    s_vol = (uint8_t)v;
    mp_nvs_set_u32("bgm_vol", s_vol);
}

static void drain_audio_q_nonblock(void)
{
    mp_audio_msg_t m;
    while (xQueueReceive(mp_audio_q, &m, 0) == pdTRUE) {
        switch (m.type) {
        case MP_AUDIO_PAUSE:
            s_pending_play = false;
            s_playing = false;
            if (s_state == MP_BGM_PLAYING) s_state = MP_BGM_PAUSED;
            break;
        case MP_AUDIO_STOP:
            s_pending_play = false;
            s_playing = false;
            s_state = MP_BGM_IDLE;
            pcm_ring_flush(s_ring);
            break;
        case MP_AUDIO_NEXT:
            s_pending_play = false;                  /* 后到控制覆盖先到选曲（到包序语义） */
            s_pending_op = 1;
            s_playing = false;
            break;
        case MP_AUDIO_PREV:
            s_pending_play = false;
            s_pending_op = 2;
            s_playing = false;
            break;
        case MP_AUDIO_VOL:
            vol_apply(m.a);
            break;
        case MP_AUDIO_VOLUME:
            vol_apply((int)s_vol + m.a);   /* 流中增减：feeder 出口即时生效，不做 HTTP 回传 */
            break;
        case MP_AUDIO_PLAY:
            if (s_greyed[s_source]) break;           /* 置灰源禁播（E8） */
            s_pending_play = true;                   /* 正播中到达：记账延后执行
                                                        （丢弃会导致播中选曲永远无效） */
            s_pending_play_id = m.a;
            s_playing = false;                       /* 中止当前流/会话 */
            break;
        case MP_AUDIO_SOURCE:
            s_pending_play = false;
            if (m.a == 0 || m.a == 1) {
                s_source = (mp_bgm_source_t)m.a;
                s_greyed[s_source] = false;
                mp_nvs_set_u32("bgm_src", (uint32_t)s_source);
                s_playing = false;
                s_state = MP_BGM_IDLE;
                s_tcur = -1;                    /* 换源：播放锚点作废（表按新源重建） */
            }
            break;
        default:
            break;                  /* RESUME 播放期间到达无意义；其余已列全 */
        }
    }
}

/* 【取证计数器 2026-09-27】bgm 任务收到消息数 / 播放状态 / feeder 循环数：
 * 与 poller 计数一起由 main.c 的 tprobe 打印，用于判定"起播后设备掉线"是
 * 哪个任务被阻塞。 */
volatile uint32_t g_bgm_msgs, g_feeder_loops;
volatile int32_t  g_bgm_state_probe;
/* PLAY 分支逐点标记：1 入口 / 2 bgm_cmd 后 / 3 codec_start 后 / 4 表情后 /
 * 5 取到表锁 / 6 tbl_ensure 后 / 7 play_session 后 —— 卡在哪一步看它 */
volatile uint32_t g_bgm_step;

static void handle_audio_msg(const mp_audio_msg_t *m)
{
    switch (m->type) {
    case MP_AUDIO_PLAY: {
        g_bgm_step = 1;
        if (s_greyed[s_source]) return;                 /* 置灰源禁播（E8） */
        int id = m->a;                              /* 0=服务端决定；负数=高位 u32 id 的位型 */
        if (id == 0) {
            if (bgm_cmd("play", 0, &id) != 0 || id == 0) {
                /* 【不再静默 2026-09-27】此前这里直接 return：服务端没给曲目 id
                 * 时（旧实现 play 不选曲 → 回 id:null）设备不发流、不报错、
                 * 状态还停在 PLAYING，用户侧就是"点了播放没声音"。
                 * 现在至少留一条日志，且服务端已修为 play 必回曲目。 */
                ESP_LOGW(TAG, "play：未取得曲目 id（服务端返回空/请求失败）→ 本次不起播");
                return;
            }
        }
        g_bgm_step = 2;                             /* bgm_cmd 已拿到曲目 id */
        s_pending_op = 0;
        s_pending_play = false;                     /* 本次 PLAY 直接落地，清延后记账 */
        s_state = MP_BGM_PLAYING;
        s_playing = true;
        mp_codec_start(s_rate ? s_rate : 44100);
        g_bgm_step = 3;                             /* I2S/codec 已就绪 */
        input_trigger_expression(MP_EXPR_HUM, 1500);     /* E10：BGM 播放=hum */
        g_bgm_step = 4;
        /* 表内有此 id → 按表位起播（next/prev 循环锚点）；
         * 表不可用/不在表内 → 服务端 id 兜底路径 */
        xSemaphoreTake(s_tbl_lock, portMAX_DELAY);
        g_bgm_step = 5;
        tbl_ensure_locked();
        g_bgm_step = 6;
        int idx = tbl_find_locked((uint32_t)id);
        xSemaphoreGive(s_tbl_lock);
        bgm_play_session(idx, idx < 0 ? id : 0);
        g_bgm_step = 7;                             /* 会话已返回 */
        break;
    }
    case MP_AUDIO_RESUME:
        if (s_state == MP_BGM_PAUSED) {
            /* 暂停采用「关流」方案（见 MP_AUDIO_PAUSE），此处重开当前曲流：
             * 从头续播（~前 1s 环形缓冲已在暂停侧丢弃），服务端进度由
             * bgm_cmd("resume") 回传记账；s_tcur 锚点未动 → 续的还是当前曲。 */
            xSemaphoreTake(s_tbl_lock, portMAX_DELAY);
            tbl_ensure_locked();
            int idx = (s_tcur >= 0) ? s_tcur : tbl_find_locked(s_cur_id);
            xSemaphoreGive(s_tbl_lock);
            if (idx < 0 && s_cur_id == 0) return;        /* 从未起播：无可续 */
            s_state = MP_BGM_PLAYING;
            s_playing = true;
            pcm_ring_flush(s_ring);                      /* 丢暂停残留，防旧数据先出声 */
            mp_codec_start(s_rate ? s_rate : 44100);
            bgm_cmd("play", 0, NULL);                  /* 现场控制回传 */
            bgm_play_session(idx, (int)s_cur_id);
        }
        break;
    case MP_AUDIO_PAUSE:
        if (s_state == MP_BGM_PLAYING) {
            /* 暂停=停 feeder+PA 静音（feeder 见 !s_playing 即关 PA），并选择
             * 关闭流：mp_http_get 为阻塞读，无「挂起保连」机制，stream_chunk
             * 见 !s_playing 返回 false 即中止 HTTP → 连接释放。代价是恢复时
             * 重开当前曲流（见 MP_AUDIO_RESUME）。 */
            s_state = MP_BGM_PAUSED;
            s_playing = false;                          /* 流/出声双停 */
            bgm_cmd("pause", 0, NULL);
        }
        break;
    case MP_AUDIO_STOP:
        s_state = MP_BGM_IDLE;
        s_playing = false;
        pcm_ring_flush(s_ring);
        bgm_cmd("pause", 0, NULL);
        break;
    case MP_AUDIO_NEXT:
    case MP_AUDIO_PREV: {
        if (s_state == MP_BGM_FAILED) return;           /* 置灰源禁播 */
        int dir = (m->type == MP_AUDIO_NEXT) ? +1 : -1;
        xSemaphoreTake(s_tbl_lock, portMAX_DELAY);
        tbl_ensure_locked();
        int idx = tbl_step_locked(dir);                 /* 表内尾↔首循环 */
        xSemaphoreGive(s_tbl_lock);
        if (idx >= 0) {
            s_pending_op = 0;
            s_state = MP_BGM_PLAYING;
            s_playing = true;
            mp_codec_start(s_rate ? s_rate : 44100);
            bgm_play_session(idx, 0);
        } else {
            /* 兜底：表不可用（audio/ 无包）→ 服务端 next/prev */
            int sid = 0;
            if (bgm_cmd(m->type == MP_AUDIO_NEXT ? "next" : "prev", 0, &sid) == 0 && sid != 0) {
                s_state = MP_BGM_PLAYING;
                s_playing = true;
                mp_codec_start(s_rate ? s_rate : 44100);
                bgm_play_session(-1, sid);
            }
        }
        break;
    }
    case MP_AUDIO_VOL:
        vol_apply(m->a);
        bgm_cmd("volume", (int)s_vol, NULL);
        break;
    case MP_AUDIO_VOLUME:
        vol_apply((int)s_vol + m->a);
        bgm_cmd("volume", (int)s_vol, NULL);              /* 现场控制回传 */
        break;
    case MP_AUDIO_SOURCE:
        /* E8：手动切类型才换源（并清该源灰显） */
        if (m->a == 0 || m->a == 1) {
            s_source = (mp_bgm_source_t)m->a;
            s_greyed[s_source] = false;
            mp_nvs_set_u32("bgm_src", (uint32_t)s_source);
            /* 换源停播：等用户显式 play */
            s_state = MP_BGM_IDLE;
            s_playing = false;
            s_tcur = -1;                                /* 曲目表按新源重建（tbl_ensure） */
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* bgm 任务 / feeder 任务                                                */
/* ------------------------------------------------------------------ */
/* ══ 【BGM 任务栈 2026-09-27 真机根因：起播即崩】══════════════════════════
 * 证据链：
 *   · 6144 栈 → `***ERROR*** A stack overflow in task bgm has been detected` → 重启；
 *   · 抬到 10240 后不再是 canary 报错，而是 minimp3 内部
 *     `Guru Meditation (LoadProhibited, EXCVADDR=0)`：
 *     addr2line 反解 → bgm_task → handle_audio_msg → bgm_play_session → play_track
 *     → stream_chunk → decode_pending → mp3dec_decode_frame → L3_huffman。
 *   · 根因就是本仓库 minimp3.h 文件头写明的栈核算：
 *     `mp3dec_decode_frame 的 mp3dec_scratch_t ≈16KB 在调用栈上 —— bgm 任务栈须 ≥20KB`。
 * 而内部 DRAM 运行期最大连续块只有 ~7.6~24KB，24KB **内部**栈建不起来（旧注释已实证
 * "12KB 栈永远建不起来"）。因此改为**PSRAM 栈**（sdkconfig 已开
 * CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y，本板 8MB PSRAM 几乎全空），
 * 既满足 24KB 需求，又不吃内部堆。创建失败时仍走原有 10s 重试（内部栈兜底）。 */
/* 【栈大小最终定案 2026-09-27】minimp3 的 16KB 解码 scratch 已改到 PSRAM
 * （见 minimp3.h 的 mp3d_scratch_psram 补丁），任务栈只需覆盖
 * esp_http_client 读链（~2KB）+ 解码调用帧（~2KB）+ 曲目表构建 ≈ 8KB。
 * 试过的三条路与实测结果：
 *   · 24KB 内部栈 → 事件任务建不起来 + lwIP `thread_sem_init: out of memory`
 *     → socket 分配失败、联网直接失败（内部 DRAM 只有十几 KB）；
 *   · PSRAM 栈（xTaskCreatePinnedToCoreWithCaps）→ 本任务读 Flash，
 *     PSRAM 栈在关 cache 临界区触发 assert → 1.5s 重启循环；
 *   · 把 scratch 搬到 PSRAM + 8KB 内部栈 → 内部堆回到健康水位、解码正常。 */
#define MP_BGM_TASK_STACK 8192

static void bgm_task(void *arg)
{
    (void)arg;
    /* 解码器状态 ≈6.7KB（float 合成器），内部 RAM 优先；紧张时退 PSRAM
     * （float 访存变慢但仍可解，好过无声） */
    s_dec = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_8BIT);
    if (!s_dec) {
        s_dec = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    }
    if (s_dec) {
        mp3dec_init(s_dec);
    }

    uint32_t vol = 50;
    if (mp_nvs_get_u32("bgm_vol", &vol)) s_vol = (uint8_t)vol;
    uint32_t src = 0;
    if (mp_nvs_get_u32("bgm_src", &src)) s_source = (mp_bgm_source_t)src;

    for (;;) {
        mp_audio_msg_t m;
        if (xQueueReceive(mp_audio_q, &m, portMAX_DELAY) == pdTRUE) {
            g_bgm_msgs++;
            g_bgm_state_probe = (int32_t)s_state;
            if (!s_dec) continue;                       /* 解码器不可用 */
            if (m.type == MP_AUDIO_VOL || m.type == MP_AUDIO_VOLUME) {
                handle_audio_msg(&m); continue;     /* 音量本地可用，断网也不拦 */
            }
            if (s_offline && m.type != MP_AUDIO_SOURCE) {
                continue;                               /* 断网静音降级（E8）；切源仍可 */
            }
            handle_audio_msg(&m);

            /* 流中收到的 next/prev/选曲记账，此刻补执行 */
            if (s_pending_op && !s_offline && s_dec) {
                mp_audio_msg_t op = {
                    .type = (s_pending_op == 1) ? MP_AUDIO_NEXT : MP_AUDIO_PREV, .a = 0 };
                s_pending_op = 0;
                if (!s_greyed[s_source]) {
                    handle_audio_msg(&op);
                }
            } else if (s_pending_play && !s_offline && s_dec) {
                mp_audio_msg_t op = { .type = MP_AUDIO_PLAY, .a = s_pending_play_id };
                s_pending_play = false;
                if (!s_greyed[s_source]) {
                    handle_audio_msg(&op);
                }
            }
        }
    }
}

static void feeder_task(void *arg)
{
    (void)arg;
    static int16_t out[FEED_FRAMES * 2];
    bool pa_on = false;
    int underruns = 0;

    for (;;) {
        g_feeder_loops++;
        /* 采样率变更：先清缓冲再重配（避免新旧采样率混流） */
        if (s_rate_dirty) {
            pcm_ring_flush(s_ring);
            mp_codec_set_rate(s_rate);
            s_rate_dirty = false;
        }

        if (!s_playing) {
            if (pa_on) { mp_pa_enable(false); pa_on = false; }
            underruns = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t n = pcm_ring_read(s_ring, out, FEED_FRAMES * 2, 100);
        if (n == 0) {
            if (++underruns > UNDERRUN_PA_OFF && pa_on) {
                mp_pa_enable(false);                 /* 空载关 PA（防底噪） */
                pa_on = false;
            }
            continue;
        }
        underruns = 0;

        /* 线性音量（出口统一缩放，改音量立即生效） */
        int32_t v = s_vol;
        for (size_t i = 0; i < n; i++) {
            int32_t s32 = ((int32_t)out[i] * v) / 100;
            if (s32 > 32767) s32 = 32767;
            if (s32 < -32768) s32 = -32768;
            out[i] = (int16_t)s32;
        }

        if (!pa_on) {
            mp_pa_enable(true);                      /* 有声才开 PA（GPIO46） */
            pa_on = true;
        }
        mp_codec_write(out, n / 2);                        /* 立体声帧数 */
    }
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                             */
/* ------------------------------------------------------------------ */
static esp_timer_handle_t s_task_retry_timer;
static volatile bool s_task_up;   /* bgm 任务是否已就绪（自愈重试判据） */

/* 【为什么不能用 PSRAM 栈 2026-09-27 真机实证】用
 * xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM) 建栈后，任务一碰 Flash
 * （FATFS 读曲库/日志）就命中
 *   `assert failed: spi_flash_disable_interrupts_caches_and_other_other_cpu
 *    (esp_task_stack_is_sane_cache_disabled())`
 * → 1.5s 一次的重启循环。PSRAM 栈在关 cache 的临界区不可访问，本任务必须读 Flash
 * 取流/曲目表，因此只能用**内部 DRAM 栈**。
 * 内部栈 24KB 的可行性：真机日志 `@bgm 任务后（渲染任务未创建）最大块=24564`
 * 说明建立之前最大连续块 ≈34.8KB —— 只要**在其它内部堆客户之前**建栈就能成。
 * 本函数因此把建栈提到 mp_codec_init 之前（解码器 scratch ≈16KB 必须在调用栈上，
 * 见 minimp3.h 文件头栈核算）。 */
static BaseType_t bgm_task_create(void)
{
    return xTaskCreatePinnedToCore(bgm_task, "bgm", MP_BGM_TASK_STACK, NULL, 3,
                                   NULL, 0 /* PRO */);
}

/* 内部堆回稳后补建 bgm 任务（成功即停表自删） */
static void bgm_task_retry_cb(void *arg)
{
    (void)arg;
    if (s_task_up) return;
    if (bgm_task_create() == pdPASS) {
        s_task_up = true;
        ESP_LOGI(TAG, "bgm 任务延迟创建成功（PSRAM 栈）");
        esp_timer_stop(s_task_retry_timer);
        esp_timer_delete(s_task_retry_timer);
        s_task_retry_timer = NULL;
    }
}

void bgm_start(void)
{
    /* 【能力位门控 2026-09-29】无音频板（has_audio=false）：BGM 任务栈
     * 24KB + feeder 4KB 全是内部 DRAM，直接省下。hal_contract.h 已
     * include（MINIPET_ACTIVE_PROFILE 可用）。216 板 has_audio=true 行为不变。 */
    if (!MINIPET_ACTIVE_PROFILE.has_audio) {
        ESP_LOGW(TAG, "本板无音频（has_audio=false）：BGM/feeder 不启动（降级）");
        return;
    }
    s_tbl_lock = xSemaphoreCreateMutex();
    if (!s_tbl_lock) {
        ESP_LOGE(TAG, "tbl mutex alloc failed");
        return;
    }
    s_ring = pcm_ring_create(RING_BYTES);
    if (!s_ring) {
        ESP_LOGE(TAG, "pcm ring alloc failed (PSRAM?)");
        return;
    }
    /* 【建栈顺序 + 栈大小 2026-09-27 真机实证】
     * ① 必须在 mp_codec_init / 渲染任务之前建栈：真机日志显示这两步之后的内部堆
     *    最大连续块只剩 ~24.5KB，24KB 栈建不起来；此刻（刚过 WiFi 初始化）还有
     *    ~34KB 连续块，一次成功。
     * ② 24KB 不是拍脑袋：minimp3.h 文件头写明 `mp3dec_decode_frame` 的
     *    mp3dec_scratch_t ≈16KB 在**调用栈**上 → 栈 <20KB 时解码必崩（真机两种
     *    形态都复现过：6144 → canary stack overflow 重启；10240 → minimp3 内
     *    L3_huffman 空指针 panic，addr2line 已反解到该调用链）。
     * ③ 不能用 PSRAM 栈（xTaskCreatePinnedToCoreWithCaps(SPIRAM) 试过）：本任务
     *    要读 Flash（FATFS 曲目表/流），PSRAM 栈在关 cache 临界区不可访问 →
     *    `assert failed: esp_task_stack_is_sane_cache_disabled()` 1.5s 重启循环。
     * 首建失败仍走 10s 自愈重试（内部堆回稳后补建）。
     * feeder 无解码，4096 够用。 */
    if (bgm_task_create() == pdPASS) {
        s_task_up = true;
    } else {
        ESP_LOGW(TAG, "bgm 任务首建失败（内部堆挤压），转 10s 周期自愈重试");
        const esp_timer_create_args_t timer_args = {
            .callback = bgm_task_retry_cb,
            .name = "bgm_task_retry",
        };
        if (esp_timer_create(&timer_args, &s_task_retry_timer) == ESP_OK) {
            esp_timer_start_periodic(s_task_retry_timer, 10ULL * 1000000ULL);
        }
    }
    xTaskCreatePinnedToCore(feeder_task, "i2s_feed", 4096, NULL, 4, NULL, 0 /* PRO */);
}

/* ---------------- 曲目表 / 播放控制（任意任务上下文，异步生效）--------- */

bool bgm_play_id(uint32_t id)
{
    if (id == 0) return false;                      /* 0=服务端决定，不走选曲 */
    if (s_offline) return false;                    /* 断网静音降级（E8） */
    if (s_greyed[s_source]) return false;           /* 置灰源禁播（E8） */
    mp_audio_msg_t m = { .type = MP_AUDIO_PLAY, .a = (int32_t)id };
    return mp_post_audio(&m);                       /* bgm 任务内锚定表位并起播 */
}

bool bgm_toggle_pause(void)
{
    mp_bgm_state_t st = s_state;
    mp_audio_msg_t m = { .type = MP_AUDIO_NONE, .a = 0 };
    bool to_playing;

    if (st == MP_BGM_PLAYING) {
        m.type = MP_AUDIO_PAUSE;
        to_playing = false;
    } else if (st == MP_BGM_PAUSED) {
        m.type = MP_AUDIO_RESUME;
        to_playing = true;
    } else {
        return false;                               /* IDLE/FAILED：无可切换 */
    }
    /* 返回值为意图态（消息刚入队，bgm 任务尚未落地；控制条回显以
     * bgm_get_state() 轮询为准） */
    mp_post_audio(&m);
    return to_playing;
}

void bgm_next(void)
{
    mp_audio_msg_t m = { .type = MP_AUDIO_NEXT, .a = 0 };
    mp_post_audio(&m);                              /* 表内循环推进（bgm 任务落地） */
}

void bgm_prev(void)
{
    mp_audio_msg_t m = { .type = MP_AUDIO_PREV, .a = 0 };
    mp_post_audio(&m);
}

int bgm_list(uint32_t *ids, char titles[][32], int max)
{
    if (max <= 0 || (!ids && !titles)) return 0;
    if (!s_tbl_lock) return 0;                      /* bgm_start 未跑（异常序） */

    int n = 0;
    xSemaphoreTake(s_tbl_lock, portMAX_DELAY);
    tbl_ensure_locked();                            /* 首调扫 TF（持锁，百 ms 级） */
    for (int i = 0; i < s_tcount && n < max; i++) {
        if (ids) ids[n] = s_tids[i];
        if (titles) memcpy(titles[n], s_titles[i], TBL_TITLE_LEN);   /* 已 32B 定宽 */
        n++;
    }
    xSemaphoreGive(s_tbl_lock);
    return n;
}

void bgm_volume_add(int delta)
{
    if (delta == 0) return;
    /* 经 audio_q 由 bgm 任务落地（clamp/NVS/回传单写者；不阻塞调用方，
     * 音量本身经 feeder 出口即时生效） */
    mp_audio_msg_t m = { .type = MP_AUDIO_VOLUME, .a = (int32_t)delta };
    mp_post_audio(&m);
}

uint8_t bgm_volume_get(void)
{
    return (uint8_t)s_vol;
}


const char *bgm_current_title(void)
{
    if (s_tcur >= 0 && s_tcur < s_tcount && s_titles) return s_titles[s_tcur];
    return "";
}
void bgm_set_offline(bool offline)
{
    if (s_offline == offline) return;
    s_offline = offline;
    if (offline) {
        /* E8：断网=静音降级（不做本地曲库缓存）；回网由用户显式续播 */
        s_playing = false;
        if (s_state == MP_BGM_PLAYING) s_state = MP_BGM_PAUSED;
        pcm_ring_flush(s_ring);
    }
}

mp_bgm_state_t bgm_get_state(void)  { return s_state; }
mp_bgm_source_t bgm_get_source(void){ return (mp_bgm_source_t)s_source; }
uint8_t bgm_get_volume(void)        { return (uint8_t)s_vol; }
bool bgm_source_greyed(mp_bgm_source_t src) { return s_greyed[src]; }
