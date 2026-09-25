/*
 * entity_anim.c — 动作播放器实现
 *
 * 帧推进：delay_ms 到点进入下一帧；单次动作播完置 finished（不自行换包，
 * 由合成器回绑 standby 循环布局，即 stand1）。帧进入位移（move_dx/dy）
 * 随事件上报，由合成器累计到实体屏幕锚点上。
 * blink：本地定时器随机 3-8s 插播 250ms（断网可用）；仅当当前动作的
 * expression 列表含 "blink" 且当前表情不是 blink 时生效。
 */
#include "entity_anim.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "anim";

/* 在布局表情名列表中查找；返回下标或 -1 */
static int expr_lookup(const mpak_layout_t *lt, const char *name)
{
    if (!lt || !name) return -1;
    for (uint32_t i = 0; i < lt->expression_count; i++)
        if (strncmp(lt->expr_names[i], name, MPAK_NAME_LEN) == 0) return (int)i;
    return -1;
}

void rc_anim_init(rc_anim_t *st)
{
    memset(st, 0, sizeof *st);
    st->blink_idx = -1;
    rc_anim_set_expression(st, "default");
}

void rc_anim_bind(rc_anim_t *st, const mpak_layout_t *lt, bool loop,
                  bool reset_expr, int64_t now_us)
{
    st->layout        = lt;
    st->loop          = loop;
    st->frame_idx     = 0;
    st->frame_start_us = now_us;
    st->blink_on      = false;
    st->blink_idx     = expr_lookup(lt, "blink");
    if (reset_expr) {
        snprintf(st->expr_want, sizeof st->expr_want, "default");
        st->expr_cur = 0;
    }
    int want = expr_lookup(lt, st->expr_want);
    st->expr_cur = (want >= 0) ? want : 0;
    rc_anim_kick_blink(st, now_us);
}

void rc_anim_kick_blink(rc_anim_t *st, int64_t now_us)
{
    uint32_t span = RC_BLINK_MAX_MS - RC_BLINK_MIN_MS;
    st->next_blink_us = now_us +
        ((int64_t)(RC_BLINK_MIN_MS + (esp_random() % (span ? span : 1))) * 1000);
}

int rc_anim_set_expression(rc_anim_t *st, const char *name)
{
    if (!name) return -1;
    snprintf(st->expr_want, sizeof st->expr_want, "%s", name);
    int want = expr_lookup(st->layout, name);
    if (want < 0) {
        ESP_LOGW(TAG, "expression '%s' not in action '%s'", name,
                 st->layout ? st->layout->action : "(none)");
        st->expr_cur = 0;
        return -1;
    }
    st->expr_cur = want;
    return 0;
}

int32_t rc_anim_active_expr(const rc_anim_t *st)
{
    if (st->blink_on && st->blink_idx >= 0) return st->blink_idx;
    return st->expr_cur;
}

bool rc_anim_advance(rc_anim_t *st, int64_t now_us, rc_anim_ev_t *ev)
{
    bool changed = false;
    memset(ev, 0, sizeof *ev);

    if (!st->layout || st->layout->frame_count == 0) return false;

    /* ---- blink 状态机 ---- */
    if (st->blink_on) {
        if (now_us >= st->blink_until_us) {
            st->blink_on = false;
            ev->expr_changed = true;
            changed = true;
        }
    } else if (st->blink_idx >= 0 && st->expr_cur != st->blink_idx &&
               now_us >= st->next_blink_us) {
        st->blink_on       = true;
        st->blink_until_us = now_us + (int64_t)RC_BLINK_HOLD_MS * 1000;
        rc_anim_kick_blink(st, now_us);
        ev->expr_changed = true;
        changed = true;
    }

    /* ---- 帧时钟（上限 64 帧/次，防坏包拖死 tick） ---- */
    const mpak_layout_t *lt = st->layout;
    for (int guard = 0; guard < 64; guard++) {
        int64_t deadline = st->frame_start_us +
                           (int64_t)lt->frames[st->frame_idx].delay_ms * 1000;
        if (now_us < deadline) break;

        st->frame_start_us = deadline;
        st->frame_idx++;
        if (st->frame_idx >= lt->frame_count) {
            if (st->loop) {
                st->frame_idx = 0;
            } else {
                /* 单次动作播完：停在末帧，通知上层回退 stand1 */
                st->frame_idx = lt->frame_count - 1;
                ev->finished = true;
                changed = true;
                break;
            }
        }
        const mpak_frame_t *fr = &lt->frames[st->frame_idx];
        ev->frame_changed = true;
        ev->dx = fr->move_dx;
        ev->dy = fr->move_dy;
        changed = true;
    }

    return changed;
}
