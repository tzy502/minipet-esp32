/**
 * @file drivers.h
 * @brief BSP 驱动层统一对外头 —— app/render/audio 模块只需要 include 这一个
 *
 * ============================ 初始化顺序（必须） ============================
 *
 *   1. i2c_bus_init()            共享 I2C（五器件的公共前置）
 *   2. display_init()            CO5300 QSPI（SPI2_HOST）
 *   3. touch_cst9220_init()      触摸（依赖 I2C）
 *   4. imu_qmi8658_init()        IMU（依赖 I2C）
 *   5. rtc_pcf85063_init()       RTC（依赖 I2C；未校时 get_time 返回 INVALID_STATE）
 *   6. pmu_axp2101_init()        PMU（依赖 I2C；低电默认轮询）
 *   7. codec_es8311_init(rate)   音频（依赖 I2C；PA 默认关，播放前 pa_ctrl_enable(true)）
 *   8. sd_mount()                TF 卡（SPI3_HOST 独立，返回 errno，失败可降级）
 *
 * 各 init 均幂等；顺序违背时 I2C 器件会因总线未初始化返回 INVALID_STATE。
 *
 * ============================ 使用纪律 ============================
 *
 * - 中断回调（触摸/IMU/按键/PMU）都在【中断上下文】执行：
 *   只允许 xTaskNotifyFromISR / xQueueSendFromISR 等 FROM_ISR 动作，
 *   禁止在回调里做 I2C/SPI 事务或任何阻塞调用。
 * - display_blit() 的像素缓冲必须是大端 RGB565（见 display_co5300.h）。
 * - GPIO ISR 服务由首个需要的驱动安装（gpio_install_isr_service(0)），
 *   未用 IRAM 标志：OTA 写 flash 窗口内中断短暂延迟，可接受。
 * - 硬件引脚一律取 MINIPET_PROFILE_AMOLED216.pins（profiles/amoled216.h）。
 */
#pragma once

#include "esp_err.h"

/* 板卡 profile（引脚表 / 能力位） */
#include "amoled216.h"

/* 驱动 API */
#include "i2c_bus.h"
#include "display_co5300.h"
#include "touch_cst9220.h"
#include "imu_qmi8658.h"
#include "rtc_pcf85063.h"
#include "pmu_axp2101.h"
#include "codec_es8311.h"
#include "sd_tf.h"
#include "key_gpio18.h"
