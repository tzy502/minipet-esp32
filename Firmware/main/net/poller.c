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
#define POLL_TIMEOUT_RETRY_MS 5000   /* 超时（-1）后的快速重试间隔：保住心跳新鲜度 */

static uint32_t s_since;         /* 指令游标（服务端 rev 序列；NVS 持久化） */
static uint32_t s_local_rev;     /* 已见 manifest rev（asset_dl 维护本地副本） */
static bool     s_reported_online;
static int      s_last_poll_status;   /* 最近一次 poll 的 HTTP 状态（-1=超时） */
static int      s_poll_fail_streak;   /* 连续真失败次数（达到阈值重新 hello） */
#define POL_REHELLO_FAILS 3           /* 掉线自愈：连续真失败达此次数 → 重新 hello */
static int      s_hello_fail_streak;  /* hello 连续失败（达 3 次强制重新关联） */

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
    char path[POLL_PATH_LEN];    /* poll 端点服务端必填 deviceId（DeviceEndpoints.cs HandlePoll 签名
     * string deviceId）——真机实证：不带参 400，设备从此只有 hello 没有心跳
     * （服务端日志只见「hello 心跳」、online 转 false、指令队列永不消费）。 */
    snprintf(path, sizeof(path), "/api/device/poll?deviceId=%s&since=%lu",
             mp_http_device_id(), (unsigned long)s_since);

    static char resp[POLL_RESP_CAP];
    resp_ctx_t ctx = { .buf = resp, .cap = sizeof(resp) };

    int status = mp_http_get(path, POLL_TIMEOUT_MS, resp_collect, &ctx);
    if (status != 200) {
        /* 【心跳抖动修复 2026-09-27】status=-1 = 客户端超时（服务端长轮询 hold
         * 未在期限内返回/响应丢失），并非网络故障——此时设备其实健在。
         * 原实现一律走指数退避（最长 60s），叠加 poll 自身的 50s hold 后，
         * 单次超时即造成 lastSeen 空窗 >90s → 服务端 OnlineWindow(90s) 判离线，
         * 表现为「配对成功但没心跳 / 状态忽上忽下」。现由 do_poll_once 把
         * 超时与真失败区分开，交给调用方选择退避策略。 */
        ESP_LOGW(TAG, "poll failed: %d%s", status,
                 status == -1 ? "（超时：视为心跳抖动，快速重试）" : "");
        s_last_poll_status = status;
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) return false;

    cJSON *jrev = cJSON_GetObjectItem(root, "rev");
    uint32_t rev = jrev ? (uint32_t)cJSON_GetNumberValue(jrev) : s_since;

    /* 服务端实际响应（DeviceEndpoints.cs）：{ commands:[{seq,type,payload}], lastSeq, mrev }
     * payload 可能是对象（map:{id} / ota:{ver,url}）或字符串——统一摊平成
     * handle_cmd 期望的 {t,v,n,u} 视图后复用既有语义映射 */
    cJSON *cmds = cJSON_GetObjectItem(root, "commands");
    if (!cJSON_IsArray(cmds)) cmds = cJSON_GetObjectItem(root, "cmds");  /* 兼容旧口径 */
    if (cJSON_IsArray(cmds)) {
        cJSON *jc;
        cJSON_ArrayForEach(jc, cmds) {
            cJSON *t = cJSON_GetObjectItem(jc, "type");
            cJSON *payload = cJSON_GetObjectItem(jc, "payload");
            if (!t) { handle_cmd(jc); continue; }   /* 旧口径直通 */
            /* 构造 {t, v, n, u} 视图：v=字符串 payload 或对象中的字符串字段 */
            cJSON *vitem = NULL;
            if (cJSON_IsString(payload)) vitem = payload;
            cJSON *pid = payload ? cJSON_GetObjectItem(payload, "id") : NULL;
            cJSON *pver = payload ? cJSON_GetObjectItem(payload, "ver") : NULL;
            cJSON *purl = payload ? cJSON_GetObjectItem(payload, "url") : NULL;
            cJSON *pn = payload ? cJSON_GetObjectItem(payload, "n") : NULL;

            if (pid && cJSON_IsString(pid)) {           /* map: {"id":...} */
                vitem = pid;
            } else if (pver && cJSON_IsString(pver)) {  /* ota: {"ver","url"} */
                vitem = pver;
            } else if (!vitem) {
                /* 【契约补链 2026-09-27】服务端 CommandQueue.EnqueueLegacy 产出
                 * payload = {"v":"vol","n":50}（旧口径），固件此前只认 payload 本身
                 * 是字符串或 payload.id/ver/url → 这类指令被静默丢弃（Web 音量下发
                 * 因此永远无效）。现补读 payload.v（字符串）作为 vitem。 */
                cJSON *pv = cJSON_GetObjectItem(payload, "v");
                if (pv && cJSON_IsString(pv)) vitem = pv;
                else if (pn) vitem = NULL;              /* 纯数值型走 n 通道 */
            }
            /* —— 专用解析：type + payload 字段 → 既有 handle_cmd 语义 —— */
            {
                const char *ts = t->valuestring;
                char tbuf[24];
                strlcpy(tbuf, ts, sizeof(tbuf));
                cJSON jv = { 0 };
                jv.type = cJSON_IsString(vitem) ? cJSON_String : cJSON_Number;
                if (cJSON_IsString(vitem)) jv.valuestring = vitem->valuestring;
                else if (pn) jv.valuedouble = cJSON_GetNumberValue(pn);

                cJSON jn = { 0 };
                if (pn) { jn.type = cJSON_Number; jn.valuedouble = cJSON_GetNumberValue(pn); }

                cJSON ju = { 0 };
                if (purl && cJSON_IsString(purl)) { ju.type = cJSON_String; ju.valuestring = purl->valuestring; }

                /* 直接内联 handle_cmd 的等价处理（构造伪节点传入） */
                if (strcmp(tbuf, "action") == 0 && cJSON_IsString(vitem)) {
                    mp_cmd_t c = { 0 }; c.type = MP_CMD_SET_ACTION;
                    strlcpy(c.s, vitem->valuestring, sizeof(c.s)); mp_post_cmd(&c);
                } else if (strcmp(tbuf, "expression") == 0 && cJSON_IsString(vitem)) {
                    mp_cmd_t c = { 0 }; c.type = MP_CMD_SET_EXPRESSION;
                    strlcpy(c.s, vitem->valuestring, sizeof(c.s)); mp_post_cmd(&c);
                } else if (strcmp(tbuf, "bubble") == 0 && cJSON_IsString(vitem)) {
                    mp_cmd_t c = { 0 }; c.type = MP_CMD_BUBBLE;
                    strlcpy(c.s, vitem->valuestring, sizeof(c.s)); mp_post_cmd(&c);
                } else if (strcmp(tbuf, "map") == 0 && cJSON_IsString(pid)) {
                    mp_cmd_t c = { 0 }; c.type = MP_CMD_SET_MAP;
                    strlcpy(c.s, pid->valuestring, sizeof(c.s)); mp_post_cmd(&c);
                } else if (strcmp(tbuf, "brightness") == 0 && pn) {
                    mp_cmd_t c = { 0 }; c.type = MP_CMD_BRIGHTNESS;
                    c.a = (int32_t)cJSON_GetNumberValue(pn); mp_post_cmd(&c);
                } else if (strcmp(tbuf, "reboot") == 0) {
                    mp_cmd_t c = { 0 }; c.type = MP_CMD_REBOOT; mp_post_cmd(&c);
                } else if (strcmp(tbuf, "bgm") == 0 && cJSON_IsString(vitem)) {
                    mp_audio_msg_t m = { 0 };
                    const char *vv = vitem->valuestring;
                    if      (strcmp(vv, "play") == 0)   m.type = MP_AUDIO_PLAY;
                    else if (strcmp(vv, "pause") == 0)  m.type = MP_AUDIO_PAUSE;
                    else if (strcmp(vv, "resume") == 0) m.type = MP_AUDIO_RESUME;
                    else if (strcmp(vv, "stop") == 0)   m.type = MP_AUDIO_STOP;
                    else if (strcmp(vv, "next") == 0)   m.type = MP_AUDIO_NEXT;
                    else if (strcmp(vv, "prev") == 0)   m.type = MP_AUDIO_PREV;
                    else if (strcmp(vv, "vol") == 0) {
                        /* 【Web BGM 控制补链 2026-09-27】音量走数值通道：
                         * 服务端 payload = {n:0..100}，vitem 恒 NULL → 此前落到
                         * 本分支外被静默丢弃，音量永远无效。现显式认 "vol"。 */
                        if (pn) {
                            m.type = MP_AUDIO_VOL;
                            m.a = (int32_t)cJSON_GetNumberValue(pn);
                        }
                    }
                    if (m.type != MP_AUDIO_NONE) mp_post_audio(&m);
                } else if (strcmp(tbuf, "ota") == 0 && pver && cJSON_IsString(pver)) {
                    const char *u = (purl && cJSON_IsString(purl)) ? purl->valuestring : "";
                    mp_ota_offer(pver->valuestring, u);
                }
            }
        }
    }

    cJSON *jls = cJSON_GetObjectItem(root, "lastSeq");
    if (jls) rev = (uint32_t)cJSON_GetNumberValue(jls);
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

    /* 【心跳缺失最终根因 2026-09-27】本任务在 app_main 里先于
     * state_machine_boot() 启动，而 HTTP 服务器地址只在 state_machine_boot
     * 里经 mp_http_init() 装载（NVS srv_url）。此前 poller 自身不初始化 →
     * mp_http_server_url() 恒 NULL → http_txn() 首行 `if (!url) return -1`
     * 静默返回，设备【从不轮询】且连失败日志都没有：服务端只见开机那一刻
     * 的 hello，online 随后转 false（"配对成功但没有心跳"）。
     * mp_http_init 幂等（仅按需填 uuid/读 NVS），此处主动调用保证轮询可用。 */
    mp_http_init();

    mp_nvs_get_u32("poll_since", &s_since);
    s_local_rev = asset_dl_local_rev();   /* 本地 manifest rev 起点（可能落后于 asset 任务装载，差一次冗余同步无妨） */

    for (;;) {
        extern bool provision_portal_active(void);   /* portal 期间停轮询：无配置时对不可达服务端的重试会耗尽 lwip 缓冲（listen ENOBUFS 根因） */
        while (provision_portal_active()) vTaskDelay(pdMS_TO_TICKS(1000));

        /* 【真机根因 2026-09-27】本任务在 app_main 里先于 state_machine_boot()
         * 启动，而 deviceId 是 hello 从服务端取回的。此前会在 hello 之前就用
         * UUID 匿名回退值发 poll → 服务端 404「未注册设备」→ 设备永远回不到
         * 在线（真机日志：poll?deviceId=44BD8D60DAC0&since=0 + HTTP_CONNECT 失败）。
         * 现等 hello 完成再轮询。
         *
         * 【死锁修复 2026-09-27（二）】原实现是 `if (!hello_done) { delay; continue; }`
         * —— 只等待、**不去连网**。而 hello 又必须在拿到 IP 之后才能发，于是
         * "开机首次 hello 失败"（服务端不可达 / 路由器暂时拒连）之后设备永久卡死：
         * poller 既不连网也不补 hello，自检阶段给的那次 20s 窗口成了唯一机会
         * （真机现象：设备在网、ping 得通，但服务端永远 offline）。
         * 现在改为：先确保连上家网，再补发 hello；补上后才进入正常轮询。 */
        if (!mp_http_hello_done()) {
            ESP_LOGW("poller", "hello 未完成 → 先确保联网再补发（自检阶段失败后的唯一补救路径）");
            if (!provision_wifi_connect_sta(15000)) {
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                if (backoff_ms < BACKOFF_MAX_MS) backoff_ms *= 2;
                continue;
            }
            if (mp_http_hello() == 0) {
                ESP_LOGW("poller", "hello 补发成功 → 立即请求素材同步并回到在线");
                asset_dl_request_sync();
                backoff_ms = BACKOFF_MIN_MS;
                s_hello_fail_streak = 0;
            } else {
                /* 【僵局打破 2026-09-27】"有 IP 但 TCP 连不通"时 connect_sta 会
                 * 立刻返回 OK（它只看 netif 有没有地址），于是我们一直在同一个
                 * 坏关联上重试 hello —— 真机上可以卡很久。
                 * 连续失败 3 次后强制断开重连（换一次关联/BSSID），并给路由器
                 * 留 3s 冷却：本板在 reason=2 时 2.6s 就重试一次，过密的认证
                 * 重试本身就可能被 AP 的认证风暴保护继续拒绝。 */
                if (++s_hello_fail_streak >= 3) {
                    s_hello_fail_streak = 0;
                    ESP_LOGW("poller", "hello 连续失败 3 次（有 IP 但 TCP 不通）→ 强制重新关联");
                    provision_wifi_force_reconnect();
                    vTaskDelay(pdMS_TO_TICKS(3000));
                }
                ESP_LOGW("poller", "hello 补发仍失败 → 退避重试");
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                if (backoff_ms < BACKOFF_MAX_MS) backoff_ms *= 2;
                continue;
            }
        }

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

        /* E9 常态化校时：借本任务执行（不新建 rtcsync 任务，绕开内部堆碎片） */
        provision_rtc_resync_step();

        /* E14 设备日志增量上报：同样借本任务（不新建任务 —— 内部堆约束）。
         * 内部自带 20s 心跳/失败退避，poll 长轮询期间不会叠加请求。 */
        mp_http_device_log_step();

        bool ok = do_poll_once();

        /* 长轮询可能 hold 50s：再补一次日志上报机会（内部节流，不会连发） */
        mp_http_device_log_step();

        if (ok) {
            s_last_poll_status = 200;
            s_poll_fail_streak = 0;
            backoff_ms = BACKOFF_MIN_MS;          /* 成功复位退避 */
            if (!s_reported_online) {
                s_reported_online = true;
                state_machine_handle(MP_SM_EV_NET_ONLINE);
            }
        } else {
            /* 【心跳新鲜度】超时（-1）≠ 网络故障：只等 5s 就重试，避免 60s 退避
             * 把 lastSeen 空窗拖过服务端 90s 在线窗口（真机"状态忽上忽下"根因）。
             * 真失败（连接被拒/无路由等）仍走指数退避，保护 lwip 缓冲。 */
            if (s_last_poll_status == -1) {
                vTaskDelay(pdMS_TO_TICKS(POLL_TIMEOUT_RETRY_MS));
                backoff_ms = BACKOFF_MIN_MS;      /* 超时不累积退避 */
                continue;                          /* 仍在线：不报 NET_OFFLINE */
            }
            /* 【掉线自愈 2026-09-27】连续真失败 ≥ POL_REHELLO_FAILS 次 → 重新 hello。
             * 场景：路由器把设备踢掉（真机 reason=2/8 每 2~4 分钟一次）后，设备的
             * deviceId/服务端侧注册可能已过期或路由器换了网关，只重试 poll 会一直
             * 404/连接失败；重新 hello 可重建注册并把地址探测再走一遍。
             * 限频：成功后清零，避免把 hello 变成每轮都发的负担。 */
            if (++s_poll_fail_streak >= POL_REHELLO_FAILS) {
                s_poll_fail_streak = 0;
                ESP_LOGW(TAG, "poll 连续失败 %d 次 → 重新 hello 重建注册", POL_REHELLO_FAILS);
                if (mp_http_hello() == 0) {
                    ESP_LOGW(TAG, "重新 hello 成功 → 请求素材同步");
                    asset_dl_request_sync();
                } else {
                    ESP_LOGW(TAG, "重新 hello 仍失败 → 继续退避重连");
                }
            }
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
    /* 【真机栈溢出修复 2026-09-27】原栈 4096 太小：poller 内要跑
     * `mp_http_get` → esp_http_client + esp-tls 握手（内部深调用链）+
     * http_txn 里 malloc(READ_CHUNK) 与 cJSON 解析响应。
     * 真机症状（服务端恢复后设备仍不重连）：任务静默卡死/栈破坏 →
     * 连 `poll failed` 日志都打不出，同时网络栈报 `wifi:m f null` 92 次/20s。
     * 提栈到 8192（失败则回退 4096 保证任务至少存在）。 */
    if (xTaskCreatePinnedToCore(poller_task, "poller", 8192, NULL, 3, NULL,
                                0 /* PRO */) != pdPASS) {
        ESP_LOGW(TAG, "poller 8K 栈任务创建失败 → 回退 4096");
        xTaskCreatePinnedToCore(poller_task, "poller", 4096, NULL, 3, NULL, 0);
    }
}
