/**
 * @file touch_cst816.c
 * @brief CST816S 触摸驱动实现（Waveshare ESP32-S3-Touch-LCD-1.85B）
 *
 * 硬件事实（官方 BSP / 板卡 wiki 实锤）：
 *  - 7-bit I2C 地址 0x15，挂在共享总线 SCL=10 / SDA=11（i2c_bus 单实例）；
 *  - TP_RST = GPIO1，TP_INT = GPIO4（profiles/lcd185b.h .pins.touch，引脚唯一来源）；
 *  - 同总线其它器件（QMI8658/PCF85063）均为 400K 档 → 本驱动同档。
 *
 * CST816S 协议（**以官方 esp_lcd_touch_cst816s v1.1.0 为准**，2026-10-01 定稿）：
 *  I2C 从寄存器 **0x02** 起连续读 5 字节（官方 DATA_START_REG=0x02）：
 *    0x00：手势码（0x00=无 …，本驱动不透出）
 *    0x01：触点数（与 0x02 处重复编码，本驱动不读）
 *    0x02：X 高位（12bit 的 bit11..8，占低 4 位）      → d[0]
 *    0x03：X 低位（bit7..0）                          → d[1]
 *    0x04：Y 高位（bit11..8，占低 4 位）              → d[2]
 *    0x05：Y 低位（bit7..0）                          → d[3]
 *    0x06：触点 ID / 事件（不读）
 *  12bit 坐标重组：x = (d[0] & 0x0F) << 8 | d[1]；y = (d[2] & 0x0F) << 8 | d[3]。
 *  （官方 bitfield：num / x_h:4 / x_l / y_h:4 / y_l，与本式逐位等价。）
 *  【演进史 · 教训】本驱动初版按"0x00 起 6 字节 + [count,XH,XY,YL] 位打包"写，
 *  与官方口径整体错位一字节 → 真机坐标恒为垃圾、触屏形同不通。**移植触摸驱动
 *  的第一件事是去官方 managed_component 里核 DATA_START_REG 与 bitfield**，
 *  不要照抄另一块板的芯片（216 是 CST9220，帧格式完全不同）。
 *  【坐标方向】X/Y 原始值透传 = 面板原生坐标（官方 BSP touch_flags 全 0）。
 *  360→480 上采样与 swap/镜像映射在消费层（input_dispatch.c 的 tmap_apply）。
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
/* ══ 【帧窗口定稿 2026-10-01：0x02 起 6 字节，与官方驱动逐字一致】════════════
 * 依据 espressif__esp_lcd_touch_cst816s v1.1.0：
 *   #define DATA_START_REG (0x02)
 *   typedef struct { uint8_t num; uint8_t x_h:4; uint8_t :4; uint8_t x_l;
 *                    uint8_t y_h:4; uint8_t :4; uint8_t y_l; } data_t;
 * 寄存器图：0x00=手势码 0x01=触点数 0x02=X高位(低4位) 0x03=X低位
 *           0x04=Y高位(低4位) 0x05=Y低位 0x06=触点ID 0x07=…（见官方 read_id 0xA7）
 * 上一版从 0x00 起读 6 字节 + 按 [count,XH,XY,YL] 解析 → **每个字段错位**，
 * 真机坐标恒为垃圾（触屏不灵的真根因）。 */
#define CST816_REG_DATA    0x02    /* 数据帧起始寄存器（官方 DATA_START_REG） */
#define CST816_FRAME_LEN   5       /* num + xh + xl + yh + yl（读到 0x06 为止，ID 不读） */
#define CST816_REG_CHIPID  0xA7    /* 芯片 ID（官方 read_id 同址；取证用） */

/* 帧内偏移（num 在 d[0] 是"重复编码"的触点数，与 0x01 处同值） */
#define CST816_OFF_COUNT   0       /* d0: 触点数（官方 num 字段） */
#define CST816_OFF_XH      1       /* d1: X 高位（bit11..8，取低 4 位） */
#define CST816_OFF_XL      2       /* d2: X 低位（bit7..0） */
#define CST816_OFF_YH      3       /* d3: Y 高位（bit11..8，取低 4 位） */
#define CST816_OFF_YL      4       /* d4: Y 低位（bit7..0） */

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

    /* 【指纹取证 2026-10-01】摸清这块屏到底挂的是谁、数据窗口起在哪：
     *   ① 芯片 ID 寄存器 0xA7（官方 read_id 同址；CST816S 家族常见 0xB4/0xB5/0xB6）
     *   ② 0x00..0x07 的原始字节（**开机无人触摸时读**：坐标寄存器应近 0，
     *      触点数应 0 → 哪个字节是点数、窗口从哪起，一眼可判）
     * 结论出来后把这段降到 LOGD 或删掉。 */
    {
        uint8_t id = 0, win[8] = { 0 };
        i2c_bus_read_reg8v(s_dev, CST816_REG_CHIPID, &id, 1);
        i2c_bus_read_reg8v(s_dev, 0x00, win, sizeof(win));
        ESP_LOGW(TAG, "指纹：chipID@0xA7=%02X | reg00..07=[%02X %02X %02X %02X %02X %02X %02X %02X]"
                      "（无触摸时应为 点数=0、坐标≈0）",
                 id, win[0], win[1], win[2], win[3], win[4], win[5], win[6], win[7]);
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
    out->pressed = (fingers >= 1 && fingers <= 5);   /* >5 = 非法帧，按未按下处理 */
    /* 12bit 重组（与官方 bitfield 逐位等价）：
     *   x = (d[x_h] & 0x0F) << 8 | d[x_l]     y = (d[y_h] & 0x0F) << 8 | d[y_l]
     * 原始值透传（0..359 面板原生），方向/镜像/360→480 上采样在消费层
     * （input_dispatch.c 的 tmap_apply + ×4/3）。 */
    out->x = (uint16_t)((((uint16_t)d[CST816_OFF_XH] & 0x0F) << 8) | d[CST816_OFF_XL]);
    out->y = (uint16_t)((((uint16_t)d[CST816_OFF_YH] & 0x0F) << 8) | d[CST816_OFF_YL]);
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
