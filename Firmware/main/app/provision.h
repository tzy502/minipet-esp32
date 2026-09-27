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

/* E9 常态化校时：起后台任务，系统时间无效（RTC 未校准）时按窗口触发 SNTP
 * 并回写 PCF85063；成功/有效后转 6 小时周期。幂等，可重复调用。 */
void provision_rtc_resync_start(void);

/* 【启动早期】只建 WiFi 栈（esp_netif + esp_wifi_init），不连接、不阻塞。
 * 必须在渲染任务/LVGL 大缓冲之前调用：本板内部堆 ~143KB，晚调会让
 * esp_netif_create_default_wifi_sta() 返回 ESP_ERR_NO_MEM → abort 无限重启，
 * WiFi 永不初始化 → 设备永不 poll。幂等，后续 provision_* 复用同一实例。 */
void provision_wifi_preinit(void);

/* 【启动早期 · 真机崩溃修复】无配网凭据时提前启动 SoftAP。
 *
 * 背景：Reset WiFi/首次开机后设备无线重启，崩点为空指针（EXCVADDR=0x2c），
 * 现场日志 `wifi:alloc eb len=752 type=4 fail` → SoftAP 的 beacon 缓冲没分到，
 * WiFi 驱动随后解引用空指针。原因：portal_task 起 AP 时，渲染任务 + 整屏缓冲
 * + BGM/I2S DMA 已把内部堆切碎（相邻日志即 "bgm 任务首建失败（内部堆挤压）"）。
 *
 * 契约：必须在渲染/BGM 任务创建【之前】调用（main.c 紧接 provision_wifi_preinit
 * 之后）。有配网凭据时本函数空转（不需要 AP，也不白占内部堆）。幂等。 */
void provision_ap_early_start_if_needed(void);

/* 【启动早期 · 配网页可用性修复】无配网凭据时提前建好 portal 三件套
 * （SoftAP + httpd 配网页 + dns53 captive 劫持）。
 *
 * 背景（真机实测 2026-09-27）：用户 Reset WiFi 后连热点却打不开 192.168.4.1。
 * 日志链：起 SoftAP 前内部堆 空闲=74751/最大块=31732 → SoftAP 起后 69331/30708
 * → portal 任务创建失败（空闲=4955 最大块=3444）。渲染任务(12K 栈)+LVGL+codec/
 * I2S+各网络任务把内部堆切成碎片，6KB 级 httpd 栈再也建不起来（旧实现还静默）。
 * 因此 6~8KB 的 httpd 必须在这个干净窗口建好；portal 任务本身只负责
 * "等 /save → 拆 AP → 连 STA → 重启"，不再承担建服务。
 *
 * 契约：必须在渲染/BGM 任务创建【之前】调用（main.c 紧接 ap_early_start 之后）。
 * 有配网凭据时空转。幂等（httpd/dns 各自查重）。 */
void provision_portal_early_start_if_needed(void);

/* 【僵局打破】强制断开并重新关联：用于"有 IP 但 TCP 一直不通"的场景
 *（当前关联质量差 / 路由器认证风暴保护），换一次关联比原地重试有效。 */
void provision_wifi_force_reconnect(void);

/* E14：恢复出厂配网（清 WiFi/服务器地址/轮询游标后重启 → 进 SoftAP portal）。 */
void provision_factory_reset(void);

/* 【真机取证】分阶段内部堆明细（空闲/最大连续块 + heap_caps 分段打印）。
 *  用于定位"启动末期最大连续块仅 2KB → lwIP 无法建连"的分配来源。 */
void provision_dump_internal_heap(const char *stage);

/* 校时任务是否已成功创建（供失败重试判定）。 */
bool provision_rtc_task_running(void);

/* 校时单步（由已存在的任务周期调用，避免新建任务栈）。 */
void provision_rtc_resync_step(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_PROVISION_H */
