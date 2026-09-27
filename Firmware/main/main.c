/**
 * main.c — 固件装配（software-design 4.1 双核三队列拓扑）
 *
 * 启动序：
 *   nvs_flash_init → 事件循环 → 三队列 → watchdog → 驱动 init
 *   （display/touch/imu/rtc/pmu/codec/sd_mount/key）→ 状态机 init
 *   → render_init(profile) → 任务创建：
 *
 *   PRO_CPU(0)：网络与后台
 *     poller    长轮询+指数退避（E2/E11）
 *     events    POST /api/device/event（E2）
 *     asset_dl  manifest diff→下载→TF（优先级低于 poller）
 *     ota       双分区升级（最低优先级）
 *     bgm       HTTP 流→minimp3→PSRAM 环形缓冲（E8）
 *     i2s_feed  环形缓冲→codec_write（DMA）→PA_CTRL
 *   APP_CPU(1)：渲染与交互
 *     render    30fps render_tick + cmd_q 排空 → app_cmd_dispatch
 *               + 看门狗喂狗点（E14 渲染心跳）
 *     input     触摸/IMU/按键/表情状态机（E6）
 *
 *   队列：event_q（input→net 上报）/ cmd_q（net→render 执行）/
 *         audio_q（UI/net→bgm 控制；PCM 走 PSRAM 环形缓冲）
 */
#include <stdio.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "app_core.h"
#include "hal_contract.h"
#include "watchdog.h"
#include "state_machine.h"
#include "provision.h"
#include "input_dispatch.h"
#include "poller.h"
#include "events.h"
#include "asset_dl.h"
#include "ota.h"
#include "bgm.h"

static const char *TAG = "main";

/* 三队列（4.1） */
QueueHandle_t mp_event_q;
QueueHandle_t mp_cmd_q;
QueueHandle_t mp_audio_q;

/* 运行配置（默认值 = software-design 2.4；hello 可下发覆盖） */
mp_app_config_t g_mp_cfg = {
    .idle_to_clock_min = 5,        /* E9：闲置 5 分钟 → CLOCK_DOZE */
    .imu_deadzone_deg  = 8.0f,     /* E6：±8° 死区 */
    .tilt_debounce_ms  = 300,      /* E6：300ms 防抖 */
    .tap_light_g       = 2.0f,     /* E6：<2g */
    .tap_hard_g        = 4.0f,     /* E6：≥4g */
    .imu_sensitivity   = 1.0f,     /* E4：IMU 灵敏度倍率（1.0=出厂；hello config 可覆盖） */
    .brightness        = 80,
};

/* ------------------------------------------------------------------ */
/* APP 核任务                                                            */
/* ------------------------------------------------------------------ */
/* 渲染任务：30fps 帧循环；每帧先排空 cmd_q（net→render 指令落地），
 * 再 render_tick 一帧，最后喂看门狗（E14 渲染心跳）。 */
static void render_task(void *arg)
{
    (void)arg;
    watchdog_subscribe_render_task();      /* 本任务上下文订阅 TWDT（E14） */
    ESP_LOGW("rt", "render_task 起步");

    mp_cmd_t cmd;
    for (;;) {
        while (xQueueReceive(mp_cmd_q, &cmd, 0) == pdTRUE) {
            ESP_LOGW("rt", "cmd 收到 type=%d", (int)cmd.type);
            app_cmd_dispatch(&cmd);
        }
        render_tick();                    /* 4.2 帧循环（30fps） */
        watchdog_kick();
        vTaskDelay(pdMS_TO_TICKS(33));    /* 30fps ≈ 33ms */
    }
}

static void input_task(void *arg)
{
    input_dispatch_task(arg);             /* 不返回 */
}

/* ------------------------------------------------------------------ */
/* 方向轮播标定（一次性标定工具：定稿后置 0 整体消失）                     */
/* ------------------------------------------------------------------ */
#define MP_ORIENT_CALIB 0   /* 方向已定稿：组合2（swap=0,mx=0,my=1）固化进 display_init，标定逻辑保留可复开 */

#if MP_ORIENT_CALIB
/* 屏幕方向标定 v2（点屏切换制）：用户握持向（USB 朝右）需要内容相对 v1 默认
 * 转 90°——那在 swap=false 半区；v1 只轮 swap=true 的 4 个镜像组合是几何错误
 * （用户实测"只会上下颠倒"且横幅被面板旋转 90° 读不了）。现改为：8 种组合
 * （swap 两模式 × mirror 四种）由【点屏幕】逐个切换（POKER/OFFLINE 触摸
 * down 沿，input_dispatch 调 mp_orient_calib_tap），选中态写 NVS，开机自动
 * 应用并打日志——用户点到位后报编号或重启，主线程从日志回读固化。 */
typedef struct { bool swap, mx, my; } orient_combo_t;
static const orient_combo_t s_orient_combos[8] = {
    { false, false, false }, { false, true,  false },
    { false, false, true  }, { false, true,  true  },
    { true,  false, false }, { true,  true,  false },
    { true,  false, true  }, { true,  true,  true  },
};
static uint8_t s_orient_k;

static void orient_apply(uint8_t k)
{
    const orient_combo_t *c = &s_orient_combos[k & 7];
    display_set_orientation(c->swap, c->mx, c->my);
    render_force_redraw();
    ESP_LOGW("orient", "组合 %d/7 swap=%d mx=%d my=%d", k & 7,
             (int)c->swap, (int)c->mx, (int)c->my);
}

#endif  /* MP_ORIENT_CALIB 组合表/应用逻辑 */

/* input_dispatch 触摸 down 沿调用（仅 MP_ORIENT_CALIB=1 期有行为） */
void mp_orient_calib_tap(void)
{
#if MP_ORIENT_CALIB
    static int64_t last_tap;
    int64_t now = esp_timer_get_time();
    if (now - last_tap < 400000) return;    /* 防连点跳两个组合 */
    last_tap = now;
    s_orient_k = (s_orient_k + 1) & 7;
    nvs_handle_t h;
    if (nvs_open("calib", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "k", s_orient_k);
        nvs_commit(h);
        nvs_close(h);
    }
    orient_apply(s_orient_k);
    render_banner_show_for("ABCDEFG", 3000);   /* 镜像判读：字母反写=镜像 */
#endif
}

/* ------------------------------------------------------------------ */
/* app_main                                                              */
/* ------------------------------------------------------------------ */
/* 【真机栈溢出修复 2026-09-27】IDF 的 main 任务默认栈仅 ~3.5KB，而
 * state_machine_boot() 在 main 上同步执行 WiFi 连接（最长 20s）+ TLS 握手 +
 * hello（cJSON 解析响应）+ 素材清单加载——真机实测：
 *   `***ERROR*** A stack overflow in task main has been detected.`
 *   Backtrace: ... |<-CORRUPTED   → rst:0xc (RTC_SW_CPU_RST)
 * 表现为设备每 ~2.7s 一轮重启循环（网络 GOT_IP 那一刻崩）。
 * 修法：app_main 只做"起一个带大栈的 app_main_task"，全部启动逻辑搬进去。
 * 大栈分配失败时回退到原行为（原栈上直接跑），至少不改变既有可用性。 */
#define MP_MAIN_STACK  16384

static void app_main_task(void *arg);

void app_main(void)
{
    if (xTaskCreatePinnedToCore(app_main_task, "mp_main", MP_MAIN_STACK, NULL,
                                tskIDLE_PRIORITY + 1, NULL, 0 /* PRO */) != pdPASS) {
        ESP_LOGW("main", "mp_main 大栈任务创建失败（内部堆挤压）→ 原栈直接启动");
        app_main_task(NULL);      /* 不返回 */
    }
    /* app_main 功成身退，由 mp_main 继续承载原启动流程 */
}

static void app_main_task(void *arg)
{
    (void)arg;
    /* NVS（配网凭据/服务器地址/看门狗计数/BGM 偏好都住这里） */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /* LWIP TCP/IP 线程启动（必需）：离线起播路径不经过配网，poller 仍会建
     * socket（连接失败优雅返回）；不初始化 → tcpip mbox 断言崩溃 */
    ESP_ERROR_CHECK(esp_netif_init());

    /* 三队列 */
    mp_event_q = xQueueCreate(MP_EVENT_Q_LEN, sizeof(mp_event_t));
    mp_cmd_q   = xQueueCreate(MP_CMD_Q_LEN, sizeof(mp_cmd_t));
    mp_audio_q = xQueueCreate(MP_AUDIO_Q_LEN, sizeof(mp_audio_msg_t));
    assert(mp_event_q && mp_cmd_q && mp_audio_q);

    /* 看门狗（E14）：先于一切应用任务——渲染任务起来前就有心跳基线 */
    watchdog_init();

    /* 驱动 init（drivers.h 规定顺序：i2c_bus → 各器件 → sd）。
     * display_init 由 render_init 内部完成（render.h 契约）。 */
    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(touch_cst9220_init());
    ESP_ERROR_CHECK(imu_qmi8658_init());
    ESP_ERROR_CHECK(rtc_pcf85063_init());
    ESP_ERROR_CHECK(pmu_axp2101_init());
    bool sd_ok = (sd_mount() == 0);            /* sd_tf.h：返回 errno，挂载 /sdcard */
    if (!sd_ok) {
        ESP_LOGE(TAG, "TF mount failed（E11 降级矩阵地基缺失）");
    }

    /* 应用层 */
    input_dispatch_init();
    state_machine_init();
    render_init(&MINIPET_PROFILE_AMOLED216);   /* FATFS 挂载后、首 tick 前（render.h） */

    /* 中键历史计数回显（永远生效：GPIO0 通路取证，与标定开关无关） */
    {
        nvs_handle_t h;
        if (nvs_open("calib", NVS_READONLY, &h) == ESP_OK) {
            uint32_t n = 0;
            if (nvs_get_u32(h, "key0", &n) == ESP_OK && n > 0) {
                uint8_t stb = 255;
                nvs_get_u8(h, "k0st", &stb);
                /* 状态枚举：0BOOT 1SELF_TEST 2WIFI_PROVISION 3POKER 4MENU
                 * 5CLOCK_DOZE 6OFFLINE 7OTA 8FATAL */
                ESP_LOGW("key0", "中键累计 %lu 次，最后一次按下时状态=%u（4=MENU）",
                         (unsigned long)n, stb);
            }
            nvs_close(h);
        }
    }
#if MP_ORIENT_CALIB
    {
        /* 应用上次标定选中的方向组合（NVS 记忆） */
        nvs_handle_t h;
        if (nvs_open("calib", NVS_READONLY, &h) == ESP_OK) {
            uint8_t k = 0;
            if (nvs_get_u8(h, "k", &k) == ESP_OK) {
                s_orient_k = k & 7;
                orient_apply(s_orient_k);
            }
            nvs_close(h);
        }
    }
#endif

    /* 任务：APP(1) 渲染+交互 —— 必须先于 bgm(24K 栈)/ota 等创建：
     * 本板内部动态堆仅 ~143KB 且碎片化，真机实证 bgm 24K 先分配后
     * render 12K 连续块即失败（internal=9115 最大块=3060，整屏黑）。
     * render 再配有界重试兜底（碎片随时序漂移，boot 间随机）。 */
    bool render_ok = false;
    for (int t = 0; t < 10 && !render_ok; t++) {
        if (xTaskCreatePinnedToCore(render_task, "render", 12288, NULL, 5, NULL, 1) == pdPASS) {
            render_ok = true;
            break;
        }
        ESP_LOGE(TAG, "render 任务创建失败(第%d次) internal=%u 最大块=%u", t + 1,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    BaseType_t rc = xTaskCreatePinnedToCore(input_task, "input", 4096, NULL, 4, NULL, 1);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "input 任务创建失败 rc=%d internal=%u", rc,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    /* 任务：PRO(0) 网络/后台 —— 4.1 */
    poller_start();        /* 长轮询+退避（内含 WiFi 回网重连） */
    events_start();        /* POST event */
    asset_dl_start();      /* manifest diff/下载/LRU（优先级低于 poller） */
    ota_start();           /* 双分区升级 */
    bgm_start();           /* BGM 解码+feeder+环形缓冲（codec_init 在内） */

    /* 自检 + 初始迁移（BOOT→SELF_TEST→…；阻塞含 WiFi/服务端探测） */
    bool psram_ok = (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0);
    state_machine_boot(sd_ok, psram_ok);

    /* E9 常态化校时：自检后启动（内部等 STA 连上才动作；时间已有效则转 6h 周期）。
     * 真机缺口见 provision.c 的 rtc_resync_task 注释（待机时钟恒 --:--）。 */
    provision_rtc_resync_start();

    /* 开机事件上报（E11：健康状态 Web 可见） */
    mp_post_event_simple(MP_EVT_BOOT, sd_ok ? 1 : 0, 0, MP_FIRMWARE_VERSION);

    ESP_LOGI(TAG, "boot done, state=%s", state_machine_name(state_machine_current()));

    /* main 任务功成身退（空闲任务回收） */
    vTaskDelete(NULL);
}
