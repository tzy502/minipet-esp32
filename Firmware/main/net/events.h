/**
 * events.h — 设备事件上报（POST /api/device/event，E2/E11）
 *
 * 任务（PRO 核）：排空 event_q → 批量 JSON POST，附 TF manifest.json
 * 的本地 hash 集（服务端据此计算选择器 cached 标记——E7/R7）。
 *
 * 上报类型：触摸 / IMU 力度分级 / 倾斜状态变迁（进入/退出，不报角度流）/
 * 低电 / 错误 / 素材损坏 / BGM failover / 开机。
 * 失败即丢（有界损失，不重放——事件价值随时间衰减，队列不积压）。
 */
#ifndef MP_EVENTS_H
#define MP_EVENTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* 创建事件上报任务（PRO 核；main.c 启动） */
void events_start(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_EVENTS_H */
