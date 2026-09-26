/**
 * provision.h — 首次配网：SoftAP + captive portal（E14）+ WiFi STA 管理
 *
 * 流程：
 *   1) 设备开热点 MiniPet-XXXX（开放网络，192.168.4.1）
 *   2) DNS 劫持：53 端口所有 A 查询应答 AP 地址（captive probe 必中）
 *   3) 内嵌 HTML 配网页：WiFi SSID/密码 + 服务器地址输入框
 *      （值占位 http://<NAS_IP>:38090）→ POST /save → NVS
 *   4) 拆 portal → STA 连接家里 WiFi → NTP 校时一次 → 写 RTC（E9）
 *   5) esp_restart() 进正常 BOOT 流程
 *
 * 本模块同时是固件内 WiFi 的唯一管理者：STA 连接供状态机自检复用。
 */
#ifndef MP_PROVISION_H
#define MP_PROVISION_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NVS 是否已有 WiFi 配置（ssid+server 必须齐） */
bool provision_has_config(void);

/* 用 NVS 配置连接 STA（阻塞 timeout_ms）。已连接则直接返回 OK。
 * 失败不断线重试（长退避由 poller 驱动）。 */
esp_err_t provision_wifi_connect_sta(uint32_t timeout_ms);

/* 启动配网 portal（SoftAP + DNS + HTTP），幂等；独立任务承载 */
void provision_start_portal(void);

/* 停止 portal（配网成功/状态机切换钩子调用），幂等 */
void provision_stop(void);

/* portal 是否在运行 */
bool provision_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_PROVISION_H */
