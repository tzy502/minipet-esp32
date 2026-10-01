/**
 * state_machine.h — 应用状态机（software-design 4.3 / E11 / E14）
 *
 *   BOOT → 自检 → (无 WiFi 配置?) WIFI_PROVISION(SoftAP captive portal)
 *   正常态 POKER ←→ MENU（独立全屏，E6/E7）
 *   POKER --闲置 N 分钟--> CLOCK_DOZE（待机时钟，RTC 走时，任意交互回 POKER）
 *   OFFLINE（无网：TF 缓存继续跑 + BGM 静音；poller 负责自动回网）
 *   FATAL（看门狗熔断 / 自检致命失败：纯文本 + 关屏）
 *   OTA（双分区升级中；失败回滚旧分区）
 *
 * 本模块为被动模块（无专属任务）：
 *   - 输入任务 / poller / watchdog 通过 state_machine_handle(ev) 驱动
 *   - input_dispatch 任务每秒调 state_machine_tick_1hz()（闲置计时）
 *   - render 任务每帧排空 cmd_q 后调 app_cmd_dispatch()（指令落地到渲染层）
 */
#ifndef MP_STATE_MACHINE_H
#define MP_STATE_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MP_ST_BOOT = 0,
    MP_ST_SELF_TEST,
    MP_ST_WIFI_PROVISION,
    MP_ST_POKER,
    MP_ST_MENU,
    MP_ST_CLOCK_DOZE,
    MP_ST_OFFLINE,
    MP_ST_OTA,
    MP_ST_FATAL,
} mp_state_t;

typedef enum {
    MP_SM_EV_NONE = 0,
    MP_SM_EV_MENU_KEY,        /* 菜单键（GPIO18）呼出/收起 */
    MP_SM_EV_TOUCH,           /* 任意触摸交互（唤醒源） */
    MP_SM_EV_IMU,             /* 任意 IMU 交互（唤醒源） */
    MP_SM_EV_IDLE_TIMEOUT,    /* 闲置到点（tick_1hz 检出） */
    MP_SM_EV_NET_ONLINE,      /* poller 首次/恢复成功 */
    MP_SM_EV_NET_OFFLINE,     /* poller 进入退避（服务端掉线） */
    MP_SM_EV_OTA_START,       /* poller 收到升级指令 */
    MP_SM_EV_OTA_END,         /* OTA 结束（成功=重启前收尾，失败=回 POKER） */
    MP_SM_EV_BATTERY_CRIT,    /* <10% 强制睡眠保电（E11） */
    MP_SM_EV_BATTERY_OK,      /* 充电中/回到 >=10% */
    MP_SM_EV_FATAL,           /* 不可恢复故障 */
} mp_sm_event_t;

/* 基础初始化（无副作用，main 早期调用） */
void state_machine_init(void);

/* 自检 + 初始迁移（main 任务里阻塞执行，含 WiFi 连接/服务端连通） */
void state_machine_boot(bool sd_ok, bool psram_ok);

mp_state_t state_machine_current(void);
const char *state_machine_name(mp_state_t st);

/* 事件驱动迁移（含 on_enter/on_exit 切换钩子） */
void state_machine_handle(mp_sm_event_t ev);

/* 每秒节拍：闲置→DOZE、低电巡检调度入口 */
void state_machine_tick_1hz(void);

/* 任意交互都刷新（唤醒 + 重置闲置计时）；唤醒 DOZE 时回 POKER */
void state_machine_notify_activity(void);

/* MENU 开关状态查询（input_dispatch 据此决定触摸归属） */
bool state_machine_menu_open(void);

/* 是否处于「无网降级」运行（POKER/OFFLINE/DOZE 下均可能） */
bool state_machine_offline_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_STATE_MACHINE_H */
