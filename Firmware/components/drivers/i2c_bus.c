/**
 * @file i2c_bus.c
 * @brief 共享 I2C 总线实现：i2c_master 新 API + 器件注册表 + 总线互斥
 *
 * 已知坑（docs/ai Waveshare wiki）：GPIO14/15 一条总线挂五个器件，
 * 任何驱动绕过本模块直接操作总线都会破坏时序，禁止。
 */
#include "i2c_bus.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "amoled216.h"

static const char *TAG = "i2c_bus";

/** 事务超时：100ms 足够 400kHz 下最长传输，超时视为主器件/器件异常 */
#define I2C_BUS_TIMEOUT_TICKS pdMS_TO_TICKS(100)

typedef struct {
    char                     name[12];
    uint16_t                 addr7;
    uint32_t                 scl_hz;
    i2c_master_dev_handle_t  handle;
} i2c_bus_entry_t;

static i2c_master_bus_handle_t s_bus;
static SemaphoreHandle_t       s_lock;
static i2c_bus_entry_t         s_devs[I2C_BUS_MAX_DEVICES];
static int                     s_ndev;

/* ---------------- 锁封装 ---------------- */

static inline bool lock_take(void)
{
    if (!s_lock) {
        return false;
    }
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE;
}

static inline void lock_give(void)
{
    xSemaphoreGive(s_lock);
}

/* ---------------- 公共 API ---------------- */

esp_err_t i2c_bus_init(void)
{
    if (s_bus) {
        return ESP_OK; /* 幂等 */
    }

    const minipet_pins_t *pins = &MINIPET_PROFILE_AMOLED216.pins;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port           = I2C_NUM_0,
        .sda_io_num         = pins->i2c.sda,          /* GPIO15 */
        .scl_io_num         = pins->i2c.scl,          /* GPIO14 */
        .clk_source         = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt  = 7,                      /* 推荐默认：滤除 <7 个源时钟毛刺 */
        .intr_priority      = 0,
        .trans_queue_depth  = 0,                      /* 全部同步事务，不用中断队列 */
        .flags = {
            /* 板上通常已有外部上拉；内部弱上拉一并打开，双保险 */
            .enable_internal_pullup = true,
            .allow_pd               = false,
        },
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus 失败: %s", esp_err_to_name(err));
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "共享 I2C 总线就绪 SCL=%d SDA=%d",
             pins->i2c.scl, pins->i2c.sda);
    return ESP_OK;
}

esp_err_t i2c_bus_register_device(const char *name, uint16_t addr7, uint32_t scl_hz,
                                  i2c_master_dev_handle_t *out_handle)
{
    if (!s_bus) {
        ESP_LOGE(TAG, "总线未初始化，先调 i2c_bus_init()");
        return ESP_ERR_INVALID_STATE;
    }
    if (!name || !out_handle || strlen(name) >= sizeof(s_devs[0].name)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 幂等：同名直接复用 */
    i2c_master_dev_handle_t exist = i2c_bus_find(name);
    if (exist) {
        *out_handle = exist;
        return ESP_OK;
    }
    if (s_ndev >= I2C_BUS_MAX_DEVICES) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr7,
        .scl_speed_hz   = scl_hz,
    };

    i2c_master_dev_handle_t handle = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册器件 %s(0x%02X) 失败: %s",
                 name, addr7, esp_err_to_name(err));
        return err;
    }

    strlcpy(s_devs[s_ndev].name, name, sizeof(s_devs[s_ndev].name));
    s_devs[s_ndev].addr7  = addr7;
    s_devs[s_ndev].scl_hz = scl_hz;
    s_devs[s_ndev].handle = handle;
    s_ndev++;

    *out_handle = handle;
    ESP_LOGD(TAG, "注册 I2C 器件 %s @0x%02X (%lu Hz)", name, addr7, (unsigned long)scl_hz);
    return ESP_OK;
}

i2c_master_dev_handle_t i2c_bus_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (int i = 0; i < s_ndev; i++) {
        if (strcmp(s_devs[i].name, name) == 0) {
            return s_devs[i].handle;
        }
    }
    return NULL;
}

esp_err_t i2c_bus_probe(uint16_t addr7)
{
    if (!s_bus) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_probe(s_bus, addr7, I2C_BUS_TIMEOUT_TICKS);
    lock_give();
    return err;
}

/* ---------------- 事务辅助 ---------------- */

esp_err_t i2c_bus_write(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_transmit(dev, buf, len, I2C_BUS_TIMEOUT_TICKS);
    lock_give();
    return err;
}

esp_err_t i2c_bus_read(i2c_master_dev_handle_t dev, uint8_t *buf, size_t len)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_receive(dev, buf, len, I2C_BUS_TIMEOUT_TICKS);
    lock_give();
    return err;
}

esp_err_t i2c_bus_write_reg8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    return i2c_bus_write_reg8v(dev, reg, &val, 1);
}

esp_err_t i2c_bus_write_reg8v(i2c_master_dev_handle_t dev, uint8_t reg,
                              const uint8_t *data, size_t len)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 寄存器地址 + 数据拼一次写（多数器件要求地址与数据同帧） */
    uint8_t tmp[16];
    if (len + 1 > sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    tmp[0] = reg;
    memcpy(&tmp[1], data, len);

    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_transmit(dev, tmp, len + 1, I2C_BUS_TIMEOUT_TICKS);
    lock_give();
    return err;
}

esp_err_t i2c_bus_read_reg8v(i2c_master_dev_handle_t dev, uint8_t reg,
                             uint8_t *buf, size_t len)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, buf, len,
                                                I2C_BUS_TIMEOUT_TICKS);
    lock_give();
    return err;
}

void i2c_bus_dump(void)
{
    ESP_LOGI(TAG, "I2C 注册表（%d 个器件）:", s_ndev);
    for (int i = 0; i < s_ndev; i++) {
        ESP_LOGI(TAG, "  [%d] %-8s 0x%02X @ %lu Hz", i,
                 s_devs[i].name, s_devs[i].addr7, (unsigned long)s_devs[i].scl_hz);
    }
}
