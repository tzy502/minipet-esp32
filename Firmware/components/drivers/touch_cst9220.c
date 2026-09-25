/**
 * @file touch_cst9220.c
 * @brief CST9220 触摸驱动实现
 *
 * BRING-UP 注意：
 *  - 7-bit 地址 0x5A（微雪 demo 口径）；若 i2c_bus_probe 不在线，
 *    先用 i2cdetect 扫总线确认（个别批次可能挂 0x14/0x15）。
 *  - 数据帧布局（d[0]=手势, d[1]=触点数低 4 位, d[2..5]=首点 XY）按微雪
 *    demo 抄录；若坐标异常，用逻辑分析仪核对帧偏移（宏定义集中在此，好改）。
 */
#include "touch_cst9220.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "amoled216.h"

static const char *TAG = "cst9220";

#define CST9220_ADDR        0x5A    /* 7-bit */
#define CST9220_I2C_HZ      400000
#define CST9220_REG_DATA    0x00    /* 数据帧起始寄存器 */
#define CST9220_FRAME_LEN   6       /* 突发读 6 字节 */

/* 帧内偏移（若与实测不符只需改这里） */
#define CST9220_OFF_GESTURE 0       /* d0: 手势 id */
#define CST9220_OFF_COUNT   1       /* d1: 低 4 位 = 触点数 */
#define CST9220_OFF_XH      2       /* d2: X 高 4 位（低 nibble 有效） */
#define CST9220_OFF_XL      3       /* d3: X 低 8 位 */
#define CST9220_OFF_YH      4       /* d4: Y 高 4 位（低 nibble 有效） */
#define CST9220_OFF_YL      5       /* d5: Y 低 8 位 */

/* 复位时序：RST 低 >=1ms，释放后等内部自校准 */
#define CST9220_RST_LOW_MS  2
#define CST9220_RST_WAIT_MS 60

static i2c_master_dev_handle_t s_dev;
static void (*s_cb)(void *arg);
static void *s_cb_arg;

/* INT 中断服务（服务级安装见 drivers.h 的 drivers_gpio_isr_service_install） */
static void touch_isr_handler(void *arg)
{
    (void)arg;
    /* 中断上下文：只做通知，不做 I2C */
    if (s_cb) {
        s_cb(s_cb_arg);
    }
}

esp_err_t touch_cst9220_init(void)
{
    const minipet_pins_t *pins = &MINIPET_PROFILE_AMOLED216.pins;
    esp_err_t err;

    err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }

    /* RST：输出，默认拉高（释放复位） */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << pins->touch.rst,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(pins->touch.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(CST9220_RST_LOW_MS));
    gpio_set_level(pins->touch.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(CST9220_RST_WAIT_MS));

    /* INT：输入上拉，下降沿触发（INT 引脚开漏低有效） */
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << pins->touch.intr,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&int_cfg);

    err = i2c_bus_register_device("cst9220", CST9220_ADDR, CST9220_I2C_HZ, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    /* 在线探测：失败不阻断启动（触摸坏不应导致整机不可用，app 层降级） */
    err = i2c_bus_probe(CST9220_ADDR);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CST9220 @0x%02X 探测失败（%s），触摸降级为不可用",
                 CST9220_ADDR, esp_err_to_name(err));
        return ESP_OK;
    }

    gpio_install_isr_service(0); /* 已装过返回 INVALID_STATE，无碍 */
    gpio_isr_handler_add(pins->touch.intr, touch_isr_handler, NULL);

    ESP_LOGI(TAG, "CST9220 就绪 INT=%d RST=%d", pins->touch.intr, pins->touch.rst);
    return ESP_OK;
}

esp_err_t touch_cst9220_read(touch_point_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t d[CST9220_FRAME_LEN];
    esp_err_t err = i2c_bus_read_reg8v(s_dev, CST9220_REG_DATA, d, sizeof(d));
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t fingers = d[CST9220_OFF_COUNT] & 0x0F;
    out->fingers = fingers;
    out->pressed = (fingers >= 1);
    out->x = (uint16_t)(((d[CST9220_OFF_XH] & 0x0F) << 8) | d[CST9220_OFF_XL]);
    out->y = (uint16_t)(((d[CST9220_OFF_YH] & 0x0F) << 8) | d[CST9220_OFF_YL]);
    return ESP_OK;
}

void touch_cst9220_set_isr_callback(void (*cb)(void *arg), void *arg)
{
    /* 注册/取消是原子指针操作，不加锁（对象指针读写对齐后天然原子） */
    s_cb_arg = arg;
    s_cb = cb;
}
