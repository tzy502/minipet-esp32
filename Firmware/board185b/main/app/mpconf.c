/**
 * @file mpconf.c
 * @brief 串口配置通道（UART0 控制台）——无 GUI/无网时改板级配置的唯一入口
 *
 * 为什么需要它：185B 是单板（只一个 BOOT 键）+ 圆屏无键盘，配网只能靠
 * SoftAP 配网页（192.168.4.1）。真机踩过的坑：
 *   · 密码打错一个字符 → 板子连不上、日志只说 ESP_FAIL，用户只能重新点一遍配网页；
 *   · 服务器地址（srv_url）写错 → 连上 WiFi 也永远不上线；
 *   · 配网页写入 NVS 后，想改回来必须再走一遍 AP 流程。
 * 有本通道后，主机侧（开发机）一条串口命令即可改配置并重启，**不必碰配网页**：
 *
 *   MPCONF WIFI <ssid> <pass>      写 WiFi 凭据（然后重启生效）
 *   MPCONF SRV  <url>              写服务器地址（自动补 http://）
 *   MPCONF SHOW                    打印当前配置（密码打码）
 *   MPCONF CLEAR                   清配网凭据（等同 Reset WiFi）
 *   MPCONF REBOOT                  立即重启
 *
 * 设计纪律：
 *   · 只在**空闲**时轮询 UART0（10ms 节拍、每次最多读 1 字节）——绝不阻塞、
 *     不与日志输出争用；日志仍走同一 UART（写方向独立）。
 *   · 只认以 "MPCONF " 开头的行，其它输入全部丢弃（防误触）。
 *   · 写 NVS 用既有的 mp_nvs_set_str（命名空间 minipet，与配网页同一套键名），
 *     写完必须 nvs_commit；失败原样报错，不静默。
 *   · 密码回显打码（与 provision 日志同口径：前 2 位 + **** + 后 2 位）。
 */
#include "mpconf.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"   /* 直读 USB-Serial-JTAG RX FIFO（不经 IDF 驱动） */
#include "esp_log.h"
#include "nvs.h"

#include "app_core.h"      /* MP_NVS_NS / mp_nvs_set_str / mp_nvs_get_str */

static const char *TAG = "mpconf";

#define MPCONF_LINE_MAX    160
#define MPCONF_POLL_MS     10
#define MPCONF_STACK       3072
#define MPCONF_PRIO        2            /* 低于一切业务任务 */

static void mask_append(char *dst, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (n <= 4) {
        snprintf(dst, cap, "****");
    } else {
        snprintf(dst, cap, "%c%c****%c%c", s[0], s[1], s[n - 2], s[n - 1]);
    }
}

static void cmd_show(void)
{
    char ssid[40] = { 0 }, pass[80] = { 0 }, srv[128] = { 0 };
    bool has_ssid = mp_nvs_get_str("wifi_ssid", ssid, sizeof(ssid)) && ssid[0];
    bool has_pass = mp_nvs_get_str("wifi_pass", pass, sizeof(pass)) && pass[0];
    bool has_srv  = mp_nvs_get_str("srv_url",  srv,  sizeof(srv))  && srv[0];
    char masked[16];
    mask_append(masked, sizeof(masked), has_pass ? pass : "");
    ESP_LOGW(TAG, "当前配置：ssid=%s pass=%s(%u位) srv_url=%s（编译期默认 %s）",
             has_ssid ? ssid : "(未设)", has_pass ? masked : "(未设)",
             has_pass ? (unsigned)strlen(pass) : 0u,
             has_srv ? srv : "(未设)", MP_DEFAULT_SRV_URL);
}

/* 返回 true = 需要重启 */
static bool handle_line(char *line)
{
    /* 去掉行尾 CR/LF/空白 */
    size_t n = strlen(line);
    while (n > 0 && (line[n-1] == '\r' || line[n-1] == '\n' || line[n-1] == ' ')) line[--n] = 0;
    if (strncmp(line, "MPCONF", 6) != 0) return false;
    char *p = line + 6;
    while (*p == ' ') p++;

    if (strncmp(p, "SHOW", 4) == 0) {
        cmd_show();
        return false;
    }
    if (strncmp(p, "REBOOT", 6) == 0) {
        ESP_LOGW(TAG, "MPCONF REBOOT → 重启");
        return true;
    }
    if (strncmp(p, "CLEAR", 5) == 0) {
        nvs_handle_t h;
        if (nvs_open(MP_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
            ESP_LOGE(TAG, "CLEAR：打开 NVS 失败");
            return false;
        }
        int erased = 0;
        const char *keys[] = { "wifi_ssid", "wifi_pass", "srv_url" };
        for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
            if (nvs_erase_key(h, keys[i]) == ESP_OK) erased++;
        }
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "已清除 %d 项配网凭据 → 重启进配网", erased);
        return true;
    }
    if (strncmp(p, "WIFI", 4) == 0) {
        char *a = p + 4;
        while (*a == ' ') a++;
        char *sp = strchr(a, ' ');
        if (!sp) { ESP_LOGE(TAG, "用法：MPCONF WIFI <ssid> <pass>"); return false; }
        *sp = 0;
        char *b = sp + 1;
        while (*b == ' ') b++;
        if (!a[0]) { ESP_LOGE(TAG, "ssid 为空"); return false; }
        esp_err_t e1 = mp_nvs_set_str("wifi_ssid", a);
        esp_err_t e2 = mp_nvs_set_str("wifi_pass", b);
        bool ok1 = (e1 == ESP_OK), ok2 = (e2 == ESP_OK);
        char masked[16]; mask_append(masked, sizeof(masked), b);
        ESP_LOGW(TAG, "写入 WiFi：ssid=%s pass=%s(%u位) → %s%s",
                 a, masked, (unsigned)strlen(b), (ok1 && ok2) ? "成功，重启生效" : "失败",
                 ok1 ? "" : esp_err_to_name(e1));
        return (ok1 && ok2);
    }
    if (strncmp(p, "SRV", 3) == 0) {
        char *a = p + 3;
        while (*a == ' ') a++;
        if (!a[0]) { ESP_LOGE(TAG, "用法：MPCONF SRV <url>"); return false; }
        esp_err_t e = mp_nvs_set_str("srv_url", a);
        ESP_LOGW(TAG, "写入服务器地址：%s → %s %s", a,
                 (e == ESP_OK) ? "成功，重启生效" : "失败", (e == ESP_OK) ? "" : esp_err_to_name(e));
        return (e == ESP_OK);
    }
    ESP_LOGW(TAG, "未知命令：%s（可用 WIFI/SRV/SHOW/CLEAR/REBOOT）", p);
    return false;
}

static void mpconf_task(void *arg)
{
    (void)arg;
    char line[MPCONF_LINE_MAX];
    size_t len = 0;
    bool warned = false;
    for (;;) {
        /* 【通道选择 2026-10-01 · 两条弯路后的定稿】
         *   弯路① uart_read_bytes(UART_NUM_0)：本板控制台是 USB-Serial-JTAG
         *         （CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y、UART_NUM=-1），UART0 无输入；
         *   弯路② usb_serial_jtag_read_bytes()：它要求"按 IDF 方式安装"的驱动，
         *         console 已装但接口不匹配 → 真机无限重启（boot 头 3 秒 4 次）。
         * 定稿：**直读 LL/RX FIFO**（usb_serial_jtag_ll_*），完全不经 IDF 驱动、
         * 不申请资源、无副作用；有数据就取 1 字节，没有就跳过。 */
        uint8_t c = 0;
        int r = 0;
        if (usb_serial_jtag_ll_rxfifo_data_available() > 0) {
            r = (usb_serial_jtag_ll_read_rxfifo(&c, 1) > 0) ? 1 : 0;
        }
        if (r == 1) {
            if (c == '\n' || c == '\r') {
                if (len) {
                    line[len] = 0;
                    if (handle_line(line)) {
                        vTaskDelay(pdMS_TO_TICKS(300));   /* 让日志刷出去 */
                        esp_restart();
                    }
                    len = 0;
                }
            } else if (len < MPCONF_LINE_MAX - 1) {
                line[len++] = (char)c;
            } else {
                len = 0;                                   /* 超长行丢弃 */
            }
        } else if (!warned) {
            warned = true;
            ESP_LOGI(TAG, "串口配置通道就绪：MPCONF WIFI <ssid> <pass> | SRV <url> | SHOW | CLEAR | REBOOT");
        }
        vTaskDelay(pdMS_TO_TICKS(MPCONF_POLL_MS));
    }
}

void mpconf_start(void)
{
    /* 【不要再装驱动 2026-10-01 血案】控制台（CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y）
     * 已经装好并持有 USB-Serial-JTAG 驱动；本模块再调 usb_serial_jtag_driver_install()
     * → 抢驱动/重置端点 → 真机立刻**无限重启**（串口刷 boot 日志，一屏一屏滚）。
     * 这里只读，不装：驱动由 console 负责，读接口直接可用。 */
    if (xTaskCreatePinnedToCore(mpconf_task, "mpconf", MPCONF_STACK, NULL,
                                MPCONF_PRIO, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "配置任务创建失败（内部堆挤压）—— 串口配置通道不可用");
    }
}
