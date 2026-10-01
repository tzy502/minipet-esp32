/**
 * @file key_gpio0.h
 * @brief 中排按键（大概率接 GPIO0，CHIP_PU 附近）——纯轮询驱动，绝不可配输出
 *
 * 硬件口径 [核对]：据微雪 wiki，板侧三键的【中键】大概率接 GPIO0（ESP32-S3
 * strapping 脚，CHIP_PU 复位电路附近）。运行期作输入安全：内部上拉、按下接地；
 * GPIO0 兼作 BOOT 模式选择，复位采样低电平会进下载模式——因此【任何情况下
 * 都不得配成输出/禁止上拉】。本驱动只做 INPUT + PULLUP，无 ISR（GPIO0 与
 * 复位时序相关，避免任何中断配置干扰），消抖与按下沿判定由调用方周期
 * tick 轮询完成。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 中键引脚（profile 引脚表未收录前先在此定义；接果与 wiki 出入只改这里） */
#define KEY_GPIO0_PIN  0

/** @brief 初始化：GPIO0 配输入 + 内部上拉（无中断、绝不输出）。幂等。 */
esp_err_t key_gpio0_init(void);

/** @brief 当前原始电平是否按下（低电平=按下，未消抖） */
bool key_gpio0_pressed(void);

/**
 * @brief 周期轮询消抖 + 按下沿查询（input 任务 ~20ms 节拍调用）
 *
 * 语义与 input_dispatch 的菜单键一致：
 *   - 原始电平稳定 30ms 才确认沿；
 *   - 确认按下沿【返回 true 一次】后上闩，长按/抖动不重复触发；
 *   - 确认释放后再静止 80ms 才开闩，允许下一次按压。
 *
 * @return true = 本轮捕获到一次「已消抖的按下沿」事件（消费一次即弃）
 */
bool key_gpio0_tick(void);

#ifdef __cplusplus
}
#endif
