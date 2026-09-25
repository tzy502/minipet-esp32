/**
 * @file display_co5300.h
 * @brief 2.16" AMOLED（CO5300 驱动 IC）QSPI 显示驱动
 *
 * 硬件（引脚取自 profile）：
 *   QSPI 数据 SIO0-3 = GPIO4/5/6/7，QSPI 时钟 = GPIO38
 *   LCD_CS = GPIO12（硬件 CS），LCD_RESET = GPIO39
 *   480x480，RGB565。AMOLED 无背光引脚，亮度走寄存器 0x51。
 *
 * 数据契约（渲染层必须遵守）：
 *   display_blit() 的像素缓冲为【大端 RGB565】（高字节在前），
 *   与 CO5300 显存字节序一致，驱动不做字节交换——
 *   LVGL/合成器输出为小端时需在渲染层预先 swap（PSRAM 上一遍可接受）。
 *   缓冲建议 heap_caps_malloc(..., MALLOC_CAP_DMA)；S3 的 GDMA 也支持
 *   PSRAM 直读，但 PSRAM 缓冲需保持 4 字节对齐。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化显示：SPI 总线 + 器件 + 硬复位 + CO5300 初始化序列
 *
 * 初始化完成后亮度默认 40%（AMOLED 全亮刺眼，应用可再调）。
 * 幂等：已初始化直接返回 ESP_OK。
 */
esp_err_t display_init(void);

/**
 * @brief 矩形区域上传（SET_WINDOW + 行流）
 *
 * @param x,y,w,h 矩形区域；自动 clamp 到屏幕范围
 * @param rgb565_be w*h*2 字节大端 RGB565 像素流（逐行、无行距）
 *
 * 实现：0x2A 列窗 + 0x2B 行窗 + 0x2C RAM 前缀 + 四线（QIO）数据。
 * 线程安全：内部互斥，可被 LVGL 任务/合成器共用。
 */
esp_err_t display_blit(int x, int y, int w, int h, const uint8_t *rgb565_be);

/** @brief 亮度百分比 0-100（映射到 DBV 0-255，寄存器 0x51） */
esp_err_t display_brightness(uint8_t pct);

/**
 * @brief 纯色填充矩形（rgb565 为本机字节序 uint16，驱动内部换大端）
 *
 * 用途：FATAL 态黑屏+错误色块（看门狗熔断通道，绝不进正常渲染路径）。
 * 逐行流式实现，不额外分配整块缓冲。
 */
esp_err_t display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                            uint16_t rgb565);

/**
 * @brief 进入/退出睡眠（CLOCK_DOZE 待机时钟 / FATAL 关屏用，design §4.3）
 *
 * sleep=true: 0x10 SLPIN（屏幕熄灭，功耗降低）
 * sleep=false: 0x11 SLPOUT + 120ms 等待 + 0x29 DSPON
 */
esp_err_t display_set_sleep(bool sleep);

#ifdef __cplusplus
}
#endif
