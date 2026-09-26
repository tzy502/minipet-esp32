/**
 * http_client.c — esp_http_client 封装（E2 协议端点的统一入口）
 *
 * 设计要点：
 *   - 每请求一次性 client（局域网单服务器场景，连接复用收益小、状态简单）
 *   - 统一 open/write → fetch_headers → 循环 read 的流式骨架，
 *     asset_dl / ota / bgm 都用 chunk_cb 拿流，不整载内存（4.4）
 *   - 服务器地址唯一来源 NVS "srv_url"（provision 配网页写入）
 */
#include "http_client.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "cJSON.h"

#include "app_core.h"
#include "hal_contract.h"

static const char *TAG = "http";

#define URL_BUF_LEN   256
#define READ_CHUNK    2048

static char s_server_url[128];      /* 无结尾斜杠 */
static char s_uuid[13];             /* 12 hex + NUL */
static char s_device_id[40];        /* hello 返回；未注册时 = s_uuid */

/* ------------------------------------------------------------------ */
/* 初始化                                                               */
/* ------------------------------------------------------------------ */
void mp_http_init(void)
{
    if (s_uuid[0] == 0) {
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_uuid, sizeof(s_uuid), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        strlcpy(s_device_id, s_uuid, sizeof(s_device_id));   /* 匿名兜底（E13） */
    }
    if (!mp_nvs_get_str("srv_url", s_server_url, sizeof(s_server_url))) {
        s_server_url[0] = 0;
    }
    size_t len = strlen(s_server_url);
    while (len > 0 && s_server_url[len - 1] == '/') s_server_url[--len] = 0;
    /* scheme 归一化（真机两坑）：①省略 scheme → 补 http://；②手机浏览器
     * 自动升级 https:// → NAS 服务端为纯 HTTP，强制回 http（否则 TLS 握手
     * 被重置 → abort 重启循环） */
    if (s_server_url[0]) {
        char tmp[sizeof(s_server_url)];
        if (strncmp(s_server_url, "http://", 7) == 0) {
            /* 已是 http，保持 */
        } else if (strncmp(s_server_url, "https://", 8) == 0) {
            snprintf(tmp, sizeof(tmp), "http://%s", s_server_url + 8);
            strlcpy(s_server_url, tmp, sizeof(s_server_url));
        } else {
            snprintf(tmp, sizeof(tmp), "http://%s", s_server_url);
            strlcpy(s_server_url, tmp, sizeof(s_server_url));
        }
    }
}

const char *mp_http_server_url(void) { return s_server_url[0] ? s_server_url : NULL; }
const char *mp_http_uuid(void)       { return s_uuid; }
const char *mp_http_device_id(void)  { return s_device_id; }

/* ------------------------------------------------------------------ */
/* 通用事务：GET / POST，流式回调                                        */
/* ------------------------------------------------------------------ */
static int http_txn(const char *url, bool is_post, const char *body,
                    mp_http_chunk_cb cb, void *ctx,
                    char *resp_buf, size_t resp_cap, uint32_t timeout_ms)
{
    if (!url || url[0] == 0) return -1;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = 2048,
        .keep_alive_enable = true,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return -1;

    int status = -1;
    char *chunk = malloc(READ_CHUNK);
    if (!chunk) goto out;

    if (is_post) {
        esp_http_client_set_method(h, HTTP_METHOD_POST);
        esp_http_client_set_header(h, "Content-Type", "application/json");
    }

    int body_len = (body ? (int)strlen(body) : 0);
    if (esp_http_client_open(h, body_len) != ESP_OK) goto out;
    if (body_len > 0) {
        int w = esp_http_client_write(h, body, body_len);
        if (w < 0) goto out_close;
    }

    if (esp_http_client_fetch_headers(h) < 0) goto out_close;
    status = esp_http_client_get_status_code(h);

    /* 流式读：优先给回调；小响应用兜底缓冲（hello/poll 的 JSON） */
    size_t resp_len = 0;
    for (;;) {
        int r = esp_http_client_read(h, chunk, READ_CHUNK);
        if (r <= 0) break;                    /* 0=EOF，-1=超时/结束 */
        bool keep = true;
        if (cb) keep = cb(ctx, chunk, (size_t)r);
        if (resp_buf && resp_cap > 1 && resp_len < resp_cap - 1) {
            size_t cpy = (size_t)r;
            if (cpy > resp_cap - 1 - resp_len) cpy = resp_cap - 1 - resp_len;
            memcpy(resp_buf + resp_len, chunk, cpy);
            resp_len += cpy;
        }
        if (!keep) break;                     /* 调用方主动中止 */
    }
    if (resp_buf) resp_buf[resp_len] = 0;

out_close:
    esp_http_client_close(h);
out:
    free(chunk);
    esp_http_client_cleanup(h);
    return status;
}

static int build_url(char *buf, size_t cap, const char *path_or_url)
{
    if (path_or_url[0] == 'h' && strncmp(path_or_url, "http", 4) == 0) {
        snprintf(buf, cap, "%s", path_or_url);       /* 绝对 URL（OTA/BGM 直链） */
    } else if (path_or_url[0] != '/') {
        /* 配网页用户常省略 scheme（如 192.168.3.46:38090）→ 补 http://（真机定稿） */
        snprintf(buf, cap, "http://%s", path_or_url);
    } else {
        const char *base = mp_http_server_url();
        if (!base) return -1;
        snprintf(buf, cap, "%s%s", base, path_or_url);
    }
    return 0;
}

int mp_http_get(const char *path_or_url, uint32_t timeout_ms,
                mp_http_chunk_cb cb, void *ctx)
{
    char url[URL_BUF_LEN];
    if (build_url(url, sizeof(url), path_or_url) != 0) return -1;
    return http_txn(url, false, NULL, cb, ctx, NULL, 0, timeout_ms);
}

int mp_http_post_json(const char *path, const char *json_body,
                      char *resp_buf, size_t resp_cap, uint32_t timeout_ms)
{
    char url[URL_BUF_LEN];
    if (build_url(url, sizeof(url), path) != 0) return -1;
    return http_txn(url, true, json_body, NULL, NULL, resp_buf, resp_cap, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* POST /api/device/hello（E2）                                         */
/* ------------------------------------------------------------------ */

/* 读数值字段：primary 优先，legacy 兼容旧服务端；缺/非数返回 false。
 * （直接对 GetNumberValue 的返回值做整型截断在缺字段时是 NaN→未定义行为） */
static bool json_num2(const cJSON *obj, const char *primary,
                      const char *legacy, double *out)
{
    const cJSON *j = cJSON_GetObjectItem(obj, primary);
    if (!j && legacy) j = cJSON_GetObjectItem(obj, legacy);
    if (!j || !cJSON_IsNumber(j)) return false;
    *out = cJSON_GetNumberValue(j);
    return true;
}

int mp_http_hello(void)
{
    if (!mp_http_server_url()) return -1;

    const minipet_profile_t *prof = &MINIPET_PROFILE_AMOLED216;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "proto", MP_PROTO_VER);
    cJSON_AddStringToObject(root, "uuid", s_uuid);
    /* 服务端 HelloRequest DTO 绑定字段是 firmware（System.Text.Json web 默认
     * camelCase）；旧字段 fw 保留一份兼容旧服务端 */
    cJSON_AddStringToObject(root, "firmware", MP_FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "fw", MP_FIRMWARE_VERSION);

    cJSON *pr = cJSON_CreateObject();                 /* E2: profile */
    cJSON_AddNumberToObject(pr, "w", prof->width);
    cJSON_AddNumberToObject(pr, "h", prof->height);
    cJSON_AddStringToObject(pr, "shape", prof->shape_name);   /* "round"/"square" */
    cJSON_AddNumberToObject(pr, "psram", prof->psram_mb);
    cJSON_AddBoolToObject(pr, "audio", prof->has_audio);
    cJSON_AddItemToObject(root, "profile", pr);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return -1;

    char resp[1024] = { 0 };
    int status = mp_http_post_json("/api/device/hello", body, resp, sizeof(resp), 8000);
    free(body);
    if (status != 200) return status ? status : -1;

    cJSON *r = cJSON_Parse(resp);
    if (!r) return -2;

    const char *did = cJSON_GetStringValue(cJSON_GetObjectItem(r, "deviceId"));
    if (did) strlcpy(s_device_id, did, sizeof(s_device_id));

    /* 服务端配置（阈值 Web 可配——E6；与 software-design 2.4 同名语义）。
     * 字段名与 DeviceEndpoints.cs hello handler 对照：config.idleToClockMin /
     * imuDeadzoneDeg / tapLightG / tapHardG（camelCase 原样）；idleMin 为旧
     * 服务端兼容回退 */
    cJSON *cfg = cJSON_GetObjectItem(r, "config");
    if (cJSON_IsObject(cfg)) {
        double d;
        if (json_num2(cfg, "idleToClockMin", "idleMin", &d))
            g_mp_cfg.idle_to_clock_min = (uint8_t)d;
        if (json_num2(cfg, "imuDeadzoneDeg", NULL, &d))
            g_mp_cfg.imu_deadzone_deg = (float)d;
        if (json_num2(cfg, "tapLightG", NULL, &d))
            g_mp_cfg.tap_light_g = (float)d;
        if (json_num2(cfg, "tapHardG", NULL, &d))
            g_mp_cfg.tap_hard_g = (float)d;
    }

    /* E13：首配对码（服务端入册后返回，已绑定则无此字段）。
     * 真机根因修复：服务端实际下发字段是 pairingCode（DeviceEndpoints.cs
     * hello handler），旧实现只读 pair → 配对码永远不显示。pairingCode
     * 优先，pair 保留兼容旧服务端 */
    const char *pair = cJSON_GetStringValue(cJSON_GetObjectItem(r, "pairingCode"));
    if (!pair || !pair[0])
        pair = cJSON_GetStringValue(cJSON_GetObjectItem(r, "pair"));
    if (pair && pair[0]) {
        mp_cmd_t c = { .type = MP_CMD_PAIRING_CODE };
        strlcpy(c.s, pair, sizeof(c.s));
        mp_post_cmd(&c);
    }

    cJSON_Delete(r);
    ESP_LOGI(TAG, "hello ok, deviceId=%s", s_device_id);
    return 0;
}
