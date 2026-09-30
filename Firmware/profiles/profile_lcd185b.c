/**
 * @file profile_lcd185b.c
 * @brief 设备 profile：Waveshare ESP32-S3-Touch-LCD-1.85B（1.85" 圆 360×360 LCD）
 *
 * 引脚抄自官方 wiki GPIO 表（docs.waveshare.net/ESP32-S3-Touch-LCD-1.85B）：
 *   LCD(ST77916 QSPI)：CS=21 PCLK=40 D0-3=46/45/42/41 RST=3 BL=5
 *   共享 I2C(六器件)：SCL=10 SDA=11 —— 与 2.16 板(14/15)不同！
 *   TF 卡：SDMMC CLK=15 CMD=14 D0-3=16/17/12/13（sd_tf 走 1-bit：CLK/CMD/D0）
 *   按键：仅 BOOT=GPIO0（key_gpio0 中键复用）；无 GPIO18 菜单键
 *
 * 能力差异（对 2.16 板）：
 *   - 触摸是 CST816S@0x15，帧格式与 CST9220 不同 → 驱动未接，has_touch=false
 *   - IMU QMI8658@0x6B 在线，但 INT 脚 wiki 未标注 → -1，走轮询（20ms 兜底读）
 *   - RTC PCF85063@0x51 在线（INT=6）；电量计是 BQ27220@0x55 非 AXP2101 → has_pmu=false
 *   - 背光 = BL GPIO5（LCD）；AMOLED 的 0x51 DBV 命令在此板上无效
 */
#include "amoled216.h"

const minipet_profile_t MINIPET_PROFILE_LCD185B = {
    .width  = 360,
    .height = 360,
    .shape  = MINIPET_SHAPE_ROUND,
    .shape_name = "round",
    .psram_mb  = 8,
    .flash_mb  = 16,
    .cpu_freq_mhz = 240,

    .has_audio = false,   /* 【降级 2026-09-29】ES8311 未验证；省 28KB 内部栈给 RAMless 刷新。音频验证后翻回 true */
    .has_touch = false,   /* CST816S@0x15：驱动未接（CST9220 驱动探测 0x5A 失败自动降级） */
    .has_imu   = true,    /* QMI8658@0x6B：INT 未引出 → int1/int2=-1，轮询模式 */
    .has_rtc   = true,    /* PCF85063@0x51，INT=6 */
    .has_pmu   = false,   /* BQ27220@0x55 电量计，非 AXP2101（驱动未接，自动降级） */
    .has_sd    = true,    /* SDMMC 槽在线（1-bit：CLK15/CMD14/D0=16），cs 未用=-1 */
    .has_key   = false,   /* 无 GPIO18 菜单键；BOOT=GPIO0 由 key_gpio0 中键复用 */

    .pins = {
        .i2c  = { .scl = 10, .sda = 11 },
        .lcd  = { .sio0 = 46, .sio1 = 45, .sio2 = 42, .sio3 = 41,
                  .sclk = 40, .cs = 21, .rst = 3 },
        .touch = { .intr = 4, .rst = 1 },   /* CST816S（占位，驱动未接） */
        .imu  = { .int1 = -1, .int2 = -1 }, /* INT 脚未标注 → 轮询模式 */
        .rtc  = { .intr = 6 },
        .audio = { .mclk = 2, .bclk = 48, .lrck = 38,
                   .dsdin = 47, .asdout = 39, .pa_en = 9 },
        .sd   = { .mosi = 14, .miso = 16, .sclk = 15, .cs = -1 }, /* SDMMC CMD/D0/CLK */
        .key  = { .menu = -1 },             /* 无 GPIO18 菜单键 */
        .pmu  = { .pmu_irq = -1 },
    },
};
