/**
 * hal_contract.h — 应用层对 BSP/渲染/profile 的统一消费面（适配层）
 *
 * 并行模块已落地，本文件不再「平行声明」而是直接包裹真实头：
 *   - components/drivers/include/drivers.h（驱动层统一入口）
 *   - main/render/render.h（渲染层总接口）
 *   - profiles/lcd185b.h（minipet_profile_t / MINIPET_ACTIVE_PROFILE）
 *
 * 本层提供的小适配（mp_* 前缀）：
 *   - IMU mg → g 换算；RTC 结构透传
 *   - codec 写接口按「立体声帧数」计数（bgm 不关心字节）——本板
 *     has_audio=false，BGM/feeder 不启动，这些入口落地为无操作直路，
 *     仅维持 bgm.c 编译链接（与原降级运行语义等价）
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

/* ============================ TF 适配 ============================= */
static inline bool mp_sd_mount(void)
{
    return sd_mount() == 0;      /* sd_tf.h：返回 errno，0=成功，挂载点 /sdcard */
}

/* ============================ codec 适配（帧数计数） =============== */
/* 【185B 单板直路】has_audio=false → BGM/feeder 不启动，以下入口运行期
 * 不可达；落地为无操作保持 bgm.c 调用点可编译可链接，运行语义与原
 * 降级行为零变化（codec 硬件链路本板不存在）。 */
static inline void mp_codec_start(uint32_t rate_hz)
{
    (void)rate_hz;
}
static inline void mp_codec_set_rate(uint32_t rate_hz)
{
    (void)rate_hz;
}
/* interleaved 16bit 立体声，frame_count 帧 = 4×frame_count 字节 */
static inline esp_err_t mp_codec_write(const int16_t *stereo_frames, size_t frame_count)
{
    (void)stereo_frames; (void)frame_count;
    return ESP_OK;
}
static inline void mp_pa_enable(bool on)
{
    (void)on;
}

#ifdef __cplusplus
}
#endif

#endif /* MP_HAL_CONTRACT_H */
