/**
 * @file sd_tf.h
 * @brief microSD 卡驱动（SPI 模式 + FATFS 挂载）
 *
 * 引脚（来自 profile）：MOSI=1 / CLK=2 / MISO=3 / CS=41，独占 SPI3_HOST
 * （显示占 SPI2_HOST，互不干扰）。
 *
 * 挂载点：/sdcard。素材目录布局见 software-design.md §4.4（/minipet/…）。
 * 长文件名已由 sdkconfig.defaults 打开（CONFIG_FATFS_LFN_HEAP_ENABLED）。
 *
 * 返回值约定：0 = 成功；失败返回正的 errno 值（ENODEV/EIO/…），
 * 同时 ESP_LOGE 打印 esp_err 原始码，方便区分是 SPI 层还是 FATFS 层。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 SPI3 + SDSPI + FATFS 挂载到 /sdcard
 *
 * 卡不插/挂载失败不视为致命：返回 errno，app 降级（无素材缓存态）。
 * 幂等：已挂载直接返回 0。
 */
int sd_mount(void);

/* TF 在位但无素材、服务端同步失败时：卸载 TF，改挂内部 Flash 出厂素材分区
 * （/sdcard 路径不变，下游零改动）。幂等；成功返回 0。 */
int sd_tf_switch_to_factory(void);

/* 工厂分区第二挂载点 /factory（TF 保持挂 /sdcard）：渲染根切工厂用。
 * 幂等；Flash 回退模式下返回 false（/sdcard 已是工厂，无需第二挂载）。 */
bool sd_factory_mount_secondary(void);

/* TF 物理在位且曾挂载成功（空卡降级后仍 true）——下载许可判据：
 * true=下载写 TF（即使渲染暂用出厂分区）；false=TF 真不在，禁下载。 */
bool sd_tf_tf_present(void);

/** @brief 卸载并释放 SPI 资源（热拔支持用；正常关机可不做） */
int sd_unmount(void);

/** @brief 当前是否已挂载可读 */
bool sd_is_mounted(void);

/** 当前是否在**内部 Flash 出厂素材**模式（TF 卡缺失/挂载失败后回退）。
 *  用于"没有 TF 卡就渲染默认形象 + 默认地图并屏上提示"的需求判定。 */
bool sd_tf_is_flash_fallback(void);

#ifdef __cplusplus
}
#endif
