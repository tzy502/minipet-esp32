/*
 * tools/stubs/watchdog.h — host 侧编译 mpak.c 用的 watchdog 替身。
 * 计数而非真喂狗：单测据此断言"瓦片整块读循环里确实调了 watchdog_kick()"
 * （真机上渲染任务 5s 不喂 = E14 三振熔断黑屏，是历史事故）。
 */
#ifndef HOST_STUB_WATCHDOG_H
#define HOST_STUB_WATCHDOG_H

extern unsigned g_test_watchdog_kicks;

static inline void watchdog_kick(void) { g_test_watchdog_kicks++; }

#endif /* HOST_STUB_WATCHDOG_H */
