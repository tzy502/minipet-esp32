/**
 * @file pmu_axp2101.c
 * @brief AXP2101 PMU 驱动实现
 *
 * 寄存器口径 [核对]：AXP2101 寄存器手册未随 wiki 提供，以下地址/位定义
 * 按社区驱动（XPowersLib 等）常用口径抄录；init 时会打印原始值，
 * bring-up 首日用「插 USB /拔 USB」「充电中/满电」两三组状态对照修正掩码。
 *
 *   0x00 状态0：VBUS 在位 / 电池在位等
 *   0x01 状态1：充电状态机
 *   0xA4 电量百分比 0-100
 *
 * PWRON（底键）轮询口径 [核对]（XPowersLib XPowersAXP2101 寄存器表）：
 *   0x40..0x43 IRQ 使能 0..3（只门控 IRQ 引脚，不影响状态锁存）
 *   0x44..0x47 IRQ 状态 0..3（写 1 清除）
 *     0x44 bit0 = PKEY 短按释放（short press off）→ 短按完成标志
 *          bit1 = PKEY 长按释放；bit2 = PKEY 按下沿；bit3 = PKEY 释放沿
 */
#include "pmu_axp2101.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "amoled216.h"

static const char *TAG = "axp2101";

#define AXP2101_ADDR       0x34    /* 7-bit，固定 */
#define AXP2101_I2C_HZ     400000

#define AXP2101_REG_STATUS0  0x00  /* 供电在位状态 [位定义核对] */
#define AXP2101_REG_STATUS1  0x01  /* 充电状态机 [位定义核对] */
#define AXP2101_REG_BATT_PCT 0xA4  /* 电量百分比 0-100 [地址核对] */

/* IRQ 状态/使能组（地址按 XPowersLib AXP2101 口径 [核对]） */
#define AXP2101_REG_IRQ_STATUS0  0x44  /* IRQ 状态0，写 1 清除 [地址核对] */
#define AXP2101_IRQ0_PKEY_SHORT  (1 << 0)  /* PKEY 短按释放 [位定义核对] */
#define AXP2101_IRQ0_PKEY_PRESS  (1 << 2)  /* PKEY 按下沿   [位定义核对] */
#define AXP2101_IRQ0_PKEY_RELEAS (1 << 3)  /* PKEY 释放沿   [位定义核对] */

/*
 * 状态位（按 XPowersLib 常用口径，bring-up 对照修正）：
 *  - STATUS0 bit5 = VBUS 在位；bit2 = 电池在位
 *  - STATUS1 bit[5:3] 充电状态机：0b010~0b011 充电中，0b111 充电完成（零散，
 *    保守判定：state == 2 || state == 3）
 */
#define AXP2101_ST0_VBUS      (1 << 5)
#define AXP2101_ST0_BAT       (1 << 2)
#define AXP2101_ST1_CHG_STATE(_v) (((_v) >> 3) & 0x07)
#define AXP2101_CHG_CHARGING  0x02 /* 状态机值之一 [核对] */
#define AXP2101_CHG_CHARGING2 0x03 /* 状态机值之二（涓流/恒流段）[核对] */

static i2c_master_dev_handle_t s_dev;
static bool s_inited;
static void (*s_cb)(void *arg);
static void *s_cb_arg;

static void pmu_isr_handler(void *arg)
{
    (void)arg;
    if (s_cb) {
        s_cb(s_cb_arg);
    }
}

esp_err_t pmu_axp2101_init(void)
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

    err = i2c_bus_register_device("axp2101", AXP2101_ADDR, AXP2101_I2C_HZ, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    if (i2c_bus_probe(AXP2101_ADDR) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 @0x%02X 探测失败，电量/充电状态不可用", AXP2101_ADDR);
        return ESP_OK; /* 不阻断启动，app 降级 */
    }

    /* 打原始状态值，bring-up 对照位定义用（见文件头 [核对]） */
    uint8_t st0 = 0, st1 = 0, pct = 0, irq0 = 0;
    i2c_bus_read_reg8v(s_dev, AXP2101_REG_STATUS0, &st0, 1);
    i2c_bus_read_reg8v(s_dev, AXP2101_REG_STATUS1, &st1, 1);
    i2c_bus_read_reg8v(s_dev, AXP2101_REG_BATT_PCT, &pct, 1);
    i2c_bus_read_reg8v(s_dev, AXP2101_REG_IRQ_STATUS0, &irq0, 1);
    ESP_LOGI(TAG, "AXP2101 原始值 status0=0x%02X status1=0x%02X batt%%=%d irq0=0x%02X",
             st0, st1, pct, irq0);

    /* IRQ 引脚（可选）：profile 未配则跳过，低电走轮询 */
    const int8_t irq = pins->pmu.pmu_irq;
    if (irq >= 0) {
        gpio_config_t irq_cfg = {
            .pin_bit_mask = 1ULL << irq,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,   /* PMU IRQ 通常开漏 */
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&irq_cfg);
        gpio_install_isr_service(0); /* 已装过无碍 */
        gpio_isr_handler_add(irq, pmu_isr_handler, NULL);
        ESP_LOGI(TAG, "PMU IRQ 挂载 GPIO%d（下降沿）", irq);
    } else {
        ESP_LOGI(TAG, "PMU IRQ 未接线（profile=-1），低电检测走轮询");
    }

    s_inited = true;
    return ESP_OK;
}

esp_err_t pmu_axp2101_read_reg(uint8_t reg, uint8_t *val)
{
    if (!s_dev || !val) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_bus_read_reg8v(s_dev, reg, val, 1);
}

esp_err_t pmu_axp2101_write_reg(uint8_t reg, uint8_t val)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_bus_write_reg8(s_dev, reg, val);
}

esp_err_t pmu_axp2101_get_battery_pct(uint8_t *pct)
{
    if (!pct) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t v = 0;
    esp_err_t err = pmu_axp2101_read_reg(AXP2101_REG_BATT_PCT, &v);
    if (err != ESP_OK) {
        return err;
    }
    *pct = (v > 100) ? 100 : v; /* 个别批次满电读 0x64+，夹一下 */
    return ESP_OK;
}

esp_err_t pmu_axp2101_get_power(bool *vbus_present, bool *battery_present,
                                bool *charging)
{
    if (!vbus_present || !battery_present || !charging) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t st0 = 0, st1 = 0;
    esp_err_t err = pmu_axp2101_read_reg(AXP2101_REG_STATUS0, &st0);
    if (err != ESP_OK) {
        return err;
    }
    err = pmu_axp2101_read_reg(AXP2101_REG_STATUS1, &st1);
    if (err != ESP_OK) {
        return err;
    }

    *vbus_present    = (st0 & AXP2101_ST0_VBUS) != 0;
    *battery_present = (st0 & AXP2101_ST0_BAT) != 0;
    const uint8_t st = AXP2101_ST1_CHG_STATE(st1);
    *charging        = (st == AXP2101_CHG_CHARGING || st == AXP2101_CHG_CHARGING2);
    return ESP_OK;
}

esp_err_t pmu_axp2101_is_low_battery(bool *low)
{
    if (!low) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t pct = 0;
    bool charging = false, vbus = false, bat = false;
    esp_err_t err = pmu_axp2101_get_battery_pct(&pct);
    if (err != ESP_OK) {
        return err;
    }
    err = pmu_axp2101_get_power(&vbus, &bat, &charging);
    if (err != ESP_OK) {
        return err;
    }
    *low = (!charging) && (pct <= PMU_LOW_BATTERY_PCT);
    return ESP_OK;
}

esp_err_t pmu_axp2101_get_temperature_c(float *out_c)
{
    if (!out_c) {
        return ESP_ERR_INVALID_ARG;
    }
    /* AXP2101 TS/电池温度寄存器地址未核实（[核对] 项），暂不支持 */
    return ESP_ERR_NOT_SUPPORTED;
}

bool pmu_pwron_short_press(void)
{
    if (!s_dev) {
        return false;                    /* PMU 未就绪（init 失败/未探测到） */
    }
    uint8_t st = 0;
    if (pmu_axp2101_read_reg(AXP2101_REG_IRQ_STATUS0, &st) != ESP_OK) {
        return false;                    /* I2C 抖动：静默（巡检型接口） */
    }
    if (st == 0) {
        return false;
    }

    /* 按下沿/释放沿只打日志辅助 bring-up 核对位定义（写 1 清除） */
    if (st & (AXP2101_IRQ0_PKEY_PRESS | AXP2101_IRQ0_PKEY_RELEAS)) {
        ESP_LOGI(TAG, "PWRON %s%s",
                 (st & AXP2101_IRQ0_PKEY_PRESS) ? "按下沿 " : "",
                 (st & AXP2101_IRQ0_PKEY_RELEAS) ? "释放沿" : "");
    }

    const bool short_press = (st & AXP2101_IRQ0_PKEY_SHORT) != 0;
    /* 写 1 清掉已见的 PKEY 位（bit0/2/3）；其余位原样回写不清除 */
    const uint8_t clr = st & (AXP2101_IRQ0_PKEY_SHORT |
                              AXP2101_IRQ0_PKEY_PRESS |
                              AXP2101_IRQ0_PKEY_RELEAS);
    if (clr) {
        pmu_axp2101_write_reg(AXP2101_REG_IRQ_STATUS0, clr);
    }
    return short_press;                  /* 短按=按下沿+释放已完成，只报一次 */
}

void pmu_axp2101_set_isr_callback(void (*cb)(void *arg), void *arg)
{
    s_cb_arg = arg;
    s_cb = cb;
}
