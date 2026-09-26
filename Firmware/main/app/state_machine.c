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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "sm";
#include "esp_heap_caps.h"

#include "app_core.h"
#include "hal_contract.h"
#include "watchdog.h"
#include "provision.h"
#include "clock_digits.h"    /* CLOCK_ANCHOR_AUTO（问题3 默认居中锚点） */
#include "http_client.h"
#include "asset_dl.h"
#include "ota.h"
#include "bgm.h"

static mp_state_t s_state = MP_ST_BOOT;
static bool       s_online = false;       /* 服务端可达（poller 维护） */
static bool       s_force_sleep = false;  /* <10% 强制睡眠保电（E11） */
static int64_t    s_last_activity_ms;
static SemaphoreHandle_t s_lock;

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
    if (mp_http_hello() == 0) {
        s_online = true;
        asset_dl_request_sync();          /* 联网成功：立即 manifest diff */
        transition(MP_ST_POKER);
        return;
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
        case MP_ST_MENU:
            transition_locked(MP_ST_POKER);
            break;
        case MP_ST_CLOCK_DOZE:
            transition_locked(MP_ST_POKER);
            break;
        default:
            break;   /* OTA/FATAL/PROVISION 下菜单键无效 */
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
        render_bubble_show(cmd->s, RENDER_FONT_24);   /* 协议传 UTF-8（E12） */
        break;
    case MP_CMD_SET_MAP:
        dispatch_map(cmd->s);
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
    case MP_CMD_NET_STATE:
    case MP_CMD_BGM_STATE:
    default:
        /* 查询型状态（bgm_get_state / state_machine_offline_mode）由渲染层轮询 */
        break;
    }
}
