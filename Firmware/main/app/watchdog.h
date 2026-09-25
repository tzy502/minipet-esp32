/**
 * watchdog.h — 渲染心跳看门狗 + 三振熔断（E14）
 *
 * 规则（E14 定稿）：
 *   - 渲染任务每帧喂 esp_task_wdt（render 任务内 esp_task_wdt_add + 本模块 kick）
 *   - 连续 3 次超时（跨重启累计，NVS 持久化）仍卡死 → 停止渲染，
 *     显示纯文本错误（不走渲染路径）后关屏待机
 *   - 稳定运行 10 分钟 → 计数清零
 */
#ifndef MP_WATCHDOG_H
#define MP_WATCHDOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* main.c 启动即调用：TWDT 初始化 + 监控任务 */
void watchdog_init(void);

/* 渲染任务启动后调用一次（在自身上下文里订阅 TWDT） */
void watchdog_subscribe_render_task(void);

/* 渲染任务每帧调用：喂 esp_task_wdt + 更新软件心跳时间戳 */
void watchdog_kick(void);

/* 上电时查询：上次是否因熔断进入 FATAL（main 可直接停渲染） */
bool watchdog_was_fatal_last_boot(void);

/* 纯文本错误 + 关屏（状态机 FATAL 态复用；绝不进渲染路径）
 * line1/line2 为 ASCII 大写串（内嵌 5x7 字体只含 ASCII 可打印子集） */
void watchdog_fatal_show(const char *line1, const char *line2);

/* 常驻纯文本屏（不关屏、不挂起；用于 FATAL 态需用户引导的场景，
 * 如「无网且无缓存」——design-review：未配对+首启无网+TF 空 = 黑屏 FATAL） */
void watchdog_text_persist(const char *line1, const char *line2);

#ifdef __cplusplus
}
#endif

#endif /* MP_WATCHDOG_H */
