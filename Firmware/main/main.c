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
#include "esp_event.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "app_core.h"
#include "hal_contract.h"
#include "watchdog.h"
#include "state_machine.h"
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

    mp_cmd_t cmd;
    for (;;) {
        while (xQueueReceive(mp_cmd_q, &cmd, 0) == pdTRUE) {
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
/* app_main                                                              */
/* ------------------------------------------------------------------ */
void app_main(void)
{
    /* NVS（配网凭据/服务器地址/看门狗计数/BGM 偏好都住这里） */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_event_loop_create_default());

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

    /* 任务：PRO(0) 网络/后台 —— 4.1 */
    poller_start();        /* 长轮询+退避（内含 WiFi 回网重连） */
    events_start();        /* POST event */
    asset_dl_start();      /* manifest diff/下载/LRU（优先级低于 poller） */
    ota_start();           /* 双分区升级 */
    bgm_start();           /* BGM 解码+feeder+环形缓冲（codec_init 在内） */

    /* 任务：APP(1) 渲染+交互 —— 4.1 */
    xTaskCreatePinnedToCore(render_task, "render", 32768, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(input_task, "input", 8192, NULL, 4, NULL, 1);

    /* 自检 + 初始迁移（BOOT→SELF_TEST→…；阻塞含 WiFi/服务端探测） */
    bool psram_ok = (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0);
    state_machine_boot(sd_ok, psram_ok);

    /* 开机事件上报（E11：健康状态 Web 可见） */
    mp_post_event_simple(MP_EVT_BOOT, sd_ok ? 1 : 0, 0, MP_FIRMWARE_VERSION);

    ESP_LOGI(TAG, "boot done, state=%s", state_machine_name(state_machine_current()));

    /* main 任务功成身退（空闲任务回收） */
    vTaskDelete(NULL);
}
