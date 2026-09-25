/**
 * input_dispatch.c — 交互分发（E6 触摸/重力/力度/按键 + E10 表情状态机）
 *
 * 采样拓扑（4.1：APP 核 input 任务）：
 *   - 触摸 touch_read() 20ms 轮询（MENU 态停读——LVGL indev 接管，防双读）
 *   - IMU imu_wait_event() 双中断（GPIO17/21）唤醒采样，非轮询
 *   - 菜单键 key_gpio18 30ms 防抖轮询
 *   - 1s 慢速节拍：闲置→DOZE 计时（state_machine_tick_1hz）、电池/温度巡检
 *   - 静置随机稀有表情（E10：wink/chu/qBlue 低频）
 */
#include "input_dispatch.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "app_core.h"
#include "hal_contract.h"
#include "state_machine.h"

/* ------------------------------------------------------------------ */
/* 参数（默认值；阈值可被服务端下发覆盖——E6「阈值全部 Web 可配」）       */
/* ------------------------------------------------------------------ */
#define TAP_MAX_MS          400     /* 按下→抬起 <400ms = 轻点 */
#define TAP_MOVE_PX         30      /* 位移 <30px = 点（否则是拖动，忽略） */
#define LONGPRESS_MS        800     /* 宠物区长按 → BGM 控制条（E6） */
#define SHAKE_WINDOW_MS     700     /* 摇晃检测窗口 */
#define SHAKE_MIN_FLIPS     4       /* 窗口内 x/y 过零次数阈值 */
#define SHAKE_MIN_G         1.6f    /* 翻转计数的幅度门槛 */
#define PICKUP_ANGLE_DEG    25.0f   /* 拿起/翻转：姿态偏离水平面 >25°（E6 拿起/翻转） */
#define PICKUP_HOLD_MS      600
#define BATTERY_CHK_S       60
#define TEMP_CHK_S          60
#define TEMP_HOT_C          45.0f
#define IDLE_RARE_EXPR_S    30      /* 静置时稀有表情的滚动窗口（E10） */

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
static int64_t  s_last_battery_s, s_last_temp_s, s_last_rare_s;
static bool     s_battery_low_reported;
static int64_t  s_last_interaction_ms;

/* 渲染层线规约（render.h）：渲染 API 只许 render 任务调 →
 * 一律走 cmd_q（动作/表情），视差例外走 render_input_tilt（异步安全） */
static void post_action(const char *action)
{
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
 * 采样后的短暂动态由 300ms 防抖吸收） */
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
    if (roll < -deadzone)  bits |= MP_TILT_LEFT;   /* 左倾 → 视差反向平移 */
    if (roll >  deadzone)  bits |= MP_TILT_RIGHT;
    if (pitch >  deadzone) bits |= MP_TILT_UP;     /* 上倾 → fly（E6） */
    return bits;
}

static void tilt_fsm_tick(const imu_accel_t *a)
{
    float roll, pitch;
    accel_to_angles(a, &roll, &pitch);
    s_roll_deg = roll;
    s_pitch_deg = pitch;

    uint8_t bits = tilt_bits_from_angles(roll, pitch, g_mp_cfg.imu_deadzone_deg);
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
    s_tilt_bits = bits;

    /* E6：倾斜视觉全本地驱动——视差条带偏移零网络往返（渲染层直读） */
    render_input_tilt(s_roll_deg);

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

    /* --- 轻拍 / 大力拍打（加速度峰值分级） --- */
    if (mag >= g_mp_cfg.tap_hard_g) {
        /* ≥4g：hit 表情 */
        mp_post_event_simple(MP_EVT_TAP_HARD, mg, 0, NULL);
        input_trigger_expression(MP_EXPR_HIT, 1500);
        note_interaction();
        return;
    } else if (mag > 1.25f && mag < g_mp_cfg.tap_light_g) {
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
/* 触摸（E6：轻点=抚摸；长按=BGM 控制条；MENU 态全部归菜单）             */
/* ================================================================== */
static void touch_tick(void)
{
    static bool down = false;
    static int16_t down_x, down_y;
    static int64_t down_ms;
    static bool longpress_fired;

    if (state_machine_menu_open()) {
        /* E6 胶水定稿：菜单是独立全屏窗口，触摸归菜单不穿透——
         * 本任务停读（渲染层 LVGL indev 接管 touch_read），只复位状态 */
        down = false;
        longpress_fired = false;
        return;
    }

    touch_sample_t t;
    if (!mp_touch_read(&t)) return;

    if (t.touched && !down) {
        down = true;
        down_x = t.x; down_y = t.y;
        down_ms = mp_now_ms();
        longpress_fired = false;
    } else if (t.touched && down) {
        if (!longpress_fired &&
            (mp_now_ms() - down_ms) >= LONGPRESS_MS &&
            abs((int)t.x - (int)down_x) < TAP_MOVE_PX &&
            abs((int)t.y - (int)down_y) < TAP_MOVE_PX) {
            /* 宠物区长按 → 呼出选择器（E6 的 BGM 控制条入口在 E7 菜单内：
             * 渲染层契约未提供独立控制条 API，长按直达菜单=BGM 入口） */
            longpress_fired = true;
            state_machine_handle(MP_SM_EV_MENU_KEY);
            note_interaction();
        }
    } else if (!t.touched && down) {
        down = false;
        int64_t dur = mp_now_ms() - down_ms;
        int dx = abs((int)t.x - (int)down_x);
        int dy = abs((int)t.y - (int)down_y);
        if (!longpress_fired && dur < TAP_MAX_MS && dx < TAP_MOVE_PX && dy < TAP_MOVE_PX) {
            /* 轻点 = 抚摸：smile/love 随机（E6） */
            mp_post_event_simple(MP_EVT_TOUCH_PET, t.x, t.y, NULL);
            input_trigger_expression((rand() % 2) ? MP_EXPR_SMILE : MP_EXPR_LOVE, 1500);
            note_interaction();
        }
    }
}

/* ================================================================== */
/* 菜单键（GPIO18，E6：任何可用物理键=菜单键）                           */
/* ================================================================== */
static void key_tick(void)
{
    static bool raw_last = false;
    static bool stable = false;
    static int64_t raw_change_ms = 0;

    bool pressed = key_gpio18_pressed();   /* 低电平=按下 */
    int64_t now = mp_now_ms();

    if (pressed != raw_last) {             /* 原始电平变化，起 30ms 防抖窗 */
        raw_last = pressed;
        raw_change_ms = now;
    }
    if ((now - raw_change_ms) >= 30 && pressed != stable) {
        stable = pressed;
        if (stable) {                      /* 按下沿：呼出/收起选择器（E7） */
            state_machine_handle(MP_SM_EV_MENU_KEY);
        }
        note_interaction();
    }
}

/* ================================================================== */
/* 慢速巡检：电池 / 温度（E10/E11）                                     */
/* ================================================================== */
static void slow_tick(int64_t idle_ms)
{
    int64_t now_s = mp_now_ms() / 1000;

    if (now_s - s_last_battery_s >= BATTERY_CHK_S) {
        s_last_battery_s = now_s;
        uint8_t pct = mp_pmu_battery_pct();
        bool charging = mp_pmu_charging();

        if (charging) {
            state_machine_handle(MP_SM_EV_BATTERY_OK);
            s_battery_low_reported = false;
        } else if (pct <= 10) {
            /* <10% 强制睡眠保电（E11） */
            mp_post_event_simple(MP_EVT_BATTERY_LOW, pct, 0, NULL);
            state_machine_handle(MP_SM_EV_BATTERY_CRIT);
        } else if (pct <= 20 && !s_battery_low_reported) {
            /* <20% 宠物打哈欠 troubled（E11）；despair 留给 BGM failover（E8） */
            s_battery_low_reported = true;
            mp_post_event_simple(MP_EVT_BATTERY_LOW, pct, 0, NULL);
            input_trigger_expression(MP_EXPR_TROUBLED, 2500);
        } else if (pct > 25) {
            s_battery_low_reported = false;
        }
    }

    if (now_s - s_last_temp_s >= TEMP_CHK_S) {
        s_last_temp_s = now_s;
        float c = mp_pmu_temp_c();
        if (c >= TEMP_HOT_C) {
            /* 过温 = hot（E10） */
            mp_post_event_simple(MP_EVT_ERROR, 1001 /* overtemp */, (int32_t)c, NULL);
            input_trigger_expression(MP_EXPR_HOT, 2000);
        }
    }

    state_machine_tick_1hz();     /* 闲置 → CLOCK_DOZE（E9） */
    expr_random_tick(idle_ms);
}

/* ================================================================== */
/* 任务入口                                                             */
/* ================================================================== */
void input_dispatch_task(void *arg)
{
    (void)arg;
    key_gpio18_init();
    srand((unsigned)mp_now_ms());
    s_last_ax = 0;

    for (;;) {
        /* IMU：双中断（GPIO17/21）唤醒采样（4.1）；20ms 超时兜底节拍
         * （超时也读一次：拿上次值更新倾斜/触摸/按键节拍，保证确定性） */
        imu_accel_t accel;
        mp_imu_wait_event(pdMS_TO_TICKS(20));
        if (mp_imu_read_accel(&accel)) {
            tilt_fsm_tick(&accel);
            force_tick(&accel);
        }

        touch_tick();
        key_tick();
        expr_fsm_tick();

        int64_t idle_ms = (s_last_interaction_ms == 0)
                          ? 0 : (mp_now_ms() - s_last_interaction_ms);
        slow_tick(idle_ms);
    }
}
