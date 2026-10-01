/**
 * logbuf.h — 设备端环形日志缓冲（E14）
 *
 * 需求原文（E14）：「日志：设备环形日志缓冲，Web 可拉取（排障不用插线）；
 * 串口日志仅开发期」。
 *
 * 设计（约束来自本板真机现状，见 logbuf.c 头注释）：
 *   - 缓冲在 PSRAM（MALLOC_CAP_SPIRAM），内部堆零占用（本板内部堆仅 ~112KB
 *     且碎片化，已有任务反复创建失败）
 *   - 经 esp_log_set_vprintf() 挂接：日志「同时」进串口与环（不改既有输出）
 *   - 只读快照接口（迭代器）不分配内存，上报方自行拼 JSON
 *
 * 读取/上报分工：
 *   - 本模块只负责「存」与「读」
 *   - 上报（POST /api/device/log）在 net/http_client.c（mp_http_device_log_step），
 *     由已在跑的 poller 任务周期调用 —— 不新建任务
 */
#ifndef MP_LOGBUF_H
#define MP_LOGBUF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#include "esp_log_level.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化：分配 PSRAM 环形缓冲并挂接 esp_log vprintf（幂等）。
 *  必须在任何网络请求前调用一次（main.c app_main_task 开头）。
 *  返回 false = PSRAM 分配失败（功能降级为「只有串口日志」，不影响启动）。 */
bool logbuf_init(void);

/** vprintf 风格写入点（供 esp_log_set_vprintf 使用；默认在 logbuf_init 里挂上）。
 *  - 不阻塞（原子 test-and-set 防重入；ESP-IDF 日志层自身已串行化）
 *  - 不递归（写入过程自身绝不打日志）
 *  - 缓冲满覆盖最旧（环形） */
int logbuf_vprintf_hook(const char *fmt, va_list ap);

/** 缓冲是否可用（PSRAM 分配成功） */
bool logbuf_ready(void);

/** 当前已写入记录数（= 最新序号；0 表示还没有任何日志） */
uint32_t logbuf_seq(void);

/** 自上次调用以来是否出现过 E 级日志（取用即清）。上报端据此提前拉取。 */
bool logbuf_take_err_flag(void);

/* ------------------------------------------------------------------ */
/* 读取：只读迭代器（不分配内存，上报方边遍历边拼 JSON）                    */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t seq;
    uint32_t t_ms;      /* 设备毫秒（esp_log_timestamp 口径，开机起算） */
    uint64_t ts_ms;     /* epoch 毫秒（系统时间未校准时为开机毫秒的近似） */
    char     lvl;       /* 'E' / 'W' / 'I' / 'D' / 'V'（缺省 'I'） */
    char     tag[16];
    const char *msg;    /* 行文本（无换行），仅在 next() 返回到下次 next() 前有效 */
} logbuf_rec_t;

typedef struct {
    uint32_t cursor;    /* 遍历游标（内部） */
    uint32_t seq_snap;  /* 快照：写入序号上限，遍历期间新写入的记录不参与 */
    uint32_t t_start;   /* 快照：起始序号 */
    bool     done;
} logbuf_iter_t;

/** 开始遍历「序号 > since_seq」的记录。必须在同一线程内 next→扫描→end 成对调用。 */
bool logbuf_scan_start(logbuf_iter_t *it, uint32_t since_seq);

/** 取下一条；返回 false = 结束（游标跨过快照或环内已无更旧记录）。 */
bool logbuf_scan_next(logbuf_iter_t *it, logbuf_rec_t *out);

/** 结束遍历（解除保护；必须与 scan_start 成对，否则阻塞日志写入） */
void logbuf_scan_end(logbuf_iter_t *it);

/** 按行 Dump（序号 > since_seq 的记录，每行以 '\n' 结尾），供真机自检/调试调用。
 *  返回写入 out 的字节数（不含结尾 NUL；out 恒 NUL 结尾）。 */
size_t logbuf_dump_since(uint32_t since_seq, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* MP_LOGBUF_H */
