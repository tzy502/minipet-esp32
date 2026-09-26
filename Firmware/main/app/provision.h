/**
 * provision.h — 首次配网：SoftAP + captive portal（E14）+ WiFi STA 管理
 *
 * 流程：
 *   1) 设备开热点 MiniPet-XXXX（开放网络，192.168.4.1，APSTA 模式）
 *   2) DNS 劫持：53 端口所有 A 查询应答 AP 地址（captive probe 必中）
 *   3) 内嵌三步向导页：① 扫描周围 WiFi 列表（GET /scan → JSON，点选自动
 *      填 SSID）→ ② 输密码 → ③ 服务器地址（示例格式提示，可留空）
 *      → POST /save → NVS；STA 连接失败重开 portal 时页面顶部横幅提示
 *   4) 拆 portal → STA 连接家里 WiFi → NTP 校时一次（成败均打日志）→ 写 RTC（E9）
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

/* SoftAP 热点名（"MiniPet-XXXX"，MAC 后 4 hex；屏显横幅提示共用，问题4） */
void provision_get_ap_ssid(char *out, size_t cap);

/* 用 NVS 配置连接 STA（阻塞 timeout_ms）。已连接则直接返回 OK。
 * 失败不断线重试（长退避由 poller 驱动）。 */
esp_err_t provision_wifi_connect_sta(uint32_t timeout_ms);

/* 启动配网 portal（SoftAP + DNS + HTTP），幂等；独立任务承载 */
void provision_start_portal(void);

/* 停止 portal（配网成功/状态机切换钩子调用），幂等 */
void provision_stop(void);

/* portal 是否在运行 */
bool provision_is_active(void);

/* portal（SoftAP+HTTP 配网）处于活动态（供 poller 挂起轮询用） */
bool provision_portal_active(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_PROVISION_H */
