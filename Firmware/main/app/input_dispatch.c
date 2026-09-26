/**
 * input_dispatch.c — 交互分发（E6 触摸/重力/力度/按键 + E10 表情状态机）
 *
 * 采样拓扑（4.1：APP 核 input 任务）：
 *   - 触摸 touch_read() ~25ms 轮询（MENU 态停读——LVGL indev 接管，防双读）
 *   - IMU wait_event() DRDY 中断唤醒 + 20ms 超时兜底轮询（~50Hz，中断没通
 *     也不影响采样）；任务启动时读回 CTRL7 自愈使能位（见 imu_link_selfcheck）
 *   - 菜单键 key_gpio18 30ms 防抖 + 状态门闩（按下沿触发后闩住，
 *     必须先见到「释放 + 80ms 静止」才允许下一次触发，真机连发根修）
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

#include "esp_log.h"

#include "app_core.h"
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
#define BATTERY_CHK_S       60
#define TEMP_CHK_S          60
#define TEMP_HOT_C          45.0f
#define IDLE_RARE_EXPR_S    30      /* 静置时稀有表情的滚动窗口（E10） */

/* 日志节流 / 键盘硬化 */
#define KEY_RELEASE_QUIET_MS 80    /* 菜单键：确认释放后需再静止 80ms 才重新上膛（门闩） */
#define IMU_FAIL_LOG_N      40      /* IMU 连续失败 ≈1s（50Hz）→ 首报 */
#define IMU_ERR_RELOG_MS    5000    /* IMU 持续失败时每 5s 重复一条 */
#define TOUCH_FAIL_LOG_N    40      /* 触摸连续读失败 ≈1s → 首报 */
#define TOUCH_ERR_RELOG_MS  5000    /* 触摸持续失败时每 5s 重复一条 */

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
/* 触摸（E6：轻点=抚摸；水平拖动=倾斜视差（问题6）；长按=BGM 控制条；
 * MENU 态全部归菜单）                                                     */
/* ================================================================== */
/* CST9220 直读帧解析 + 480×480 映射（真机问题：按屏幕中部上报 (355,1)）。
 *
 * 根因（对照 touch_cst9220.c 的字节组装 + 真机日志实证）：本板触摸控制器
 * 属 Hynitron CST9217/9220 家族（板级 BSP waveshare__esp32_s3_touch_amoled_2_16
 * 对同料即挂 esp_lcd_touch_cst9217），真实数据帧是【12 位跨字节打包】：
 *   d0=status  d1=X[11:4]  d2=Y[11:4]  d3={X[3:0]<<4 | Y[3:0]}  d5=触点数  d6≈0xAB
 * touch_cst9220.c 旧布局假设 d1=触点数、d2..d5=XH/XL/YH/YL 独立字节：
 * 旧「Y」实际读到 d5=触点数（单指恒为 1！）；旧「X」=(Y[7:4]<<8)|
 * (X[3:0]<<4)|Y[3:0]。真机「屏幕中部 → (355,1)」逐位可解：355=0x163 即
 * Y[7:4]=1、X[3:0]=6、Y[3:0]=3；1=触点数。信息在旧解析中已丢失，任何
 * 线性映射都救不回 → 消费侧（本文件）直读原始帧按 12 位重组。
 *
 * 方向/比例照抄板级 BSP 官方默认（esp32_s3_touch_amoled_2_16.c：
 * swap_xy=1, mirror_x=0, mirror_y=1，x_max=y_max=480），换算式与
 * esp_lcd_touch get_xy_process 逐字一致：先 mirror_y（y=480-y）再 swap：
 *   screen_x = 480 - raw_y    screen_y = raw_x
 * 自校目标：屏幕中心 → (240,240)，四角 → 对应角；每次按下沿打校准日志
 * （原始帧 + 重组 raw + 映射值）供真机核对，若不符只需改下面两行映射。 */
#define TOUCH_RANGE_PX     480     /* 屏幕 480×480（profile_amoled216） */
#define TOUCH_FRAME_REG    0x00    /* 数据帧起始寄存器（同 touch_cst9220.c） */
#define TOUCH_FRAME_LEN    8       /* 多读 2 字节：d[6] 应≈0xAB（帧对齐校验） */

typedef struct {
    bool    touched;
    int16_t x, y;           /* 映射后屏幕坐标（已 clamp 到 0..479） */
    int16_t raw_x, raw_y;   /* 重组的 12 位原始值（校准日志用） */
    uint8_t count;          /* 触点数（d[5]&0x7F） */
    uint8_t d[TOUCH_FRAME_LEN];
} touch_frame_t;

static bool touch_read_frame(touch_frame_t *f)
{
    static i2c_master_dev_handle_t s_dev;   /* 懒解析（touch init 在 main 里先行） */
    static int16_t s_last_x, s_last_y;      /* 最后有效坐标：抬起帧沿用（同 esp_lcd_touch 语义） */
    static int16_t s_last_rx, s_last_ry;

    if (!s_dev) {
        s_dev = i2c_bus_find("cst9220");    /* touch_cst9220_init 注册的设备名 */
        if (!s_dev) return false;
    }
    if (i2c_bus_read_reg8v(s_dev, TOUCH_FRAME_REG, f->d, sizeof(f->d)) != ESP_OK) {
        return false;
    }

    f->count = f->d[5] & 0x7F;
    if (f->count == 0) {                    /* 抬起：坐标回填最后有效值（LVGL 同款） */
        f->touched = false;
        f->x = s_last_x;  f->y = s_last_y;
        f->raw_x = s_last_rx;  f->raw_y = s_last_ry;
        return true;
    }

    int rx = ((int)f->d[1] << 4) | (f->d[3] >> 4);      /* 12 位 X（旧驱动误当触点数） */
    int ry = ((int)f->d[2] << 4) | (f->d[3] & 0x0F);    /* 12 位 Y */
    int sx = TOUCH_RANGE_PX - ry;                       /* mirror_y 后 swap（见上注释） */
    int sy = rx;
    if (sx < 0) sx = 0;
    if (sx > TOUCH_RANGE_PX - 1) sx = TOUCH_RANGE_PX - 1;
    if (sy < 0) sy = 0;
    if (sy > TOUCH_RANGE_PX - 1) sy = TOUCH_RANGE_PX - 1;

    f->touched = true;
    f->raw_x = (int16_t)rx;  f->raw_y = (int16_t)ry;
    f->x = (int16_t)sx;      f->y = (int16_t)sy;
    s_last_x = f->x;  s_last_y = f->y;
    s_last_rx = f->raw_x;  s_last_ry = f->raw_y;
    return true;
}

static void touch_tick(void)
{
    static bool down = false;
    static int16_t down_x, down_y;
    static int64_t down_ms;
    static bool longpress_fired;
    static bool drag_active;              /* 问题6：本次按住已进入水平拖动 */
    static int fail_cnt;                  /* 触摸 I2C 连续读失败计数 */
    static int64_t fail_last_log_ms;
    static bool frame_fmt_logged;         /* 首帧字节转储（只打一次，防 count 位置翻车无据可查） */

    if (state_machine_menu_open()) {
        /* E6 胶水定稿：菜单是独立全屏窗口，触摸归菜单不穿透——
         * 本任务停读（渲染层 LVGL indev 接管 touch_read），只复位状态 */
        if (drag_active) {
            drag_active = false;
            render_input_tilt(0.0f);      /* 进菜单前先回中 */
        }
        down = false;
        longpress_fired = false;
        return;
    }

    touch_frame_t f;
    if (!touch_read_frame(&f)) {
        /* 问题3 兜底：读失败显式报错不静默（首报 ≈1s，之后每 5s 一条） */
        fail_cnt++;
        if (fail_cnt == TOUCH_FAIL_LOG_N ||
            (fail_cnt > TOUCH_FAIL_LOG_N &&
             mp_now_ms() - fail_last_log_ms >= TOUCH_ERR_RELOG_MS)) {
            fail_last_log_ms = mp_now_ms();
            ESP_LOGE(TAG, "触摸读取失败（I2C 连续 %d 次）——轻点/拖拽不可用", fail_cnt);
        }
        return;
    }
    if (fail_cnt >= TOUCH_FAIL_LOG_N) {
        ESP_LOGI(TAG, "触摸读取恢复");
    }
    fail_cnt = 0;

    if (!frame_fmt_logged) {
        frame_fmt_logged = true;
        ESP_LOGI(TAG, "触摸首帧 [%02X %02X %02X %02X %02X %02X %02X %02X] n=%d"
                      "（d[6] 期望≈0xAB，d[5]=触点数）",
                 f.d[0], f.d[1], f.d[2], f.d[3],
                 f.d[4], f.d[5], f.d[6], f.d[7], f.count);
    }

    if (f.touched && !down) {
        down = true;
        down_x = f.x; down_y = f.y;
        down_ms = mp_now_ms();
        longpress_fired = false;
        drag_active = false;
        /* 校准日志（每次按下沿一条）：原始帧 + 重组 raw + 映射值。
         * 核对目标：屏幕中心 → (240,240)、四角 → 对应角；不符时按
         * touch_read_frame 上方注释改两行映射即可 */
        ESP_LOGI(TAG, "触摸按下 raw(%d,%d)->屏幕(%d,%d) 帧[%02X %02X %02X %02X %02X %02X %02X %02X] n=%d",
                 f.raw_x, f.raw_y, f.x, f.y,
                 f.d[0], f.d[1], f.d[2], f.d[3],
                 f.d[4], f.d[5], f.d[6], f.d[7], f.count);
    } else if (f.touched && down) {
        int dx = (int)f.x - (int)down_x;
        /* 问题6：按住并水平拖动（≥TAP_MOVE_PX）→ 倾斜视差同款效果：
         * dx ±60px 线性映射 ±8°（render_input_tilt 内部再 clamp），
         * 条带视差 + 实体 ±8px 偏移与 IMU 倾斜共用一条通路 */
        if (!drag_active && !longpress_fired && abs(dx) >= TAP_MOVE_PX) {
            drag_active = true;
        }
        if (drag_active) {
            float deg = ((float)dx * 8.0f) / (float)DRAG_TILT_FULL_PX;
            render_input_tilt(deg);
            note_interaction();
            return;
        }
        if (!longpress_fired &&
            (mp_now_ms() - down_ms) >= LONGPRESS_MS &&
            abs((int)f.x - (int)down_x) < TAP_MOVE_PX &&
            abs((int)f.y - (int)down_y) < TAP_MOVE_PX) {
            /* 宠物区长按 → 呼出选择器（E6 的 BGM 控制条入口在 E7 菜单内：
             * 渲染层契约未提供独立控制条 API，长按直达菜单=BGM 入口） */
            longpress_fired = true;
            state_machine_handle(MP_SM_EV_MENU_KEY);
            note_interaction();
        }
    } else if (!f.touched && down) {
        down = false;
        int64_t dur = mp_now_ms() - down_ms;
        int dx = abs((int)f.x - (int)down_x);
        int dy = abs((int)f.y - (int)down_y);
        if (drag_active) {
            /* 问题6：拖动结束 → 视差回中 */
            drag_active = false;
            render_input_tilt(0.0f);
            note_interaction();
        } else if (!longpress_fired && dur < TAP_MAX_MS && dx < TAP_MOVE_PX && dy < TAP_MOVE_PX) {
            /* 轻点 = 抚摸：smile/love 随机 + 短气泡可见反馈（问题6）。
             * 气泡走 FONT 包渲染，字形缺失时 bridge_bubble_render 报错跳过
             * → 自动降级为只做表情 */
            mp_post_event_simple(MP_EVT_TOUCH_PET, f.x, f.y, NULL);
            input_trigger_expression((rand() % 2) ? MP_EXPR_SMILE : MP_EXPR_LOVE, 1500);
            mp_cmd_t bc = { .type = MP_CMD_BUBBLE };
            strlcpy(bc.s, "hello", sizeof(bc.s));
            mp_post_cmd(&bc);
            note_interaction();
        }
    }
}

/* ================================================================== */
/* 菜单键（GPIO18，E6：任何可用物理键=菜单键）                           */
/* ================================================================== */
/* 菜单键按下沿：POKER/OFFLINE→MENU，MENU→POKER（DOZE 下=先唤醒）。
 * 状态机内部锁竞争时会静默丢事件（handle 里 take 失败直接 return），
 * 这里用 before/after 迁移日志把「按了没反应」暴露出来（问题2）。 */
static void key_fire_menu_toggle(void)
{
    mp_state_t before = state_machine_current();
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

/* 状态门闩版 key_tick（真机问题：30ms 防抖 + 250ms 再上膛挡不住机械抖动/
 * 长按重复——抖动沿间隔 >30ms 即绕过防抖，250ms 后又能触发 → MENU/POKER
 * 以 ~420ms 间隔反复交替）。规则：
 *   1. 30ms 防抖照旧（原始电平稳定 30ms 才算确认沿）；
 *   2. 按下沿触发动作后【上闩】：之后一切按下沿全部吞掉；
 *   3. 必须先确认【释放沿】，再静止满 80ms 才开闩——期间弹回的抖动
 *      （<80ms）视为同一次按压，不产生第二次迁移。 */
static void key_tick(void)
{
    static bool raw_last = false;
    static bool stable_pressed = false;
    static int64_t raw_change_ms = 0;
    static bool latched = false;        /* 已按本次按压触发过：未见过释放前保持闩住 */
    static bool release_pending = false;/* 已确认释放沿，80ms 静止确认计时中 */
    static int64_t released_ms = 0;

    bool pressed = key_gpio18_pressed();   /* 低电平=按下 */
    int64_t now = mp_now_ms();

    if (pressed != raw_last) {             /* 原始电平变化，起 30ms 防抖窗 */
        raw_last = pressed;
        raw_change_ms = now;
    }
    if ((now - raw_change_ms) >= 30 && pressed != stable_pressed) {
        stable_pressed = pressed;
        if (stable_pressed) {
            release_pending = false;       /* 弹回按下：静止确认作废，闩继续关 */
            if (!latched) {
                latched = true;            /* 只在【按下沿】触发一次 */
                key_fire_menu_toggle();
            }
            /* 闩住期间的按下沿直接吞掉（长按/抖动不产生第二次迁移） */
        } else {
            release_pending = true;        /* 确认释放：起 80ms 静止确认 */
            released_ms = now;
        }
        note_interaction();
    }

    if (latched && release_pending && !stable_pressed &&
        (now - released_ms) >= KEY_RELEASE_QUIET_MS) {
        latched = false;                   /* 开闩：允许下一次按压触发 */
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
    (void)arg;
    key_gpio18_init();
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

        touch_tick();
        key_tick();
        expr_fsm_tick();

        int64_t idle_ms = (s_last_interaction_ms == 0)
                          ? 0 : (mp_now_ms() - s_last_interaction_ms);
        slow_tick(idle_ms);
    }
}
