/**
 * @file display_co5300.c
 * @brief 2.16" AMOLED 显示驱动 —— 内部实现 = 微雪官方 BSP 组件包装
 *
 * 2026-09-26 真机 bring-up 定稿：手写 QSPI 时序点不亮面板，整体切换到
 * 厂商维护的 waveshare/esp32_s3_touch_amoled_2_16 BSP（内部 = esp_lcd
 * co5300 QSPI 面板驱动，init 序列/四线像素写入均为官方实现）。
 * 对外 API（display_co5300.h）不变，渲染层零改动。
 *
 * 数据契约：display_blit() 像素缓冲 = 大端 RGB565（BSP_LCD_BIGENDIAN=1 一致）。
 * CO5300 QSPI 面板按 2 像素粒度寻址（官方 rounder 佐证）——blit/fill 区域
 * 在本驱动内向外取偶并越界 clamp，奇数区域经 PSRAM 暂存缓冲拷齐。
 */
#include "display_co5300.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp/esp-bsp.h"
#include "amoled216.h"

static const char *TAG = "co5300";

static esp_lcd_panel_handle_t  s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_lock;
static bool s_inited;

static const uint16_t SW = 480;
static const uint16_t SH = 480;

/* 区域向外取偶（CO5300 2 像素寻址粒度），x2/y2 为含端坐标 */
static void even_round(int *x1, int *y1, int *x2, int *y2)
{
    *x1 -= (*x1 & 1);
    *y1 -= (*y1 & 1);
    if ((*x2 & 1) == 0) (*x2)++;
    if ((*y2 & 1) == 0) (*y2)++;
    if (*x1 < 0) *x1 = 0;
    if (*y1 < 0) *y1 = 0;
    if (*x2 > SW - 1) *x2 = SW - 1;
    if (*y2 > SH - 1) *y2 = SH - 1;
}

esp_err_t display_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    bsp_display_config_t cfg = {
        .max_transfer_sz = SW * SH * 2,
    };
    esp_err_t err = bsp_display_new(&cfg, &s_panel, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_display_new 失败: %s", esp_err_to_name(err));
        return err;
    }
    /* bsp_display_new 内部已完成 reset + init + disp_on（官方时序） */

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    display_brightness(80);   /* 默认 80%，应用可再调 */
    s_inited = true;
    ESP_LOGI(TAG, "CO5300(BSP) 就绪 %ux%u QSPI", SW, SH);
    return ESP_OK;
}

esp_err_t display_blit(int x, int y, int w, int h, const uint8_t *rgb565_be)
{
    if (!s_inited || !rgb565_be) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > SW || y + h > SH) {
        return ESP_ERR_INVALID_ARG;
    }

    int x1 = x, y1 = y, x2 = x + w - 1, y2 = y + h - 1;
    even_round(&x1, &y1, &x2, &y2);
    int aw = x2 - x1 + 1, ah = y2 - y1 + 1;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err;
    if (aw == w && ah == h) {
        err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, (void *)rgb565_be);
    } else {
        /* 奇数区域：PSRAM 暂存补齐（边缘像素按邻边复制，视觉无差） */
        size_t sz = (size_t)aw * ah * 2u;
        uint8_t *tmp = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp) { xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
        memset(tmp, 0, sz);
        for (int ry = 0; ry < ah; ry++) {
            int sy = ry + (y1 - y);                    /* 目标行 → 源行 */
            if (sy < 0) sy = 0;
            if (sy > h - 1) sy = h - 1;
            const uint8_t *src = rgb565_be + (size_t)sy * w * 2;
            uint8_t *dst = tmp + (size_t)ry * aw * 2;
            for (int rx = 0; rx < aw; rx++) {
                int sx = rx + (x1 - x);
                if (sx < 0) sx = 0;
                if (sx > w - 1) sx = w - 1;
                dst[rx * 2]     = src[sx * 2];
                dst[rx * 2 + 1] = src[sx * 2 + 1];
            }
        }
        err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, tmp);
        heap_caps_free(tmp);
    }

    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap 失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t display_brightness(uint8_t pct)
{
    /* BSP 亮度 = 0x51 命令（CO5300 无背光 PWM）；panel 未就绪前静默跳过，
     * display_init 完成后由应用再调即可 */
    if (!s_panel) {
        return ESP_OK;
    }
    return bsp_display_brightness_set(pct);
}

esp_err_t display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                            uint16_t rgb565)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t sw = MINIPET_PROFILE_AMOLED216.width;
    const uint16_t sh = MINIPET_PROFILE_AMOLED216.height;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > sw || y + h > sh) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 行缓冲：内部 SRAM 静态分配，天然 DMA 可用。两行一组推送（2 行寻址粒度）。 */
    static uint8_t line[2][480 * 2];
    const uint8_t hi = (uint8_t)(rgb565 >> 8);   /* 大端：高字节在前 */
    const uint8_t lo = (uint8_t)(rgb565 & 0xFF);
    for (int i = 0; i < w; i++) {
        line[0][2 * i]     = hi;
        line[0][2 * i + 1] = lo;
        line[1][2 * i]     = hi;
        line[1][2 * i + 1] = lo;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    for (int16_t yy = y; yy + 2 <= y + h && err == ESP_OK; yy += 2) {
        err = esp_lcd_panel_draw_bitmap(s_panel, x, yy, x + w, yy + 2, line);
    }
    if (((y + h) & 1) && err == ESP_OK) {        /* 奇数行收尾：借上一行组成 2 行 */
        err = esp_lcd_panel_draw_bitmap(s_panel, x, y + h - 2, x + w, y + h, line);
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t display_set_sleep(bool sleep)
{
    if (!s_inited || !s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, !sleep);
    if (!sleep) {
        vTaskDelay(pdMS_TO_TICKS(120));   /* SLPOUT 稳定时间 */
    }
    return err;
}
