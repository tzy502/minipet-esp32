/**
 * provision.c — SoftAP captive portal 配网（E14）+ STA 连接管理
 *
 * captive portal 三件套：
 *   - DNS 劫持：UDP:53 全部 A 查询 → 192.168.4.1（302 探测必落到本机）
 *   - HTTP：esp_http_server；非 "/" 的 GET（generate_204 / hotspot-detect.html
 *     等）一律 302 → http://192.168.4.1/
 *   - 页面：三步傻瓜化向导
 *       第 1 步：扫描周围 WiFi 列表（GET /scan → JSON，APSTA 阻塞式扫描，
 *                按信号排序去重）点选自动填 SSID
 *       第 2 步：输入 WiFi 密码
 *       第 3 步：服务器地址（示例格式提示 http://IP:38090，可留空回落默认）
 *     STA 连接失败重开 portal 时，页面在列表顶部横幅提示重选 WiFi。
 *
 * 保存后：portal 任务拆栈 → STA 连接（15s）→ SNTP 校时（15s，成功与否均打日志）
 * → rtc_set_time → esp_restart()。任一步失败 → 重开 portal（用户重试）。
 */
#include "provision.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "cJSON.h"

#include "app_core.h"
#include "hal_contract.h"

static const char *TAG = "provision";

#define PORTAL_TASK_STACK  6144
#define SCAN_MAX_APS       25            /* /scan 返回上限（去重前） */

static bool           s_wifi_inited;       /* esp_wifi_init 只做一次 */
static bool           s_portal_active;
static bool           s_last_connect_failed; /* STA 失败重开 portal：页面顶部横幅 */
static httpd_handle_t s_httpd;
static TaskHandle_t   s_dns_task;
static TaskHandle_t   s_portal_task;
static EventGroupHandle_t s_wifi_events;   /* IDF5：句柄类型是 EventGroupHandle_t */
#define WIFI_GOT_IP_BIT   BIT0
#define WIFI_FAIL_BIT     BIT1

/* ================================================================== */
/* 配置页（内嵌；UTF-8；三步向导；HEAD/BODY 分段以便注入失败横幅）        */
/* ================================================================== */
static const char PAGE_PORTAL_HEAD[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>MiniPet 配网</title><style>"
"body{font-family:sans-serif;background:#101014;color:#e8e8ec;max-width:420px;"
"margin:24px auto;padding:0 16px}"
"h2{font-weight:600;text-align:center;margin:20px 0 4px}"
".banner{background:#5c1f2b;border:1px solid #e0486a;color:#ffd9e0;border-radius:10px;"
"padding:12px 14px;margin:14px 0;font-size:15px;line-height:1.5}"
".step{margin-top:24px}"
".step b{display:block;font-size:17px;margin-bottom:10px}"
".aplist{border:1px solid #3a3a44;border-radius:10px;max-height:240px;"
"overflow-y:auto;overflow-x:hidden}"
".ap{display:flex;align-items:center;padding:12px;border-bottom:1px solid #2a2a32;"
"cursor:pointer;font-size:16px;border-left:4px solid transparent}"
".ap:last-child{border-bottom:0}"
".ap.sel{background:#2a1c23;border-left:4px solid #e0486a}"
".ic{display:flex;align-items:center;margin-right:10px;flex:none}"
".ic svg{margin-left:4px;flex:none}"
".nm{flex:1;word-break:break-all}"
".bars{display:inline-flex;align-items:flex-end;gap:2px}"
".bars i{width:4px;border-radius:1px;background:#555}"
".bars i.on{background:#4cd964}"
".bars i.h1{height:5px}.bars i.h2{height:9px}.bars i.h3{height:13px}.bars i.h4{height:17px}"
".hint{color:#8a93a6;font-size:13px;line-height:1.6;margin-top:8px;padding:0 2px}"
".hint.pad{padding:14px}"
".err{color:#ff8aa0;font-size:14px;margin-top:8px}"
"label{display:block;margin-top:14px;color:#9aa}"
"input{width:100%;box-sizing:border-box;margin:6px 0 4px;padding:12px;"
"font-size:16px;border-radius:8px;border:1px solid #555;background:#1c1c22;color:#eee}"
".big{width:100%;margin-top:28px;padding:16px;font-size:20px;font-weight:600;"
"border:0;border-radius:10px;background:#e0486a;color:#fff}"
".rescan{margin-top:10px;padding:10px 16px;font-size:14px;border-radius:8px;"
"border:1px solid #555;background:#1c1c22;color:#ccc}"
"</style></head><body>"
"<h2>MiniPet 配网向导</h2>";

/* STA 连接失败重开 portal 时注入在列表顶部（portal_get_handler 按需拼接） */
static const char PAGE_BANNER_FAIL[] =
"<div class=\"banner\">上一次连接失败，请重选 WiFi 并确认密码输入正确。</div>";

static const char PAGE_PORTAL_BODY[] =
"<div class=\"step\"><b>第 1 步：选择你家的 WiFi</b>"
"<div class=\"aplist\" id=\"list\"><div class=\"hint pad\">正在扫描附近的 WiFi，请稍候…</div></div>"
"<button type=\"button\" class=\"rescan\" onclick=\"scan()\">重新扫描</button>"
"<div class=\"err\" id=\"err\" style=\"display:none\">没找到你家的 WiFi？把设备放近一点，再点一次「重新扫描」。</div>"
"</div>"
"<form method=\"POST\" action=\"/save\">"
"<div class=\"step\"><b>第 2 步：输入 WiFi 密码</b>"
"<label>WiFi 名称（点上面列表会自动填）</label>"
"<input name=\"ssid\" id=\"ssid\" maxlength=\"32\" required>"
"<label>WiFi 密码</label>"
"<input name=\"pass\" id=\"pass\" type=\"password\" maxlength=\"64\">"
"<div class=\"hint\" id=\"passhint\">在上方列表点一下你家的 WiFi，名称会自动填到这里。</div>"
"</div>"
"<div class=\"step\"><b>第 3 步：服务器地址</b>"
"<label>服务器地址（不清楚可留空，或问部署服务的人）</label>"
"<input name=\"server\" maxlength=\"127\" placeholder=\"例如：http://192.168.1.100:38090\">"
"<div class=\"hint\">格式：http://服务器IP:38090（端口默认 38090）。</div>"
"</div>"
"<button type=\"submit\" class=\"big\">保存并连接</button>"
"<div class=\"hint\" style=\"text-align:center\">保存后设备会自动连 WiFi、校时并重启，约 30 秒，期间请不要断电。</div>"
"</form>"
"<script>"
"var LOCK='<svg width=\"14\" height=\"14\" viewBox=\"0 0 24 24\"><path fill=\"#b9c\" d=\"M12 2a5 5 0 0 0-5 5v3H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-8a2 2 0 0 0-2-2h-1V7a5 5 0 0 0-5-5zm-3 8V7a3 3 0 0 1 6 0v3H9z\"/></svg>';"
"function bars(r){var n=r>=-55?4:r>=-67?3:r>=-79?2:1;var s='';"
"for(var i=1;i<5;i++){s+='<i class=\"'+(i<=n?'on ':'')+'h'+i+'\"></i>'}"
"return '<span class=\"bars\">'+s+'</span>';}"
"function scan(){"
"var list=document.getElementById('list'),err=document.getElementById('err');"
"err.style.display='none';"
"list.innerHTML='<div class=\"hint pad\">正在扫描附近的 WiFi，请稍候…（约 3 秒，期间手机可能短暂断开热点）</div>';"
"fetch('/scan?_='+Date.now()).then(function(r){if(!r.ok)throw 0;return r.json()})"
".then(function(d){list.innerHTML='';"
"if(!d.length){list.innerHTML='<div class=\"hint pad\">附近没有扫到 WiFi</div>';err.style.display='block';return}"
"d.forEach(function(ap){"
"var row=document.createElement('div');row.className='ap';"
"var ic=document.createElement('span');ic.className='ic';ic.innerHTML=bars(ap.rssi)+(ap.auth>0?LOCK:'');"
"var nm=document.createElement('span');nm.className='nm';nm.textContent=ap.ssid;"
"row.appendChild(ic);row.appendChild(nm);"
"row.onclick=function(){pick(row,ap.ssid,ap.auth)};list.appendChild(row);});})"
".catch(function(){list.innerHTML='<div class=\"hint pad\">扫描失败，请点「重新扫描」再试一次</div>'});}"
"function pick(row,ssid,auth){"
"document.getElementById('ssid').value=ssid;"
"var sel=document.querySelectorAll('.ap.sel');"
"for(var i=0;i<sel.length;i++){sel[i].classList.remove('sel')}"
"row.classList.add('sel');document.getElementById('pass').value='';"
"document.getElementById('passhint').textContent=auth>0?'已选「'+ssid+'」，请在下面输入它的 WiFi 密码':'「'+ssid+'」无需密码，直接点「保存并连接」就行';"
"if(auth>0){document.getElementById('pass').focus()}}"
"scan();"
"</script></body></html>";

static const char PAGE_OK[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>MiniPet</title></head>"
"<body style=\"font-family:sans-serif;background:#101014;color:#e8e8ec;"
"text-align:center;padding-top:18%;max-width:420px;margin:0 auto\">"
"<h2>已保存，设备正在连接…</h2>"
"<p>正在连接你选的 WiFi 并自动校时，完成后设备会自动重启。</p>"
"<p style=\"color:#778\">这一步不用操作，等 30 秒左右就好。</p></body></html>";

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
/* HTTP（captive portal 探测 + 扫描列表 + 配网页 + 保存）                */
/* ================================================================== */
static esp_err_t portal_get_handler(httpd_req_t *req)
{
    ESP_LOGI("portal", "HTTP GET 命中");
    const char *uri = req->uri;
    if (strcmp(uri, "/") != 0 && strcmp(uri, "/index.html") != 0) {
        /* Android/Apple/Windows 的联网探测路径：302 到首页 */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* 分段发送：HEAD + （失败横幅，按需） + BODY。页面拆两段是为了把
     * 「上一次连接失败」横幅注入到第 1 步列表顶部 */
    httpd_resp_send_chunk(req, PAGE_PORTAL_HEAD, sizeof(PAGE_PORTAL_HEAD) - 1);
    if (s_last_connect_failed) {
        httpd_resp_send_chunk(req, PAGE_BANNER_FAIL, sizeof(PAGE_BANNER_FAIL) - 1);
    }
    httpd_resp_send_chunk(req, PAGE_PORTAL_BODY, sizeof(PAGE_PORTAL_BODY) - 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static int cmp_rssi_desc(const void *a, const void *b)
{
    const wifi_ap_record_t *ra = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *rb = (const wifi_ap_record_t *)b;
    return rb->rssi - ra->rssi;
}

/* GET /scan → [{"ssid":"...","rssi":-52,"auth":3},...]
 * 信号强→弱排序，同名去重（保留最强），隐藏 SSID 跳过。 */
static esp_err_t portal_scan_handler(httpd_req_t *req)
{
    ESP_LOGI("portal", "HTTP SCAN 命中");
    if (!s_portal_active) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_send(req, NULL, 0);
    }

    /* APSTA 下扫描会逐信道跳转，SoftAP 客户端会卡 2~3 秒；
     * 页面拉 /scan 前已显示「正在扫描…」，属预期现象 */
    wifi_scan_config_t scfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,                            /* 全信道 */
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 0, .max = 120 },
    };
    esp_err_t err = esp_wifi_scan_start(&scfg, true /* 阻塞直到扫完 */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan start failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, NULL, 0);
    }

    uint16_t num = SCAN_MAX_APS;
    wifi_ap_record_t *records = calloc(num, sizeof(wifi_ap_record_t));
    if (!records) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    err = esp_wifi_scan_get_ap_records(&num, records);   /* 取结果并释放内部缓存 */
    if (err != ESP_OK || num == 0) {
        free(records);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    qsort(records, num, sizeof(wifi_ap_record_t), cmp_rssi_desc);

    char added[SCAN_MAX_APS][33];                /* 已输出 SSID（去重用） */
    int  added_n = 0;
    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < num; i++) {
        char ssid[33];
        memcpy(ssid, records[i].ssid, sizeof(ssid) - 1);  /* 32B SSID 可能无 NUL */
        ssid[sizeof(ssid) - 1] = 0;
        if (ssid[0] == 0) continue;              /* 隐藏网络：手输场景，不进列表 */
        bool dup = false;
        for (int j = 0; j < added_n; j++) {
            if (strcmp(added[j], ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        memcpy(added[added_n++], ssid, sizeof(ssid));

        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "ssid", ssid);
        cJSON_AddNumberToObject(it, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(it, "auth", (double)records[i].authmode);
        cJSON_AddItemToArray(arr, it);
    }
    free(records);

    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t send_err = httpd_resp_send(req, body, strlen(body));
    cJSON_free(body);
    ESP_LOGI(TAG, "scan done: %u APs seen, %d listed", (unsigned)num, added_n);
    return send_err;
}

static esp_err_t portal_post_save_handler(httpd_req_t *req)
{
    ESP_LOGI("portal", "HTTP POST_SAVE 命中");
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

bool provision_portal_active(void)
{
    return s_portal_active;
}

/* 【临时诊断】内部堆布局转储：查 httpd 任务/listen 分配失败的真实内存状况 */
static void dump_internal_heap(void)
{
    ESP_LOGW(TAG, "内部堆: 总 %u 空闲 %u 最大块 %u",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
}

static void start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;    /* 通配捕获所有探测路径 */
    cfg.max_uri_handlers = 4;
    /* 内部 RAM 碎片化：8K 栈分配失败（ESP_ERR_HTTPD_TASK）→ 降档重试。
     * /scan 阻塞扫描+cJSON 需要栈，6144 为下限 */
    static const int stacks[] = { 8192, 6144, 4096 };
    static bool dumped;
    for (int attempt = 0; attempt < 20; attempt++) {   /* 20×3s：等 lwip 缓冲/poller 停摆后重试 */
        if (!dumped) { dump_internal_heap(); dumped = true; }
        int si = attempt % 3;
        cfg.stack_size = stacks[si];
        esp_err_t hs = httpd_start(&s_httpd, &cfg);
        if (hs == ESP_OK) {
            ESP_LOGI(TAG, "httpd 已启动，监听 80 端口（栈 %d）", stacks[si]);
            httpd_uri_t get_scan = {
                .uri = "/scan", .method = HTTP_GET, .handler = portal_scan_handler, .user_ctx = NULL };
            httpd_uri_t get_any = {
                .uri = "/*", .method = HTTP_GET, .handler = portal_get_handler, .user_ctx = NULL };
            httpd_uri_t post_save = {
                .uri = "/save", .method = HTTP_POST, .handler = portal_post_save_handler, .user_ctx = NULL };
            /* 先注册精确路由再注册通配：命中查找按注册顺序取第一个 */
            httpd_register_uri_handler(s_httpd, &get_scan);
            httpd_register_uri_handler(s_httpd, &get_any);
            httpd_register_uri_handler(s_httpd, &post_save);
            return;
        }
        ESP_LOGW(TAG, "httpd 第 %d 次启动失败(栈 %d): %s，最大连续内部块 %u",
                 attempt + 1, stacks[si], esp_err_to_name(hs),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    ESP_LOGE(TAG, "httpd 重试窗口耗尽——配网页不可用");

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
    esp_wifi_stop();    /* 失败重开路径下 WiFi 可能仍以 STA 模式在跑，先停干净 */
    /* APSTA：STA 口保持 up 才能扫周围 WiFi（GET /scan）。
     * 扫描逐信道跳转时 SoftAP 客户端短暂卡顿 —— 页面已提示「正在扫描…」 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
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
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed, RTC not updated");
        return false;
    }
    bool ok = (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK);
    esp_netif_sntp_deinit();
    if (!ok) {
        ESP_LOGW(TAG, "SNTP sync timeout (15s), RTC not updated");
        return false;
    }

    time_t now = 0;
    struct tm tm_now = { 0 };
    time(&now);
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year < 120) {
        ESP_LOGW(TAG, "SNTP returned bogus time, RTC not updated");
        return false;                             /* 时钟显然未同步 */
    }

    setenv("TZ", "CST-8", 1);                     /* 东八区 */
    tzset();
    ESP_LOGI(TAG, "SNTP synced: %04d-%02d-%02d %02d:%02d:%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    if (!mp_rtc_set_time(&tm_now)) {              /* PCF85063（hal_contract 适配） */
        ESP_LOGW(TAG, "RTC write failed");
        return false;
    }
    ESP_LOGI(TAG, "RTC set OK");
    return true;
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

    ESP_LOGI(TAG, "portal up: ssid=%s ip=192.168.4.1 last_fail=%d",
             ssid, (int)s_last_connect_failed);

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
    /* 空密码 = 开放网络：WPA2 阈值会把它拒掉，按密码有无选阈值 */
    sta.sta.threshold.authmode =
        pass_sta[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    xEventGroupClearBits(s_wifi_events, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & WIFI_GOT_IP_BIT) {
        s_last_connect_failed = false;
        sntp_and_set_rtc();                       /* 校时失败不阻断（下次补），成败均有日志 */
        ESP_LOGI(TAG, "provision done, rebooting into normal boot");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    /* STA 失败：立横幅标记 → 重开 portal（用户在列表顶部看到失败提示重选） */
    ESP_LOGI(TAG, "STA connect failed, reopening portal with fail banner");
    s_last_connect_failed = true;
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
    sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
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
