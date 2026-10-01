/**
 * hal_contract.h — 应用层对 BSP/渲染/profile 的统一消费面（适配层）
 *
 * 并行模块已落地，本文件不再「平行声明」而是直接包裹真实头：
 *   - components/drivers/include/drivers.h（驱动层统一入口）
 *   - main/render/render.h（渲染层总接口）
 *   - profiles/lcd185b.h（minipet_profile_t / MINIPET_ACTIVE_PROFILE）
 *
 * 本层提供的小适配（mp_* 前缀）：
 *   - IMU mg → g 换算；PMU 电量/充电/温度便捷读法；RTC 结构透传
 *   - 触摸首点读取（touch_cst816 → touch_sample_t）
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
/* 【185B 2026-10-01 移植】与 216 的 mp_touch_read 同构，驱动换 CST816S
 * （touch_cst816.h：同款 touch_point_t，地址 0x15 / INT=GPIO4 / RST=GPIO1）。
 * 注意：185b 的 input 任务另有一条"直读 I2C 原始帧"的快路（×4/3 上采样到
 * 480 UI 空间），本适配供需要标准接口的调用方使用。 */
typedef struct {
    bool    touched;
    int16_t x, y;
} touch_sample_t;

static inline bool mp_touch_read(touch_sample_t *out)
{
    touch_point_t t;
    if (touch_cst816_read(&t) != ESP_OK) return false;
    out->touched = t.pressed;
    out->x = (int16_t)t.x;
    out->y = (int16_t)t.y;
    return true;
}

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
/* 【185B 2026-10-01 移植】216 是 AXP2101（0x34）；本板是 BQ27220（0x55）。
 * 接口同名同签名（见 pmu_bq27220.h），差别如实反映在驱动注释里：
 * vbus 无检测位（以 charging 近似）、无 PWRON 键、无 IRQ 脚。 */
static inline uint8_t mp_pmu_battery_pct(void)
{
    uint8_t pct = 100;
    pmu_bq27220_get_battery_pct(&pct);
    return pct;
}
static inline bool mp_pmu_charging(void)
{
    bool vbus = false, bat = false, charging = false;
    pmu_bq27220_get_power(&vbus, &bat, &charging);
    return charging;
}
static inline float mp_pmu_temp_c(void)
{
    float c = -1.0f;
    pmu_bq27220_get_temperature_c(&c);
    return c;
}

/* ============================ TF 适配 ============================= */
static inline bool mp_sd_mount(void)
{
    return sd_mount() == 0;      /* sd_tf.h：返回 errno，0=成功，挂载点 /sdcard */
}

/* ============================ codec 适配（帧数计数） =============== */
/* 【185B 音频链路 2026-10-01 移植】ES8311 + I2S TX（引脚走 profile
 * .pins.audio：MCLK=2/BCLK=48/LRCK=38/DOUT=47/PA=9，与官方 BSP 一致）。
 * 与 216 同款实现（codec_es8311.c），本板仅换了引脚来源（同样是 profile）。 */
static inline esp_err_t mp_codec_init(uint32_t rate_hz)
{
    return codec_es8311_init(rate_hz);   /* bgm_start 一次性调用（PA 默认关） */
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
