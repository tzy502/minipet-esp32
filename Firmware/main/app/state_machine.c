/**
 * state_machine.c — 应用状态机（software-design 4.3）
 *
 * 迁移图（含切换钩子 on_enter/on_exit）：
 *   BOOT → SELF_TEST → [PSRAM/TF 致命失败]                    → FATAL
 *                     → [无 WiFi 配置]                        → WIFI_PROVISION
 *                     → [WiFi/服务端不可达，有 TF 缓存]        → OFFLINE
 *                     → [正常]                                → POKER
 *   POKER ↔ MENU（菜单键；E7 独立全屏，宠物交互冻结）
 *   POKER/MENU --闲置 N 分钟--> CLOCK_DOZE；任意交互 → POKER（E9）
 *   OFFLINE：TF 缓存独立运行 + BGM 静音；poller 恢复联网 → POKER（E11）
 *   OTA：poller 升级指令 → OTA 态；失败回滚回 POKER（E11）
 *
 * 线规约（render.h）：渲染层 API 只允许在 render 任务调用 ——
 * 状态机一律经 cmd_q 投递（MP_CMD_*），由 render 任务里的
 * app_cmd_dispatch() 落地（含 hash→路径 解析，见 asset_dl 查询面）。
 */
#include "state_machine.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>     /* strtoul（BGM_PLAYID 数字串） */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "sm";
#include "esp_heap_caps.h"

#include "app_core.h"
#include "hal_contract.h"
#include "watchdog.h"
#include "provision.h"
#include "input_dispatch.h"   /* E7：切换后随机表情 */
#include "clock_digits.h"    /* CLOCK_ANCHOR_AUTO（问题3 默认居中锚点） */
#include "http_client.h"
#include "mdns_discover.h"   /* E14：服务端 mDNS 兜底发现 */
#include "asset_dl.h"
#include "ota.h"
#include "bgm.h"

static mp_state_t s_state = MP_ST_BOOT;
static bool       s_online = false;       /* 服务端可达（poller 维护） */
static bool       s_force_sleep = false;  /* <10% 强制睡眠保电（E11） */
static bool       s_mdns_fallback_done;   /* E14：手输地址失败后的 mDNS 兜底只做一次 */
static int64_t    s_last_activity_ms;
static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------ */
/* E11 降级事件上报：离线态进出（Web 可见设备健康）                        */
/* ------------------------------------------------------------------ */
/* events.c 的 event_name() 只映射 touch/tap_light/tap_hard/shake/pickup/
 * tilt_enter/tilt_exit/battery/asset_error/bgm_failover/boot/error —— 没有
 * net_* 类型，且 events.c/app_core.h 本轮不归本改动动 → 不自造事件名（会落到
 * event_name() 的 "unknown" 分支），复用【已映射】的 MP_EVT_ERROR，用 s 区分子类
 * （服务端 DeviceEventRequest.data 原样存 JSON，Web 可按 data.s 区分）：
 *   s="net_offline" a=1     → 降级进离线态（E11 无网络）
 *   s="net_online"  a=0 b=1 → 回网恢复
 * 断网瞬间的 POST 必然失败（events.c「失败即丢，不重放」），故离线事件记下发生
 * 时刻；回网时按原始 ts 补报一次 —— 否则 Web 只能看到「回网」，看不到「掉线」。 */
static bool    s_offline_evt_pending;
static int64_t s_offline_evt_ts_ms;

static void post_net_event(const char *sub, int32_t a, int32_t b, int64_t ts_ms)
{
    mp_event_t e = { .type = MP_EVT_ERROR, .a = a, .b = b, .ts_ms = ts_ms };
    strlcpy(e.s, sub, sizeof(e.s));
    mp_post_event(&e);
}

/* ------------------------------------------------------------------ */
/* 工具                                                                 */
/* ------------------------------------------------------------------ */
static void cmd_simple(mp_cmd_type_t t, const char *s, int32_t a, int32_t b)
{
    mp_cmd_t c = { .type = t, .a = a, .b = b };
    if (s) strlcpy(c.s, s, sizeof(c.s));
    mp_post_cmd(&c);
}

/* 未配网常驻横幅（问题4）：进 POKER/OFFLINE 时无 WiFi 配置 → 顶部 480×28
 * 深色底白字提示 SoftAP 热点名（5x7 内嵌字体只认大写 → SSID 转大写） */
static void post_banner_if_needed(void)
{
    if (provision_has_config()) {
        cmd_simple(MP_CMD_BANNER, NULL, 0, 0);      /* 已配网：隐藏 */
        return;
    }
    char ssid[16];
    provision_get_ap_ssid(ssid, sizeof(ssid));
    for (char *p = ssid; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    char msg[48];
    snprintf(msg, sizeof(msg), "WIFI AP: %s", ssid);
    cmd_simple(MP_CMD_BANNER, msg, 1, 0);
}

/* 问题3：时钟图源路径——优先 fontTime 专属 PARTS 包（selector==clock），
 * 缺省回退当前默认 PARTS 包（fontTime id 900..912 已内置于装扮包） */
static bool clock_parts_path(char *path, size_t cap);

static void transition_locked(mp_state_t next)
{
    if (next == s_state) return;

    /* ---- on_exit 钩子 ---- */
    switch (s_state) {
    case MP_ST_MENU:
        cmd_simple(MP_CMD_MENU_EXIT, NULL, 0, 0);   /* MENU → 任意：宠物态恢复 */
        break;
    case MP_ST_WIFI_PROVISION:
        provision_stop();            /* 配网完成/中止：拆 portal */
        break;
    default:
        break;
    }

    mp_state_t prev = s_state;
    s_state = next;

    /* ---- on_enter 钩子 ---- */
    switch (next) {
    case MP_ST_WIFI_PROVISION:
        provision_start_portal();    /* SoftAP + captive portal（E14） */
        break;

    case MP_ST_POKER:
        cmd_simple(MP_CMD_SET_ACTION, MP_ACTION_STAND, 0, 0);
        if (prev == MP_ST_CLOCK_DOZE) {
            cmd_simple(MP_CMD_CLOCK, NULL, 0, 0);               /* 收时钟 */
            cmd_simple(MP_CMD_SET_EXPRESSION, MP_EXPR_BLINK, 0, 0);  /* 唤醒瞬目 */
        }
        post_banner_if_needed();     /* 问题4：无配置 → 常驻配网横幅 */
        /* 闲置计时只由用户交互/用户可达的场景切换刷新（boot/自检/菜单/唤醒/
         * OTA 回滚）。OFFLINE→POKER 是 poller 回网驱动：路由器掐长轮询空闲
         * 连接会让 poll 周期性失败重连，若在此刷新 s_last_activity_ms，
         * 网络抖动会把「闲置」永远清零 → CLOCK_DOZE 永不进入（真机症状） */
        if (prev != MP_ST_OFFLINE) {
            s_last_activity_ms = mp_now_ms();
        }
        break;

    case MP_ST_MENU:
        cmd_simple(MP_CMD_MENU_ENTER, NULL, 0, 0);  /* E7 独立全屏选择器 */
        s_last_activity_ms = mp_now_ms();
        break;

    case MP_ST_CLOCK_DOZE:
        /* E9：待机时钟浮现（AMOLED 纯黑背景只数字发光，RTC 独立走时）；
         * fontTime 素材与地图锚点由 dispatch 查 asset_dl */
        cmd_simple(MP_CMD_CLOCK, NULL, 1, 0);
        cmd_simple(MP_CMD_SET_EXPRESSION, MP_EXPR_DEFAULT, 0, 0);
        break;

    case MP_ST_OFFLINE:
        /* E11 无网降级：TF 缓存继续跑（布局表本地播，blink 本地循环），
         * BGM 静音；poller 指数退避自动回网 */
        bgm_set_offline(true);
        cmd_simple(MP_CMD_NET_STATE, NULL, 0, 0);
        cmd_simple(MP_CMD_BUBBLE, "离线模式：本地缓存运行", 0, 0);
        post_banner_if_needed();     /* 问题4：离线+未配网同样提示热点 */
        /* E11 降级事件上报：进离线态（断网期 POST 多半丢 → 回网补报，见上方说明） */
        s_offline_evt_ts_ms = mp_now_ms();
        s_offline_evt_pending = true;
        post_net_event("net_offline", 1, 0, s_offline_evt_ts_ms);
        ESP_LOGW(TAG, "E11 降级：进 OFFLINE，上报 net_offline（type=error/a=1）");
        break;

    case MP_ST_OTA:
        cmd_simple(MP_CMD_OTA_BEGIN, "固件升级中…", 0, 0);
        break;

    case MP_ST_FATAL:
        /* 纯文本 + 关屏（watchdog 提供，不进渲染路径） */
        break;

    default:
        break;
    }
}

static void transition(mp_state_t next)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    transition_locked(next);
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* 自检（E14：WiFi/内存/TF/服务端连通）                                  */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* E14 mDNS 兜底：设备自动发现服务端（配网页手输地址的「补充」）           */
/* ------------------------------------------------------------------ */
/* 语义（严格按需求「手输地址优先」）：
 *   1) NVS srv_url 有值且 hello 成功 → 一次 mDNS 都不发（手输优先）
 *   2) srv_url 为空（用户配网时留空）→ 启动即发现一次，命中就用
 *   3) srv_url 有值但连不上 → 发现一次兜底（服务端换了 IP / 手输地址写错）
 * 约束：超时短（~2.2s，见 mdns_discover.c），失败静默；发现结果**不写 NVS**
 * （否则会把手输地址顶掉，且 DHCP 换 IP 后永远用旧值）。
 * 调用点都在自检期（WiFi 已拿 IP），不在任何高频路径上。 */
static bool mdns_try_discover(void)
{
    char url[64];
    if (!mp_mdns_discover_server(url, sizeof(url))) return false;
    mp_http_set_server_url(url);
    return true;
}

/* ------------------------------------------------------------------ */
/* 自检（E14：WiFi/内存/TF/服务端连通）                                  */
/* ------------------------------------------------------------------ */
static bool mdns_try_discover(void);   /* E14：定义在同文件下方（mDNS 兜底） */

/* 【掉线自愈】provision.c 提供（本地 extern 声明，不动 provision.h）：
 * STA 是否已拿到 IP —— 用来判断"是网络断了"还是"只是服务端不可达" */
extern bool provision_wifi_is_connected(void);

static void self_test(bool sd_ok, bool psram_ok)
{
    s_state = MP_ST_SELF_TEST;

    /* 1) PSRAM：渲染大缓冲的物理前提 */
    if (!psram_ok) {
        transition(MP_ST_FATAL);
        watchdog_fatal_show("MINIPET BOOT FAULT", "PSRAM CHECK FAILED");
        return;
    }

    /* 2) TF 卡：素材缓存层（E11 降级矩阵的地基） */
    if (!sd_ok) {
        transition(MP_ST_FATAL);
        watchdog_fatal_show("MINIPET BOOT FAULT", "TF CARD MOUNT FAIL");
        return;
    }

    /* 3) WiFi 配置：无配置但有本地素材 → 直接离线起播（出厂素材保底，
     *    不让用户面对黑屏）。无素材 → 配网 + 屏显提示 */
    if (!provision_has_config()) {
        if (asset_dl_have_local_manifest()) {
            /* 问题4：离线起播的同时把 SoftAP portal 拉起（旧实现 portal 没启动，
             * 用户无从配网）；POKER on_enter 另发顶部常驻横幅提示热点名 */
            ESP_LOGW(TAG, "无 WiFi 配置但有本地素材 → 离线起播 + 启动配网 portal");
            provision_start_portal();
            transition(MP_ST_POKER);
            return;
        }
        transition(MP_ST_WIFI_PROVISION);
        watchdog_text_persist("NO WIFI CONFIG", "AP: MINIPET-XXXX");
        return;   /* portal 完成后自行重启 */
    }

    /* 4) WiFi 连接（20s）；失败 → OFFLINE（TF 缓存跑，poller 后台回网） */
    if (provision_wifi_connect_sta(20000) != ESP_OK) {
        goto offline_check_cache;
    }

    /* 5) 服务端连通：hello（含 profile/UUID/固件版本注册，E2） */
    mp_http_init();
    if (!mp_http_server_url()) {
        /* 5a) srv_url 为空（配网页留空）：mDNS 兜底发现（E14）。
         * 发现不到 = 保持未配置，走下方 offline_check_cache（绝不阻塞黑屏）。 */
        ESP_LOGI(TAG, "srv_url 未配置 → 尝试 mDNS 发现 _minipet._tcp（E14）");
        mdns_try_discover();
    }
    if (mp_http_hello() == 0) {
        s_online = true;
        asset_dl_request_sync();          /* 联网成功：立即 manifest diff */
        transition(MP_ST_POKER);
        return;
    }

    /* 5b) 手输地址优先但连不上 → mDNS 兜一次（服务端换 IP / 地址写错）。
     * 无论成败都把内存地址还原成手输值：发现结果只做「本次运行」的兜底，
     * 绝不覆盖用户配置（手输优先 —— E14 原文）。每次开机至多一次。 */
    if (mp_http_server_url() && !s_mdns_fallback_done) {
        s_mdns_fallback_done = true;
        char manual[128];
        strlcpy(manual, mp_http_server_url(), sizeof(manual));
        ESP_LOGW(TAG, "hello 失败（%s 不可达）→ mDNS 兜底发现（E14）", manual);
        if (mdns_try_discover()) {
            if (mp_http_hello() == 0) {
                s_online = true;
                asset_dl_request_sync();
                transition(MP_ST_POKER);
                return;
            }
            ESP_LOGW(TAG, "mDNS 发现的地址同样 hello 失败 → 回落离线");
        }
        mp_http_set_server_url(manual);   /* 还原手输地址（本次运行内的地址即配置快照） */
    }

offline_check_cache:
    mp_http_init();
    if (!asset_dl_have_local_manifest()) {
        /* 未配对 + 首启无网 + TF 为空 = 黑屏 FATAL（design-review）：
         * 常驻文本提示（不关屏），引导用户恢复网络 */
        transition(MP_ST_FATAL);
        watchdog_text_persist("MINIPET NO ASSETS", "CHECK WIFI/SERVER");
        return;
    }
    /* 【真机恢复能力 2026-09-27】有凭据但服务端连不上 → 拉起配网 portal：
     *   - 排障不用插线：手机连 MiniPet-XXXX 即可核对/改服务器地址、重选 WiFi
     *   - 换网/服务端换 IP 时有人工入口，不必连电脑擦 NVS
     * 但要**延后**：provision_start_portal() 会把 WiFi 切成 APSTA 并起 SoftAP，
     * 真机实测紧随其后出现 `WiFi 断开 reason=8`（STA 被模式切换打断）——
     * 而 poller 的 hello 补发本来很可能在几秒后成功（已验证可上线）。
     * 因此：只有"连续多次仍然连不上"才拉 portal，给 STA 留稳定窗口。 */
    {
        static int s_boot_hello_fails;
        /* 【自愈优先 2026-09-27】拉 portal 前先看 STA 链路：已经有 IP（只是服务端
         * TCP 不可达，典型是 NAS 重启/换 IP）时**不要**拉 portal ——
         * provision_start_portal() 会把 WiFi 切成 APSTA 并起 SoftAP，真机实测紧随
         * 其后 `WiFi 断开 reason=8`（STA 被模式切换打断），而且 poller 在 portal
         * 活动期间是停摆的（poller.c 的 portal 等待循环）→ 反而把"分钟级自愈"
         * 变成"一直离线"。只有连 WiFi 都连不上（凭据/信号问题）才值得开配网入口。
         * 注：本分支只在自检里自增一次（self_test 每次开机只调用一次），阈值 3
         * 实际不可达 —— 这里顺手写成正确语义，避免以后有人挪动调用点踩坑。 */
        if (++s_boot_hello_fails >= 3 && !provision_wifi_is_connected()) {
            ESP_LOGW(TAG, "服务端连续 %d 次不可达且 STA 未连上 → 并行拉起配网 portal"
                          "（凭据保留，poller 继续回网）", s_boot_hello_fails);
            provision_start_portal();
        } else if (s_boot_hello_fails >= 3) {
            ESP_LOGW(TAG, "服务端连续 %d 次不可达但 STA 已拿到 IP → 不拉 portal"
                          "（APSTA 切换会打断 STA 且 poller 停摆），交给 poller 自愈",
                     s_boot_hello_fails);
        } else {
            ESP_LOGW(TAG, "服务端不可达（第 %d 次）→ 先交给 poller 补发 hello，"
                          "暂不拉 portal（避免 APSTA 切换打断 STA）", s_boot_hello_fails);
        }
    }
    transition(MP_ST_OFFLINE);
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                             */
/* ------------------------------------------------------------------ */
void state_machine_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_last_activity_ms = mp_now_ms();
}

void state_machine_boot(bool sd_ok, bool psram_ok)
{
    bool psram = psram_ok &&
                 (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) >= (4u << 20));
    self_test(sd_ok, psram);

    /* 本地已有素材清单 → 让渲染任务立即从 TF 起播（离线也可跑，E11） */
    ESP_LOGW(TAG, "boot: have_local=%d → 发 MANIFEST_SYNCED", (int)asset_dl_have_local_manifest());
    if (asset_dl_have_local_manifest()) {
        cmd_simple(MP_CMD_MANIFEST_SYNCED, NULL, 0, 0);
    }
}

mp_state_t state_machine_current(void)
{
    return s_state;
}

const char *state_machine_name(mp_state_t st)
{
    switch (st) {
    case MP_ST_BOOT:           return "BOOT";
    case MP_ST_SELF_TEST:      return "SELF_TEST";
    case MP_ST_WIFI_PROVISION: return "WIFI_PROVISION";
    case MP_ST_POKER:          return "POKER";
    case MP_ST_MENU:           return "MENU";
    case MP_ST_CLOCK_DOZE:     return "CLOCK_DOZE";
    case MP_ST_OFFLINE:        return "OFFLINE";
    case MP_ST_OTA:            return "OTA";
    case MP_ST_FATAL:          return "FATAL";
    }
    return "?";
}

bool state_machine_menu_open(void)
{
    return s_state == MP_ST_MENU;
}

bool state_machine_offline_mode(void)
{
    return !s_online;
}

/* 用户活动通知：只准交互通路调用（input_dispatch 触摸/按键/IMU）。
 * 网络/后台任务禁用——poller 周期性收发不算用户活动，否则闲置计时被
 * 永远清零，CLOCK_DOZE 永不进入（E9）。 */
void state_machine_notify_activity(void)
{
    s_last_activity_ms = mp_now_ms();
    if (s_state == MP_ST_CLOCK_DOZE) {
        state_machine_handle(MP_SM_EV_TOUCH);   /* 任意交互立即唤醒（E9） */
    }
}

void state_machine_handle(mp_sm_event_t ev)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;

    switch (ev) {
    case MP_SM_EV_MENU_KEY:
        /* 400ms 硬限速：真机一次按压曾产生 6 连发（400ms 间隔）导致菜单关了又开。
         * 人手不可能 400ms 内两次有效按压 */
        {
            static int64_t s_last_menu_key_ms;
            int64_t now = mp_now_ms();
            if (now - s_last_menu_key_ms < 400) break;
            s_last_menu_key_ms = now;
        }
        switch (s_state) {
        case MP_ST_POKER:
        case MP_ST_OFFLINE:      /* 离线也可开菜单：只显本地缓存项（E7/E11） */
            transition_locked(MP_ST_MENU);
            break;
        case MP_ST_SELF_TEST:
        case MP_ST_WIFI_PROVISION:
            /* 【2026-09-27 真机】开机自检/配网期（WiFi 探测最长 20s + hello 数秒）
             * 菜单键此前被 default 丢弃 → 用户"启动的时候菜单呼不出来"。
             * 处理：允许在自检期开菜单——菜单只读本地缓存，不依赖自检结果；
             * 退出仍走 MENU→POKER，自检后台继续跑（boot 完成后再落 POKER/配网态）。 */
            transition_locked(MP_ST_MENU);
            break;
        case MP_ST_MENU:
            transition_locked(MP_ST_POKER);
            break;
        case MP_ST_CLOCK_DOZE:
            transition_locked(MP_ST_POKER);
            break;
        default:
            break;   /* OTA/FATAL 下菜单键无效 */
        }
        break;

    case MP_SM_EV_TOUCH:
    case MP_SM_EV_IMU:
        if (s_state == MP_ST_CLOCK_DOZE) {
            transition_locked(MP_ST_POKER);   /* 唤醒回桌宠态（E9） */
        }
        s_last_activity_ms = mp_now_ms();
        break;

    case MP_SM_EV_IDLE_TIMEOUT:
        if (s_state == MP_ST_POKER || s_state == MP_ST_MENU ||
            s_state == MP_ST_OFFLINE) {
            transition_locked(MP_ST_CLOCK_DOZE);   /* 菜单闲置也收起再睡 */
        }
        break;

    case MP_SM_EV_NET_ONLINE:
        s_online = true;
        bgm_set_offline(false);
        asset_dl_request_sync();          /* manifest 补拉（E11：回网自动同步） */
        mp_ota_confirm_valid();           /* OTA 新分区稳定验证（E11 回滚机制） */
        if (s_state == MP_ST_OFFLINE) {
            /* E11 降级事件上报：回网恢复。此刻网络已通，POST 可达：
             * 先按原始时刻补报离线事件（补上断网期丢掉的那笔），再报恢复 */
            bool replayed = s_offline_evt_pending;
            if (replayed) {
                post_net_event("net_offline", 1, 0, s_offline_evt_ts_ms);
                s_offline_evt_pending = false;
            }
            post_net_event("net_online", 0, 1, mp_now_ms());
            ESP_LOGW(TAG, "E11 恢复：OFFLINE→POKER，上报 net_online（离线补报=%d）",
                     (int)replayed);
            transition_locked(MP_ST_POKER);
        }
        break;

    case MP_SM_EV_NET_OFFLINE:
        s_online = false;
        bgm_set_offline(true);            /* BGM 静音降级（E8/E11） */
        if (s_state == MP_ST_POKER) {
            transition_locked(MP_ST_OFFLINE);
        }
        break;

    case MP_SM_EV_OTA_START:
        if (s_state != MP_ST_FATAL && s_state != MP_ST_WIFI_PROVISION) {
            transition_locked(MP_ST_OTA);
        }
        break;

    case MP_SM_EV_OTA_END:
        if (s_state == MP_ST_OTA) {
            transition_locked(MP_ST_POKER);   /* 失败回滚旧分区后回正常态 */
        }
        break;

    case MP_SM_EV_BATTERY_CRIT:
        /* <10%：强制睡眠保电 = 纯时钟态（E11）；充电解除 */
        s_force_sleep = true;
        if (s_state == MP_ST_POKER || s_state == MP_ST_MENU || s_state == MP_ST_OFFLINE) {
            transition_locked(MP_ST_CLOCK_DOZE);
        }
        break;

    case MP_SM_EV_BATTERY_OK:
        s_force_sleep = false;
        break;

    case MP_SM_EV_FATAL:
        transition_locked(MP_ST_FATAL);
        watchdog_fatal_show("MINIPET FATAL", "SEE SERVER LOG");
        break;

    case MP_SM_EV_NONE:
    default:
        break;
    }

    xSemaphoreGive(s_lock);
}

void state_machine_tick_1hz(void)
{
    /* 低电强制睡眠的持续钳制（防止交互又唤醒） */
    if (s_force_sleep && (s_state == MP_ST_POKER || s_state == MP_ST_MENU)) {
        state_machine_handle(MP_SM_EV_BATTERY_CRIT);
        return;
    }

    if (s_state != MP_ST_POKER && s_state != MP_ST_MENU && s_state != MP_ST_OFFLINE) {
        return;
    }

    int64_t idle_ms = mp_now_ms() - s_last_activity_ms;
    uint32_t limit_ms = (uint32_t)g_mp_cfg.idle_to_clock_min * 60u * 1000u;
    if (limit_ms == 0) limit_ms = 5u * 60u * 1000u;
    if (idle_ms >= (int64_t)limit_ms) {
        state_machine_handle(MP_SM_EV_IDLE_TIMEOUT);
    }
}

/* ================================================================== */
/* cmd_q 落地（render 任务每帧排空后调用 —— 本函数是渲染层唯一调用点）    */
/* ================================================================== */

/* 动作 → 布局包绑定；loop：持续动作循环播，触发型单次播完自动回 stand（E5） */
static void dispatch_action(const char *action)
{
    char path[MP_MPK_PATH_MAX];
    if (!asset_dl_layout_path(action, path, sizeof(path))) {
        return;                          /* 布局缺（未下发/被淘汰）：保留旧画面 */
    }
    bool loop = (strcmp(action, MP_ACTION_STAND) == 0 ||
                 strcmp(action, MP_ACTION_WALK) == 0 ||
                 strcmp(action, MP_ACTION_FLY) == 0);
    render_set_layout(path, loop);
}

/* 按 hash 换装扮（E13）：hash → parts 路径 → render_set_parts */
/* E7：切换（地图/装扮）完成后的随机表情反馈。
 * 从 25 个实证表情里随机挑一个"有表现力但不突兀"的（排除 default/blink 这类
 * 常态表情），持续 1.5s 后由既有表情 FSM 自动回 default。 */
static void switch_random_expression(void)
{
    static const char *pool[] = {
        MP_EXPR_SMILE, MP_EXPR_LOVE, MP_EXPR_CHEERS, MP_EXPR_SHINE,
        MP_EXPR_WINK,  MP_EXPR_CHU,   MP_EXPR_GLITTER, MP_EXPR_HUM,
    };
    uint32_t k = esp_random() % (sizeof(pool) / sizeof(pool[0]));
    ESP_LOGI(TAG, "E7 切换完成 → 随机表情 %s", pool[k]);
    input_trigger_expression(pool[k], 1500);
}

static void dispatch_set_parts_by_hash(const char *hash)
{
    char path[MP_MPK_PATH_MAX];
    if (!hash || !asset_dl_parts_path(hash, path, sizeof(path))) {
        ESP_LOGW(TAG, "SET_PARTS：hash %s 无对应部件包", hash ? hash : "(null)");
        return;
    }
    int rc = render_set_parts(path);
    ESP_LOGI(TAG, "换装 %s rc=%d", path, rc);
}

static void dispatch_map(const char *hash)
{
    char bg[MP_MPK_PATH_MAX];
    static char strips[8][MP_MPK_PATH_MAX];   /* BGMAP 条带引用（§五） */

    asset_dl_set_active_map(hash);
    asset_dl_touch(hash);                     /* E7：切过的秒切（LRU 前排） */

    if (!asset_dl_map_path(hash, bg, sizeof(bg))) return;
    int n = asset_dl_map_strips(bg, strips, 8);
    if (n < 0) n = 0;
    render_set_map(bg, (n > 0) ? (const char **)strips : NULL, n);

    /* 地图时钟锚点随地图切换预置（E9/R15；开关留待 CLOCK 指令）。
     * 问题3：无锚点 → CLOCK_ANCHOR_AUTO（渲染层整块居中屏幕 240,120） */
    char ft[MP_MPK_PATH_MAX];
    int16_t ax = 0, ay = 0;
    bool has_anchor = asset_dl_clock_anchor(&ax, &ay);
    if (clock_parts_path(ft, sizeof(ft))) {
        render_set_clock(ft, has_anchor ? ax : CLOCK_ANCHOR_AUTO,
                         has_anchor ? ay : CLOCK_ANCHOR_AUTO, false);
    }
}

/* 问题3：时钟图源路径——优先 fontTime 专属 PARTS 包（selector==clock），
 * 缺省回退当前默认 PARTS 包（fontTime id 900..912 已内置于装扮包） */
static bool clock_parts_path(char *path, size_t cap)
{
    if (asset_dl_fonttime_path(path, cap)) return true;
    return asset_dl_parts_path(NULL, path, cap);
}

static void dispatch_clock(int enable)
{
    int16_t ax = 0, ay = 0;
    bool has = asset_dl_clock_anchor(&ax, &ay);   /* 无地图/无表项 → AUTO 居中 */
    if (!enable) {
        /* 收时钟（DOZE→POKER 唤醒）不依赖素材在位：fontTime 包可能已被 LRU
         * 淘汰/尚未同步，若查路径失败直接 return，时钟永远收不掉 → 永久黑屏。
         * render.h 契约：path=NULL 仅改锚点/开关 → 关闭必成功，
         * render_set_clock 内部做全幅重合成（= 唤醒后强制重绘一帧，宠物恢复） */
        render_set_clock(NULL, CLOCK_ANCHOR_AUTO, CLOCK_ANCHOR_AUTO, false);
        return;
    }
    char ft[MP_MPK_PATH_MAX];
    if (!clock_parts_path(ft, sizeof(ft))) return;   /* 素材未就绪：留在当前画面 */
    render_set_clock(ft, has ? ax : CLOCK_ANCHOR_AUTO,
                     has ? ay : CLOCK_ANCHOR_AUTO, true);
}

/* 素材就绪（boot 本地清单 or 网络同步完成）→ 渲染层全量重绑 */
static void dispatch_manifest_synced(void)
{
    char path[MP_MPK_PATH_MAX];
    ESP_LOGW(TAG, "dispatch_manifest_synced 进入");

    /* 字体三档（气泡 24 / 列表 16 / 标题 32，E12） */
    static const struct { render_font_t id; int px; } fonts[] = {
        { RENDER_FONT_16, 16 }, { RENDER_FONT_24, 24 }, { RENDER_FONT_32, 32 },
    };
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        if (asset_dl_font_path(fonts[i].px, path, sizeof(path))) {
            ESP_LOGW(TAG, "set_font px=%d 开始 %s", fonts[i].px, path);
            render_set_font(fonts[i].id, path);
            ESP_LOGW(TAG, "set_font px=%d 完成", fonts[i].px);
        }
    }

    /* 默认纸娃娃部件 + 站立布局（E13：每设备独立装扮） */
    if (asset_dl_parts_path(NULL, path, sizeof(path))) {
        int prc = render_set_parts(path);
        ESP_LOGW(TAG, "parts 路径=%s rc=%d", path, prc);
    } else {
        ESP_LOGE(TAG, "parts 路径查询失败（清单里没有 PARTS）");
    }
    /* 加载失败不黑屏：屏显文字提示（E11 素材故障 → dam 语义的文本版） */
    bool l_ok = asset_dl_layout_path("stand1", path, sizeof(path));
    int lrc = -1;
    if (l_ok) { lrc = render_set_layout(path, true); ESP_LOGW(TAG, "layout 路径=%s rc=%d", path, lrc); }
    if (!l_ok || lrc != 0 ||
        !asset_dl_parts_path(NULL, path, sizeof(path))) {
        ESP_LOGE(TAG, "本地素材加载失败（parts/stand1 缺失）");
        transition(MP_ST_FATAL);
        watchdog_text_persist("ASSET LOAD FAILED", "WAIT SERVER SYNC");
        return;
    }
    dispatch_action(MP_ACTION_STAND);

    /* 字体绑定完成后重显配对码（hello 早于字体加载，首显气泡会是空） */
    {
        const char *code = mp_http_pairing_code();
        if (code && code[0]) {
            mp_cmd_t c = { .type = MP_CMD_PAIRING_CODE };
            strlcpy(c.s, code, sizeof(c.s));
            mp_post_cmd(&c);
        }
    }

    /* 素材全量重绑后强制一次全屏重绘：清除面板自检色块/旧画面残留
     * （无 BGMAP → 全屏填黑；有 BGMAP → static_back+tile），此后每帧走脏区 */
    render_force_redraw();
}

void app_cmd_dispatch(const mp_cmd_t *cmd)
{
    switch (cmd->type) {
    case MP_CMD_SET_ACTION:
        dispatch_action(cmd->s);
        break;
    case MP_CMD_SET_EXPRESSION:
        render_set_expression(cmd->s);
        break;
    case MP_CMD_BUBBLE:
        /* 【E9/E12 互斥 2026-09-27】待机时钟态（CLOCK_DOZE）是纯黑只数字全屏，
         * 合成器在时钟激活时提前 return（不画气泡层）→ 此态收到气泡（E12 静置
         * 台词默认 300s 与 E9 待机时钟默认 5min 同刻触发）会静默丢失。
         * 按 E9 定稿「交互（触摸/按键/IMU）→ 立即唤醒回桌宠态」的口径，
         * 先唤醒再显示：气泡可见，且用户看到台词时不会停在黑屏时钟态。 */
        if (state_machine_current() == MP_ST_CLOCK_DOZE) {
            state_machine_notify_activity();
            ESP_LOGI(TAG, "气泡指令到达：先从待机时钟唤醒回桌宠态");
        }
        render_bubble_show(cmd->s, RENDER_FONT_24);   /* 协议传 UTF-8（E12） */
        break;
    case MP_CMD_SET_MAP:
        dispatch_map(cmd->s);
        /* 【E7 补齐 2026-09-27】需求：「切换完成后宠物反应 = 随机表情」。
         * 此前切换路径无任何 render_set_expression 调用（核对报告列为缺口）。 */
        switch_random_expression();
        break;
    case MP_CMD_SET_PARTS:
        dispatch_set_parts_by_hash(cmd->s);
        switch_random_expression();   /* E7：换装完成同样给随机表情反馈 */
        break;
    case MP_CMD_BRIGHTNESS:
        display_brightness((uint8_t)cmd->a);
        g_mp_cfg.brightness = (uint8_t)cmd->a;
        break;
    case MP_CMD_REBOOT:
        esp_restart();
        break;
    case MP_CMD_PAIRING_CODE:
        /* E13：屏显 6 位配对码 + 配对成功 cheers */
        render_bubble_show(cmd->s, RENDER_FONT_24);
        render_set_expression(MP_EXPR_CHEERS);
        break;
    case MP_CMD_MANIFEST_SYNCED:
        dispatch_manifest_synced();
        break;
    case MP_CMD_MENU_ENTER:
        render_enter_menu();                 /* E7 独立全屏（LVGL 接管写屏） */
        break;
    case MP_CMD_MENU_EXIT:
        render_exit_menu();
        break;
    case MP_CMD_CLOCK:
        dispatch_clock(cmd->a);              /* E9 待机时钟浮现/收起 */
        break;
    case MP_CMD_BANNER:
        /* 问题4：未配网常驻横幅（a=1 显示 s=文本 / a=0 隐藏） */
        if (cmd->a) render_banner_show(cmd->s);
        else render_banner_hide();
        break;
    case MP_CMD_OTA_BEGIN:
        render_bubble_show("固件升级中…", RENDER_FONT_24);
        break;
    case MP_CMD_OTA_FAIL:
        render_bubble_show("升级失败：已回滚", RENDER_FONT_24);
        render_set_expression(MP_EXPR_DAM);
        break;
    case MP_CMD_OTA_DONE:
        render_bubble_show("升级完成，重启中", RENDER_FONT_24);
        break;
    case MP_CMD_BGM_TOGGLE:                 /* BGM 控制：转调 bgm（audio_q 异步生效） */
        bgm_toggle_pause();
        break;
    case MP_CMD_BGM_NEXT:
        bgm_next();
        break;
    case MP_CMD_BGM_PREV:
        bgm_prev();
        break;
    case MP_CMD_BGM_PLAYID:                 /* s=数字串曲目 id */
        bgm_play_id((uint32_t)strtoul(cmd->s, NULL, 10));
        break;
    case MP_CMD_NET_STATE:
    case MP_CMD_BGM_STATE:
    default:
        /* 查询型状态（bgm_get_state / state_machine_offline_mode）由渲染层轮询 */
        break;
    }
}
