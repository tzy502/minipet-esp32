/**
 * ota.h — WiFi 直接 OTA（E11：双分区 + 失败自动回滚）
 *
 * 触发：poll 指令 {"t":"ota","v":"0.3.1","u":"/api/device/firmware/0.3.1.bin"}
 * （版本号低于等于当前 → 忽略）。
 *
 * 流程：ota 任务 → GET bin 流式 → esp_ota_begin(OTA_SIZE_UNKNOWN) 逐块写 →
 * esp_ota_end 校验镜像 → esp_ota_set_boot_partition → 重启。
 * 下载/校验失败 → esp_ota_abort，留在当前分区，OTA_END（状态机回 POKER）。
 * 新分区启动后不稳定（未确认）→ IDF 下次启动自动回滚旧分区；
 * 确认点 = poller 首次成功（mp_ota_confirm_valid）。
 */
#ifndef MP_OTA_H
#define MP_OTA_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 创建 OTA 任务（PRO 核，最低优先级——4.1；平时阻塞等指令） */
void ota_start(void);

/* poller 收到 ota 指令时投递（ver 形如 "0.3.1"；url 为绝对或相对路径） */
void mp_ota_offer(const char *ver, const char *url);

/* 新分区稳定确认（联网成功后调用；仅 pending 状态生效） */
void mp_ota_confirm_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_OTA_H */
