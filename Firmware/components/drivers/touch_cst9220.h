/**
 * @file touch_cst9220.h
 * @brief CST9220 电容触摸驱动（共享 I2C + INT + RST）
 *
 * 引脚（来自 profile）：TP_INT = GPIO11（低有效），TP_RESET = GPIO40，I2C 0x5A
 *
 * 使用模式（design §4.1 input 任务）：
 *   1. touch_cst9220_set_isr_callback() 注册通知回调（INT 下降沿触发）；
 *   2. 回调在【中断上下文】执行，只允许做 xTaskNotifyFromISR / queue send
 *      这类 FROM_ISR 动作，禁止 I2C 读写（I2C 事务不可在中断里做）；
 *   3. 任务上下文收到通知后调 touch_cst9220_read() 拿坐标。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 触摸首点 */
typedef struct {
    bool     pressed;  /**< true = 有手指按下（读首点） */
    uint8_t  fingers;  /**< 当前触点总数（>1 供手势扩展用） */
    uint16_t x;        /**< 首点 X，屏幕坐标系 */
    uint16_t y;        /**< 首点 Y */
} touch_point_t;

/** @brief 初始化：硬件复位 + 注册到 I2C 总线 + INT 引脚配置。幂等。 */
esp_err_t touch_cst9220_init(void);

/**
 * @brief 读首点（任务上下文调用）
 *
 * 一次 I2C 突发读数据帧，解析出触点数与首点坐标；I2C 失败时返回错误、
 * out 不变。可在中断通知后调用，也可轮询。
 */
esp_err_t touch_cst9220_read(touch_point_t *out);

/**
 * @brief 注册 INT 中断通知回调（中断上下文！）
 *
 * @param cb  回调函数（FROM_ISR 安全动作），传 NULL 取消注册
 * @param arg 透传给回调的用户参数
 */
void touch_cst9220_set_isr_callback(void (*cb)(void *arg), void *arg);

#ifdef __cplusplus
}
#endif
