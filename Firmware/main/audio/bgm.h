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

/* ---------------- 曲目表与播放控制（任意任务上下文，经 audio_q 异步生效）--- */

/* 选曲播放：id 取自 AUDIO_META 曲目表（bgm_list 同源）。
 * true=已受理入队（离线/置灰源/id 非法拒绝）；实际起播在 bgm 任务完成。 */
bool bgm_play_id(uint32_t id);

/* 播放/暂停切换：返回切换后是否在播（true=PLAYING / false=PAUSED 或无可切换）。
 * 暂停语义=停 feeder+PA 静音+关流（见 bgm.c MP_AUDIO_PAUSE 注释）。 */
bool bgm_toggle_pause(void);

/* 下一首/上一首：本地曲目表内循环（到尾回首/到首回尾）；
 * 表不可用时回退服务端 next/prev（audio/ 目录尚无 AUDIO_META 包的兜底）。 */
void bgm_next(void);
void bgm_prev(void);

/* 列出当前源曲目（AUDIO_META 包，/sdcard/minipet/audio/<hash>.mpk）。
 * ids[i]=曲目 id；titles[i] 32B 定长（31 字节 UTF-8 边界安全截断+补 NUL）。
 * ids/titles 可为 NULL 跳过对应输出（bgm_list(NULL,NULL,INT_MAX)=表容量查询）。
 * 返回实际填充条数；0=当前源无曲目（包未同步/解析全败）。首次调用扫 TF，
 * 之后走缓存（切源/重启后重建）。 */
int bgm_list(uint32_t *ids, char titles[][32], int max);

/* 音量增减：当前音量 + delta，clamp 0..100，feeder 出口线性缩放即时生效；
 * 经 audio_q 异步落地并存 NVS 偏好 + 服务端回传。 */
void bgm_volume_add(int delta);

/* 当前音量 0..100（bgm_get_volume 的同义别名，控制条直读） */
uint8_t bgm_volume_get(void);
/* 当前曲目显示名（无表/未播放返回空串） */
const char *bgm_current_title(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_BGM_H */
