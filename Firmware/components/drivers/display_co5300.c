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
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_co5300.h"
#include "driver/spi_master.h"
#include "amoled216.h"

static const char *TAG = "co5300";

static esp_lcd_panel_handle_t  s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_tx_done;    /* QSPI color 传输完成信号（同步化：深度 1 + 等待） */
static bool s_inited;

/* esp_lcd 颜色传输完成回调：释放等待者（同步化关键——BSP 默认异步队列深度 3
 * 会被 30fps 连续 blit 打爆 ESP_ERR_NO_MEM 丢帧，真机定稿改深度 1+等待） */
static bool IRAM_ATTR color_tx_done_cb(esp_lcd_panel_io_handle_t io,
                                       esp_lcd_panel_io_event_data_t *edata, void *user)
{
    (void)io; (void)edata; (void)user;
    BaseType_t hi = pdFALSE;
    xSemaphoreGiveFromISR(s_tx_done, &hi);
    return hi == pdTRUE;
}

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

    s_tx_done = xSemaphoreCreateBinary();
    s_lock = xSemaphoreCreateMutex();
    if (!s_tx_done || !s_lock) {
        return ESP_ERR_NO_MEM;
    }

    /* 直建 QSPI 总线 + 同步面板 IO（trans_queue_depth=1 + 完成信号量）。
     * 引脚=本板定值（与 BSP 默认一致）：PCLK38 D0-3=4/5/6/7 CS12 RST39 */
    const spi_bus_config_t buscfg = CO5300_PANEL_BUS_QSPI_CONFIG(38, 4, 5, 6, 7,
                                    SW * SH * 2);
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(12,
                                           color_tx_done_cb, NULL);
    io_cfg.trans_queue_depth = 1;        /* 同步化关键：深度 1，永不积压 */
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                   &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_io 创建失败: %s", esp_err_to_name(err));
        return err;
    }

    const co5300_vendor_config_t vc = {
        .flags.use_qspi_interface = 1,
        /* init_cmds=NULL → 组件内置默认初始化序列（官方维护） */
    };
    const esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = 39,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vc,
    };
    err = esp_lcd_new_panel_co5300(s_io, &pc, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "co5300 面板创建失败: %s", esp_err_to_name(err));
        return err;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);

    display_brightness(80);   /* 默认 80%，应用可再调 */
    s_inited = true;
    ESP_LOGI(TAG, "CO5300(SYNC) 就绪 %ux%u QSPI", SW, SH);
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
        /* BSP 面板 IO 为异步队列（深度 3），推送过快返回 ESP_ERR_NO_MEM →
         * 短暂让权重试（QSPI 40MHz 排空一包 <2ms）——丢帧会造成画面停滞 */
        err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, (void *)rgb565_be);
        if (err == ESP_OK) xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(100));
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
        if (err == ESP_OK) xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(100));
        heap_caps_free(tmp);
    }

    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        static int64_t s_last_err_log;
        int64_t now = esp_timer_get_time();
        if (now - s_last_err_log > 5000000) {   /* 5s 限频，防刷屏拖慢系统 */
            ESP_LOGE(TAG, "draw_bitmap 失败: %s", esp_err_to_name(err));
            s_last_err_log = now;
        }
    }
    return err;
}

esp_err_t display_brightness(uint8_t pct)
{
    /* CO5300 亮度 = 0x51 DBV 命令（0-255）。panel 未就绪前静默跳过，
     * display_init 完成后由应用再调即可 */
    if (!s_io) {
        return ESP_OK;
    }
    return esp_lcd_panel_io_tx_param(s_io, 0x51, (const uint8_t[]){ pct }, 1);
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
        if (err == ESP_OK) xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(100));
    }
    if (((y + h) & 1) && err == ESP_OK) {        /* 奇数行收尾：借上一行组成 2 行 */
        err = esp_lcd_panel_draw_bitmap(s_panel, x, y + h - 2, x + w, y + h, line);
        if (err == ESP_OK) xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(100));
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
