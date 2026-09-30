/**
 * @file display_st77916.c
 * @brief 1.85" 圆 LCD（ST77916 驱动 IC）QSPI 显示驱动 —— LCD-1.85B 板专用
 *
 * 结构与 display_co5300.c（AMOLED-2.16）逐函数对齐：同一份 API 契约
 * （display_co5300.h）、同一套背压/2px 对齐/暂存补齐语义，渲染层零改动。
 * 差异点（板型决定）：
 *   - 面板 ST77916、360x360、引脚 CS21/PCLK40/D0-3=46/45/42/41/RST3
 *   - LCD 有背光：BL=GPIO5，display_brightness 映射为开关（非 AMOLED 的 0x51 DBV）
 *   - 睡眠走标准 DCS 0x28/0x29 + BL 拉低（esp_lcd st77916 组件内置序列）
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
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st77916.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "amoled216.h"

static const char *TAG = "st77916";

/* 组件私有宏，此处本地复刻（组件头未导出） */
#ifndef MP_ST77916_OPCODE_READ
#define MP_ST77916_OPCODE_READ 0x0B
#endif

static esp_lcd_panel_handle_t  s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_tx_slots;   /* 背压：在飞 color 传输槽位（=队列深度），完成回调归还 */
static volatile uint32_t s_tx_done_cnt;
static bool s_inited;

static const uint16_t SW = 360;
static const uint16_t SH = 360;

/* BL=GPIO5（profile lcd.bl 没有背光字段——按板型定值，同 co5300 引脚定值口径） */
#define BL_GPIO 5
static bool s_bl_on;

/* 深度 1 + 槽位信号量背压：与 co5300 修复后的口径一致（串行传输，
 * 防窗口切换交叠裂纹），见 display_co5300.c 同名宏注释。 */
#define TX_QUEUE_DEPTH 1

static bool IRAM_ATTR color_tx_done_cb(esp_lcd_panel_io_handle_t io,
                                       esp_lcd_panel_io_event_data_t *edata,
                                       void *user)
{
    (void)io; (void)edata; (void)user;
    s_tx_done_cnt++;
    BaseType_t hi = pdFALSE;
    xSemaphoreGiveFromISR(s_tx_slots, &hi);
    return hi == pdTRUE;
}

static esp_err_t polling_draw(int x1, int y1, int x2, int y2, const uint8_t *px, size_t len);

static bool tx_slot_take(void)
{
    /* polling 模式：事务同步完成，无背压概念（恒真直通） */
    return true;
#if 0
    if (!s_tx_slots) return true;
    uint32_t done_seen = s_tx_done_cnt;
    int64_t last_progress = esp_timer_get_time();
    for (;;) {
        if (xSemaphoreTake(s_tx_slots, pdMS_TO_TICKS(20)) == pdTRUE) {
            return true;
        }
        int64_t now = esp_timer_get_time();
        if (s_tx_done_cnt != done_seen) {
            done_seen = s_tx_done_cnt;
            last_progress = now;
        } else if (now - last_progress > 2000000) {
            ESP_LOGW(TAG, "槽位 2s 无完成回调（泄漏疑点）done=%u——放行降级",
                     (unsigned)s_tx_done_cnt);
            return false;
        }
    }
#endif
}

static void tx_slot_give(void)
{
    if (s_tx_slots) xSemaphoreGive(s_tx_slots);
}

/* 背光开关（LCD：亮度即背光；PWM 调光后续可换 LEDC，bring-up 先直控） */
static void backlight_set(bool on)
{
    gpio_set_level(BL_GPIO, on ? 1 : 0);
    s_bl_on = on;
}

// ==== vendor_specific_init_version_1: 182 条 ====
static const st77916_lcd_init_cmd_t vendor_specific_init_version_1[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x49}, 1, 0},
    {0xB1, (uint8_t []){0x4A}, 1, 0},
    {0xB2, (uint8_t []){0x1F}, 1, 0},
    {0xB4, (uint8_t []){0x46}, 1, 0},
    {0xB5, (uint8_t []){0x34}, 1, 0},
    {0xB6, (uint8_t []){0xD5}, 1, 0},
    {0xB7, (uint8_t []){0x30}, 1, 0},
    {0xB8, (uint8_t []){0x04}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x70, 0x09, 0x12, 0x0C, 0x0B, 0x27, 0x38, 0x54, 0x4E, 0x19, 0x15, 0x15, 0x2C, 0x2F}, 14, 0},
    {0xE1, (uint8_t []){0x70, 0x08, 0x11, 0x0C, 0x0B, 0x27, 0x38, 0x43, 0x4C, 0x18, 0x14, 0x14, 0x2B, 0x2D}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x08}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x0B}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x00}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x82}, 1, 0},
    {0xEA, (uint8_t []){0xDF}, 1, 0},
    {0xEB, (uint8_t []){0x89}, 1, 0},
    {0xEC, (uint8_t []){0x20}, 1, 0},
    {0xED, (uint8_t []){0x14}, 1, 0},
    {0xEE, (uint8_t []){0xFF}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0xFF}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x30}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x42}, 1, 0},
    {0x61, (uint8_t []){0xE0}, 1, 0},
    {0x62, (uint8_t []){0x40}, 1, 0},
    {0x63, (uint8_t []){0x40}, 1, 0},
    {0x64, (uint8_t []){0x02}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x40}, 1, 0},
    {0x67, (uint8_t []){0x03}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x42}, 1, 0},
    {0x71, (uint8_t []){0xE0}, 1, 0},
    {0x72, (uint8_t []){0x40}, 1, 0},
    {0x73, (uint8_t []){0x40}, 1, 0},
    {0x74, (uint8_t []){0x02}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x40}, 1, 0},
    {0x77, (uint8_t []){0x03}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x38}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x04}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xDC}, 1, 0},
    {0x85, (uint8_t []){0x00}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x38}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x06}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xDE}, 1, 0},
    {0x8D, (uint8_t []){0x00}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x38}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x08}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xE0}, 1, 0},
    {0x95, (uint8_t []){0x00}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x38}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0A}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xE2}, 1, 0},
    {0x9D, (uint8_t []){0x00}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x38}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x03}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xDB}, 1, 0},
    {0xA5, (uint8_t []){0x00}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x38}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x05}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xDD}, 1, 0},
    {0xAD, (uint8_t []){0x00}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x38}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x07}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xDF}, 1, 0},
    {0xB5, (uint8_t []){0x00}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x38}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x09}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xE1}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x22}, 1, 0},
    {0xC1, (uint8_t []){0xAA}, 1, 0},
    {0xC2, (uint8_t []){0x65}, 1, 0},
    {0xC3, (uint8_t []){0x74}, 1, 0},
    {0xC4, (uint8_t []){0x47}, 1, 0},
    {0xC5, (uint8_t []){0x56}, 1, 0},
    {0xC6, (uint8_t []){0x00}, 1, 0},
    {0xC7, (uint8_t []){0x88}, 1, 0},
    {0xC8, (uint8_t []){0x99}, 1, 0},
    {0xC9, (uint8_t []){0x33}, 1, 0},
    {0xD0, (uint8_t []){0x11}, 1, 0},
    {0xD1, (uint8_t []){0xAA}, 1, 0},
    {0xD2, (uint8_t []){0x65}, 1, 0},
    {0xD3, (uint8_t []){0x74}, 1, 0},
    {0xD4, (uint8_t []){0x47}, 1, 0},
    {0xD5, (uint8_t []){0x56}, 1, 0},
    {0xD6, (uint8_t []){0x00}, 1, 0},
    {0xD7, (uint8_t []){0x88}, 1, 0},
    {0xD8, (uint8_t []){0x99}, 1, 0},
    {0xD9, (uint8_t []){0x33}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 0},
};

// ==== vendor_specific_init_version_2: 185 条 ====
static const st77916_lcd_init_cmd_t vendor_specific_init_version_2[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 0},
};

/* ══ 持续全帧刷新（RAMless/TE 面板必需）══════════════════════════════
 * 本板 ST77916 实测：一次性窗口写入 ESP_OK 但不显示（纯黑）——面板不锁存
 * 像素，必须持续重推帧数据（微雪 BSP 的 TE-resistant 模式同源架构）。
 * 合成器照常只做脏区更新到 g_fb；刷新任务以 ~50-60fps 把整个帧缓冲
 * 经 12KB 内部 stage 流式上屏（PSRAM 大块直推会被 esp_lcd bounce 拒之
 * NO_MEM，真机实证）。GRAM 板（216）无此需求=空操作。 */
static const uint16_t *s_fb_src;
static int s_fb_stride;
static volatile bool s_refresh_on;
static uint8_t *s_refr_stage;        /* 内部 DMA stage（display_init 堆干净窗口分配） */
static int s_refr_rows;              /* 实际行数/块（降级链决定） */

static void refresh_task(void *arg)
{
    (void)arg;
    uint8_t *stage = s_refr_stage;
    const int rows = s_refr_rows;
    const int chunk_sz = SW * rows * 2u;
    const int chunks = SH / rows;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t frames = 0, fails = 0, last_report = 0;
    while (s_refresh_on && s_fb_src && s_panel) {
        for (int c = 0; c < chunks && s_refresh_on; c++) {
            const int y0 = c * rows;
            const uint8_t *src = (const uint8_t *)(s_fb_src + (size_t)y0 * s_fb_stride);
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) continue;
            memcpy(stage, src, chunk_sz);
            bool slot = tx_slot_take();
            esp_err_t e = polling_draw(0, y0, SW - 1, y0 + rows - 1, stage, chunk_sz);
            if (e != ESP_OK) fails++;               /* 静默失败会让黑屏无从判读 */
            if (slot && e != ESP_OK) tx_slot_give();
            xSemaphoreGive(s_lock);
        }
        frames++;
        if (frames - last_report >= 150) {           /* ~5s 一条遥测：帧数/失败数 */
            ESP_LOGW(TAG, "刷新遥测：frames=%u fails=%u", (unsigned)frames, (unsigned)fails);
            last_report = frames;
        }
        /* ~60fps 节流：80MHz 下全帧 6.5ms 传输 + 5ms 停顿 */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(5));
    }
    heap_caps_free(stage);
    ESP_LOGW(TAG, "持续刷新已停止");
    vTaskDelete(NULL);
}

/* stage 降级链：块越大吞吐越高；谷底堆小时退到最小可用（2 行=1.4KB）。
 * 在 display_init 的堆干净窗口调用（任务内分配会在谷底失败，真机实证）。 */
static int refresh_stage_alloc(void)
{
    /* 185B 内部堆极紧（素材绑定后健康余量仅 ~10KB）：stage 必须 ≤2.9KB，
     * 否则 fopen(malloc FILE 锁)/events 任务在谷底直接 abort（真机实证）。 */
    static const int rows_opts[] = { 4, 2 };
    for (int i = 0; i < 4; i++) {
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
        if (xTaskCreatePinnedToCore(refresh_task, "lcd_refr", 2560, NULL,
                                    4 /* 低于 watchdog(6)/render(5)，高于 idle */,
                                    NULL, 0) != pdPASS) {
            ESP_LOGE(TAG, "刷新任务创建失败");
            s_refresh_on = false;
        } else {
            ESP_LOGW(TAG, "持续全帧刷新已启动（RAMless 模式，%dfps 目标）", 60);
        }
    }
#else
    ESP_LOGW(TAG, "持续刷新已禁用（CONFIG_MP_LCD_CONTINUOUS_REFRESH=n）");
#endif
}

/* 【polling 写图】绕过 esp_lcd 队列色彩路径：CASET/RASET 走 0x02，像素流
 * 走 0x32 + tx_param（spi_device_polling_transmit，阻塞直到面板吃完）。
 * 真机实证队列+DMA 色彩路径静默丢失（RAMRD 不变），polling 是当前唯一
 * 已验证落显存的通道。 */
static esp_err_t polling_draw(int x1, int y1, int x2, int y2, const uint8_t *px, size_t len)
{
    esp_err_t err;
    err = esp_lcd_panel_io_tx_param(s_io, (uint32_t)0x02 << 24 | 0x2A,
                                    (uint8_t[]) { (x1 >> 8) & 0xFF, x1 & 0xFF,
                                                  (x2 >> 8) & 0xFF, x2 & 0xFF }, 4);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_io_tx_param(s_io, (uint32_t)0x02 << 24 | 0x2B,
                                    (uint8_t[]) { (y1 >> 8) & 0xFF, y1 & 0xFF,
                                                  (y2 >> 8) & 0xFF, y2 & 0xFF }, 4);
    if (err != ESP_OK) return err;
    return esp_lcd_panel_io_tx_param(s_io, (uint32_t)0x32 << 24 | 0x2C, px, len);
}

/* 区域 2 像素对齐（同 co5300：先 clamp 屏内再取偶，右/下缘向内收尾） */
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

esp_err_t display_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    const minipet_pins_t *pins = &MINIPET_ACTIVE_PROFILE.pins;

    s_lock = xSemaphoreCreateMutex();
    s_tx_slots = xSemaphoreCreateCounting(TX_QUEUE_DEPTH, TX_QUEUE_DEPTH);
    if (!s_lock || !s_tx_slots) {
        return ESP_ERR_NO_MEM;
    }

    /* 背光脚先拉高：LCD 点亮的前提（AMOLED 板无此脚） */
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << BL_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&bl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BL(%d) 配置失败: %s", BL_GPIO, esp_err_to_name(err));
        return err;
    }
    backlight_set(true);

    /* QSPI 总线 + 面板 IO（引脚取 profile：CS21/PCLK40/D0-3=46/45/42/41） */
    spi_bus_config_t buscfg = ST77916_PANEL_BUS_QSPI_CONFIG(
                                        pins->lcd.sclk, pins->lcd.sio0,
                                        pins->lcd.sio1, pins->lcd.sio2,
                                        pins->lcd.sio3, SW * SH * 2);
    buscfg.flags = SPICOMMON_BUSFLAG_QUAD;   /* BSP 一致：显式声明四线总线 */
    err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 【读 ID 前必须硬复位】仿微雪 BSP：RST 低 10ms → 高 10ms，否则面板处于
     * 未定义态，RDDID 读回全 00（真机实证 2026-09-29）。 */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << pins->lcd.rst,
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&rst_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RST(%d) 配置失败: %s", pins->lcd.rst, esp_err_to_name(err));
        return err;
    }
    gpio_set_level(pins->lcd.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(pins->lcd.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(
                                           pins->lcd.cs, color_tx_done_cb, NULL);
    /* 【polling 色彩 2026-09-29】诊断实证：参数（polling）能到面板，色彩
     * （queue+DMA）静默丢失（RAMRD 跨启动恒定）。队列深度保留 1（spi_master
     * add_device 必建队列，=0 直接 assert），但驱动所有写图走 polling_draw()
     * —— esp_lcd tx_param 恒为 spi_device_polling_transmit。 */
    io_cfg.trans_queue_depth = 1;
    io_cfg.pclk_hz = CONFIG_MP_LCD_PCLK_HZ;   /* bring-up 期可降频排除信号完整性 */
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                   &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_io 创建失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 【面板序列按 ID 选版，仿微雪官方 BSP】ST77916 有两批面板（ID=0x04 读回
     * 00 7F 7F 7F → v1 / 00 02 7F 7F → v2），序列互不通用；registry 默认序列是
     * 给别家模块的，照抄=花屏且色块无变化（真机实证 2026-09-29）。
     * ID 读取结果无论匹配与否都打日志（bring-up 判据：读回全 0/FF=NACK 级问题）。 */
    st77916_vendor_config_t vc = {
        .flags = { .use_qspi_interface = 1, },
    };
    {
        /* 仿 BSP：ID 读取走 3MHz 专用 IO（读完删除），命令拼装逐字复刻——
         * 寄存器 <<8 后再 | 读 opcode<<24（0x0B_00_04_00，与写路径寄存器位置不同！） */
        uint8_t id[4] = { 0 };
        esp_lcd_panel_io_spi_config_t probe_cfg = io_cfg;
        probe_cfg.pclk_hz = 3 * 1000 * 1000;
        probe_cfg.on_color_trans_done = NULL;
        esp_lcd_panel_io_handle_t probe_io = NULL;
        esp_err_t perr = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                                  &probe_cfg, &probe_io);
        if (perr == ESP_OK) {
            uint32_t rd = (uint32_t)0x04 << 8; rd |= MP_ST77916_OPCODE_READ << 24;
            esp_err_t err = esp_lcd_panel_io_rx_param(probe_io, rd, id, sizeof(id));
            ESP_LOGW(TAG, "RDDID(0x04)@3MHz = %02X %02X %02X %02X（%s）", id[0], id[1], id[2], id[3],
                     err == ESP_OK ? "读取成功" : esp_err_to_name(err));
            esp_lcd_panel_io_del(probe_io);
        } else {
            ESP_LOGE(TAG, "探测 IO 创建失败: %s", esp_err_to_name(perr));
        }
        if (id[0] == 0x00 && id[1] == 0x7F && id[2] == 0x7F && id[3] == 0x7F) {
            vc.init_cmds = vendor_specific_init_version_1;
            vc.init_cmds_size = sizeof(vendor_specific_init_version_1) / sizeof(vendor_specific_init_version_1[0]);
            ESP_LOGW(TAG, "面板 v1 序列（%u 条）", (unsigned)vc.init_cmds_size);
        } else if (id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F) {
            vc.init_cmds = vendor_specific_init_version_2;
            vc.init_cmds_size = sizeof(vendor_specific_init_version_2) / sizeof(vendor_specific_init_version_2[0]);
            ESP_LOGW(TAG, "面板 v2 序列（%u 条）", (unsigned)vc.init_cmds_size);
        } else {
            /* registry 默认序列已被真机证伪（花屏无变化），不匹配也绝不能回落：
             * 猜 v1（BSP 的 ID 全 00 多半是读时序问题而非面板真身未知），日志留痕 */
            vc.init_cmds = vendor_specific_init_version_1;
            vc.init_cmds_size = sizeof(vendor_specific_init_version_1) / sizeof(vendor_specific_init_version_1[0]);
            ESP_LOGE(TAG, "RDDID 未匹配 v1/v2（全 00/FF = 命令没到面板）→ 猜 v1 序列 %u 条",
                     (unsigned)vc.init_cmds_size);
        }
    }
    const esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = pins->lcd.rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vc,
    };
    err = esp_lcd_new_panel_st77916(s_io, &pc, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "st77916 面板创建失败: %s", esp_err_to_name(err));
        return err;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);
    /* 方向：同 216 板终版口径（原生正立，不交换不镜像）；若用户反馈颠倒/镜像
     * 再按组合表调整 */
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);

    s_inited = true;
    ESP_LOGI(TAG, "ST77916(LCD) 就绪 %ux%u QSPI BL=GPIO%d PCLK=%dMHz", SW, SH, BL_GPIO,
             CONFIG_MP_LCD_PCLK_HZ / 1000000);

#if CONFIG_MP_LCD_BRINGUP_TEST
    /* 高对比判读（用户口径：别用黑）：绿↔白整屏交替 8 轮（40s，防错过）；
     * 偶数轮绿屏时中央叠 160x160 红块（blit/PSRAM 路径=应用同款）。
     * 全程检查返回码并打日志——上一次测试跑了没人看也没采日志，判读悬空。 */
    {
        /* 【读回诊断】三段式定位断点层：
         * ① 0x09 RDDPM（电源模式：bit1=sleep out? bit2=display on?）
         * ② 全屏写红 → 0x2E RAMRD 读回 8 像素：非零=数据进了显存(扫描门控问题)，零=写入没落地
         * ③ 再读 0x0A RDDST 交叉验证 */
        esp_lcd_panel_io_spi_config_t probe_cfg = io_cfg;
        probe_cfg.pclk_hz = 3 * 1000 * 1000;
        probe_cfg.on_color_trans_done = NULL;
        esp_lcd_panel_io_handle_t pio = NULL;
        if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &probe_cfg, &pio) == ESP_OK) {
            uint8_t st[4] = { 0 };
            uint32_t rd;
            rd = (uint32_t)0x09 << 8; rd |= MP_ST77916_OPCODE_READ << 24;
            esp_err_t e1 = esp_lcd_panel_io_rx_param(pio, rd, st, 4);
            ESP_LOGW(TAG, "RDDPM(09)=%02X %02X %02X %02X (%s)", st[0], st[1], st[2], st[3], esp_err_to_name(e1));
            rd = (uint32_t)0x0A << 8; rd |= MP_ST77916_OPCODE_READ << 24;
            e1 = esp_lcd_panel_io_rx_param(pio, rd, st, 4);
            ESP_LOGW(TAG, "RDDST(0A)=%02X %02X %02X %02X (%s)", st[0], st[1], st[2], st[3], esp_err_to_name(e1));
            esp_lcd_panel_io_del(pio);
        }

        /* 【强制唤醒】RDDPM 显示面板仍睡眠 → 直接单发 SLPOUT+DISPON 后复读，
         * 判定"序列被吞"还是"机制不同" */
        {
            esp_lcd_panel_io_handle_t wio = NULL;
            if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &probe_cfg, &wio) == ESP_OK) {
                esp_lcd_panel_io_tx_param(wio, (uint32_t)0x02 << 24 | 0x11, NULL, 0);   /* SLPOUT */
                vTaskDelay(pdMS_TO_TICKS(150));
                esp_lcd_panel_io_tx_param(wio, (uint32_t)0x02 << 24 | 0x29, NULL, 0);   /* DISPON */
                vTaskDelay(pdMS_TO_TICKS(50));
                uint8_t st[4] = { 0 };
                uint32_t rd = (uint32_t)0x09 << 8; rd |= MP_ST77916_OPCODE_READ << 24;
                esp_err_t e1 = esp_lcd_panel_io_rx_param(wio, rd, st, 4);
                ESP_LOGW(TAG, "强制唤醒后 RDDPM(09)=%02X %02X %02X %02X (%s)", st[0], st[1], st[2], st[3], esp_err_to_name(e1));
                esp_lcd_panel_io_del(wio);
            }
        }

        display_fill_rect(0, 0, SW, SH, 0xF800);   /* 全屏红（BE 契约） */
        display_wait_tx_idle();

        if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &probe_cfg, &pio) == ESP_OK) {
            uint8_t ram[16] = { 0 };
            uint32_t rd = (uint32_t)0x2E << 8; rd |= MP_ST77916_OPCODE_READ << 24;
            esp_err_t e2 = esp_lcd_panel_io_rx_param(pio, rd, ram, sizeof(ram));
            ESP_LOGW(TAG, "RAMRD(2E)=%02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X (%s)",
                     ram[0], ram[1], ram[2], ram[3], ram[4], ram[5], ram[6], ram[7],
                     ram[8], ram[9], ram[10], ram[11], ram[12], ram[13], ram[14], ram[15],
                     esp_err_to_name(e2));
            esp_lcd_panel_io_del(pio);
        }

        esp_err_t e;
        for (int round = 0; round < 8; round++) {
            e = display_fill_rect(0, 0, SW, SH, 0x07E0);   /* 绿（高字节 0x07 先出） */
            ESP_LOGW(TAG, "r%d 绿 fill=%s", round, esp_err_to_name(e));
            if (round % 2 == 0) {
                /* 仿应用真实路径：内部 DMA 内存 12.8KB stage，4 块推送
                 * （大块 PSRAM 直推会被 esp_lcd bounce buffer 拒之 NO_MEM） */
                uint16_t *sq = heap_caps_malloc(160u * 40u * 2u,
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
                if (sq) {
                    for (int i = 0; i < 160 * 40; i++) sq[i] = 0xF800;
                    for (int row = 0; row < 160; row += 40) {
                        e = display_blit(100, 100 + row, 160, 40, (const uint8_t *)sq);
                        if (e != ESP_OK) ESP_LOGE(TAG, "r%d blit 块%d=%s", round, row, esp_err_to_name(e));
                    }
                    ESP_LOGW(TAG, "r%d 红块 blit(4x160x40 内部)=%s", round, esp_err_to_name(e));
                    heap_caps_free(sq);
                } else {
                    ESP_LOGE(TAG, "r%d 内部 DMA 分配失败", round);
                }
            }
            display_wait_tx_idle();
            vTaskDelay(pdMS_TO_TICKS(2500));

            e = display_fill_rect(0, 0, SW, SH, 0xFFFF);   /* 白 */
            ESP_LOGW(TAG, "r%d 白 fill=%s", round, esp_err_to_name(e));
            display_wait_tx_idle();
            vTaskDelay(pdMS_TO_TICKS(2500));
        }
    }
#endif
    return ESP_OK;
}

void display_set_orientation(bool swap_xy, bool mirror_x, bool mirror_y)
{
    if (!s_inited || !s_panel) {
        ESP_LOGW(TAG, "orientation 忽略：display 未初始化 swap=%d mx=%d my=%d",
                 swap_xy, mirror_x, mirror_y);
        return;
    }
    esp_lcd_panel_swap_xy(s_panel, swap_xy);
    esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y);
    ESP_LOGI(TAG, "orientation swap=%d mx=%d my=%d", swap_xy, mirror_x, mirror_y);
}

void display_wait_tx_idle(void)
{
    if (!s_inited) return;
    if (tx_slot_take()) tx_slot_give();
}

bool display_tx_busy(void)
{
    if (!s_inited || !s_tx_slots) return false;
    return uxSemaphoreGetCount(s_tx_slots) == 0;
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
    bool slot = tx_slot_take();
    if (aw == w && ah == h) {
        err = polling_draw(x1, y1, x2, y2, rgb565_be, (size_t)aw * ah * 2u);
    } else {
        /* 奇数区域：PSRAM 暂存补齐（同 co5300 语义） */
        size_t sz = (size_t)aw * ah * 2u;
        uint8_t *tmp = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp) { if (slot) tx_slot_give(); xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
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
        err = polling_draw(x1, y1, x2, y2, tmp, sz);
        heap_caps_free(tmp);
    }
    if (slot && err != ESP_OK) tx_slot_give();

    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        static int64_t s_last_err_log;
        int64_t now = esp_timer_get_time();
        if (now - s_last_err_log > 5000000) {
            ESP_LOGE(TAG, "draw_bitmap 失败: %s", esp_err_to_name(err));
            s_last_err_log = now;
        }
    }
    return err;
}

esp_err_t display_brightness(uint8_t pct)
{
    /* LCD 亮度=背光通断（PWM 调光留待后续；pct>0 亮、0 灭） */
    if (!s_inited) {
        return ESP_OK;
    }
    backlight_set(pct > 0);
    return ESP_OK;
}

esp_err_t display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                            uint16_t rgb565)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > SW || y + h > SH) {
        return ESP_ERR_INVALID_ARG;
    }

    static uint8_t line[2][360 * 2] __attribute__((aligned(64)));
    int x1 = x, y1 = y, x2 = x + w - 1, y2 = y + h - 1;
    even_round(&x1, &y1, &x2, &y2);
    const int aw = x2 - x1 + 1;
    if (aw > (int)sizeof(line[0]) / 2) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t hi = (uint8_t)(rgb565 >> 8);
    const uint8_t lo = (uint8_t)(rgb565 & 0xFF);
    for (int i = 0; i < aw; i++) {
        line[0][2 * i]     = hi;
        line[0][2 * i + 1] = lo;
        line[1][2 * i]     = hi;
        line[1][2 * i + 1] = lo;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    for (int yy = y1; yy <= y2 && err == ESP_OK; yy += 2) {
        bool slot = tx_slot_take();
        err = esp_lcd_panel_draw_bitmap(s_panel, x1, yy, x2 + 1, yy + 2, line);
        if (slot && err != ESP_OK) tx_slot_give();
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t display_set_sleep(bool sleep)
{
    if (!s_inited || !s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    backlight_set(!sleep);                    /* LCD 关背光即时黑屏 */
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, !sleep);
    if (!sleep) {
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return err;
}
