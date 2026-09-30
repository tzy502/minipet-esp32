/**
 * @file display_st77916.c
 * @brief 1.85" 圆 LCD（ST77916）QSPI 显示驱动 —— LCD-1.85B 板专用
 *
 * 【2026-09-29 协议重构】弃用 esp_lcd panel-io 通道，直用 spi_master：
 * 真机诊断实证 esp_lcd 的写色帧序对本面板无效（队列/轮询两路径都"成功"
 * 但 RAMRD 恒定、屏纯黑）。对照 ESPHome qspi_dbi（同面板家族实机跑通）
 * 的帧序协议：
 *   - 寄存器写：opcode 0x02，24bit 地址 = reg<<8（寄存器在 bits[15:8]！
 *     —— 与 RDDID 读法同构），参数随其后；
 *   - 像素写：opcode 0x32，24bit 地址 = 0x2C00（RAMWR 同样在中间字节），
 *     地址相后 4 个 dummy 周期再进数据；
 *   - 生命周期：RST(高5 低5 高5) → 120ms → SLPOUT → 120ms → 厂商序列 →
 *     INVON/MADCTL/DISPON。
 * esp_lcd 的帧序为 reg 在低字节、无 dummy —— 与上不兼容，此为本板纯黑总根因。
 *
 * 数据契约不变：display_blit() 像素缓冲 = 大端 RGB565。
 */
#include "display_co5300.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "amoled216.h"

static const char *TAG = "st77916";

#ifndef MP_ST77916_OPCODE_READ
#define MP_ST77916_OPCODE_READ 0x0B
#endif

static SemaphoreHandle_t s_lock;
static bool s_inited;

static const uint16_t SW = 360;
static const uint16_t SH = 360;

#define BL_GPIO 5
static bool s_bl_on;

static spi_device_handle_t s_spi;

/* ── 微雪 1.85B 面板两批次初始化序列（st77916_init_tables.c include）── */

/* ══ QSPI 底层 ═══════════════════════════════════════════════════════ */

/* 通用事务：8bit 命令相(QIO) + 24bit 地址相(QIO) + dummy + 数据。
 * ESPHome qspi_dbi 同款帧序：cmd=0x02/0x32，addr=reg<<8 / 0x2C00。 */
static esp_err_t qspi_write(uint8_t cmd, uint32_t addr, uint32_t dummy_bits,
                            const void *data, size_t len)
{
    spi_transaction_ext_t t = { 0 };
    t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                   SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    t.base.cmd = cmd;
    t.base.addr = addr;
    t.command_bits = 8;
    t.address_bits = 24;
    t.dummy_bits = dummy_bits;
    /* len==0（SLPOUT/DISPON 等无参命令）：必须无 MOSI 相——USE_TXDATA/
     * tx_buffer/length 全不能设，否则 spi_master 校验直接 INVALID_ARG
     * （真机实证：三条点亮屏幕的命令静默失败）。 */
    if (len > 0 && len <= 4) {
        t.base.flags |= SPI_TRANS_USE_TXDATA;
        memcpy(t.base.tx_data, data, len);
        t.base.length = len * 8;
    } else if (len > 4) {
        t.base.tx_buffer = data;
        t.base.length = len * 8;
    }
    return spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
}

/* 寄存器写（参数随命令） */
static esp_err_t reg_write(uint8_t reg, const void *data, size_t len)
{
    return qspi_write(0x02, (uint32_t)reg << 8, 0, data, len);
}

/* 寄存器读（寄存器在中间字节；数据相经 rx 缓冲） */
static esp_err_t reg_read(uint8_t reg, void *out, size_t len)
{
    spi_transaction_ext_t t = { 0 };
    t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                   SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY |
                   SPI_TRANS_USE_RXDATA;
    t.base.cmd = MP_ST77916_OPCODE_READ;
    t.base.addr = (uint32_t)reg << 8;
    t.command_bits = 8;
    t.address_bits = 24;
    t.dummy_bits = 8;                    /* 读前 8 dummy（2 QIO clock） */
    t.base.rxlength = len * 8;
    esp_err_t err = spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
    if (err == ESP_OK) memcpy(out, t.base.rx_data, len);
    return err;
}

/* 设窗 + 像素流（0x32 | addr 0x2C00 | 4 dummy | 大端 RGB565） */
static esp_err_t panel_draw(int x1, int y1, int x2, int y2,
                            const uint8_t *px, size_t len)
{
    uint8_t caset[4] = { (uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2 };
    uint8_t raset[4] = { (uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2 };
    esp_err_t err = reg_write(0x2A, caset, 4);
    if (err != ESP_OK) return err;
    err = reg_write(0x2B, raset, 4);
    if (err != ESP_OK) return err;
    return qspi_write(0x32, 0x2C00, 4, px, len);
}

/* ══ 背光 ═════════════════════════════════════════════════════════════ */
static void backlight_set(bool on)
{
    gpio_set_level(BL_GPIO, on ? 1 : 0);
    s_bl_on = on;
}

/* ══ 初始化序列两批次（微雪 BSP vendor_specific_init_version_1/2）════ */
#include "st77916_init_tables.c"

/* ══ 区域 2 像素对齐 ═══════════════════════════════════════════════════ */
static void even_round(int *x1, int *y1, int *x2, int *y2)
{
    if (*x1 < 0) *x1 = 0;
    if (*y1 < 0) *y1 = 0;
    if (*x2 > SW - 1) *x2 = SW - 1;
    if (*y2 > SH - 1) *y2 = SH - 1;
    *x1 -= (*x1 & 1);
    *y1 -= (*y1 & 1);
    if ((*x2 & 1) == 0) (*x2)++;
    if ((*y2 & 1) == 0) (*y2)++;
}

/* ══ 持续全帧刷新（RAMless/TE 面板必需）══════════════════════════════ */
static const uint16_t *s_fb_src;
static int s_fb_stride;
static volatile bool s_refresh_on;
static uint8_t *s_refr_stage;
static int s_refr_rows;

static void refresh_task(void *arg)
{
    (void)arg;
    uint8_t *stage = s_refr_stage;
    const int rows = s_refr_rows;
    const int chunk_sz = SW * rows * 2u;
    const int chunks = SH / rows;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t frames = 0, fails = 0, last_report = 0;
    while (s_refresh_on && s_fb_src) {
        for (int c = 0; c < chunks && s_refresh_on; c++) {
            const int y0 = c * rows;
            const uint8_t *src = (const uint8_t *)(s_fb_src + (size_t)y0 * s_fb_stride);
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) continue;
            memcpy(stage, src, chunk_sz);
            esp_err_t e = panel_draw(0, y0, SW - 1, y0 + rows - 1, stage, chunk_sz);
            if (e != ESP_OK) fails++;
            xSemaphoreGive(s_lock);
        }
        frames++;
        if (frames - last_report >= 150) {
            ESP_LOGW(TAG, "刷新遥测：frames=%u fails=%u", (unsigned)frames, (unsigned)fails);
            last_report = frames;
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(5));
    }
    heap_caps_free(stage);
    ESP_LOGW(TAG, "持续刷新已停止");
    vTaskDelete(NULL);
}

/* stage 降级链（display_init 堆干净窗口分配） */
static int refresh_stage_alloc(void)
{
    static const int rows_opts[] = { 4, 2 };
    for (int i = 0; i < 2; i++) {
        s_refr_stage = heap_caps_malloc(SW * rows_opts[i] * 2u,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (s_refr_stage) {
            s_refr_rows = rows_opts[i];
            ESP_LOGW(TAG, "刷新 stage=%d 行（%uB 内部）", s_refr_rows,
                     (unsigned)(SW * rows_opts[i] * 2u));
            return 0;
        }
    }
    return -1;
}

void display_set_frame_source(const uint16_t *fb, int stride)
{
    if (!s_inited) {
        ESP_LOGW(TAG, "frame_source 忽略：display 未初始化");
        return;
    }
    s_fb_src = fb;
    s_fb_stride = stride > 0 ? stride : SW;
#if CONFIG_MP_LCD_CONTINUOUS_REFRESH
    if (!s_refr_stage && refresh_stage_alloc() != 0) {
        ESP_LOGE(TAG, "刷新 stage 分配失败（内部堆枯竭）——黑屏");
        return;
    }
    if (fb && !s_refresh_on) {
        s_refresh_on = true;
        if (xTaskCreatePinnedToCore(refresh_task, "lcd_refr", 2560, NULL, 4, NULL, 0) != pdPASS) {
            ESP_LOGE(TAG, "刷新任务创建失败");
            s_refresh_on = false;
        } else {
            ESP_LOGW(TAG, "持续全帧刷新已启动（qspi_dbi 帧序）");
        }
    }
#else
    ESP_LOGW(TAG, "持续刷新已禁用（CONFIG_MP_LCD_CONTINUOUS_REFRESH=n）");
#endif
}

esp_err_t display_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    const minipet_pins_t *pins = &MINIPET_ACTIVE_PROFILE.pins;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    /* 背光脚先拉高（LCD 点亮前提） */
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << BL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&bl_cfg);
    if (err != ESP_OK) return err;
    backlight_set(true);

    /* 硬复位（ESPHome qspi_dbi 同款时序） */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << pins->lcd.rst,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&rst_cfg);
    if (err != ESP_OK) return err;
    gpio_set_level(pins->lcd.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(pins->lcd.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(pins->lcd.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* QSPI 总线 + 直挂设备（绕开 esp_lcd panel-io：其帧序与本面板不兼容） */
    spi_bus_config_t buscfg = {
        .sclk_io_num = pins->lcd.sclk,
        .data0_io_num = pins->lcd.sio0,
        .data1_io_num = pins->lcd.sio1,
        .data2_io_num = pins->lcd.sio2,
        .data3_io_num = pins->lcd.sio3,
        .max_transfer_sz = SW * SH * 2,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = CONFIG_MP_LCD_PCLK_HZ,
        .mode = 0,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi 设备挂载失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 面板 ID 读取（寄存器中间字节读法）——v1/v2 批次判定 */
    {
        uint8_t id[4] = { 0 };
        esp_err_t e1 = reg_read(0x04, id, sizeof(id));
        ESP_LOGW(TAG, "RDDID(04)=%02X %02X %02X %02X（%s）", id[0], id[1], id[2], id[3],
                 e1 == ESP_OK ? "OK" : esp_err_to_name(e1));
        if (id[0] == 0x00 && id[1] == 0x02) {
            ESP_LOGW(TAG, "面板 v2 批次");
        } else if (id[0] == 0x00 && id[1] == 0x7F) {
            ESP_LOGW(TAG, "面板 v1 批次");
        } else {
            ESP_LOGE(TAG, "RDDID 未匹配（继续 v2 序列，日志留痕）");
        }
    }

    /* SLPOUT → 厂商序列（按批次） → INVON/MADCTL/DISPON（ESPHome 生命周期） */
    err = reg_write(0x11, NULL, 0);                 /* SLPOUT */
    if (err != ESP_OK) ESP_LOGE(TAG, "SLPOUT: %s", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(120));

    const st77916_lcd_init_cmd_t *tbl = vendor_specific_init_version_2;
    size_t tbl_n = sizeof(vendor_specific_init_version_2) / sizeof(vendor_specific_init_version_2[0]);
    for (size_t i = 0; i < tbl_n; i++) {
        err = reg_write((uint8_t)tbl[i].cmd, tbl[i].data, tbl[i].data_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "序列 %zu 条(0x%02X)失败: %s", i, tbl[i].cmd, esp_err_to_name(err));
            return err;
        }
        if (tbl[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(tbl[i].delay_ms));
    }
    ESP_LOGW(TAG, "v2 序列 %u 条完成", (unsigned)tbl_n);

    uint8_t mad = 0x00;                              /* 原生正立（同 216 终版口径） */
    err = reg_write(0x36, &mad, 1);                  /* MADCTL */
    err = reg_write(0x21, NULL, 0);                  /* INVON */
    err = reg_write(0x29, NULL, 0);                  /* DISPON */
    (void)err;
    vTaskDelay(pdMS_TO_TICKS(50));

    s_inited = true;
    ESP_LOGI(TAG, "ST77916(LCD) 就绪 %ux%u QSPI BL=GPIO%d PCLK=%dMHz（qspi_dbi 帧序）",
             SW, SH, BL_GPIO, CONFIG_MP_LCD_PCLK_HZ / 1000000);

#if CONFIG_MP_LCD_BRINGUP_TEST
    {
        for (int round = 0; round < 3; round++) {
            esp_err_t e = display_fill_rect(0, 0, SW, SH, 0x07E0);
            ESP_LOGW(TAG, "r%d 绿 fill=%s", round, esp_err_to_name(e));
            vTaskDelay(pdMS_TO_TICKS(2000));
            e = display_fill_rect(0, 0, SW, SH, 0xFFFF);
            ESP_LOGW(TAG, "r%d 白 fill=%s", round, esp_err_to_name(e));
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
#endif
    return ESP_OK;
}

void display_set_orientation(bool swap_xy, bool mirror_x, bool mirror_y)
{
    if (!s_inited) return;
    uint8_t mad = 0x00;
    if (mirror_x) mad |= 0x40;                       /* MX */
    if (mirror_y) mad |= 0x80;                       /* MY */
    if (swap_xy)  mad |= 0x20;                       /* MV */
    reg_write(0x36, &mad, 1);
    ESP_LOGI(TAG, "orientation swap=%d mx=%d my=%d", swap_xy, mirror_x, mirror_y);
}

void display_wait_tx_idle(void)
{
    /* polling 事务同步完成：无在飞概念 */
}

bool display_tx_busy(void)
{
    return false;
}

esp_err_t display_blit(int x, int y, int w, int h, const uint8_t *rgb565_be)
{
    if (!s_inited || !rgb565_be) return ESP_ERR_INVALID_STATE;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > SW || y + h > SH)
        return ESP_ERR_INVALID_ARG;

    int x1 = x, y1 = y, x2 = x + w - 1, y2 = y + h - 1;
    even_round(&x1, &y1, &x2, &y2);
    int aw = x2 - x1 + 1, ah = y2 - y1 + 1;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t err;
    if (aw == w && ah == h) {
        err = panel_draw(x1, y1, x2, y2, rgb565_be, (size_t)aw * ah * 2u);
    } else {
        size_t sz = (size_t)aw * ah * 2u;
        uint8_t *tmp = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp) { xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
        memset(tmp, 0, sz);
        for (int ry = 0; ry < ah; ry++) {
            int sy = ry + (y1 - y);
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
        err = panel_draw(x1, y1, x2, y2, tmp, sz);
        heap_caps_free(tmp);
    }

    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        static int64_t s_last_err_log;
        int64_t now = esp_timer_get_time();
        if (now - s_last_err_log > 5000000) {
            ESP_LOGE(TAG, "draw 失败: %s", esp_err_to_name(err));
            s_last_err_log = now;
        }
    }
    return err;
}

esp_err_t display_brightness(uint8_t pct)
{
    if (!s_inited) return ESP_OK;
    backlight_set(pct > 0);
    return ESP_OK;
}

esp_err_t display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t rgb565)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > SW || y + h > SH)
        return ESP_ERR_INVALID_ARG;

    static uint8_t line[4][360 * 2] __attribute__((aligned(64)));
    int x1 = x, y1 = y, x2 = x + w - 1, y2 = y + h - 1;
    even_round(&x1, &y1, &x2, &y2);
    const int aw = x2 - x1 + 1;
    if (aw > (int)sizeof(line[0]) / 2) return ESP_ERR_INVALID_ARG;

    const uint8_t hi = (uint8_t)(rgb565 >> 8);
    const uint8_t lo = (uint8_t)(rgb565 & 0xFF);
    for (int i = 0; i < aw; i++) {
        line[0][2 * i] = hi;  line[0][2 * i + 1] = lo;
        line[1][2 * i] = hi;  line[1][2 * i + 1] = lo;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t err = ESP_OK;
    for (int yy = y1; yy <= y2 && err == ESP_OK; yy += 2) {
        err = panel_draw(x1, yy, x2, yy + 1, (const uint8_t *)line, (size_t)aw * 2u * 2u);
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t display_set_sleep(bool sleep)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    backlight_set(!sleep);
    esp_err_t err;
    if (sleep) {
        err = reg_write(0x28, NULL, 0);              /* DISPOFF */
        err |= reg_write(0x10, NULL, 0);             /* SLPIN */
    } else {
        err = reg_write(0x11, NULL, 0);              /* SLPOUT */
        vTaskDelay(pdMS_TO_TICKS(120));
        err |= reg_write(0x29, NULL, 0);             /* DISPON */
    }
    return err;
}
