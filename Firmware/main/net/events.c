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

        /* 【契约对齐 + 网络风暴修复 2026-09-27】
         * 服务端 DeviceEventRequest（DeviceEndpoints.cs:54-62）是【单事件】形状
         * {deviceId,type,tsUtc,data,hashes}，旧的 {events:[...],cache:[...]} 数组
         * 会被 400 拒收（真机实证事件表恒空）。
         * 但"每条一个 POST"同样不可取：真机实证事件突发时会把 lwip/WiFi 缓冲打爆
         * （`Failed to create socket errno=105 No buffer space available` +
         * `wifi:m f null` 数百条）→ 网络栈瘫痪、连带输入/菜单卡死。
         * 折中：一次 POST 带整批——顶层 type 取首条（满足 DTO 必填校验，事件成功
         * 落库并点亮 hashes/cached 标记），完整批次放 data.batch[]（服务端 data 是
         * 任意 JsonElement，原样存，不丢信息）。HTTP 请求数从 N 降到 1。 */
        size_t hlen = asset_dl_collect_hashes(hashes, sizeof(hashes));

        cJSON *root = cJSON_CreateObject();
        if (!root) continue;
        cJSON_AddNumberToObject(root, "proto", MP_PROTO_VER);
        cJSON_AddStringToObject(root, "deviceId", mp_http_device_id());
        cJSON_AddStringToObject(root, "type", event_name(batch[0].type));
        {
            /* tsUtc：以首条事件时刻换算（ts_ms 为开机毫秒，非 epoch） */
            time_t now_s = time(NULL);
            int64_t age_ms = (int64_t)mp_now_ms() - (int64_t)batch[0].ts_ms;
            if (age_ms < 0) age_ms = 0;
            time_t ev_s = now_s - (time_t)(age_ms / 1000);
            struct tm tmv;
            gmtime_r(&ev_s, &tmv);
            char iso[32];
            /* tm_year 是 int（理论 16 位宽），GCC 的 format-truncation 会假定
             * 极端值判超限；实际年份恒为 4 位，故局部关掉该告警。 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
            snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
#pragma GCC diagnostic pop
            cJSON_AddStringToObject(root, "tsUtc", iso);
        }
        cJSON *d = cJSON_AddObjectToObject(root, "data");
        if (d) {
            /* 首条事件的 a/b/s 平铺在 data 顶层（兼容既有消费口径），
             * 整批再以 batch[] 数组携带 */
            if (batch[0].a) cJSON_AddNumberToObject(d, "a", batch[0].a);
            if (batch[0].b) cJSON_AddNumberToObject(d, "b", batch[0].b);
            if (batch[0].s[0]) cJSON_AddStringToObject(d, "s", batch[0].s);
            if (n > 1) {
                cJSON *arr = cJSON_AddArrayToObject(d, "batch");
                if (arr) {
                    for (int i = 1; i < n; i++) {
                        cJSON *je = cJSON_CreateObject();
                        if (!je) continue;
                        cJSON_AddStringToObject(je, "type", event_name(batch[i].type));
                        cJSON_AddNumberToObject(je, "ts", (double)batch[i].ts_ms);
                        if (batch[i].a) cJSON_AddNumberToObject(je, "a", batch[i].a);
                        if (batch[i].b) cJSON_AddNumberToObject(je, "b", batch[i].b);
                        if (batch[i].s[0]) cJSON_AddStringToObject(je, "s", batch[i].s);
                        cJSON_AddItemToArray(arr, je);
                    }
                }
            }
        }
        /* E7/R7：捎带本地缓存 hash 集（服务端据此算选择器 cached 标记） */
        if (hlen > 0) add_hashes_array(root, hashes);

        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!body) continue;

        char resp[RESP_CAP];
        int status = mp_http_post_json("/api/device/event", body,
                                       resp, sizeof(resp), POST_TIMEOUT_MS);
        free(body);
        if (status != 200) {
            /* 有界损失：失败即丢，不重放（队列不积压，事件价值随时间衰减） */
            ESP_LOGD(TAG, "event post dropped (n=%d type=%s status=%d)",
                     n, event_name(batch[0].type), status);
        }
    }
}

void events_start(void)
{
    xTaskCreatePinnedToCore(events_task, "events", 4096, NULL, 3, NULL, 0 /* PRO */);
}
