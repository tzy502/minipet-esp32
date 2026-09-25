/**
 * http_client.h — esp_http_client 封装 + 服务器地址 NVS（E2）
 *
 * 服务器地址：NVS "srv_url"（provision 写入，形如 http://<NAS_IP>:38090，
 * 无结尾斜杠）。所有端点 = base + "/api/device/..."，设备永远是 client。
 *
 * 端点对照（E2）：
 *   POST /api/device/hello            注册（profile+UUID+固件版本）
 *   GET  /api/device/manifest         素材版本表
 *   GET  /api/device/asset/{hash}     素材包
 *   GET  /api/device/poll?since=      长轮询指令队列
 *   POST /api/device/event            事件上报
 *   GET  /api/device/bgm/stream?id=   MP3 流
 *   POST /api/device/bgm/cmd          设备端 BGM 控制回传
 *   GET  /api/device/firmware/{v}.bin OTA 固件包
 */
#ifndef MP_HTTP_CLIENT_H
#define MP_HTTP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 分块回调：返回 false 中止下载（调用方主动停止，如 BGM 切歌） */
typedef bool (*mp_http_chunk_cb)(void *ctx, const char *data, size_t len);

/* 初始化：读 NVS 服务器地址 + 生成设备 UUID（efuse MAC）。
 * 在 WiFi 就绪后、任何请求前调用一次（state_machine_boot）。 */
void mp_http_init(void);

/* 服务器 base URL（如 http://192.168.1.100:38090）；未配置返回 NULL */
const char *mp_http_server_url(void);

/* 设备 UUID（"AABBCCDDEEFF"，efuse MAC，开机生成，稳定不变） */
const char *mp_http_uuid(void);

/* 注册返回的 deviceId（hello 之前返回 UUID 兜底） */
const char *mp_http_device_id(void);

/* 流式 GET：path 为完整路径（"/api/device/..."）或绝对 URL（OTA/BGM 下发时）。
 * 返回 HTTP 状态码（200/304…）；网络/传输错误返回 -1。
 * chunk_cb 为 NULL 时仅探测状态码（HEAD 语义用 GET 实现）。 */
int mp_http_get(const char *path_or_url, uint32_t timeout_ms,
                mp_http_chunk_cb cb, void *ctx);

/* POST JSON：body 为序列化好的 JSON；响应（可选）写入 resp_buf。
 * 返回 HTTP 状态码；网络错误 -1。 */
int mp_http_post_json(const char *path, const char *json_body,
                      char *resp_buf, size_t resp_cap, uint32_t timeout_ms);

/* POST /api/device/hello（E2：profile(w,h,shape,psram,audio)+UUID+固件版本
 * → deviceId 与配置）。返回 0=成功；非 0=失败码。 */
int mp_http_hello(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_HTTP_CLIENT_H */
