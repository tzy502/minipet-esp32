/**
 * @file imu_qmi8658.h
 * @brief QMI8658A/C 六轴 IMU 驱动（共享 I2C + INT1/INT2）
 *
 * 引脚（来自 profile）：QMI_INT1 = GPIO17（数据就绪），QMI_INT2 = GPIO21（预留）
 *
 * 使用模式（design §4.1 双中断唤醒采样，非轮询）：
 *   1. imu_qmi8658_set_drdy_callback() 注册 INT1 通知回调；
 *   2. 回调在【中断上下文】执行，只允许 FROM_ISR 动作；
 *   3. 任务里收到通知后调 imu_qmi8658_read_acc_gyro() 一次突发拿六轴。
 *
 * 单位约定：加速度输出 mg（1g = 9.8 m/s²），陀螺输出 mdps。
 * 量程换算基于本驱动配置的量程（±8g / ±256dps），改量程须同步改 .c 里换算。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一次性读六轴（推荐：一次突发，时间戳同源） */
typedef struct {
    float acc_mg[3];    /**< X/Y/Z，单位 mg */
    float gyro_mdps[3]; /**< X/Y/Z，单位 mdps */
} imu_sample_t;

/** @brief 初始化：探测器件 + 配置量程/ODR + 使能 + INT1 配置。幂等。 */
esp_err_t imu_qmi8658_init(void);

/** @brief 只读加速度（单位 mg） */
esp_err_t imu_qmi8658_read_acc(float mg[3]);

/** @brief 只读陀螺（单位 mdps） */
esp_err_t imu_qmi8658_read_gyro(float mdps[3]);

/** @brief 一次突发读六轴（12 字节寄存器 0x65 起，推荐接口） */
esp_err_t imu_qmi8658_read_acc_gyro(imu_sample_t *out);

/**
 * @brief 注册 INT1 数据就绪通知回调（中断上下文！FROM_ISR 动作 only）
 *        若 INT1 未配置成功/未接线，读取接口不受影响（退化为轮询采样）。
 */
void imu_qmi8658_set_drdy_callback(void (*cb)(void *arg), void *arg);

/**
 * @brief 阻塞等待 INT1 数据就绪（design §4.1「双中断唤醒采样，非轮询」）
 *
 * 首次调用会把当前任务注册为等待者（单等待者模型），此后 INT1 中断
 * 直接 xTaskNotifyFromISR 唤醒它。返回 true=有新数据可读；
 * 超时返回 false（此时读接口仍可用，只是数据可能为上次值）。
 * 典型用法：input 任务循环 wait_event(portMAX_DELAY) -> read_acc_gyro()。
 */
bool imu_qmi8658_wait_event(uint32_t wait_ticks);

/** @brief 器件是否在线（WHO_AM_I 校验通过） */
bool imu_qmi8658_ready(void);

#ifdef __cplusplus
}
#endif
