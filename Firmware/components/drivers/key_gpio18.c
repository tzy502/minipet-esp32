/**
 * @file key_gpio18.c
 * @brief 菜单按键实现：双边沿中断 + 20ms 时间窗消抖（ISR 内完成）
 */
#include "key_gpio18.h"

#include <stdatomic.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "amoled216.h"

static const char *TAG = "key18";

/* 消抖窗口：两次有效电平变化至少间隔 20ms */
#define KEY_DEBOUNCE_US  20000

static void (*s_cb)(void *arg);
static void *s_cb_arg;
static _Atomic int64_t s_last_edge_us;
static _Atomic bool    s_fired_for_press; /* 本轮按下是否已触发 */

static void key_isr_handler(void *arg)
{
    (void)arg;
    const int64_t now = esp_timer_get_time();
    const int64_t last = atomic_load(&s_last_edge_us);
    const bool pressed = (gpio_get_level(MINIPET_PROFILE_AMOLED216.pins.key.menu) == 0);

    /* 20ms 窗口内的任何电平跳变都视为抖动，仅刷新时间戳 */
    if (now - last < KEY_DEBOUNCE_US) {
        atomic_store(&s_last_edge_us, now);
        return;
    }
    atomic_store(&s_last_edge_us, now);

    /* 确认的按下沿：一次按压只触发一次回调 */
    if (pressed && !atomic_load(&s_fired_for_press)) {
        atomic_store(&s_fired_for_press, true);
        if (s_cb) {
            s_cb(s_cb_arg); /* 中断上下文：FROM_ISR 动作 only */
        }
    } else if (!pressed) {
        atomic_store(&s_fired_for_press, false); /* 释放，为下一轮上膛 */
    }
}

esp_err_t key_gpio18_init(void)
{
    const int8_t pin = MINIPET_PROFILE_AMOLED216.pins.key.menu;

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* 外部 R18 10K 上拉 + 内部弱上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,    /* 按下/释放沿都进 ISR 做状态机 */
    };
    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        return err;
    }

    gpio_install_isr_service(0); /* 已装过返回 INVALID_STATE，无碍 */
    err = gpio_isr_handler_add(pin, key_isr_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    atomic_store(&s_last_edge_us, esp_timer_get_time());
    atomic_store(&s_fired_for_press, false);
    ESP_LOGI(TAG, "菜单键就绪 GPIO%d（上拉/按下接地/20ms 消抖）", pin);
    return ESP_OK;
}

bool key_gpio18_pressed(void)
{
    return gpio_get_level(MINIPET_PROFILE_AMOLED216.pins.key.menu) == 0;
}

void key_gpio18_set_callback(void (*on_press)(void *arg), void *arg)
{
    s_cb_arg = arg;
    s_cb = on_press;
}
