/**
 * events.c — POST /api/device/event（E2 事件上报 + E11 降级事件可见）
 */
#include "events.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"

#include "app_core.h"
#include "http_client.h"
#include "asset_dl.h"

static const char *TAG = "events";

#define BATCH_MAX        8            /* 单次 POST 最多事件数 */
#define RESP_CAP         256
#define POST_TIMEOUT_MS  8000

/* 事件类型 → 协议字符串（E2：触摸/力度分级/倾斜变迁/低电/错误/缓存hash集） */
static const char *event_name(mp_event_type_t t)
{
    switch (t) {
    case MP_EVT_TOUCH_PET:    return "touch";
    case MP_EVT_TAP_LIGHT:    return "tap_light";
    case MP_EVT_TAP_HARD:     return "tap_hard";
    case MP_EVT_SHAKE:        return "shake";
    case MP_EVT_PICKUP:       return "pickup";
    case MP_EVT_TILT_ENTER:   return "tilt_enter";
    case MP_EVT_TILT_EXIT:    return "tilt_exit";
    case MP_EVT_BATTERY_LOW:  return "battery";
    case MP_EVT_ASSET_ERROR:  return "asset_error";
    case MP_EVT_BGM_FAILOVER: return "bgm_failover";
    case MP_EVT_BOOT:         return "boot";
    case MP_EVT_ERROR:        return "error";
    default:                  return "unknown";
    }
}

/* 【已删除】旧报文形状 {events:[{type,ts,a,b,s}]} 的组装函数 add_event_json：
 * 服务端 DeviceEventRequest 是单事件 DTO，旧形状被 400 拒收（真机实证事件表
 * 恒空），故 events_task 改为逐条按新形状发送，本函数随之删除。 */

/* 把 asset_dl_collect_hashes() 的逗号分隔 hash 串拆成 JSON 字符串数组
 * （服务端 DTO 字段名 = hashes，非旧报文的 cache） */
static void add_hashes_array(cJSON *root, const char *hashes)
{
    cJSON *cache = cJSON_AddArrayToObject(root, "hashes");
    if (!cache) return;
    const char *p = hashes;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char one[24];
        if (len > 0 && len < sizeof(one)) {
            memcpy(one, p, len);
            one[len] = 0;
            cJSON_AddItemToArray(cache, cJSON_CreateString(one));
        }
        if (!comma) break;
        p = comma + 1;
    }
}

/* ------------------------------------------------------------------ */
/* 任务                                                                 */
/* ------------------------------------------------------------------ */
static void events_task(void *arg)
{
    (void)arg;
    static mp_event_t batch[BATCH_MAX];
    static char hashes[MP_HASH_LIST_CAP];

    for (;;) {
        /* 阻塞等第一件，再非阻塞捎带（降低请求频次） */
        if (xQueueReceive(mp_event_q, &batch[0], portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int n = 1;
        while (n < BATCH_MAX && xQueueReceive(mp_event_q, &batch[n], 0) == pdTRUE) {
            n++;
        }

        /* 【契约对齐】服务端 DeviceEventRequest（DeviceEndpoints.cs:54-62）是
         * 【单事件】形状：{deviceId, type, tsUtc, data, hashes}。此前固件发
         * {events:[...], cache:[...]} 数组 → 服务端 body.Type 为空 → 400，
         * 真机实证事件表恒空（E11 健康上报全丢、E7 cached 永不写入）。
         * 现改为：批内逐条按服务端 DTO 形状发送，hashes 随首条捎带（cached
         * 标记只需写一次）；批量语义不变，仅换报文形状。 */
        size_t hlen = asset_dl_collect_hashes(hashes, sizeof(hashes));

        for (int i = 0; i < n; i++) {
            const mp_event_t *e = &batch[i];
            cJSON *root = cJSON_CreateObject();
            if (!root) break;
            cJSON_AddNumberToObject(root, "proto", MP_PROTO_VER);
            cJSON_AddStringToObject(root, "deviceId", mp_http_device_id());
            cJSON_AddStringToObject(root, "type", event_name(e->type));
            /* tsUtc：DateTime 解析 ISO8601；ts_ms 为设备开机毫秒（非 epoch），
             * 换算成“现在 - (开机时长 - 事件时刻)”的近似 UTC 时间戳。
             * 时钟未校准（1970）时服务端拿到的也是 1970，属可接受的降级。 */
            {
                time_t now_s = time(NULL);
                int64_t age_ms = (int64_t)mp_now_ms() - (int64_t)e->ts_ms;
                if (age_ms < 0) age_ms = 0;
                time_t ev_s = now_s - (time_t)(age_ms / 1000);
                struct tm tmv;
                gmtime_r(&ev_s, &tmv);
                char iso[32];
                /* tm_year 是 int（理论 16 位宽），GCC 的 format-truncation 会
                 * 假定极端值判超限；实际年份恒为 4 位，故局部关掉该告警。 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
                snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                         tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
#pragma GCC diagnostic pop
                cJSON_AddStringToObject(root, "tsUtc", iso);
            }
            /* 事件载荷：a/b/s 折进 data（服务端 data 为 JsonElement，任意对象） */
            if (e->a || e->b || e->s[0]) {
                cJSON *d = cJSON_AddObjectToObject(root, "data");
                if (d) {
                    if (e->a) cJSON_AddNumberToObject(d, "a", e->a);
                    if (e->b) cJSON_AddNumberToObject(d, "b", e->b);
                    if (e->s[0]) cJSON_AddStringToObject(d, "s", e->s);
                }
            }
            /* E7/R7：首条捎带本地缓存 hash 集（服务端据此算选择器 cached 标记） */
            if (i == 0 && hlen > 0) {
                add_hashes_array(root, hashes);
            }

            char *body = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            if (!body) continue;

            char resp[RESP_CAP];
            int status = mp_http_post_json("/api/device/event", body,
                                           resp, sizeof(resp), POST_TIMEOUT_MS);
            free(body);
            if (status != 200) {
                /* 有界损失：失败即丢，不重放（队列不积压，事件价值随时间衰减） */
                ESP_LOGD(TAG, "event post dropped (type=%s status=%d)",
                         event_name(e->type), status);
            }
        }
    }
}

void events_start(void)
{
    xTaskCreatePinnedToCore(events_task, "events", 4096, NULL, 3, NULL, 0 /* PRO */);
}
