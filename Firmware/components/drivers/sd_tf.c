/**
 * @file sd_tf.c
 * @brief microSD 驱动实现：SPI3_HOST + SDSPI + esp_vfs_fat
 *
 * 注意：SDSPI 的 host/slot 结构体必须常驻（驱动内部持有指针），用 static。
 */
#include "sd_tf.h"

#include <errno.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "esp_partition.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "amoled216.h"

static const char *TAG = "sd_tf";

#define SD_SPI_HOST     SPI3_HOST    /* 显示在 SPI2_HOST，互不干扰 */
#define SD_SPI_HZ       20000000     /* 20MHz：GPIO 矩阵 + 卡兼容性稳妥档 */
#define SD_MOUNT_POINT  "/sdcard"
#define SD_MAX_FILES    8            /* manifest + 多个素材包并发流式读 */

/* TF 挂载失败时的兜底：内部 Flash 的 "assets" FAT 分区挂到同一 /sdcard
 * （出厂预置默认素材，无 TF 也能起播——design-review 3.11 出厂保底） */
static wl_handle_t s_flash_wl = WL_INVALID_HANDLE;
static bool        s_on_flash;

/* SDSPI 的 host/slot 结构体必须常驻（驱动内部持有指针）。
 * IDF5：SDSPI_HOST_DEFAULT() 返回 sdmmc_host_t（sdspi_host_t 类型已不存在）。
 * C 里花括号宏只能做声明初始化，所以 static 处直接初始化，字段在 sd_mount() 里覆盖。 */
static sdmmc_host_t          s_host = SDSPI_HOST_DEFAULT();
static sdspi_device_config_t s_slot = SDSPI_DEVICE_CONFIG_DEFAULT();
static sdmmc_card_t         *s_card;
static bool                  s_mounted;

int sd_mount(void)
{
    if (s_mounted) {
        return 0;
    }
    const minipet_pins_t *pins = &MINIPET_PROFILE_AMOLED216.pins;

    /* 1) SPI 总线（SD 卡专用，无 quad 引脚） */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = pins->sd.mosi,   /* GPIO1  */
        .miso_io_num   = pins->sd.miso,   /* GPIO3  */
        .sclk_io_num   = pins->sd.sclk,   /* GPIO2  */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,          /* FATFS 单簇级读足够 */
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI3 初始化失败: %s", esp_err_to_name(err));
        return EIO;
    }

    /* 2) SDSPI slot */
    s_slot.gpio_cs  = pins->sd.cs;        /* GPIO41 */
    s_slot.host_id = SD_SPI_HOST;

    /* 3) FATFS 挂载（不自动格式化：卡需在 PC 上预先 FAT32/exFAT 格式化） */
    s_host.slot = SD_SPI_HOST;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = SD_MAX_FILES,
        .allocation_unit_size   = 0,      /* 由卡容量决定簇大小 */
        .disk_status_check_enable = false,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &s_host, &s_slot,
                                  &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD 挂载失败: %s（未插卡? 卡格式? CS 接线?）→ 尝试内部 Flash assets 分区",
                 esp_err_to_name(err));
        /* 释放 SPI 总线，给重试留干净状态 */
        spi_bus_free(SD_SPI_HOST);

        /* Flash 兜底：同一挂载点 /sdcard，下游路径零改动 */
        s_card = NULL;
        esp_vfs_fat_mount_config_t flash_cfg = {
            .format_if_mount_failed = false,   /* 已出厂预置；空分区按挂载失败报 */
            .max_files              = SD_MAX_FILES,
            .allocation_unit_size   = 4096,
        };
        err = esp_vfs_fat_spiflash_mount_rw_wl(SD_MOUNT_POINT, "assets",
                                               &flash_cfg, &s_flash_wl);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Flash assets 分区挂载也失败: %s", esp_err_to_name(err));
            return ENODEV;
        }
        s_on_flash = true;
        ESP_LOGI(TAG, "内部 Flash assets 分区已挂载 %s（出厂素材模式）", SD_MOUNT_POINT);
        return 0;
    }

    s_mounted = true;
    s_on_flash = false;
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD 已挂载 %s（MOSI=%d CLK=%d MISO=%d CS=%d）",
             SD_MOUNT_POINT, pins->sd.mosi, pins->sd.sclk,
             pins->sd.miso, pins->sd.cs);
    return 0;
}

int sd_unmount(void)
{
    if (!s_mounted) {
        return 0;
    }
    if (s_on_flash) {
        esp_err_t err = esp_vfs_fat_spiflash_unmount_rw_wl(SD_MOUNT_POINT, s_flash_wl);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Flash 分区卸载失败: %s", esp_err_to_name(err));
            return EIO;
        }
        s_on_flash = false;
        s_flash_wl = WL_INVALID_HANDLE;
        s_mounted = false;
        return 0;
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "卸载失败: %s", esp_err_to_name(err));
        return EIO;
    }
    s_mounted = false;
    s_card = NULL;
    spi_bus_free(SD_SPI_HOST);
    return 0;
}

bool sd_is_mounted(void)
{
    return s_mounted;
}
