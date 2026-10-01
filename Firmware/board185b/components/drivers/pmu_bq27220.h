/**
 * @file pmu_bq27220.h
 * @brief BQ27220 电量计驱动（共享 I2C，纯轮询）—— 185B 的 PMU 等价物
 *
 * 【为什么是 BQ27220】1.85B 板没有 216 的 AXP2101 电源管理芯片：它的电量计是
 * TI **BQ27220**（I2C 7-bit 0x55，与 IMU/RTC/触摸/ES8311 共用 SCL=10/SDA=11）。
 * 依据：官方 BSP 的依赖声明
 * `waveshare__esp32_s3_touch_lcd_1_85B/idf_component.yml` 里
 * `espressif/bq27220: ^0.1.1`；本板 profile 注释亦记 "电量计是 BQ27220@0x55"。
 * 官方 BSP 只声明依赖、**代码里从未调用**，故本驱动按标准命令自写（不引入
 * espressif/bq27220 组件——那个组件的 create 会 unseal + 回读/改写 gauge 数据
 * 内存并复位 gauge，参数还要猜电池容量；我们只需要读，风险最低的路径是直读
 * 标准命令字）。
 *
 * 【只读纪律】本驱动**只读标准命令**（0x06/0x08/0x0A/0x0C/0x2C），
 * 不 unseal、不写数据内存、不发控制子命令——不可能改坏 gauge 配置。
 *
 * 【接口口径】与 216 的 pmu_axp2101.h **同名同签名**（get_battery_pct /
 * get_power / is_low_battery / get_temperature_c / read_reg / write_reg），
 * 便于 app 层（hal_contract.h 的 mp_pmu_*、input_dispatch 的电池巡检）零改动接入。
 * 差异（本板硬件如实反映）：
 *   · **无 PWRON 键**：BQ27220 无按键输入引脚 → pmu_pwron_short_press()/
 *     pmu_pwron_long_press() 恒返回 false（保留签名供 app 层单键降级使用）；
 *   · **无 IRQ 脚**：BSP 未引出 ALERT → set_isr_callback() 空操作；
 *   · **vbus 无对应位**：电量计不测 USB 在位 → *vbus_present 以 charging 近似，
 *     注释如实标注（充电判定用 Current>0）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 低电阈值（百分比）：低于此值 app 应降级（提示充电/进待机时钟） */
#ifndef PMU_LOW_BATTERY_PCT
#define PMU_LOW_BATTERY_PCT  15
#endif

/**
 * @brief 初始化：注册 I2C 器件 + 探测 + 打原始关键寄存器值（bring-up 取证）。
 *
 * 探测失败只告警不阻断（返回 ESP_OK）——电量计缺失不应导致整机不可用，
 * 与 touch/imu 的降级口径一致（app 层按"读不到就跳过"处理）。
 * 幂等：已初始化直接返回 ESP_OK。
 */
esp_err_t pmu_bq27220_init(void);

/** @brief 读电池剩余电量百分比 0-100（标准命令 StateOfCharge = 0x2C，u16 小端） */
esp_err_t pmu_bq27220_get_battery_pct(uint8_t *pct);

/**
 * @brief 读供电状态
 *
 * @param vbus_present     USB/适配器在位（**近似**：电量计不测 VBUS，取 charging）
 * @param battery_present  电池在位（BatteryStatus 0x0A bit3 BATTPRES）
 * @param charging         正在充电（Current 0x0C > 0，i16 小端，正=充入）
 */
esp_err_t pmu_bq27220_get_power(bool *vbus_present, bool *battery_present,
                                bool *charging);

/** @brief 便捷接口：true = 电量低于 PMU_LOW_BATTERY_PCT 且未在充电 */
esp_err_t pmu_bq27220_is_low_battery(bool *low);

/** @brief 电池温度（℃）：标准命令 Temperature 0x06（0.1°K）→ ℃ 换算 */
esp_err_t pmu_bq27220_get_temperature_c(float *out_c);

/** 电池电压 mV（标准命令 Voltage 0x08；诊断/上报扩展用） */
esp_err_t pmu_bq27220_get_voltage_mv(uint16_t *mv);

/** 直接标准命令读（16bit 小端；bring-up 取证用） */
esp_err_t pmu_bq27220_read_cmd16(uint8_t cmd, uint16_t *val);

/** 器件是否在线（init 探测结果；false = 后续读数全部不可信） */
bool pmu_bq27220_online(void);

/* ---------------- 按键口径占位（本板无 PMU 键） ----------------
 * 216 的 AXP2101 有 PWRON 底键；BQ27220 是纯电量计、无任何按键输入引脚
 * （官方 BSP 的按键表只有 GPIO0 一个脚）。保留同名签名是为了让 app 层的
 * 单键降级路径（pwron_tick）无需条件编译即可编译链接，语义恒为"没有按键"。 */
static inline bool pmu_pwron_short_press(void) { return false; }
static inline bool pmu_pwron_long_press(void)  { return false; }

#ifdef __cplusplus
}
#endif
