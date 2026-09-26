/**
 * provision.c — SoftAP captive portal 配网（E14）+ STA 连接管理
 *
 * captive portal 三件套：
 *   - DNS 劫持：UDP:53 全部 A 查询 → 192.168.4.1（302 探测必落到本机）
 *   - HTTP：esp_http_server 通配路由；非 "/" 的 GET（generate_204 /
 *     hotspot-detect.html / connecttest.txt 等）一律 302 → http://192.168.4.1/
 *   - 页面：SSID / 密码 / 服务器地址（占位 http://<NAS_IP>:38090）
 *
 * 保存后：portal 任务拆栈 → STA 连接（15s）→ SNTP 校时（15s）→ rtc_set_time
 * → esp_restart()。任一步失败 → 重开 portal（用户重试）。
 */
#include "provision.h"

#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_log.h"

#include "app_core.h"
#include "hal_contract.h"

static const char *TAG = "provision";

#define PORTAL_TASK_STACK  6144

static bool           s_wifi_inited;       /* esp_wifi_init 只做一次 */
static bool           s_portal_active;
static httpd_handle_t s_httpd;
static TaskHandle_t   s_dns_task;
static TaskHandle_t   s_portal_task;
static EventGroupHandle_t s_wifi_events;   /* IDF5：句柄类型是 EventGroupHandle_t */
#define WIFI_GOT_IP_BIT   BIT0
#define WIFI_FAIL_BIT     BIT1

/* ================================================================== */
/* 配置页（内嵌；UTF-8；移动端可用的最小样式）                           */
/* ================================================================== */
static const char PAGE_PORTAL[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>MiniPet 配网</title><style>"
"body{font-family:sans-serif;background:#101014;color:#e8e8ec;"
"max-width:420px;margin:48px auto;padding:0 18px}"
"h2{font-weight:600}label{display:block;margin-top:18px;color:#9aa}"
"input{width:100%;box-sizing:border-box;margin:6px 0 4px;padding:12px;"
"font-size:16px;border-radius:8px;border:1px solid #555;background:#1c1c22;color:#eee}"
"button{width:100%;margin-top:22px;padding:14px;font-size:17px;border:0;"
"border-radius:8px;background:#e0486a;color:#fff}"
"small{color:#778}</style></head><body>"
"<h2>MiniPet 配网</h2>"
"<form method=\"POST\" action=\"/save\">"
"<label>WiFi 名称（SSID）</label>"
"<input name=\"ssid\" maxlength=\"32\" required>"
"<label>WiFi 密码</label>"
"<input name=\"pass\" type=\"password\" maxlength=\"64\">"
"<label>服务器地址（NAS 上的 MiniPet 服务）</label>"
"<input name=\"server\" value=\"http://192.168.1.100:38090\" "
"placeholder=\"http://<NAS_IP>:38090\">"
"<small>默认端口 38090（部署层 .env MINIPET_PORT 可改）</small>"
"<button type=\"submit\">保存并连接</button></form>"
"<p><small>保存后设备会连 WiFi、自动校时并重启。约 30 秒。</small></p>"
"</body></html>";

static const char PAGE_OK[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"></head>"
"<body style=\"font-family:sans-serif;background:#101014;color:#e8e8ec;"
"text-align:center;padding-top:20%\"><h2>已保存</h2>"
"<p>设备正在连接 WiFi 并校时，随后自动重启。</p></body></html>";

/* application/x-www-form-urlencoded 解码（+ → 空格，%XX） */
static int url_decode(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (*p == '+') { *o++ = ' '; }
        else if (*p == '%' && p[1] && p[2]) {
            int hi, lo;
            hi = (p[1] >= '0' && p[1] <= '9') ? p[1] - '0' :
                 (p[1] >= 'a' && p[1] <= 'f') ? p[1] - 'a' + 10 :
                 (p[1] >= 'A' && p[1] <= 'F') ? p[1] - 'A' + 10 : -1;
            lo = (p[2] >= '0' && p[2] <= '9') ? p[2] - '0' :
                 (p[2] >= 'a' && p[2] <= 'f') ? p[2] - 'a' + 10 :
                 (p[2] >= 'A' && p[2] <= 'F') ? p[2] - 'A' + 10 : -1;
            if (hi < 0 || lo < 0) return -1;
            *o++ = (char)((hi << 4) | lo);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
    return 0;
}

/* 从 "a=1&b=2" 提取 key 的值到 out（cap 含 NUL） */
static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (!eq || (amp && amp < eq)) { p = amp ? amp + 1 : NULL; continue; }
        size_t name_len = (size_t)(eq - p);
        if (name_len == klen && memcmp(p, key, klen) == 0) {
            size_t vlen = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, eq + 1, vlen);
            out[vlen] = 0;
            return url_decode(out) == 0;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

/* ================================================================== */
/* DNS 劫持（UDP:53 → 本机 AP IP）                                      */
/* ================================================================== */
static void dns_hijack_task(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    while (s_portal_active) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(fd, buf, sizeof(buf) - 16, 0,
                         (struct sockaddr *)&src, &slen);
        if (n < 12) continue;                       /* 比最小 DNS 头还小 */

        uint16_t flags = 0x8180;                    /* QR|AA|RD|RA */
        uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
        if (qd == 0) continue;

        int qend = 12;
        while (qend < n && buf[qend] != 0) qend += buf[qend] + 1;   /* QNAME */
        qend += 5;                                  /* NUL + QTYPE + QCLASS */
        if (qend > n) continue;

        uint8_t *r = buf + qend;
        /* Answer: 指针压缩 NAME + A + IN + TTL + 4B AP 地址 */
        static const uint8_t answer[] = {
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x3C,                 /* TTL 60s */
            0x00, 0x04, 192, 168, 4, 1,
        };
        memcpy(r, answer, sizeof(answer));
        int rlen = qend + (int)sizeof(answer);

        buf[2] = (uint8_t)(flags >> 8);             /* flags */
        buf[3] = (uint8_t)(flags & 0xFF);
        buf[6] = 0; buf[7] = 1;                     /* ANCOUNT=1 */
        buf[8] = 0; buf[9] = 0;                     /* NSCOUNT */
        buf[10] = 0; buf[11] = 0;                   /* ARCOUNT */

        sendto(fd, buf, rlen, 0, (struct sockaddr *)&src, slen);
    }

    close(fd);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* ================================================================== */
/* HTTP（captive portal 探测 + 配网页 + 保存）                           */
/* ================================================================== */
static esp_err_t portal_get_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (strcmp(uri, "/") != 0 && strcmp(uri, "/index.html") != 0) {
        /* Android/Apple/Windows 的联网探测路径：302 到首页 */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE_PORTAL, sizeof(PAGE_PORTAL) - 1);
}

static esp_err_t portal_post_save_handler(httpd_req_t *req)
{
    char body[512] = { 0 };
    int total = req->content_len > 0 ? req->content_len : 0;
    if (total <= 0 || (size_t)total >= sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, NULL, 0);
    }
    int recvd = httpd_req_recv(req, body, total);
    if (recvd <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, NULL, 0);
    }
    body[recvd] = 0;

    char ssid[33] = { 0 }, pass[65] = { 0 }, server[128] = { 0 };
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "pass", pass, sizeof(pass));
    form_get(body, "server", server, sizeof(server));

    /* 基本校验：SSID 必填；server 缺省回落占位端口 */
    if (ssid[0] == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, NULL, 0);
    }
    if (server[0] == 0) {
        strlcpy(server, "http://192.168.1.100:38090", sizeof(server));
    }
    /* 去掉结尾斜杠（http_client 拼接约定） */
    size_t sl = strlen(server);
    while (sl > 0 && server[sl - 1] == '/') server[--sl] = 0;

    mp_nvs_set_str("wifi_ssid", ssid);
    mp_nvs_set_str("wifi_pass", pass);
    mp_nvs_set_str("srv_url", server);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PAGE_OK, sizeof(PAGE_OK) - 1);

    ESP_LOGI(TAG, "provision saved: ssid=%s server=%s", ssid, server);

    /* 延迟拆栈（等本响应发出），由 portal 任务接管后续流程 */
    if (s_portal_task) xTaskNotifyGive(s_portal_task);
    return ESP_OK;
}

static void start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;    /* 通配捕获所有探测路径 */
    cfg.max_uri_handlers = 4;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) return;

    httpd_uri_t get_any = {
        .uri = "/*", .method = HTTP_GET, .handler = portal_get_handler, .user_ctx = NULL };
    httpd_uri_t post_save = {
        .uri = "/save", .method = HTTP_POST, .handler = portal_post_save_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_httpd, &get_any);
    httpd_register_uri_handler(s_httpd, &post_save);
}

static void stop_httpd(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

/* ================================================================== */
/* WiFi 底座（全固件唯一 esp_wifi 入口）                                 */
/* ================================================================== */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_GOT_IP_BIT);
    }
}

static void wifi_init_once(void)
{
    if (s_wifi_inited) return;

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    s_wifi_events = xEventGroupCreate();
    s_wifi_inited = true;
}

static void wifi_start_ap(const char *ssid)
{
    wifi_init_once();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    ap.ap.channel = 6;
    ap.ap.authmode = WIFI_AUTH_OPEN;              /* 开放网络：手机连上即弹页 */
    ap.ap.max_connection = 2;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ================================================================== */
/* NTP 校时 → RTC（E9：配网时校准一次，断网独立走时）                    */
/* ================================================================== */
static bool sntp_and_set_rtc(void)
{
    /* IDF 5.5 API：esp_sntp_config_t + DEFAULT_CONFIG 宏；服务器数组
     * 大小 = CONFIG_LWIP_SNTP_MAX_SERVERS（当前 1），只放主用 ntp.aliyun.com */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) return false;
    bool ok = (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK);
    esp_netif_sntp_deinit();
    if (!ok) return false;

    time_t now = 0;
    struct tm tm_now = { 0 };
    time(&now);
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year < 120) return false;       /* 时钟显然未同步 */

    setenv("TZ", "CST-8", 1);                     /* 东八区 */
    tzset();
    return mp_rtc_set_time(&tm_now);              /* PCF85063（hal_contract 适配） */
}

/* ================================================================== */
/* portal 主任务：起栈 → 等 /save 通知 → 拆栈 → STA+NTP+RTC → 重启       */
/* ================================================================== */
static void portal_task(void *arg)
{
    (void)arg;

    /* SSID = MiniPet-<MAC 后 4 hex>（横幅提示共用 provision_get_ap_ssid） */
    char ssid[16];
    provision_get_ap_ssid(ssid, sizeof(ssid));

    wifi_start_ap(ssid);
    s_portal_active = true;
    xTaskCreate(dns_hijack_task, "dns53", 3072, NULL, 4, &s_dns_task);
    start_httpd();

    ESP_LOGI(TAG, "portal up: ssid=%s ip=192.168.4.1", ssid);

    /* 等保存（长等待；状态机 provision_stop 会删除本任务） */
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3600 * 1000)) == 0) {
        /* 1 小时无人配网：留在 portal 继续等（重启循环无意义） */
        for (;;) {
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3600 * 1000)) > 0) break;
        }
    }

    /* —— 用户已提交配置：拆 portal —— */
    s_portal_active = false;                      /* DNS 循环退出 */
    stop_httpd();
    vTaskDelay(pdMS_TO_TICKS(300));               /* 等 DNS 任务自删 */
    ESP_ERROR_CHECK(esp_wifi_stop());
    vTaskDelay(pdMS_TO_TICKS(200));

    /* —— STA 连接 —— */
    char ssid_sta[33] = { 0 }, pass_sta[65] = { 0 };
    mp_nvs_get_str("wifi_ssid", ssid_sta, sizeof(ssid_sta));
    mp_nvs_get_str("wifi_pass", pass_sta, sizeof(pass_sta));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid_sta, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass_sta, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    xEventGroupClearBits(s_wifi_events, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & WIFI_GOT_IP_BIT) {
        sntp_and_set_rtc();                       /* 校时失败不阻断（下次补） */
        ESP_LOGI(TAG, "provision done, rebooting into normal boot");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    /* STA 失败：重开 portal（回到开头逻辑，简化为重启 portal 任务） */
    ESP_LOGI(TAG, "STA connect failed, reopening portal");
    s_portal_task = NULL;
    provision_start_portal();
    vTaskDelete(NULL);
}

/* ================================================================== */
/* 公开 API                                                             */
/* ================================================================== */
void provision_get_ap_ssid(char *out, size_t cap)
{
    /* SSID = MiniPet-<SOFTAP MAC 后 4 hex>（portal 与屏显横幅共用） */
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, cap, "MiniPet-%02X%02X", mac[4], mac[5]);
}

bool provision_has_config(void)
{
    char ssid[33], server[128];
    return mp_nvs_get_str("wifi_ssid", ssid, sizeof(ssid)) && ssid[0] != 0 &&
           mp_nvs_get_str("srv_url", server, sizeof(server)) && server[0] != 0;
}

esp_err_t provision_wifi_connect_sta(uint32_t timeout_ms)
{
    char ssid[33] = { 0 }, pass[65] = { 0 };
    if (!mp_nvs_get_str("wifi_ssid", ssid, sizeof(ssid)) || ssid[0] == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    mp_nvs_get_str("wifi_pass", pass, sizeof(pass));

    wifi_init_once();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    xEventGroupClearBits(s_wifi_events, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_GOT_IP_BIT) return ESP_OK;
    return ESP_FAIL;
}

void provision_start_portal(void)
{
    if (s_portal_task) return;
    s_portal_active = true;
    xTaskCreatePinnedToCore(portal_task, "portal", PORTAL_TASK_STACK, NULL,
                            4, &s_portal_task, tskNO_AFFINITY);
}

void provision_stop(void)
{
    if (!s_portal_task && !s_portal_active) return;
    s_portal_active = false;
    stop_httpd();
    if (s_portal_task) {
        vTaskDelete(s_portal_task);               /* 状态机切换钩子：中止配网 */
        s_portal_task = NULL;
    }
    esp_wifi_stop();
}

bool provision_is_active(void)
{
    return s_portal_active;
}
