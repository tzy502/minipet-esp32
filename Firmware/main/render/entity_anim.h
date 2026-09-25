/*
 * entity_anim.h — 动作播放器（LAYOUT 帧推进 / 表情切换 / blink 随机插播）
 *
 * 纯状态机：不持锁、不碰 TF/显示；由合成器在 render_tick 中驱动。
 * 约定：所有函数须与 render_tick 同任务调用（APP_CPU lvgl 任务）。
 */
#ifndef RENDER_ENTITY_ANIM_H
#define RENDER_ENTITY_ANIM_H

#include <stdint.h>
#include <stdbool.h>

#include "mpak.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RC_BLINK_MIN_MS   3000u   /* blink 间隔下限（随机 3-8s） */
#define RC_BLINK_MAX_MS   8000u   /* blink 间隔上限              */
#define RC_BLINK_HOLD_MS  250u    /* blink 持续时间               */

typedef struct {
    const mpak_layout_t *layout;  /* 当前绑定的动作布局 */
    bool     loop;               /* true=循环；false=单次（播完通知 finished 由上层回退 stand1） */
    uint32_t frame_idx;
    int64_t  frame_start_us;     /* 当前帧起始时刻 */
    /* 表情维度 */
    char     expr_want[MPAK_NAME_LEN + 1]; /* 期望表情名（跨动作切换时按名重解析） */
    int32_t  expr_cur;           /* 当前表情索引（layout->expr_names 下标） */
    int32_t  blink_idx;          /* "blink" 在 expr_names 中的下标；-1 = 无 */
    bool     blink_on;           /* blink 插播生效中 */
    int64_t  blink_until_us;
    int64_t  next_blink_us;
} rc_anim_t;

typedef struct {
    bool     frame_changed;      /* 进入新帧（dx/dy 为该帧位移，世界 1x px） */
    int16_t  dx, dy;
    bool     expr_changed;       /* 表情索引变化（blink 切换也置位） */
    bool     finished;           /* 单次动作播完（上层应回绑 loop 布局） */
} rc_anim_ev_t;

void rc_anim_init(rc_anim_t *st);
/*
 * 绑定布局：解析 expr_want/blink 索引、帧计数器清零。
 * reset_expr=false 时保留既有 expr_want 按新布局重解析。
 */
void rc_anim_bind(rc_anim_t *st, const mpak_layout_t *lt, bool loop,
                  bool reset_expr, int64_t now_us);
/* 帧时钟推进；返回 true 表示实体层内容可能变化（ev 输出事件位） */
bool rc_anim_advance(rc_anim_t *st, int64_t now_us, rc_anim_ev_t *ev);
/* 设置表情（按名）；当前布局无该表情 → 保持 default 并返回 -1 */
int  rc_anim_set_expression(rc_anim_t *st, const char *name);
/* 生效表情索引（含 blink 插播覆盖） */
int32_t rc_anim_active_expr(const rc_anim_t *st);
/* 重排 blink 定时（exit_menu 后调用，避免恢复瞬间立刻眨眼） */
void rc_anim_kick_blink(rc_anim_t *st, int64_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* RENDER_ENTITY_ANIM_H */
