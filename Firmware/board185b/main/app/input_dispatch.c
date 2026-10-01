/**
 * input_dispatch.c — 交互分发（E6 按键/重力/力度 + E10 表情状态机）
 *
 * 采样拓扑（4.1：APP 核 input 任务）——本板（LCD-1.85B）无触摸/无音频/无 PMU，
 * 触摸与电源巡检链路已随 216 专属驱动一并拆除：
 *   - IMU wait_event() DRDY 中断唤醒 + 20ms 超时兜底轮询（~50Hz，中断没通
 *     也不影响采样）；任务启动时读回 CTRL7 自愈使能位（见 imu_link_selfcheck）
 *   - 菜单键 = BOOT(GPIO0)（key_gpio0）纯轮询同口径消抖：短按=菜单开关、
 *     长按 ≥700ms=待机时钟（与原 GPIO18 菜单键口径一致，无 ISR——
 *     GPIO0 与复位时序相关）
 *   - 1s 慢速节拍：闲置→DOZE 计时（state_machine_tick_1hz）
 *   - 静置随机稀有表情（E10：wink/chu/qBlue 低频）
 */
#include "input_dispatch.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"

#include "app_core.h"
#include "provision.h"
#include "hal_contract.h"
#include "state_machine.h"

static const char *TAG = "input";

/* ------------------------------------------------------------------ */
/* 参数（默认值；阈值可被服务端下发覆盖——E6「阈值全部 Web 可配」）       */
/* ------------------------------------------------------------------ */
#define TAP_MAX_MS          400     /* 按下→抬起 <400ms = 轻点 */
#define TAP_MOVE_PX         30      /* 位移 <30px = 点（≥30px = 水平拖动，问题6） */
#define DRAG_TILT_FULL_PX   60      /* 问题6：水平拖动 ±60px ↔ ±8° 满幅视差 */
#define LONGPRESS_MS        800     /* 宠物区长按 → BGM 控制条（E6） */
#define SHAKE_WINDOW_MS     700     /* 摇晃检测窗口 */
#define SHAKE_MIN_FLIPS     4       /* 窗口内 x/y 过零次数阈值 */
#define SHAKE_MIN_G         1.6f    /* 翻转计数的幅度门槛 */
#define PICKUP_ANGLE_DEG    25.0f   /* 拿起/翻转：姿态偏离水平面 >25°（E6 拿起/翻转） */
#define PICKUP_HOLD_MS      600
#define IDLE_RARE_EXPR_S    30      /* 静置时稀有表情的滚动窗口（E10） */

/* 日志节流 / 键盘硬化 */
#define KEY_RELEASE_QUIET_MS 80    /* 菜单键：确认释放后需再静止 80ms 才重新上膛（门闩） */
#define IMU_FAIL_LOG_N      40      /* IMU 连续失败 ≈1s（50Hz）→ 首报 */
#define IMU_ERR_RELOG_MS    5000    /* IMU 持续失败时每 5s 重复一条 */

/* QMI8658 CTRL7（0x08）：bit0=aEN bit1=gEN（QMI8658A 手册 Register Map 口径；
 * 加速度+陀螺同开=0x03）。驱动 imu_qmi8658.c 写了 0xC0（bit7/6=自测位），
 * 传感器从未使能 → 数据恒 0。本文件启动时读回自愈，见 imu_link_selfcheck。 */
#define QMI_CTRL7_REG       0x08
#define QMI_CTRL7_ACC_GYR   0x03

/* ------------------------------------------------------------------ */
/* 倾斜状态（渲染层直读做视差；只在此处做死区+防抖）                     */
/* ------------------------------------------------------------------ */
static float    s_roll_deg, s_pitch_deg;
static uint8_t  s_tilt_bits;                 /* 稳定后的倾斜位 */
static uint8_t  s_tilt_candidate;            /* 防抖中的候选 */
static int64_t  s_tilt_cand_since_ms;

/* ------------------------------------------------------------------ */
/* 表情状态机（触发型播完回 default；静置低频稀有表情）                  */
/* ------------------------------------------------------------------ */
static char     s_expr_cur[24] = MP_EXPR_DEFAULT;
static int64_t  s_expr_until_ms;
static bool     s_expr_active;               /* 触发进行中（抑制随机表情） */

/* ------------------------------------------------------------------ */
/* 力度分级 / 摇晃                                                      */
/* ------------------------------------------------------------------ */
static int64_t  s_shake_flips_ts[16];
static int      s_shake_flip_cnt;
static float    s_last_ax;

/* ------------------------------------------------------------------ */
/* 拿起/翻转                                                            */
/* ------------------------------------------------------------------ */
static int64_t  s_pickup_since_ms;
static bool     s_pickup_active;
static bool     s_pickup_latched;

/* ------------------------------------------------------------------ */
/* 巡检                                                                 */
/* ------------------------------------------------------------------ */
static int64_t  s_last_rare_s;
static int64_t  s_last_interaction_ms;

/* 渲染层线规约（render.h）：渲染 API 只许 render 任务调 →
 * 一律走 cmd_q（动作/表情），视差例外走 render_input_tilt（异步安全） */
static char s_cur_action[24] = MP_EXPR_DEFAULT;    /* 幂等：同动作不重复重绑布局 */
static void post_action(const char *action)
{
    if (strcmp(s_cur_action, action) == 0) return;   /* 同动作跳过（防风暴） */
    strlcpy(s_cur_action, action, sizeof(s_cur_action));
    mp_cmd_t c = { .type = MP_CMD_SET_ACTION };
    strlcpy(c.s, action, sizeof(c.s));
    mp_post_cmd(&c);
}

static void post_expression(const char *expr)
{
    mp_cmd_t c = { .type = MP_CMD_SET_EXPRESSION };
    strlcpy(c.s, expr, sizeof(c.s));
    mp_post_cmd(&c);
}

/* 交互记账：本地闲置计时（随机表情用）+ 状态机唤醒/闲置刷新 */
static void note_interaction(void)
{
    extern void render_note_activity(void);
    render_note_activity();      /* 交互期冻结地图视差刷新（拖拽手感优先） */
    s_last_interaction_ms = mp_now_ms();
    state_machine_notify_activity();
}

/* ================================================================== */
/* 表情状态机（input 任务 + bgm/asset 任务都会触发 → 互斥）              */
/* ================================================================== */
static SemaphoreHandle_t s_expr_lock;

void input_dispatch_init(void)
{
    s_expr_lock = xSemaphoreCreateMutex();
}

void input_trigger_expression(const char *expr, uint32_t duration_ms)
{
    if (!s_expr_lock) return;
    if (xSemaphoreTake(s_expr_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (state_machine_menu_open()) {              /* 菜单期宠物表情冻结（E6） */
        xSemaphoreGive(s_expr_lock);
        return;
    }
    strlcpy(s_expr_cur, expr, sizeof(s_expr_cur));
    s_expr_until_ms = mp_now_ms() + duration_ms;
    s_expr_active = true;
    post_expression(s_expr_cur);
    xSemaphoreGive(s_expr_lock);
}

static void expr_fsm_tick(void)
{
    if (!s_expr_active || !s_expr_lock) return;
    if (xSemaphoreTake(s_expr_lock, 0) != pdTRUE) return;
    if (s_expr_active && mp_now_ms() >= s_expr_until_ms) {
        s_expr_active = false;                    /* 播完回 default（E10） */
        strlcpy(s_expr_cur, MP_EXPR_DEFAULT, sizeof(s_expr_cur));
        if (!state_machine_menu_open()) {
            post_expression(MP_EXPR_DEFAULT);
        }
    }
    xSemaphoreGive(s_expr_lock);
}

/* 静置低频稀有表情（E10）：静置 >60s 且无触发进行中，30s 窗口 15% 概率 */
static void expr_random_tick(int64_t idle_ms)
{
    if (s_expr_active || state_machine_menu_open()) return;
    if (idle_ms < 60000) { s_last_rare_s = 0; return; }

    int64_t now_s = mp_now_ms() / 1000;
    if (s_last_rare_s == 0) { s_last_rare_s = now_s; return; }
    if (now_s - s_last_rare_s < IDLE_RARE_EXPR_S) return;
    s_last_rare_s = now_s;

    if ((rand() % 100) < 15) {
        static const char *rare[] = { MP_EXPR_WINK, MP_EXPR_CHU, MP_EXPR_QBLUE };
        input_trigger_expression(rare[rand() % 3], 1600);
    }
}

/* ================================================================== */
/* 倾斜：±8° 死区 + 300ms 防抖，只报进入/退出（E6）                      */
/* ================================================================== */
void input_get_tilt(float *roll_deg, float *pitch_deg, uint8_t *bits)
{
    if (roll_deg)  *roll_deg  = s_roll_deg;
    if (pitch_deg) *pitch_deg = s_pitch_deg;
    if (bits)      *bits      = s_tilt_bits;
}

/* 从加速度矢量推角度（静止/低动态假设；IMU 中断触发意味着有动作，
 * 采样后的短暂动态由 300ms 防抖吸收）。
 *
 * 轴选择结论（问题1 ③）：QMI8658 标准贴装 X=板宽向（左右）、Y=板高向
 * （上下）、Z=屏幕法向朝外。板子左右倾斜=绕「指向使用者外侧」的水平轴
 * 旋转 → 改变重力在板坐标系的左右向分量（X）→ roll 用 X：
 * roll=atan2(-ax,√(ay²+az²))；上下倾斜（抬顶边）改变 Y → pitch 用 Y。
 * 验证：看 tilt_fsm_tick 变迁日志里的 acc 三分量——若真机左右倾斜时
 * ay 摆动而 ax 几乎不动（芯片转 90° 贴装），把本函数 roll 的 x/y 对调即可。 */
static void accel_to_angles(const imu_accel_t *a, float *roll, float *pitch)
{
    float norm = sqrtf(a->x_g * a->x_g + a->y_g * a->y_g + a->z_g * a->z_g);
    if (norm < 0.5f) {            /* 自由落体附近角度无意义 */
        *roll = s_roll_deg;
        *pitch = s_pitch_deg;
        return;
    }
    *roll  = atan2f(-a->x_g, sqrtf(a->y_g * a->y_g + a->z_g * a->z_g)) * 57.29578f;
    *pitch = atan2f(a->y_g, norm) * 57.29578f;
}

static uint8_t tilt_bits_from_angles(float roll, float pitch, float deadzone)
{
    uint8_t bits = 0;
    /* 竖握保护（真机定稿 2026-09-26）：|角度|>45° 是"手持姿态"而非"倾斜"——
     * 竖握时 roll≈±80° 曾导致 walk/stand 每 300ms 互切（布局重绑风暴→卡死）。
     * 只有 8°..45° 的温和倾斜才算倾斜交互 */
    if (fabsf(roll) <= 45.0f) {
        if (roll < -deadzone)  bits |= MP_TILT_LEFT;   /* 左倾 → 视差反向平移 */
        if (roll >  deadzone)  bits |= MP_TILT_RIGHT;
    }
    if (fabsf(pitch) <= 45.0f) {
        if (pitch >  deadzone) bits |= MP_TILT_UP;     /* 上倾 → fly（E6） */
    }
    return bits;
}

static void tilt_fsm_tick(const imu_accel_t *a)
{
    float roll, pitch;
    accel_to_angles(a, &roll, &pitch);
    s_roll_deg = roll;
    s_pitch_deg = pitch;

    /* E4 灵敏度同样作用于倾斜死区：有效死区 = 名义死区 ÷ 灵敏度
     * （灵敏度越高 → 死区越小 → 更小的倾斜即触发视差/动作） */
    float sens = g_mp_cfg.imu_sensitivity;
    if (sens < 0.2f) sens = 0.2f;
    if (sens > 3.0f) sens = 3.0f;
    uint8_t bits = tilt_bits_from_angles(roll, pitch, g_mp_cfg.imu_deadzone_deg / sens);
    if (bits != s_tilt_candidate) {
        s_tilt_candidate = bits;
        s_tilt_cand_since_ms = mp_now_ms();
        return;
    }
    if ((mp_now_ms() - s_tilt_cand_since_ms) < (int64_t)g_mp_cfg.tilt_debounce_ms) {
        return;                                   /* 300ms 防抖未满 */
    }
    if (bits == s_tilt_bits) return;              /* 无变迁 */

    /* 只报进入/退出（E6：不上报角度流；视觉由渲染层本地驱动） */
    uint8_t entered = bits & ~s_tilt_bits;
    uint8_t exited  = s_tilt_bits & ~bits;
    uint8_t prev    = s_tilt_bits;
    s_tilt_bits = bits;

    /* 节流日志：只在状态变迁时打一条（含 roll/pitch 与原始三分量，
     * 真机上直接核对轴向假设与死区/防抖行为；整数打印避免 %f 依赖） */
    int rd = (int)(s_roll_deg * 10.0f);
    int pd = (int)(s_pitch_deg * 10.0f);
    ESP_LOGI(TAG, "倾斜 0x%02X->0x%02X roll=%d.%d pitch=%d.%d (acc %d,%d,%d)mg",
             prev, bits, rd / 10, abs(rd % 10), pd / 10, abs(pd % 10),
             (int)(a->x_g * 1000.0f), (int)(a->y_g * 1000.0f),
             (int)(a->z_g * 1000.0f));

    /* 问题7 修复「回正不回中」：视差角度跟随稳定态——左右倾斜中透传当前
     * roll，退出（含回死区内）清零；旧实现只在变迁时透传 roll，退出后
     * 残角永留 → 条带/实体回不到中位。 */
    render_input_tilt((bits & (MP_TILT_LEFT | MP_TILT_RIGHT)) ? s_roll_deg : 0.0f);

    if (entered & MP_TILT_LEFT) {
        mp_post_event_simple(MP_EVT_TILT_ENTER, MP_TILT_LEFT, 0, NULL);
        post_action(MP_ACTION_WALK);              /* 左右倾斜：人物原地 walk1（E6） */
    }
    if (entered & MP_TILT_RIGHT) {
        mp_post_event_simple(MP_EVT_TILT_ENTER, MP_TILT_RIGHT, 0, NULL);
        post_action(MP_ACTION_WALK);
    }
    if (entered & MP_TILT_UP) {
        mp_post_event_simple(MP_EVT_TILT_ENTER, MP_TILT_UP, 0, NULL);
        post_action(MP_ACTION_FLY);               /* 上倾 → fly（E6） */
    }
    if (exited & MP_TILT_UP) {
        mp_post_event_simple(MP_EVT_TILT_EXIT, MP_TILT_UP, 0, NULL);
        post_action(MP_ACTION_STAND);             /* 回正 → stand（E6） */
    }
    if ((exited & (MP_TILT_LEFT | MP_TILT_RIGHT)) && !(bits & (MP_TILT_LEFT | MP_TILT_RIGHT))) {
        mp_post_event_simple(MP_EVT_TILT_EXIT, MP_TILT_LEFT | MP_TILT_RIGHT, 0, NULL);
        post_action(MP_ACTION_STAND);
    }

    note_interaction();
}

/* ================================================================== */
/* 力度分级（E6：设备端算好只上报事件）                                  */
/* ================================================================== */
static void force_tick(const imu_accel_t *a)
{
    float mag = sqrtf(a->x_g * a->x_g + a->y_g * a->y_g + a->z_g * a->z_g);
    int32_t mg = (int32_t)(mag * 1000.0f);

    /* E4 IMU 灵敏度：倍率作用于名义阈值（有效阈值 = 名义 ÷ 灵敏度）。
     * 兜底夹取到 0.2–3.0，防手改 NVS/服务端异常值把判定打飞。 */
    float sens = g_mp_cfg.imu_sensitivity;
    if (sens < 0.2f) sens = 0.2f;
    if (sens > 3.0f) sens = 3.0f;
    const float thr_hard  = g_mp_cfg.tap_hard_g  / sens;
    const float thr_light = g_mp_cfg.tap_light_g / sens;

    /* --- 轻拍 / 大力拍打（加速度峰值分级） --- */
    if (mag >= thr_hard) {
        /* ≥4g：hit 表情 */
        mp_post_event_simple(MP_EVT_TAP_HARD, mg, 0, NULL);
        input_trigger_expression(MP_EXPR_HIT, 1500);
        note_interaction();
        return;
    } else if (mag > 1.25f && mag < thr_light) {
        /* <2g：alert 动作 + bewildered 表情（E6） */
        mp_post_event_simple(MP_EVT_TAP_LIGHT, mg, 0, NULL);
        post_action(MP_ACTION_ALERT);
        input_trigger_expression(MP_EXPR_BEWILDERED, 1800);
        note_interaction();
    }

    /* --- 剧烈摇晃：窗口内 x/y 大幅度过零次数（有来回才有「摇」） --- */
    int64_t now = mp_now_ms();
    if (fabsf(a->x_g - s_last_ax) > SHAKE_MIN_G) {
        bool sign_flip = (a->x_g * s_last_ax < 0);
        s_last_ax = a->x_g;
        if (sign_flip) {
            /* 压缩翻转时间戳数组（环形） */
            int keep = 0;
            for (int i = 0; i < s_shake_flip_cnt; i++) {
                if (now - s_shake_flips_ts[i] < SHAKE_WINDOW_MS) {
                    s_shake_flips_ts[keep++] = s_shake_flips_ts[i];
                }
            }
            s_shake_flip_cnt = keep;
            if (s_shake_flip_cnt < 16) {
                s_shake_flips_ts[s_shake_flip_cnt++] = now;
            }
            if (s_shake_flip_cnt >= SHAKE_MIN_FLIPS) {
                s_shake_flip_cnt = 0;
                mp_post_event_simple(MP_EVT_SHAKE, 0, 0, NULL);
                input_trigger_expression(MP_EXPR_STUNNED, 2200);   /* 剧烈摇晃 → stunned */
                note_interaction();
            }
        }
    } else {
        s_last_ax = a->x_g;
    }

    /* --- 拿起/翻转：姿态偏离水平面持续 600ms → fly + oops（E6） --- */
    float devi = sqrtf(s_roll_deg * s_roll_deg + s_pitch_deg * s_pitch_deg);
    if (devi > PICKUP_ANGLE_DEG && fabsf(a->z_g) < 0.85f) {
        /* z 轴明显不朝天（翻转/竖持）——拿起判定 */
        if (!s_pickup_active) {
            s_pickup_active = true;
            s_pickup_since_ms = now;
        } else if (now - s_pickup_since_ms > PICKUP_HOLD_MS && !s_pickup_latched) {
            s_pickup_latched = true;
            mp_post_event_simple(MP_EVT_PICKUP, 0, 0, NULL);
            post_action(MP_ACTION_FLY);
            input_trigger_expression(MP_EXPR_OOPS, 1800);
            note_interaction();
        }
    } else {
        s_pickup_active = false;
        s_pickup_latched = false;
    }
}

/* ================================================================== */
/* 菜单键（本板 = BOOT GPIO0；原 216 板的 GPIO18 独立菜单键已随板拆除）    */
/* ================================================================== */
/* 菜单键按下沿：POKER/OFFLINE→MENU，MENU→POKER（DOZE 下=先唤醒）。
 * 状态机内部锁竞争时会静默丢事件（handle 里 take 失败直接 return），
 * 这里用 before/after 迁移日志把「按了没反应」暴露出来（问题2）。 */
static void key_fire_menu_toggle(void)
{
    mp_state_t before = state_machine_current();

    /* 【E6 半屏控制条】控制条显示时，顶键=确认当前控件（侧键口径与菜单一致：
     * 顶=OK 中=移光标 底=收起），不打开菜单。 */
    if (render_bgm_bar_showing()) {
        ESP_LOGI(TAG, "顶键：控制条确认第 %d 项", render_bgm_bar_sel());
        render_bgm_bar_activate(render_bgm_bar_sel());
        return;
    }

    if (before == MP_ST_MENU) {
        /* 菜单真实化：MENU 态顶键短按=确认当前项（LVGL 内部处理），
         * 退出菜单走屏上 Exit 项（post MENU_EXIT → 状态机回 POKER） */
        render_menu_ok();
        mp_state_t after = state_machine_current();
        ESP_LOGI(TAG, "菜单键：确认（%s）", state_machine_name(after));
        return;
    }
    state_machine_handle(MP_SM_EV_MENU_KEY);
    mp_state_t after = state_machine_current();
    if (before != after) {
        ESP_LOGI(TAG, "菜单键：%s -> %s（%s）", state_machine_name(before),
                 state_machine_name(after),
                 (after == MP_ST_MENU) ? "菜单全屏" : "宠物画面");
    } else if (before == MP_ST_POKER || before == MP_ST_MENU ||
               before == MP_ST_OFFLINE || before == MP_ST_CLOCK_DOZE) {
        /* 该迁移本应发生却没发生（锁竞争丢事件），值得一条告警；
         * OTA/FATAL/配网态菜单键本就无效，不打日志防刷屏 */
        ESP_LOGW(TAG, "菜单键未迁移（态 %s，疑似事件被丢）", state_machine_name(before));
    }
}

static int64_t s_k0_pressed_ms;      /* 菜单内该键按下时刻（长按判定） */
static bool    s_k0_long_fired;      /* 本次按压已触发长按 */
static bool    s_k0_wait_release;    /* 正在等释放（短按=下移在释放时执行） */
static bool    s_k0_bar_mode;        /* 【E6】本次按压归 BGM 控制条（否则归菜单） */
/* 【185B 菜单键 2026-10-01】声明提前到首个使用（key0_tick 的菜单键分支）之前，
 * 否则 C 文件作用域声明后置 = 编译错误（板A构建实证）。
 * 一次按压只产生一个动作：按下沿只上闩不动作，key0_menu_key_long_tick 里
 * 「持续 ≥700ms → 待机时钟」或「释放且未转时钟 → 菜单开关」二选一——
 * clock_fired 保证互斥（原 GPIO18 菜单键同口径）。 */
static bool    s_k0_menu_latch;
static int64_t s_k0_menu_press_ms;
static bool    s_k0_menu_clock_fired;

static void key0_menu_hold_tick(void);   /* 定义见下（前向声明） */
static void key0_menu_key_long_tick(void);

static void key0_tick(void)
{
    key0_menu_hold_tick();               /* 菜单内：按住时长状态机（长按退出/短按下移） */
    key0_menu_key_long_tick();           /* 185B：BOOT=菜单键的长按转时钟 */
    if (!key_gpio0_tick()) return;       /* 消抖后的按下沿事件（一次/按压） */
    ESP_LOGI(TAG, "底键（GPIO0）按下沿");
    /* 【唤醒前快照】下方 note_interaction() 在 CLOCK_DOZE 态会同步唤醒回
     * POKER，其后 state_machine_current() 恒读到 POKER（原 DOZE 分支只在
     * 唤醒事件被锁竞争丢弃时才可达）。185B 菜单键分支用该快照识别
     * 「本次按下沿已承担唤醒」：唤醒沿只唤醒、不再当菜单键（菜单键事件在
     * CLOCK_DOZE=仅唤醒，同原 GPIO18 MENU_KEY 口径），否则在时钟态按一下 BOOT 会
     * 连菜单一起带出来、按住则唤醒后又被拉回时钟。 */
    mp_state_t st_before = state_machine_current();
    {
        /* 【中键取证】NVS 累计计数 + 按下时状态机状态：计数证明通路，
         * 状态字节裁决"菜单里没反应"是按键没到还是分支走错 */
        mp_state_t st_now = st_before;
        nvs_handle_t h;
        if (nvs_open("calib", NVS_READWRITE, &h) == ESP_OK) {
            uint32_t n = 0;
            nvs_get_u32(h, "key0", &n);
            nvs_set_u32(h, "key0", n + 1);
            nvs_set_u8(h, "k0st", (uint8_t)st_now);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    note_interaction();
    mp_state_t st = state_machine_current();
    if (st == MP_ST_MENU) {
        /* 【用户定稿 2026-09-27】这个键（红框底键，走 GPIO0 通路）在菜单里：
         *   短按 → 光标【下移】（此前是上移，用户明确要求改向下）
         *   长按（≥800ms）→ 退出菜单
         * 实现：按下沿不直接动作，交给"按住时长"判定——≥阈值走长按退出；
         * 否则在【释放】时按短按下移（这样长按不会顺带挪一格）。 */
        extern void render_menu_nav(int dir);
        extern int  render_menu_nav_down(void);      /* 下移（含末行回绕） */
        extern void render_menu_request_exit(void);  /* 退出菜单 */

        s_k0_pressed_ms = mp_now_ms();
        s_k0_long_fired = false;
        s_k0_wait_release = true;
        s_k0_bar_mode = false;
        (void)render_menu_nav;
        return;
    }
    if (st == MP_ST_CLOCK_DOZE) {
        state_machine_notify_activity(); /* DOZE：先唤醒回 POKER */
        return;
    }
    /* 【E6 半屏控制条】控制条显示时，底键（GPIO0）= 光标左移（短按）/收起（长按），
     * 与菜单内「短按移动 · 长按退出」同构，用户不用记两套。 */
    if (render_bgm_bar_showing()) {
        s_k0_pressed_ms = mp_now_ms();
        s_k0_long_fired = false;
        s_k0_wait_release = true;
        s_k0_bar_mode = true;
        return;
    }
    /* POKER/OFFLINE：BOOT(GPIO0) = 菜单键（【185B 2026-10-01 定稿】本板无
     * GPIO18 且音频关闭，BOOT 升级为菜单键：短按（释放时）=菜单开关，
     * 长按 ≥700ms=待机时钟。动作【不在按下沿】执行：若按下沿就 toggle，
     * 长按会「先开菜单再转时钟」两个都触发；改在上闩后由
     * key0_menu_key_long_tick 按时长/释放二选一。本板 profile key.menu 恒
     * -1，原 216 板的「menu>=0 → 音量减」分支已随板拆除。 */
    if (st_before == MP_ST_CLOCK_DOZE) return;   /* 唤醒沿：只唤醒（见上） */
    s_k0_menu_latch = true;                      /* 上闩：动作延到释放/700ms */
    s_k0_menu_press_ms = mp_now_ms();
    s_k0_menu_clock_fired = false;
}

/* 185B 菜单键（BOOT）短按/长按分辨，key0_tick 每轮驱动（不看状态机状态，
 * 故闩在任何态下都有出路：释放或 700ms 二者必至其一，不会卡死）：
 *   - 持续 ≥700ms → 待机时钟（IDLE_TIMEOUT），闩清；
 *   - 释放且未转时钟 → 菜单开关一次（key_fire_menu_toggle，含控制条确认/
 *     菜单内确认语义）。
 * clock_fired 保证长按转时钟后释放不再补发
 * 菜单开关（互斥）；按下沿不动作，故短按/长按天然只出其一。
 * 释放判定用原始电平（同 key0_menu_hold_tick 口径）；驱动 key_gpio0_tick
 * 的「释放+80ms 静止再上膛」闩保证释放弹回不会产生第二次按下沿。 */
static void key0_menu_key_long_tick(void)
{
    if (!s_k0_menu_latch) return;
    if (!key_gpio0_pressed()) {              /* 释放沿 */
        s_k0_menu_latch = false;
        if (!s_k0_menu_clock_fired) {
            s_k0_menu_clock_fired = true;    /* 一次按压只 fire 一个动作 */
            key_fire_menu_toggle();          /* 短按：菜单开关 */
        }
        return;
    }
    if (mp_now_ms() - s_k0_menu_press_ms >= 700) {
        s_k0_menu_clock_fired = true;
        s_k0_menu_latch = false;
        ESP_LOGI(TAG, "菜单键长按 → 待机时钟");
        state_machine_handle(MP_SM_EV_IDLE_TIMEOUT);
    }
}

/* 菜单内该键的"按住时长"状态机（key0_tick 每次调用都跑，含无按下沿的轮询帧）。
 * 【E6】BGM 半屏控制条复用同一套：短按=光标左移，长按=收起控制条。 */
#define K0_LONG_MS 800
static void key0_menu_hold_tick(void)
{
    if (!s_k0_wait_release) return;
    mp_state_t st = state_machine_current();
    bool on_bar = render_bgm_bar_showing();
    /* 菜单态与"控制条显示中"两种归属，都不在则清状态 */
    if (st != MP_ST_MENU && !on_bar) {
        s_k0_wait_release = false;
        return;
    }
    bool still_down = key_gpio0_pressed();
    int64_t held = mp_now_ms() - s_k0_pressed_ms;

    if (still_down && !s_k0_long_fired && held >= K0_LONG_MS) {
        s_k0_long_fired = true;
        if (on_bar) {
            ESP_LOGI(TAG, "底键长按（≥%dms）→ 收起 BGM 控制条", K0_LONG_MS);
            render_bgm_bar_hide();
        } else {
            ESP_LOGI(TAG, "底键长按（≥%dms）→ 退出菜单", K0_LONG_MS);
            extern void render_menu_request_exit(void);
            render_menu_request_exit();
        }
        return;
    }
    if (!still_down) {                      /* 释放 */
        s_k0_wait_release = false;
        if (!s_k0_long_fired) {
            if (s_k0_bar_mode) {
                ESP_LOGI(TAG, "底键短按 → 控制条光标左移");
                render_bgm_bar_nav(0);
            } else {
                ESP_LOGI(TAG, "底键短按 → 菜单光标下移");
                extern int render_menu_nav_down(void);
                render_menu_nav_down();
            }
        }
        s_k0_long_fired = false;
    }
}

/* ================================================================== */
/* 【临时探针】中键引脚 GPIO 电平扫描（MP_KEY_SCAN_PROBE，定稿后整段删）  */
/* ================================================================== */
/* 中键疑似在 GPIO0（wiki：Key2 同接 GPIO0/CHIP_PU），但真机按下零日志 →
 * 引脚归属未定。本探针 boot 后扫 60s 自动自删：20ms 轮询候选脚（输入+内部
 * 上拉，与 key_gpio0 同款），任一脚电平跳变即 ESP_LOGI（每脚 1s 限频防刷屏）。常驻不退出。
 *
 * 候选集 = ESP32-S3 全脚（0-21,26-48）排除后剩下的空闲脚：
 *   - 板上外设已占（profile + wiki GPIO 表）：SDMMC 12/13/14/15/16、
 *     LCD 3/5/21/40/41/42/45/46、I2C 10/11、RTC INT 6；
 *   - 系统用途不可碰：26-32（Flash）、35-37（S3R8 Octal PSRAM）、
 *     19/20（原生 USB=烧录/日志口）、43/44（UART0 控制台）；
 *   → 剩余：GPIO0（BOOT，strapping 只能输入正合适）、GPIO16（板卡
 *     wiki 列为 SD 用途，防走线存疑）、GPIO47/48（S3 空脚，wiki 表
 *     未列出，防板上另有走线；未连时上拉读 1 恒定，无害）。
 */
#define MP_KEY_SCAN_PROBE 0   /* 诊断期结束：常驻扫描探针占内部堆，已定位按键归属 */

#if MP_KEY_SCAN_PROBE
#include "driver/gpio.h"

/* 【2026-09-27 扩表】中键实测不在 GPIO0（GPIO0 恒 1、按下沿恒 0）→ 扩到
 * ESP32-S3 上所有"可能空闲"的脚，逐一上拉监听跳变，用户按一下即可定位。
 * 排除项（踩过的坑必须写清）：
 *   - 板上外设：SDMMC 12/13/14/15/16、LCD 3/5/21/40/41/42/45/46、
 *     I2C 10/11、RTC INT 6；
 *   - 系统：26-32 Flash、**33/34 也是 Octal PSRAM 数据脚**（本板 8MB Octal
 *     PSRAM；配成 GPIO 输入会打断 PSRAM 总线 → 立刻 WDT 复位，实测踩过）、
 *     35-37 PSRAM、19/20 USB、43/44 UART0。 */
static const int s_keyscan_pins[] = { 0, 16, 47, 48 };
#define KEYSCAN_PIN_N       (sizeof(s_keyscan_pins) / sizeof(s_keyscan_pins[0]))
#define KEYSCAN_POLL_MS     20       /* 轮询节拍（与键 tick 同） */
#define KEYSCAN_LOG_GAP_MS  1000     /* 同一脚跳变日志限频 */

static void keyscan_task(void *arg)
{
    (void)arg;
    int     last_lvl[KEYSCAN_PIN_N];
    int64_t last_log[KEYSCAN_PIN_N] = {0};
    bool    cfg_ok[KEYSCAN_PIN_N]   = {false};

    for (int i = 0; i < (int)KEYSCAN_PIN_N; i++) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_keyscan_pins[i],
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,   /* 按下接地口径（key_gpio0 同款） */
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,    /* 只读不扰，一律不挂中断 */
        };
        cfg_ok[i] = (gpio_config(&io) == ESP_OK);
        if (!cfg_ok[i]) {
            ESP_LOGW("keyscan", "GPIO%d 配置失败，跳过", s_keyscan_pins[i]);
            last_lvl[i] = -1;
            continue;
        }
        last_lvl[i] = gpio_get_level(s_keyscan_pins[i]);
        ESP_LOGI("keyscan", "GPIO%d 初值 level=%d", s_keyscan_pins[i], last_lvl[i]);
    }
    ESP_LOGI("keyscan", "探针启动：候选 0/16/47/48，常驻（MP_KEY_SCAN_PROBE 关闭才退出）");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(KEYSCAN_POLL_MS));
        int64_t now = mp_now_ms();
        for (int i = 0; i < (int)KEYSCAN_PIN_N; i++) {
            if (!cfg_ok[i]) continue;
            int lvl = gpio_get_level(s_keyscan_pins[i]);
            if (lvl == last_lvl[i]) continue;
            last_lvl[i] = lvl;                   /* 限频窗口内的跳变只刷新电平 */
            if (now - last_log[i] >= KEYSCAN_LOG_GAP_MS) {
                last_log[i] = now;
                ESP_LOGI("keyscan", "GPIO%d -> %d", s_keyscan_pins[i], lvl);
            }
        }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));   /* 常驻：任何时刻按键都会被记录 */
}

static int     s_keyscan_tries;
static int64_t s_keyscan_next_try_ms;

/* 首次创建 + 主循环低频重试（非阻塞：每 2s 一次，至多 15 次）。
 * 扫描窗从任务真正跑起来起算，重试不损诊断价值 */
static void keyscan_probe_try(bool first)
{
    if (xTaskCreatePinnedToCore(keyscan_task, "keyscan", 3072, NULL,
                                tskIDLE_PRIORITY + 1, NULL, 0) == pdPASS) {
        s_keyscan_tries = 99;            /* 成功：关闭重试 */
        return;
    }
    s_keyscan_tries++;
    if (first || s_keyscan_tries == 15)
        ESP_LOGW(TAG, "keyscan 探针任务创建失败(第%d次) 内部空闲=%u 最大块=%u",
                 s_keyscan_tries,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    s_keyscan_next_try_ms = mp_now_ms() + 2000;
}

static void keyscan_probe_retry(void)
{
    if (s_keyscan_tries >= 15 || s_keyscan_tries == 0) return;   /* 未失败/已放弃/已成功 */
    if (mp_now_ms() < s_keyscan_next_try_ms) return;
    keyscan_probe_try(false);
}
#else
static inline void keyscan_probe_retry(void) {}   /* 探针关闭：主循环调用点免 #if */
#endif /* MP_KEY_SCAN_PROBE */

/* ================================================================== */
/* 慢速巡检：RTC 校时重试（E10/E11 的电池/温度巡检随 PMU 驱动一并移除：
 * 本板 has_pmu=false，原轮询恒空转）                                     */
static bool    s_rtc_retry_done;
static int64_t s_rtc_retry_s;

static void slow_tick(int64_t idle_ms)
{
    int64_t now_s = mp_now_ms() / 1000;

    /* 【真机修复 2026-09-27】校时任务创建失败时重试（内部堆挤压是启动期常见，
     * 几秒后往往就能分配）；幂等：已成功启动则内部直接返回。 */
    if (!s_rtc_retry_done && now_s - s_rtc_retry_s >= 5) {
        s_rtc_retry_s = now_s;
        provision_rtc_resync_start();
        extern bool provision_rtc_task_running(void);
        if (provision_rtc_task_running()) s_rtc_retry_done = true;
    }

    state_machine_tick_1hz();     /* 闲置 → CLOCK_DOZE（E9） */
    expr_random_tick(idle_ms);
}

/* ================================================================== */
/* IMU 通路自愈 + 自检（问题1 根因）                                     */
/* ================================================================== */
/* 根因：驱动 imu_qmi8658.c 写 CTRL7=0xC0——按 QMI8658A 手册 Register Map，
 * bit0=aEN / bit1=gEN 才是传感器使能（bit7/6 为自测位 aST/gST），0xC0 两个
 * 传感器都没开 → 数据寄存器恒 0 → norm<0.5 兜底把角度永久钉在 0：倾斜/
 * 拍打/摇晃全灭；而 WHO_AM_I 校验通过、驱动照打「QMI8658 就绪」——正是
 * 「就绪但完全无效」的假象。驱动文件不在本次修改边界 → 消费侧启动时读回
 * 自愈一次。永久修复点（主线程落地）：imu_qmi8658.c 的
 * QMI8658_CTRL7_VAL 0xC0 → 0x03。 */
static void imu_link_selfcheck(void)
{
    if (!imu_qmi8658_ready()) {
        ESP_LOGE(TAG, "IMU 未就绪（init 失败）：倾斜/拍打/摇晃不可用");
        return;
    }
    i2c_master_dev_handle_t dev = i2c_bus_find("qmi8658");
    if (dev) {
        uint8_t ctrl7 = 0;
        if (i2c_bus_read_reg8v(dev, QMI_CTRL7_REG, &ctrl7, 1) == ESP_OK &&
            ctrl7 != QMI_CTRL7_ACC_GYR) {
            i2c_bus_write_reg8(dev, QMI_CTRL7_REG, QMI_CTRL7_ACC_GYR);
            ESP_LOGW(TAG, "IMU CTRL7=0x%02X 使能位错误(aEN/gEN 未开) -> 复写 0x03",
                     ctrl7);
        }
    }
    /* 使能后等首批数据：静止重力 ≈1g；200ms 内仍无有效矢量即通路不通 */
    for (int i = 0; i < 5; i++) {
        vTaskDelay(pdMS_TO_TICKS(40));
        imu_accel_t a;
        if (mp_imu_read_accel(&a)) {
            float norm = sqrtf(a.x_g * a.x_g + a.y_g * a.y_g + a.z_g * a.z_g);
            if (norm > 0.5f) {
                ESP_LOGI(TAG, "IMU 通路自检 OK：重力 (%d,%d,%d)mg",
                         (int)(a.x_g * 1000.0f), (int)(a.y_g * 1000.0f),
                         (int)(a.z_g * 1000.0f));
                return;
            }
        }
    }
    ESP_LOGE(TAG, "IMU 读取失败：CTRL7 修复后仍无有效重力（读数全 0）"
                  "——驱动层需核对 CTRL7/量程档位/接线");
}

/* ================================================================== */
/* 任务入口                                                             */
/* ================================================================== */
void input_dispatch_task(void *arg)
{
    /* 【放行旗标 2026-09-27】本任务被提前创建（内部堆宽裕时抢 4096 栈，
     * 否则真机上它建不起来 → 触摸/按键/IMU 全失效）。为保证初始化顺序不变
     * （I2C/触摸/IMU 依赖启动中段的驱动就绪），这里先等 app_main 放行。 */
    {
        extern volatile bool g_input_go;
        while (!g_input_go) vTaskDelay(pdMS_TO_TICKS(50));
    }

    (void)arg;
    key_gpio0_init();             /* 菜单键 BOOT=GPIO0（输入+上拉+轮询消抖，绝不输出） */
#if MP_KEY_SCAN_PROBE
    /* 【临时探针】中键引脚扫描：低优先级 core0，60s 自删（定稿后随
     * MP_KEY_SCAN_PROBE 一起移除） */
    /* 真机实证（2026-09-26）：启动挤压窗口期内部堆可低至空闲 6KB/最大块 3KB
     * （<3072 任务栈）——首次失败由主循环 keyscan_probe_retry 低频重试 */
    keyscan_probe_try(true);
#endif
    srand((unsigned)mp_now_ms());
    s_last_ax = 0;
    imu_link_selfcheck();

    static int imu_fail_cnt;              /* 连续失败/全 0 采样计数 */
    static int64_t imu_err_last_ms;

    for (;;) {
        imu_accel_t accel;
        if (imu_qmi8658_ready()) {
            /* 主路：DRDY 中断（GPIO17/INT1）唤醒；兜底：20ms 超时后
             * 【仍然无条件读一次】→ 中断没配通时等效 ~50Hz 轮询（采样
             * 循环不被阻塞，倾斜判定不受中断影响）。 */
            mp_imu_wait_event(pdMS_TO_TICKS(20));
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));   /* IMU 不在线：维持 ~50Hz 节拍防忙转 */
        }

        bool got = mp_imu_read_accel(&accel);
        float norm = got ? sqrtf(accel.x_g * accel.x_g + accel.y_g * accel.y_g +
                                 accel.z_g * accel.z_g) : 0.0f;
        /* 兜底报错（问题1）：读失败【或读数全 0（<0.05g，静止现实不存在）】
         * 都显式报「IMU 读取失败」，不再静默——前者 I2C/器件问题，后者典型
         * 即 CTRL7 使能未开。节流：累计 ≈1s 首报 + 每 5s 重复。 */
        if (got && norm > 0.05f) {
            if (imu_fail_cnt >= IMU_FAIL_LOG_N) {
                ESP_LOGI(TAG, "IMU 采样恢复");
            }
            imu_fail_cnt = 0;
            tilt_fsm_tick(&accel);
            force_tick(&accel);
        } else if (++imu_fail_cnt == IMU_FAIL_LOG_N ||
                   (imu_fail_cnt > IMU_FAIL_LOG_N &&
                    mp_now_ms() - imu_err_last_ms >= IMU_ERR_RELOG_MS)) {
            imu_err_last_ms = mp_now_ms();
            if (!got) {
                ESP_LOGE(TAG, "IMU 读取失败（I2C 连续 %d 次）——倾斜不可用",
                         imu_fail_cnt);
            } else {
                ESP_LOGE(TAG, "IMU 读取失败（连续 %d 次读数全 0，传感器未出数）"
                              "——倾斜不可用，查 CTRL7 使能", imu_fail_cnt);
            }
        }

        keyscan_probe_retry();
        /* 【E7】菜单 BGM 入口请求：收菜单 + 呼出半屏控制条（消费点唯一，避免
         * 渲染任务与输入任务同时改状态） */
        if (render_bgm_bar_take_menu_request()) {
            ESP_LOGI(TAG, "菜单 BGM 入口：收菜单并呼出半屏控制条");
            state_machine_handle(MP_SM_EV_MENU_KEY);   /* MENU → POKER */
            render_bgm_bar_show();
            note_interaction();
        }
        key0_tick();              /* 菜单键 BOOT=GPIO0：短按菜单开关 / 长按待机时钟 */
        expr_fsm_tick();

        int64_t idle_ms = (s_last_interaction_ms == 0)
                          ? 0 : (mp_now_ms() - s_last_interaction_ms);
        slow_tick(idle_ms);
    }
}
