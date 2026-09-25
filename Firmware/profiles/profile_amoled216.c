/**
 * @file profile_amoled216.c
 * @brief Waveshare ESP32-S3-Touch-AMOLED-2.16（SKU 33969）profile 实例
 *
 * 引脚值逐条对应 docs/ai/waveshare-wiki-ESP32-S3-Touch-AMOLED-2.16.md「GPIO 引脚分配」。
 * 修改引脚只改这里，驱动层自动跟随。
 */
#include "amoled216.h"

const minipet_profile_t MINIPET_PROFILE_AMOLED216 = {
    .width  = 480,
    .height = 480,
    /* 官方产品图为圆形 AMOLED（2.16" 480x480 圆屏）；如实测为方形改 MINIPET_SHAPE_SQUARE */
    .shape  = MINIPET_SHAPE_ROUND,
    .shape_name = "round", /* 官方产品图为圆形 AMOLED；如实测方形改 SQUARE/"square" */
    .psram_mb = 8,       /* ESP32-S3R8 叠封 Octal PSRAM */
    .flash_mb = 16,      /* 板载 16MB NOR Flash */
    .cpu_freq_mhz = 240, /* 双核 240MHz（sdkconfig.defaults.amoled216 同步声明） */
    .has_audio = true,   /* ES8311 DAC + 功放（PA_CTRL=GPIO46） */
    .has_touch = true,   /* CST9220 */
    .has_imu   = true,   /* QMI8658 */
    .has_rtc   = true,   /* PCF85063ATL */
    .has_pmu   = true,   /* AXP2101 */
    .has_sd    = true,   /* microSD（SPI + FATFS） */
    .has_key   = true,   /* Key3 = GPIO18 */
    .pins = {
        .i2c  = { .scl = 14, .sda = 15 }, /* 五器件共享：CST9220/AXP2101/QMI8658/PCF85063/ES8311 */
        .lcd  = { .sio0 = 4, .sio1 = 5, .sio2 = 6, .sio3 = 7,
                  .sclk = 38, .cs = 12, .rst = 39 },
        .touch = { .intr = 11, .rst = 40 },
        .imu   = { .int1 = 17, .int2 = 21 },
        .rtc   = { .intr = 13 },       /* PCF85063 INT：当前仅配置为输入，闹钟功能后续按需开 */
        .audio = { .mclk = 42, .bclk = 9, .lrck = 45,
                   .dsdin = 8, .asdout = 10, .pa_en = 46 },
        .sd    = { .mosi = 1, .miso = 3, .sclk = 2, .cs = 41 },
        .key   = { .menu = 18 },
        /* 原理图未明确标注 AXP2101 IRQ 接到哪个 GPIO（GPIO16=SYS_OUT 用途存疑）。
         * 低电检测默认走轮询（pmu_axp2101_get_power/get_battery_pct）；
         * 拿到原理图确认后再把实际引脚填进来，pmu_axp2101_set_isr_callback() 即可用。 */
        .pmu   = { .pmu_irq = -1 },
    },
};
