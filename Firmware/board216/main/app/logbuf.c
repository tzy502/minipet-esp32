/**
 * logbuf.c — 设备端环形日志缓冲（E14）
 *
 * 三条硬约束（全部来自真机现状，不是设计偏好）：
 *   1) 缓冲必须放 PSRAM：本板内部动态堆仅 ~112KB 且碎片化（已有任务反复
 *      创建失败，见 provision.c dump_internal_heap 注释），任何内部堆新增
 *      都可能把 render/poller 挤出局。故 s_slot = heap_caps_malloc(..,
 *      MALLOC_CAP_SPIRAM)，内部堆常驻占用 = 0。
 *   2) 不阻塞、不递归：本函数挂在 esp_log 输出路径上，任何时刻都可能被任意
 *      任务（含高优先级 render/input）调用。故
 *        - 重入保护用 __atomic_test_and_set（无阻塞语义），命中即直通串口；
 *        - 写入过程绝不打日志（只有内存写入 + 定长结构赋值）；
 *        - 中断上下文直接跳过（PSRAM 在 ISR 里不可用，且不能持锁）；
 *        - 栈水位过低直接跳过（vsnprintf 需栈，宁可少记录也不能溢出）。
 *   3) 不新建任务：写入路径零任务；上报挂在已在跑的 poller 任务里
 *      （net/http_client.c 的 mp_http_device_log_step）。本模块不发起网络动作。
 *
 * 数据结构：【定长槽位环】而不是变长记录环。
 *   槽 = s_log_slot_t（24B 头 + 16B tag + 168B 文本 + 对齐填充 = 216B），
 *   环 = 152 槽 ≈ 32.8KB（PSRAM，见 LB_SLOTS / sizeof 断言在主机 harness）。
 *   为什么定长：变长记录只有两种选择，都有坑 ——
 *     ① 允许跨环尾：读者定位要处理拆分，复杂且易错；
 *     ② 回绕时留尾部空洞：逻辑偏移 ≠ 物理偏移，多圈后漂移，实测出现
 *        「最新记录被当旧记录覆盖、dump 取不到最新」。
 *   定长槽用「序号 → 槽下标」直接映射（O(1)），读写都在单槽内完成，
 *   上述两种病都不存在（主机 harness 2000 条压力测试逐条校验序号连续）。
 *
 * 并发模型：单写者（日志经原子门串行化）+ 读者独占（scan 期间置 s_read_hold，
 * 写入者见之让路，绝不阻塞写者）。
 */
#include "logbuf.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_log_write.h"
#include "esp_timer.h"

/* ------------------------------------------------------------------ */
/* 配置                                                                */
/* ------------------------------------------------------------------ */
#define LB_SLOTS       152u     /* 槽数：152 × sizeof(slot)=216B ≈ 32.8KB（PSRAM） */
#define LB_TEXT_MAX    168u     /* 单条文本上限（超出截断，保头） */
#define LB_TAG_MAX     15u

#define LB_STACK_MIN_WORDS  1200u   /* 低于此栈余量不写环（vsnprintf 需栈） */

/* 定长槽：写入者先填负载、最后发布 seq（读者见 seq 匹配才认为槽有效） */
typedef struct {
    uint32_t seq;                   /* 0 = 空槽；否则单调递增序号（从 1 起） */
    uint32_t t_ms;                  /* 设备毫秒（esp_log_timestamp 口径） */
    uint64_t ts_ms;                 /* epoch 毫秒（未校时退化为开机毫秒） */
    uint8_t  lvl;                   /* 'E'/'W'/'I'/'D'/'V' */
    uint8_t  tag_len;
    uint8_t  text_len;              /* 不含 NUL */
    uint8_t  rsv;
    char     tag[16];
    char     text[LB_TEXT_MAX + 8];
} s_log_slot_t;

/* ------------------------------------------------------------------ */
/* 状态（全部静态；内部动态堆零占用）                                     */
/* ------------------------------------------------------------------ */
static s_log_slot_t *s_slot;        /* PSRAM：LB_SLOTS 个槽 */
static uint32_t  s_seq;             /* 已写入记录数（= 最新序号；0 = 无） */
static uint32_t  s_head;            /* 下一个待写槽下标（物理，0..LB_SLOTS-1） */
static vprintf_like_t s_orig_vprintf;
static volatile bool s_read_hold;   /* 读者独占：写入者让路 */
static volatile bool s_err_seen;    /* 出现过 E 级日志（上报端可提前拉取） */
static volatile bool s_in_hook;     /* 重入门（原子） */

/** 环内最旧序号（0 = 空环）。槽数固定，最旧 = 最新 - (LB_SLOTS-1) */
static uint32_t oldest_seq(void)
{
    if (s_seq == 0) return 0;
    return (s_seq > LB_SLOTS) ? (s_seq - LB_SLOTS + 1u) : 1u;
}

/** 当前 epoch 毫秒（系统时间未校准 → 退化为开机毫秒，仍单调可用） */
static uint64_t now_epoch_ms(void)
{
    int64_t us = esp_timer_get_time();
    time_t  s  = time(NULL);
    if (s > 1700000000) {                       /* 已校时（>=2023）：epoch 口径 */
        return (uint64_t)s * 1000ull + (uint64_t)((us % 1000000) / 1000);
    }
    return (uint64_t)(us / 1000);               /* 未校时：退化，仅保序 */
}

/* ------------------------------------------------------------------ */
/* 写入                                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    char lvl;
    char tag[16];
    const char *text;
    int  text_len;
} lb_parsed_t;

/** 解析一条已格式化日志行：`<ts> <L> (<t_ms>) tag: msg`
 *  返回 false = 空行/纯换行（不记录）。 */
static bool parse_line(char *buf, lb_parsed_t *o)
{
    char *p = buf;
    while (*p == '\r' || *p == '\n' || *p == ' ') p++;
    if (!*p) return false;

    o->lvl = 'I';
    o->tag[0] = 0;
    o->text = "";

    /* 时间戳之后是级别括号："12 I (12) tag: msg" */
    char *pl = strchr(p, '(');
    if (pl && pl > p && (pl[-1] == ' ' || pl[-1] == '\t')) {
        char lv = '\0';
        for (char *q = p; q < pl - 1; q++) {
            if (*q == 'E' || *q == 'W' || *q == 'I' || *q == 'D' || *q == 'V') lv = *q;
        }
        if (!lv && (pl[-1] == 'E' || pl[-1] == 'W' || pl[-1] == 'I' ||
                    pl[-1] == 'D' || pl[-1] == 'V')) {
            lv = pl[-1];
        }
        if (lv) o->lvl = lv;

        char *rp = strchr(pl, ')');
        if (rp) {
            char *body = rp + 1;
            while (*body == ' ') body++;
            size_t ti = 0;                      /* tag 到第一个 ':' 或空白为止 */
            while (body[ti] && body[ti] != ':' && body[ti] != ' ' && ti < LB_TAG_MAX) {
                o->tag[ti] = body[ti];
                ti++;
            }
            o->tag[ti] = 0;
            char *m = strchr(body, ':');
            o->text = m ? m + 1 : body;
        } else {
            o->text = pl;
        }
    } else {
        o->text = p;                    /* 非标准行（printf 直出）：原样存 */
    }

    while (*o->text == ' ') o->text++;
    int n = (int)strlen(o->text);
    while (n > 0 && (o->text[n - 1] == '\n' || o->text[n - 1] == '\r' ||
                     o->text[n - 1] == ' ' || o->text[n - 1] == '\x1b')) {
        n--;
    }
    if (n <= 0) return false;
    if ((size_t)n > LB_TEXT_MAX) n = (int)LB_TEXT_MAX;
    o->text_len = n;
    return true;
}

static void ring_write(const char *fmt, va_list ap)
{
    if (!s_slot || s_read_hold) return;        /* 未初始化 / 读者独占 */
    if (xPortInIsrContext()) return;           /* ISR：PSRAM 不可用，不记录 */
    if (uxTaskGetStackHighWaterMark(NULL) < LB_STACK_MIN_WORDS) return;

    char line[384];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    line[n] = 0;

    lb_parsed_t pr;
    if (!parse_line(line, &pr)) return;

    uint32_t idx = s_head % LB_SLOTS;
    s_log_slot_t *sl = &s_slot[idx];

    /* 先填负载、最后发布 seq：读者见 seq == 目标序号 才认这条有效 */
    sl->seq      = 0;                          /* 逻辑上先失效（防读者读到半条） */
    sl->t_ms     = esp_log_timestamp();
    sl->ts_ms    = now_epoch_ms();
    sl->lvl      = (uint8_t)pr.lvl;
    sl->tag_len  = (uint8_t)strlen(pr.tag);
    sl->text_len = (uint8_t)pr.text_len;
    sl->rsv      = 0;
    memcpy(sl->tag, pr.tag, sl->tag_len);
    sl->tag[sl->tag_len] = 0;
    memcpy(sl->text, pr.text, (size_t)pr.text_len);
    sl->text[pr.text_len] = 0;

    /* 序号从 1 起（0 是「空槽」哨兵：logbuf_seq()==0 与 find_slot 判定都靠它） */
    sl->seq = s_seq + 1;                       /* 发布 */
    s_seq++;
    s_head = (idx + 1u) % LB_SLOTS;

    if (pr.lvl == 'E') s_err_seen = true;
}

int logbuf_vprintf_hook(const char *fmt, va_list ap)
{
    /* 记录侧先做（va_list 在 ring_write 内被消费，故串口输出用 va_copy） */
    if (__atomic_test_and_set(&s_in_hook, __ATOMIC_ACQ_REL)) {
        /* 重入（极端：日志调用嵌套）→ 只走串口 */
        return s_orig_vprintf ? s_orig_vprintf(fmt, ap) : vprintf(fmt, ap);
    }
    va_list cp;
    va_copy(cp, ap);
    ring_write(fmt, cp);
    va_end(cp);
    __atomic_clear(&s_in_hook, __ATOMIC_RELEASE);

    return s_orig_vprintf ? s_orig_vprintf(fmt, ap) : vprintf(fmt, ap);
}

bool logbuf_init(void)
{
    if (s_orig_vprintf) return true;           /* 已挂过（含 PSRAM 失败降级） */
    if (s_slot) return true;

    s_log_slot_t *p = (s_log_slot_t *)heap_caps_malloc(
        (size_t)LB_SLOTS * sizeof(s_log_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        memset(p, 0, (size_t)LB_SLOTS * sizeof(s_log_slot_t));   /* seq=0 → 全空槽 */
        s_slot = p;
        s_seq = 0;
        s_head = 0;
    }
    s_orig_vprintf = esp_log_set_vprintf(logbuf_vprintf_hook);   /* 成败都挂（失败=纯串口） */
    if (!p) {
        ESP_LOGW("logbuf", "PSRAM 环形日志缓冲分配失败（%u B）→ 仅串口日志",
                 (unsigned)(LB_SLOTS * sizeof(s_log_slot_t)));
        return false;
    }
    return true;
}

bool logbuf_ready(void) { return s_slot != NULL; }
uint32_t logbuf_seq(void) { return s_seq; }

/* E 级日志出现过（上报端据此跳过心跳间隔，尽快把故障日志送出去）。
 * 取用即清：读一次清一次，避免同一批错误被反复触发即时上报。 */
bool logbuf_take_err_flag(void)
{
    if (!s_err_seen) return false;
    s_err_seen = false;
    return true;
}

/* ------------------------------------------------------------------ */
/* 读取：序号 → 槽（O(1)），无需顺序扫描                                  */
/* ------------------------------------------------------------------ */
/** 序号 seq 所在槽；返回 NULL = 已被覆盖 / 还没产生 / 槽无效 */
static const s_log_slot_t *find_slot(uint32_t seq)
{
    if (seq == 0 || seq > s_seq) return NULL;
    if (seq < oldest_seq()) return NULL;
    uint32_t back = s_seq - seq;               /* 0 = 最新 */
    uint32_t idx = (s_head + LB_SLOTS - 1u - (back % LB_SLOTS)) % LB_SLOTS;
    const s_log_slot_t *sl = &s_slot[idx];
    if (sl->seq != seq) return NULL;           /* 绕圈后已落到别的序号 */
    if (sl->tag_len > LB_TAG_MAX || sl->text_len > LB_TEXT_MAX) return NULL;
    return sl;
}

bool logbuf_scan_start(logbuf_iter_t *it, uint32_t since_seq)
{
    memset(it, 0, sizeof(*it));
    if (!s_slot) return false;
    s_read_hold = true;                        /* 读者独占：写入者让路 */
    it->seq_snap = s_seq;
    it->t_start  = since_seq + 1;
    uint32_t o = oldest_seq();
    if (it->t_start < o) it->t_start = o;      /* 已被覆盖：从最旧开始 */
    if (it->t_start > it->seq_snap) { it->done = true; return false; }
    it->cursor = it->t_start;
    return true;
}

bool logbuf_scan_next(logbuf_iter_t *it, logbuf_rec_t *out)
{
    if (!it || it->done || !s_slot) return false;
    if (it->cursor > it->seq_snap) { it->done = true; return false; }
    const s_log_slot_t *sl = find_slot(it->cursor);
    if (!sl) { it->done = true; return false; }  /* 断档（被覆盖）：停止，不跳号 */
    out->seq    = sl->seq;
    out->t_ms   = sl->t_ms;
    out->ts_ms  = sl->ts_ms;
    out->lvl    = (char)sl->lvl;
    memcpy(out->tag, sl->tag, sizeof(out->tag));
    out->tag[sizeof(out->tag) - 1] = 0;
    out->msg    = sl->text;
    it->cursor++;
    return true;
}

void logbuf_scan_end(logbuf_iter_t *it)
{
    if (it) it->done = true;
    s_read_hold = false;
}

size_t logbuf_dump_since(uint32_t since_seq, char *out, size_t cap)
{
    if (!out || cap < 2) return 0;
    out[0] = 0;
    if (!s_slot) return 0;
    size_t used = 0;
    logbuf_iter_t it;
    if (!logbuf_scan_start(&it, since_seq)) { logbuf_scan_end(&it); return 0; }
    logbuf_rec_t r;
    while (logbuf_scan_next(&it, &r)) {
        int w = snprintf(out + used, cap - used, "%lu %c %s: %s\n",
                         (unsigned long)r.seq, r.lvl, r.tag, r.msg);
        if (w <= 0) break;
        if ((size_t)w >= cap - used) { used = cap - 1; break; }
        used += (size_t)w;
    }
    logbuf_scan_end(&it);
    out[used] = 0;
    return used;
}
