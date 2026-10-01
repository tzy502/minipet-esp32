/**
 * mp_psram.h — 【内部 RAM 腾挪 2026-10-02】PSRAM 优先的分配助手
 *
 * 背景（board216 真机基线）：内部动态 DRAM 只有 ~133KB，开机"素材全绑后"
 * 空闲一度只剩 1.3KB / 最大块 820B，导致 24KB 门限踩穿 →
 * 「内部堆不足，跳过本轮素材绑定/字体装载」= 用户看到的"纸娃娃消失、中文变方框"。
 *
 * 为什么不能靠调 malloc 策略解决：sdkconfig 的
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 规定「普通 malloc 请求 ≤4KB 一律
 * 走内部 RAM」。这条**不能往下调**——FatFS 的扇区窗口恰好是 4096B
 * （CONFIG_FATFS_SECTOR_4096=y），且它必须 DMA 可及；把这个门限降到 4096 以下
 * 会让扇区窗口落到 PSRAM，SD 读写路径随之恶化。于是所有 ≤4KB 的普通 malloc
 * （cJSON 节点、静态暂存、小结构体）**全都**压在内部 DRAM 上：要省内部堆，
 * 必须逐处显式点名 MALLOC_CAP_SPIRAM。
 *
 * 本助手把"PSRAM 优先 + 失败退默认堆（内部）"这条兜底收敛成一处，
 * 失败语义与直接 malloc 完全一致（不会引入新的失败模式），只是把成功的
 * 那一支换到 PSRAM。
 *
 * ⚠️ 使用纪律（与任务书「不能回退的东西」一致）：
 *   · 只用于**不会被 SPI/SDMMC/I2S/LCD DMA 直接读**的缓冲（元数据、文本、
 *     CPU 侧中间结果）；
 *   · DMA 源/目的缓冲必须留内部：本板已知的有 s_blit_stage（上屏暂存）、
 *     LVGL draw buffer（若已 PSRAM 则不动）、I2S DMA 描述符与 feeder 的
 *     out[]（i2s_channel_write 的数据源）、SD 的 fread/fwrite 缓冲。
 */
#pragma once

#include <stddef.h>

#include "esp_heap_caps.h"

/* PSRAM 优先；失败退默认堆（内部 RAM）——与裸 malloc 同语义 */
static inline void *mp_psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

static inline void *mp_psram_calloc(size_t n, size_t sz)
{
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(n, sz, MALLOC_CAP_8BIT);
}
