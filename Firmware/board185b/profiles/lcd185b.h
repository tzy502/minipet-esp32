/**
 * @file lcd185b.h
 * @brief 设备 profile：Waveshare ESP32-S3-Touch-LCD-1.85B（单板分支唯一板型）
 *
 * 引脚唯一来源（single source of truth）：
 *  - 所有 BSP 驱动（components/drivers/）从本实例取引脚号，不各自硬编码；
 *  - 引脚值抄自官方 wiki 的 GPIO 表（profile_lcd185b.c 顶部有逐项注明）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 屏幕物理形状（影响合成器的安全绘制区域 / 圆形裁剪） */
typedef enum {
    MINIPET_SHAPE_ROUND = 0,   /**< 圆形屏 */
    MINIPET_SHAPE_SQUARE = 1,  /**< 方形/矩形屏 */
} minipet_shape_t;

/** 引脚表。-1 = 该板未引出/未使用 */
typedef struct {
    struct { int8_t scl; int8_t sda; } i2c;              /**< 共享 I2C 总线 */
    struct { int8_t sio0; int8_t sio1; int8_t sio2; int8_t sio3; /**< QSPI 数据 0-3 */
             int8_t sclk; int8_t cs; int8_t rst; } lcd;  /**< ST77916 QSPI + CS + 复位 */
    struct { int8_t intr; int8_t rst; } touch;           /**< 触摸（本板驱动未接） */
    struct { int8_t int1; int8_t int2; } imu;            /**< QMI8658 INT1/INT2 */
    struct { int8_t intr; } rtc;                         /**< PCF85063 INT */
    struct { int8_t mclk; int8_t bclk; int8_t lrck;      /**< I2S 时钟/帧 */
             int8_t dsdin; int8_t asdout;                /**< 播放数据 / 录音数据 */
             int8_t pa_en; } audio;                      /**< PA_CTRL 功放使能 */
    struct { int8_t mosi; int8_t miso; int8_t sclk; int8_t cs; } sd; /**< TF 卡 (SDMMC) */
    struct { int8_t menu; } key;                         /**< 菜单键脚（本板恒 -1，用 BOOT） */
    struct { int8_t pmu_irq; } pmu;                      /**< PMU 中断（本板无 PMU） */
} minipet_pins_t;

/** 设备能力与硬件参数 */
typedef struct {
    uint16_t width;          /**< 屏幕像素宽 */
    uint16_t height;         /**< 屏幕像素高 */
    minipet_shape_t shape;   /**< 屏幕形状（枚举） */
    const char *shape_name;  /**< 形状名字符串："round"/"square"（hello 上报用） */
    uint32_t psram_mb;       /**< PSRAM 容量 MB */
    uint16_t flash_mb;       /**< Flash 容量 MB */
    uint16_t cpu_freq_mhz;   /**< CPU 主频 */
    bool     has_audio;      /**< 扬声器播放链路（本板 false，BGM 降级） */
    bool     has_touch;      /**< 电容触摸（本板 false，驱动未接） */
    bool     has_imu;        /**< 六轴 IMU（QMI8658） */
    bool     has_rtc;        /**< RTC（PCF85063，走时/待机时钟） */
    bool     has_pmu;        /**< 电源管理（本板 false，电量计未接） */
    bool     has_sd;         /**< TF 卡（FATFS 素材缓存） */
    bool     has_key;        /**< 独立菜单键脚（本板 false；菜单键=BOOT GPIO0） */
    int16_t  ground_cam_shift_px; /**< 相机下移实验（真 foothold 层入镜，0=关）：
                                   *   本板（lcd185b）=248 实验 */
    /* ══ 【世界→屏 缩放系数上报 2026-10-01】══════════════════════════════════════
     * 语义 = 合成器的 RC_SCALE（世界 1x 像素 → 合成器空间像素），**设备相机窗口的
     * 权威口径**：可见窗口(世界px) = width / world_scale。
     * 为什么必须上报：服务端原先硬编码 `PlacementMath.Scale = 2`（= 480 屏/2 = 240 窗口），
     * 但 1.85B 做 A1（合成器 480→360、RC_SCALE 2→1）后窗口变成 **360**，
     * 服务端仍按 240 算 → 机位夹取范围错（1560 vs 实际 148）、预览窗口错、
     * 「设备视角」图与设备真实所见不一致（用户报障："摄像机可以展示的地图大了
     * 但是对应服务端推送的没修改"）。此处上报后，服务端按 `width/rcScale` 算窗口，
     * 两端口径自动一致（216 上报 2 保持原行为）。
     * 缺省/0 由服务端按 2 兜底（旧固件不上报时行为不变）。 */
    uint8_t  world_scale;
    minipet_pins_t pins;     /**< 完整引脚表 */
} minipet_profile_t;

/* ── 单板分支（185B 专用）────────────────────────────────────────────
 * MINIPET_ACTIVE_PROFILE 是全工程引用当前板 profile 的唯一入口——
 * 单板分支下直接展开为 MINIPET_PROFILE_LCD185B。 */
/** LCD-1.85B 板 profile 实例（定义在 profile_lcd185b.c） */
extern const minipet_profile_t MINIPET_PROFILE_LCD185B;
#define MINIPET_ACTIVE_PROFILE MINIPET_PROFILE_LCD185B

#ifdef __cplusplus
}
#endif
