/**
 * @file drivers.h
 * @brief BSP 驱动层统一对外头 —— app/render/audio 模块只需要 include 这一个
 *
 * ============================ 初始化顺序（必须） ============================
 *
 *   1. i2c_bus_init()            共享 I2C（IMU/RTC/触摸/PMU/Codec 的公共前置）
 *   2. display_init()            ST77916 QSPI（SPI2_HOST；render_init 内部调用）
 *   3. touch_cst816_init()       触摸（CST816S@0x15；缺失降级，input 任务内调）
 *   4. imu_qmi8658_init()        IMU（依赖 I2C；缺失不阻断启动，app 层有降级路径）
 *   5. rtc_pcf85063_init()       RTC（依赖 I2C；未校时 get_time 返回 INVALID_STATE）
 *   6. pmu_bq27220_init()        电量计（依赖 I2C；探测失败仅降级，返回 ESP_OK）
 *   7. sd_mount()                TF 卡（SDMMC，返回 errno，失败可降级）
 *   8. key_gpio0_init()          BOOT 键（GPIO0，输入+上拉+轮询消抖）
 *   （codec_es8311_init 由 bgm_start 在任务建栈**之后**调用，见 bgm.c 顺序纪律）
 *
 * 各 init 均幂等；顺序违背时 I2C 器件会因总线未初始化返回 INVALID_STATE。
 *
 * ============================ 使用纪律 ============================
 *
 * - 中断回调（IMU/RTC）都在【中断上下文】执行：
 *   只允许 xTaskNotifyFromISR / xQueueSendFromISR 等 FROM_ISR 动作，
 *   禁止在回调里做 I2C/SPI 事务或任何阻塞调用。
 * - display_blit() 的像素缓冲必须是大端 RGB565（见 display_st77916.h）。
 * - GPIO ISR 服务由首个需要的驱动安装（gpio_install_isr_service(0)），
 *   未用 IRAM 标志：OTA 写 flash 窗口内中断短暂延迟，可接受。
 * - 硬件引脚一律取 MINIPET_ACTIVE_PROFILE.pins（profiles/lcd185b.h）。
 */
#pragma once

#include "esp_err.h"

/* 板卡 profile（引脚表 / 能力位） */
#include "lcd185b.h"

/* 驱动 API */
#include "i2c_bus.h"
#include "display_st77916.h"
#include "imu_qmi8658.h"
#include "rtc_pcf85063.h"
#include "sd_tf.h"
#include "key_gpio0.h"
#include "touch_cst816.h"
#include "pmu_bq27220.h"
#include "codec_es8311.h"
