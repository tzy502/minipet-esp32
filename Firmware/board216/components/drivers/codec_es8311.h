/**
 * @file codec_es8311.h
 * @brief ES8311 音频 codec（共享 I2C 控制 + I2S 播放数据）+ 功放控制
 *
 * 引脚（来自 profile）：
 *   I2C 控制口 = GPIO14/15（总线共享，器件地址 0x18）
 *   I2S：MCLK=42 / BCLK=9 / LRCK=45 / DSDIN=8（ESP32-S3 -> ES8311 播放数据）
 *   PA_CTRL = GPIO46 功放使能 ——【铁律：不开功放无声】默认 false
 *
 * I2S 由本驱动创建为 master（ES8311 做 slave，MCLK=256*fs 由 S3 输出）。
 * 录音链路（ES7210，ASDOUT=GPIO10）不在本驱动范围，audio 模块如需录音
 * 应重建为全双工 channel（见 .c 文件头注释）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 codec：I2C 上电序列 + I2S TX 通道 + PA_CTRL 引脚（保持关闭）
 *
 * @param sample_rate_hz 采样率（16000/44100/48000…），MCLK 自动 = 256*fs
 *
 * 初始化后 PA 仍是关的：调 pa_ctrl_enable(true) 才出声（避免上电爆音）。
 */
esp_err_t codec_es8311_init(uint32_t sample_rate_hz);

/** @brief 运行中切换采样率（I2S 时钟重配；I2C 模拟配置不动） */
esp_err_t codec_es8311_set_sample_rate(uint32_t sample_rate_hz);

/** @brief codec 数字音量 0-100（寄存器 0x32；粗调，关声用 PA） */
esp_err_t codec_es8311_set_volume(uint8_t pct);

/**
 * @brief 阻塞写 PCM（16-bit interleaved，I2S Philips 标准）
 *
 * audio_q 消费侧调用；内部循环直到 bytes 全部写入或超时。
 * 数据格式：小端 int16，声道数按初始化配置（默认立体声，codec 取左声道）。
 */
esp_err_t codec_es8311_write(const void *pcm16, size_t bytes);

/** @brief 功放使能（GPIO46）。true=出声；默认 false；静音优先用这个 */
esp_err_t pa_ctrl_enable(bool on);

/** @brief PA 当前状态 */
bool pa_ctrl_is_enabled(void);

#ifdef __cplusplus
}
#endif
