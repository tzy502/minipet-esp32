/**
 * @file display_co5300.c
 * @brief CO5300 AMOLED QSPI 驱动实现
 *
 * ===== 传输协议（CO5300 / SH8601 / RM67162 家族通用“单线指令”QSPI 协议）=====
 *
 * 该家族面板没有 D/C 引脚，改用 8-bit 指令前缀区分传输类型，指令字节永远
 * 走单线（SIO0），数据线宽由指令决定：
 *
 *   前缀 0x02 : 后续字节（单线）= 命令
 *   前缀 0x00 : 后续字节（单线）= 命令参数 / 小块数据
 *   前缀 0x32 : 后续字节（四线 SIO0-3）= 显存像素流
 *
 * ESP32-S3 实现技巧：SPI 器件配置 command_bits=8，把前缀字节放进每个
 * transaction 的 cmd 阶段（cmd 阶段恒为单线），数据阶段用
 * SPI_TRANS_MODE_QIO 标志切到四线——一条 transaction 恰好一个 CS 帧。
 *
 * BRING-UP 注意（无法本机编译/实测，以下两点上机核对）：
 *  1) 初始化寄存器表抄自微雪 demo 惯用序列（SH8601 家族），见 s_init_cmds；
 *  2) 若屏无响应，优先试 CO5300_SPI_MODE 0 -> 3。
 */
#include "display_co5300.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "amoled216.h"

static const char *TAG = "co5300";

/* ---------- 硬件常量（引脚从 profile 取，时序/协议常量在此） ---------- */

#define CO5300_SPI_HOST       SPI2_HOST   /* SD 卡占 SPI3_HOST，两者互不干扰 */
#define CO5300_SPI_HZ         (40 * 1000 * 1000) /* GPIO 矩阵下 40MHz 稳妥 */
#define CO5300_SPI_MODE       0           /* 屏不响应时改 3 */

/* 单线指令前缀（协议见文件头） */
#define CO5300_INSTR_CMD      0x02        /* 后续单线字节 = 命令 */
#define CO5300_INSTR_DATA     0x00        /* 后续单线字节 = 参数 */
#define CO5300_INSTR_RAM_QUAD 0x32        /* 后续四线字节 = 显存数据 */

/* MIPI DCS 常用命令（与 ST7789 同语义） */
#define CO5300_CMD_CASET      0x2A        /* 列地址窗 */
#define CO5300_CMD_PASET      0x2B        /* 行地址窗 */
#define CO5300_CMD_RAMWR      0x2C        /* 显存写 */
#define CO5300_CMD_SLPIN      0x10
#define CO5300_CMD_SLPOUT     0x11
#define CO5300_CMD_DSPON      0x29
#define CO5300_CMD_BRIGHTNESS 0x51        /* DBV 亮度 0-255 */
#define CO5300_CMD_MADCTL     0x36
#define CO5300_CMD_COLMOD     0x3A        /* 0x55 = RGB565 */

/* 窗口寄存器参数固定 4 字节（16-bit 起始 + 16-bit 结束） */
#define CASET_PARAM_LEN       4
#define PASET_PARAM_LEN       4

/** 初始化表条目：命令 + 参数（<=4 字节）+ 后置延时 ms */
typedef struct {
    uint8_t  cmd;
    uint8_t  data[4];
    uint8_t  data_len;
    uint16_t delay_ms;
} co5300_init_cmd_t;

/**
 * CO5300 上电初始化序列
 *
 * BRING-UP：本表按微雪官方 demo（CO5300/SH8601 家族分页寄存器）抄录整理，
 * 未经实测。若点不亮，用逻辑分析仪/官方 demo 逐条核对，重点 0x53/0xC4。
 */
static const co5300_init_cmd_t s_init_cmds[] = {
    {0xFE, {0x20},           1, 0},   /* 切到 Page1 */
    {0x26, {0x08},           1, 0},   /* Gamma 模式（残留默认即可） */
    {0xFE, {0x00},           1, 0},   /* 回到 Page0 */
    {0xC4, {0x80},           1, 0},   /* 驱动能力/电荷泵（核对） */
    {0x36, {0x00},           1, 0},   /* MADCTL：默认扫描方向；镜像/旋转改 bit7/6/5 */
    {0x3A, {0x55},           1, 0},   /* COLMOD = RGB565 16bit */
    {0x53, {0x28},           1, 0},   /* 显示写使能（Write Display ON，核对） */
    {0x51, {0x00},           1, 0},   /* 亮度先 0，初始化完由应用抬升 */
    {0x35, {0x00},           1, 0},   /* TE 帧同步输出：关闭（ tearing 由帧率控制） */
    {0x11, {0x00},           0, 120}, /* Sleep Out，等待 >=120ms */
    {0x29, {0x00},           0, 20},  /* Display On */
};

/* ---------- 模块状态 ---------- */

static spi_device_handle_t s_spi;
static SemaphoreHandle_t   s_lock;
static bool                s_inited;

/* ---------- 底层传输 ---------- */

/**
 * @brief 发一条命令（单线）：CS 帧内 = [0x02 指令][cmd]
 */
static esp_err_t lcd_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .cmd    = CO5300_INSTR_CMD,
        .flags  = SPI_TRANS_USE_TXDATA,
        .length = 8,
        .tx_data = { cmd },
    };
    return spi_device_polling_transmit(s_spi, &t);
}

/**
 * @brief 发命令参数（单线）：CS 帧内 = [0x00 指令][data...]
 *        参数最多 4 字节，走 tx_data 免 DMA。
 */
static esp_err_t lcd_data_small(const uint8_t *data, int len)
{
    spi_transaction_t t = {
        .cmd    = CO5300_INSTR_DATA,
        .flags  = SPI_TRANS_USE_TXDATA,
        .length = len * 8,
    };
    memcpy(t.tx_data, data, len);
    return spi_device_polling_transmit(s_spi, &t);
}

/**
 * @brief 显存四线写入：CS 帧内 = [0x32 指令(单线)][像素流(四线)]
 *
 * cmd 阶段（0x32）恒单线、数据阶段由 SPI_TRANS_MODE_QIO 切四线，
 * 与面板“单线指令 + 四线数据”协议严格一致。
 * 大块传输用 spi_device_transmit（中断+DMA），避免 polling 长占 CPU。
 */
static esp_err_t lcd_ram_quad(const uint8_t *px, size_t len)
{
    spi_transaction_t t = {
        .cmd      = CO5300_INSTR_RAM_QUAD,
        .flags    = SPI_TRANS_MODE_QIO,
        .length   = len * 8,
        .tx_buffer = px,
    };
    return spi_device_transmit(s_spi, &t);
}

static inline void lcd_rst_pulse(const minipet_pins_t *pins)
{
    gpio_set_level(pins->lcd.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(pins->lcd.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(120)); /* 复位后等待内部稳压/振荡 */
}

/* ---------- 公共 API ---------- */

esp_err_t display_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    const minipet_pins_t *pins = &MINIPET_PROFILE_AMOLED216.pins;
    esp_err_t err;

    /* 复位脚：先配好再碰 SPI */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << pins->lcd.rst,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(pins->lcd.rst, 1);

    /* QSPI 总线：SIO0-3 + CLK。miso 不用（面板只写不读） */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = pins->lcd.sio0,     /* GPIO4  */
        .miso_io_num   = -1,
        .sclk_io_num   = pins->lcd.sclk,     /* GPIO38 */
        .quadwp_io_num = pins->lcd.sio2,     /* GPIO6  */
        .quadhd_io_num = pins->lcd.sio3,     /* GPIO7  */
        /* 单笔最大整屏 blit：480*480*2 = 460800 字节 */
        .max_transfer_sz = 480 * 480 * 2,
    };
    err = spi_bus_initialize(CO5300_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * 器件配置要点：
     *  - command_bits=8：每个 transaction 的 cmd 字段即协议前缀字节
     *    （0x02/0x00/0x32），cmd 阶段硬件恒单线 —— 这正是面板要的时序；
     *  - HALFDUPLEX：QIO 数据方向的常规配置（面板只写）；
     *  - CS=GPIO12 交给硬件管理，一个 transaction 恰好一个 CS 帧。
     */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = CO5300_SPI_HZ,
        .mode           = CO5300_SPI_MODE,
        .spics_io_num   = pins->lcd.cs,      /* GPIO12 */
        .queue_size     = 3,                 /* IDF5：queue_depth 改名 queue_size */
        .command_bits   = 8,
        .address_bits   = 0,
        .dummy_bits     = 0,
        /* max_transfer_sz 在 IDF5 属总线级（上方 bus_cfg 已设），器件级无此字段 */
        .flags          = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(CO5300_SPI_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI 器件挂载失败: %s", esp_err_to_name(err));
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    lcd_rst_pulse(pins);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const co5300_init_cmd_t *c = &s_init_cmds[i];
        if ((err = lcd_cmd(c->cmd)) != ESP_OK) {
            ESP_LOGE(TAG, "init 表第 %d 条 (0x%02X) 失败: %s",
                     (int)i, c->cmd, esp_err_to_name(err));
            xSemaphoreGive(s_lock);
            return err;
        }
        if (c->data_len > 0) {
            lcd_data_small(c->data, c->data_len);
        }
        if (c->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
    xSemaphoreGive(s_lock);

    s_inited = true;
    display_brightness(40); /* 默认亮度，应用可再调 */
    ESP_LOGI(TAG, "CO5300 就绪 %ux%u QSPI@%dMHz",
             MINIPET_PROFILE_AMOLED216.width, MINIPET_PROFILE_AMOLED216.height,
             CO5300_SPI_HZ / 1000000);
    return ESP_OK;
}

esp_err_t display_blit(int x, int y, int w, int h, const uint8_t *rgb565_be)
{
    if (!s_inited || !rgb565_be) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t sw = MINIPET_PROFILE_AMOLED216.width;
    const uint16_t sh = MINIPET_PROFILE_AMOLED216.height;

    /* 参数 clamp：越界直接拒绝（脏区算法层不该产生越界，这里兜底） */
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > sw || y + h > sh) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* 1) SET_WINDOW：列窗 + 行窗（16-bit 起止，大端） */
    const uint8_t col[CASET_PARAM_LEN] = {
        (uint8_t)(x >> 8), (uint8_t)x,
        (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1),
    };
    const uint8_t row[PASET_PARAM_LEN] = {
        (uint8_t)(y >> 8), (uint8_t)y,
        (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1),
    };
    if ((err = lcd_cmd(CO5300_CMD_CASET)) != ESP_OK ||
        (err = lcd_data_small(col, sizeof(col))) != ESP_OK ||
        (err = lcd_cmd(CO5300_CMD_PASET)) != ESP_OK ||
        (err = lcd_data_small(row, sizeof(row))) != ESP_OK ||
        (err = lcd_cmd(CO5300_CMD_RAMWR)) != ESP_OK) {
        goto out;
    }

    /* 2) 行流上传：整块 w*h*2 字节一次性四线推完（逐行连续、无行距） */
    err = lcd_ram_quad(rgb565_be, (size_t)w * h * 2);

out:
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "blit(%d,%d,%d,%d) 失败: %s",
                 x, y, w, h, esp_err_to_name(err));
    }
    return err;
}

esp_err_t display_brightness(uint8_t pct)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pct > 100) {
        pct = 100;
    }
    /* 百分比 -> DBV(0-255) 四舍五入 */
    const uint8_t dbv = (uint8_t)((pct * 255 + 50) / 100);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = lcd_cmd(CO5300_CMD_BRIGHTNESS);
    if (err == ESP_OK) {
        err = lcd_data_small(&dbv, 1);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                            uint16_t rgb565)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t sw = MINIPET_PROFILE_AMOLED216.width;
    const uint16_t sh = MINIPET_PROFILE_AMOLED216.height;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > sw || y + h > sh) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 行缓冲：内部 SRAM 静态分配，天然 DMA 可用。逐行流式推送。 */
    static uint8_t line[480 * 2];
    const uint8_t hi = (uint8_t)(rgb565 >> 8);   /* 大端：高字节在前 */
    const uint8_t lo = (uint8_t)(rgb565 & 0xFF);
    for (int i = 0; i < w; i++) {
        line[2 * i]     = hi;
        line[2 * i + 1] = lo;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err;
    const uint8_t col[CASET_PARAM_LEN] = {
        (uint8_t)(x >> 8), (uint8_t)x,
        (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1),
    };
    const uint8_t row[PASET_PARAM_LEN] = {
        (uint8_t)(y >> 8), (uint8_t)y,
        (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1),
    };
    if ((err = lcd_cmd(CO5300_CMD_CASET)) != ESP_OK ||
        (err = lcd_data_small(col, sizeof(col))) != ESP_OK ||
        (err = lcd_cmd(CO5300_CMD_PASET)) != ESP_OK ||
        (err = lcd_data_small(row, sizeof(row))) != ESP_OK ||
        (err = lcd_cmd(CO5300_CMD_RAMWR)) != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }
    for (int16_t r = 0; r < h && err == ESP_OK; r++) {
        err = lcd_ram_quad(line, (size_t)w * 2); /* 0x32 帧可续传，逐行合法 */
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t display_set_sleep(bool sleep)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err;
    if (sleep) {
        err = lcd_cmd(CO5300_CMD_SLPIN);
        /* SLPIN 后需 >=50ms 才允许下一条命令 */
        vTaskDelay(pdMS_TO_TICKS(60));
    } else {
        err = lcd_cmd(CO5300_CMD_SLPOUT);
        vTaskDelay(pdMS_TO_TICKS(120)); /* SLPOUT 稳定时间 */
        if (err == ESP_OK) {
            err = lcd_cmd(CO5300_CMD_DSPON);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}
