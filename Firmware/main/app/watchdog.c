/**
 * watchdog.c — 渲染心跳看门狗 + 三振熔断（E14 / software-design 4.1）
 *
 * 机制：
 *   1) render 任务（APP 核）启动时 watchdog_subscribe_render_task()
 *      将自身加入 esp_task_wdt；每帧 render_tick() 后 watchdog_kick()
 *      → esp_task_wdt_reset() + 软件心跳时间戳。
 *   2) 本模块的监控任务（PRO 核，高优先级）每秒检查心跳年龄：
 *        age > MP_WDT_TIMEOUT_MS  → 判定一次「渲染卡死」：
 *            strike = NVS 计数 + 1
 *            strike <  3  → esp_restart()（宠物永不黑屏：重启自愈）
 *            strike >= 3  → 熔断：纯文本错误（内嵌 5x7 字体 + display_fill_rect，
 *                           绝不进渲染路径）→ 显示 15s → display_off() → 系统挂起待机
 *   3) 稳定窗口：开机后 MP_STABLE_CLEAR_MS（10 分钟）内心跳一直健康
 *      → NVS 计数清零（防「久远的历史超时」误杀）。
 *
 * TWDT 本身配置为不 panic（trigger_panic=false），由本模块决策重启/熔断，
 * 避免 IDF panic 处理器在渲染栈损坏时反而不受控。
 */
#include "watchdog.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "app_core.h"
#include "hal_contract.h"

#define MP_WDT_TIMEOUT_MS       5000    /* 30fps 下 150 帧无心跳 = 卡死 */
#define MP_WDT_STRIKE_MAX       3       /* 连续 3 次超时 → 熔断（E14） */
#define MP_WDT_STABLE_CLEAR_MS  (10u * 60u * 1000u)  /* 稳定 10 分钟清零 */
#define MP_WDT_FATAL_SHOW_MS    15000   /* 熔断后文本停留时间，然后关屏 */

static volatile int64_t s_last_kick_ms;
static volatile bool    s_fatal_latched;      /* 熔断后停检 */
static volatile uint8_t s_strikes;
static bool             s_was_fatal_last_boot;

/* ------------------------------------------------------------------ */
/* 内嵌 5x7 ASCII 字体（列字节 ×5，bit0=顶行）。                          */
/* 只需覆盖错误文案常用字符：A-Z 0-9 空格 : . - / ! ? _ > < =            */
/* ------------------------------------------------------------------ */
static const uint8_t s_font5x7[128][5] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['!'] = {0x00, 0x00, 0x5F, 0x00, 0x00},
    ['-'] = {0x00, 0x00, 0x08, 0x00, 0x00},
    ['.'] = {0x00, 0x00, 0x40, 0x00, 0x00},
    ['/'] = {0x60, 0x30, 0x18, 0x0C, 0x06},
    ['0'] = {0x3E, 0x41, 0x49, 0x4D, 0x36},
    ['1'] = {0x40, 0x42, 0x3F, 0x40, 0x40},
    ['2'] = {0x62, 0x51, 0x49, 0x45, 0x43},
    ['3'] = {0x41, 0x49, 0x49, 0x49, 0x36},
    ['4'] = {0x18, 0x14, 0x73, 0x7F, 0x08},
    ['5'] = {0x47, 0x45, 0x45, 0x5D, 0x39},
    ['6'] = {0x3E, 0x49, 0x49, 0x49, 0x38},
    ['7'] = {0x01, 0x01, 0x71, 0x0F, 0x03},
    ['8'] = {0x3E, 0x49, 0x49, 0x49, 0x3E},
    ['9'] = {0x06, 0x49, 0x49, 0x79, 0x3E},
    [':'] = {0x00, 0x00, 0x22, 0x00, 0x00},
    ['<'] = {0x00, 0x08, 0x14, 0x22, 0x41},
    ['='] = {0x24, 0x24, 0x24, 0x24, 0x24},
    ['>'] = {0x41, 0x22, 0x14, 0x08, 0x00},
    ['?'] = {0x02, 0x01, 0x49, 0x05, 0x03},
    ['A'] = {0x1C, 0x0A, 0x09, 0x0A, 0x1C},
    ['B'] = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ['C'] = {0x3E, 0x41, 0x41, 0x41, 0x41},
    ['D'] = {0x7F, 0x41, 0x41, 0x41, 0x3E},
    ['E'] = {0x7F, 0x49, 0x49, 0x49, 0x7F},
    ['F'] = {0x3F, 0x09, 0x09, 0x01, 0x01},
    ['G'] = {0x3E, 0x41, 0x49, 0x79, 0x3A},
    ['H'] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
    ['I'] = {0x41, 0x41, 0x7F, 0x41, 0x41},
    ['J'] = {0x30, 0x70, 0x79, 0x0F, 0x01},
    ['K'] = {0x7F, 0x08, 0x1C, 0x2A, 0x41},
    ['L'] = {0x7F, 0x00, 0x00, 0x00, 0x40},
    ['M'] = {0x7F, 0x02, 0x06, 0x02, 0x7F},
    ['N'] = {0x7F, 0x02, 0x04, 0x08, 0x7F},
    ['O'] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
    ['P'] = {0x7F, 0x09, 0x09, 0x09, 0x01},
    ['Q'] = {0x3E, 0x41, 0x51, 0x11, 0x52},
    ['R'] = {0x7F, 0x09, 0x09, 0x0B, 0x71},
    ['S'] = {0x06, 0x49, 0x49, 0x09, 0x30},
    ['T'] = {0x01, 0x01, 0x7F, 0x01, 0x01},
    ['U'] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ['V'] = {0x1F, 0x20, 0x60, 0x20, 0x1F},
    ['W'] = {0x7F, 0x20, 0x18, 0x20, 0x7F},
    ['X'] = {0xC3, 0x14, 0x08, 0x14, 0xC3},
    ['Y'] = {0x03, 0x04, 0x78, 0x04, 0x03},
    ['Z'] = {0xE1, 0x51, 0x49, 0x45, 0x83},
    ['_'] = {0x40, 0x40, 0x40, 0x40, 0x40},
};

#define GLYPH_W   5
#define GLYPH_H   7
#define TEXT_SCALE 6          /* 480 屏：单字 30x42px，一行 15 字符 */

/* 纯文本绘制：黑底白字，逐像素 fill_rect（一次熔断只画几十个矩形，可接受） */
static void fatal_draw_char(char c, int x, int y)
{
    unsigned char u = (unsigned char)c;
    if (u >= 128) u = '?';
    const uint8_t *cols = s_font5x7[u];
    for (int col = 0; col < GLYPH_W; col++) {
        uint8_t bits = cols[col];
        for (int row = 0; row < GLYPH_H; row++) {
            if (bits & (1u << row)) {
                display_fill_rect((int16_t)(x + col * TEXT_SCALE),
                                  (int16_t)(y + row * TEXT_SCALE),
                                  TEXT_SCALE, TEXT_SCALE, 0xFFFF);
            }
        }
    }
}

static void fatal_draw_line(const char *text, int y)
{
    int x = 24;  /* 左边距 */
    for (const char *p = text; *p; p++) {
        fatal_draw_char(*p, x, y);
        x += (GLYPH_W + 1) * TEXT_SCALE;   /* 1 列字距 */
        if (x > 480 - 36) break;           /* 单行截断 */
    }
}

/* ------------------------------------------------------------------ */
/* NVS 三振计数                                                         */
/* ------------------------------------------------------------------ */
static uint8_t nvs_load_strikes(void)
{
    uint32_t v = 0;
    if (mp_nvs_get_u32("wdt_hit", &v)) return (uint8_t)(v & 0xFF);
    return 0;
}

static void nvs_save_strikes(uint8_t n)
{
    mp_nvs_set_u32("wdt_hit", n);
}

/* ------------------------------------------------------------------ */
/* 熔断路径（E14）：文本 → 15s → 关屏 → 待机                            */
/* ------------------------------------------------------------------ */
static void enter_fatal(const char *line1, const char *line2)
{
    s_fatal_latched = true;
    /* 停掉 TWDT 干预：系统进入受控待机，不再自愈重启 */
    esp_task_wdt_deinit();

    /* 纯黑整屏（AMOLED 纯黑=不发光） */
    display_fill_rect(0, 0, 480, 480, 0x0000);
    fatal_draw_line(line1, 140);
    if (line2 && line2[0]) fatal_draw_line(line2, 220);

    int64_t until = mp_now_ms() + MP_WDT_FATAL_SHOW_MS;
    while (mp_now_ms() < until) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    mp_display_off();   /* 关屏/待机（display_set_sleep） */

    /* 挂起自身；系统低负载空转（联网后台任务仍可为 Web 提供健康状态）。
     * 恢复手段 = 物理断电重上电（NVS 计数保留，避免再次无限循环）。 */
    vTaskSuspend(NULL);
    for (;;) { vTaskDelay(portMAX_DELAY); }   /* not reached */
}

/* ------------------------------------------------------------------ */
/* 监控任务                                                             */
/* ------------------------------------------------------------------ */
static void watchdog_task(void *arg)
{
    (void)arg;
    const int64_t boot_ms = mp_now_ms();
    bool cleared = (s_strikes == 0);
    int64_t strike_at_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (s_fatal_latched) {
            continue;   /* 已熔断，enter_fatal 挂起了本任务之外的路径 */
        }

        /* 3) 稳定 10 分钟 → 清零（一次即可） */
        if (!cleared && (mp_now_ms() - boot_ms) >= MP_WDT_STABLE_CLEAR_MS) {
            s_strikes = 0;
            nvs_save_strikes(0);
            cleared = true;
        }

        int64_t age = mp_now_ms() - s_last_kick_ms;
        if (age <= MP_WDT_TIMEOUT_MS) {
            continue;
        }

        /* 渲染心跳超时 —— 三振判定（600ms 内重复采样只计一次） */
        if (strike_at_ms != 0 && (mp_now_ms() - strike_at_ms) < 600) {
            continue;
        }
        strike_at_ms = mp_now_ms();

        s_strikes++;
        nvs_save_strikes(s_strikes);

        if (s_strikes >= MP_WDT_STRIKE_MAX) {
            enter_fatal("MINIPET RENDER FAULT", "REBOOT DEVICE TO RESET");
        }

        /* 前两振：重启自愈（宠物永不黑屏）。重启前把心跳时间戳复位，
         * 避免 monitor 在重启竞争中再次计数 */
        s_last_kick_ms = mp_now_ms();
        esp_restart();
    }
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                             */
/* ------------------------------------------------------------------ */
void watchdog_init(void)
{
    s_last_kick_ms = mp_now_ms();     /* 上电即有心跳基线，防误报 */
    s_fatal_latched = false;
    s_strikes = nvs_load_strikes();
    s_was_fatal_last_boot = (s_strikes >= MP_WDT_STRIKE_MAX);
    if (s_was_fatal_last_boot) {
        /* 上一轮已熔断过：计数保留但降为 2，给本次开机一次正常机会，
         * 再卡死则第三次直接熔断（仍是「连续 3 次」语义） */
        s_strikes = MP_WDT_STRIKE_MAX - 1;
        nvs_save_strikes(s_strikes);
    }

    /* TWDT：不 panic，只告警；决策在本模块 */
    esp_task_wdt_config_t cfg = {
        .timeout_ms = MP_WDT_TIMEOUT_MS,
        .idle_core_mask = 0,          /* 不监视 idle（避免后台任务饿死误报） */
        .trigger_panic = false,
    };
    if (esp_task_wdt_init(&cfg) != ESP_OK) {
        esp_task_wdt_reconfigure(&cfg);   /* 已被启动代码初始化过的场景 */
    }

    xTaskCreatePinnedToCore(watchdog_task, "wdt_mon", 4096, NULL,
                            6 /* 高于一切应用任务 */, tskNO_AFFINITY, NULL);
}

void watchdog_subscribe_render_task(void)
{
    /* 在渲染任务自身上下文调用 */
    esp_task_wdt_add(NULL);
    watchdog_kick();
}

void watchdog_kick(void)
{
    if (s_fatal_latched) return;
    s_last_kick_ms = mp_now_ms();
    esp_task_wdt_reset();
}

bool watchdog_was_fatal_last_boot(void)
{
    return s_was_fatal_last_boot;
}

void watchdog_fatal_show(const char *line1, const char *line2)
{
    enter_fatal(line1, line2 ? line2 : "");
}

void watchdog_text_persist(const char *line1, const char *line2)
{
    /* 只画屏：不关屏不挂起（渲染任务此时无素材可画或已被熔断停止，
     * 该通道独立于渲染路径，E14） */
    s_fatal_latched = true;   /* 停止心跳监控：无渲染心跳是本态的既定事实 */
    display_fill_rect(0, 0, 480, 480, 0x0000);
    fatal_draw_line(line1, 140);
    if (line2 && line2[0]) fatal_draw_line(line2, 220);
}
