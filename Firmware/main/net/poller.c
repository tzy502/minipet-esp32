/**
 * poller.c — 长轮询 + 指数退避重连（E2 / E11）
 *
 * 退避：1s→2s→4s→8s→16s→32s→60s 封顶；任一次成功即复位。
 * 服务端重启/升级期间设备本地模式运行（状态机 OFFLINE），poller 是
 * 「回到网络自动重连并同步」的唯一驱动源（E11）。
 */
#include "poller.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_system.h"

#include "app_core.h"
#include "hal_contract.h"
#include "http_client.h"
#include "asset_dl.h"
#include "ota.h"
#include "state_machine.h"
#include "provision.h"

static const char *TAG = "poller";

#define POLL_PATH_LEN   96
#define POLL_RESP_CAP   4096     /* cmds 批量响应上限 */
#define BACKOFF_MIN_MS  1000
#define BACKOFF_MAX_MS  60000    /* E11: 60s 封顶 */
#define POLL_TIMEOUT_MS 65000    /* 服务端 hold 50s + 余量 */

static uint32_t s_since;         /* 指令游标（服务端 rev 序列；NVS 持久化） */
static uint32_t s_local_rev;     /* 已见 manifest rev（asset_dl 维护本地副本） */
static bool     s_reported_online;

/* ------------------------------------------------------------------ */
/* 单条指令落地                                                          */
/* ------------------------------------------------------------------ */
static void handle_cmd(cJSON *jc)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(jc, "t"));
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(jc, "v"));
    cJSON *jn = cJSON_GetObjectItem(jc, "n");
    int32_t n = jn ? (int32_t)cJSON_GetNumberValue(jn) : 0;
    if (!t) return;

    mp_cmd_t c = { 0 };

    if (strcmp(t, "action") == 0 && v) {
        c.type = MP_CMD_SET_ACTION;
        strlcpy(c.s, v, sizeof(c.s));
        mp_post_cmd(&c);
    } else if (strcmp(t, "expression") == 0 && v) {
        c.type = MP_CMD_SET_EXPRESSION;
        strlcpy(c.s, v, sizeof(c.s));
        mp_post_cmd(&c);
    } else if (strcmp(t, "bubble") == 0 && v) {
        c.type = MP_CMD_BUBBLE;
        strlcpy(c.s, v, sizeof(c.s));
        mp_post_cmd(&c);
    } else if (strcmp(t, "map") == 0 && v) {
        c.type = MP_CMD_SET_MAP;
        strlcpy(c.s, v, sizeof(c.s));
        mp_post_cmd(&c);
    } else if (strcmp(t, "brightness") == 0) {
        c.type = MP_CMD_BRIGHTNESS;
        c.a = n;
        mp_post_cmd(&c);
    } else if (strcmp(t, "reboot") == 0) {
        c.type = MP_CMD_REBOOT;
        mp_post_cmd(&c);
    } else if (strcmp(t, "bgm") == 0 && v) {
        /* E8：控制入口在设备，但 Web/服务端也可下发纯桌宠指令 */
        mp_audio_msg_t m = { 0 };
        if      (strcmp(v, "play") == 0)   m.type = MP_AUDIO_PLAY;
        else if (strcmp(v, "pause") == 0)  m.type = MP_AUDIO_PAUSE;
        else if (strcmp(v, "resume") == 0) m.type = MP_AUDIO_RESUME;
        else if (strcmp(v, "stop") == 0)   m.type = MP_AUDIO_STOP;
        else if (strcmp(v, "next") == 0)   m.type = MP_AUDIO_NEXT;
        else if (strcmp(v, "prev") == 0)   m.type = MP_AUDIO_PREV;
        else if (strcmp(v, "vol") == 0)  { m.type = MP_AUDIO_VOL; m.a = n; }
        else if (strcmp(v, "source") == 0) { m.type = MP_AUDIO_SOURCE; m.a = n; }
        if (m.type != MP_AUDIO_NONE) mp_post_audio(&m);
    } else if (strcmp(t, "ota") == 0 && v) {
        /* E11：poll 指令含固件版本 + 下载地址 */
        const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(jc, "u"));
        mp_ota_offer(v, u ? u : "");
    }
}

/* ------------------------------------------------------------------ */
/* 一次轮询                                                              */
/* ------------------------------------------------------------------ */
/* 响应整体落缓冲（≤4KB，cmds 批量小 JSON）再解析 */
typedef struct {
    char  *buf;
    size_t len, cap;
} resp_ctx_t;

static bool resp_collect(void *ctx, const char *data, size_t len)
{
    resp_ctx_t *r = ctx;
    if (r->len + len < r->cap) {
        memcpy(r->buf + r->len, data, len);
        r->len += len;
        r->buf[r->len] = 0;
        return true;
    }
    return false;    /* 超限：截断（防御服务端异常大响应） */
}

static bool do_poll_once(void)
{
    char path[POLL_PATH_LEN];
    snprintf(path, sizeof(path), "/api/device/poll?since=%lu", (unsigned long)s_since);

    static char resp[POLL_RESP_CAP];
    resp_ctx_t ctx = { .buf = resp, .cap = sizeof(resp) };

    int status = mp_http_get(path, POLL_TIMEOUT_MS, resp_collect, &ctx);
    if (status != 200) {
        ESP_LOGW(TAG, "poll failed: %d", status);
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) return false;

    cJSON *jrev = cJSON_GetObjectItem(root, "rev");
    uint32_t rev = jrev ? (uint32_t)cJSON_GetNumberValue(jrev) : s_since;

    cJSON *cmds = cJSON_GetObjectItem(root, "cmds");
    if (cJSON_IsArray(cmds)) {
        cJSON *jc;
        cJSON_ArrayForEach(jc, cmds) {
            handle_cmd(jc);
        }
    }

    if (rev > s_since) {
        s_since = rev;
        mp_nvs_set_u32("poll_since", s_since);   /* 断电续读（尽力而为） */
    }

    /* manifest rev 前进 → 触发素材 diff（E11 回网补拉；E13 按设备隔离下发） */
    cJSON *mrev = cJSON_GetObjectItem(root, "mrev");
    uint32_t manifest_rev = mrev ? (uint32_t)cJSON_GetNumberValue(mrev) : 0;
    if (manifest_rev && manifest_rev != s_local_rev) {
        s_local_rev = manifest_rev;
        asset_dl_request_sync();
    }

    cJSON_Delete(root);
    return true;
}

/* ------------------------------------------------------------------ */
/* 任务                                                                 */
/* ------------------------------------------------------------------ */
static void poller_task(void *arg)
{
    (void)arg;
    uint32_t backoff_ms = BACKOFF_MIN_MS;

    mp_nvs_get_u32("poll_since", &s_since);
    s_local_rev = asset_dl_local_rev();   /* 本地 manifest rev 起点（可能落后于 asset 任务装载，差一次冗余同步无妨） */

    for (;;) {
        /* WiFi 掉线先重连（OFFLINE 期间唯一回网驱动，E11） */
        if (!provision_wifi_connect_sta(15000)) {
            /* 连不上家网：慢退避重试，不忙转 */
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            if (backoff_ms < BACKOFF_MAX_MS) backoff_ms *= 2;
            if (s_reported_online) {
                s_reported_online = false;
                state_machine_handle(MP_SM_EV_NET_OFFLINE);
            }
            continue;
        }

        bool ok = do_poll_once();

        if (ok) {
            backoff_ms = BACKOFF_MIN_MS;          /* 成功复位退避 */
            if (!s_reported_online) {
                s_reported_online = true;
                state_machine_handle(MP_SM_EV_NET_ONLINE);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            if (backoff_ms < BACKOFF_MAX_MS) backoff_ms *= 2;
            if (s_reported_online) {
                s_reported_online = false;
                state_machine_handle(MP_SM_EV_NET_OFFLINE);
            }
        }
    }
}

void poller_start(void)
{
    xTaskCreatePinnedToCore(poller_task, "poller", 6144, NULL, 3, NULL, 0 /* PRO */);
}
