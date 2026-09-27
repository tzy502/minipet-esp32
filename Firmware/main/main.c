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
#include "driver/uart.h"
#include "driver/uart_vfs.h"

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
#include "logbuf.h"      /* E14 设备环形日志（PSRAM） */

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
        /* 【看门狗熔断修复 2026-09-27】先喂狗再排空指令队列。
         * 真机实证：启动期一次性涌入几十条指令，而每条都打一条 WARN 日志
         * （115200 波特率下串口是阻塞写）→ 渲染任务连续 >5s 没喂狗 →
         * `wdt: E14 熔断：已关屏待机（恢复 = 物理断电重上电）` → 后续必然掉线。
         * ① 每条指令的 WARN 降为 DEBUG（启动期不再刷屏）；
         * ② 喂狗提到排空之前，并在每条指令后补喂一次。 */
        watchdog_kick();
        while (xQueueReceive(mp_cmd_q, &cmd, 0) == pdTRUE) {
            ESP_LOGD("rt", "cmd 收到 type=%d", (int)cmd.type);
            app_cmd_dispatch(&cmd);
            watchdog_kick();              /* 单条指令若耗时（素材懒加载），也持续喂狗 */
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
#define MP_MAIN_STACK  10240   /* 【内部堆腾挪 2026-09-27】原 12288：本板内部堆
                                * 启动末期只剩几百字节最大块，SoftAP 的 DHCP/
                                * 管理帧与配网页都被饿死（真机：Mac 关联成功但
                                * 拿不到 IP）。该任务只做 init，10K 实测足够 */

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
    /* 【串口非阻塞 2026-09-27】默认 UART 写是阻塞的：无人读串口时 TX 缓冲满
     * → 打日志的任务被挂住（真机表现：渲染任务 >5s 不喂狗 → E14 熔断关屏）。
     * 设为非阻塞 + 允许覆盖，宁可丢日志也不能拖死任务。 */
    {
        static char txbuf[4096];
        /* IDF5：uart_vfs 提供带缓冲的控制台输出；不接主机读串口时缓冲写满即丢，
         * 不再把调用任务挂死（见上方注释）。失败仅告警，不阻断启动。 */
        uart_vfs_dev_use_driver(-1);
        esp_err_t ur = uart_driver_install(UART_NUM_0, 256, sizeof(txbuf), 0, NULL, 0);
        if (ur != ESP_OK && ur != ESP_ERR_INVALID_STATE) {
            ESP_LOGW("main", "UART 驱动安装失败（%s）→ 日志仍走默认阻塞模式",
                     esp_err_to_name(ur));
        } else {
            uart_vfs_dev_use_driver(UART_NUM_0);
        }
    }

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

    /* E14 设备环形日志：尽早上电（越早，Web 能拉到的启动期日志越全）。
     * 缓冲在 PSRAM（32KB，内部动态堆 0 占用）；挂 esp_log vprintf，
     * 串口输出不受影响。失败仅降级（不阻塞启动）。 */
    logbuf_init();

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

    /* 【真机 NO_MEM 根因修复 2026-09-27】WiFi/esp_netif 必须在【渲染任务与
     * LVGL 大缓冲之前】初始化：本板内部堆仅 ~143KB，渲染任务(12K)+菜单整屏
     * 缓冲(PSRAM)+LVGL 初始化会把内部堆切碎，之后 state_machine_boot() 里的
     * wifi_init_once() 调 esp_netif_create_default_wifi_sta() 分配失败：
     *   ESP_ERROR_CHECK failed: esp_err_t 0x101 (ESP_ERR_NO_MEM)
     *   file: "./main/app/provision.c" line 574 / func: wifi_init_once → abort()
     * → 无限重启、WiFi 从未初始化 → 设备永不 poll（"服务器重启后不重连"总根因）。
     * 这里在任务创建前先把 WiFi 栈建好（幂等；后续 provision_* 调用直接复用）。 */
    provision_wifi_preinit();

    /* 【Reset WiFi 后无线重启修复 2026-09-27】无配网凭据 → 这里就把 SoftAP 起了。
     * 崩因与修复口径见 provision.h 的 provision_ap_early_start_if_needed 注释：
     * portal_task 起 AP 太晚（渲染/BGM 之后），beacon 缓冲分配失败 → WiFi 驱动
     * 空指针 → rst:0xc 无限重启。必须在 render_init 与各任务创建之前。 */
    provision_dump_internal_heap("wifi_preinit 后");
    provision_ap_early_start_if_needed();

    /* 【配网页可用性修复 2026-09-27】httpd(6~8KB 栈) 必须同样在这个干净窗口
     * 建好：等渲染任务/LVGL/codec 起来后内部堆只剩 5KB/最大块 3.4KB，portal
     * 任务建不起来 → 用户 Reset WiFi 后连上热点却打不开 192.168.4.1（真机实测）。 */
    provision_portal_early_start_if_needed();

    state_machine_init();
    render_init(&MINIPET_PROFILE_AMOLED216);   /* FATFS 挂载后、首 tick 前（render.h） */
    provision_dump_internal_heap("render_init 后");

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
    /* 【内部堆腾挪 2026-09-27】12K → 10K：真机启动末期内部堆只剩 2.3KB 空闲、
     * 最大连续块 2036B，lwIP 连 TCP PCB/发送缓冲都分不到（连自己的网关都
     * connect 失败 errno=113），而 Mac 侧同一 URL curl 200。渲染任务只做
     * compose+blit，菜单构建期的深栈需求已由 lvgl_bridge 内部收敛；
     * 保留 10K 余量并保留下方有界重试。 */
    /* 【认证帧分配失败根因修复 2026-09-27】把「网络任务 + 自检联网」提前到渲染任务
     * 创建**之前**。真机证据链：
     *   · 成功连上的那几次：`state: init -> auth` 发生在 1.6s（渲染任务尚未推像素），
     *     全程无 `m f auth`；
     *   · 现在渲染任务先跑，认证被推到 4.0s，此后**每次**认证都 `W wifi:m f auth`
     *     （802.11 层分配认证帧缓冲失败，串见 libnet80211.a 的 "m f auth"），1s 后
     *     `auth -> init (0x200)`，对外表现为 reason=2/205 无限循环；
     *   · 与总空闲内存无关（内部空闲 7.9KB、最大块 7.6KB 时依旧失败）→ 是驱动管理帧
     *     池与 LVGL 渲染互相抢内存。
     * 联网只需几百 ms，期间屏幕短暂黑屏可接受（心跳/OTA 优先）。 */
    {
        bool psram_ok_early = (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0);
        poller_start();
        events_start();
        asset_dl_start();
        provision_dump_internal_heap("联网前（渲染任务未创建）");
        state_machine_boot(sd_ok, psram_ok_early);
        provision_dump_internal_heap("联网后");
    }

    bool render_ok = false;
    static const uint32_t render_stacks[] = { 8192, 6144 };
    for (int t = 0; t < 10 && !render_ok; t++) {
        uint32_t stk = render_stacks[t < 6 ? 0 : 1];   /* 前 6 次 8K，之后降 6K */
        /* 【内部 DRAM 腾挪 2026-09-27】渲染任务栈改从 PSRAM 分配：
         * 真机实测内部堆运行期只剩 空闲 3356B / 最大连续块 2036B，lwIP 连
         * socket 都开不出来（`out of memory` → ESP_ERR_HTTP_CONNECT errno=105），
         * 设备"永远不上线"。渲染任务是纯内存拷贝/合成，栈放 PSRAM 只需一次
         * xTaskCreateStatic 前置分配，换来 8KB 内部 DRAM 给网络栈。
         * 失败（PSRAM 不足）则回落原有内部栈路径。 */
        TaskHandle_t rh = NULL;
        StackType_t *rstack = heap_caps_malloc(stk * sizeof(StackType_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        StaticTask_t *rtcb = rstack ? heap_caps_malloc(sizeof(StaticTask_t),
                                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : NULL;
        if (rstack && rtcb) {
            rh = xTaskCreateStaticPinnedToCore(render_task, "render", stk, NULL, 5, rstack, rtcb, 1);
        }
        if (rh) {
            render_ok = true;
            ESP_LOGW(TAG, "render 任务已创建（栈 %u @PSRAM）", (unsigned)stk);
            break;
        }
        if (xTaskCreatePinnedToCore(render_task, "render", stk, NULL, 5, NULL, 1) == pdPASS) {
            render_ok = true;
            ESP_LOGW(TAG, "render 任务已创建（栈 %u）", (unsigned)stk);
            break;
        }
        ESP_LOGE(TAG, "render 任务创建失败(第%d次, 栈%u) internal=%u 最大块=%u", t + 1,
                 (unsigned)stk,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    BaseType_t rc = xTaskCreatePinnedToCore(input_task, "input", 4096, NULL, 4, NULL, 1);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "input 任务创建失败 rc=%d internal=%u", rc,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    /* 后台任务：OTA / BGM（网络任务已在渲染任务之前启动，见上） */
    ota_start();           /* 双分区升级 */
    bgm_start();           /* BGM 解码+feeder+环形缓冲（codec_init 在内） */
    provision_dump_internal_heap("全部任务创建后");

    /* E9 常态化校时：自检后启动（内部等 STA 连上才动作；时间已有效则转 6h 周期）。
     * 真机缺口见 provision.c 的 rtc_resync_task 注释（待机时钟恒 --:--）。 */
    provision_rtc_resync_start();

    /* 开机事件上报（E11：健康状态 Web 可见） */
    mp_post_event_simple(MP_EVT_BOOT, sd_ok ? 1 : 0, 0, MP_FIRMWARE_VERSION);

    ESP_LOGI(TAG, "boot done, state=%s", state_machine_name(state_machine_current()));

    /* main 任务功成身退（空闲任务回收） */
    vTaskDelete(NULL);
}
