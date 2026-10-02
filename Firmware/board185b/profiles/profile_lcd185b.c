/**
 * @file profile_lcd185b.c
 * @brief 设备 profile：Waveshare ESP32-S3-Touch-LCD-1.85B（1.85" 圆 360×360 LCD）
 *
 * 引脚抄自官方 wiki GPIO 表（docs.waveshare.net/ESP32-S3-Touch-LCD-1.85B）：
 *   LCD(ST77916 QSPI)：CS=21 PCLK=40 D0-3=46/45/42/41 RST=3 BL=5
 *   共享 I2C：SCL=10 SDA=11
 *   TF 卡：SDMMC CLK=15 CMD=14 D0-3=16/17/12/13（sd_tf 走 1-bit：CLK/CMD/D0）
 *   按键：仅 BOOT=GPIO0（菜单键，key_gpio0 轮询消抖）
 *
 * 能力位定稿（单板分支，降级路径即正式行为）：
 *   - 触摸是 CST816S@0x15，驱动未接 → has_touch=false
 *   - IMU QMI8658@0x6B 在线，但 INT 脚 wiki 未标注 → -1，走轮询（20ms 兜底读）
 *   - RTC PCF85063@0x51 在线（INT=6）；电量计是 BQ27220@0x55 → 驱动未接，has_pmu=false
 *   - 背光 = BL GPIO5（LCD）开/关两档；无逐级调光命令
 */
#include "lcd185b.h"

const minipet_profile_t MINIPET_PROFILE_LCD185B = {
    /* 【合成器空间 = 480×480（2026-10-01 修复回归）】与既有 480 世界→屏幕管线
     * 同构：RC_SCALE=2 是**全局契约**（实体/条带/时钟/娃娃一律 <<RC_SCALE_SHIFT
     * 即 2×），360 面板的适配只收敛在显示驱动的唯一缩放点（refresh_task 的
     * 480→360 ×3/4 最近邻取样）。
     *
     * 【为什么必须回 480】工作区一度把这里改成 360：于是 static/tile 走
     * layer_rgb_load 的通用路径变成 240→360 = **1.5×**，而条带（strip_blit 的
     * band_y = y<<1）/实体/时钟仍是 **2×** —— 层间比例不一致 → 上半地形错位/
     * 重复图案（bring-up 坑档 §2.1 的症状）与画面整体比例失真。
     * 定稿依据见 docs/amoled185b-bringup-pitfalls.md §一/§五（480 合成器 →
     * 0.75 驱动取样 → 360 面板）。教训：板级差异走 profile 字段，但
     * width/height = **合成器空间 = 面板原生 360×360**（2026-10-01 定稿 A1）。
     * 为什么改成 360：原 480 空间靠驱动层 0.75 最近邻缩到面板 → **每 4 列丢 1 列**
     * （480→360，step_fp=87381），资产像素与面板像素是 1.5:1 且周期性丢样。
     * 改 360 后 step_fp=(360<<16)/360=65536 → **恒等映射：一个像素不丢不重（真 1:1）**。
     * 连带口径：世界视场 = 屏/RC_SCALE = 360 世界 px（原 240）；娃娃按资产原始
     * 1x 尺寸上屏（原 ×2）→ 占屏比变小、可视地图范围变大（用户口径选 A1）。 */
    .width  = 360,
    .height = 360,
    .shape  = MINIPET_SHAPE_ROUND,
    .shape_name = "round",
    .psram_mb  = 8,
    .flash_mb  = 16,
    .cpu_freq_mhz = 240, /* 双核 240MHz（sdkconfig 同步声明） */

    .has_audio = true,    /* 【2026-10-01 移植】ES8311 + I2S TX（MCLK2/BCLK48/LRCK38/DOUT47/PA9） */
    .has_touch = true,    /* 【2026-10-01 移植】CST816S@0x15（INT=4 RST=1，纯轮询） */
    .has_imu   = true,    /* QMI8658@0x6B：INT 未引出 → int1/int2=-1，轮询模式 */
    .has_rtc   = true,    /* PCF85063@0x51，INT=6 */
    .has_pmu   = true,    /* 【2026-10-01 移植】BQ27220@0x55 电量计（只读标准命令） */
    .has_sd    = true,    /* SDMMC 槽在线（1-bit：CLK15/CMD14/D0=16），cs 未用=-1 */
    .has_key   = false,   /* 无独立菜单键脚；BOOT=GPIO0 菜单键（input_dispatch） */
    .ground_cam_shift_px = 248, /* 185B 实验：各层上移248行，底部接真 foothold 地面带
                                 * （单位=合成器 px；整图包路径自动跳过，见 compositor） */

    .world_scale = 1,          /* A1：世界 1x = 屏 1x（1:1 定稿） */
    .pins = {
        .i2c  = { .scl = 10, .sda = 11 },
        .lcd  = { .sio0 = 46, .sio1 = 45, .sio2 = 42, .sio3 = 41,
                  .sclk = 40, .cs = 21, .rst = 3 },
        .touch = { .intr = 4, .rst = 1 },   /* CST816S（touch_cst816.c，纯轮询） */
        .imu  = { .int1 = -1, .int2 = -1 }, /* INT 脚未标注 → 轮询模式 */
        .rtc  = { .intr = 6 },
        .audio = { .mclk = 2, .bclk = 48, .lrck = 38,
                   .dsdin = 47, .asdout = 39, .pa_en = 9 },
        .sd   = { .mosi = 14, .miso = 16, .sclk = 15, .cs = -1 }, /* SDMMC CMD/D0/CLK */
        .key  = { .menu = -1 },             /* 无独立菜单键脚（BOOT=GPIO0 兼任） */
        .pmu  = { .pmu_irq = -1 },
    },
};
