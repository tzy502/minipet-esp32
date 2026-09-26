/**
 * @file pmu_axp2101.h
 * @brief AXP2101 电源管理驱动（共享 I2C）：电量 / 供电状态 / 低电中断
 *
 * 板上情况（重要）：Waveshare wiki GPIO 表未标注 AXP2101 的 IRQ 引脚
 * （GPIO16=SYS_OUT 用途存疑，可能是 PMU 中断也可能是系统电源控制）。
 * 因此 v1 低电检测走【轮询】：app 定期调 pmu_axp2101_get_power() +
 * pmu_axp2101_get_battery_pct()，低于阈值（如 15%）自行降亮度/告警。
 * 拿到原理图确认 IRQ 引脚后填 profile 的 pins.pmu.pmu_irq，
 * pmu_axp2101_set_isr_callback() 即自动生效。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 低电阈值（百分比）：低于此值 app 应降级（调低亮度/提示充电） */
#define PMU_LOW_BATTERY_PCT  15

/**
 * @brief 初始化：注册 I2C 器件 + 探测 + （若 profile 配了 IRQ 脚）中断挂载。
 *
 * 初始化时会把 0x00/0x01 状态寄存器原始值打进日志，方便 bring-up 对照
 * 位定义（见 .c 内 [核对] 注释）。
 */
esp_err_t pmu_axp2101_init(void);

/** @brief 读电池电量百分比 0-100（寄存器 0xA4 [核对]） */
esp_err_t pmu_axp2101_get_battery_pct(uint8_t *pct);

/**
 * @brief 读供电状态
 *
 * @param vbus_present     USB/适配器在位
 * @param battery_present  电池在位
 * @param charging         正在充电
 *
 * 位定义见 .c 内 [核对] 注释；bring-up 时以实测为准修正掩码。
 */
esp_err_t pmu_axp2101_get_power(bool *vbus_present, bool *battery_present,
                                bool *charging);

/** @brief 便捷接口：true = 电量低于 PMU_LOW_BATTERY_PCT 且未在充电 */
esp_err_t pmu_axp2101_is_low_battery(bool *low);

/**
 * @brief 电池温度（过温 -> hot 表情，E10）
 *
 * v1 返回 ESP_ERR_NOT_SUPPORTED：AXP2101 温度寄存器地址未随 wiki 提供、
 * 未核实，先让 app 走"无温度"降级分支；bring-up 核实后补实现。
 */
esp_err_t pmu_axp2101_get_temperature_c(float *out_c);

/** 直接寄存器读写（bring-up 调参 / 清中断标志用） */
esp_err_t pmu_axp2101_read_reg(uint8_t reg, uint8_t *val);
esp_err_t pmu_axp2101_write_reg(uint8_t reg, uint8_t val);

/**
 * @brief PMU 中断回调（中断上下文！）
 *
 * 仅当 profile 的 pins.pmu.pmu_irq >= 0 时有效；本板默认未接（-1），
 * 调用无副作用。触发后应读状态寄存器并写 1 清标志（用上面裸读写 API）。
 */
void pmu_axp2101_set_isr_callback(void (*cb)(void *arg), void *arg);

/**
 * @brief 查询 PWRON（PKEY）是否完成了一次【短按】（轮询，本板 IRQ 引脚未接线）
 *
 * 实现：读 IRQ 状态寄存器 0x44 的 bit0（PKEY 短按释放标志，XPowersLib 口径
 * [核对]），置位即代表 PMU 硬件已捕获「按下沿+释放」的完整短按；读后写 1
 * 清除，因此一次短按本接口只返回一次 true。状态位锁存与 IRQ 使能无关
 * （使能位只门控 IRQ 引脚），未使能中断也可轮询。
 *
 * 注意：PWRON 短按不会导致关机（长按 >6s 才是硬关机，寄存器默认值），
 * 运行期可安全用作底键输入。调用周期建议 100ms 级（事件有锁存，慢轮询不丢）。
 *
 * @return true = 捕获到一次新短按（再调返回 false，直到下一次短按）
 */
bool pmu_pwron_short_press(void);

#ifdef __cplusplus
}
#endif
