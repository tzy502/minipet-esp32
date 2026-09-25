/**
 * pcm_ring.h — PSRAM PCM 环形缓冲（bgm 解码线程 → I2S DMA feeder）
 *
 * 设计（E8/4.1）：
 *   - 128KB 缓冲分配在 PSRAM（MALLOC_CAP_SPIRAM），内部 RAM 零占用
 *   - 单写者（bgm 解码）/ 单读者（feeder）：互斥锁保护读写指针即可
 *   - 写永不阻塞（空间不足返回短写 → HTTP 读流自然背压）
 *   - 读带超时；支持清空（切采样率/停止时用）
 * 存储单位：int16 交错立体声样本（I2S DMA 直喂）。
 */
#ifndef MP_PCM_RING_H
#define MP_PCM_RING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pcm_ring_t pcm_ring_t;

/* 创建 PSRAM 环形缓冲（size_bytes 向下取整到 4 字节）；失败返回 NULL */
pcm_ring_t *pcm_ring_create(size_t size_bytes);

/* 写入样本数（int16 计）；返回实际写入数（短写=满） */
size_t pcm_ring_write(pcm_ring_t *r, const int16_t *data, size_t samples);

/* 读样本数（int16 计），最多 timeout_ms 等待数据；返回实际读到数 */
size_t pcm_ring_read(pcm_ring_t *r, int16_t *out, size_t samples, uint32_t timeout_ms);

/* 当前可读样本数（int16 计） */
size_t pcm_ring_count(pcm_ring_t *r);

/* 总容量（int16 计） */
size_t pcm_ring_capacity(pcm_ring_t *r);

/* 清空（不释放内存） */
void pcm_ring_flush(pcm_ring_t *r);

void pcm_ring_destroy(pcm_ring_t *r);

#ifdef __cplusplus
}
#endif

#endif /* MP_PCM_RING_H */
