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
#include "sd_tf.h"

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
#include "mp_psram.h"
#include "provision.h"
#include "nvs.h"              /* §3.3 相机 per-map 持久化（namespace "cam"） */
#include "input_dispatch.h"   /* E7：切换后随机表情 */
#include "clock_digits.h"    /* CLOCK_ANCHOR_AUTO（问题3 默认居中锚点） */
#include "http_client.h"
#include "mdns_discover.h"   /* E14：服务端 mDNS 兜底发现 */
#include "asset_dl.h"
#include "ota.h"
#include "bgm.h"
/* 【契约 §3.2 相机接口】render_cam_supported/set（compositor.h 明示"相机 UX 层
 * 请 #include compositor.h，render.h 不转出本组接口"）——本文件用于 §3.3 的
 * 「装载地图 → 读 NVS 应用相机」= 全局加载。 */
#include "compositor.h"

/* ══ 【本轮新增的静态状态】（2026-10-02）
 * s_map_before_doze：待机专用背景图（manifest doze_map）切换前的当前图，唤醒切回用；
 * s_redraw_done   ：首次绑定后是否已做过整屏重绘（"素材未变 → 跳过重绘（防闪）"用）。 */
static char s_map_before_doze[32];
static bool s_redraw_done;

/* ══ 【气泡整体去除 2026-10-01 · 用户口径（两板同口径）】════════════════════
 * "整体去除气泡 效果不好" → 桌宠不再显示任何对话气泡；1.85B 同步同改。
 * 所有气泡渲染收敛到下面的包装函数，CONFIG_MP_BUBBLE_ENABLE=n（默认）时整段不画。
 * 【配对码不能一起删】MP_CMD_PAIRING_CODE 原本也走气泡（屏显 6 位码）→ 删了
 * 新设备无法配对；该分支已改走**顶部常驻横幅**（黑底白字，同一可见通道）。 */
#ifndef CONFIG_MP_BUBBLE_ENABLE
#define MP_BUBBLE_ENABLE 0
#else
#define MP_BUBBLE_ENABLE CONFIG_MP_BUBBLE_ENABLE
#endif

static void bubble_show(const char *text, render_font_t font)
{
#if MP_BUBBLE_ENABLE
    render_bubble_show(text, font);
#else
    (void)text; (void)font;
#endif
}

static mp_state_t s_state = MP_ST_BOOT;
static bool       s_online = false;       /* 服务端可达（poller 维护） */
static bool       s_force_sleep = false;  /* <10% 强制睡眠保电（E11） */
static bool       s_mdns_fallback_done;   /* E14：手输地址失败后的 mDNS 兜底只做一次 */
static int64_t    s_last_activity_ms;
static SemaphoreHandle_t s_lock;

/* 【活动地图持久化】NVS 助手定义在文件后段，boot 段先用 → 这里前置声明 */
static void active_map_save(const char *map_id);
static const char *active_map_get(char *buf, size_t cap);

/* 【相机拖动压测】连续平移相机 steps 次、每次 step 世界 px，逐次打点。
 * 判读：每步耗时 ≈ 整屏合成/上屏成本 ⇒ 瓶颈在合成或 blit；
 *       个别步尖峰（跨瓦片列那一拍）⇒ 瓶颈在 SD 补块。
 * 触发通道：服务端 `{"type":"camtest"}`（需服务端白名单）**或**
 * 气泡魔数 `::camtest <步数>,<步长>`（走已有 bubble 通道，无需部署服务端）。 */
/* 【服务端相机挂起】见 MP_CMD_CAM_SET：地图没装好时先记住，装载成功后补上 */
bool sm_cam_nvs_set(const char *key, int32_t x, int32_t y);   /* 定义在后段 */
static bool    s_cam_pending;
static int32_t s_cam_pending_x, s_cam_pending_y;
static char    s_cam_pending_map[16];      /* 目标地图 id（空=不限定） */

static void cam_pending_apply(void)
{
    if (!s_cam_pending || !render_cam_supported()) return;
    /* 目标图校验：不等目标图装载完就不应用（防"应用到错误的图"） */
    if (s_cam_pending_map[0]) {
        char cur[16] = "";
        const char *h = asset_dl_active_map_hash();
        if (h) asset_dl_map_id_of(h, cur, sizeof cur);
        if (strcmp(cur, s_cam_pending_map) != 0) return;
    }
    s_cam_pending = false;
    render_cam_set(s_cam_pending_x, s_cam_pending_y);
    int32_t gx = 0, gy = 0;
    render_cam_get(&gx, &gy);
    char key[16];
    const char *ah = asset_dl_active_map_hash();
    if (ah && asset_dl_map_key(ah, key, sizeof key)) {
        sm_cam_nvs_set(key, gx, gy);
        ESP_LOGW(TAG, "挂起的服务端相机已补上 (%d,%d)，已写入 NVS[key=%s]", (int)gx, (int)gy, key);
    } else {
        ESP_LOGW(TAG, "挂起的服务端相机已补上 (%d,%d)（无活动地图键，未持久化）", (int)gx, (int)gy);
    }
}

static void cam_pan_test_run_ex(int steps, int step, int level);

static void cam_pan_test_run(int steps, int step)
{
    cam_pan_test_run_ex(steps, step, -1);
}

static void cam_pan_test_run_ex(int steps, int step, int level)
{
    /* 真实拖动节奏：每步之间让渲染帧跑一次（约 60ms），否则 20 次 set 会被
     * 批成一次大平移，测出的不是"每步成本"。level 参数用于对比"摄像机流程内
     * （level 1，不画条带）"与"常态（level 0，全层）"。 */
    if (!render_cam_supported()) { ESP_LOGW(TAG, "相机压测：当前图非整图包"); return; }
    int32_t x0 = 0, y0 = 0;
    render_cam_get(&x0, &y0);
    int lvl_save = render_cam_adjust_get();
    if (level >= 0) render_cam_adjust_set(level);
    ESP_LOGW(TAG, "相机压测开始：起点 (%d,%d) 步数 %d 步长 %d 档位 %d→%d",
             (int)x0, (int)y0, steps, step, lvl_save, level >= 0 ? level : lvl_save);
    int64_t t_all = esp_timer_get_time();
    int64_t t_max = 0;
    for (int i = 1; i <= steps; i++) {
        int64_t t0 = esp_timer_get_time();
        render_cam_set(x0 + i * step, y0);
        int64_t dt = esp_timer_get_time() - t0;
        vTaskDelay(pdMS_TO_TICKS(60));        /* 让出一帧：模拟手指移动的真实节奏 */
        if (dt > t_max) t_max = dt;
        ESP_LOGW(TAG, "相机压测 步 %d/%d → x=%d 耗时 %lld ms",
                 i, steps, (int)(x0 + i * step), (long long)(dt / 1000));
    }
    int64_t all = (esp_timer_get_time() - t_all) / 1000;
    ESP_LOGW(TAG, "相机压测结束：%d 步共 %lld ms（均 %lld ms，最大 %lld ms）"
                  "—— 判据：均 ≤30ms 为流畅；个别尖峰=补块；每步都百毫秒级=合成/上屏",
             steps, (long long)all, (long long)(all / (steps ? steps : 1)),
             (long long)(t_max / 1000));
    if (level >= 0) render_cam_adjust_set(lvl_save);   /* 复原档位 */
}

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
        /* E9：待机时钟浮现（背景/锚点均已写死在固件里，见 render_doze_bg.c）。
         * 【2026-10-03 删除待机切图】原先进待机要 SET_MAP(doze_map) 把背景换成那张图；
         * 现在背景是**固件内置的一屏**（compositor 第 4.4 步整块覆盖），切图纯属多余 ——
         * 而且那张图若在本地不可装载（真机：地图包 rc=-8），SET_MAP 失败会走
         * "路径查询失败 → asset_dl_request_sync()"，形成周期性拉取（用户报"没推送却一直
         * 提示拉取中"）。删掉即断根。 */
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

    /* 【无 TF 卡提示 + 默认渲染 2026-09-27】SD 卡不在时固件走内部 Flash 的
     * 出厂素材分区（sd_tf.c 的 fallback）：此时渲染的是出厂默认形象与默认地图，
     * 内容不会随 Web 换装变化。需求（胶水）：没有 TF 卡就渲染默认，
     * 地图默认 000010000，并且**屏上明确提示没有 TF 卡**，别让用户以为坏了。 */
    /* 【开机加载提示 2026-10-01】用户口径"启动的时候渲染卡住"。整图包装载 +
     * 窗口缓存填充在 1-bit SDMMC 上是秒级（真机曾 9.4s），这段时间屏上什么都没有，
     * 看起来就是"卡死"。这里先挂一条常驻横幅，地图/形象落地后立即撤掉
     * （见 dispatch_map 成功分支与 dispatch_manifest_synced 收尾）。 */
    render_banner_show_for("LOADING ASSETS...", 20000);   /* 20s 自动撤，避免踩掉后续常驻横幅 */

    if (sd_tf_is_flash_fallback()) {
        char mid[32];
        const char *want = active_map_get(mid, sizeof mid);
        ESP_LOGW(TAG, "无 TF 卡（内部 Flash 出厂素材模式）→ 渲染出厂默认形象 + 地图 %s", want);
        render_banner_show("NO TF CARD - FACTORY ASSETS");   /* 常驻横幅（配网页横幅同通道） */
        mp_cmd_t mc = { .type = MP_CMD_SET_MAP };
        strlcpy(mc.s, want, sizeof(mc.s));
        mp_post_cmd(&mc);
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
    if (!asset_dl_have_local_manifest()) {
        /* 【空卡降级 2026-09-29，用户定稿："没配置走降级"】TF 在位但为空：
         * 等服务端清单同步最多 12s（清单一到立即继续）；同步已出结果或超时
         * 仍无本地素材 → 切内部 Flash 出厂素材 + 横幅提示，绝不让用户看黑屏。
         * boot 上下文安全：此刻无资产绑定/无打开的 SD 文件，可卸 TF。 */
        /* 等同步【完全结束】再判：have_local 在元数据登记时就翻真（文件可能
         * 还没下），只看它会在下载中途误判“有素材”→ dispatch 全灭 + 降级切
         * 分区失败（卸载撞在途下载）→ FATAL（真机实证 2026-09-29）。上限 60s。 */
        for (int i = 0; i < 240; i++) {
            if (asset_dl_have_local_manifest() && asset_dl_critical_ready()) break;
            if (asset_dl_sync_attempted() && asset_dl_sync_idle()) break;
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (!(asset_dl_have_local_manifest() && asset_dl_critical_ready())) {
            if (sd_tf_switch_to_factory() == 0) {
                /* 双根：TF 保持挂载（下载继续写 /sdcard），渲染读 /factory 快照；
                 * TF 素材齐了由 sync 门切回渲染根热重绑，全程不打断渲染 */
                render_banner_show("EMPTY TF - FACTORY ASSETS");
                mp_cmd_t mc = { .type = MP_CMD_SET_MAP };
                strlcpy(mc.s, MP_DEFAULT_MAP_ID, sizeof(mc.s));
                mp_post_cmd(&mc);
                cmd_simple(MP_CMD_MANIFEST_SYNCED, NULL, 0, 0);
                ESP_LOGW(TAG, "空卡降级：出厂素材已接管（横幅提示 + 默认地图 %s）",
                         MP_DEFAULT_MAP_ID);
            }
            return;   /* 降级失败（无任何本地素材）：维持后续离线/配网流程 */
        }
    }
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

/* 【契约 §3.3】相机调参态入口专用：MENU→POKER 内部通道。
 * 为什么不复用 state_machine_handle(MP_SM_EV_MENU_KEY)：那条事件带 **400ms 硬限速**
 * （防实体键抖动把菜单关了又开，见上），而"菜单里确认②修改当前地图的摄像头"是
 * **显式 UI 动作**，不该被"上一次按键 <400ms"吞掉——被吞的表现是屏上 `CAM BUSY - RETRY`
 * 且调参态进不去。本函数不吃那条限速，也**不刷新**限速时间戳（不占用按键配额，
 * 不改变既有按键手感）。语义 = MENU_KEY 在 MENU 态的分支（transition_locked(POKER)）。
 * 返回 true = 确已迁到 POKER（调用方据此决定是否进入调参态）。 */
bool state_machine_cam_enter_poker(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "相机入口：状态机锁超时，MENU→POKER 未执行");
        return false;
    }
    if (s_state == MP_ST_MENU) transition_locked(MP_ST_POKER);
    bool ok = (s_state == MP_ST_POKER);
    xSemaphoreGive(s_lock);
    if (!ok) {
        ESP_LOGW(TAG, "相机入口：MENU→POKER 未生效（当前态 %s）",
                 state_machine_name(state_machine_current()));
    }
    return ok;
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
static bool heap_ok_for_asset_load(void);

/* 【堆保护 2026-09-29】素材切换要 fopen/fread TF 大包——内部堆见底时
 * newlib 锁分配失败会 abort 重启（真机：菜单确认动作即崩）。
 * 【板级配置 2026-09-30】门值不再写死 24KB：改由 Kconfig
 * MP_ASSET_HEAP_GATE_KB 决定（默认 24 = 216 板既有保护不变）。 */
static bool heap_ok_for_asset_load(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= CONFIG_MP_ASSET_HEAP_GATE_KB * 1024;
}

/* ══ 【绑定窗口逐段内部堆取证 2026-10-02】════════════════════════════════
 * 真机现象（胶水抓串口）：@联网后 空闲=32819 → 一段"地图装载（6 个 mpak
 * opened）+ 字体 + parts + layout"之后 → @@素材全绑后 只剩 1307（掉 ~31KB），
 * 直接把 24KB 门限踩穿 →「跳过本轮素材绑定/字体装载」= 纸娃娃消失。
 * 但"这 31KB 到底是谁吃的"此前只能靠猜（每个包的 open 日志不带水位）。
 * 本探针在每个子阶段后打一行，配合 mpak.c 里 `opened ... 内部堆(此包后)`
 * 与 asset_dl 的 sync 日志，能**逐段算出净增量**，把嫌疑锁死到一个包/一步。
 * 常态只多 5 行启动日志，无需开关。 */
static void bind_heap_probe(const char *stage)
{
    ESP_LOGW(TAG, "· 绑定水位[%s] 内部堆 空闲=%u 最大块=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static esp_timer_handle_t s_bind_retry_timer;   /* 低堆跳过绑定/字体后的自愈重试 */
static bool s_font_pending;                     /* 字体还没装上（跳过或装载失败） */
static void bind_retry_cb(void *arg)
{
    (void)arg;
    mp_cmd_t c = { .type = MP_CMD_MANIFEST_SYNCED };   /* 重跑绑定段（内部堆守卫会再拦） */
    mp_post_cmd(&c);
}

/* 字体未就绪时的兜底重试：复用同一个 10s 周期定时器（幂等，已在跑就不重开）。
 * 触发一次 MANIFEST_SYNCED 重跑绑定段：堆够就装字，不够就再等一轮。 */
/* ------------------------------------------------------------------ */
/* 【活动地图持久化 2026-10-01】用户口径：设置成某张图（如神之子神殿）并调好
 * 相机后，**重启必须还是这张图**，不能重置回默认 000010000。
 * 存 map_id（不是 hash：重导会换 hash）；开机校验它仍在清单里，不在才回默认。 */
#define MP_ACTIVE_MAP_NVS_NS  "uimap"
#define MP_ACTIVE_MAP_NVS_KEY "active"

static void active_map_save(const char *map_id)
{
    if (!map_id || !map_id[0]) return;
    nvs_handle_t h;
    if (nvs_open(MP_ACTIVE_MAP_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char cur[32] = "";
    size_t len = sizeof cur;
    bool same = (nvs_get_str(h, MP_ACTIVE_MAP_NVS_KEY, cur, &len) == ESP_OK &&
                 strcmp(cur, map_id) == 0);
    if (!same) {
        if (nvs_set_str(h, MP_ACTIVE_MAP_NVS_KEY, map_id) == ESP_OK) {
            nvs_commit(h);
            ESP_LOGW(TAG, "活动地图已记入 NVS：%s（重启后仍用这张）", map_id);
        }
    }
    nvs_close(h);
}

/* 取出"上次用的图"：不在清单里（被删/被摘）→ 回默认图，避免开机黑屏 */
static const char *active_map_get(char *buf, size_t cap)
{
    nvs_handle_t h;
    buf[0] = 0;
    if (nvs_open(MP_ACTIVE_MAP_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = cap;
        if (nvs_get_str(h, MP_ACTIVE_MAP_NVS_KEY, buf, &len) != ESP_OK) buf[0] = 0;
        nvs_close(h);
    }
    if (buf[0] && asset_dl_map_exists(buf)) return buf;
    if (buf[0]) ESP_LOGW(TAG, "上次使用的地图 %s 已不在清单 → 回默认 %s", buf, MP_DEFAULT_MAP_ID);
    return MP_DEFAULT_MAP_ID;
}

static void font_retry_arm(void)
{
    if (s_bind_retry_timer) return;
    const esp_timer_create_args_t t = {
        .callback = bind_retry_cb, .name = "bind_retry",
    };
    if (esp_timer_create(&t, &s_bind_retry_timer) == ESP_OK)
        esp_timer_start_periodic(s_bind_retry_timer, 10ULL * 1000000ULL);
}

/* 【压测通道】气泡层已被用户整体去除（CONFIG_MP_BUBBLE_ENABLE=n），
 * 原来的 `::camtest` 气泡魔数随之失效 → 拖动压测改走 action 指令：
 *   {"type":"action","value":"camtest:10,8,1"} = 10 步 × 8 世界 px，档位 1。
 * 正常动作名不含 ':'，不会误触。 */
static void cam_pan_test_run_ex(int steps, int step, int level);

static bool action_maybe_camtest(const char *action)
{
    /* 【抓帧魔数】气泡层已被去除（::shot 失效）→ 帧倾倒改走 action：
     *   {"type":"action","value":"shot"} → UDP 帧倾倒（排障通道） */
    if (action && strcmp(action, "shot") == 0) {
        extern void render_frame_dump_udp(void);
        render_frame_dump_udp();
        return true;
    }
    /* 【实体形象远程切换通道 2026-10-02】action 值 "entity:mob:100100" /
     * "entity:npc:2100000"（"entity:paperdoll" = 切回纸娃娃）→ MP_CMD_SET_ENTITY。
     * 为什么留这条：Web 的 push(kind=mob) 是主通道，但那条要**服务端升级后才存在**；
     * 既有 action 指令通道任何服务端版本都有（/devices/{id}/command {"type":"action"}），
     * 真机验证与排障都靠它。正常动作名不含 ':'，不会误触。 */
    if (action && strncmp(action, "entity:", 7) == 0) {
        mp_cmd_t c = { .type = MP_CMD_SET_ENTITY };
        strlcpy(c.s, action + 7, sizeof(c.s));
        mp_post_cmd(&c);
        ESP_LOGW(TAG, "action 魔数：切换实体形象 → %s", c.s[0] ? c.s : "(纸娃娃)");
        return true;
    }
    /* 【只设档位】"camlevel:N" → 只切调参档位（不动相机），用于抓帧比对 */
    if (action && strncmp(action, "camlevel:", 9) == 0) {
        render_cam_adjust_set(atoi(action + 9));
        return true;
    }
    if (!action || strncmp(action, "camtest:", 8) != 0) return false;
    int st = 12, sp = 8, lv = -1;
    sscanf(action + 8, "%d,%d,%d", &st, &sp, &lv);
    if (st <= 0) st = 12;
    cam_pan_test_run_ex(st, sp, lv);
    return true;
}

/* ══ 实体（怪物 / NPC）形象 2026-10-02 ══════════════════════════════════════
 * 用户口径：「怪物页选中 = 宠物形象变成这只怪物」，216 与 1.85B 同口径。
 * 实现 = 复用纸娃娃那条渲染通道（PARTS + LAYOUT），只是包从 entity 上取：
 *   · 服务端 kind=mob 导出 selector=mob/entity="mob:<id>" 的包（Web 怪物 tab 📤 推送）；
 *   · 设备把该实体当"另一套装扮"绑上（render_set_parts + render_set_layout）；
 *   · 动作名不同（mob: stand/move/fly/hit1，纸娃娃: stand1/walk1/fly/alert/hit）→
 *     下面 entity_action_of() 做映射，查不到就回落到实体默认动作（不黑屏）；
 *   · 选择写 NVS（"entity"），重启后自动恢复；包还没下完则先记意图，
 *     MANIFEST_SYNCED（素材同步完成）时自动补绑。
 * 为什么不是"新渲染分支"：合成器只认 PARTS/LAYOUT 两种包，实体包与纸娃娃包**逐字节
 * 同构**（同一 PartPackWriter/LayoutPackWriter），多一条分支只会多一处漂移风险。 */
#define SM_ENTITY_NVS_KEY "entity"

static char s_entity[40];               /* "" = 纸娃娃；否则 "mob:<id>"/"npc:<id>" */

static bool s_entity_bound;            /* 该实体已成功绑到渲染层（幂等：避免每次 manifest 同步重开包） */
/* 【闪烁治理 2026-10-02】每次 manifest 同步都"重绑 + 整屏重绘"会在屏上闪一下：
 * 大素材下载期每 30s 一轮同步 ⇒ 肉眼可见的周期性闪。这里记住上次成功绑定的
 * 路径 + 是否已重绘过，只有"素材真的换了/第一次绑"才重绘整屏。 */
static char s_bound_parts[MP_MPK_PATH_MAX];
static char s_bound_layout[MP_MPK_PATH_MAX];
static bool s_bound_once;


static int64_t s_entity_retry_last_ms; /* 绑定失败后的自动重试节流（见 dispatch_entity 失败分支） */
static int     s_entity_retry_cnt;

const char *sm_active_entity(void) { return s_entity; }

/* 纸娃娃动作名 → 实体动作名。实体 WZ 动作用的是 stand/move/fly/hit1 这套命名，
 * 直接拿 stand1/walk1 去查实体的 LAYOUT 必然查不到（真机表现会是"切过去就静止"）。 */
static const char *entity_action_of(const char *pd_action)
{
    if (!pd_action || !pd_action[0]) return NULL;
    if (strcmp(pd_action, MP_ACTION_STAND) == 0) return "stand";
    if (strcmp(pd_action, MP_ACTION_WALK)  == 0) return "move";
    if (strcmp(pd_action, MP_ACTION_FLY)   == 0) return "fly";
    if (strcmp(pd_action, MP_ACTION_ALERT) == 0) return "stand";
    if (strcmp(pd_action, MP_ACTION_HIT)   == 0) return "hit1";
    return pd_action;                       /* 服务端直发实体动作名时按原名试 */
}

/* 实体动作是否循环播（与 dispatch_action 的纸娃娃口径一致：站立/走路/飞=循环） */
static bool entity_action_loops(const char *a)
{
    return a && (strcmp(a, "stand") == 0 || strcmp(a, "move") == 0 ||
                 strcmp(a, "fly") == 0 || strcmp(a, "walk") == 0 ||
                 strcmp(a, "stand0") == 0 || strcmp(a, "move0") == 0);
}

/* 实体默认动作 → LAYOUT 路径（清单无 defaultAction 时 asset_dl 回退首个动作）。 */
static bool entity_default_layout_path(const char *entity, char *path, size_t cap, char *act_out,
                                       size_t act_cap)
{
    char act[32];
    if (!asset_dl_entity_default_action(entity, act, sizeof act)) return false;
    if (!asset_dl_entity_layout_path(entity, act, path, cap)) return false;
    if (act_out) strlcpy(act_out, act, act_cap);
    return true;
}

/* 实体切换（MP_CMD_SET_ENTITY / 开机恢复）。entity 空或 "paperdoll*" = 回纸娃娃。 */
static void dispatch_entity(const char *entity)
{
    const char *want = entity ? entity : "";

    /* ── 回纸娃娃 ── */
    if (!want[0] || strncmp(want, "paperdoll", 9) == 0) {
        bool was = (s_entity[0] != 0);
        s_entity[0] = 0;
        s_entity_bound = false;
        mp_nvs_set_str(SM_ENTITY_NVS_KEY, "");
        if (!heap_ok_for_asset_load()) {
            ESP_LOGW(TAG, "内部堆不足，跳过回纸娃娃绑定（防 fopen abort）");
            return;
        }
        char pp[MP_MPK_PATH_MAX], ll[MP_MPK_PATH_MAX];
        int prc = asset_dl_parts_path(NULL, pp, sizeof(pp)) ? render_set_parts(pp) : -1;
        int lrc = asset_dl_layout_path(MP_ACTION_STAND, ll, sizeof(ll))
                      ? render_set_layout(ll, true) : -1;
        ESP_LOGW(TAG, "形象切回纸娃娃（was_entity=%d）parts rc=%d layout rc=%d", (int)was, prc, lrc);
        return;
    }

    if (!heap_ok_for_asset_load()) {
        ESP_LOGW(TAG, "内部堆不足，跳过形象切换 %s（防 fopen abort）", want);
        return;
    }
    /* 意图先落地（NVS + 内存）：包可能还在下载 —— 记住它，素材同步完成后自动补绑 */
    bool changed = (strcmp(s_entity, want) != 0);
    if (changed) { s_entity_retry_cnt = 0; s_entity_retry_last_ms = 0; }   /* 新目标：重试预算重置 */
    strlcpy(s_entity, want, sizeof s_entity);
    mp_nvs_set_str(SM_ENTITY_NVS_KEY, s_entity);

    /* 全有才绑：PARTS 与**默认动作 LAYOUT** 必须都在本地 —— 只绑一半会让渲染层出现
     * "怪物 PARTS + 纸娃娃 LAYOUT"的错配帧（合成器会按"部件解析失败"自愈并刷日志，
     * 用户看到的是形象错乱）。缺任一包 → 只记住意图，等 MANIFEST_SYNCED 重试。
     * ⚠️ **两条路径必须各用独立缓冲**（仓库老坑，真机刚又踩一次 2026-10-02）：
     *   共用 path 时 entity_default_layout_path() 会把 LAYOUT 路径盖掉 PARTS 路径，
     *   随后 render_set_parts() 打开的是 LAYOUT 包 → mpak_open 返回 MPAK_ERR_KIND(-7)
     *   （真机日志：`形象切换 → npc:2100000（parts rc=-7）`）。
     *   同款警告见 dispatch_manifest_synced 的出厂降级绑定段。 */
    char ppath[MP_MPK_PATH_MAX], lpath[MP_MPK_PATH_MAX], act[32] = "";
    if (!asset_dl_entity_parts_path(s_entity, ppath, sizeof ppath) ||
        !entity_default_layout_path(s_entity, lpath, sizeof lpath, act, sizeof act)) {
        ESP_LOGW(TAG, "实体 %s 的 PARTS/默认动作布局还没下全 → 先记住，素材同步后自动切",
                 s_entity);
        return;
    }
    int prc = render_set_parts(ppath);
    if (prc != 0) {
        vTaskDelay(pdMS_TO_TICKS(50));           /* TF 争用重试（同 parts 首开口径） */
        prc = render_set_parts(ppath);
    }
    int lrc = -1;
    if (prc == 0) {
        if (!asset_dl_entity_layout_path(s_entity, act, lpath, sizeof lpath)) {
            ESP_LOGW(TAG, "实体 %s 默认动作 %s 的布局路径消失（被淘汰？）", s_entity, act);
        } else {
            lrc = render_set_layout(lpath, entity_action_loops(act));
            if (lrc != 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                lrc = render_set_layout(lpath, entity_action_loops(act));
            }
        }
    }
    ESP_LOGW(TAG, "实体绑定路径：parts=%s | layout=%s(%s)", ppath, lpath, act[0] ? act : "-");
    s_entity_bound = (prc == 0 && lrc == 0);
    if (s_entity_bound) { s_entity_retry_cnt = 0; s_entity_retry_last_ms = 0; }
    ESP_LOGW(TAG, "形象切换%s → %s（parts rc=%d）默认动作 %s（layout rc=%d）"
                  "｜内部堆 空闲=%u 最大块=%u",
             changed ? "" : "（同实体重绑）", s_entity, prc,
             act[0] ? act : "(无)", lrc,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (!s_entity_bound) {
        char last_want[40];
        strlcpy(last_want, s_entity, sizeof last_want);
        /* 【失败必须回滚"活动实体" 2026-10-02 真机实证】原实现失败后仍留 s_entity：
         * 紧接着 dispatch_action("stand1") 会走实体分支，把**没绑上的实体的 LAYOUT**
         * 绑到渲染层 ⇒ 屏上是"纸娃娃部件 + 实体布局"的混合态（部件 id 全对不上，
         * 合成器还会判定错配反复请求同步）。真机日志形态：
         *   sm: 形象切换 → npc:2100000（parts rc=-7）默认动作 stand（layout rc=-1）
         *   mpak: opened …/layout/dd0839cdc3e56ad2.mpk（= 实体布局被绑上了）
         * 现改为：活动实体退回纸娃娃（**NVS 里的意图保留**）→ 动作一律回纸娃娃通道路径；
         * 同时请求一次素材同步，MANIFEST_SYNCED 时 entity_restore() 会按 NVS 意图重试。 */
        ESP_LOGW(TAG, "实体 %s 绑定失败 → 活动实体回退纸娃娃（NVS 意图保留，待素材同步重试）",
                 last_want);
        s_entity[0] = 0;
        /* 【回滚必须把渲染层也复位 2026-10-02 用户报"人物渲染可能出问题"核查补强】
         * 上面只清了"活动实体"这个**状态**：如果失败发生在
         * 「PARTS 已绑上、默认动作 LAYOUT 打开失败」这个窗口，渲染层此刻是
         * **实体 PARTS + 纸娃娃 LAYOUT** 的混合态 —— 两边 part_id 都从 1 开始但含义
         * 完全不同 ⇒ 整帧部件解析失败（用户看到的就是"人物渲染出问题/缺件"）。
         * 现在失败即**主动重绑纸娃娃**（parts + stand1），把渲染层也拉回一致状态。 */
        if (prc == 0 || lrc != 0) {
            char pp[MP_MPK_PATH_MAX], ll[MP_MPK_PATH_MAX];
            int prc2 = asset_dl_parts_path(NULL, pp, sizeof pp) ? render_set_parts(pp) : -1;
            int lrc2 = asset_dl_layout_path(MP_ACTION_STAND, ll, sizeof ll)
                           ? render_set_layout(ll, true) : -1;
            ESP_LOGW(TAG, "实体绑定失败回滚：重绑纸娃娃 parts rc=%d layout rc=%d（parts=%s）",
                     prc2, lrc2, pp);
        }
        /* 重试节流：绑定失败会请求素材同步，而同步收尾又会调 entity_restore() 重试 ——
         * 不节流就是"失败→同步→失败"的热循环（每轮一次 HTTP + 一串日志）。
         * 同一实体最多自动重试 3 次、间隔 ≥30s；用尽即停（显式切换/重启会重置）。 */
        extern void asset_dl_request_sync(void);
        int64_t now_ms = mp_now_ms();
        /* 【大包 = 异步等待，不烧重试预算 2026-10-02 用户口径"推送的时候最好异步下载"】
         * 包还没下完（自动缩放后仍可达 8MB，TF ~230KB/s ⇒ 分钟级）时，绑定失败是
         * **正常中间态**：只做 ≥30s 节流的同步请求（催下载），**不动** retry_cnt，
         * 下完那次 MANIFEST_SYNCED 会自然重绑。只有"文件已在 TF 却仍打不开"才算真失败。 */
        if (!asset_dl_entity_ready(last_want)) {
            /* 【不再催同步 2026-10-02】下载本来就在 asset 任务里飞着，下完它自己会
             * post MANIFEST_SYNCED → entity_restore 重绑；这里再 request_sync 只会
             * 每 30s 触发一轮同步 → 每轮一次整屏重绘 ⇒ 屏上周期闪（用户报"闪烁"）。 */
            if (s_entity_retry_last_ms == 0 || now_ms - s_entity_retry_last_ms >= 30000) {
                s_entity_retry_last_ms = now_ms;
                ESP_LOGW(TAG, "实体 %s 的包还没下完（大包分钟级）→ 等异步下载完成自动绑定",
                         last_want);
            }
            return;
        }
        if (s_entity_retry_cnt < 3 &&
            (s_entity_retry_last_ms == 0 || now_ms - s_entity_retry_last_ms >= 30000)) {
            s_entity_retry_last_ms = now_ms;
            s_entity_retry_cnt++;
            ESP_LOGW(TAG, "实体绑定失败 → 请求素材同步重试（第 %d/3 次）", s_entity_retry_cnt);
            asset_dl_request_sync();
        } else if (s_entity_retry_cnt >= 3) {
            ESP_LOGE(TAG, "实体 %s 连续 %d 次绑定失败 → 停止自动重试（等显式切换或重启）",
                     last_want, s_entity_retry_cnt);
        }
    }
}

static void dispatch_action(const char *action)
{
    char path[MP_MPK_PATH_MAX];
    if (!heap_ok_for_asset_load()) {
        ESP_LOGW(TAG, "内部堆不足，跳过动作切换 %s（防 fopen abort）", action);
        return;
    }
    /* ── 实体态：动作从该实体自己的 LAYOUT 里取（先映射名字，再回落默认动作）──
     * 判据用 s_entity_bound（真正绑上了）而不是 s_entity：绑定失败时绝不能把实体
     * 布局配到纸娃娃部件上（部件 id 全对不上 = 屏上形象消失）。 */
    if (s_entity[0] && s_entity_bound) {
        const char *want = entity_action_of(action);
        char use[32] = "";
        bool ok = false;
        if (want && want[0]) {
            ok = asset_dl_entity_layout_path(s_entity, want, path, sizeof(path));
            if (ok) strlcpy(use, want, sizeof use);
        }
        if (!ok) {
            /* 该实体没有这个动作（多数怪物没有 alert/hit）→ 回落默认动作，
             * 绝不去查纸娃娃的布局（那会让怪物突然变成纸娃娃的姿势/部件错配）。 */
            ok = entity_default_layout_path(s_entity, path, sizeof(path), use, sizeof use);
        }
        if (!ok) {
            ESP_LOGW(TAG, "实体 %s 无可用布局（动作 %s）→ 保留当前画面", s_entity, action);
            return;
        }
        render_set_layout(path, entity_action_loops(use));
        return;
    }
    if (!asset_dl_layout_path(action, path, sizeof(path))) {
        return;                          /* 布局缺（未下发/被淘汰）：保留旧画面 */
    }
    bool loop = (strcmp(action, MP_ACTION_STAND) == 0 ||
                 strcmp(action, MP_ACTION_WALK) == 0 ||
                 strcmp(action, MP_ACTION_FLY) == 0);
    render_set_layout(path, loop);
}

/* 开机恢复上次选的实体（NVS）。清单里已无该实体（被服务端摘掉/换设备）→ 清掉意图，
 * 保持纸娃娃（不黑屏）。返回是否已接管形象（true = 调用方不要再绑纸娃娃）。 */
static bool entity_restore(void)
{
    char ent[40] = "";
    if (!mp_nvs_get_str(SM_ENTITY_NVS_KEY, ent, sizeof ent) || !ent[0]) return false;
    if (!asset_dl_entity_exists(ent)) {
        /* 【开机清单还是旧的，别急着清意图 2026-10-02 真机踩到】开机时本地清单是
         * 上一次同步的产物（网络同步往往还没收尾）——此时新推送的怪物/NPC 当然
         * "不存在"。旧实现在这里直接清 NVS：真机现象 = 指向新服务器后首次开机，
         * mob 意图被清掉，等同步真把包拉下来时已经没人记得要切它了。
         * 现口径：清单里还没有 → **保留意图**，等 MANIFEST_SYNCED（下载完成后一定会
         * 再来一次）重试；只有"重试预算已用尽且仍不在清单"才认定服务端已摘除并清除。 */
        if (s_entity_retry_cnt < 3) {
            ESP_LOGW(TAG, "实体 %s 还不在本地清单（同步未收尾？）→ 保留意图待重试", ent);
            return false;
        }
        ESP_LOGW(TAG, "NVS 记住的实体 %s 不在当前清单（重试预算已用尽）→ 清除，回纸娃娃", ent);
        mp_nvs_set_str(SM_ENTITY_NVS_KEY, "");
        return false;
    }
    /* 已接管且实体没变 → 不重复开包（manifest 每次同步都会走这里） */
    if (s_entity_bound && strcmp(s_entity, ent) == 0) return true;
    dispatch_entity(ent);
    return s_entity_bound;
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
    if (!heap_ok_for_asset_load()) {
        ESP_LOGW(TAG, "内部堆不足，跳过换装 %s（防 fopen abort）", hash ? hash : "");
        return;
    }
    char path[MP_MPK_PATH_MAX];
    if (!hash || !asset_dl_parts_path(hash, path, sizeof(path))) {
        ESP_LOGW(TAG, "SET_PARTS：hash %s 无对应部件包", hash ? hash : "(null)");
        return;
    }
    int rc = render_set_parts(path);
    /* 【与实体形象的互斥 2026-10-02】服务端下发换装 = 用户要的是**纸娃娃这套衣服**：
     * 若当前正显示怪物/NPC，必须先退出实体态，并立刻把布局重绑回 stand1 ——
     * 否则实体的 LAYOUT（part_id 属于怪物包）配新纸娃娃 PARTS ⇒ 整帧部件解析失败
     * ⇒ 屏上"人物消失"（合成器会误判错配并反复请求同步）。 */
    if (s_entity[0]) {
        ESP_LOGW(TAG, "收到换装（纸娃娃）→ 退出实体形象 %s，布局重绑 stand1", s_entity);
        s_entity[0] = 0;
        s_entity_bound = false;
        mp_nvs_set_str(SM_ENTITY_NVS_KEY, "");
        char lp[MP_MPK_PATH_MAX];
        if (asset_dl_layout_path(MP_ACTION_STAND, lp, sizeof(lp)))
            render_set_layout(lp, true);
    }
    ESP_LOGI(TAG, "换装 %s rc=%d", path, rc);
}

/* 是否已成功装载过 BGMAP（默认地图重投判据） */
static bool g_map_loaded;

/* ══ 【相机 UX · 契约 §3.3】per-map 相机持久化（namespace "cam"）══════════════
 * docs/ai/map-fullmap-firmware-contract.md §3.3：
 *   · key = per-map 键，与 asset_dl 的隐藏标识 / asset_dl_map_key() **同口径**
 *     （map_id 优先，无 map_id → "h"+hash 前 14 位；恒 ≤15 字符 = NVS 键长上限）；
 *   · value = 两个 int32（x,y）= 可见窗口左上角的**世界坐标**（1x 世界系，见 §1）；
 *   · 「全局加载」= 装载地图成功路径读 NVS 应用（本文件 dispatch_map 末尾
 *     cam_apply_for_map），不是"每次进相机页才读"；
 *   · 清除点 = 菜单子页③「删除此地图」（隐藏标识置位处，lvgl_bridge.c 调
 *     sm_cam_nvs_erase）。
 * 注：菜单「Reset WiFi」不是出厂复位——provision_factory_reset() 只清配网键
 * （provision.c），故不在此顺手清相机（避免"重置 WiFi 顺手丢用户调好的相机"）；
 * 真·出厂（NVS 区整片擦除）自然连 cam 命名空间一起清。 */
#define SM_CAM_NVS_NS  "cam"
#define SM_CAM_KEY_MAX 15          /* NVS_KEY_NAME_MAX_SIZE-1（键名硬上限） */

/* 【契约 §3.3】"脚踩地面线"结算：实现在 lvgl_bridge.c（相机 UX 层，掌握屏尺寸与
 * 相机调参态的 pending 旗标）。本文件只负责在**地图装载成功之后**叫它一次——这是
 * 站位链（render_set_map → ent_stand_on_ground_locked）与相机应用都已完成的最早时刻。 */
extern void bridge_cam_settle_after_reload(void);

/* 读该图相机：(x,y) 世界 px；无该键/命名空间不存在/长度不符 → false */
bool sm_cam_nvs_get(const char *key, int32_t *x, int32_t *y)
{
    if (!key || !key[0] || !x || !y) return false;
    nvs_handle_t h;
    if (nvs_open(SM_CAM_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    int32_t v[2] = { 0, 0 };
    size_t len = sizeof(v);
    esp_err_t e = nvs_get_blob(h, key, v, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(v)) return false;
    *x = v[0];
    *y = v[1];
    return true;
}

/* 写该图相机（8B blob：x,y 各 int32）→ true=已 commit 落地 */
bool sm_cam_nvs_set(const char *key, int32_t x, int32_t y)
{
    if (!key || !key[0] || strlen(key) > SM_CAM_KEY_MAX) {
        ESP_LOGE(TAG, "相机 NVS 写入拒绝：键非法（%s）", key ? key : "(null)");
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(SM_CAM_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "相机 NVS：打开命名空间 %s 失败", SM_CAM_NVS_NS);
        return false;
    }
    const int32_t v[2] = { x, y };
    esp_err_t e = nvs_set_blob(h, key, v, sizeof(v));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "相机 NVS 写入失败 key=%s：%s", key, esp_err_to_name(e));
        return false;
    }
    return true;
}

/* 清该图相机键（隐藏/删除地图时；键不存在视为成功）；true = 已确认干净 */
bool sm_cam_nvs_erase(const char *key)
{
    if (!key || !key[0]) return false;
    nvs_handle_t h;
    if (nvs_open(SM_CAM_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return true;  /* 无命名空间=无键 */
    esp_err_t e = nvs_erase_key(h, key);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;      /* 解除不存在的键 = 成功 */
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return (e == ESP_OK);
}

/* 装载地图成功后应用该图相机 = 契约 §3.3 的「全局加载」：
 *   非整图包（旧窗口包）→ render_cam_supported()==false → 整段跳过（旧包行为逐字节不变）；
 *   整图包有 NVS → render_cam_set(保存值)；
 *   整图包无 NVS → 不额外动作（render_set_map 装载时已置中 = 服务端导出参考相机）。 */
static void cam_apply_for_map(const char *hash)
{
    if (!render_cam_supported()) {
        ESP_LOGI(TAG, "相机：地图 %s 非整图包（无可平移余量）→ 跳过 NVS 相机", hash);
        return;
    }
    char key[16];
    if (!asset_dl_map_key(hash, key, sizeof(key))) {
        ESP_LOGW(TAG, "相机：地图 %s 无 per-map 键 → 跳过 NVS 相机", hash);
        return;
    }
    int32_t x = 0, y = 0;
    if (sm_cam_nvs_get(key, &x, &y)) {
        render_cam_set(x, y);          /* 越界由实现夹取（§3.2） */
        ESP_LOGW(TAG, "sm: 地图 %s 应用相机 (%d,%d) [ns=%s key=%s]",
                 hash, (int)x, (int)y, SM_CAM_NVS_NS, key);
    } else {
        /* 无 NVS：渲染层 render_set_map 装载整图包时已置中（compositor.h 明示），
         * 无需再调 render_cam_center()；这里只留可判读的日志。 */
        ESP_LOGI(TAG, "sm: 地图 %s 无 NVS 相机 → 保持装载置中（服务端导出参考相机）", hash);
    }
}

/* 横幅收尾（相机调参退出后由 lvgl_bridge 调用）：恢复「无 WiFi 配置」常驻横幅，
 * 已配网则隐藏——复用 POKER on_enter 的同一判据，绝不留下调参横幅 */
void state_machine_banner_restore(void)
{
    post_banner_if_needed();
}

static void dispatch_map(const char *hash)
{
    char bg[MP_MPK_PATH_MAX];
    /* BGMAP 条带引用（§五）；16=旧导出器分段可到 9+（真机 200000000=9，8 截断致 rc=-100）。
     * 【内部 RAM 腾挪 2026-10-02】16×96B=1536B 原为内部 .bss 常驻。它只是
     * 一摞**路径字符串**（喂给 mpak_open/asset_dl 查询），无 DMA 直接读 →
     * PSRAM 懒分配；分配失败则本图不装条带（退化成只有 static_back，
     * 与既有"条带 0 条"路径同语义，不崩）。 */
    static char (*strips)[MP_MPK_PATH_MAX];
    if (!strips) strips = mp_psram_malloc((size_t)16 * MP_MPK_PATH_MAX);
    /* 两边都拿不到时 n 归零走"0 条带"路径：active_map/LRU/NVS/相机等后续
     * 流程一字不动，只是不装条带（与既有"该图没有条带"完全同语义）。 */
    if (!strips) ESP_LOGW(TAG, "条带路径表分配失败 → 本条按 0 条带装载");
    /* 【指针数组修复 2026-09-27】render_set_map 的形参是 const char **，
     * 此前直接 `(const char **)strips` 强转二维数组 —— 布局是"每行 96B 连续"，
     * 按 char* 解释会把行首 8 个字节当成指针 → 传进去的是野指针，
     * 真机表现：`mpak: open  failed` / `strip 0 load failed ()`（路径恒空）
     * → 默认地图的视差条带永远加载不了（地图静默退化成只有 static_back）。
     * 正确做法：显式建指针数组。 */
    const char *strip_ptrs[16];

    asset_dl_set_active_map(hash);
    asset_dl_touch(hash);                     /* E7：切过的秒切（LRU 前排） */

    /* 【默认地图必须落地 2026-09-27】无 TF 卡时启动早期（1.3s）就 post 了
     * SET_MAP 000010000，但那时清单还没解析（MANIFEST_SYNCED 在 ~5s 才到）
     * → asset_dl_map_path 查不到 BGMAP → 此前**静默 return**，用户看到的
     * 是纯黑底（需求："没有 TF 卡就渲染默认，地图默认渲染 000010000"）。
     * 现在：查不到就记 ERROR 并请求一次清单/素材同步，等
     * dispatch_manifest_synced 末尾重投默认地图（见 mp_post_cmd(MP_CMD_SET_MAP)）。 */
    if (!asset_dl_map_path(hash, bg, sizeof(bg))) {
        ESP_LOGE(TAG, "地图 %s 路径查询失败（清单未就绪/无 BGMAP 条目）→ 待清单同步后重投", hash);
        asset_dl_request_sync();
        return;
    }
    int n = strips ? asset_dl_map_strips(bg, strips, 16) : 0;
    if (n < 0) n = 0;
    /* 【条带数上限修正 2026-10-01】原写死 8：整图新导出里 神秘岛 14 条、明珠港 12 条、
     * 时空裂缝 1 条……被截到 8 → 渲染层判"条带不全"→ **缺段不绘制**（背景少层、
     * 看着就是"背景不对"）。缓冲区本就是 16（strips[16]/strip_ptrs[16]），
     * 渲染层 g_strips 也是按 strip_count 动态分配 ⇒ 上限放开到 16 即可。
     * 判据日志：`地图装载 <id>（条带 N）rc=0` 的 N 应等于 BGMAP 声明条带数。 */
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) strip_ptrs[i] = strips[i];
    /* 【装载期直落相机】把该图的 NVS 相机在装载**之前**交给渲染层，让它第一次
     * 填窗口缓存就落在正确位置（否则置中填一遍、应用相机再整窗重填一遍，
     * 真机实测开机 10.7s + 14.2s）。取不到记忆就不调（渲染层按置中）。 */
    {
        char ckey[16];
        int32_t cx = 0, cy = 0;
        if (asset_dl_map_key(hash, ckey, sizeof(ckey)) && sm_cam_nvs_get(ckey, &cx, &cy)) {
            render_cam_set_pending(cx, cy);
            ESP_LOGI(TAG, "地图 %s：装载期直落 NVS 相机 (%d,%d)", hash, (int)cx, (int)cy);
        } else {
            render_cam_clear_pending();
        }
    }
    int mrc = render_set_map(bg, (n > 0) ? strip_ptrs : NULL, n);
    if (n > 0)
        ESP_LOGI(TAG, "地图条带 %d 条：%s | %s", n, strips[0], (n > 1) ? strips[1] : "-");
    ESP_LOGW(TAG, "地图装载 %s（条带 %d）rc=%d", hash, n, mrc);
    if (mrc == 0) {
        g_map_loaded = true;
        cam_pending_apply();          /* 服务端相机若在地图装载前到达，这里补上 */
        /* 装载成功即记忆（含服务端推送/菜单选择/开机重投三条路径）：下次开机仍用它 */
        char mid[32];
        if (asset_dl_map_id_of(hash, mid, sizeof mid)) active_map_save(mid);
    }

    /* 【契约 §3.3 全局加载】装载成功 → 应用该图 NVS 相机（非整图包自动跳过）。
     * 放在 render_set_map 之后：render_cam_* 的"当前图"口径以刚装入的包为准。 */
    if (mrc == 0) {
        cam_apply_for_map(hash);
        /* 【§3.3「脚踩地面线」结算点】必须在**装载落地之后**：此刻 render_set_map 已
         * 跑完站位链（宠物归屏心）、相机也已应用 → render_ground_screen_y(屏心x) 才是
         * 收尾后的真值（在派发前采样会早一帧，取到的是旧相机下的地面线）。
         * 非相机收尾触发的地图装载里这是空操作（lvgl_bridge 侧 pending 未置位）。 */
        bridge_cam_settle_after_reload();
    }

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
    /* 【时钟锚点写死 2026-10-03 用户口径"地图资源直接写死在硬件里"】待机背景是固件内置
     * 那一屏（render_doze_bg.c），锚点必须配套 ⇒ 直接用内置锚点（不再查 clock_table，
     * 服务端怎么配都不会让时钟跑偏）。 */
    extern const int16_t g_doze_clock_anchor_x;
    extern const int16_t g_doze_clock_anchor_y;
    int16_t ax = g_doze_clock_anchor_x, ay = g_doze_clock_anchor_y;
    bool has = true;
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

    /* 【绑定挂起 2026-09-30】本函数全程要 fopen TF 包（字体×3 / parts /
     * layout / 降级重绑），绑定期挂起显示刷新（屏面静止在最后合成帧，
     * 任务与 stage 均保留）。
     * 【挂起泄漏红线】本函数共 5 条退出路径——①后台补齐 return ②sync
     * 在飞 return ③④两处 FATAL return ⑤函数末尾正常走完——每条都先
     * display_refresh_resume() 再退出，漏一条 = 屏幕永久冻结。
     * 216 板（GRAM）该 API 为空操作，本段零行为变化。 */
    display_refresh_suspend();
    bind_heap_probe("绑定段入口");
    /* 本轮是否真的换了素材（决定结尾要不要整屏重绘；见 s_bound_* 注释） */
    bool bind_changed = false;
    /* 【换装期隐藏 2026-10-02 用户口径】parts 与 layout 分两次换的窗口里先不出人：
     * 否则中间那一帧是"新 PARTS + 旧 LAYOUT"（part_id 同名不同义）= 用户看到的错乱。
     * 窗口结束（或失败回滚）后 render_rebind_end() 会整屏重绘把人放回来。 */
    render_rebind_begin();

    /* 字体三档（气泡 24 / 列表 16 / 标题 32，E12） */
    static const struct { render_font_t id; int px; } fonts[] = {
        { RENDER_FONT_16, 16 }, { RENDER_FONT_24, 24 }, { RENDER_FONT_32, 32 },
    };
    /* 【字体堆门控 2026-10-01 · 真机根因：整图包下载与字体装载抢内部堆】
     * 证据（216 板内部 DRAM ~133KB，串口逐点打点）：
     *   · 首启 sync 触发 17MB BGMAP 分块下载，下载在飞时并发跑本函数：
     *     `set_font px=16` 期间内部堆 24,419 → 6,095（mpak_open rc=-1），
     *     px=24 之后只剩 **299B / 最大块 4B**；
     *   · 接着 SDMMC 连 512B DMA 缓冲都拿不到 → `sdmmc_read_sectors: not enough mem`
     *     → `mpak: open /sdcard/minipet/bg/<整图包>.mpk failed` → 地图装载 rc=-1（黑屏）
     *     → 素材绑定被 heap_ok_for_asset_load() 跳过 → 10s 一轮死循环；
     *   · 对照：素材已下完的那次启动，同样三个字体装载后内部堆反而回到 38KB。
     * 结论：字体装载本身不泄漏，是**与下载并发时内部堆不够**（老代码只护
     * parts/layout 绑定，字体这一段是裸奔的）。
     * 修法：字体装载吃同一道门——堆不足就整轮跳过（保留上一轮已装字体，屏上
     * 不会缺字），10s 后随 sync 重试；下载完、堆回稳后自然装上。 */
    bool heap_ok_font = heap_ok_for_asset_load();
    if (!heap_ok_font) {
        /* 【2026-10-01 气泡/菜单全是占位框的真因】跳过后**不是**"随下轮 sync 重试"
         * 那么轻——实测开机整图装载期内部堆 22.8KB < 24KB 门限，这一跳之后
         * font_lazy 的实例根本没 open：fl_glyph_dsc 直接 return false →
         * LVGL 对每个字都画 placeholder ⇒ 用户看到"气泡是空的没字"、菜单变方框。
         * 而下一轮 sync 可能要等几十秒甚至几分钟（还有可能再次被门限拦）。
         * 修法：① 一旦发现"字体未就绪"，用既有的 10s 重试定时器兜底（与 parts
         * 绑定同一个 timer，见下）；② 让"是否已就绪"可查（render_font_ready()），
         * 只有真的还缺才重试，堆回稳后 10s 内自动补上，不必等 sync。 */
        ESP_LOGW(TAG, "内部堆不足（%uB < %dKB）→ 跳过本轮字体装载（屏上暂为占位框），10s 重试",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (int)CONFIG_MP_ASSET_HEAP_GATE_KB);
        s_font_pending = true;
        font_retry_arm();                 /* 见下：与 bind 重试共用一个周期定时器 */
    }
    if (heap_ok_font) {
        bool all_ok = true;
        for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
            if (asset_dl_font_path(fonts[i].px, path, sizeof(path))) {
                ESP_LOGW(TAG, "set_font px=%d 开始 %s", fonts[i].px, path);
                if (render_set_font(fonts[i].id, path) != 0) all_ok = false;
                ESP_LOGW(TAG, "set_font px=%d 完成", fonts[i].px);
            } else {
                all_ok = false;                 /* 清单里没有这档字体 */
            }
        }
        if (all_ok) s_font_pending = false;
        else        { s_font_pending = true; font_retry_arm(); }
    }
    bind_heap_probe("字体三档之后");      /* 门拦截时也要打（否则看不到"未装"的对照） */

    /* 默认纸娃娃部件 + 站立布局（E13：每设备独立装扮） */
    if (!heap_ok_for_asset_load()) {
        ESP_LOGW(TAG, "内部堆不足，跳过本轮素材绑定（防低堆 fopen abort）→ 10s 后重试");
        if (!s_bind_retry_timer) {
            const esp_timer_create_args_t t = {
                .callback = bind_retry_cb, .name = "bind_retry",
            };
            if (esp_timer_create(&t, &s_bind_retry_timer) == ESP_OK)
                esp_timer_start_periodic(s_bind_retry_timer, 10ULL * 1000000ULL);
        }
    } else if (asset_dl_parts_path(NULL, path, sizeof(path))) {
        int prc = 0;
        if (s_bound_once && strcmp(path, s_bound_parts) == 0) {
            ESP_LOGI(TAG, "parts 未变（%s）→ 跳过重复绑定（防闪）", path);
        } else {
            prc = render_set_parts(path);
            if (prc != 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                prc = render_set_parts(path);
                ESP_LOGW(TAG, "parts 首开失败（TF 争用?）重试 rc=%d", prc);
            }
            if (prc == 0) { strlcpy(s_bound_parts, path, sizeof(s_bound_parts)); bind_changed = true; }
        }
        ESP_LOGW(TAG, "parts 路径=%s rc=%d", path, prc);
        if (s_bind_retry_timer && !s_font_pending) {   /* 绑定+字体都好了：停自愈重试 */
            esp_timer_stop(s_bind_retry_timer);
            esp_timer_delete(s_bind_retry_timer);
            s_bind_retry_timer = NULL;
        }
    } else {
        ESP_LOGE(TAG, "parts 路径查询失败（清单里没有 PARTS）");
    }
    bind_heap_probe("parts 绑定之后");
    /* 加载失败不黑屏：屏显文字提示（E11 素材故障 → dam 语义的文本版） */
    /* 【TF 并发争用重试 2026-09-30】dispatch（渲染任务）与 sync_once（asset_dl
     * 任务）并发读 TF：SDMMC 1-bit 下偶发 open 失败（真机：同文件 60s 前
     * rc=0、dispatch 时 failed→误降级白屏）。读失败等 50ms 重试一次再判。 */
    bool l_ok = asset_dl_layout_path("stand1", path, sizeof(path));
    int lrc = -1;
    if (l_ok) {
        if (s_bound_once && strcmp(path, s_bound_layout) == 0) {
            lrc = 0;
            ESP_LOGI(TAG, "layout 未变（%s）→ 跳过重复绑定（防闪）", path);
        } else {
            lrc = render_set_layout(path, true);
            if (lrc != 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                lrc = render_set_layout(path, true);
                ESP_LOGW(TAG, "layout 首开失败（TF 争用?）重试 rc=%d", lrc);
            }
            if (lrc == 0) { strlcpy(s_bound_layout, path, sizeof(s_bound_layout)); bind_changed = true; }
        }
        ESP_LOGW(TAG, "layout 路径=%s rc=%d", path, lrc);
    }
    /* 换装窗口收尾：layout 失败 = 半新半旧，先把 parts 回滚到上次成功的一对再放人出来 */
    if (!l_ok || lrc != 0) {
        if (s_bound_once && s_bound_parts[0]) {
            int rrc = render_set_parts(s_bound_parts);
            ESP_LOGW(TAG, "换装失败回滚：parts 回到上次成功的一对（%s）rc=%d", s_bound_parts, rrc);
        }
    }
    render_rebind_end();
    bind_heap_probe("layout 绑定之后");
    if (!l_ok || lrc != 0 ||
        !asset_dl_parts_path(NULL, path, sizeof(path))) {
        /* 【拉取失败降级 2026-09-29，用户定稿】TF 在位但素材没下全（链路/堆
         * 紧张）→ 不进 FATAL，切内部 Flash 出厂分区（神子快照）重试一次；
         * 二次仍失败才 FATAL（真机：出厂分区空壳时代卡过这一步） */
        ESP_LOGE(TAG, "本地素材加载失败（parts/stand1 缺失）");
        /* 【抢跑修复 2026-09-30】TF 在位 + sync 正在下载（或 GET 飞行中）时
         * 【绝不切出厂分区】——卸载 TF 会杀死在途下载 socket，形成"降级
         * 杀救兵"死循环（真机：文件被 FAT 清空后永远等不到重下）。改为：
         * 等 sync 完整收尾并复查关键素材；只有 sync 已结束且仍缺（服务端
         * 也救不了/超时 60s）才降级出厂保命。 */
        {
            /* 【等待下载推进 2026-09-30】上限 300s；sync 忙（下载中）就一直等
             * （真机：950KB 大包多轮块化续传需数分钟）；仅当 sync 已收尾且
             * 连续 3 轮（10s 自愈周期×3）素材仍缺才认命降级。全程喂狗。 */
            for (int i = 0; i < 1200; i++) {
                watchdog_kick();
                if (asset_dl_critical_ready()) break;
                if (i > 40 && asset_dl_sync_idle()) {
                    static int s_degrade_grace;
                    if (++s_degrade_grace >= 6) break;
                    vTaskDelay(pdMS_TO_TICKS(10000));
                    continue;
                }
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            if (asset_dl_critical_ready()) {
                /* 素材已由后台补齐：重走绑定（不降级不切分区） */
                char pp[MP_MPK_PATH_MAX], ll[MP_MPK_PATH_MAX];
                int prc = -1, lrc2 = -1;
                if (asset_dl_parts_path(NULL, pp, sizeof(pp))) prc = render_set_parts(pp);
                if (asset_dl_layout_path("stand1", ll, sizeof(ll))) lrc2 = render_set_layout(ll, true);
                ESP_LOGW(TAG, "后台下载补齐：已绑定 TF 素材（不降级）parts rc=%d layout rc=%d",
                         prc, lrc2);
                display_refresh_resume();   /* 退出路径①：恢复刷新（防挂起泄漏） */
                return;
            }
        }
        if (!asset_dl_sync_idle()) {
            /* sync 仍在飞（下载未收尾）：切分区必撕 FATFS 锁（真机 assert
             * _lock_close）。此时宁可不降级：渲染已绑出厂或空，等下轮。 */
            ESP_LOGW(TAG, "sync 仍在飞，跳过本轮降级（防 FATFS 锁撕裂）");
            display_refresh_resume();   /* 退出路径②：恢复刷新（防挂起泄漏） */
            return;
        }
        ESP_LOGW(TAG, "→ 尝试出厂分区降级");
        if (sd_tf_switch_to_factory() == 0) {
            /* 【单挂载整体切换】双挂载（/factory max_files=2）实测渲染打开多包
             * 超额 rc=-1 且内存代价压垮内部堆 → 回退本方案（已验证 rc=0/0）。 */
            asset_dl_reload_local();
            char ppath[MP_MPK_PATH_MAX], lpath[MP_MPK_PATH_MAX];
            /* ⚠️ 两个查询必须各用独立缓冲：layout_path 会覆盖 path（真机实证：
             * 共用一个缓冲 → render_set_parts 拿到 layout 路径 → 实体空） */
            if (asset_dl_parts_path(NULL, ppath, sizeof(ppath)) &&
                asset_dl_layout_path("stand1", lpath, sizeof(lpath))) {
                int prc = render_set_parts(ppath);
                int lrc2 = render_set_layout(lpath, true);
                ESP_LOGW(TAG, "降级绑定：parts=%s rc=%d | layout=%s rc=%d",
                         ppath, prc, lpath, lrc2);
                render_banner_show("FACTORY ASSETS - TF SYNC PENDING");
                ESP_LOGW(TAG, "出厂分区降级成功：神子快照接管渲染");
            } else {
                ESP_LOGE(TAG, "出厂分区降级后仍缺素材 → FATAL");
                display_refresh_resume();   /* 退出路径③：FATAL 前恢复刷新（防挂起泄漏） */
                transition(MP_ST_FATAL);
                watchdog_text_persist("ASSET LOAD FAILED", "WAIT SERVER SYNC");
                return;
            }
        } else {
            display_refresh_resume();   /* 退出路径④：FATAL 前恢复刷新（防挂起泄漏） */
            transition(MP_ST_FATAL);
            watchdog_text_persist("ASSET LOAD FAILED", "WAIT SERVER SYNC");
            return;
        }
    }
    /* 【实体形象恢复 2026-10-02】上次选的是怪物/NPC（NVS "entity"）→ 用它的
     * PARTS + 默认动作接管形象。纸娃娃的 parts/stand1 上面已经绑好 = 天然回落：
     * 实体包缺失/打开失败时屏上仍是纸娃娃，不会黑屏。 */
    if (entity_restore()) {
        /* 实体已接管形象（dispatch_entity 内部已绑默认动作循环布局）→ 不再叠一次
         * stand1（那是纸娃娃的动作名；实体态下只会多开一次包） */
        ESP_LOGW(TAG, "开机/素材同步后恢复实体形象：%s", s_entity);
    } else {
        dispatch_action(MP_ACTION_STAND);
    }

    /* 字体绑定完成后重显配对码（hello 早于字体加载，首显气泡会是空） */
    {
        const char *code = mp_http_pairing_code();
        if (code && code[0]) {
            mp_cmd_t c = { .type = MP_CMD_PAIRING_CODE };
            strlcpy(c.s, code, sizeof(c.s));
            mp_post_cmd(&c);
        }
    }

    /* 【默认地图重投】启动早期投的 SET_MAP 早于清单解析，查不到 BGMAP 路径
     * → 这里清单就绪后补投一次，保证"渲染默认地图 000010000"真的落地。
     * 【TF 模式同样重投 2026-10-01】旧条件限出厂模式：TF 模式下地图晚到
     * （950KB 大包多轮续传，真机本轮 250s 才齐）时 boot 那发失败后没人重投
     * → 背景全黑到底（真机实证：parts rc=0 宠物在、背景黑）。改为只要
     * 本轮没装载过地图就补投（g_map_loaded 门保证只补一次）。 */
    if (!g_map_loaded) {
        char mid[32];
        const char *want = active_map_get(mid, sizeof mid);
        mp_cmd_t mc = { .type = MP_CMD_SET_MAP };
        strlcpy(mc.s, want, sizeof(mc.s));
        mp_post_cmd(&mc);
        ESP_LOGW(TAG, "清单就绪 → 投活动地图 %s（NVS 记忆，缺省 %s）", want, MP_DEFAULT_MAP_ID);

        /* ══ 【地图兜底装载 2026-10-01（自 1.85B 迁移）】════════════════════════
         * 现象（185B 真机实证，216 同构风险）：活动地图 id 是按"地图 id"下发的，
         * 而服务端给本设备登记的 BGMAP **完全可能是另一张图**（真机 185B：
         * 固件要 000010000，服务器只登记了 mapId=2「枫叶路：香格里拉号」）→
         * set_active_map 按 id 查路径永远失败 → **背景全黑到底**，日志只有
         *   `set_active_map：M 无对应 BGMAP 条目（清单未就绪？）`
         * 而"补投"补的还是同一个不存在的 id，永远补不上。
         * 本兜底：投完用 `asset_dl_map_exists(want)`（语义 = 该 map_id 是否还在
         * 当前清单里，见 asset_dl.h）判一次；若清单里有 BGMAP 但目标图不在，
         * 就**直接用清单里第一张已缓存 BGMAP 的 content hash 再投一次**
         * （绕过 id→hash 映射），保证"有图就一定有背景"。
         * 用户从菜单选图时照旧按 id 走，不受影响。 */
        /* 【内部 RAM 腾挪 2026-10-02】兜底挑选用的 hash/label/cached 三张小表
         * （8×20 + 8×32 + 8B = 424B）原为内部 .bss；纯字符串/旗标，
         * 只在 state_machine 内读 → PSRAM 懒分配，失败即跳过兜底挑选
         * （与既有"清单里没有 BGMAP"路径同语义）。 */
        static char (*hs)[20]; static char (*lb)[32]; static bool *ca;
        if (!hs) hs = mp_psram_malloc(8 * 20);
        if (!lb) lb = mp_psram_malloc(8 * 32);
        if (!ca) ca = mp_psram_malloc(8);
        bool sel_ok = (hs && lb && ca);
        if (!sel_ok) ESP_LOGW(TAG, "兜底挑选缓冲分配失败 → 跳过本轮兜底挑选");
        int n = sel_ok ? asset_dl_bgmap_list(hs, lb, ca, 8) : 0;
        if (n > 0 && !asset_dl_map_exists(want)) {
            /* 【挑选优先级 2026-10-01 真机修正】原先只挑"第一张已缓存"，真机暴露
             * 真实场景：NVS 记着 004000032（另一个会话/服务端推过的图），但本设备
             * 清单里只有另一张 → 兜底会随便挑一张，与用户当前想看的图不符。
             * 现改为三级优先：
             *   ① 当前**激活**图（asset_dl_map_is_active：服务端推送或菜单刚选过的
             *      那张，语义="用户现在要的图"）；
             *   ② 已缓存的（不用等下载）；
             *   ③ 清单首图（触发下载）。
             * 三级都不命中才算真的没图。 */
            int pick = -1;
            for (int i = 0; i < n; i++) if (asset_dl_map_is_active(hs[i])) { pick = i; break; }
            if (pick < 0) for (int i = 0; i < n; i++) if (ca[i]) { pick = i; break; }
            if (pick < 0) pick = 0;
            mp_cmd_t fc = { .type = MP_CMD_SET_MAP };
            strlcpy(fc.s, hs[pick], sizeof(fc.s));
            mp_post_cmd(&fc);
            ESP_LOGW(TAG, "目标图 %s 不在清单 → 兜底装载 %s%.16s（%s）",
                     want,
                     asset_dl_map_is_active(hs[pick]) ? "当前激活图 " : "",
                     hs[pick], lb[pick]);
        } else if (n == 0) {
            ESP_LOGW(TAG, "清单里没有任何 BGMAP（服务端未登记地图？）—— 背景保持黑底");
        }
    }

    /* 素材全量重绑后强制一次全屏重绘：清除面板自检色块/旧画面残留
     * （无 BGMAP → 全屏填黑；有 BGMAP → static_back+tile），此后每帧走脏区。
     * 【闪烁治理 2026-10-02】只在"真的换了素材"或"首次绑定"时做 —— 否则每次
     * manifest 同步（大包下载期每 30s 一轮）都整屏重绘 = 屏上周期性闪一下。 */
    if (bind_changed || !s_redraw_done) {
        render_force_redraw();
        s_redraw_done = true;
        if (bind_changed) s_bound_once = true;
    } else {
        ESP_LOGI(TAG, "素材未变 → 跳过整屏重绘（防闪）");
    }

    /* 素材全绑完后的内部堆水位（可观测性：文件描述符/字模/位图都在内部堆或 PSRAM，
     * 真机曾因 max_files 用尽导致后续 mpak_open 全失败 → 一条水位日志能提前发现） */
    {
        extern void provision_dump_internal_heap(const char *stage);
        provision_dump_internal_heap("@素材全绑后");
    }

    /* 退出路径⑤（门拦截/绑定成功/失败都汇到这里的正常走完）：恢复刷新。
     * 至此入口挂起与 5 条退出路径的 resume 一一配对，无泄漏。 */
    display_refresh_resume();
}

void app_cmd_dispatch(const mp_cmd_t *cmd)
{
    switch (cmd->type) {
    case MP_CMD_SET_ACTION:
        if (action_maybe_camtest(cmd->s)) break;   /* 压测魔数（见上） */
        dispatch_action(cmd->s);
        break;
    case MP_CMD_SET_ENTITY:
        /* 怪物/NPC 形象切换（服务端 push kind=mob|npc / 菜单怪物页） */
        dispatch_entity(cmd->s);
        break;
    case MP_CMD_SET_EXPRESSION:
        /* 实体（怪物/NPC）的 LAYOUT 没有表情维度（导出契约：ExprIndex=255）→
         * 表情指令对它无意义，静默跳过（真机日志留一条，便于"表情没反应"取证）。 */
        if (s_entity[0]) {
            ESP_LOGD(TAG, "实体 %s 无表情维度，忽略表情指令 %s", s_entity, cmd->s);
            break;
        }
        render_set_expression(cmd->s);
        break;
    case MP_CMD_BUBBLE:
        /* 【压测魔数】与 `::shot` 同款：不需要服务端白名单即可触发相机拖动压测。
         * 例：气泡文本 "::camtest 12,8" = 连续平移 12 步、每步 8 世界像素。 */
        if (strncmp(cmd->s, "::camtest", 9) == 0) {
            int st = 12, sp = 8, lv = -1;
            sscanf(cmd->s + 9, "%d,%d,%d", &st, &sp, &lv);
            if (st <= 0) st = 12;
            cam_pan_test_run_ex(st, sp, lv);
            break;
        }
        /* 【远程取证魔数 2026-10-01】bubble 文本 "::shot" → UDP 帧倾倒（不走
         * TF，不需白名单放行 screenshot）。帧里不含本气泡——在显示前截走。 */
        if (strncmp(cmd->s, "::shot", 6) == 0) {
            render_frame_dump_udp();
            break;
        }
        /* 【E9/E12 互斥 2026-09-27】待机时钟态（CLOCK_DOZE）是纯黑只数字全屏，
         * 合成器在时钟激活时提前 return（不画气泡层）→ 此态收到气泡（E12 静置
         * 台词默认 300s 与 E9 待机时钟默认 5min 同刻触发）会静默丢失。
         * 按 E9 定稿「交互（触摸/按键/IMU）→ 立即唤醒回桌宠态」的口径，
         * 先唤醒再显示：气泡可见，且用户看到台词时不会停在黑屏时钟态。 */
        if (state_machine_current() == MP_ST_CLOCK_DOZE) {
            state_machine_notify_activity();
            ESP_LOGI(TAG, "气泡指令到达：先从待机时钟唤醒回桌宠态");
        }
        bubble_show(cmd->s, RENDER_FONT_24);   /* 协议传 UTF-8（E12） */
        break;
    case MP_CMD_CAM_SET: {
        /* 服务端"选镜头"界面下发：应用到渲染层并写入该图 NVS（重启/断网后仍生效）。
         * 【时序兜底 2026-10-01】真机：指令常在**地图尚未装载完**时到达（长轮询
         * 与启动/换图重叠）→ render_cam_supported() 还是 false → 之前直接丢弃，
         * 用户在 Web 点了"上送"却没反应。改为**挂起记忆**：地图一装载成功就补上
         * （见 dispatch_map 成功分支的 cam_pending_apply）。 */
        /* 【目标图校验 2026-10-02】指令带 mapId 时必须等**那张图**装载完成再应用；
         * 若当前显示的是别的图（或不支持平移），一律挂起（SET_MAP 已由 poller 先投）。 */
        char want[16];
        strlcpy(want, cmd->s, sizeof want);
        char cur[16] = "";
        const char *ah = asset_dl_active_map_hash();
        if (ah) asset_dl_map_id_of(ah, cur, sizeof cur);
        bool map_ok = (want[0] == 0) || (cur[0] && strcmp(want, cur) == 0);
        if (!render_cam_supported() || !map_ok) {
            s_cam_pending_x = cmd->a;
            s_cam_pending_y = cmd->b;
            s_cam_pending = true;
            strlcpy(s_cam_pending_map, want, sizeof s_cam_pending_map);
            ESP_LOGW(TAG, "服务端相机 %d,%d 目标图='%s'（当前='%s' 支持=%d）→ 挂起，"
                          "待目标整图装载后自动补上", (int)cmd->a, (int)cmd->b,
                     want[0] ? want : "(当前图)", cur[0] ? cur : "(无)",
                     (int)render_cam_supported());
            break;
        }
        render_cam_set(cmd->a, cmd->b);
        int32_t gx = 0, gy = 0;
        render_cam_get(&gx, &gy);
        char key[16];
        /* 当前活动地图的 NVS 键：activity map hash → map_id 派生（与相机 UX 同口径） */
        if (ah && asset_dl_map_key(ah, key, sizeof key)) {
            sm_cam_nvs_set(key, gx, gy);
            ESP_LOGW(TAG, "服务端相机 → 应用 (%d,%d)，已写入 NVS[key=%s]", (int)gx, (int)gy, key);
        } else {
            ESP_LOGW(TAG, "服务端相机 → 应用 (%d,%d)（无活动地图键，未持久化）", (int)gx, (int)gy);
        }
        break;
    }
    case MP_CMD_CAM_PAN_TEST:
        cam_pan_test_run(cmd->a > 0 ? cmd->a : 20, cmd->b != 0 ? cmd->b : 8);
        break;
    case MP_CMD_SET_MAP:
        bind_heap_probe("地图装载之前");
        dispatch_map(cmd->s);
        bind_heap_probe("地图装载之后");
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
        /* E13：屏显 6 位配对码 + 配对成功 cheers。
         * 【气泡去除后改道 2026-10-01】原走气泡 → 改走顶部常驻横幅，
         * 保证关掉气泡后新设备仍能看到 6 位配对码。 */
        {
            char code_line[28];
            snprintf(code_line, sizeof(code_line), "PAIR CODE: %.10s",
                     (cmd->s[0]) ? cmd->s : "------");
            render_banner_show(code_line);
        }
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
        bubble_show("固件升级中…", RENDER_FONT_24);
        break;
    case MP_CMD_OTA_FAIL:
        bubble_show("升级失败：已回滚", RENDER_FONT_24);
        render_set_expression(MP_EXPR_DAM);
        break;
    case MP_CMD_OTA_DONE:
        bubble_show("升级完成，重启中", RENDER_FONT_24);
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
    case MP_CMD_SCREENSHOT:                 /* 调试取证：g_fb 存 BMP 到 TF（渲染任务上下文） */
        render_screenshot_to_tf();
        break;
    case MP_CMD_NET_STATE:
    case MP_CMD_BGM_STATE:
    default:
        /* 查询型状态（bgm_get_state / state_machine_offline_mode）由渲染层轮询 */
        break;
    }
}
