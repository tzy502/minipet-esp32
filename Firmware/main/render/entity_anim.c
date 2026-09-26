/*
 * entity_anim.c — 动作播放器实现
 *
 * 帧推进：delay_ms 到点进入下一帧；单次动作播完置 finished（不自行换包，
 * 由合成器回绑 standby 循环布局，即 stand1）。帧位移（move_dx/dy）随事件
 * 上报；合成器在实体画布内按帧绝对叠加（对齐桌面 +mv 语义，非屏幕累计）。
 * blink：本地定时器随机 3-8s 插播 250ms（断网可用）；仅当当前动作的
 * expression 列表含 "blink" 且当前表情不是 blink 时生效。
 *
 * 坏包防御（真机「人物完全静止」排查）：delay=0 → 50ms（mpak 解析已兜底，
 * 此处双保险）；delay > RC_FRAME_DELAY_MAX_MS（导出器上限 300ms，>10s 只能是
 * 坏数据/旧版包）→ 钳到 1s 并告警一次。bind 时输出 delay 表审计日志
 * （min/max/首 4 帧），用于真机一眼判定 delay 是否被读坏。逐帧推进走
 * ESP_LOGD（默认关闭，CONFIG_LOG_DEFAULT_LEVEL=3）。
 */
#include "entity_anim.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "anim";

#define RC_FRAME_DELAY_MAX_MS 10000u  /* 单帧 delay 合法上限；超过视为坏数据 */
#define RC_FRAME_DELAY_CLAMP_MS 1000u /* 坏 delay 钳制值 */

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
    st->delay_clamp_logged = false;
    rc_anim_kick_blink(st, now_us);

    /* delay 表审计（bind 为低频事件，INFO 一条）：真机「人物静止」时先看这行——
     * min/max 异常（0 或极大）即 delay 表损坏，正常导出为 100..300ms */
    if (lt && lt->frame_count) {
        uint32_t dmin = UINT32_MAX, dmax = 0;
        for (uint32_t i = 0; i < lt->frame_count; i++) {
            uint32_t d = lt->frames[i].delay_ms;
            if (d < dmin) dmin = d;
            if (d > dmax) dmax = d;
        }
        ESP_LOGI(TAG, "bind '%s' loop=%d frames=%" PRIu32 " exprs=%" PRIu32
                 " delay[min/max]=%" PRIu32 "/%" PRIu32 "ms d[0..3]=%" PRIu32
                 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32,
                 lt->action, (int)loop, lt->frame_count, lt->expression_count,
                 dmin, dmax,
                 lt->frames[0].delay_ms,
                 lt->frame_count > 1 ? lt->frames[1].delay_ms : 0,
                 lt->frame_count > 2 ? lt->frames[2].delay_ms : 0,
                 lt->frame_count > 3 ? lt->frames[3].delay_ms : 0);
    }
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
        uint32_t d = lt->frames[st->frame_idx].delay_ms;
        if (d == 0) d = 50u;                       /* 双保险（解析端已兜底） */
        if (d > RC_FRAME_DELAY_MAX_MS) {
            /* 坏数据防御：极大 delay 让帧永不到期（真机症状=人物完全静止） */
            if (!st->delay_clamp_logged) {
                ESP_LOGW(TAG, "frame %" PRIu32 " delay %" PRIu32
                         "ms > %" PRIu32 "ms, clamp to %" PRIu32,
                         st->frame_idx, d,
                         (uint32_t)RC_FRAME_DELAY_MAX_MS,
                         (uint32_t)RC_FRAME_DELAY_CLAMP_MS);
                st->delay_clamp_logged = true;
            }
            d = RC_FRAME_DELAY_CLAMP_MS;
        }
        int64_t deadline = st->frame_start_us + (int64_t)d * 1000;
        if (now_us < deadline) break;

        st->frame_start_us = deadline;
        uint32_t prev_idx = st->frame_idx;
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
        ESP_LOGD(TAG, "frame %" PRIu32 "->%" PRIu32 " (delay=%" PRIu32 "ms)",
                 prev_idx, st->frame_idx, d);
    }

    return changed;
}
