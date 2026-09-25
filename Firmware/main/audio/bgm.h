/**
 * bgm.h — BGM 子系统（E8）：HTTP 流 → minimp3 → PSRAM 环形缓冲 → I2S → codec
 *
 * 短路规则（E8 定稿）：
 *   - 失败只在同源内重试降级（同曲重试 2 次 → 跳下一首），
 *     禁止跨源自动换歌
 *   - 某源整体不可用（连续 3 曲失败）→ 该源入口置灰（状态机+渲染层），
 *     failover 事件上报 + despair 表情；手动切类型才换源
 *   - 断网 = 静音降级（不做本地曲库缓存）
 *
 * 控制入口：audio_q（触摸控制条 / poll 指令），控制回传 POST /api/device/bgm/cmd。
 * PA_CTRL（GPIO46）：有声才开（codec_pa_enable）。
 * 音量：线性（feeder 出口统一缩放，立即生效）。
 */
#ifndef MP_BGM_H
#define MP_BGM_H

#include <stdint.h>
#include <stdbool.h>

#include "app_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MP_BGM_IDLE = 0,     /* 无曲 */
    MP_BGM_PLAYING,
    MP_BGM_PAUSED,
    MP_BGM_FAILED,       /* 音源整体不可用（置灰，等手动切源） */
} mp_bgm_state_t;

/* 创建 bgm 任务（PRO 核）+ i2s feeder 任务 + 环形缓冲（main.c 启动） */
void bgm_start(void);

/* 网络态切换：true=离线（静音降级，停流保状态）；false=回网 */
void bgm_set_offline(bool offline);

/* 渲染层控制条回显用（hal_contract 备注） */
mp_bgm_state_t bgm_get_state(void);
mp_bgm_source_t bgm_get_source(void);
uint8_t bgm_get_volume(void);
bool bgm_source_greyed(mp_bgm_source_t src);

#ifdef __cplusplus
}
#endif

#endif /* MP_BGM_H */
