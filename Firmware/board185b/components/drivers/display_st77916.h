/**
 * @file display_st77916.h
 * @brief 1.85" 圆 LCD（ST77916 驱动 IC）QSPI 显示驱动 —— API 契约头
 *
 * 硬件（引脚取自 profile，BL 固定 GPIO5）：
 *   QSPI 数据 SIO0-3 = GPIO46/45/42/41，QSPI 时钟 = GPIO40
 *   LCD_CS = GPIO21，LCD_RESET = GPIO3，背光 BL = GPIO5
 *   360x360 圆形，RGB565。RAMless 面板：显存不驻留，靠持续全帧刷新任务维持画面。
 *
 * 数据契约（渲染层必须遵守）：
 *   display_blit() 的像素缓冲为【大端 RGB565】（高字节在前），
 *   持续刷新任务在上屏边界统一做小端→大端交换（480→360 取样同点）；
 *   合成器管线恒为 480x480 小端，缩放/交换收敛在驱动内（display_st77916.c）。
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
 * @brief 初始化显示：SPI 总线 + 器件 + 硬复位 + ST77916 批次 init 序列
 *
 * 官方 BSP 精确流程：RST → 3MHz 探针 IO 读 RDDID 判批次（v1/v2 init 表）→
 * 重建主 IO（CONFIG_MP_LCD_PCLK_HZ）→ init + disp_on + INVOFF 固化。
 * 初始化完成后背光全开（BL GPIO5 只有开/关两档，无逐级调光）。
 * 幂等：已初始化直接返回 ESP_OK。
 */
esp_err_t display_init(void);

/**
 * @brief 运行时切换面板方向（方向标定轮播专用）
 *
 * @param swap_xy  宽高轴交换
 * @param mirror_x/mirror_y  逐轴镜像
 *
 * 须在 display_init() 成功后调用，未初始化时忽略。
 */
void display_set_orientation(bool swap_xy, bool mirror_x, bool mirror_y);

/**
 * @brief 矩形区域上传（SET_WINDOW + 行流）
 *
 * @param x,y,w,h 矩形区域；自动 clamp 到屏幕范围并对齐到 2 像素边界
 * @param rgb565_be w*h*2 字节大端 RGB565 像素流（逐行、无行距）
 *
 * 实现：esp_lcd draw_bitmap（官方 BSP 同款）。
 * 持续全帧刷新开启期间为无操作（刷新流是唯一显示通路，脏区直推会交替闪烁）。
 * 线程安全：内部互斥，可被 LVGL 任务/合成器共用。
 */
esp_err_t display_blit(int x, int y, int w, int h, const uint8_t *rgb565_be);

/**
 * @brief 该缓冲是否仍在飞（合成器多暂存轮转前查；false = 可安全重填）
 *
 * 【2026-10-01 自 216 co5300 同步】驱动侧登记"在飞缓冲指针环"（发送顺序 FIFO，
 * 完成回调按序弹出）；合成器以此实现多块暂存轮转，避免"每块都等上一笔传完"
 * 导致 SPI 不流水（满屏 blit 明显变慢）。
 */
bool display_blit_buf_busy(const void *buf);

/* 等上一笔 color 传输读完调用方缓冲（重填共用暂存前必调）。
 * 返回时保证无在飞传输；display_tx_busy() 供探针判定"填缓冲时是否仍有传输在飞"。 */
void display_wait_tx_idle(void);
bool display_tx_busy(void);

/** @brief 亮度百分比 0-100（本板背光只有开/关两档：>0=亮，0=灭） */
esp_err_t display_brightness(uint8_t pct);

/**
 * @brief 纯色填充矩形（rgb565 为本机字节序 uint16，驱动内部换大端）
 *
 * 用途：FATAL 态黑屏+错误色块（看门狗熔断通道，绝不进正常渲染路径）。
 * 逐行流式实现，不额外分配整块缓冲。
 * 持续全帧刷新开启期间为无操作（同 display_blit）。
 */
esp_err_t display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                            uint16_t rgb565);

/**
 * @brief 注册持续刷新帧源（RAMless 面板持续全帧刷新专用）
 *
 * @param fb 帧缓冲（RGB565，合成器 480x480 小端管线；驱动内做 480→360
 *           最近邻取样 + 字节交换）
 * @param stride 帧缓冲行距（像素）。NULL = 停止持续刷新。
 * 内部按 CONFIG_MP_LCD_CONTINUOUS_REFRESH 分配 DMA stage 并启动刷新任务
 * （分块上屏，任务保留不退出——退出即无刷新通路 = 屏幕永久冻结）。
 */
void display_set_frame_source(const uint16_t *fb, int stride);

/** 注册帧数据锁（持续刷新拷贝帧缓冲前调用；锁序=先帧锁后显示锁，防 ABBA 死锁）。 */
void display_set_frame_locks(void (*lock)(void), void (*unlock)(void));

/**
 * @brief 暂停/恢复持续全帧刷新
 *
 * 用途：素材绑定等 fopen TF 密集段挂起刷新任务，减少显示锁/TF 竞争。
 * 挂起期间屏幕静止在最后一帧，任务与 DMA stage 均保留，
 * resume 后无缝续刷。两个 API 幂等、可重入安全（仅置/清内部标志）。
 * 【契约】调用方必须保证每条 return 路径都 resume——挂起泄漏 = 屏幕永久冻结。
 */
void display_refresh_suspend(void);
void display_refresh_resume(void);

/**
 * @brief 进入/退出睡眠（CLOCK_DOZE 待机时钟 / FATAL 关屏用，design §4.3）
 *
 * sleep=true: 面板 disp_off + 背光灭
 * sleep=false: 面板 disp_on + 120ms 等待 + 背光亮
 */
esp_err_t display_set_sleep(bool sleep);

#ifdef __cplusplus
}
#endif
