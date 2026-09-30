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
 * @brief 运行时切换面板方向（方向标定轮播专用）
 *
 * @param swap_xy  宽高轴交换（本板握持定稿恒为 true：面板原生竖屏坐标系）
 * @param mirror_x/mirror_y  逐轴镜像
 *
 * 用途：开机方向轮播标定模式——依次展示 4 种 mirror 组合供用户肉眼定稿；
 * 定稿后由 display_init() 固化对应常量（当前默认 swap_xy=true +
 * mirror(false,false)），本 API 即可删除。须在 display_init() 成功后调用，
 * 未初始化时忽略并告警。
 */
void display_set_orientation(bool swap_xy, bool mirror_x, bool mirror_y);

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

/* 等上一笔 color 传输读完调用方缓冲（重填共用暂存前必调；见 .c 注释的混行竞态）。
 * 返回时保证无在飞传输；display_tx_busy() 供探针判定"填缓冲时是否仍有传输在飞"。 */
void display_wait_tx_idle(void);
bool display_tx_busy(void);

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
 * @brief 注册持续刷新帧源（RAMless/TE 面板专用；GRAM 板为空操作）
 *
 * @param fb 帧缓冲（RGB565，本机字节序或驱动契约字节序）
 * @param stride 帧缓冲行距（像素）。NULL = 停止持续刷新。
 * 216（CO5300 GRAM）实现为空操作：脏区增量上屏语义不变。
 */
void display_set_frame_source(const uint16_t *fb, int stride);

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
