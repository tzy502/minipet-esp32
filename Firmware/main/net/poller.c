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
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

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
static int      s_last_poll_status;   /* 最近一次 poll 的 HTTP 状态（-1=超时或建连失败） */
static int      s_poll_fail_streak;   /* 连续真失败次数（达到阈值重新 hello） */
#define POL_REHELLO_FAILS 3           /* 掉线自愈：连续真失败达此次数 → 重新 hello */
static int      s_hello_fail_streak;  /* hello 连续失败（达 3 次强制重新关联） */

/* ------------------------------------------------------------------ */
/* 【回网自愈 2026-09-27】可达性门 + 回网补 hello                          */
/* ------------------------------------------------------------------ */
/* 真机死结：http_txn 在 open（TCP 建连）失败时也返回 -1，与"长轮询读超时"同码；
 * 而 poll 的 timeout=65s 是"建连+读"的总预算 —— AP 半死（有 IP 但 SYN 进黑洞）
 * 时一次 poll 要阻塞 65s，调用方还把它当"心跳抖动"再等 5s，于是既不重连也不补
 * hello，服务端 lastSeen 空窗轻松超过 2 分钟（设备在网、ping 得通，服务端 offline）。
 * 现在每轮 poll 前用 1.5s 裸 socket 探针先判定：探不通 = 真断线（立刻退避/重连，
 * 不浪费 65s）；探得通 = 上一轮 -1 只是长轮询抖动（保持 5s 快速重试、不误报离线）。 */
#define POLL_PROBE_MS      1500   /* 探针建连上限（局域网正常 <20ms） */
#define POL_UNREACH_RECONN 2      /* 连续不可达达此次数 → 强制重新关联 */
#define POL_UNREACH_RECONN_MAX 3  /* 一次"不可达事件"内最多重连 3 次：服务端整机不可达
                                   * （NAS 重启）时不该反复折腾 WiFi（本板 reason=2 与
                                   * 重连密度正相关）；探针恢复即复位 */
#define BACKOFF_UNREACH_MAX_MS 5000   /* 不可达分支退避封顶 5s（探针很轻，不必退到 60s） */
#define POLL_JITTER_MIN_MS 5000   /* -1 且耗时 ≥ 此值才算"真跑满 hold 的超时"（服务端 hold 55s） */
static bool     s_link_up = true;         /* 最近一次探针结论 */
static bool     s_link_was_down;          /* 探针曾判定不可达（恢复时补 hello 用） */
static bool     s_need_hello;             /* 回网后立即补 hello（掉线/强制重连后置位） */
static int      s_unreach_streak;
static int      s_reconn_tries;           /* 本轮"不可达事件"里已强制重连次数 */
static uint32_t s_seen_disconnects;       /* 已见过的 WiFi 断线次数（provision 侧计数） */
static int64_t  s_offline_since_ms;       /* 本设备进入"服务端不可达"的时刻（回网时算耗时） */

/* 【掉线自愈】provision.c 提供（声明与 poller.c:284 的 provision_portal_active 同法，
 * 不动公共头文件）：断线次数累计 + STA 是否已拿 IP */
extern uint32_t provision_wifi_disconnect_count(void);
extern bool     provision_wifi_is_connected(void);

/**
 * 1.5s 上限的裸 TCP 可达性探针（不建 HTTP、不发请求：只回答"到服务端能不能建连"）。
 * 三态返回（真机实测必须区分后两者 —— 见下）：
 *   PROBE_UP      建连成功
 *   PROBE_DOWN    建连失败/超时（真不可达：该重连）
 *   PROBE_NO_MEM  本地内存不足（socket()/内核缓冲分配失败 errno=105 ENOBUFS）
 * 【为什么区分】真机 `internal heap 空闲 2231 最大块 2036` 时，lwIP 直接
 * `thread_sem_init: out of memory` + `socket 失败 errno=105`，此时**换关联毫无用处**
 * （是内部 RAM 不够，不是 Wi-Fi 链路问题），盲目强制重连只会把认证风暴叠上去。
 * 只探点分 IP；非点分 IP（域名）直接算通，交给 http 层判断，避免误判。
 */
#define PROBE_UP      1
#define PROBE_DOWN    0
#define PROBE_NO_MEM (-1)

static int server_tcp_probe(uint32_t timeout_ms)
{
    const char *url = mp_http_server_url();
    if (!url || !url[0]) return PROBE_DOWN;

    const char *p = strstr(url, "//");
    p = p ? p + 2 : url;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    char host[64] = { 0 };
    size_t hl = (colon && (!slash || colon < slash)) ? (size_t)(colon - p)
              : (slash ? (size_t)(slash - p) : strlen(p));
    if (hl == 0 || hl >= sizeof(host)) return PROBE_UP;
    memcpy(host, p, hl);
    int port = (colon && (!slash || colon < slash)) ? atoi(colon + 1) : 80;

    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_aton(host, &sa.sin_addr) != 1) return PROBE_UP;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGW(TAG, "探针：socket() 失败 errno=%d (%s) → 本地内部堆/缓冲不足",
                 errno, strerror(errno));
        return PROBE_NO_MEM;                  /* 不是网络问题：别去动 WiFi */
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    int rc = PROBE_DOWN;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        rc = PROBE_UP;                        /* 立即成功（本机/回环路径） */
    } else if (errno == ENOMEM || errno == ENOBUFS) {
        rc = PROBE_NO_MEM;
    } else if (errno == EINPROGRESS || errno == EALREADY) {
        fd_set wf, ef;
        FD_ZERO(&wf); FD_ZERO(&ef);
        FD_SET(fd, &wf); FD_SET(fd, &ef);
        struct timeval tv = { .tv_sec = 0, .tv_usec = (suseconds_t)(timeout_ms * 1000) };
        int r = select(fd + 1, NULL, &wf, &ef, &tv);
        if (r > 0 && FD_ISSET(fd, &wf)) {
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) {
                rc = PROBE_UP;
            } else if (soerr == ENOMEM || soerr == ENOBUFS) {
                rc = PROBE_NO_MEM;
            }
        }
    }
    close(fd);
    return rc;
}

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
                /* 【内存不足时不折腾 WiFi · 2026-09-27 真机取证】内部堆只剩
                 * 2.2KB/最大块 2.0KB 时，hello 的失败原因是
                 * `lwip_arch: thread_sem_init: out of memory` + socket ENOBUFS
                 * —— 换关联完全无效，反复 force_reconnect 反而把 reason=2 认证
                 * 风暴叠上去（真机 320s 内 13 次 reason=2 断线、6 次强制重连）。
                 * 探针报 PROBE_NO_MEM → 只退避，等内存回收。 */
                if (server_tcp_probe(POLL_PROBE_MS) == PROBE_NO_MEM) {
                    ESP_LOGW(TAG, "hello 失败原因是本地内存不足（socket 分配失败）→ "
                                  "跳过强制重连，仅退避重试");
                    vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                    if (backoff_ms < BACKOFF_MAX_MS) backoff_ms *= 2;
                    continue;
                }
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
                s_offline_since_ms = mp_now_ms();
                state_machine_handle(MP_SM_EV_NET_OFFLINE);
            }
            continue;
        }

        /* 【掉线自愈 · 新增】WiFi 侧掉过线（provision 的断线计数自增）→ 回网立刻补 hello：
         * 服务端 lastSeen 马上刷新，不必等下一轮 55s 长轮询结束。
         * 计数只在 DISCONNECTED 事件里自增，天然限频（不会变成每轮都发）。 */
        {
            uint32_t dn = provision_wifi_disconnect_count();
            if (dn != s_seen_disconnects) {
                ESP_LOGW(TAG, "检测到 WiFi 断线（累计 %u 次）→ 回网立即补 hello",
                         (unsigned)dn);
                s_seen_disconnects = dn;
                s_need_hello = true;
            }
        }

        /* E9 常态化校时：借本任务执行（不新建 rtcsync 任务，绕开内部堆碎片） */
        provision_rtc_resync_step();

        /* E14 设备日志增量上报：同样借本任务（不新建任务 —— 内部堆约束）。
         * 内部自带 20s 心跳/失败退避，poll 长轮询期间不会叠加请求。 */
        mp_http_device_log_step();

        /* 【可达性门 · 新增】1.5s 裸 TCP 探针：分辨"真断线"与"长轮询抖动"。
         * 探不通就跳过本轮 poll（省下 65s 的建连黑洞），直接走重连/退避；
         * 探得通则说明链路在，上一轮 -1 只是长轮询抖动，保持 5s 快速重试。 */
        int probe = server_tcp_probe(POLL_PROBE_MS);
        if (probe != PROBE_UP) {
            int streak = ++s_unreach_streak;
            s_link_up = false;
            if (probe == PROBE_DOWN) s_link_was_down = true;
            ESP_LOGW(TAG, "服务端 TCP 探针=%s（连续第 %d 次，上限 %ums，STA 已连=%d）→ 跳过本轮 poll",
                     probe == PROBE_NO_MEM ? "本地内存不足" : "不可达",
                     streak, (unsigned)POLL_PROBE_MS, (int)provision_wifi_is_connected());
            if (probe == PROBE_NO_MEM) {
                /* 本地内部堆耗尽（真机实测：空闲 2.2KB/最大块 2.0KB 时 lwIP
                 * `thread_sem_init: out of memory` + socket errno=105）——
                 * 换关联解决不了它，只会把认证风暴叠上去；只退避等内存回收。 */
                ESP_LOGW(TAG, "本轮不强制重连（非链路问题，是本地内存不足）");
            } else if (streak >= POL_UNREACH_RECONN && s_reconn_tries < POL_UNREACH_RECONN_MAX) {
                s_reconn_tries++;
                ESP_LOGW(TAG, "连续不可达 → 第 %d 次强制重新关联（换关联/BSSID）",
                         s_reconn_tries);
                provision_wifi_force_reconnect();
                s_need_hello = true;
                vTaskDelay(pdMS_TO_TICKS(3000));   /* 给路由器留冷却（认证风暴保护） */
            }
            if (streak >= POL_UNREACH_RECONN && s_reported_online) {
                s_reported_online = false;
                s_offline_since_ms = mp_now_ms();
                state_machine_handle(MP_SM_EV_NET_OFFLINE);
                ESP_LOGW(TAG, "上报 NET_OFFLINE（服务端 TCP 不可达）");
            }
            /* 探针很轻（一次 SYN），退避封顶 5s：路由器刚恢复/服务端刚重启时
             * 要在 1 分钟内回来，不能退到 60s */
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms < BACKOFF_UNREACH_MAX_MS) ? backoff_ms * 2
                                                              : BACKOFF_UNREACH_MAX_MS;
            continue;
        }
        s_unreach_streak = 0;
        if (!s_link_up) {
            s_link_up = true;
            if (s_link_was_down) {
                s_link_was_down = false;
                s_reconn_tries = 0;
                s_need_hello = true;
                ESP_LOGW(TAG, "链路恢复（探针已通）→ 立即补 hello 刷新服务端 lastSeen");
            }
        }
        if (s_need_hello) {
            s_need_hello = false;
            if (mp_http_hello() == 0) {
                s_hello_fail_streak = 0;
                asset_dl_request_sync();
                ESP_LOGW(TAG, "回网补 hello 成功（掉线/重连后立即执行，不等 poll 轮次）");
            } else {
                ESP_LOGW(TAG, "回网补 hello 失败 → 继续走 poll（poll 成功同样刷新 lastSeen）");
            }
        }

        int64_t poll_t0 = mp_now_ms();
        bool ok = do_poll_once();
        int64_t poll_dt_ms = mp_now_ms() - poll_t0;

        /* 长轮询可能 hold 50s：再补一次日志上报机会（内部节流，不会连发） */
        mp_http_device_log_step();

        if (ok) {
            s_last_poll_status = 200;
            s_poll_fail_streak = 0;
            s_reconn_tries = 0;
            backoff_ms = BACKOFF_MIN_MS;          /* 成功复位退避 */
            if (!s_reported_online) {
                s_reported_online = true;
                int64_t gap_ms = s_offline_since_ms ? (mp_now_ms() - s_offline_since_ms) : 0;
                s_offline_since_ms = 0;
                state_machine_handle(MP_SM_EV_NET_ONLINE);
                ESP_LOGW(TAG, "poll 成功 → 回到 online（离线时长 %lld ms）", (long long)gap_ms);
            }
        } else {
            /* 本轮 poll 前探针是通过的 → -1 只可能是长轮询读超时/响应丢失（设备仍在线）。
             * 置 s_link_up=false 让下一轮先探针复检：若 hold 期间掉了线，下一轮就会
             * 走"不可达"分支快速重连，而不是再等一个 65s。 */
            s_link_up = false;
            /* 【-1 的第二种含义 2026-09-27】服务端 hold 最长 55s，真"长轮询超时"必然
             * 跑满 poll_dt≈65s；若 -1 却在几秒内返回，那是 open/写阶段就失败
             * （真机形态：lwIP ENOBUFS、连接被拒、socket 分配失败），属于真失败 ——
             * 旧实现一律当抖动 5s 重试，于是"内存不够导致的连不上"被无限空转，
             * 既不退避也不重新 hello、更不报离线。现按耗时区分。 */
            if (s_last_poll_status == -1 && poll_dt_ms >= POLL_JITTER_MIN_MS) {
                ESP_LOGW(TAG, "poll 超时（%lld ms，跑满 hold）→ 判为心跳抖动，5s 快速重试",
                         (long long)poll_dt_ms);
                vTaskDelay(pdMS_TO_TICKS(POLL_TIMEOUT_RETRY_MS));
                backoff_ms = BACKOFF_MIN_MS;      /* 超时不累积退避 */
                continue;                          /* 仍在线：不报 NET_OFFLINE */
            }
            if (s_last_poll_status == -1) {
                ESP_LOGW(TAG, "poll 快速失败（%lld ms < %d ms）→ 判为真失败，走退避/补 hello",
                         (long long)poll_dt_ms, POLL_JITTER_MIN_MS);
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
                s_offline_since_ms = mp_now_ms();
                state_machine_handle(MP_SM_EV_NET_OFFLINE);
                ESP_LOGW(TAG, "上报 NET_OFFLINE（poll 真失败 %d）", s_last_poll_status);
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
