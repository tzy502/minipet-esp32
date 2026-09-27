/**
 * logbuf.c — 设备端环形日志缓冲（E14）
 *
 * 三条硬约束（全部来自真机现状，不是设计偏好）：
 *   1) 缓冲必须放 PSRAM：本板内部动态堆仅 ~112KB 且碎片化（已有任务反复
 *      创建失败，见 provision.c dump_internal_heap 注释），任何内部堆新增
 *      都可能把 render/poller 挤出局。故 s_ring = heap_caps_malloc(..,
 *      MALLOC_CAP_SPIRAM)，内部堆占用 = 0。
 *   2) 不阻塞、不递归：本函数挂在 esp_log 输出路径上，任何时刻都可能被任意
 *      任务（含高优先级 render/input）调用。故
 *        - 重入保护用 __atomic_test_and_set（无阻塞语义），命中即直通串口；
 *        - 写入过程绝不打日志（只有内存写入 + 环形 memcpy）；
 *        - 中断上下文直接跳过（PSRAM 在 ISR 里不可用，且不能持锁）；
 *        - 栈水位过低直接跳过（vsnprintf 需栈，宁可少记录也不能溢出）。
 *   3) 不新建任务：写入路径零任务；上报挂在已在跑的 poller 任务里
 *      （net/http_client.c 的 mp_http_device_log_step）。本模块自身不发起任何
 *      网络动作。
 *
 * 记录格式（按行）：
 *   [uint32 seq][uint32 t_ms][uint64 ts_ms][uint8 lvl][uint8 tag_len]
 *   [uint8 msg_len][uint8 rsv][char tag[tag_len]][char msg[msg_len]]
 *   定长头 20B + 按需体；整体越环尾则回绕到 0（读侧用大小累加直接跳过）。
 *
 * 并发模型：单写者（日志写入经由原子门串行化）+ 多读者（scan_start 里把
 * 写入者挡在环外，见 LOG_BARRIER_MAX_MS）。写入者永不等待（try-lock），
 * 读者最多等 LOG_BARRIER_MAX_MS（poller 侧一次上报 4s 超时，留足余量）。
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
#define LB_RING_BYTES       32768u   /* PSRAM，内部堆 0 占用 */
#define LB_MSG_MAX          160u     /* 单行文本上限（超出截断，保头留尾信息） */
#define LB_TAG_MAX          15u      /* 存进记录里的 tag 长度上限 */
#define LB_MTU              (20u + LB_TAG_MAX + 1u + LB_MSG_MAX)

#define LB_STACK_MIN_WORDS  1200u    /* 低于此栈余量不写环（vsnprintf 需栈） */
#define LB_BARRIER_MAX_MS   8000     /* 读侧等待写入者让路的硬上限 */

/* ------------------------------------------------------------------ */
/* 状态（全部静态；无内部堆分配）                                         */
/* ------------------------------------------------------------------ */
static uint8_t  *s_ring;
static uint32_t  s_cap;
static uint32_t  s_head;        /* 写入偏移 */
static uint32_t  s_seq;         /* 已写入记录数（= 最新序号） */
static uint32_t  s_oldest;      /* 环内最旧记录的序号（丢弃时递增） */
static vprintf_like_t s_orig_vprintf;
static volatile bool s_read_hold;          /* 读者持锁：写入者让路 */
static volatile uint32_t s_read_hold_ms;
static volatile bool s_err_seen;           /* 出现过 E 级日志（上报端可提前拉取） */
static volatile bool s_in_hook;            /* 重入门（原子） */

static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

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
/* 读取屏障（读侧独占，写侧 try-lock）                                    */
/* ------------------------------------------------------------------ */
static bool read_lock(void)
{
    s_read_hold_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_read_hold = true;
    /* 等正在写入的那一条收尾（写入极短：一次 memcpy 级）。超时则照常读，
     * 只可能读到半条记录，由长度校验丢弃 —— 绝不阻塞读者。 */
    for (int i = 0; i < 200; i++) {
        if (!s_in_hook) return true;
        vTaskDelay(1);
    }
    return false;
}

static void read_unlock(void) { s_read_hold = false; }

/* ------------------------------------------------------------------ */
/* 写入                                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    char lvl;
    char tag[LB_TAG_MAX + 1];
    const char *msg;
    int  msg_len;
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
    o->msg = "";

    /* 时间戳（"I (12345) tag: ..." 之后）→ 级别括号 */
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
            /* tag 到第一个 ':' 或空白为止 */
            size_t ti = 0;
            while (body[ti] && body[ti] != ':' && body[ti] != ' ' && ti < LB_TAG_MAX) {
                o->tag[ti] = body[ti];
                ti++;
            }
            o->tag[ti] = 0;
            char *m = strchr(body, ':');
            o->msg = m ? m + 1 : body;
        } else {
            o->msg = rp ? rp : pl;
        }
    } else {
        o->msg = p;                     /* 非标准行（printf 直出）：原样存 */
    }

    while (*o->msg == ' ') o->msg++;
    int n = (int)strlen(o->msg);
    while (n > 0 && (o->msg[n - 1] == '\n' || o->msg[n - 1] == '\r' ||
                     o->msg[n - 1] == ' ' || o->msg[n - 1] == '\x1b')) {
        n--;
    }
    if (n <= 0) return false;
    if ((size_t)n > LB_MSG_MAX) n = (int)LB_MSG_MAX;
    o->msg_len = n;
    return true;
}

static void ring_write(const char *fmt, va_list ap)
{
    if (!s_ring || s_read_hold) return;        /* 未初始化 / 读者独占 */
    if (xPortInIsrContext()) return;           /* ISR：PSRAM 不可用，不记录 */
    if (uxTaskGetStackHighWaterMark(NULL) < LB_STACK_MIN_WORDS) return;

    char line[384];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    line[n] = 0;

    lb_parsed_t pr;
    if (!parse_line(line, &pr)) return;

    uint32_t need = 20u + (uint32_t)strlen(pr.tag) + (uint32_t)pr.msg_len;
    if (need > s_cap) return;                  /* 理论不可达（MTU 远小于 cap） */
    if (need > LB_MTU) return;

    uint32_t seq = s_seq;
    uint64_t ts  = now_epoch_ms();
    uint32_t tms = esp_log_timestamp();

    /* 需要腾空间：丢最旧（可能丢多条 —— 单条接近满环时） */
    uint32_t guard = 0;
    while ((s_cap - (s_head - s_oldest)) < need) {
        if (s_oldest >= s_head) { s_head = 0; s_oldest = seq; break; }  /* 环空 */
        uint32_t o = s_oldest % s_cap;
        uint32_t olen = 20u + s_ring[o + 16] + s_ring[o + 17];
        if (olen == 0 || olen > LB_MTU) { s_head = 0; s_oldest = seq; break; }
        s_oldest++;
        if (++guard > 4096) { s_head = 0; s_oldest = seq; break; }
    }

    uint32_t off = s_head % s_cap;
    uint8_t hdr[20];
    wr32(hdr + 0,  seq);
    wr32(hdr + 4,  tms);
    wr64(hdr + 8,  ts);
    hdr[16] = (uint8_t)pr.lvl;
    hdr[17] = (uint8_t)strlen(pr.tag);
    hdr[18] = (uint8_t)pr.msg_len;
    hdr[19] = 0;

    /* 环内可能跨界：先量尾部空间，再回绕。两段写，无中间状态被读者
     * 认为「完整」—— 完整性由 seq 发布保证（先体后头）。 */
    uint32_t tail = s_cap - off;
    if (tail >= need) {
        memcpy(s_ring + off + 20, pr.tag, hdr[17]);
        memcpy(s_ring + off + 20 + hdr[17], pr.msg, pr.msg_len);
        wr32(s_ring + off + 0, seq);
        wr32(s_ring + off + 4, tms);
        wr64(s_ring + off + 8, ts);
        s_ring[off + 16] = hdr[16];
        s_ring[off + 17] = hdr[17];
        s_ring[off + 18] = hdr[18];
        s_ring[off + 19] = 0;
        s_head = off + need;
    } else {
        uint32_t p = 0;
        memcpy(s_ring + off + 20, pr.tag, hdr[17]);
        memcpy(s_ring + off + 20 + hdr[17], pr.msg, hdr[18]);
        p = 20 + hdr[17] + hdr[18];
        if (p < need) {
            memcpy(s_ring, pr.tag, hdr[17]);              /* 回绕：整条重写（简单且原子） */
            memcpy(s_ring + 20, pr.msg, hdr[18]);
        }
        wr32(s_ring + 0, seq);
        wr32(s_ring + 4, tms);
        wr64(s_ring + 8, ts);
        s_ring[16] = hdr[16];
        s_ring[17] = hdr[17];
        s_ring[18] = hdr[18];
        s_ring[19] = 0;
        s_head = need;
    }

    s_seq = seq + 1;
    if (pr.lvl == 'E') s_err_seen = true;
}

int logbuf_vprintf_hook(const char *fmt, va_list ap)
{
    /* 先做记录侧（va_list 在 ring_write 里被消费，故串口输出用 va_copy） */
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
    if (s_ring) return true;
    if (s_orig_vprintf) return true;           /* 已挂过（PSRAM 失败降级） */

    uint8_t *p = (uint8_t *)heap_caps_malloc(LB_RING_BYTES,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        /* 不阻塞启动：只打一条 WARN，功能降级为纯串口日志 */
        s_orig_vprintf = esp_log_set_vprintf(logbuf_vprintf_hook);
        ESP_LOGW("logbuf", "PSRAM 环形缓冲分配失败（%u B）→ 设备日志上报不可用",
                 (unsigned)LB_RING_BYTES);
        return false;
    }
    s_ring = p;
    s_cap  = LB_RING_BYTES;
    s_head = 0;
    s_seq  = 0;
    s_oldest = 0;
    memset(s_ring, 0, s_cap);
    s_orig_vprintf = esp_log_set_vprintf(logbuf_vprintf_hook);
    return true;
}

bool logbuf_ready(void) { return s_ring != NULL; }
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
/* 读取                                                                */
/* ------------------------------------------------------------------ */
/** 从序号 s 的记录起始偏移（从新到旧扫描，命中即返回）。 */
static bool find_off(uint32_t s, uint32_t *off_out)
{
    if (s < s_oldest || s >= s_seq) return false;
    uint32_t off = s_head;
    for (uint32_t i = 0; i < 8192; i++) {
        if (off == 0) off = s_cap;
        off -= 20;
        uint32_t olen = 20u + s_ring[off + 17] + s_ring[off + 18];
        if (olen == 0 || olen > LB_MTU) return false;
        uint32_t rseq = rd32(s_ring + off);
        if (rseq == s) { *off_out = off; return true; }
        if (off < olen) break;                 /* 跨环尾，无法继续回退 */
        off -= olen;
    }
    return false;
}

bool logbuf_scan_start(logbuf_iter_t *it, uint32_t since_seq)
{
    memset(it, 0, sizeof(*it));
    if (!s_ring) return false;
    read_lock();
    it->seq_snap = s_seq;
    it->t_start  = since_seq + 1;
    if (it->t_start < s_oldest) it->t_start = s_oldest;   /* 已被覆盖：从最旧开始 */
    uint32_t off;
    if (!find_off(it->t_start, &off)) { it->done = true; return false; }
    it->cursor = off;
    return true;
}

bool logbuf_scan_next(logbuf_iter_t *it, logbuf_rec_t *out)
{
    if (!it || it->done || !s_ring) return false;
    for (;;) {
        uint32_t off = it->cursor;
        if (off >= s_cap) off = 0;
        uint32_t olen = 20u + s_ring[off + 17] + s_ring[off + 18];
        if (olen == 0 || olen > LB_MTU || s_seq == 0) { it->done = true; return false; }
        uint32_t rseq = rd32(s_ring + off);
        if (rseq >= it->seq_snap) { it->done = true; return false; }

        uint32_t tag_len = s_ring[off + 17];
        uint32_t msg_len = s_ring[off + 18];
        if (off + 19u + tag_len + msg_len > s_cap) { it->done = true; return false; }

        out->seq    = rseq;
        out->t_ms   = rd32(s_ring + off + 4);
        out->ts_ms  = rd64(s_ring + off + 8);
        out->lvl    = (char)s_ring[off + 16];
        uint32_t tl = tag_len > sizeof(out->tag) - 1 ? (uint32_t)sizeof(out->tag) - 1 : tag_len;
        memcpy(out->tag, s_ring + off + 20, tl);
        out->tag[tl] = 0;
        out->msg    = (const char *)(s_ring + off + 20 + tag_len);

        it->cursor = off + 20u + tag_len + msg_len;
        if (it->cursor >= s_cap) it->cursor -= s_cap;
        return true;
    }
}

void logbuf_scan_end(logbuf_iter_t *it)
{
    if (it) it->done = true;
    read_unlock();
}

size_t logbuf_dump_since(uint32_t since_seq, char *out, size_t cap)
{
    if (!out || cap < 2) return 0;
    out[0] = 0;
    if (!s_ring) return 0;
    size_t used = 0;
    logbuf_iter_t it;
    if (!logbuf_scan_start(&it, since_seq)) { logbuf_scan_end(&it); return 0; }
    logbuf_rec_t r;
    while (logbuf_scan_next(&it, &r)) {
        /* msg 非 NUL 结尾（指向环内），用 snprintf 精度截断 */
        int w = snprintf(out + used, cap - used, "%lu %c %s: %.160s\n",
                         (unsigned long)r.seq, r.lvl, r.tag, r.msg);
        if (w <= 0) break;
        if ((size_t)w >= cap - used) { used = cap - 1; break; }
        used += (size_t)w;
    }
    logbuf_scan_end(&it);
    out[used] = 0;
    return used;
}
