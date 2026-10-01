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
#define MP_TASK_PROBE 1   /* 任务存活取证（排障时置 1：poller 阶段/bgm 步骤/消息计数） */

#include <stdio.h>
#include <assert.h>
#include <string.h>      /* 指令合并用的 strcmp（见 render_drain_cmds） */

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
#include "cJSON.h"       /* cJSON_InitHooks：清单/指令树的分配改走 PSRAM */
#include "esp_rom_uart.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"
#include "esp_log.h"

#include "app_core.h"
#include "hal_contract.h"
#include "mp_psram.h"    /* PSRAM 优先分配（内部 DRAM 只有 ~133KB） */
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
/* ══ 【指令排空 + 合并 2026-10-01】═══════════════════════════════════════════
 * 用户原话："现在因为卡顿体验比较差 会有一种卡顿了以后在突然好多按钮一口气
 * 点一遍"。
 *
 * 机理：渲染任务被长任务堵住时（整图装载 >5s、整屏重合成 100ms 级），cmd_q
 * 照常收指令（16 深）。堵完恢复后旧代码是 `while (xQueueReceive(...)) dispatch()`
 * —— 把攒下的十几条**一次性全执行**：屏上就是一串动作连播（气泡/表情/横幅
 * 一帧一条）。
 *
 * 现在的语义（与"忙时丢弃"配套，见 input_dispatch 的 bridge_cam_busy 入口）：
 *   ① 排空整批后再落地，落地前做一次合并；
 *   ② 合并只对**纯显示、后到覆盖先到**的类型生效（白名单见 cmd_latest_wins）：
 *      同类只保留最后一次，屏上表现为"直接到最终状态"，不会有中间态连播；
 *   ③ 其余类型（BGM_TOGGLE 乒乓、MENU_ENTER/EXIT 状态迁移、REBOOT、OTA_*、
 *      PAIRING_CODE…）**原样按队列顺序执行**——合并它们会改变语义；
 *   ④ SET_MAP 只做**完全重复**合并（同 hash 的连投 = 一次 5s 装载的重放）。
 *   ⑤ 与旧写法的唯一行为差异：指令处理函数内部若再投一条新指令（如
 *      dispatch_manifest_synced 末尾重投默认地图），旧写法会在**同一次排空**里
 *      继续收走并执行，现在会留到**下一帧**排空（+33ms）。对渲染/横幅类结果无
 *      影响，且顺带把"处理函数互相投递"造成的同帧长链截断了（对看门狗更友好）。
 * 批缓冲是静态的（不进渲染任务栈：该任务栈最紧时只有 3584 字节，见下面
 * render_stacks 的降档表——1.7KB 的栈数组就是一次栈溢出）。 */
static bool cmd_latest_wins(mp_cmd_type_t t)
{
    switch (t) {
    case MP_CMD_BUBBLE:          /* 气泡文本：后一条覆盖前一条 */
    case MP_CMD_BANNER:          /* 顶部横幅：同上 */
    case MP_CMD_SET_ACTION:      /* 动作：只需最终动作 */
    case MP_CMD_SET_EXPRESSION:  /* 表情：同上 */
    case MP_CMD_BRIGHTNESS:      /* 亮度：同上 */
    case MP_CMD_NET_STATE:       /* 在线态：同上 */
    case MP_CMD_BGM_STATE:       /* BGM 回显态：同上 */
    case MP_CMD_OTA_BEGIN:       /* 升级进度提示：同上 */
        return true;
    default:
        return false;
    }
}

static int render_drain_cmds(void)
{
    /* 静态（免占渲染任务 3.5KB 的最紧栈），但**不必占内部 DRAM**：
     * 【内部 RAM 腾挪 2026-10-02】s_drain（16×108B=1728B）+ s_drop 原来常驻
     * 内部 .bss。它只是"从 cmd_q 出队的拷贝"，只在渲染任务内被读、随后喂给
     * app_cmd_dispatch；无任何 DMA 直接读它 → PSRAM 懒分配（首次排空时分配，
     * 之后复用；分配失败退内部堆，再失败则本拍不排空、下拍重试，不丢指令）。 */
    static mp_cmd_t *s_drain;
    static bool     *s_drop;
    if (!s_drain) s_drain = mp_psram_malloc(sizeof(mp_cmd_t) * MP_CMD_Q_LEN);
    if (!s_drop)  s_drop  = mp_psram_malloc(sizeof(bool) * MP_CMD_Q_LEN);
    if (!s_drain || !s_drop) return 0;
    int n = 0;
    while (n < MP_CMD_Q_LEN && xQueueReceive(mp_cmd_q, &s_drain[n], 0) == pdTRUE) n++;
    for (int i = 0; i < n; i++) s_drop[i] = false;

    int merged = 0;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            bool dup = (s_drain[j].type == s_drain[i].type);
            if (dup && !cmd_latest_wins(s_drain[i].type)) {
                /* 非白名单：只合并**完全一样**的条目（同类型同参数同文本）——
                 * 例：同 hash 的 MP_CMD_SET_MAP 连投。 */
                dup = (s_drain[j].a == s_drain[i].a && s_drain[j].b == s_drain[i].b &&
                       strcmp(s_drain[j].s, s_drain[i].s) == 0);
            }
            if (dup) { s_drop[i] = true; merged++; break; }
        }
    }
    if (merged > 0) {
        ESP_LOGW(TAG, "cmd_q 合并：本拍排空 %d 条，丢弃被覆盖的旧条目 %d 条（防卡顿后一口气执行）",
                 n, merged);
    }
    for (int i = 0; i < n; i++) {
        if (s_drop[i]) continue;
        ESP_LOGW(TAG, "[取证] cmd 派发 type=%d s='%.16s'", (int)s_drain[i].type, s_drain[i].s);
        app_cmd_dispatch(&s_drain[i]);
        watchdog_kick();              /* 单条指令若耗时（素材懒加载），也持续喂狗 */
    }
    return n - merged;
}

/* 渲染任务：30fps 帧循环；每帧先排空 cmd_q（net→render 指令落地），
 * 再 render_tick 一帧，最后喂看门狗（E14 渲染心跳）。 */
static void render_task(void *arg)
{
    (void)arg;
    watchdog_subscribe_render_task();      /* 本任务上下文订阅 TWDT（E14） */
    ESP_LOGW("rt", "render_task 起步");

    for (;;) {
        /* 【看门狗熔断修复 2026-09-27】先喂狗再排空指令队列。
         * 真机实证：启动期一次性涌入几十条指令，而每条都打一条 WARN 日志
         * （115200 波特率下串口是阻塞写）→ 渲染任务连续 >5s 没喂狗 →
         * `wdt: E14 熔断：已关屏待机（恢复 = 物理断电重上电）` → 后续必然掉线。
         * ① 每条指令的 WARN 降为 DEBUG（启动期不再刷屏）；
         * ② 喂狗提到排空之前，并在每条指令后补喂一次。 */
        watchdog_kick();
        render_drain_cmds();              /* 排空 + 合并（见上"指令排空 + 合并"） */
        render_tick();                    /* 4.2 帧循环（30fps） */
        watchdog_kick();

#if MP_TASK_PROBE
        /* 【任务存活取证 2026-09-27】用户报障「BGM 一起播设备就从服务器掉线」
         * （真机：发 bgm=play 后 /api/admin/devices 的 lastSeen 不再更新，串口也
         * 没有 BGM 日志）。这里每 10s 打一次各任务的自增计数：
         * 计数在涨 = 任务在跑（问题在协议层）；计数冻结 = 该任务被阻塞。
         * 定稿后 MP_TASK_PROBE 置 0。 */
        {
            static int64_t s_tp_ms;
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - s_tp_ms > 10000) {
                s_tp_ms = now_ms;
                extern volatile uint32_t g_poll_loops, g_poll_ok, g_poll_fail;
                extern volatile int32_t  g_poll_last_status;
                extern volatile uint32_t g_bgm_msgs, g_feeder_loops;
                extern volatile uint32_t g_bgm_step;
                extern volatile uint32_t g_bgm_drop_nodec, g_bgm_drop_offline,
                                         g_bgm_drop_greyed, g_bgm_wr_err;
                extern volatile uint32_t g_bgm_underruns, g_bgm_ring_min, g_bgm_wr_max_us;
                extern volatile uint32_t g_bgm_gap_max_us, g_bgm_gap_over, g_bgm_gap_at_ms;
                extern volatile uint32_t g_bgm_drop_bytes, g_bgm_drops;
                extern volatile uint32_t g_pol_stage[10];
                extern volatile int32_t  g_bgm_state_probe;
                ESP_LOGW("tprobe", "poller 阶段=[%u %u %u %u %u %u %u %u %u] 门失败=%u 成功=%u 失败=%u | "
                                   "bgm 消息=%u 状态=%d feeder=%u",
                         (unsigned)g_pol_stage[0], (unsigned)g_pol_stage[1],
                         (unsigned)g_pol_stage[2], (unsigned)g_pol_stage[3],
                         (unsigned)g_pol_stage[4], (unsigned)g_pol_stage[5],
                         (unsigned)g_pol_stage[6], (unsigned)g_pol_stage[7],
                         (unsigned)g_pol_stage[8], (unsigned)g_pol_stage[9],
                         (unsigned)g_poll_ok, (unsigned)g_poll_fail,
                         (unsigned)g_bgm_msgs, (int)g_bgm_state_probe,
                         (unsigned)g_feeder_loops);
                ESP_LOGW("tprobe", "bgm 步骤=%u（1入口 2拿到id 3codec 4表情 5表锁 6表建成 7会话返回）"
                                   " 丢弃[无解码器=%u 断网=%u 置灰=%u] codec写失败=%u",
                         (unsigned)g_bgm_step,
                         (unsigned)g_bgm_drop_nodec, (unsigned)g_bgm_drop_offline,
                         (unsigned)g_bgm_drop_greyed, (unsigned)g_bgm_wr_err);
                /* 【卡顿取证】断供次数 / 周期内最低水位（样本）/ I2S 单次写最长耗时 */
                ESP_LOGW("tprobe", "bgm 音频：断供=%u 最低水位=%d 样本（%d ms）| I2S 写最长=%u us | 音量=%u",
                         (unsigned)g_bgm_underruns, (int)g_bgm_ring_min,
                         (int)(g_bgm_ring_min == 0xFFFFFFFFu ? -1
                               : (int32_t)((uint64_t)g_bgm_ring_min * 1000u /
                                           (uint64_t)((bgm_rate_get() ? bgm_rate_get() : 44100) * 2u))),
                         (unsigned)g_bgm_wr_max_us, (unsigned)bgm_volume_get());
                ESP_LOGW("tprobe", "bgm 卡顿取证：写间隔最长=%u us @%ums | >150ms 次数=%u（DMA 深度 22.05k≈139ms）",
                         (unsigned)g_bgm_gap_max_us, (unsigned)g_bgm_gap_at_ms,
                         (unsigned)g_bgm_gap_over);
                ESP_LOGW("tprobe", "bgm 丢数据取证：缓冲满丢弃 %u B / %u 次（大于 0 = 音乐被跳过，曲子会变短）",
                         (unsigned)g_bgm_drop_bytes, (unsigned)g_bgm_drops);
                g_bgm_ring_min = 0xFFFFFFFFu;
                g_bgm_wr_max_us = 0;
                g_bgm_gap_max_us = 0;
            }
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(33));    /* 30fps ≈ 33ms */
    }
}

/* input 任务是否已创建（早建/兜底共用）+ 放行旗标（任务体等它再初始化） */
volatile bool g_input_go;
static bool g_input_created;

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

/* ══ 【内部 RAM 腾挪 2026-10-02：cJSON 全量改走 PSRAM】══════════════════════
 * 机理：本板 sdkconfig 的 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 规定
 * 「普通 malloc ≤4KB 一律给内部 RAM」。这条**不能往下调**（FatFS 的 4096B
 * 扇区窗口必须 DMA 可及，见 mp_psram.h），于是所有 ≤4KB 的默认堆分配都压在
 * 内部 DRAM 上。cJSON 恰好是最典型的受害者：节点 ~40B/个、每条字符串再一份，
 * 一份清单（真机实测 27,461B）就是数百个节点 + 数百个字符串，**全部**落在
 * 内部堆（清单树 + 每轮 poll 的响应树 + 上行 body 树，是持续的内部堆抖动源）。
 *
 * ⚠️ 诚实边界（避免误判收益）：清单那棵树在 sync_once 里是**先**
 * cJSON_Delete(root) **再** post MANIFEST_SYNCED 的，所以它**不**与随后的
 * "地图装载/素材绑定"窗口重叠 —— 这一条不是"@@素材全绑后掉 31KB"的元凶。
 * 保留它的理由是两个真实但较小的收益：
 *   ① 同步/poll 窗口内的**内部堆峰值**下降（去掉那几十 KB 的瞬时内部占用）；
 *   ② 内部堆**碎片**下降：几百个小块分配再释放，正是"空闲 43KB 但最大块只剩
 *      9.7KB"这类碎片的典型来源之一，而本板的最大块恰恰是硬约束（fopen /
 *      任务栈都要求连续块）。
 * 修法：装全局 hooks，让 cJSON 的 malloc/free 走 PSRAM（失败自动退内部堆）。
 *   · cJSON 只在各任务上下文里解析/打印，**没有任何 ISR 或关 cache 临界区
 *     访问它**，PSRAM 访问安全；
 *   · heap_caps_free 区域无关，PSRAM/内部两块都能还；
 *   · 唯一要求：必须在**第一次 cJSON_* 调用之前**装好 —— 本函数在 app_main_task
 *     入口调用，早于任何任务/网络/渲染代码（全仓 cJSON 调用点都在任务里）。 */
static void *cj_psram_alloc(size_t sz)
{
    return mp_psram_malloc(sz);
}
static void cj_psram_free(void *p)
{
    heap_caps_free(p);      /* 区域无关：PSRAM 与内部堆指针都能还 */
}
static void cjson_hooks_install(void)
{
    cJSON_Hooks h = { .malloc_fn = cj_psram_alloc, .free_fn = cj_psram_free };
    cJSON_InitHooks(&h);
}

static void app_main_task(void *arg)
{
    (void)arg;
    cjson_hooks_install();      /* 必须早于任何 cJSON_* 调用（见上注释） */
    /* 【串口非阻塞 2026-09-27】默认 UART 写是阻塞的：无人读串口时 TX 缓冲满
     * → 打日志的任务被挂住（真机表现：渲染任务 >5s 不喂狗 → E14 熔断关屏）。
     * 设为非阻塞 + 允许覆盖，宁可丢日志也不能拖死任务。 */
    /* 【日志不阻塞 2026-09-27】默认日志走 newlib stdout（带递归锁 + 阻塞写）：
     * 无人读串口时 TX 缓冲塞满 → 打日志的任务被挂住。真机后果是渲染任务 >5s
     * 不喂狗 → E14 三振熔断关屏（且 strike 持久化，只能物理断电恢复）。
     * 这里把日志接到 ROM 的 UART 直写：FIFO 满就丢弃本行，**绝不阻塞**；
     * 不碰 newlib 锁（此前用 uart_vfs_dev_use_driver 在 main_task 上下文里
     * 触发 newlib 锁空指针崩溃 —— 已由 addr2line 定位并移除）。 */

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
    /* IMU 缺失不阻断启动：不带 IMU 的板型（如 1.85B）上 QMI8658 探测 NACK
     * 会一路返回错误，原先 ESP_ERROR_CHECK 直接 abort → 无限重启，IMU 之后
     * 的 RTC/PMU/SD/网络/渲染全部验证不到。app 层已有 ready() 降级路径。 */
    esp_err_t imu_err = imu_qmi8658_init();
    if (imu_err != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 init 失败（%s）：IMU 降级为不可用，继续启动",
                 esp_err_to_name(imu_err));
    }
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
    render_init(&MINIPET_ACTIVE_PROFILE);   /* FATFS 挂载后、首 tick 前（render.h） */
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

    /* 【顺序定案 2026-09-27】渲染任务必须在**配网页/联网之后**、但在任何其它
     * 后台任务之前创建：内部 DRAM 总共 ~180KB，portal+httpd+SoftAP 缓冲 +
     * 渲染栈(8K) 无法同时容纳（真机实测最大连续块 3.3KB → 渲染栈 3.5K 都拿不到
     * → 无渲染降级黑屏）。这里把渲染放在网络任务之后、OTA/BGM 之前，
     * 实测能拿到最大连续块并成功建栈。 */

    /* 【BGM 任务必须早于渲染 2026-09-27】bgm 任务栈 6K、渲染栈 8K，两者在内部
     * DRAM 里只能先到先得：实测先起渲染后，bgm 任务连 6K 都拿不到（
     * `bgm 任务首建失败（内部堆挤压）` 每 10s 重试、永不成功）→ 用户"设了 BGM
     * 没声音"。BGM 是用户直接可感知的功能，优先级高于渲染任务，故先起 bgm。 */
    bgm_start();           /* BGM 解码+feeder+环形缓冲（codec_init 在内） */
    provision_dump_internal_heap("bgm 任务后（渲染任务未创建）");

    bool render_ok = false;
    /* 【无凭据时不起渲染 2026-09-27】设备在配网态（无凭据 → SoftAP + portal）
     * 时，屏上要显示的是配网引导而不是宠物，而 portal 任务 + httpd + SoftAP 已
     * 占掉内部堆（真机最大连续块仅 3572B）→ 渲染任务 8K/6K/5K/4K 全部创建失败，
     * 连试 10 次后彻底没有渲染任务（黑屏且再也起不来）。
     * 需求（胶水）：**没有 TF 卡就渲染默认形象 + 默认地图 + 屏上提示没有 TF 卡**；
     * 配网态则不需要宠物画面。因此这里先判断状态：
     *   · 配网态 → 跳过渲染任务（省下内部堆给配网页，用户配置完会重启进正常流程）
     *   · 其它态 → 起渲染（栈逐级降档到 3.5K，覆盖碎片最坏情况） */
    /* 【状态判定修正 2026-09-27】设备"无凭据但有出厂素材"时状态机进的是
     * **POKER + 并行 portal**（见 state_machine.c 的 self_test），不是
     * WIFI_PROVISION —— 原先只判 WIFI_PROVISION 导致配网页起来后渲染任务仍然
     * 硬试 8K 栈、连败 10 次、17s 后看门狗熔断关屏（真机实证）。
     * 现在：只要 portal 在跑（provision_portal_active）或处于无素材 FATAL，
     * 就让位给配网页；其余情况正常起渲染（栈逐级降档到 3.5K 兜碎片）。 */
    extern bool provision_portal_active(void);
    bool portal_running = provision_portal_active();
    bool skip_render = (state_machine_current() == MP_ST_WIFI_PROVISION) ||
                       (state_machine_current() == MP_ST_FATAL && !sd_ok) ||
                       (portal_running && !sd_ok);
    if (skip_render) {
        ESP_LOGW(TAG, "配网/无素材态（portal=%d sd_ok=%d）→ 跳过渲染任务，内部堆留给配网页",
                 (int)portal_running, (int)sd_ok);
    }
    /* 栈逐级降档：碎片最坏时最大连续块约 3.5KB，8K 固定栈必然失败 */
    static const uint32_t render_stacks[] = { 8192, 6144, 5120, 4096, 3584 };
    /* 【最多试 5 次 2026-09-27】原来 10 次 × 200ms = 2s 内必然全部失败（portal 占着
     * 内部堆，最大连续块仅 ~3.3KB），白白拖延启动并让看门狗在渲染任务缺席时
     * 计振熔断。现在：逐档各试一次（8K→3.5K），全败就**明确记录并交棒**给
     * 无渲染降级路径（心跳/联网/配网页照常，屏幕保持黑屏），不再硬耗。 */
    for (int t = 0; t < (int)(sizeof(render_stacks)/sizeof(render_stacks[0])) && !render_ok && !skip_render; t++) {
        uint32_t stk = render_stacks[t];
        /* 【不要放 PSRAM】曾把渲染栈挪到 PSRAM 省内部 DRAM，真机立刻崩：
         * `assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
         *  (esp_task_stack_is_sane_cache_disabled())` —— flash 写（FAT/OTA）期间
         * cache 关闭，栈在 PSRAM 的任务不可运行。内部栈是硬约束。 */
        if (xTaskCreatePinnedToCore(render_task, "render", stk, NULL, 5, NULL, 1) == pdPASS) {
            render_ok = true;
            ESP_LOGW(TAG, "render 任务已创建（栈 %u，内部 DRAM）", (unsigned)stk);
            break;
        }
        ESP_LOGW(TAG, "render 任务创建失败(栈%u) internal=%u 最大块=%u，降档重试",
                 (unsigned)stk,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    if (!render_ok && !skip_render) {
        ESP_LOGE(TAG, "render 任务最终未能创建（内部堆 空闲=%u 最大块=%u）→ 无渲染降级："
                      "联网/心跳/配网页照常，屏幕保持黑屏；不进入看门狗计振",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        watchdog_render_absent();        /* 渲染缺失时停掉渲染心跳计振（否则必然熔断） */
    }

    /* 早建 input（见尾段注释）：此刻内部堆最宽裕，4096 栈必然拿得到 */
    if (xTaskCreatePinnedToCore(input_task, "input", 4096, NULL, 4, NULL, 1) == pdPASS) {
        g_input_created = true;
        ESP_LOGW(TAG, "input 任务已创建（栈 4096，提前到联网/素材之前）");
    } else {
        ESP_LOGW(TAG, "input 任务早建失败（internal=%u）→ 启动尾段兜底重试",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    /* 【顺序（第四版）2026-09-27】网络任务与联网**放在渲染任务之后**：
     * 真机实测三种顺序后的内部堆空闲/最大块：
     *   渲染先 → 联网后：423B / 244B   （socket 开不出来，指令全丢）
     *   联网先 → 渲染后：3167B / 2804B （渲染栈拿不到 8K，屏幕黑）
     * 本次（bgm → 网络+联网 → 渲染）：让网络栈先拿到它的小块，
     * 渲染再拿大块（8K 栈），期望两者都能落地。 */
    {
        bool psram_ok_early = (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0);
        poller_start();
        events_start();
        asset_dl_start();
        provision_dump_internal_heap("联网前（渲染任务已创建）");
        state_machine_boot(sd_ok, psram_ok_early);
        provision_dump_internal_heap("联网后");
        provision_dump_internal_heap("全部任务创建后");
    }

    /* 【顺序定案（第三版）2026-09-27】网络任务与联网放在**渲染任务之后**：
     * 内部 DRAM ~180KB 装不下"portal+httpd+SoftAP 缓冲 + 渲染栈"，谁先申请谁拿到
     * 连续块。渲染栈是硬需求（没有它整块屏都是黑的），所以让渲染先拿；网络栈
     * 本来就走 PSRAM（CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP）+ 碎片容忍度高。
     * 实测顺序：渲染 → poller/events/asset_dl → state_machine_boot（含联网）。 */

    /* 【任务创建时机修复 2026-09-27：触摸/按键/IMU 全失效的真机根因】
     * 原先在这里（素材全绑 + 联网完成之后）才建 input 任务，而那一刻内部堆实测
     * 只剩 `internal=1875`（素材/字体/LVGL/网络缓冲把内部 DRAM 吃到见底）：
     *   `main: input 任务创建失败 rc=-1 internal=1875`
     * → **input 任务不存在 ⇒ 触摸、三键、IMU 全部失效**（用户报障"点哪都没反应、
     *   中键按了没反应"）。现改为：
     *   ① 渲染任务建好后立刻建 input（此刻内部堆 ~15KB 空闲，4096 栈稳拿到）；
     *   ② 任务本体先等 g_input_go 放行旗标再跑初始化 —— 保持原有初始化顺序
     *      （I2C/触摸/IMU 依赖 state_machine_boot 之后的驱动状态）；
     *   ③ 这里再兜底重试一次（早建失败时）。 */
    if (!g_input_created) {
        if (xTaskCreatePinnedToCore(input_task, "input", 4096, NULL, 4, NULL, 1) == pdPASS) {
            g_input_created = true;
            ESP_LOGW(TAG, "input 任务兜底创建成功（internal=%u）",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        } else {
            ESP_LOGE(TAG, "input 任务创建失败 internal=%u 最大块=%u → 触摸/按键/IMU 将不可用",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
    }
    g_input_go = true;      /* 放行 input 任务初始化（无论早晚建，此处统一放行） */

    /* 后台任务：OTA（BGM 已提到渲染任务之前，见上） */
    ota_start();           /* 双分区升级 */
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
