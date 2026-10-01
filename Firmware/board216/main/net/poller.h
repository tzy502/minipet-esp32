/**
 * poller.h — 长轮询（GET /api/device/poll?since=，E2）+ 指数退避（E11）
 *
 * 任务（PRO 核 4.1 http_poll）：
 *   GET poll?since=<cursor>（服务端 hold ~50s）→
 *     {"rev":N, "cmds":[{"t":"action","v":"walk1"}, ...]}
 *   成功：cmds → cmd_q / audio_q / ota；退避复位；rev 变化 → asset_dl 同步
 *   失败：1s→2s→4s→…→60s 封顶；WiFi 掉线时先重连 STA
 *   网络状态变迁 → 状态机 NET_ONLINE / NET_OFFLINE（E11 自动回网）
 *
 * 指令类型（t 字段）：
 *   action / expression / bubble / map(hash) / brightness(0..100) /
 *   reboot / bgm(play|pause|resume|stop|next|prev|vol, n=曲号或音量) /
 *   ota(v=新版本号, u=bin URL)
 */
#ifndef MP_POLLER_H
#define MP_POLLER_H

#ifdef __cplusplus
extern "C" {
#endif

/* 创建长轮询任务（PRO 核；main.c 启动） */
void poller_start(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_POLLER_H */
