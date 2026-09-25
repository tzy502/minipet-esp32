/**
 * @file imu_qmi8658.c
 * @brief QMI8658A/C 六轴 IMU 驱动实现
 *
 * 地址：0x6B（部分丝印 QMI8658C 为 0x6A，探测双地址兜底）。
 * 数据寄存器 0x65 起连续 12 字节：ax ay az gx gy gz（int16 小端）。
 *
 * BRING-UP 注意（无法本机验证，寄存器语义按 QMI8658A 数据手册最佳记忆
 * 抄录，标 [核对] 的字段上机首日须对照手册 §寄存器表复核）：
 *  - CTRL1=0x60（SIM=1 I2C 模式 + 地址自增，突发读依赖自增）[核对]
 *  - CTRL7 使能位序（本文件取 bit7=aEN / bit6=gEN）[核对：若读数恒 0，
 *    先查使能位写法，部分代码库用 0x03]
 *  - CTRL8 DRDY 路由到 INT1 的位段 [核对：错了只影响中断，不影响读取]
 */
#include "imu_qmi8658.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "amoled216.h"

static const char *TAG = "qmi8658";

/* ---------- 寄存器表（QMI8658A） ---------- */
#define QMI8658_ADDR_A        0x6B
#define QMI8658_ADDR_B        0x6A
#define QMI8658_I2C_HZ        400000

#define QMI8658_REG_WHO_AM_I  0x00   /* 期望读数 0x05 */
#define QMI8658_REG_CTRL1     0x02   /* 串口/地址自增 */
#define QMI8658_REG_CTRL2     0x03   /* 陀螺量程+ODR */
#define QMI8658_REG_CTRL3     0x04   /* 加速度量程+ODR */
#define QMI8658_REG_CTRL7     0x08   /* 传感器使能 */
#define QMI8658_REG_CTRL8     0x09   /* INT1/INT2 路由 */
#define QMI8658_REG_CTRL9     0x0A   /* CmdDone/软复位 */
#define QMI8658_REG_DATA      0x65   /* 六轴数据起始（12 字节突发） */

#define QMI8658_WHO_AM_I_VAL  0x05

/* CTRL1：SIM=1（I2C 模式）+ 地址自增 [核对] */
#define QMI8658_CTRL1_VAL     0x60

/* CTRL2（陀螺）：量程档位 [6:4]，ODR [3:0]
 * 本驱动取 gFS=±256dps、ODR 档位 0x6 [档位编码核对手册 ODR 表] */
#define QMI8658_GFS_256DPS    (0x04 << 4)   /* ±256dps -> 128 LSB/dps */
#define QMI8658_ODR_BITS      0x06
#define QMI8658_CTRL2_VAL     (QMI8658_GFS_256DPS | QMI8658_ODR_BITS)

/* CTRL3（加速度）：取 aFS=±8g（覆盖 2.0g/4.0g 敲击阈值判定）、同 ODR
 * ±8g -> 16bit 满量程 32768/8 = 4096 LSB/g */
#define QMI8658_AFS_8G        (0x02 << 4)   /* [档位编码核对] */
#define QMI8658_CTRL3_VAL     (QMI8658_AFS_8G | QMI8658_ODR_BITS)

/* CTRL7：bit7=aEN，bit6=gEN [核对] */
#define QMI8658_CTRL7_VAL     0xC0

/* CTRL8：DRDY 路由到 INT1 [核对：位段随手册定，错了只丢中断不丢数据] */
#define QMI8658_CTRL8_VAL     0xC0

/* ---------- 量程换算（与上面量程档位绑定，改档位必须同步改这里） ---------- */
#define ACC_LSB_PER_G         4096.0f   /* ±8g */
#define GYRO_LSB_PER_DPS      128.0f    /* ±256dps */

/* INT1 数据就绪脉冲：推挽高有效 -> 上升沿触发 */
#define QMI8658_INT_EDGE      GPIO_INTR_POSEDGE

static i2c_master_dev_handle_t s_dev;
static bool s_ready;
static void (*s_cb)(void *arg);
static void *s_cb_arg;
static TaskHandle_t s_wait_task;   /* wait_event 注册的等待任务（单等待者） */

#define IMU_DRDY_BIT  0x1UL       /* 任务通知位 */

static void imu_isr_handler(void *arg)
{
    (void)arg;
    /* 中断上下文：只通知，读 I2C 留给任务 */
    if (s_cb) {
        s_cb(s_cb_arg);
    }
    if (s_wait_task) {
        BaseType_t hpw = pdFALSE;
        xTaskNotifyFromISR(s_wait_task, IMU_DRDY_BIT, eSetBits, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}

esp_err_t imu_qmi8658_init(void)
{
    const minipet_pins_t *pins = &MINIPET_PROFILE_AMOLED216.pins;
    esp_err_t err;

    if (s_ready) {
        return ESP_OK;
    }
    err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }

    /* 0x6B 优先，失败试 0x6A（不同丝印批次） */
    uint16_t addr = QMI8658_ADDR_A;
    if (i2c_bus_probe(QMI8658_ADDR_A) != ESP_OK &&
        i2c_bus_probe(QMI8658_ADDR_B) == ESP_OK) {
        addr = QMI8658_ADDR_B;
    }
    err = i2c_bus_register_device("qmi8658", addr, QMI8658_I2C_HZ, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    /* 器件校验：WHO_AM_I != 0x05 视为异常（接线/地址错），硬失败提示 bring-up */
    uint8_t who = 0;
    err = i2c_bus_read_reg8v(s_dev, QMI8658_REG_WHO_AM_I, &who, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (who != QMI8658_WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "WHO_AM_I=0x%02X（期望 0x05），地址/接线异常", who);
        return ESP_ERR_NOT_FOUND;
    }

    /* 配置序列：接口模式 -> 陀螺 -> 加速度 -> 使能 -> 中断路由 */
    uint8_t cfg[][2] = {
        { QMI8658_REG_CTRL1, QMI8658_CTRL1_VAL },
        { QMI8658_REG_CTRL2, QMI8658_CTRL2_VAL },
        { QMI8658_REG_CTRL3, QMI8658_CTRL3_VAL },
        { QMI8658_REG_CTRL7, QMI8658_CTRL7_VAL },
        { QMI8658_REG_CTRL8, QMI8658_CTRL8_VAL },
    };
    for (size_t i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
        err = i2c_bus_write_reg8(s_dev, cfg[i][0], cfg[i][1]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "写 CTRL 0x%02X 失败: %s",
                     cfg[i][0], esp_err_to_name(err));
            return err;
        }
    }

    /* INT1 = 数据就绪（上升沿）；INT2 一并配成输入备用（中断扩展） */
    gpio_config_t int1 = {
        .pin_bit_mask = 1ULL << pins->imu.int1,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .intr_type    = QMI8658_INT_EDGE,
    };
    gpio_config(&int1);
    gpio_config_t int2 = {
        .pin_bit_mask = 1ULL << pins->imu.int2,
        .mode         = GPIO_MODE_INPUT,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&int2);

    gpio_install_isr_service(0); /* 已装过返回 INVALID_STATE，无碍 */
    gpio_isr_handler_add(pins->imu.int1, imu_isr_handler, NULL);

    s_ready = true;
    ESP_LOGI(TAG, "QMI8658 就绪 @0x%02X ±8g/±256dps INT1=%d", addr, pins->imu.int1);
    return ESP_OK;
}

bool imu_qmi8658_ready(void)
{
    return s_ready;
}

esp_err_t imu_qmi8658_read_acc_gyro(imu_sample_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 一次突发 12 字节：ax ay az gx gy gz（int16 小端） */
    uint8_t raw[12];
    esp_err_t err = i2c_bus_read_reg8v(s_dev, QMI8658_REG_DATA, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    int16_t v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = (int16_t)((uint16_t)raw[2 * i] | ((uint16_t)raw[2 * i + 1] << 8));
    }
    for (int i = 0; i < 3; i++) {
        out->acc_mg[i]   = (float)v[i] * 1000.0f / ACC_LSB_PER_G;
        out->gyro_mdps[i] = (float)v[3 + i] * 1000.0f / GYRO_LSB_PER_DPS;
    }
    return ESP_OK;
}

esp_err_t imu_qmi8658_read_acc(float mg[3])
{
    if (!mg) {
        return ESP_ERR_INVALID_ARG;
    }
    imu_sample_t s;
    esp_err_t err = imu_qmi8658_read_acc_gyro(&s);
    if (err == ESP_OK) {
        memcpy(mg, s.acc_mg, sizeof(s.acc_mg));
    }
    return err;
}

esp_err_t imu_qmi8658_read_gyro(float mdps[3])
{
    if (!mdps) {
        return ESP_ERR_INVALID_ARG;
    }
    imu_sample_t s;
    esp_err_t err = imu_qmi8658_read_acc_gyro(&s);
    if (err == ESP_OK) {
        memcpy(mdps, s.gyro_mdps, sizeof(s.gyro_mdps));
    }
    return err;
}

void imu_qmi8658_set_drdy_callback(void (*cb)(void *arg), void *arg)
{
    s_cb_arg = arg;
    s_cb = cb;
}

bool imu_qmi8658_wait_event(uint32_t wait_ticks)
{
    if (!s_ready) {
        return false;
    }
    /* 首次调用注册当前任务为等待者（input 任务独占语义） */
    const TaskHandle_t cur = xTaskGetCurrentTaskHandle();
    if (s_wait_task != cur) {
        s_wait_task = cur;
        xTaskNotifyWait(IMU_DRDY_BIT, IMU_DRDY_BIT, NULL, 0); /* 清历史位 */
    }
    uint32_t bits = 0;
    if (xTaskNotifyWait(0, IMU_DRDY_BIT, &bits, wait_ticks) != pdTRUE) {
        return false; /* 超时：允许调用方读上次值 */
    }
    return (bits & IMU_DRDY_BIT) != 0;
}
