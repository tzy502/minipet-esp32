/**
 * mdns_discover.c — mDNS 服务发现：设备自动找到服务端（E14）
 *
 * API 依据（禁止凭猜）：ESP-IDF 5.5 不内置 lwip mdns，走 registry 组件
 * `espressif/mdns`（见 main/idf_component.yml）。本文件按 **mdns 1.2.2**
 * 头文件逐字核对签名后编写（tag mdns-v1.2.2 / components/mdns/include/mdns.h）：
 *   esp_err_t mdns_init(void);
 *   void      mdns_free(void);
 *   esp_err_t mdns_query_ptr(const char *service_type, const char *proto,
 *                            uint32_t timeout, size_t max_results,
 *                            mdns_result_t **results);
 *   void      mdns_query_results_free(mdns_result_t *results);
 *   esp_err_t mdns_query_a(const char *host_name, uint32_t timeout,
 *                          esp_ip4_addr_t *addr);
 *   struct mdns_result_s { ... char *hostname; uint16_t port;
 *                          mdns_ip_addr_t *addr; mdns_result_t *next; };
 *   struct mdns_ip_addr_s { esp_ip_addr_t addr; mdns_ip_addr_t *next; };
 * mdns_query_ptr 内部是阻塞式（mdns_query → xSemaphoreTake(portMAX_DELAY)），
 * 由组件自己的 "mdns" 任务在 timeout 后唤醒，故本调用最坏 = timeout。
 *
 * 设计取舍：
 *   - 只查 PTR（拿 instance/hostname/port，外加可能的 A 记录）；A 记录不在一
 *     个响应里时（真机常见：SRV 与 A 分两包）再用 mdns_query_a 按 hostname 补一次
 *   - 两次查询合计 ~2.2s 上限，失败静默（不拖慢启动/联网）
 *   - 用完 mdns_free：不常驻任务、不常驻内部堆（发现成功后地址只在内存里）
 */
#include "mdns_discover.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "mdns.h"

static const char *TAG = "mdns";

#define MDNS_PTR_TIMEOUT_MS   1100      /* 单次查询窗口（服务端 mDNS 响应通常在 100ms 内） */
#define MDNS_A_TIMEOUT_MS     1100
#define MDNS_MAX_RESULTS      4         /* 局域网内可能有多台服务端（多实例） */
#define MDNS_DEFAULT_PORT     38090     /* 服务端默认端口（配网页提示同值） */

/* mdns_ip_addr_t 链表里挑第一个 IPv4 */
static const mdns_ip_addr_t *first_ipv4(const mdns_ip_addr_t *a)
{
    for (; a; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) return a;
    }
    return NULL;
}

static void format_url(const mdns_ip_addr_t *a, uint16_t port, char *out, size_t cap)
{
    snprintf(out, cap, "http://" IPSTR ":%u", IP2STR(&a->addr.u_addr.ip4),
             (unsigned)(port ? port : MDNS_DEFAULT_PORT));
}

bool mp_mdns_discover_server(char *url_out, size_t cap)
{
    if (!url_out || cap < 32) return false;
    url_out[0] = 0;

    /* STA 没拿到 IP 时无出口：直接放弃（不打印，调用方每分钟会重试） */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = { 0 };
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0) {
        return false;
    }

    if (mdns_init() != ESP_OK) return false;     /* 静默：拿不到 mDNS 不该刷屏 */

    bool ok = false;
    mdns_result_t *res = NULL;
    if (mdns_query_ptr(MP_MDNS_SERVICE, MP_MDNS_PROTO,
                       MDNS_PTR_TIMEOUT_MS, MDNS_MAX_RESULTS, &res) == ESP_OK && res) {
        for (mdns_result_t *r = res; r && !ok; r = r->next) {
            const mdns_ip_addr_t *a = first_ipv4(r->addr);
            if (!a && r->hostname && r->hostname[0]) {
                /* PTR/SRV 与 A 分两包：按 hostname 再补一次 A 查询 */
                esp_ip4_addr_t v4 = { 0 };
                if (mdns_query_a(r->hostname, MDNS_A_TIMEOUT_MS, &v4) == ESP_OK &&
                    v4.addr != 0) {
                    snprintf(url_out, cap, "http://" IPSTR ":%u", IP2STR(&v4),
                             (unsigned)(r->port ? r->port : MDNS_DEFAULT_PORT));
                    ESP_LOGI(TAG, "发现服务端（A 补查）: %s instance=%s host=%s",
                             url_out, r->instance_name ? r->instance_name : "?",
                             r->hostname);
                    ok = true;
                }
            } else if (a) {
                format_url(a, r->port, url_out, cap);
                ESP_LOGI(TAG, "发现服务端: %s instance=%s host=%s",
                         url_out, r->instance_name ? r->instance_name : "?",
                         r->hostname ? r->hostname : "?");
                ok = true;
            }
        }
    }
    if (res) mdns_query_results_free(res);
    mdns_free();                                  /* 用完即还：无常驻任务/内部堆 */

    return ok;
}
