/**
 * @file touch_cst816.c
 * @brief CST816S 触摸驱动实现（Waveshare ESP32-S3-Touch-LCD-1.85B）
 *
 * 硬件事实（官方 BSP / 板卡 wiki 实锤）：
 *  - 7-bit I2C 地址 0x15，挂在共享总线 SCL=10 / SDA=11（i2c_bus 单实例）；
 *  - TP_RST = GPIO1，TP_INT = GPIO4（profiles/lcd185b.h .pins.touch，引脚唯一来源）；
 *  - 同总线其它器件（QMI8658/PCF85063）均为 400K 档 → 本驱动同档。
 *
 * CST816S 协议（公开常识，Hynitron CST816S 数据帧，写入备查）：
 *  I2C 从寄存器 0x00 起连续读多点帧（本驱动突发读 6 字节 d[0..5]，d[6] 可另读）：
 *    0x00：bit3..0 = 触点数（低 4 位有效）
 *    0x01：手势码（0x00=无 …，本驱动暂不透出，留手势扩展）
 *    0x03：X 高 8 位（12bit 坐标的 bit11..4）
 *    0x04：(X 低 4 位 << 4) | Y 高 4 位 —— 高 nibble=X bit3..0，低 nibble=Y bit11..8
 *    0x05：Y 低 8 位（bit7..0）
 *    0x06：触点 ID / 事件（press/down/up，本驱动暂不透出）
 *  12bit 坐标重组：x = (d[3] << 4) | (d[4] >> 4)；y = ((d[4] & 0x0F) << 8) | d[5]。
 *  【TODO 坐标方向校准】X/Y 原始值直接透传。360 圆屏原生竖向（面板 0..359），
 *  与本工程合成器 480×480 空间的映射（swap/镜像/缩放）等真机按四角+中心实测
 *  后再定——方法论与 216 板 tmap 8 候选自校准一致（board216 input_dispatch.c
 *  的 tmap_apply 先例），届时在消费层（input 任务）加映射表，本驱动保持透传。
 *
 * 驱动风格：纯轮询（同 key_gpio0 的无 ISR 风格）——init 不装任何中断服务，
 * TP_INT(GPIO4) 只配输入留作将来；touch_cst816_set_isr_callback() 首次注册
 * 时才懒挂 GPIO ISR（见该函数注释）。探测失败返回 ESP_OK 只打告警，
 * 绝不上抛阻断启动（触摸缺失降级，同 touch_cst9220 先例）。
 */
#include "touch_cst816.h"

#if CONFIG_MP_TOUCH_CST816   /* 板级开关（main/Kconfig.projbuild，default y） */

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "lcd185b.h"

static const char *TAG = "cst816";

#define CST816_ADDR        0x15    /* 7-bit */
#define CST816_I2C_HZ      400000  /* 同总线 QMI8658/PCF85063 档位 */
#define CST816_REG_DATA    0x00    /* 数据帧起始寄存器 */
#define CST816_FRAME_LEN   6       /* 突发读 6 字节（0x06 触点 ID 暂不读） */

/* 帧内偏移（若与实测不符只需改这里） */
#define CST816_OFF_COUNT   0       /* d0: 低 4 位 = 触点数 */
#define CST816_OFF_GESTURE 1       /* d1: 手势码（暂不透出） */
#define CST816_OFF_XH      3       /* d3: X bit11..4 */
#define CST816_OFF_XY      4       /* d4: 高 nibble=X bit3..0，低 nibble=Y bit11..8 */
#define CST816_OFF_YL      5       /* d5: Y bit7..0 */

/* 复位时序（官方 BSP 口径）：高 10ms → 低 10ms → 高 10ms → 等 50ms 后探测 0x15 */
#define CST816_RST_PRE_MS   10
#define CST816_RST_LOW_MS   10
#define CST816_RST_POST_MS  10
#define CST816_RDY_WAIT_MS  50

static i2c_master_dev_handle_t s_dev;
static void (*s_cb)(void *arg);
static void *s_cb_arg;
static bool s_isr_installed;

/* INT 中断服务（懒安装：首次 set_isr_callback 注册非空回调才挂，见下） */
static void touch_isr_handler(void *arg)
{
    (void)arg;
    /* 中断上下文：只做通知，不做 I2C */
    if (s_cb) {
        s_cb(s_cb_arg);
    }
}

esp_err_t touch_cst816_init(void)
{
    if (s_dev) {
        return ESP_OK;                        /* 幂等：已初始化直接返回 */
    }
    const minipet_pins_t *pins = &MINIPET_ACTIVE_PROFILE.pins;
    esp_err_t err;

    err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }

    /* 复位时序：高 10ms → 低 10ms → 高 10ms → 等 50ms 内部就绪再探测 */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << pins->touch.rst,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(pins->touch.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(CST816_RST_PRE_MS));
    gpio_set_level(pins->touch.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(CST816_RST_LOW_MS));
    gpio_set_level(pins->touch.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(CST816_RST_POST_MS));
    vTaskDelay(pdMS_TO_TICKS(CST816_RDY_WAIT_MS));

    /* INT：输入上拉（开漏低有效），不挂中断——纯轮询 bring-up，留作将来 */
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << pins->touch.intr,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);

    err = i2c_bus_register_device("cst816", CST816_ADDR, CST816_I2C_HZ, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    /* 在线探测：失败不阻断启动（触摸坏不应导致整机不可用，app 层降级） */
    err = i2c_bus_probe(CST816_ADDR);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CST816S @0x%02X 探测失败（%s），触摸降级为不可用",
                 CST816_ADDR, esp_err_to_name(err));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "CST816S 就绪 INT=%d RST=%d（纯轮询）",
             pins->touch.intr, pins->touch.rst);
    return ESP_OK;
}

esp_err_t touch_cst816_read(touch_point_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t d[CST816_FRAME_LEN];
    esp_err_t err = i2c_bus_read_reg8v(s_dev, CST816_REG_DATA, d, sizeof(d));
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t fingers = d[CST816_OFF_COUNT] & 0x0F;
    out->fingers = fingers;
    out->pressed = (fingers >= 1);
    /* 12bit 重组（布局见文件头注释）：X=XH:XY高nibble，Y=XY低nibble:YL。
     * 原始值透传，方向/镜像/缩放映射留真机校准（TODO 见文件头）。 */
    out->x = (uint16_t)((((uint16_t)d[CST816_OFF_XH]) << 4) |
                        (d[CST816_OFF_XY] >> 4));
    out->y = (uint16_t)(((((uint16_t)d[CST816_OFF_XY]) & 0x0F) << 8) |
                        d[CST816_OFF_YL]);
    return ESP_OK;
}

void touch_cst816_set_isr_callback(void (*cb)(void *arg), void *arg)
{
    /* 注册/取消是原子指针操作，不加锁（对象指针读写对齐后天然原子） */
    s_cb_arg = arg;
    s_cb = cb;

    /* 懒安装：首次注册非空回调才挂 GPIO ISR（bring-up 期零中断参与） */
    if (cb && !s_isr_installed) {
        const int pin = MINIPET_ACTIVE_PROFILE.pins.touch.intr;
        gpio_install_isr_service(0);          /* 已装过返回 INVALID_STATE，无碍 */
        if (gpio_isr_handler_add(pin, touch_isr_handler, NULL) == ESP_OK) {
            /* 边沿最后使能：服务+handler 就位前不使能，防无 handler 的裸中断 */
            gpio_set_intr_type(pin, GPIO_INTR_NEGEDGE);
            s_isr_installed = true;
            ESP_LOGI(TAG, "INT 通知已挂（GPIO%d 下降沿）", pin);
        }
    }
}

#endif /* CONFIG_MP_TOUCH_CST816 */
