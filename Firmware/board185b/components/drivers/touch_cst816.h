/**
 * @file touch_cst816.h
 * @brief CST816S 电容触摸驱动（共享 I2C，纯轮询，无 ISR 依赖）
 *
 * 引脚（profiles/lcd185b.h）：TP_RST = GPIO1，TP_INT = GPIO4，I2C 7-bit 0x15
 * （SCL=10 / SDA=11 共享总线，与 QMI8658/PCF85063 同线）。
 *
 * 对外接口与 216 板的 touch_cst9220 完全同构（init / read / set_isr_callback /
 * touch_point_t，坐标语义一致：屏幕坐标系、pressed=首点按下、fingers=触点总数），
 * 应用层（hal_contract.h 的 mp_touch_read / input 任务）按 216 同款接入即可工作。
 *
 * 使用模式：
 *   1. touch_cst816_init()       复位 + 注册 I2C + 探测（缺失降级，不阻断启动）；
 *   2. 任务上下文周期调 touch_cst816_read() 轮询坐标（bring-up 口径，
 *      同 key_gpio0 的无 ISR 风格）；
 *   3. touch_cst816_set_isr_callback() 留作将来 INT 通知加速——首次注册时
 *      才挂 GPIO 中断服务，不注册则零中断参与。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 触摸首点（与 touch_cst9220.h 的 touch_point_t 同构） */
typedef struct {
    bool     pressed;  /**< true = 有手指按下（读首点） */
    uint8_t  fingers;  /**< 当前触点总数（>1 供手势扩展用） */
    uint16_t x;        /**< 首点 X，屏幕坐标系（当前为原始 12bit 值透传，待真机校准） */
    uint16_t y;        /**< 首点 Y（同上） */
} touch_point_t;

/** @brief 初始化：复位时序 + 注册到 I2C 总线 + INT 引脚配置 + 在线探测。幂等。 */
esp_err_t touch_cst816_init(void);

/**
 * @brief 读首点（任务上下文调用，纯轮询）
 *
 * 一次 I2C 突发读数据帧，解析出触点数与首点坐标；I2C 失败时返回错误、
 * out 不变。
 */
esp_err_t touch_cst816_read(touch_point_t *out);

/** 读法矩阵探针（诊断用）：遍历 寄存器×长度×两种读法，打印各自 err。
 *  用于"哪种读法能通"的取证，定稿后删。 */
void touch_cst816_probe_matrix(void);

/**
 * @brief 软复位唤醒：写命令字 0x00（CST816S RESET），等 1.5ms 让其进入工作态
 *
 * 必要性（真机实证）：芯片在无触摸一段时间后进入 auto-sleep，**停止应答 I2C**
 * （恒 NACK）；读帧前先软复位即可稳定读到数据。返回 ESP_OK = 命令已发出。
 */
esp_err_t touch_cst816_wake(void);

/**
 * @brief 总线级自愈：复位触摸 IC（RST 低 12ms → 高）并重新探测地址
 *
 * 用于"连续读失败（NACK / ESP_ERR_INVALID_STATE）导致触屏恒不可用"的真机故障
 * （开机正常、1~2 秒后开始恒失败）。返回 ESP_OK = 探测通过、可继续轮询。
 * 注意：会短暂断开触摸（~70ms），期间按下会丢——只在连续失败时调用。
 */
esp_err_t touch_cst816_recover(void);

/**
 * @brief 注册 INT 中断通知回调（中断上下文！）
 *
 * CST816S 检测到触摸时 INT 拉低（下降沿脉冲）。回调只允许
 * xTaskNotifyFromISR / queue send 这类 FROM_ISR 动作，禁止 I2C 读写。
 * 首次注册非空回调时才安装 GPIO ISR 服务并使能下降沿；传 NULL 取消注册
 * （已装的中断保留，handler 变为空操作）。
 *
 * @param cb  回调函数（FROM_ISR 安全动作），传 NULL 取消注册
 * @param arg 透传给回调的用户参数
 */
void touch_cst816_set_isr_callback(void (*cb)(void *arg), void *arg);

#ifdef __cplusplus
}
#endif
