/**
 * @file i2c_bus.h
 * @brief 板载共享 I2C 总线（GPIO10=SCL / GPIO11=SDA，器件复用）
 *
 * 板上挂在同一条总线上的器件（7-bit 地址）：
 *   0x6B QMI8658 IMU       0x51 PCF85063 RTC
 *   （0x15 CST816S 触摸 / 0x55 BQ27220 电量计本板驱动未接）
 *
 * 使用 ESP-IDF 5.x i2c_master 新 API（driver/i2c_master.h）。
 * 所有驱动必须经 i2c_bus_register_device() 注册后拿 handle 使用，
 * 不得直接 i2c_new_master_bus()（总线只有一个实例）。
 *
 * 线程安全：总线级互斥锁串行化所有事务；各驱动可在任意任务上下文调用。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 注册表容量（板上 5 器件 + 余量） */
#define I2C_BUS_MAX_DEVICES 8

/**
 * @brief 初始化共享 I2C 总线（幂等，重复调用返回 ESP_OK）
 *
 * SCL/SDA 取自 MINIPET_ACTIVE_PROFILE.pins.i2c。
 * 必须在任何 I2C 器件驱动 init 之前调用。
 */
esp_err_t i2c_bus_init(void);

/**
 * @brief 注册一个 I2C 器件（幂等：同名器件返回已有 handle）
 *
 * @param name      器件名（诊断用，如 "qmi8658"），长度 <= 11 字符
 * @param addr7     7-bit 器件地址
 * @param scl_hz    该器件允许的总线时钟（如 400000），总线按器件逐笔降速
 * @param out_handle 出参：i2c_master 设备句柄
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM / ESP_ERR_INVALID_STATE(总线未 init)
 */
esp_err_t i2c_bus_register_device(const char *name, uint16_t addr7, uint32_t scl_hz,
                                  i2c_master_dev_handle_t *out_handle);

/** @brief 按名查 handle（未注册返回 NULL） */
i2c_master_dev_handle_t i2c_bus_find(const char *name);

/** @brief 总线上探测地址（在线返回 ESP_OK） */
esp_err_t i2c_bus_probe(uint16_t addr7);

/* ---------- 事务辅助（全部加总线锁，寄存器型器件读写常用封装） ---------- */

/** 纯写（无寄存器地址，如 RTC 复位/停止命令） */
esp_err_t i2c_bus_write(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len);

/** 两段式寄存器读（写地址 → STOP → gap_us → 独立读；同一把总线锁内完成）。
 *  专治 transmit_receive（重复起始）在慢/忙器件上把控制器卡死的问题。 */
esp_err_t i2c_bus_read_reg8v_split(i2c_master_dev_handle_t dev, uint8_t reg,
                                   uint8_t *buf, size_t len, uint32_t gap_us);

/** 总线硬件复位：清"事务没收尾导致 status 非 IDLE → 之后每笔都 INVALID_STATE"的卡死。
 *  返回 ESP_OK = 已复位（status 置回 IDLE），调用方随后重试即可。 */
esp_err_t i2c_bus_hw_reset(void);

/** 纯读（无寄存器地址） */
esp_err_t i2c_bus_read(i2c_master_dev_handle_t dev, uint8_t *buf, size_t len);

/** 写单寄存器 */
esp_err_t i2c_bus_write_reg8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);

/** 突发写：寄存器地址 + N 字节 */
esp_err_t i2c_bus_write_reg8v(i2c_master_dev_handle_t dev, uint8_t reg,
                              const uint8_t *data, size_t len);

/** 突发读：寄存器地址 + 读回 N 字节（QMI8658 12 字节突发等） */
esp_err_t i2c_bus_read_reg8v(i2c_master_dev_handle_t dev, uint8_t reg,
                             uint8_t *buf, size_t len);

/** @brief 打印当前注册表（诊断用，log_i） */
void i2c_bus_dump(void);

#ifdef __cplusplus
}
#endif
