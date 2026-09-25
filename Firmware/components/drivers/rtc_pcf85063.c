/**
 * @file rtc_pcf85063.c
 * @brief PCF85063 RTC 驱动实现（BCD 读写）
 *
 * 寄存器布局（PCF85063 数据手册，无 [核对] 项，寄存器表公开且稳定）：
 *   0x00 Control_1（bit5=STOP）
 *   0x01 Control_2
 *   0x02 Offset / 0x03 RAM
 *   0x04 Seconds（bit7=OS 振荡器停止标志）
 *   0x05 Minutes / 0x06 Hours（24h 模式） / 0x07 Days / 0x08 Weekdays
 *   0x09 Months / 0x0A Years（00-99，基准 2000）
 */
#include "rtc_pcf85063.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "amoled216.h"

static const char *TAG = "pcf85063";

#define PCF85063_ADDR      0x51    /* 7-bit，固定 */
#define PCF85063_I2C_HZ    400000

#define PCF85063_REG_CTRL1     0x00
#define PCF85063_REG_CTRL2     0x01
#define PCF85063_REG_SECONDS   0x04
#define PCF85063_REG_MINUTES   0x05
#define PCF85063_REG_HOURS     0x06
#define PCF85063_REG_DAYS      0x07
#define PCF85063_REG_WEEKDAYS  0x08
#define PCF85063_REG_MONTHS    0x09
#define PCF85063_REG_YEARS     0x0A

#define PCF85063_CTRL1_STOP    (1 << 5)   /* 置 1 停振，安全写时间窗 */
#define PCF85063_SECONDS_OS    (1 << 7)   /* 振荡器停止/时间无效标志 */

/* 时间寄存器连续 7 字节：秒 分 时 日 星期 月 年 */
#define PCF85063_TIME_LEN  7

static i2c_master_dev_handle_t s_dev;
static bool s_inited;

/* ---------- BCD 辅助 ---------- */

static inline uint8_t bcd2bin(uint8_t v)
{
    return (uint8_t)((v & 0x0F) + ((v >> 4) & 0x0F) * 10);
}

static inline uint8_t bin2bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* ---------- 公共 API ---------- */

esp_err_t rtc_pcf85063_init(void)
{
    const minipet_pins_t *pins = &MINIPET_PROFILE_AMOLED216.pins;
    esp_err_t err;

    if (s_inited) {
        return ESP_OK;
    }
    err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_bus_register_device("pcf85063", PCF85063_ADDR, PCF85063_I2C_HZ, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    /* 探测：RTC 不在线不阻断启动（app 降级用系统 tick 走时） */
    err = i2c_bus_probe(PCF85063_ADDR);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 @0x%02X 探测失败: %s",
                 PCF85063_ADDR, esp_err_to_name(err));
        return ESP_OK;
    }

    /* INT 引脚：开漏低有效，先配输入；闹钟/秒脉冲中断后续按需开 */
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << pins->rtc.intr,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);

    /* Control_1 清 STOP、Control_2 清中断标志，进入正常走时 */
    i2c_bus_write_reg8(s_dev, PCF85063_REG_CTRL1, 0x00);
    i2c_bus_write_reg8(s_dev, PCF85063_REG_CTRL2, 0x00);

    uint8_t sec = 0;
    i2c_bus_read_reg8v(s_dev, PCF85063_REG_SECONDS, &sec, 1);
    if (sec & PCF85063_SECONDS_OS) {
        ESP_LOGW(TAG, "振荡器停止标志置位（电池曾失效/首次上电），等待联网校时");
    }

    s_inited = true;
    ESP_LOGI(TAG, "PCF85063 就绪 INT=%d", pins->rtc.intr);
    return ESP_OK;
}

bool rtc_pcf85063_osc_ok(void)
{
    if (!s_dev) {
        return false;
    }
    uint8_t sec = 0;
    if (i2c_bus_read_reg8v(s_dev, PCF85063_REG_SECONDS, &sec, 1) != ESP_OK) {
        return false;
    }
    return (sec & PCF85063_SECONDS_OS) == 0;
}

esp_err_t rtc_pcf85063_get_time(struct tm *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t d[PCF85063_TIME_LEN];
    esp_err_t err = i2c_bus_read_reg8v(s_dev, PCF85063_REG_SECONDS, d, sizeof(d));
    if (err != ESP_OK) {
        return err;
    }
    if (d[0] & PCF85063_SECONDS_OS) {
        /* 时间无效：强制调用方先校时，避免拿 2000-00-00 走时钟 */
        return ESP_ERR_INVALID_STATE;
    }

    out->tm_sec  = bcd2bin(d[0] & 0x7F);
    out->tm_min  = bcd2bin(d[1] & 0x7F);
    out->tm_hour = bcd2bin(d[2] & 0x3F);      /* 24h 模式 */
    out->tm_mday = bcd2bin(d[3] & 0x3F);
    out->tm_wday = bcd2bin(d[4] & 0x07);
    out->tm_mon  = bcd2bin(d[5] & 0x1F) - 1;  /* 1-12 -> 0-11 */
    out->tm_year = bcd2bin(d[6]) + 100;       /* 00-99 -> 2000 基准：tm_year=year-1900 */
    out->tm_yday = 0;
    out->tm_isdst = 0;
    return ESP_OK;
}

/** STOP 包裹写入 7 个时间寄存器 + 回读校验（set_from_epoch/set_time 共用） */
static esp_err_t rtc_write_tm(const struct tm *tm)
{
    uint8_t d[PCF85063_TIME_LEN];
    d[0] = bin2bcd((uint8_t)tm->tm_sec);
    d[1] = bin2bcd((uint8_t)tm->tm_min);
    d[2] = bin2bcd((uint8_t)tm->tm_hour);
    d[3] = bin2bcd((uint8_t)tm->tm_mday);
    d[4] = bin2bcd((uint8_t)tm->tm_wday);
    d[5] = bin2bcd((uint8_t)(tm->tm_mon + 1));
    d[6] = bin2bcd((uint8_t)(tm->tm_year - 100));
    /* 注：写秒寄存器时 OS 位(bit7)写 0，写入即清除停止标志 */

    /* STOP 包裹：停振 -> 写 7 寄存器 -> 走时，保证不出现半新半旧时间 */
    esp_err_t err = i2c_bus_write_reg8(s_dev, PCF85063_REG_CTRL1, PCF85063_CTRL1_STOP);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_bus_write_reg8v(s_dev, PCF85063_REG_SECONDS, d, sizeof(d));
    if (err != ESP_OK) {
        /* 失败也要恢复走时，别把 RTC 停在 STOP */
        i2c_bus_write_reg8(s_dev, PCF85063_REG_CTRL1, 0x00);
        return err;
    }
    err = i2c_bus_write_reg8(s_dev, PCF85063_REG_CTRL1, 0x00);

    /* 回读校验：秒寄存器 OS 位必须为 0 */
    uint8_t sec = 0xFF;
    if (i2c_bus_read_reg8v(s_dev, PCF85063_REG_SECONDS, &sec, 1) == ESP_OK &&
        (sec & PCF85063_SECONDS_OS)) {
        ESP_LOGW(TAG, "校时后 OS 仍置位，时间可能无效");
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

esp_err_t rtc_pcf85063_set_time(const struct tm *t)
{
    if (!t) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return rtc_write_tm(t);
}

esp_err_t rtc_pcf85063_set_from_epoch(int64_t unix_epoch)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    time_t t = (time_t)unix_epoch;
    struct tm tm;
    gmtime_r(&t, &tm); /* UTC 口径，见头文件说明 */
    return rtc_write_tm(&tm);
}
