/**
 * input_dispatch.h — 触摸 / IMU 力度 / 倾斜 / 按键 交互分发（E6）
 *
 * 职责：
 *   - 触摸轻点 = 抚摸（smile/love）；宠物区长按 = 呼出 BGM 半屏控制条
 *   - IMU 力度分级：<2g alert+bewildered / ≥4g hit / 摇晃 stunned / 拿起翻转 fly+oops
 *   - 倾斜状态变迁：±8° 死区 + 300ms 防抖，只报进入/退出（不上报角度流）；
 *     倾斜视觉（背景视差 + walk1/fly）全本地驱动——渲染层经 input_get_tilt() 直读
 *   - 25 表情状态机：触发型播完回 default；静置低频 wink/chu/qBlue（E10）
 *   - 菜单键（GPIO18）呼出/收起选择器（E6/E7）；MENU 态触摸归菜单（本任务停读）
 *   - 低电 / 过温巡检 → 表情（troubled / hot，E10/E11）
 *
 * 任务：APP 核（4.1 input 任务），内部 20ms 主循环 + 1s 慢速巡检。
 */
#ifndef MP_INPUT_DISPATCH_H
#define MP_INPUT_DISPATCH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 渲染层每帧调用：读当前倾斜状态（本地视差驱动，零网络往返——E6/4.2）
 * roll：绕纵轴（左负右正，度）；pitch：前后倾（上倾为正，度）；
 * bits：MP_TILT_LEFT/RIGHT/UP 已过死区+防抖的稳定态 */
void input_get_tilt(float *roll_deg, float *pitch_deg, uint8_t *bits);

/* 表情触发（供其他模块借用同一状态机：BGM hum / 素材 dam 等）。
 * duration_ms 后自动回 default（触发型播完回 default——E10）。
 * menu 打开时宠物表情被冻结，忽略。 */
void input_trigger_expression(const char *expr, uint32_t duration_ms);

/* 早期初始化（main 创建任务前调用：表情状态机跨任务互斥） */
void input_dispatch_init(void);

/* input 任务入口（main.c 创建，APP 核） */
void input_dispatch_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* MP_INPUT_DISPATCH_H */
