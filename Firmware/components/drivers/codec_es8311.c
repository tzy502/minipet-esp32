/**
 * @file codec_es8311.c
 * @brief ES8311 codec 实现：I2C 上电序列 + I2S 标准模式 TX + PA 控制
 *
 * I2S 拓扑：ESP32-S3 为 I2S master（S3 出 MCLK/BCLK/LRCK），ES8311 为
 * slave。ES7210（录音）共享同一组 BCLK/LRCK/MCLK，但 ASDOUT(=GPIO10)
 * 的 RX 通道本驱动不创建——audio 模块要录音时需把通道重建为全双工
 * （i2s_new_channel 同时给 tx/rx handle），本文件只保留 TX。
 *
 * BRING-UP 注意 [核对]：I2C 上电序列按 espressif esp_codec_dev / esp-adf
 * 的 es8311 参考序列抄录（DAC 播放链路、MCLK=256fs、I2S slave），
 * 未经实测。若无声按官方参考驱动逐条核对 0x01/0x02/0x12/0x13/0x14/0x32。
 */
#include "codec_es8311.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "amoled216.h"

static const char *TAG = "es8311";

#define ES8311_ADDR        0x18    /* 7-bit */
#define ES8311_I2C_HZ      400000

/* ES8311 寄存器（仅列本文件用到的） */
#define ES8311_REG_RESET   0x00    /* 0x80 软复位 */
#define ES8311_REG_VOLUME  0x32    /* DAC 数字音量 0-0xBF */
#define ES8311_VOLUME_MAX  0xBF

/* PA 使能极性：GPIO46 高电平开功放（低电平/复位默认为关，见 strapping 注释） */
#define PA_ACTIVE_LEVEL    1

/** {寄存器, 值} 初始化表（DAC 播放链路；逐条语义见行尾，全部 [核对]） */
static const struct { uint8_t reg; uint8_t val; } s_es8311_init[] = {
    { 0x00, 0x80 }, /* 软复位 */
    { 0x00, 0x00 }, /* 释放复位 */
    { 0x01, 0x3F }, /* CLK1：MCLK 源选择 = MCLK 引脚输入（外部 256fs）[核对] */
    { 0x02, 0x10 }, /* CLK2：内部时钟分频 [核对] */
    { 0x12, 0x00 }, /* 系统电源管理：DAC 链路上电 [核对] */
    { 0x0D, 0x01 }, /* 模拟参考（VREF）上电 [核对] */
    { 0x0E, 0x02 }, /* ADC/DAC 模拟供电 [核对] */
    { 0x13, 0x10 }, /* 耳机/DAC 输出驱动 [核对] */
    { 0x14, 0x10 }, /* 输出驱动级 [核对] */
    { 0x32, 0xBF }, /* DAC 音量默认最大（应用层用 set_volume 调小） */
};

static i2s_chan_handle_t     s_tx;
static i2c_master_dev_handle_t s_dev;
static uint32_t              s_rate;
static bool                  s_pa_on;

/* ---------------- 公共 API ---------------- */

esp_err_t codec_es8311_init(uint32_t sample_rate_hz)
{
    const minipet_pins_t *pins = &MINIPET_PROFILE_AMOLED216.pins;
    esp_err_t err;

    err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_bus_register_device("es8311", ES8311_ADDR, ES8311_I2C_HZ, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    /* I2C 上电序列（失败仅告警：先保证 I2S 链路可用，无声问题留 log 排查） */
    if (i2c_bus_probe(ES8311_ADDR) == ESP_OK) {
        for (size_t i = 0; i < sizeof(s_es8311_init) / sizeof(s_es8311_init[0]); i++) {
            err = i2c_bus_write_reg8(s_dev, s_es8311_init[i].reg, s_es8311_init[i].val);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "写寄存器 0x%02X 失败: %s",
                         s_es8311_init[i].reg, esp_err_to_name(err));
                return err;
            }
        }
    } else {
        ESP_LOGW(TAG, "ES8311 @0x%02X 探测失败，播放不可用", ES8311_ADDR);
    }

    /* I2S TX：S3 做 master，MCLK=256*fs。立体声槽位（codec 取左声道；
       mono 源由 audio 模块在解码后自行重复/映射到双声道） */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* DMA 欠载时保持末帧电平，防爆音 */
    err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel 失败: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = pins->audio.mclk,   /* GPIO42 */
            .bclk = pins->audio.bclk,   /* GPIO9  */
            .ws   = pins->audio.lrck,   /* GPIO45 */
            .dout = pins->audio.dsdin,  /* GPIO8  */
            .din  = -1,                 /* ES7210 录音 RX 不在此建（见文件头） */
            .invert_flags = { 0 },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256; /* ES8311 需要 256fs */

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        return err;
    }
    s_rate = sample_rate_hz;

    /* PA_CTRL：GPIO46。注意 46 是 strapping 脚，复位期间内部下拉=功放关闭，
       这正好是我们要的默认态。显式拉低兜底。 */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << pins->audio.pa_en,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(pins->audio.pa_en, !PA_ACTIVE_LEVEL);
    s_pa_on = false;

    ESP_LOGI(TAG, "ES8311 就绪 %lu Hz MCLK=256fs PA=%d(off)",
             (unsigned long)s_rate, pins->audio.pa_en);
    return ESP_OK;
}

esp_err_t codec_es8311_set_sample_rate(uint32_t sample_rate_hz)
{
    if (!s_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = i2s_channel_disable(s_tx);
    if (err != ESP_OK) {
        return err;
    }
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_reconfig_std_clock(s_tx, &clk);
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) {
        s_rate = sample_rate_hz;
    }
    return err;
}

esp_err_t codec_es8311_set_volume(uint8_t pct)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pct > 100) {
        pct = 100;
    }
    const uint8_t reg = (uint8_t)((pct * ES8311_VOLUME_MAX + 50) / 100);
    return i2c_bus_write_reg8(s_dev, ES8311_REG_VOLUME, reg);
}

esp_err_t codec_es8311_write(const void *pcm16, size_t bytes)
{
    if (!s_tx || !pcm16 || bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *p = (const uint8_t *)pcm16;
    size_t remain = bytes;
    /* 阻塞直到写完；单笔超时 1s（audio_q 断流时向上报错而不是死等） */
    while (remain > 0) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, p, remain, &written,
                                          pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            return err;
        }
        p += written;
        remain -= written;
    }
    return ESP_OK;
}

esp_err_t pa_ctrl_enable(bool on)
{
    const minipet_pins_t *pins = &MINIPET_PROFILE_AMOLED216.pins;
    gpio_set_level(pins->audio.pa_en, on ? PA_ACTIVE_LEVEL : !PA_ACTIVE_LEVEL);
    s_pa_on = on;
    return ESP_OK;
}

bool pa_ctrl_is_enabled(void)
{
    return s_pa_on;
}
