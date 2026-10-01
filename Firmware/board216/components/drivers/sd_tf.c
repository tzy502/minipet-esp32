/**
 * @file sd_tf.c
 * @brief microSD 驱动实现：SDMMC 1-bit 原生模式 + esp_vfs_fat
 *
 * 2026-09-29 模式切换（SDSPI → SDMMC）：真机实证同一张卡（Mac 可读、FAT32/MBR
 * 正常）在 SPI 模式 CMD59 被 R1 ILLEGAL_CMD 拒绝（sdmmc_init_spi_crc 0x106），
 * 而 Waveshare BSP 官方同引脚走 SDMMC 1-bit（bsp_sdcard_mount 原样照抄口径：
 * CMD=1 CLK=2 D0=3，width=1，无 CD/WP）。SDMMC 原生模式不走 CMD59 路径。
 * 注意：host/slot 结构体常驻（驱动持有），用 static。
 */
#include "sd_tf.h"

#include <time.h>        /* time()：TF 自愈节流用系统时钟 */

#include <errno.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "esp_partition.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "amoled216.h"

static const char *TAG = "sd_tf";

#define SD_SPI_HZ       20000000     /* 20MHz：GPIO 矩阵 + 卡兼容性稳妥档 */
#define SD_MOUNT_POINT  "/sdcard"
/* 【同时打开文件数 2026-09-27】必须覆盖"常驻打开的素材包"：
 *   时钟数字 PARTS(1) + 三档 FONT(3) + 纸娃娃 PARTS(1) + LAYOUT 循环/单次(≤2)
 *   + 清单/下载瞬时(1~2) ≈ 9~10。
 * 旧值 4 的来源是"manifest+1 包并发"这个早期假设——只对**下载**成立，对**渲染常驻**
 * 不成立。当时没暴雷是因为字体压根没绑定成功（px 读错）；字体一修好，
 * 真机立刻变成：`vfs_fat: open: no free file descriptors` →
 * `mpak: open /sdcard/minipet/parts/… failed` → `本地素材加载失败` → FATAL 态
 * + 渲染任务 TWDT 连续触发（人物整只消失，实体缓冲全 0）。
 * 每个打开文件在内部堆上约 0.6~0.7KB（FIL + vfs 包装），12 个 ≈ 8KB；
 * 每次开机都会打印内部堆水位（provision_dump_internal_heap）便于回归。 */
/* 注意：ESP-IDF 的 FATFS VFS 是**按 max_files 预分配 FIL 数组**的
 * （vfs_fat.c:202 `ctx_size = sizeof(vfs_fat_ctx_t) + max_files * sizeof(FIL)`，
 * FIL ≈0.6KB）→ 每多一个名额就多吃内部 DRAM。实测常驻打开数为 7（时钟 PARTS 1
 * + 三档 FONT 3 + 纸娃娃 PARTS 1 + LAYOUT ≤2），留 3 个瞬时名额即 10。 */
#define SD_MAX_FILES    10

/* TF 挂载失败时的兜底：内部 Flash 的 "assets" FAT 分区挂到同一 /sdcard
 * （出厂预置默认素材，无 TF 也能起播——design-review 3.11 出厂保底） */
static wl_handle_t s_flash_wl = WL_INVALID_HANDLE;
static bool s_tf_present;   /* TF 物理在位且挂载成功过（空卡降级后仍 true——
                             * 下载继续写 TF，只有 TF 真不在才禁下载） */
static bool        s_on_flash;

/* SDMMC host/slot 常驻（esp_vfs_fat_sdmmc_mount 内部持有引用） */
static sdmmc_host_t         s_host = SDMMC_HOST_DEFAULT();
static sdmmc_slot_config_t  s_slot;
static sdmmc_card_t         *s_card;
static bool                  s_mounted;

int sd_mount(void)
{
    if (s_mounted) {
        return 0;
    }
    const minipet_pins_t *pins = &MINIPET_ACTIVE_PROFILE.pins;

    /* 原 CS=41（=卡 D3）：SD 卡上电复位时按 CS/D3 电平选模式（低=SPI 高=SD）。
     * 之前 SPI 固件数十次启动可能已把卡锁在 SPI 态——这里配内部上拉保证
     * CMD0 期间为高（卡选 SD 模式），若仍超时需拔 USB 给卡断电磁复位 */
    gpio_config_t cs_pull = {
        .pin_bit_mask = 1ULL << pins->sd.cs,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (pins->sd.cs >= 0) {
        gpio_config(&cs_pull);   /* cs<0 = 无 CS 脚（SDMMC 原生模式板，如 1.85B） */
    }

    /* SDMMC 1-bit slot（引脚=BSP 官方宏同款：CMD=1(mosi) CLK=2(sclk) D0=3(miso)）
     * 不自动格式化：卡需在 PC 上预先 FAT32 格式化 */
    s_slot = (sdmmc_slot_config_t) {
        .clk  = pins->sd.sclk,            /* GPIO2 */
        .cmd  = pins->sd.mosi,            /* GPIO1 */
        .d0   = pins->sd.miso,            /* GPIO3 */
        .d1   = GPIO_NUM_NC,
        .d2   = GPIO_NUM_NC,
        .d3   = GPIO_NUM_NC,
        .d4   = GPIO_NUM_NC,
        .d5   = GPIO_NUM_NC,
        .d6   = GPIO_NUM_NC,
        .d7   = GPIO_NUM_NC,
        .cd   = SDMMC_SLOT_NO_CD,
        .wp   = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = 0,
    };

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = SD_MAX_FILES,
        .allocation_unit_size   = 0,      /* 由卡容量决定簇大小 */
        .disk_status_check_enable = false,
    };

    /* 【挂载重试 2026-10-01】真机反复出现 `sdmmc_init_ocr: send_op_cond(1)
     * returned 0x107`（TIMEOUT）→ 一次没谈成 OCR 就整机回退"出厂素材模式"：
     * 表现是背景变默认图、相机不可用、**气泡中文全丢**（出厂字库只有 116 字），
     * 而重插/重启后又能好——典型的卡上电时序/接触边缘问题。
     * 这里改成 4 次重试（每次 host deinit + 250ms 让卡与控制器都回到干净态），
     * 覆盖绝大多数边缘情况；仍失败才走 Flash 兜底。 */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 4; attempt++) {
        s_host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
        err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &s_host, &s_slot,
                                      &mount_cfg, &s_card);
        if (err == ESP_OK) {
            if (attempt) ESP_LOGW(TAG, "SD 挂载第 %d 次成功（前几次 0x%x）", attempt + 1, err);
            break;
        }
        ESP_LOGW(TAG, "SD 挂载失败（第 %d/4 次）: %s", attempt + 1, esp_err_to_name(err));
        sdmmc_host_deinit();              /* 归还 SDMMC 外设，给重试留干净状态 */
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD 挂载 4 次均失败: %s（未插卡? 卡接触? 卡格式?）→ 尝试内部 Flash assets 分区",
                 esp_err_to_name(err));

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
            /* 【真机崩溃根因，2026-09-27】SD 卡不在时走本回退，而 assets 分区
             * 首次挂载/坏 WL 状态会触发 WL 初始化 + FAT 格式化/擦除，在内部堆
             * 仅 ~143KB 且已被 WiFi/LVGL 挤压的情况下分配失败 →
             * `wl_read failed (0x101)` → 驱动内部 ESP_ERROR_CHECK 直接 abort →
             * 设备无限重启（实测 12s 内 4 次重启，日志见 boot_new.log）。
             * 回退是【可选路径】：失败绝不能拖垮整机——降级为"无本地素材"
             * （素材仍可由服务端 manifest 下发到内存/其他存储），返回错误即可。 */
            ESP_LOGE(TAG, "Flash assets 分区挂载失败: %s（内部空闲=%u 最大块=%u）"
                          "→ 降级为无本地素材，不阻断启动",
                     esp_err_to_name(err),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            s_flash_wl = (wl_handle_t)0;
            return ENODEV;
        }
        s_on_flash = true;
        ESP_LOGI(TAG, "内部 Flash assets 分区已挂载 %s（出厂素材模式）", SD_MOUNT_POINT);
        return 0;
    }

    s_mounted = true;
    s_on_flash = false;
    s_tf_present = true;                 /* TF 在位（此后空卡降级不清此标志） */
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD 已挂载 %s（SDMMC 1-bit：CMD=%d CLK=%d D0=%d）",
             SD_MOUNT_POINT, pins->sd.mosi, pins->sd.sclk, pins->sd.miso);
    return 0;
}

static wl_handle_t s_factory_wl = WL_INVALID_HANDLE;

bool sd_factory_mount_secondary(void)
{
    /* 【双根目录】工厂分区挂 /factory（TF 同时挂在 /sdcard）：
     * 渲染读 /factory（内部 Flash，不受下载影响），下载写 /sdcard（TF）。
     * 幂等；Flash 回退模式（/sdcard 已是工厂）无需第二挂载返回 false。 */
    if (s_on_flash) return false;
    if (s_factory_wl != WL_INVALID_HANDLE) return true;
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files              = 2,      /* 渲染侧同时至多开 1-2 个包 */
        .allocation_unit_size   = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/factory", "assets",
                                                     &cfg, &s_factory_wl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "/factory 挂载失败: %s", esp_err_to_name(err));
        s_factory_wl = WL_INVALID_HANDLE;
        return false;
    }
    ESP_LOGI(TAG, "工厂分区已挂载 /factory（渲染根）");
    return true;
}

int sd_tf_switch_to_factory(void)
{
    /* 【空卡降级 2026-09-29】TF 在位但为空、服务端清单同步未成功时切回内部
     * Flash 出厂素材（复用无 TF 兜底整条链）。调用上下文：boot（无资产绑定、
     * bgm 无打开文件——卸载 TF 安全）。已工厂模式/未挂载则幂等返回。 */
    if (s_on_flash || !s_mounted) return 0;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TF 卸载失败: %s（放弃降级）", esp_err_to_name(err));
        return -1;
    }
    s_mounted = false;
    s_card = NULL;
    esp_vfs_fat_mount_config_t flash_cfg = {
        .format_if_mount_failed = false,
        .max_files              = SD_MAX_FILES,
        .allocation_unit_size   = 4096,
    };
    err = esp_vfs_fat_spiflash_mount_rw_wl(SD_MOUNT_POINT, "assets",
                                           &flash_cfg, &s_flash_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Flash assets 分区挂载失败: %s（降级失败，无本地素材）",
                 esp_err_to_name(err));
        s_flash_wl = (wl_handle_t)0;
        return -1;
    }
    s_on_flash = true;
    ESP_LOGW(TAG, "已切换内部 Flash 出厂素材（TF 为空且清单同步未成）");
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
    return 0;
}

bool sd_tf_tf_present(void)
{
    return s_tf_present;
}

bool sd_tf_is_flash_fallback(void)
{
    return s_on_flash;      /* 由 sd_mount() 的 Flash 回退分支置位 */
}

/* ------------------------------------------------------------------ */
/* 【TF 掉卡自愈 2026-10-01】                                            */
/* ------------------------------------------------------------------ */
/* 出厂回退态下周期性探测"卡是否回来了"。只做 **纯卡层探测**（sdmmc_card_init），
 * 不挂 FS、不触碰已打开的文件（渲染侧此刻正把字体/素材从 Flash 分区读着，
 * 贸然卸载 /sdcard 会把它们的 fd 打断）。探测成功 = 卡已可通信 → 由调用方
 * （poller）安排一次重启，让启动路径重新按 TF 优先挂载。
 * 节流：RTC 记忆上次自愈重启时刻，5 分钟内最多一次，防"边缘卡"把设备拖进
 * 重启循环。 */
#define SD_HEAL_RTC_MAGIC 0x53444831u   /* 'SDH1' */
static RTC_NOINIT_ATTR uint32_t s_heal_magic;
static RTC_NOINIT_ATTR uint32_t s_heal_last_s;

bool sd_tf_probe_card(void)
{
    if (!s_on_flash) return false;                 /* 只在出厂回退态探测 */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 10000;                     /* 探测用低频，边缘卡更容易谈成 */
    if (sdmmc_host_init() != ESP_OK) return false;
    if (sdmmc_host_init_slot(host.slot, &s_slot) != ESP_OK) {
        sdmmc_host_deinit();
        return false;
    }
    sdmmc_card_t *card = calloc(1, sizeof(sdmmc_card_t));   /* card_init 需要已分配的结构 */
    if (!card) { sdmmc_host_deinit(); return false; }
    esp_err_t err = sdmmc_card_init(&host, card);
    free(card);
    sdmmc_host_deinit();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "TF 卡探测成功（卡已恢复通信）");
        return true;
    }
    return false;
}

/* 是否允许为"TF 恢复"重启一次（5 分钟节流，RTC 记忆跨重启） */
bool sd_tf_heal_reboot_allowed(void)
{
    time_t now = time(NULL);
    if (s_heal_magic != SD_HEAL_RTC_MAGIC) {       /* 冷启动（RTC 域刚上电） */
        s_heal_magic = SD_HEAL_RTC_MAGIC;
        s_heal_last_s = 0;
    }
    if (now < 1600000000) return true;             /* 系统时间无效：不节流（极少见） */
    if (s_heal_last_s && (uint32_t)now - s_heal_last_s < 300u) {
        ESP_LOGW(TAG, "TF 恢复重启节流中（%us 前刚试过）→ 本次只记录不自愈",
                 (unsigned)((uint32_t)now - s_heal_last_s));
        return false;
    }
    s_heal_last_s = (uint32_t)now;
    return true;
}

bool sd_is_mounted(void)
{
    return s_mounted;
}
