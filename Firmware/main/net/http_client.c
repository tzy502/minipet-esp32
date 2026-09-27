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
#include "lwip/sockets.h"
#include <errno.h>
#include <stdlib.h>

#include "app_core.h"
#include "hal_contract.h"
#include "logbuf.h"
#include "mdns_discover.h"

static const char *TAG = "http";

#define URL_BUF_LEN   256
#define READ_CHUNK    2048

/* E14 日志端点 */
#define LOG_PATH          "/api/device/log"
#define LOG_POST_TIMEOUT  4000        /* 短超时：绝不拖慢长轮询 */
#define LOG_BODY_CAP      4608
#define LOG_RESP_CAP      256
#define LOG_HEX_MAX       512         /* 单次上报的 msg hex 上限（与 logbuf msg 上限对齐） */

static char s_server_url[128];      /* 无结尾斜杠 */
static char s_uuid[13];             /* 12 hex + NUL */
static char s_device_id[40];        /* hello 返回；未注册时 = s_uuid */
static volatile bool s_hello_done;
static char s_pairing_code[8];      /* hello 下发的 6 位配对码（暂存，字体绑定后重显） */

/* 设备日志上报游标（已成功送达服务端的最大序号） */
static uint32_t s_log_sent_seq;
static uint32_t s_log_last_try_ms;
static uint32_t s_log_fail_streak;

/* ------------------------------------------------------------------ */
/* 初始化                                                               */
/* ------------------------------------------------------------------ */
/* 归一化：去尾斜杠 + scheme 兜底（真机两坑）：
 *   ①省略 scheme → 补 http://；②手机浏览器自动升级 https:// → NAS 服务端为
 *   纯 HTTP，强制回 http（否则 TLS 握手被重置 → abort 重启循环）。 */
static void url_apply(char *dst, size_t cap, const char *raw)
{
    while (*raw == ' ') raw++;
    strlcpy(dst, raw, cap);
    size_t len = strlen(dst);
    while (len > 0 && dst[len - 1] == '/') dst[--len] = 0;
    if (dst[0] == 0) return;
    char tmp[128];
    if (strncmp(dst, "http://", 7) == 0) {
        /* 已是 http，保持 */
    } else if (strncmp(dst, "https://", 8) == 0) {
        snprintf(tmp, sizeof(tmp), "http://%s", dst + 8);
        strlcpy(dst, tmp, cap);
    } else {
        snprintf(tmp, sizeof(tmp), "http://%s", dst);
        strlcpy(dst, tmp, cap);
    }
}

void mp_http_init(void)
{
    if (s_uuid[0] == 0) {
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_uuid, sizeof(s_uuid), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        strlcpy(s_device_id, s_uuid, sizeof(s_device_id));   /* 匿名兜底（E13） */
    }
    char raw[128] = { 0 };
    if (!mp_nvs_get_str("srv_url", raw, sizeof(raw)) || raw[0] == 0) {
        s_server_url[0] = 0;
        return;
    }
    url_apply(s_server_url, sizeof(s_server_url), raw);
}

const char *mp_http_server_url(void) { return s_server_url[0] ? s_server_url : NULL; }
const char *mp_http_uuid(void)       { return s_uuid; }
const char *mp_http_device_id(void)  { return s_device_id; }
bool mp_http_hello_done(void) { return s_hello_done; }
const char *mp_http_pairing_code(void)    { return s_pairing_code; }

/* 覆盖本次运行的服务器地址（E14 mDNS 兜底用；**不写 NVS**，见 mdns_discover.h）。
 * url 为 NULL 或空串 = 清空（地址不可达时清掉，避免对已知坏地址反复发请求）。 */
void mp_http_set_server_url(const char *url)
{
    if (!url || url[0] == 0) {
        s_server_url[0] = 0;
        s_hello_done = false;      /* 地址换了：hello 需要重新做 */
        return;
    }
    url_apply(s_server_url, sizeof(s_server_url), url);
    s_hello_done = false;
}

/* ------------------------------------------------------------------ */
/* 传输失败限频诊断（问题8 卡点）                                         */
/* ------------------------------------------------------------------ */
/* 一次失败一行读全：esp_err + errno + 完整 URL + 失败阶段。errno 用
 * IDF 5.5 公开 API esp_http_client_get_errno（转发 esp_transport_get_errno，
 * 传输失败时存有 lwip errno，可区分 ECONNRESET/ETIMEDOUT/EHOSTUNREACH；
 * 须在 close 拆传输前取，errno=0 说明对端无错关闭/无上下文）。
 * 限频：同 key（phase+ret+errno 组合）5s 一条——hello/poll 长轮询失败
 * 风暴期间不刷屏。 */
static void raw_tcp_probe_once(const char *url);

static void tx_fail_log(const char *url, const char *path,
                        const char *phase, esp_err_t ret, int eno)
{
    static uint32_t last_ms;                 /* 同 key 上次输出时刻（开机 ms） */
    static int      last_key;
    static bool     key_valid;
    int key = (int)((uint32_t)ret ^ ((uint32_t)eno << 8) ^ ((uint32_t)phase[0] << 16));
    uint32_t now = esp_log_timestamp();
    if (key_valid && key == last_key && (uint32_t)(now - last_ms) < 5000) return;
    last_key  = key;
    last_ms   = now;
    key_valid = true;
    ESP_LOGW(TAG, "mp_http_tx_fail ret=0x%x (%s) errno=%d (%s) url=%s path=%s phase=%s",
             (unsigned)ret, esp_err_to_name(ret), eno, strerror(eno), url, path, phase);
    if (phase[0] == 'o' && strcmp(phase, "open") == 0) raw_tcp_probe_once(url);
}

/* 【网络取证】绕过 esp_http_client 的裸 socket 三步探针（每次开机至多一次，
 * 首次 open 失败触发）：区分「网络层按源掐连接」（raw 同样失败）vs
 * 「esp_http_client 层问题」（raw 成功）。Mac curl 实测同请求 200——
 * 服务端健康，差异必在板子到服务端的路径上。 */
static void raw_tcp_probe_once(const char *url)
{
    static bool done;
    if (done) return;
    done = true;

    /* 【真机取证 2026-09-27】设备关联+DHCP+ICMP 全通，但 TCP 一律打不开
     * （ESP_ERR_HTTP_CONNECT / 8s 超时），而 Mac 对同一 URL curl 得 200。
     * 这里先把两个"一定在线"的目标各连一次（网关 + 服务端），把 errno 打出来：
     * 若连网关也失败 → 设备侧 socket/TCP 分配问题（内部堆最大连续块仅 2KB）；
     * 若网关通、服务端不通 → 路径/防火墙按源拦截。 */
    {
        /* 【只测"确认在监听"的目标】上轮把网关 80 也当探针目标，但路由器多半
         * 不监听 80 → 返回 RST 被 lwIP 记成 errno=113(ECONNABORTED)，据此得出
         * "设备 TCP 坏了"是错误结论。现在只探服务端 38090（Mac 侧 nc/curl 均通），
         * 并保留 3 次重试，结果才有判据价值。 */
        static const struct { const char *ip; int port; const char *name; } tgts[] = {
            { "<NAS_IP>",   38090, "服务端" },
        };
        for (size_t i = 0; i < sizeof(tgts) / sizeof(tgts[0]); i++) {
            int s2 = socket(AF_INET, SOCK_STREAM, 0);
            if (s2 < 0) {
                ESP_LOGE("probe", "[%s] socket 失败 errno=%d (%s) → socket 池/内部堆不足",
                         tgts[i].name, errno, strerror(errno));
                continue;
            }
            struct sockaddr_in a2 = { 0 };
            a2.sin_family = AF_INET;
            a2.sin_port = htons((uint16_t)tgts[i].port);
            inet_aton(tgts[i].ip, &a2.sin_addr);
            int rc = -1, last_errno = 0;
            /* 连两次（间隔 2s）：区分"首次包丢/ARP 未就绪"与"稳定不通" */
            for (int attempt = 0; attempt < 3; attempt++) {
                rc = connect(s2, (struct sockaddr *)&a2, sizeof a2);
                last_errno = (rc == 0) ? 0 : errno;
                if (rc == 0) break;
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            ESP_LOGW("probe", "[%s] %s:%d connect=%d errno=%d (%s)",
                     tgts[i].name, tgts[i].ip, tgts[i].port, rc,
                     last_errno, rc == 0 ? "OK" : strerror(last_errno));
            close(s2);
        }
    }

    const char *p = strstr(url, "//");
    if (!p) return;
    p += 2;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    char host[64] = { 0 };
    int port = 80;
    size_t hl = (colon && (!slash || colon < slash)) ? (size_t)(colon - p)
              : (slash ? (size_t)(slash - p) : strlen(p));
    if (hl == 0 || hl >= sizeof host) return;
    memcpy(host, p, hl);
    if (colon && (!slash || colon < slash)) port = atoi(colon + 1);

    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_aton(host, &sa.sin_addr) != 1) {
        ESP_LOGW("probe", "RAW 主机非点分 IP（%s），跳过", host);
        return;
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { ESP_LOGW("probe", "RAW socket 失败 errno=%d", errno); return; }
    if (connect(sock, (struct sockaddr *)&sa, sizeof sa) != 0) {
        ESP_LOGW("probe", "RAW connect %s:%d 失败 errno=%d (%s) → 网络层按源拦截",
                 host, port, errno, strerror(errno));
        close(sock);
        return;
    }
    ESP_LOGW("probe", "RAW connect %s:%d 成功（TCP 握手通）", host, port);
    char req[128];
    int n = snprintf(req, sizeof req, "GET / HTTP/1.0\r\nHost: %s:%d\r\n\r\n", host, port);
    int w = send(sock, req, n, 0);
    ESP_LOGW("probe", "RAW send=%d errno=%d", w, w < 0 ? errno : 0);
    char buf[96];
    int r = recv(sock, buf, sizeof buf - 1, 0);
    if (r > 0) {
        buf[r] = 0;
        for (int i = 0; i < r; i++) if (buf[i] == '\r' || buf[i] == '\n') { buf[i] = 0; break; }
        ESP_LOGW("probe", "RAW recv=%d 首行: %s → 服务端正常回包，问题在 esp_http_client 层", r, buf);
    } else {
        ESP_LOGW("probe", "RAW recv=%d errno=%d (%s) → 握手后被按源 RST，查 NAS 防火墙",
                 r, errno, strerror(errno));
    }
    close(sock);
}

/* ------------------------------------------------------------------ */
/* 通用事务：GET / POST，流式回调                                        */
/* ------------------------------------------------------------------ */
static int http_txn(const char *url, const char *path, bool is_post, const char *body,
                    mp_http_chunk_cb cb, void *ctx,
                    char *resp_buf, size_t resp_cap, uint32_t timeout_ms)
{
    if (!url || url[0] == 0) return -1;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = 2048,
        /* 【socket 耗尽修复 2026-09-27】POST 关闭 keep-alive：
         * 真机实证事件上报突发时 `Failed to create socket errno=105
         * (No buffer space available)` + `wifi:m f null` 数百条 → 网络栈瘫痪、
         * 输入/菜单连带卡死。事件/心跳这类低频请求保持长连接只会积压 socket，
         * 每次走短连接更稳。GET（长轮询/素材流）仍保留 keep-alive 复用。 */
        .keep_alive_enable = !is_post,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        tx_fail_log(url, path, "init", ESP_FAIL, 0);   /* 无传输上下文，errno 记 0 */
        return -1;
    }

    int status = -1;
    char *chunk = malloc(READ_CHUNK);
    if (!chunk) goto out;

    if (is_post) {
        esp_http_client_set_method(h, HTTP_METHOD_POST);
        esp_http_client_set_header(h, "Content-Type", "application/json");
    }

    int body_len = (body ? (int)strlen(body) : 0);
    esp_err_t open_err = esp_http_client_open(h, body_len);
    if (open_err != ESP_OK) {
        /* open = 建连 + 发请求头阶段：ECONNRESET/EHOSTUNREACH 等在这里现形 */
        tx_fail_log(url, path, "open", open_err, esp_http_client_get_errno(h));
        goto out;
    }
    if (body_len > 0) {
        int w = esp_http_client_write(h, body, body_len);
        if (w < 0) {
            tx_fail_log(url, path, "write", ESP_FAIL, esp_http_client_get_errno(h));
            goto out_close;
        }
    }

    if (esp_http_client_fetch_headers(h) < 0) {
        tx_fail_log(url, path, "fetch_headers", ESP_FAIL, esp_http_client_get_errno(h));
        goto out_close;
    }
    status = esp_http_client_get_status_code(h);

    /* 流式读：优先给回调；小响应用兜底缓冲（hello/poll 的 JSON） */
    size_t resp_len = 0;
    for (;;) {
        int r = esp_http_client_read(h, chunk, READ_CHUNK);
        if (r < 0) {
            /* -1=读超时/对端中途掐断（0=正常 EOF，不算失败） */
            tx_fail_log(url, path, "read", ESP_FAIL, esp_http_client_get_errno(h));
        }
        if (r <= 0) break;
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
        /* 配网页用户常省略 scheme（如 <NAS_IP>:38090）→ 补 http://（真机定稿） */
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
    return http_txn(url, path_or_url, false, NULL, cb, ctx, NULL, 0, timeout_ms);
}

int mp_http_post_json(const char *path, const char *json_body,
                      char *resp_buf, size_t resp_cap, uint32_t timeout_ms)
{
    char url[URL_BUF_LEN];
    if (build_url(url, sizeof(url), path) != 0) return -1;
    return http_txn(url, path, true, json_body, NULL, NULL, resp_buf, resp_cap, timeout_ms);
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
    /* 【真机连不通取证 2026-09-27】把"设备到底在连哪个 URL、用什么 deviceId"
     * 打成事实：若与 Mac 侧 curl 的地址不一致（NVS 旧值），一切"连不通"都由此解释。 */
    mp_http_init();
    ESP_LOGW(TAG, "hello 目标 http=%s deviceId=%s",
             mp_http_server_url() ? mp_http_server_url() : "(未配置)",
             mp_http_device_id());
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
    /* 【超时收紧 2026-09-27】8s → 4s：hello 是局域网内的小 POST（Mac curl 同 URL
     * 毫秒级返回）。真机失败形态是 `ESP_ERR_HTTP_CONNECT phase=open` 一直挂到
     * 超时（AP 半死/SYN 黑洞），8s 只是把"卡住"拉长一倍。缩短后失败更快暴露，
     * 由 poller 的退避/重连逻辑接手（每轮代价从 8s 降到 4s）。 */
    int status = mp_http_post_json("/api/device/hello", body, resp, sizeof(resp), 4000);
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
        /* E4 IMU 灵敏度（0.2–3.0 由服务端夹取；此处再兜底一次防手改配置发散） */
        if (json_num2(cfg, "imuSensitivity", NULL, &d)) {
            if (d < 0.2) d = 0.2;
            if (d > 3.0) d = 3.0;
            g_mp_cfg.imu_sensitivity = (float)d;
        }
    }

    /* E13：首配对码（服务端入册后返回，已绑定则无此字段）。
     * 真机根因修复：服务端实际下发字段是 pairingCode（DeviceEndpoints.cs
     * hello handler），旧实现只读 pair → 配对码永远不显示。pairingCode
     * 优先，pair 保留兼容旧服务端 */
    const char *pair = cJSON_GetStringValue(cJSON_GetObjectItem(r, "pairingCode"));
    if (!pair || !pair[0])
        pair = cJSON_GetStringValue(cJSON_GetObjectItem(r, "pair"));
    if (pair && pair[0]) {
        strlcpy(s_pairing_code, pair, sizeof(s_pairing_code));   /* 暂存：dispatch_manifest_synced 字体就绪后重显 */
        mp_cmd_t c = { .type = MP_CMD_PAIRING_CODE };
        strlcpy(c.s, pair, sizeof(c.s));
        mp_post_cmd(&c);
    }

    cJSON_Delete(r);
    s_hello_done = true;
    ESP_LOGI(TAG, "hello ok, deviceId=%s", s_device_id);
    return 0;
}

/* ------------------------------------------------------------------ */
/* POST /api/device/log（E14 设备环形日志上报）                          */
/* ------------------------------------------------------------------ */
/* 「Web 可拉取」的通路选择（三选一，见任务书）：
 *   a) poll 响应捎带 —— 不合适：poll 是拉指令的下行通道，日志是上行数据；
 *   b) 设备侧开 HTTP 服务端 —— 不可行：设备是纯 client，唯一 server 是配网 portal；
 *   c) 【本实现】设备定期 POST 到服务端 /api/device/log，服务端存每设备环缓，
 *      Web 从服务端拉 —— 与既有 hello/poll/event 同构，零新增任务/端口。
 *
 * 增量语义：body 带 since（上次成功送达的最大序号），服务端按 seq 去重/排序即可；
 * 只有成功（HTTP 200）才推进游标，失败下轮重传（不丢日志）。
 *
 * 【2026-09-27 改】body/hex 两个缓冲原为 .bss（内部 DRAM 常驻 5.6KB）。真机实测
 * 该常驻量直接导致"IP 拿到了但 hello 发不出去"：启动末期内部堆只剩 ~7KB/最大块
 * 3KB，HTTP 客户端建连所需的工作内存拿不到。现改为 PSRAM 懒分配（首次上报时
 * 一次分配、失败则本次跳过下轮再试），把内部堆完整让给网络栈。 */

/* 单条日志的 msg 走 hex（json_escape 的替代：零堆分配、零栈大数组；
 * 不可打印字节/引号/换行都不需要再转义；服务端 hex→UTF-8 即可）。
 * 唯一读者是 poller 任务（单线程），故 scratch 用文件级 static 复用。 */
static char *s_log_hex;      /* PSRAM 懒分配（LOG_HEX_MAX*2+1） */

static bool log_scratch_alloc(void)
{
    if (!s_log_hex) {
        s_log_hex = heap_caps_malloc(LOG_HEX_MAX * 2 + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_log_hex != NULL;
}

static void log_msg_to_hex(const char *msg)
{
    static const char HEX[] = "0123456789abcdef";
    if (!s_log_hex) return;
    size_t slen = strlen(msg);
    size_t sn = slen > LOG_HEX_MAX ? LOG_HEX_MAX : slen;      /* 超长截断（保头） */
    size_t hl = 0;
    for (size_t i = 0; i < sn; i++) {
        uint8_t b = (uint8_t)msg[i];
        s_log_hex[hl++] = HEX[b >> 4];
        s_log_hex[hl++] = HEX[b & 0x0F];
    }
    s_log_hex[hl] = 0;
}

/**
 * 增量上报一步（由已在跑的 poller 任务周期调用 —— 不新建任务）。
 *  - 仅在 hello 成功（网络/deviceId 就绪）且缓冲可用时动作
 *  - 心跳间隔 20s；出现 E 级日志或从未上报过 → 立即上报
 *  - 失败退避（连续失败 60s 一轮），绝不拖慢 poll（单次 4s 超时上限）
 */
void mp_http_device_log_step(void)
{
    if (!logbuf_ready() || !s_hello_done) return;
    if (!s_server_url[0]) return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool urgent = logbuf_take_err_flag() || s_log_sent_seq == 0;
    uint32_t period = urgent ? 0 : (s_log_fail_streak ? 60000u : 20000u);
    if (!urgent && (uint32_t)(now_ms - s_log_last_try_ms) < period) return;

    uint32_t seq_now = logbuf_seq();
    if (seq_now <= s_log_sent_seq) return;               /* 无新日志 */

    s_log_last_try_ms = now_ms;

    static char *body;
    if (!body) body = heap_caps_malloc(LOG_BODY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body || !log_scratch_alloc()) return;      /* PSRAM 不足：本轮跳过，下轮再试 */
    int off = snprintf(body, LOG_BODY_CAP,
                       "{\"proto\":%d,\"deviceId\":\"%s\",\"since\":%lu,\"logs\":[",
                       MP_PROTO_VER, s_device_id, (unsigned long)s_log_sent_seq);

    logbuf_iter_t it;
    uint32_t last_seq = s_log_sent_seq;
    int n = 0;
    if (logbuf_scan_start(&it, s_log_sent_seq)) {
        logbuf_rec_t rec;
        while (logbuf_scan_next(&it, &rec)) {
            if (off <= 0 || (size_t)off >= (size_t)LOG_BODY_CAP - 640) break;   /* 留余量给尾部 */
            log_msg_to_hex(rec.msg);
            char one[LOG_HEX_MAX * 2 + 160];
            int w = snprintf(one, sizeof(one),
                             "{\"seq\":%lu,\"ts\":%llu,\"t\":%lu,\"lvl\":\"%c\","
                             "\"tag\":\"%s\",\"msgHex\":\"%s\"}",
                             (unsigned long)rec.seq, (unsigned long long)rec.ts_ms,
                             (unsigned long)rec.t_ms, rec.lvl, rec.tag, s_log_hex);
            if (w <= 0 || (size_t)w >= sizeof(one)) continue;   /* 不该发生：跳过 */
            if (off + w + 3 >= LOG_BODY_CAP) break;
            if (n) body[off++] = ',';
            memcpy(body + off, one, (size_t)w);
            off += w;
            last_seq = rec.seq;
            n++;
        }
    }
    logbuf_scan_end(&it);

    if (n == 0) return;                                  /* 一条都塞不进：等缓冲腾挪 */

    off += snprintf(body + off, (size_t)LOG_BODY_CAP - (size_t)off, "],\"count\":%d}", n);

    char resp[LOG_RESP_CAP];
    int status = mp_http_post_json(LOG_PATH, body, resp, sizeof(resp), LOG_POST_TIMEOUT);
    if (status == 200) {
        s_log_sent_seq = last_seq;
        s_log_fail_streak = 0;
        ESP_LOGD(TAG, "device log uploaded: %d lines, since→%lu",
                 n, (unsigned long)last_seq);
    } else {
        if (s_log_fail_streak < 100) s_log_fail_streak++;
        ESP_LOGD(TAG, "device log post failed: %d（下轮重传，n=%d）", status, n);
    }
}
