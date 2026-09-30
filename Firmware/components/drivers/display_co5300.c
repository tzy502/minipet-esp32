/**
 * @file display_co5300.c
 * @brief 2.16" AMOLED 显示驱动 —— 内部实现 = 微雪官方 BSP 组件包装
 *
 * 2026-09-26 真机 bring-up 定稿：手写 QSPI 时序点不亮面板，整体切换到
 * 厂商维护的 waveshare/esp32_s3_touch_amoled_2_16 BSP（内部 = esp_lcd
 * co5300 QSPI 面板驱动，init 序列/四线像素写入均为官方实现）。
 * 对外 API（display_co5300.h）不变，渲染层零改动。
 *
 * 数据契约：display_blit() 像素缓冲 = 大端 RGB565（BSP_LCD_BIGENDIAN=1 一致）。
 * CO5300 QSPI 面板按 2 像素粒度寻址（官方 rounder 佐证）——blit/fill 区域
 * 在本驱动内先 clamp 到屏内再取偶（右/下缘向内收尾），奇数区域经 PSRAM
 * 暂存缓冲拷齐（与合成器 mark/compose 矩形同源，防右缘残影）。
 */
#include "display_co5300.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_memory_utils.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_co5300.h"
#include "driver/spi_master.h"
#include "amoled216.h"

static const char *TAG = "co5300";

static esp_lcd_panel_handle_t  s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_tx_slots;   /* 背压：在飞 color 传输槽位（=队列深度），完成回调归还 */
static volatile uint32_t s_tx_done_cnt;   /* 累计完成次数（槽位泄漏探针判据） */
static bool s_inited;

static const uint16_t SW = 480;
static const uint16_t SH = 480;

/* 【2026-09-27 真机"刷新有裂纹"修复】队列深度 3 → 1。
 * 原深度 3 是为吞吐，但一帧会被 blit_be 拆成多笔（368×288 区域 / 16 行每笔
 * ≈18 笔）连续入队，面板在【上一笔尚未扫出】时就收到下一笔的窗口切换
 * （CASET/RASET + RAMWR）→ 同屏两笔传输交叠 = 用户看到的"刷新有裂纹"。
 * 深度 1 = 每笔完成回调归还槽位后才允许下一笔入队（配合既有槽位信号量背压
 * 天然串行），代价是 SPI 利用率略降；本板瓶颈在内部堆与面板带宽，不在深度。 */
#define TX_QUEUE_DEPTH 1

/* esp_lcd color 传输完成（SPI ISR 上下文）：归还一个槽位。
 * 背压模型（2026-09-26 真机定稿）：BSP 异步队列深度 3，队列满时 esp_lcd
 * 【立即返回 ESP_ERR_NO_MEM 丢帧】（绝不阻塞）——真机实证：拖拽期全帧 20
 * 连发打满队列后 80s 内 7404 条 "send color data failed"（且疑似个别传输
 * 被 WiFi+GDMA 并发吞掉后槽位永不归还，风暴转持续性）。现以计数信号量把
 * 在飞硬限在深度内：推送前取槽（满则阻塞排队 = 健康背压），完成回调归还，
 * 队列从此不满 → NO_MEM 路径不再触发。 */
static bool IRAM_ATTR color_tx_done_cb(esp_lcd_panel_io_handle_t io,
                                       esp_lcd_panel_io_event_data_t *edata,
                                       void *user)
{
    (void)io; (void)edata; (void)user;
    s_tx_done_cnt++;
    BaseType_t hi = pdFALSE;
    xSemaphoreGiveFromISR(s_tx_slots, &hi);
    return hi == pdTRUE;
}

/* 推送前取槽。等待中持续观察完成计数：有完成 = 管线活，纯背压继续等；
 * 2s 零完成 = 槽位泄漏（正常 24KB@40MHz 排空 ≈1.2ms/槽）→ WARN + 放行
 * （让 draw_bitmap 自己报 NO_MEM，宁可丢一帧绝不死锁）。返回 false =
 * 未取得槽位（仅泄漏降级路径），调用方须在 push 失败时勿重复归还。 */
static bool tx_slot_take(void)
{
    uint32_t done_seen = s_tx_done_cnt;
    int64_t last_progress = esp_timer_get_time();
    for (;;) {
        if (xSemaphoreTake(s_tx_slots, pdMS_TO_TICKS(20)) == pdTRUE) {
            return true;
        }
        int64_t now = esp_timer_get_time();
        if (s_tx_done_cnt != done_seen) {
            done_seen = s_tx_done_cnt;
            last_progress = now;
        } else if (now - last_progress > 2000000) {
            ESP_LOGW(TAG, "槽位 2s 无完成回调（泄漏疑点）done=%u——放行降级",
                     (unsigned)s_tx_done_cnt);
            return false;
        }
    }
}

/* 推送失败时手动归还（失败路径没有完成回调） */
static void tx_slot_give(void)
{
    xSemaphoreGive(s_tx_slots);
}

/* 区域 2 像素对齐（CO5300 寻址粒度），x1/y1/x2/y2 为含端坐标。
 * 规则（右缘 480 残影根修，与合成器 mark/compose 统一）：
 * 「先 clamp 到屏内，再决定奇偶」——右/下缘 clamp 到 SW-1/SH-1（天然为奇，
 * 即向内收尾），然后才做向外取偶；屏内偶数端点 +1 后仍 ≤ 479。
 * 修复前「先外扩后 clamp」：x2=480 会被顶到 481 的越屏中间态，aw 与源宽
 * 错位转入 scratch 且源映射偏移，最终窗口与 mark/compose 矩形分歧。 */
static void even_round(int *x1, int *y1, int *x2, int *y2)
{
    if (*x1 < 0) *x1 = 0;
    if (*y1 < 0) *y1 = 0;
    if (*x2 > SW - 1) *x2 = SW - 1;      /* 先 clamp：杜绝越屏中间态 */
    if (*y2 > SH - 1) *y2 = SH - 1;
    *x1 -= (*x1 & 1);                    /* 左/上缘向下取偶：0 即屏缘，向内收 */
    *y1 -= (*y1 & 1);
    if ((*x2 & 1) == 0) (*x2)++;         /* 屏内向外取偶：x2≤478 → ≤479 */
    if ((*y2 & 1) == 0) (*y2)++;
}

esp_err_t display_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    s_tx_slots = xSemaphoreCreateCounting(TX_QUEUE_DEPTH, TX_QUEUE_DEPTH);
    if (!s_lock || !s_tx_slots) {
        return ESP_ERR_NO_MEM;
    }

    /* 直建 QSPI 总线 + 面板 IO（队列深度 3 与官方 bsp_display_new 一致，
     * 队列不满由上层槽位背压保证，见 color_tx_done_cb 注释）。
     * 引脚=本板定值（与 BSP 默认一致）：PCLK38 D0-3=4/5/6/7 CS12 RST39 */
    const spi_bus_config_t buscfg = CO5300_PANEL_BUS_QSPI_CONFIG(38, 4, 5, 6, 7,
                                    SW * SH * 2);
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(12,
                                           color_tx_done_cb, NULL);
    io_cfg.trans_queue_depth = TX_QUEUE_DEPTH;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                   &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_io 创建失败: %s", esp_err_to_name(err));
        return err;
    }

    const co5300_vendor_config_t vc = {
        .flags.use_qspi_interface = 1,
        /* init_cmds=NULL → 组件内置默认初始化序列（官方维护） */
    };
    const esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = 39,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vc,
    };
    err = esp_lcd_new_panel_co5300(s_io, &pc, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "co5300 面板创建失败: %s", esp_err_to_name(err));
        return err;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);
    /* 方向定稿（2026-09-27 菜单照片终审）：组合 0 = swap=false + 无镜像。
     * 证据链：组合1(mirror_x) 菜单文字左右镜像（"Maps"→"sdɐM"）、垂直正常；
     * 组合2(mirror_y) 垂直颠倒（标定线在顶）；→ 原生方向即用户握持正立方向。 */
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);

    display_brightness(80);   /* 默认 80%，应用可再调 */
    s_inited = true;
    ESP_LOGI(TAG, "CO5300(SYNC) 就绪 %ux%u QSPI", SW, SH);
    return ESP_OK;
}

/* 运行时方向切换（方向标定轮播用，见头文件注释）：直接下发 esp_lcd
 * swap_xy/mirror，只改面板扫描方向，不触碰 blit 数据通路与对齐逻辑。 */
void display_set_orientation(bool swap_xy, bool mirror_x, bool mirror_y)
{
    if (!s_inited || !s_panel) {
        ESP_LOGW(TAG, "orientation 忽略：display 未初始化 swap=%d mx=%d my=%d",
                 swap_xy, mirror_x, mirror_y);
        return;
    }
    esp_lcd_panel_swap_xy(s_panel, swap_xy);
    esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y);
    ESP_LOGI(TAG, "orientation swap=%d mx=%d my=%d", swap_xy, mirror_x, mirror_y);
}

/* 【上屏暂存复用竞态修复 2026-09-27】给合成器用：在**重填共用暂存缓冲之前**
 * 必须确认上一笔 color 传输已经读完它。此前 blit_be 的循环是
 *   填暂存(第 N 块) → display_blit(第 N 块) → 填暂存(第 N+1 块) → ...
 * 而 display_blit 内部才做 tx_slot_take()（等槽）——于是第 N 笔 DMA 还在读暂存时，
 * 第 N+1 块的填充已经把同一块内存覆盖 ⇒ 面板收到**两个块的数据混在一起**：
 * 真机表现就是横彩条/竖条纹/"分割线"（用户照片实证）。
 * 本函数取槽再立刻归还：返回时保证"无在飞传输"，可安全重填暂存。 */
void display_wait_tx_idle(void)
{
    if (!s_inited) return;
    if (tx_slot_take()) tx_slot_give();
}

/* 在飞传输数 >0（探针用：填暂存时若为真即命中竞态） */
bool display_tx_busy(void)
{
    if (!s_inited || !s_tx_slots) return false;
    return uxSemaphoreGetCount(s_tx_slots) == 0;
}

esp_err_t display_blit(int x, int y, int w, int h, const uint8_t *rgb565_be)
{
    if (!s_inited || !rgb565_be) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > SW || y + h > SH) {
        return ESP_ERR_INVALID_ARG;
    }

    int x1 = x, y1 = y, x2 = x + w - 1, y2 = y + h - 1;
    even_round(&x1, &y1, &x2, &y2);
    int aw = x2 - x1 + 1, ah = y2 - y1 + 1;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err;
    bool slot = tx_slot_take();          /* 背压：无空闲槽则排队等待（见 color_tx_done_cb） */
    if (aw == w && ah == h) {
        err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, (void *)rgb565_be);
    } else {
        /* 奇数区域：PSRAM 暂存补齐（边缘像素按邻边复制，视觉无差） */
        size_t sz = (size_t)aw * ah * 2u;
        uint8_t *tmp = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp) { if (slot) tx_slot_give(); xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
        memset(tmp, 0, sz);
        for (int ry = 0; ry < ah; ry++) {
            int sy = ry + (y1 - y);                    /* 目标行 → 源行 */
            if (sy < 0) sy = 0;
            if (sy > h - 1) sy = h - 1;
            const uint8_t *src = rgb565_be + (size_t)sy * w * 2;
            uint8_t *dst = tmp + (size_t)ry * aw * 2;
            for (int rx = 0; rx < aw; rx++) {
                int sx = rx + (x1 - x);
                if (sx < 0) sx = 0;
                if (sx > w - 1) sx = w - 1;
                dst[rx * 2]     = src[sx * 2];
                dst[rx * 2 + 1] = src[sx * 2 + 1];
            }
        }
        err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, tmp);
        heap_caps_free(tmp);
    }
    if (slot && err != ESP_OK) tx_slot_give();   /* 失败无完成回调，手动归还 */

    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        static int64_t s_last_err_log;
        int64_t now = esp_timer_get_time();
        if (now - s_last_err_log > 5000000) {   /* 5s 限频，防刷屏拖慢系统 */
            ESP_LOGE(TAG, "draw_bitmap 失败: %s", esp_err_to_name(err));
            s_last_err_log = now;
        }
    }
    return err;
}

/* 216 板（CO5300 GRAM）：脏区增量上屏即可持续显示，无持续刷新需求。
 * 本函数仅为跨板 API 完整性而存在（185B 的 RAMless 面板才真正消费帧源）。 */
void display_set_frame_source(const uint16_t *fb, int stride)
{
    (void)fb; (void)stride;
}

void display_set_frame_locks(void (*lock)(void), void (*unlock)(void))
{
    (void)lock; (void)unlock;   /* 216 GRAM：无持续刷新，无撕裂源 */
}

esp_err_t display_brightness(uint8_t pct)
{
    /* CO5300 亮度 = 0x51 DBV 命令（0-255）。panel 未就绪前静默跳过，
     * display_init 完成后由应用再调即可 */
    if (!s_io) {
        return ESP_OK;
    }
    return esp_lcd_panel_io_tx_param(s_io, 0x51, (const uint8_t[]){ pct }, 1);
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

    /* 行缓冲：内部 SRAM 静态分配，天然 DMA 可用。两行一组推送（2 行寻址粒度）。 */
    static uint8_t line[2][480 * 2] __attribute__((aligned(64)));   /* 64B 对齐：SPI 驱动零拷贝直推（同 s_blit_stage 注释） */
    /* 与 display_blit 同一条 2px 对齐规则（even_round：先 clamp 屏内再取偶）。
     * 取偶后 x1/y1 恒偶、x2/y2 恒奇 → 窗口宽高恒偶，两行一组推送，
     * 无需奇数收尾分支。修复点：旧实现手写奇数行收尾 [y+h-2, y+h)，
     * y=0,h=1 时起点为 -1（RASET 负坐标 → 面板窗口未定义）；且列方向
     * 从未对齐 2px 粒度（奇 x/奇 w 窗口直接下发）。 */
    int x1 = x, y1 = y, x2 = x + w - 1, y2 = y + h - 1;
    even_round(&x1, &y1, &x2, &y2);
    const int aw = x2 - x1 + 1;          /* x1≥0 且 x2≤479 → aw ≤ 480 */
    if (aw > (int)sizeof(line[0]) / 2) {
        return ESP_ERR_INVALID_ARG;      /* 防御：缓冲按整屏宽定长 */
    }

    const uint8_t hi = (uint8_t)(rgb565 >> 8);   /* 大端：高字节在前 */
    const uint8_t lo = (uint8_t)(rgb565 & 0xFF);
    for (int i = 0; i < aw; i++) {
        line[0][2 * i]     = hi;
        line[0][2 * i + 1] = lo;
        line[1][2 * i]     = hi;
        line[1][2 * i + 1] = lo;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    for (int yy = y1; yy <= y2 && err == ESP_OK; yy += 2) {
        bool slot = tx_slot_take();      /* 背压同 display_blit（2 行/笔，整屏 240 笔） */
        err = esp_lcd_panel_draw_bitmap(s_panel, x1, yy, x2 + 1, yy + 2, line);
        if (slot && err != ESP_OK) tx_slot_give();
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t display_set_sleep(bool sleep)
{
    if (!s_inited || !s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, !sleep);
    if (!sleep) {
        vTaskDelay(pdMS_TO_TICKS(120));   /* SLPOUT 稳定时间 */
    }
    return err;
}
