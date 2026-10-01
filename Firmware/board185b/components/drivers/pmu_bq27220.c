/**
 * @file pmu_bq27220.c
 * @brief BQ27220 电量计实现（共享 I2C 标准命令直读，只读不写配置）
 *
 * 寄存器口径来源：espressif__bq27220 v0.1.2 的 priv_include/bq27220_reg.h
 * （本地 BSP 依赖树 /Users/a502/mp-work/ws_official/managed_components/）与
 * TI BQ27220 标准命令表。**未在 185B 真机实测**——首次上电请看 init 日志里
 * 打出的 SOC/电压/电流原始值是否合理（SOC 0-100、电压 3000-4300mV）。
 *
 * 协议：所有标准命令都是「写 1 字节命令 → 读 N 字节」，多字节均为**小端**。
 * 本驱动只用 i2c_bus_read_reg8v()（写命令+重复起始+读，与 BQ27220 要求一致）。
 *
 * 【只读纪律】不 unseal（0x0414/0x3672）、不发 CONTROL 子命令、不写数据内存。
 */
#include "pmu_bq27220.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "lcd185b.h"

static const char *TAG = "bq27220";

#define BQ27220_ADDR        0x55    /* 7-bit，固定（组件源码 bq27220.c:20 同值） */
#define BQ27220_I2C_HZ      400000  /* 同总线其它器件档位 */

/* 标准命令（小端 16bit；地址见 bq27220_reg.h） */
#define BQ_CMD_TEMPERATURE  0x06    /* 0.1 °K */
#define BQ_CMD_VOLTAGE      0x08    /* mV */
#define BQ_CMD_BATT_STATUS  0x0A    /* u16 位域：bit0 DSG / bit3 BATTPRES / bit9 FC */
#define BQ_CMD_CURRENT      0x0C    /* i16 mA，正 = 充入电池 */
#define BQ_CMD_SOC          0x2C    /* %（0-100） */

#define BQ_STATUS_DSG       (1u << 0)
#define BQ_STATUS_BATTPRES  (1u << 3)
#define BQ_STATUS_FC        (1u << 9)

static i2c_master_dev_handle_t s_dev;
static bool s_online;

/** 读 16bit 标准命令（小端） */
esp_err_t pmu_bq27220_read_cmd16(uint8_t cmd, uint16_t *val)
{
    if (!s_dev || !val) return ESP_ERR_INVALID_STATE;
    uint8_t b[2] = { 0, 0 };
    esp_err_t err = i2c_bus_read_reg8v(s_dev, cmd, b, sizeof(b));
    if (err != ESP_OK) return err;
    *val = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return ESP_OK;
}

esp_err_t pmu_bq27220_init(void)
{
    if (s_dev) return ESP_OK;                       /* 幂等 */

    const minipet_pins_t *pins = &MINIPET_ACTIVE_PROFILE.pins;
    (void)pins;                                     /* BQ27220 无独立引脚（无 IRQ/无按键） */

    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) return err;

    err = i2c_bus_register_device("bq27220", BQ27220_ADDR, BQ27220_I2C_HZ, &s_dev);
    if (err != ESP_OK) return err;

    if (i2c_bus_probe(BQ27220_ADDR) != ESP_OK) {
        /* 降级：探测不到只告警（电量计缺失不该让整机不可用） */
        ESP_LOGW(TAG, "BQ27220 @0x%02X 探测失败，电量/充电不可用（降级）", BQ27220_ADDR);
        s_online = false;
        return ESP_OK;
    }
    s_online = true;

    /* bring-up 取证：把关键标准命令原始值一次打全（判在线 + 判口径） */
    uint16_t soc = 0, mv = 0, st = 0, cur = 0, tk = 0;
    pmu_bq27220_read_cmd16(BQ_CMD_SOC, &soc);
    pmu_bq27220_read_cmd16(BQ_CMD_VOLTAGE, &mv);
    pmu_bq27220_read_cmd16(BQ_CMD_BATT_STATUS, &st);
    pmu_bq27220_read_cmd16(BQ_CMD_CURRENT, &cur);
    pmu_bq27220_read_cmd16(BQ_CMD_TEMPERATURE, &tk);
    ESP_LOGI(TAG, "BQ27220 就绪 SOC=%u%% 电压=%umV 电流=%dmA 状态=0x%04X"
                  "（DSG=%d BATTPRES=%d FC=%d）温度=%.1f℃",
             (unsigned)soc, (unsigned)mv, (int)(int16_t)cur, (unsigned)st,
             (st & BQ_STATUS_DSG) ? 1 : 0, (st & BQ_STATUS_BATTPRES) ? 1 : 0,
             (st & BQ_STATUS_FC) ? 1 : 0,
             (tk == 0) ? -273.15f : ((float)tk / 10.0f - 273.15f));
    return ESP_OK;
}

esp_err_t pmu_bq27220_get_battery_pct(uint8_t *pct)
{
    if (!pct) return ESP_ERR_INVALID_ARG;
    if (!s_online) return ESP_ERR_INVALID_STATE;
    uint16_t v = 0;
    esp_err_t err = pmu_bq27220_read_cmd16(BQ_CMD_SOC, &v);
    if (err != ESP_OK) return err;
    if (v > 100) v = 100;                    /* 未校准 gauge 可能越界，夹取 */
    *pct = (uint8_t)v;
    return ESP_OK;
}

esp_err_t pmu_bq27220_get_voltage_mv(uint16_t *mv)
{
    if (!mv) return ESP_ERR_INVALID_ARG;
    if (!s_online) return ESP_ERR_INVALID_STATE;
    return pmu_bq27220_read_cmd16(BQ_CMD_VOLTAGE, mv);
}

esp_err_t pmu_bq27220_get_power(bool *vbus_present, bool *battery_present,
                                bool *charging)
{
    if (!s_online) return ESP_ERR_INVALID_STATE;
    uint16_t st = 0, cur = 0;
    esp_err_t err = pmu_bq27220_read_cmd16(BQ_CMD_BATT_STATUS, &st);
    if (err != ESP_OK) return err;
    err = pmu_bq27220_read_cmd16(BQ_CMD_CURRENT, &cur);
    if (err != ESP_OK) return err;

    /* 充电判据：电流为正（充入电池）且电池在位且不在放电态。
     * 【口径说明】电量计不测 USB VBUS → vbus 只能以 charging 近似（如实标注，
     * 与 216 的 AXP2101 语义在此项上不等价）。 */
    bool bat = (st & BQ_STATUS_BATTPRES) != 0;
    bool chg = ((int16_t)cur > 0) && bat && !(st & BQ_STATUS_DSG);

    if (battery_present) *battery_present = bat;
    if (charging)        *charging = chg;
    if (vbus_present)    *vbus_present = chg;      /* 近似值（无 VBUS 检测位） */
    return ESP_OK;
}

esp_err_t pmu_bq27220_is_low_battery(bool *low)
{
    if (!low) return ESP_ERR_INVALID_ARG;
    uint8_t pct = 100;
    esp_err_t err = pmu_bq27220_get_battery_pct(&pct);
    if (err != ESP_OK) return err;
    bool chg = false;
    pmu_bq27220_get_power(NULL, NULL, &chg);
    *low = (!chg && pct <= PMU_LOW_BATTERY_PCT);
    return ESP_OK;
}

esp_err_t pmu_bq27220_get_temperature_c(float *out_c)
{
    if (!out_c) return ESP_ERR_INVALID_ARG;
    if (!s_online) return ESP_ERR_INVALID_STATE;
    uint16_t tk = 0;
    esp_err_t err = pmu_bq27220_read_cmd16(BQ_CMD_TEMPERATURE, &tk);
    if (err != ESP_OK) return err;
    if (tk == 0) return ESP_ERR_INVALID_STATE;    /* 0 = 未出数（非 0°K 现实值） */
    *out_c = (float)tk / 10.0f - 273.15f;
    return ESP_OK;
}

bool pmu_bq27220_online(void)
{
    return s_online;
}
