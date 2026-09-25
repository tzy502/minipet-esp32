/**
 * @file rtc_pcf85063.h
 * @brief PCF85063ATL RTC 驱动（共享 I2C + INT）
 *
 * 引脚（来自 profile）：RTC_INT = GPIO13（当前仅配为输入；闹钟/秒脉冲
 * 功能后续需要时再使能对应中断方向）。
 *
 * 时间口径：设备端日历 = Unix epoch 经 gmtime_r 展开的 UTC 日历（不带时区）。
 * set_from_epoch(t) 写入的就是 epoch 对应的 UTC 字段；get_time() 读回同口径
 * struct tm（tm_year 已补 1900 基准）。app 层显示本地时间自行加时区偏移。
 */
#pragma once

#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化：注册 I2C 器件 + 检查振荡器停止(OS)标志。
 *
 * 首次上电 / 后备电池失效时 OS=1，get_time() 会报 ESP_ERR_INVALID_STATE，
 * app 应在联网后用服务器时间 set_from_epoch() 校时。
 */
esp_err_t rtc_pcf85063_init(void);

/** @brief 读时间到 struct tm（gmtime 口径）。OS 置位时返回 ESP_ERR_INVALID_STATE。 */
esp_err_t rtc_pcf85063_get_time(struct tm *out);

/** @brief 用 Unix epoch 校时（写全部时间寄存器，STOP 包裹保证原子性） */
esp_err_t rtc_pcf85063_set_from_epoch(int64_t unix_epoch);

/**
 * @brief 直接写 struct tm（字段按 gmtime 口径：tm_mon 0-11 / tm_year = 年-1900）
 *
 * set_from_epoch 的底层面；app 若自己持有 struct tm（如 NTP 结果）可直接写。
 */
esp_err_t rtc_pcf85063_set_time(const struct tm *t);

/** @brief 振荡器是否正常走时（OS 标志为 0） */
bool rtc_pcf85063_osc_ok(void);

#ifdef __cplusplus
}
#endif
