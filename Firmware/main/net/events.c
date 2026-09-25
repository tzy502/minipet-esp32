/**
 * events.c — POST /api/device/event（E2 事件上报 + E11 降级事件可见）
 */
#include "events.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

static void add_event_json(cJSON *arr, const mp_event_t *e)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", event_name(e->type));
    cJSON_AddNumberToObject(j, "ts", (double)e->ts_ms);
    if (e->a) cJSON_AddNumberToObject(j, "a", e->a);
    if (e->b) cJSON_AddNumberToObject(j, "b", e->b);
    if (e->s[0]) cJSON_AddStringToObject(j, "s", e->s);
    cJSON_AddItemToArray(arr, j);
}

/* 把 asset_dl_collect_hashes() 的逗号分隔 hash 串拆成 JSON 字符串数组 */
static void add_cache_array(cJSON *root, const char *hashes)
{
    cJSON *cache = cJSON_AddArrayToObject(root, "cache");
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

        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "proto", MP_PROTO_VER);
        cJSON_AddStringToObject(root, "deviceId", mp_http_device_id());

        cJSON *evts = cJSON_AddArrayToObject(root, "events");
        if (evts) {
            for (int i = 0; i < n; i++) {
                add_event_json(evts, &batch[i]);
            }
        }

        /* E7/R7：附本地缓存 hash 集（服务端据此计算选择器 cached 标记）。
         * asset_dl 内部做 60s 缓存，避免频繁重读 TF manifest.json。 */
        size_t hlen = asset_dl_collect_hashes(hashes, sizeof(hashes));
        if (hlen > 0) {
            add_cache_array(root, hashes);
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
            ESP_LOGD(TAG, "event post dropped (status=%d)", status);
        }
    }
}

void events_start(void)
{
    xTaskCreatePinnedToCore(events_task, "events", 6144, NULL, 3, NULL, 0 /* PRO */);
}
