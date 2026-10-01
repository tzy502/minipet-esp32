/**
 * @file key_gpio18.h
 * @brief 板载菜单按键 Key3（GPIO18，外部 R18 10K 上拉，按下接地）
 *
 * 硬件：按下 = 低电平。内部上拉一并打开（与外部 10K 并联无害）。
 * 消抖：中断里做 20ms 时间窗过滤，回调只在「确认按下沿」触发一次。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化按键 GPIO + 双边沿中断（消抖状态机在 ISR 内）。幂等。 */
esp_err_t key_gpio18_init(void);

/** @brief 当前是否按下（电平轮询，含 20ms 稳定窗口语义由调用方自定） */
bool key_gpio18_pressed(void);

/**
 * @brief 注册按键回调（中断上下文！FROM_ISR 动作 only）
 *
 * 触发时机：确认的按下沿（消抖通过）。回调里典型动作是
 * xTaskNotifyFromISR(input_task, KEY_EVENT_MENU, ...)。
 * 传 NULL 取消注册。
 */
void key_gpio18_set_callback(void (*on_press)(void *arg), void *arg);

#ifdef __cplusplus
}
#endif
