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
 *   4) 「停止渲染」是熔断的必要条件，不是关屏就够了：
 *      - 熔断发生在监控任务里，而卡死任务本身只是「慢」不是「死」，
 *        它恢复后会继续 render_tick() 把错误文本盖掉 → enter_fatal()
 *        在画完文本后 vTaskSuspend() 掉渲染任务（句柄由订阅点自取）。
 *      - 跨开机：上一轮熔断（NVS=3）且本次复位不是用户动作（软件重启/
 *        panic/TWDT/brownout）→ 熔断锁定 watchdog_render_allowed()==false，
 *        渲染任务在入口（watchdog_subscribe_render_task）就被拦下：纯文本 +
 *        关屏待机，不订阅 TWDT、一帧都不渲染。物理断电/EN 复位/USB 复位 =
 *        用户显式恢复动作（本模块承诺的恢复手段）→ 给一次机会（计数降 2）。
 *
 * TWDT 本身配置为不 panic（CONFIG_ESP_TASK_WDT_PANIC=n → trigger_panic=false，
 * 见 sdkconfig.defaults「E14 定稿」段），由本模块决策重启/熔断，
 * 避免 IDF panic 处理器在渲染栈损坏时反而不受控。
 */
#include "watchdog.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"

#include "app_core.h"
#include "hal_contract.h"
#include "font5x7.h"     /* 内嵌 5x7 ASCII 字体（与合成器横幅共用，问题4） */

static const char *TAG = "wdt";

#define MP_WDT_TIMEOUT_MS       5000    /* 30fps 下 150 帧无心跳 = 卡死 */
#define MP_WDT_STRIKE_MAX       3       /* 连续 3 次超时 → 熔断（E14） */
#define MP_WDT_STABLE_CLEAR_MS  (10u * 60u * 1000u)  /* 稳定 10 分钟清零 */
#define MP_WDT_FATAL_SHOW_MS    15000   /* 熔断后文本停留时间，然后关屏 */
#define MP_WDT_PRESTART_GRACE_MS 20000  /* 渲染任务订阅前的宽限（驱动 init /
                                         * TF 挂载慢启动 >5s 时不计振，避免
                                         * 慢启动被误判成卡死 → 重启循环）；
                                         * 宽限期后仍无订阅 = 渲染任务真没
                                         * 起来，照常计振上报 */

static volatile int64_t s_last_kick_ms;
static volatile bool    s_fatal_latched;      /* 熔断后停检 */
static volatile uint8_t s_strikes;
static bool             s_was_fatal_last_boot;
static bool             s_fatal_lockdown;     /* 本次开机处于熔断锁定态（拒绝渲染） */
static bool             s_render_absent;      /* 渲染任务彻底缺席（停计振） */
static bool             s_render_subscribed;  /* 渲染任务已订阅（宽限期判定） */
static TaskHandle_t     s_render_task;        /* 渲染任务句柄（订阅点自取，熔断时挂起它） */

/* ------------------------------------------------------------------ */
/* 内嵌 5x7 ASCII 字体表见 font5x7.h（熔断文本与合成器横幅共用）。        */
/* ------------------------------------------------------------------ */

#define TEXT_SCALE 6          /* 480 屏：单字 30x42px */
#define TEXT_COLS  12         /* 本字号单行字符数：x=24 起、步进 36px、右界 444 */
#define TEXT_LINE_H 50        /* 行距（字高 42 + 8 间隙） */

/* 纯文本绘制：黑底白字，逐像素 fill_rect（一次熔断只画几十个矩形，可接受） */
static void fatal_draw_char(char c, int x, int y)
{
    unsigned char u = (unsigned char)c;
    if (u >= 128) u = '?';
    const uint8_t *cols = MP_FONT5X7[u];
    for (int col = 0; col < MP_FONT_GLYPH_W; col++) {
        uint8_t bits = cols[col];
        for (int row = 0; row < MP_FONT_GLYPH_H; row++) {
            if (bits & (1u << row)) {
                display_fill_rect((int16_t)(x + col * TEXT_SCALE),
                                  (int16_t)(y + row * TEXT_SCALE),
                                  TEXT_SCALE, TEXT_SCALE, 0xFFFF);
            }
        }
    }
}

/* 逐字符绘制，超过 TEXT_COLS 自动折到下一行；返回占用行数。
 * （原实现是 `if (x > 480 - 36) break;` 硬截断 —— 20 字符的
 *  "MINIPET RENDER FAULT" 只能显示成 "MINIPET REND"，错误提示读不全） */
static int fatal_draw_line(const char *text, int y)
{
    int x = 24;  /* 左边距 */
    int rows = 1;
    for (const char *p = text; *p; p++) {
        if (x > 24 + (TEXT_COLS - 1) * (MP_FONT_GLYPH_W + 1) * TEXT_SCALE) {
            x = 24;
            y += TEXT_LINE_H;
            rows++;
        }
        fatal_draw_char(*p, x, y);
        x += (MP_FONT_GLYPH_W + 1) * TEXT_SCALE;   /* 1 列字距 */
    }
    return rows;
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
/* 熔断路径（E14）：停止渲染 → 文本 → 15s → 关屏 → 待机                  */
/* ------------------------------------------------------------------ */
static void enter_fatal(const char *line1, const char *line2)
{
    s_fatal_latched = true;
    /* 停掉 TWDT 干预：系统进入受控待机，不再自愈重启 */
    esp_task_wdt_deinit();

    /* 纯黑整屏（AMOLED 纯黑=不发光） */
    display_fill_rect(0, 0, MINIPET_ACTIVE_PROFILE.width, MINIPET_ACTIVE_PROFILE.height, 0x0000);
    int rows = fatal_draw_line(line1, 140);
    if (line2 && line2[0]) fatal_draw_line(line2, 140 + rows * TEXT_LINE_H);

    /* E14「停止渲染」：真正把渲染任务停掉。只关屏是不够的——触发熔断的
     * 卡死往往只是某个慢操作（TF/SPI/I2C 抖动），渲染任务恢复后会继续
     * render_tick()，把刚画的错误文本覆盖掉，并让「待机」名不副实。
     * 句柄由 watchdog_subscribe_render_task() 在渲染任务自身上下文里取；
     * 若本函数就在渲染任务里被调用（开机锁定路径），跳过——末尾
     * vTaskSuspend(NULL) 已挂起本任务。
     * 注意：vTaskSuspend 挂在持锁点（面板/SPI 互斥）上时，后续 display_*
     * 可能阻塞；此路径的语义就是「终态待机、只等断电」，故可接受。 */
    if (s_render_task && s_render_task != xTaskGetCurrentTaskHandle()) {
        vTaskSuspend(s_render_task);
        ESP_LOGE(TAG, "E14 熔断：渲染任务(%p)已挂起 —— 渲染停止（render gate CLOSED）",
                 (void *)s_render_task);
    }

    int64_t until = mp_now_ms() + MP_WDT_FATAL_SHOW_MS;
    while (mp_now_ms() < until) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    mp_display_off();   /* 关屏/待机（display_set_sleep） */
    ESP_LOGE(TAG, "E14 熔断：已关屏待机（恢复 = 物理断电重上电）");

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

        /* 启动宽限：渲染任务尚未订阅（驱动 init / TF 挂载可能超过 5s 超时），
         * 此时「心跳年龄」没有意义，计振会把慢启动误判成卡死 → 重启 → 再慢
         * 启动 = 无限重启。宽限期后仍无订阅 = 渲染任务真没起来，照常计振。 */
        if (!s_render_subscribed &&
            (mp_now_ms() - boot_ms) < MP_WDT_PRESTART_GRACE_MS) {
            continue;
        }

        /* 3) 稳定 10 分钟 → 清零（一次即可） */
        if (!cleared && (mp_now_ms() - boot_ms) >= MP_WDT_STABLE_CLEAR_MS) {
            s_strikes = 0;
            nvs_save_strikes(0);
            cleared = true;
        }

        if (s_render_absent) {
            continue;   /* 渲染任务不存在：心跳年龄无意义（见 watchdog_render_absent） */
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
            enter_fatal("RENDER FAULT", "POWER CYCLE TO RESET");
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
    s_fatal_lockdown = false;
    s_render_subscribed = false;
    s_render_task = NULL;
    s_strikes = nvs_load_strikes();
    s_was_fatal_last_boot = (s_strikes >= MP_WDT_STRIKE_MAX);
    if (s_was_fatal_last_boot) {
        /* 上一轮已熔断（NVS=3 未被稳定窗口清零）。区分「谁把设备叫醒的」：
         *  a) 物理断电重上电 / EN 复位 / USB 复位 = 用户显式恢复动作（本模块
         *     头注释承诺的恢复手段）→ 计数降为 2，给本次开机一次正常机会，
         *     再卡死即第 3 振熔断（仍是「连续 3 次」语义）；
         *  b) 软件重启 / panic / TWDT / brownout = 非用户动作（自动重启循环
         *     嫌疑）→ 熔断锁定：本次开机拒绝渲染（watchdog_render_allowed()
         *     ==false，渲染任务在入口被拦），纯文本 + 关屏待机，只等人工断电。
         *     这正是 E14「防无限重启」要的：自动重启永远换不来一次渲染。 */
        esp_reset_reason_t why = esp_reset_reason();
        bool user_reset = (why == ESP_RST_POWERON || why == ESP_RST_EXT
                           || why == ESP_RST_USB);
        if (user_reset) {
            s_strikes = MP_WDT_STRIKE_MAX - 1;
            nvs_save_strikes(s_strikes);
            ESP_LOGW(TAG, "上次熔断后复位 reason=%d（用户复位）→ 给 1 次机会（strike=%u）",
                     (int)why, (unsigned)s_strikes);
        } else {
            s_fatal_lockdown = true;
            s_fatal_latched = true;   /* 监控任务不再自动重启：受控待机 */
            ESP_LOGE(TAG, "上次熔断后复位 reason=%d（非用户复位）→ 熔断锁定：本次开机不渲染",
                     (int)why);
        }
    }

    /* TWDT：默认不 panic（CONFIG_ESP_TASK_WDT_PANIC=n，sdkconfig.defaults
     * 「E14 定稿」段）：超时只报警，重启/熔断决策在本模块。仅诊断期可临时置 y
     * 换取 panic 精确回溯，此处跟随该 Kconfig，用完必须回 n。 */
    esp_task_wdt_config_t cfg = {
        .timeout_ms = MP_WDT_TIMEOUT_MS,
        .idle_core_mask = 0,          /* 不监视 idle（避免后台任务饿死误报） */
#if CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#else
        .trigger_panic = false,
#endif
    };
    if (esp_task_wdt_init(&cfg) != ESP_OK) {
        esp_task_wdt_reconfigure(&cfg);   /* 已被启动代码初始化过的场景 */
    }

    xTaskCreatePinnedToCore(watchdog_task, "wdt_mon", 4096, NULL,
                            6 /* 高于一切应用任务 */, NULL, tskNO_AFFINITY);
}

void watchdog_subscribe_render_task(void)
{
    /* 在渲染任务自身上下文调用（main.c render_task 第一句）。
     * 本函数同时是熔断态的「渲染闸门」：锁定态下不订阅 TWDT、不进渲染循环，
     * 直接走纯文本 + 关屏待机（E14）。main.c 也可在创建任务前查询
     * watchdog_render_allowed() 跳过创建（等价钩子）。 */
    if (!watchdog_render_allowed()) {
        ESP_LOGE(TAG, "E14 熔断锁定：拒绝渲染任务启动（不订阅 TWDT，渲染 0 帧）");
        watchdog_fatal_show("RENDER OFF", "POWER CYCLE TO RESET");
        for (;;) { vTaskDelay(portMAX_DELAY); }   /* watchdog_fatal_show 已挂起本任务；双保险 */
    }

    s_render_task = xTaskGetCurrentTaskHandle();
    s_render_subscribed = true;
    esp_task_wdt_add(NULL);
    watchdog_kick();
    ESP_LOGI(TAG, "渲染任务已订阅 TWDT（handle=%p，超时 %dms）",
             (void *)s_render_task, MP_WDT_TIMEOUT_MS);
    if (watchdog_was_fatal_last_boot()) {
        /* 上一轮熔断过、本次是用户复位后的「一次机会」：只剩 1 振 */
        ESP_LOGW(TAG, "上次开机曾熔断（strike 计数保留）：本次再卡死 1 次即熔断锁定");
    }
}

bool watchdog_render_allowed(void)
{
    return !s_fatal_lockdown;
}

void watchdog_kick(void)
{
    /* FATAL 锁存：软件心跳监控停（本态无渲染心跳是既定事实），但硬件 TWDT
     * 必须继续喂——否则 CONFIG_ESP_TASK_WDT_PANIC=y 下进 FATAL 5s 必 panic
     * → 重启循环（真机实证 2026-09-29：黑屏循环的总根因）。 */
    if (s_fatal_latched) {
        esp_task_wdt_reset();
        return;
    }
    s_last_kick_ms = mp_now_ms();
    esp_task_wdt_reset();
}

bool watchdog_was_fatal_last_boot(void)
{
    return s_was_fatal_last_boot;
}

void watchdog_render_absent(void)
{
    /* 渲染任务不存在 → 渲染心跳永远不再更新，计振毫无意义（必然熔断）。
     * 这里把它标记为"已知缺席"，计振分支直接跳过；其余保护（TWDT 订阅、
     * FATAL 文本、低电等）不受影响。 */
    s_render_absent = true;
    ESP_LOGW("wdt", "渲染任务缺席：已关闭渲染心跳计振（不再熔断关屏）");
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
    display_fill_rect(0, 0, MINIPET_ACTIVE_PROFILE.width, MINIPET_ACTIVE_PROFILE.height, 0x0000);
    int rows = fatal_draw_line(line1, 140);
    if (line2 && line2[0]) fatal_draw_line(line2, 140 + rows * TEXT_LINE_H);
}
