/**
 * hal_contract.h — 应用层对 BSP/渲染/profile 的统一消费面（适配层）
 *
 * 并行模块已落地，本文件不再「平行声明」而是直接包裹真实头：
 *   - components/drivers/include/drivers.h（驱动层统一入口）
 *   - main/render/render.h（渲染层总接口）
 *   - profiles/amoled216.h（minipet_profile_t / MINIPET_PROFILE_AMOLED216）
 *
 * 本层提供的小适配（mp_* 前缀）：
 *   - IMU mg → g 换算；PMU 轮询的便捷读法；RTC 结构透传
 *   - codec 写接口按「立体声帧数」计数（bgm 不关心字节）
 *   - display_off = display_set_sleep(true)（FATAL 关屏语义）
 * 渲染层路径型 API（render_set_parts/set_layout/set_map/set_clock）由
 * 状态机直接调用（素材 hash→路径 解析在 asset_dl）。
 */
#ifndef MP_HAL_CONTRACT_H
#define MP_HAL_CONTRACT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "drivers.h"          /* components/drivers 真实接口 */
#include "render/render.h"    /* 渲染层真实接口（含 profile 类型） */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 显示适配 ============================ */
static inline void mp_display_off(void)      { display_set_sleep(true); }
static inline void mp_display_backlight(uint8_t pct) { display_brightness(pct); }
/* display_fill_rect / display_blit / display_init 原名直用（drivers.h） */

/* ============================ 触摸适配 ============================ */
typedef struct {
    bool    touched;
    int16_t x, y;
} touch_sample_t;

static inline bool mp_touch_read(touch_sample_t *out)
{
    touch_point_t t;
    if (touch_cst9220_read(&t) != ESP_OK) return false;
    out->touched = t.pressed;
    out->x = (int16_t)t.x;
    out->y = (int16_t)t.y;
    return true;
}

/* ============================ IMU 适配（mg → g） ==================== */
typedef struct {
    float x_g, y_g, z_g;
} imu_accel_t;

static inline bool mp_imu_wait_event(TickType_t wait_ticks)
{
    return imu_qmi8658_wait_event((uint32_t)wait_ticks);
}

static inline bool mp_imu_read_accel(imu_accel_t *out)
{
    float mg[3];
    if (imu_qmi8658_read_acc(mg) != ESP_OK) return false;
    out->x_g = mg[0] / 1000.0f;
    out->y_g = mg[1] / 1000.0f;
    out->z_g = mg[2] / 1000.0f;
    return true;
}

/* ============================ RTC 适配 ============================ */
static inline bool mp_rtc_get_time(struct tm *out)
{
    return rtc_pcf85063_get_time(out) == ESP_OK;
}
static inline bool mp_rtc_set_time(const struct tm *t)
{
    return rtc_pcf85063_set_time(t) == ESP_OK;
}

/* ============================ PMU 适配 ============================ */
static inline uint8_t mp_pmu_battery_pct(void)
{
    uint8_t pct = 100;
    pmu_axp2101_get_battery_pct(&pct);
    return pct;
}
static inline bool mp_pmu_charging(void)
{
    bool vbus = false, bat = false, charging = false;
    pmu_axp2101_get_power(&vbus, &bat, &charging);
    return charging;
}
static inline float mp_pmu_temp_c(void)
{
    float c = -1.0f;
    pmu_axp2101_get_temperature_c(&c);
    return c;
}

/* ============================ TF 适配 ============================= */
static inline bool mp_sd_mount(void)
{
    return sd_mount() == 0;      /* sd_tf.h：返回 errno，0=成功，挂载点 /sdcard */
}

/* ============================ codec 适配（帧数计数） =============== */
static inline void mp_codec_init(uint32_t rate_hz)
{
    codec_es8311_init(rate_hz);   /* bgm_start 一次性调用（PA 默认关） */
}
/* 播放前按当前流采样率配置（init 之后调；44.1k/48k 都走 16bit 档） */
static inline void mp_codec_start(uint32_t rate_hz)
{
    codec_es8311_set_sample_rate(rate_hz);
}
static inline void mp_codec_set_rate(uint32_t rate_hz)
{
    codec_es8311_set_sample_rate(rate_hz);
}
/* interleaved 16bit 立体声，frame_count 帧 = 4×frame_count 字节 */
static inline esp_err_t mp_codec_write(const int16_t *stereo_frames, size_t frame_count)
{
    return codec_es8311_write(stereo_frames, frame_count * 4);
}
static inline void mp_pa_enable(bool on)
{
    pa_ctrl_enable(on);
}

#ifdef __cplusplus
}
#endif

#endif /* MP_HAL_CONTRACT_H */
