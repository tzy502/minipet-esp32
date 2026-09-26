/**
 * @file key_gpio0.c
 * @brief 中排按键（GPIO0）实现：纯轮询消抖，无 ISR
 *
 * 与 key_gpio18 的差异：GPIO0 与 CHIP_PU/复位时序相关（strapping 脚），
 * 不挂任何中断（intr_type=NEGRATE/ANYEDGE 都可能影响唤醒/复位边缘行为），
 * 只配输入+内部上拉，消抖状态机由 key_gpio0_tick() 在调用方任务里跑。
 */
#include "key_gpio0.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "key0";

/* 与菜单键同口径：原始电平稳定 30ms 才算确认沿 */
#define KEY0_DEBOUNCE_MS      30
/* 确认释放后再静止 80ms 才重新上膛（吸收弹回抖动，防一次按压双触发） */
#define KEY0_RELEASE_QUIET_MS 80

esp_err_t key_gpio0_init(void)
{
    const int pin = KEY_GPIO0_PIN;

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,      /* 只输入——GPIO0 绝不可输出 */
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* 内部上拉：按下接地 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,    /* 复位时序相关脚，不挂中断 */
    };
    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "中键就绪 GPIO%d（输入/内部上拉/按下接地/30ms 轮询消抖，无 ISR）", pin);
    return ESP_OK;
}

bool key_gpio0_pressed(void)
{
    return gpio_get_level(KEY_GPIO0_PIN) == 0;
}

bool key_gpio0_tick(void)
{
    static bool raw_last = false;
    static bool stable_pressed = false;
    static int64_t raw_change_ms = 0;
    static bool latched = false;         /* 本轮按压已触发：未见释放保持闩住 */
    static bool release_pending = false; /* 已确认释放，80ms 静止确认计时中 */
    static int64_t released_ms = 0;

    bool pressed = key_gpio0_pressed();
    int64_t now = esp_timer_get_time() / 1000;

    if (pressed != raw_last) {           /* 原始电平变化，重开 30ms 防抖窗 */
        raw_last = pressed;
        raw_change_ms = now;
    }
    bool fire = false;
    if ((now - raw_change_ms) >= KEY0_DEBOUNCE_MS && pressed != stable_pressed) {
        stable_pressed = pressed;
        if (stable_pressed) {
            release_pending = false;     /* 弹回按下：静止确认作废，闩继续关 */
            if (!latched) {
                latched = true;          /* 只在确认按下沿触发一次 */
                fire = true;
            }
        } else {
            release_pending = true;
            released_ms = now;
        }
    }

    if (latched && release_pending && !stable_pressed &&
        (now - released_ms) >= KEY0_RELEASE_QUIET_MS) {
        latched = false;                 /* 开闩：允许下一次按压触发 */
    }
    return fire;
}
