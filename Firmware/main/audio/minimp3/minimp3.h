/**
 * minimp3.h — 公有领域 MP3 解码器集成契约（vendored 占位）
 *
 * 来源：github.com/lieff/minimp3（公有领域，作者 lieff）
 * 完整实现集成时拷入：将上游 minimp3.h 原文整体替换本文件（同名 API 保持
 * 不变），并删除下方占位实现段。minimp3.c（#define MINIMP3_IMPLEMENTATION
 * + include 本文件）即为上游官方的集成方式，无需改动。
 *
 * 上游 API 精确签名（bgm.c 依赖面，与 lieff/minimp3 master 一致）：
 *   void mp3dec_init(mp3dec_t *dec);
 *   int  mp3dec_decode_frame(mp3dec_t *dec, const uint8_t *inp, int input_bytes,
 *                            short *out, mp3dec_frame_info_t *info);
 *   返回值 = 本帧每声道样本数（0=无帧/跳过；1152 为 MPEG1 Layer3 最大值）
 *   info->frame_bytes = 本帧消耗的输入字节数（含头；0=需要更多数据）
 *   info->frame_offset / channels / hz / layer / bitrate_kbps
 *
 * 常量（上游定义，此处等值声明）：
 *   MP3D_MAX_SAMPLE_RATE 48000
 *   MINIMP3_MAX_SAMPLES_PER_FRAME (1152*2)
 *
 * mp3dec_t 以不透明定长块暴露：sizeof(上游 mp3dec_t) ≈ 20KB（float 合成器
 * 状态），此处预留 32KB，方向是宁大勿小；替换上游原文后以真实类型为准。
 */
#ifndef MINIMP3_H
#define MINIMP3_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP3D_MAX_SAMPLE_RATE           48000
#define MINIMP3_MAX_SAMPLES_PER_FRAME  (1152 * 2)

typedef struct {
    int frame_bytes;
    int frame_offset;
    int channels;
    int hz;
    int layer;
    int bitrate_kbps;
} mp3dec_frame_info_t;

/* 不透明解码器状态（见文件头注释；上游原文替换后为真实结构体） */
typedef struct {
    uint8_t opaque[32 * 1024];
} mp3dec_t;

void mp3dec_init(mp3dec_t *dec);
int mp3dec_decode_frame(mp3dec_t *dec, const uint8_t *inp, int input_bytes,
                        short *out, mp3dec_frame_info_t *info);

/* ------------------------------------------------------------------ */
/* 占位实现：仅保证链接与逻辑骨架可审；替换上游原文后此段整体删除。       */
/* ------------------------------------------------------------------ */
#ifdef MINIMP3_IMPLEMENTATION
#warning "minimp3 vendored 占位实现：请拷入 github.com/lieff/minimp3 完整源码（见文件头注释）"

#include <string.h>

void mp3dec_init(mp3dec_t *dec)
{
    memset(dec, 0, sizeof(*dec));
}

int mp3dec_decode_frame(mp3dec_t *dec, const uint8_t *inp, int input_bytes,
                        short *out, mp3dec_frame_info_t *info)
{
    (void)dec; (void)inp; (void)out;
    /* 占位：不产 PCM。集成上游实现后：返回每声道样本数并填 info。
     * bgm.c 对「连续 N 帧 0 样本」有失联保护，占位实现下表现为取流失败。 */
    info->frame_bytes = 0;
    info->frame_offset = 0;
    info->channels = 0;
    info->hz = 0;
    info->layer = 0;
    info->bitrate_kbps = 0;
    return 0;
}
#endif /* MINIMP3_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* MINIMP3_H */
