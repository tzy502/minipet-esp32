/**
 * @file mpconf.h
 * @brief 串口配置通道（UART0）：MPCONF WIFI/SRV/SHOW/CLEAR/REBOOT
 *
 * 独立小任务 + 非阻塞单字节读，绝不阻塞日志与业务任务。
 * 在 app_main 里调用 mpconf_start() 启动（幂等）。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 启动串口配置通道（失败仅告警，不影响主流程） */
void mpconf_start(void);

#ifdef __cplusplus
}
#endif
