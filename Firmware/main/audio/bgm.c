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
 *     → PA_CTRL 有声才开（codec_pa_enable）
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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "app_core.h"
#include "hal_contract.h"
#include "http_client.h"
#include "input_dispatch.h"
#include "pcm_ring.h"
#include "minimp3/minimp3.h"

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

static pcm_ring_t *s_ring;

/* ------------------------------------------------------------------ */
/* bgm/cmd 回传（E8：设备端现场控制）                                    */
/* ------------------------------------------------------------------ */
static const char *source_str(mp_bgm_source_t s) { return s ? "qq" : "wz"; }

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
        take = MP3_INBUF_LEN - c->in_len;        /* 理论不可达：decode 后必空 */
    }
    memcpy(c->in + c->in_len, data, take);
    c->in_len += take;

    return decode_pending(c);
}

/* 播放一首：返回 true=自然播完（可续下一首），false=失败/中止 */
static bool play_track(int track_id)
{
    char url[128];
    snprintf(url, sizeof(url), "/api/device/bgm/stream?id=%d&source=%s",
             track_id, source_str((mp_bgm_source_t)s_source));

    memset(&s_sc, 0, sizeof(s_sc));
    s_sc.dec = s_dec;                           /* 解码器常驻复用（不重 init） */
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

static void bgm_play_session(int track_id)
{
    int track = track_id;
    int track_fails = 0;

    while (track_fails < TRACK_FAIL_LIMIT) {
        drain_audio_q_nonblock();                       /* 曲间也收控制 */
        if (!s_playing || s_offline) return;            /* 中止 */
        if (s_greyed[s_source]) return;

        int retries = 0;
        bool ok = false;
        while (retries <= RETRY_SAME_TRACK) {
            if (!s_playing || s_offline) return;
            if (play_track(track)) { ok = true; break; }
            retries++;
        }
        if (!ok) {
            track_fails++;
            /* 同曲重试耗尽 → 跳下一首（仍同源——E8 禁跨源自动换歌） */
            int next = 0;
            if (bgm_cmd("next", 0, &next) != 0 || next <= 0) {
                track_fails++;                   /* 取下一首也失败：加速三振 */
                break;
            }
            track = next;
            continue;
        }

        track_fails = 0;                         /* 有成功播放即复位 */

        /* 自然播完 → 下一首（同源内切歌） */
        int next = 0;
        if (bgm_cmd("next", 0, &next) != 0 || next <= 0) {
            track_fails = 1;
            break;                               /* 拿不到下一首：会话结束 */
        }
        track = next;
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
 * 暂停/停止/切歌立即中止流；音量即时生效；下一首动作记账延后执行 */
static volatile int s_pending_op;      /* 0=无 1=next 2=prev */

static void drain_audio_q_nonblock(void)
{
    mp_audio_msg_t m;
    while (xQueueReceive(mp_audio_q, &m, 0) == pdTRUE) {
        switch (m.type) {
        case MP_AUDIO_PAUSE:
            s_playing = false;
            if (s_state == MP_BGM_PLAYING) s_state = MP_BGM_PAUSED;
            break;
        case MP_AUDIO_STOP:
            s_playing = false;
            s_state = MP_BGM_IDLE;
            pcm_ring_flush(s_ring);
            break;
        case MP_AUDIO_NEXT:
            s_pending_op = 1;
            s_playing = false;
            break;
        case MP_AUDIO_PREV:
            s_pending_op = 2;
            s_playing = false;
            break;
        case MP_AUDIO_VOL:
            if (m.a >= 0 && m.a <= 100) {
                s_vol = (uint8_t)m.a;
                mp_nvs_set_u32("bgm_vol", s_vol);
            }
            break;
        case MP_AUDIO_SOURCE:
            if (m.a == 0 || m.a == 1) {
                s_source = (mp_bgm_source_t)m.a;
                s_greyed[s_source] = false;
                mp_nvs_set_u32("bgm_src", (uint32_t)s_source);
                s_playing = false;
                s_state = MP_BGM_IDLE;
            }
            break;
        default:
            break;                  /* PLAY 期间到达：忽略（先停后放） */
        }
    }
}

static void handle_audio_msg(const mp_audio_msg_t *m)
{
    switch (m->type) {
    case MP_AUDIO_PLAY: {
        if (s_greyed[s_source]) return;                 /* 置灰源禁播（E8） */
        int id = m->a;
        if (id <= 0) {
            if (bgm_cmd("play", 0, &id) != 0 || id <= 0) return;
        }
        s_pending_op = 0;
        s_state = MP_BGM_PLAYING;
        s_playing = true;
        mp_codec_start(s_rate ? s_rate : 44100);
        input_trigger_expression(MP_EXPR_HUM, 1500);     /* E10：BGM 播放=hum */
        bgm_play_session(id);
        break;
    }
    case MP_AUDIO_RESUME:
        if (s_state == MP_BGM_PAUSED) {
            s_state = MP_BGM_PLAYING;
            s_playing = true;
            mp_codec_start(s_rate ? s_rate : 44100);
            bgm_cmd("resume", 0, NULL);                 /* 现场控制回传 */
        }
        break;
    case MP_AUDIO_PAUSE:
        if (s_state == MP_BGM_PLAYING) {
            s_state = MP_BGM_PAUSED;
            s_playing = false;                          /* 流/出声双停 */
            bgm_cmd("pause", 0, NULL);
        }
        break;
    case MP_AUDIO_STOP:
        s_state = MP_BGM_IDLE;
        s_playing = false;
        pcm_ring_flush(s_ring);
        bgm_cmd("stop", 0, NULL);
        break;
    case MP_AUDIO_NEXT:
    case MP_AUDIO_PREV: {
        if (s_state == MP_BGM_FAILED) return;           /* 置灰源禁播 */
        int id = 0;
        if (bgm_cmd(m->type == MP_AUDIO_NEXT ? "next" : "prev", 0, &id) == 0 && id > 0) {
            s_state = MP_BGM_PLAYING;
            s_playing = true;
            mp_codec_start(s_rate ? s_rate : 44100);
            bgm_play_session(id);
        }
        break;
    }
    case MP_AUDIO_VOL:
        if (m->a >= 0 && m->a <= 100) {
            s_vol = (uint8_t)m->a;
            mp_nvs_set_u32("bgm_vol", s_vol);           /* 偏好存设备配置（E8） */
            bgm_cmd("vol", m->a, NULL);
        }
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
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* bgm 任务 / feeder 任务                                                */
/* ------------------------------------------------------------------ */
static mp3dec_t *s_dec;

static void bgm_task(void *arg)
{
    (void)arg;
    s_dec = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_8BIT);  /* 内部 RAM 优先 */
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
            if (!s_dec) continue;                       /* 解码器不可用 */
            if (m.type == MP_AUDIO_VOL) { handle_audio_msg(&m); continue; }
            if (s_offline && m.type != MP_AUDIO_SOURCE && m.type != MP_AUDIO_VOL) {
                continue;                               /* 断网静音降级（E8） */
            }
            handle_audio_msg(&m);

            /* 流中收到的 next/prev 记账，此刻补执行 */
            if (s_pending_op && !s_offline && s_dec) {
                mp_audio_msg_t op = {
                    .type = (s_pending_op == 1) ? MP_AUDIO_NEXT : MP_AUDIO_PREV, .a = 0 };
                s_pending_op = 0;
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
void bgm_start(void)
{
    s_ring = pcm_ring_create(RING_BYTES);
    if (!s_ring) {
        ESP_LOGE(TAG, "pcm ring alloc failed (PSRAM?)");
        return;
    }
    mp_codec_init(44100);
    xTaskCreatePinnedToCore(bgm_task, "bgm", 12288, NULL, 3, NULL, 0 /* PRO */);
    xTaskCreatePinnedToCore(feeder_task, "i2s_feed", 4096, NULL, 4, NULL, 0 /* PRO */);
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
