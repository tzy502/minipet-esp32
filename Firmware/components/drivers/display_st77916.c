/**
 * @file display_st77916.c
 * @brief 1.85" 圆 LCD（ST77916）QSPI 显示驱动 —— LCD-1.85B 板专用
 *
 * 【2026-09-30 定稿】回移微雪官方 BSP 精确流程（官方 02_lvgl_demo 在本板
 * 实证点亮）：RST → 3MHz 探针 IO 读 RDDID 判批次 → 重建 80MHz 主 IO →
 * esp_lcd_st77916 组件 + 批次专属 init 表 → reset/init/disp_on。
 * 早期自研 qspi_dbi 帧序（寄存器中间字节+4dummy）在本板黑屏，弃用。
 * 像素上屏走 esp_lcd draw_bitmap（官方同款），持续全帧刷新任务维持
 * （官方栈靠 LVGL 连续刷，本固件无 LVGL 常刷，等价物=刷新任务）。
 *
 * 数据契约不变：display_blit() 像素缓冲 = 大端 RGB565。
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
#include "esp_lcd_st77916.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "amoled216.h"

static const char *TAG = "st77916";

static esp_lcd_panel_handle_t  s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_tx_slots;
static volatile uint32_t s_tx_done_cnt;
static bool s_inited;

static const uint16_t SW = 360;
static const uint16_t SH = 360;

#define BL_GPIO 5
static bool s_bl_on;

static void backlight_set(bool on)
{
    gpio_set_level(BL_GPIO, on ? 1 : 0);
    s_bl_on = on;
}

/* init tables (weixue BSP v1/v2) */
#include "st77916_init_tables_esp.c"

#define TX_QUEUE_DEPTH 1

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

static bool tx_slot_take(void)
{
    uint32_t done_seen = s_tx_done_cnt;
    int64_t last_progress = esp_timer_get_time();
    for (;;) {
        if (xSemaphoreTake(s_tx_slots, pdMS_TO_TICKS(20)) == pdTRUE) return true;
        int64_t now = esp_timer_get_time();
        if (s_tx_done_cnt != done_seen) { done_seen = s_tx_done_cnt; last_progress = now; }
        else if (now - last_progress > 2000000) {
            ESP_LOGW(TAG, "槽位 2s 无完成回调 done=%u——放行降级", (unsigned)s_tx_done_cnt);
            return false;
        }
    }
}

static void tx_slot_give(void) { xSemaphoreGive(s_tx_slots); }

static void even_round(int *x1, int *y1, int *x2, int *y2)
{
    if (*x1 < 0) *x1 = 0;
    if (*y1 < 0) *y1 = 0;
    if (*x2 > SW - 1) *x2 = SW - 1;
    if (*y2 > SH - 1) *y2 = SH - 1;
    *x1 -= (*x1 & 1);
    *y1 -= (*y1 & 1);
    if ((*x2 & 1) == 0) (*x2)++;
    if ((*y2 & 1) == 0) (*y2)++;
}

/* ══ 持续全帧刷新 ═════════════════════════════════════════════════════ */
static const uint16_t *s_fb_src;
static int s_fb_stride;
static void (*s_frame_lock)(void);
static void (*s_frame_unlock)(void);
static volatile bool s_refresh_on;
static volatile bool s_refresh_suspended;   /* 绑定期挂起标志（display_refresh_suspend/resume） */
static uint8_t *s_refr_stage;
static int s_refr_rows;

static void refresh_task(void *arg)
{
    (void)arg;
    uint8_t *stage = s_refr_stage;
    const int rows = s_refr_rows;
    const int chunk_sz = SW * rows * 2u;
    const int chunks = SH / rows;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t frames = 0, fails = 0, last_report = 0;
    while (s_refresh_on && s_fb_src && s_panel) {
        if (s_refresh_suspended) {
            /* 【绑定挂起 2026-09-30】素材绑定 fopen 期间挂起绘制，减少本任务
             * 与渲染层对 s_lock / TF 的竞争。挂起 = 跳过绘制 + 100ms 轮询，
             * 【绝不退出循环、绝不释放 stage】——任务一旦走到下方 heap_caps_free
             * + vTaskDelete，resume 后就再无刷新通路 = 屏幕永久冻结。
             * 期间 display_blit/display_fill_rect 的 s_refresh_on 跳过逻辑照旧，
             * 屏面静止在最后一帧合成结果。顺手重置相位：挂起期间 last_wake
             * 持续落后，若不重置，恢复后 vTaskDelayUntil 会连续立即返回追帧空转。 */
            last_wake = xTaskGetTickCount();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        for (int c = 0; c < chunks && s_refresh_on; c++) {
            const int y0 = c * rows;
            /* 锁序=先帧锁后显示锁：合成器持帧锁→display_blit 拿显示锁；
             * 若本任务反序（持显示锁等帧锁）= ABBA 死锁（真机 15s 全系统冻结实证） */
            if (s_frame_lock) s_frame_lock();
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
                if (s_frame_unlock) s_frame_unlock();
                continue;
            }
            /* 【480 管线复用 2026-10-01】合成器按 216 同构跑 480×480（娃娃/地面/
             * 时钟/横幅全部原逻辑零改动），本任务唯一缩放点：480→360（×3/4）
             * 最近邻取样 + 小端→大端交换。 */
            {
                const int src_w = (int)s_fb_stride;      /* 480 */
                const int step_fp = (int)(((int64_t)src_w << 16) / SW);   /* 87381 */
                for (int ry = 0; ry < rows; ry++) {
                    int sy = ((int64_t)(y0 + ry) * src_w) / SW;
                    const uint16_t *srow = s_fb_src + (size_t)sy * s_fb_stride;
                    uint16_t *drow = (uint16_t *)stage + (size_t)ry * SW;
                    int sx_fp = 0;
                    for (int i = 0; i < SW; i++, sx_fp += step_fp)
                        drow[i] = __builtin_bswap16(srow[sx_fp >> 16]);
                }
            }
            xSemaphoreGive(s_lock);
            if (s_frame_unlock) s_frame_unlock();
            bool slot = tx_slot_take();
            esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, 0, y0, SW, y0 + rows, stage);
            if (slot && e != ESP_OK) tx_slot_give();
            if (e != ESP_OK) fails++;
        }
        frames++;
        if (frames - last_report >= 150) {
            ESP_LOGW(TAG, "刷新遥测：frames=%u fails=%u", (unsigned)frames, (unsigned)fails);
            last_report = frames;
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(5));
    }
    heap_caps_free(stage);
    vTaskDelete(NULL);
}

static int refresh_stage_alloc(void)
{
    static const int rows_opts[] = { 2, 1 };
    for (int i = 0; i < 2; i++) {
        s_refr_stage = heap_caps_malloc(SW * rows_opts[i] * 2u,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (s_refr_stage) {
            s_refr_rows = rows_opts[i];
            ESP_LOGW(TAG, "刷新 stage=%d 行（%uB 内部）", s_refr_rows,
                     (unsigned)(SW * rows_opts[i] * 2u));
            return 0;
        }
    }
    return -1;
}

void display_set_frame_locks(void (*lock)(void), void (*unlock)(void))
{
    s_frame_lock = lock;
    s_frame_unlock = unlock;
}

void display_set_frame_source(const uint16_t *fb, int stride)
{
    if (!s_inited) return;
    s_fb_src = fb;
    s_fb_stride = stride > 0 ? stride : SW;
#if CONFIG_MP_LCD_CONTINUOUS_REFRESH
    if (!s_refr_stage && refresh_stage_alloc() != 0) {
        ESP_LOGE(TAG, "刷新 stage 分配失败——黑屏");
        return;
    }
    if (fb && !s_refresh_on) {
        s_refresh_on = true;
        if (xTaskCreatePinnedToCore(refresh_task, "lcd_refr", 2560, NULL, 4, NULL, 0) != pdPASS) {
            s_refresh_on = false;
            ESP_LOGE(TAG, "刷新任务创建失败");
        } else {
            ESP_LOGW(TAG, "持续全帧刷新已启动");
        }
    }
#endif
}

/* 【绑定挂起 2026-09-30】素材绑定（fopen TF 包）期间暂停全帧刷新，减少
 * 锁/TF 竞争。约定：suspend/resume 必须同作用域成对（调用方负责所有
 * return 路径都 resume，见 state_machine dispatch_manifest_synced）。
 * 只置标志，不触碰任务/stage/面板——幂等，重复 suspend 无害。 */
void display_refresh_suspend(void)
{
    s_refresh_suspended = true;
}

void display_refresh_resume(void)
{
    s_refresh_suspended = false;   /* 下一轮 while 检查即恢复绘制 */
}

esp_err_t display_init(void)
{
    if (s_inited) return ESP_OK;
    const minipet_pins_t *pins = &MINIPET_ACTIVE_PROFILE.pins;

    s_lock = xSemaphoreCreateMutex();
    s_tx_slots = xSemaphoreCreateCounting(TX_QUEUE_DEPTH, TX_QUEUE_DEPTH);
    if (!s_lock || !s_tx_slots) return ESP_ERR_NO_MEM;

    /* 背光 */
    gpio_config_t bl_cfg = { .pin_bit_mask = 1ULL << BL_GPIO,
                             .mode = GPIO_MODE_OUTPUT, .intr_type = GPIO_INTR_DISABLE };
    esp_err_t err = gpio_config(&bl_cfg);
    if (err != ESP_OK) return err;
    backlight_set(true);

    /* 硬复位（官方 BSP 时序） */
    gpio_config_t rst_cfg = { .pin_bit_mask = 1ULL << pins->lcd.rst,
                              .mode = GPIO_MODE_OUTPUT, .intr_type = GPIO_INTR_DISABLE };
    err = gpio_config(&rst_cfg);
    if (err != ESP_OK) return err;
    gpio_set_level(pins->lcd.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(pins->lcd.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* QSPI 总线（官方 BSP 同款 QUAD flag） */
    spi_bus_config_t buscfg = ST77916_PANEL_BUS_QSPI_CONFIG(
                                  pins->lcd.sclk, pins->lcd.sio0, pins->lcd.sio1,
                                  pins->lcd.sio2, pins->lcd.sio3, SW * SH * 2);
    buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
    err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI 总线失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 【探针 IO @3MHz】RDDID 判批次（官方 BSP 同款；读帧序=reg 在中间字节） */
    esp_lcd_panel_io_spi_config_t probe_cfg = ST77916_PANEL_IO_QSPI_CONFIG(
                                              pins->lcd.cs, NULL, NULL);
    probe_cfg.pclk_hz = 3 * 1000 * 1000;
    esp_lcd_panel_io_handle_t probe_io = NULL;
    uint8_t id[4] = { 0 };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &probe_cfg, &probe_io) == ESP_OK) {
        uint32_t rd = (uint32_t)0x04 << 8 | (uint32_t)0x0B << 24;
        esp_err_t e1 = esp_lcd_panel_io_rx_param(probe_io, rd, id, sizeof(id));
        ESP_LOGW(TAG, "RDDID(04)@3MHz = %02X %02X %02X %02X（%s）", id[0], id[1], id[2], id[3],
                 e1 == ESP_OK ? "OK" : esp_err_to_name(e1));
        esp_lcd_panel_io_del(probe_io);
    }

    /* 【主 IO @CONFIG 时钟】官方 BSP：重建新 IO 承担 init+绘图 */
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(
                                           pins->lcd.cs, color_tx_done_cb, NULL);
    io_cfg.trans_queue_depth = TX_QUEUE_DEPTH;
    io_cfg.pclk_hz = CONFIG_MP_LCD_PCLK_HZ;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_io 创建失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 批次序列（官方 BSP v1/v2 表；本板实测=v2） */
    st77916_vendor_config_t vc = { .flags = { .use_qspi_interface = 1 } };
    if (id[0] == 0x00 && id[1] == 0x7F) {
        vc.init_cmds = vendor_specific_init_version_1;
        vc.init_cmds_size = sizeof(vendor_specific_init_version_1) / sizeof(vendor_specific_init_version_1[0]);
        ESP_LOGW(TAG, "面板 v1 序列");
    } else {
        /* v2 及未匹配皆走 v2（官方 BSP 对 v2 ID 的选择；未匹配时不回落 registry） */
        vc.init_cmds = vendor_specific_init_version_2;
        vc.init_cmds_size = sizeof(vendor_specific_init_version_2) / sizeof(vendor_specific_init_version_2[0]);
        ESP_LOGW(TAG, "面板 %s 序列（%u 条）",
                 (id[0] == 0x00 && id[1] == 0x02) ? "v2" : "未匹配→猜v2",
                 (unsigned)vc.init_cmds_size);
    }

    /* 【INVOFF 固化 2026-09-30】官方 v2 表尾的 0x21(INVON) 对本批次面板是
     * 错误极性——扫频实证 INVOFF 段颜色正确（段2/3）、INVON 段颜色负片。
     * 表后补 0x20 关闭反转。MADCTL 保持 RGB(0x00)——扫频段 2/3 均正确，
     * BGR 位在本面板无区分度。 */

    const esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = pins->lcd.rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vc,
    };
    err = esp_lcd_new_panel_st77916(s_io, &pc, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "面板创建失败: %s", esp_err_to_name(err));
        return err;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_io_tx_param(s_io, (uint32_t)0x02 << 24 | 0x20, NULL, 0);   /* INVOFF */

    /* 【COLMOD 取证+强制】像素格式错位=全图色偏+闪烁的候选根因：
     * 读 0x3A（中间字节读法），非 0x55(16bpp) 则以同款写法强制 0x55。 */
    {
        uint8_t cm[2] = { 0 };
        uint32_t rd = (uint32_t)0x3A << 8 | (uint32_t)0x0B << 24;
        if (esp_lcd_panel_io_rx_param(s_io, rd, cm, 2) == ESP_OK)
            ESP_LOGW(TAG, "COLMOD(3A)=%02X %02X", cm[0], cm[1]);
        uint8_t set55 = 0x55;
        esp_lcd_panel_io_tx_param(s_io, (uint32_t)0x02 << 24 | 0x3A, &set55, 1);
        cm[0] = cm[1] = 0;
        if (esp_lcd_panel_io_rx_param(s_io, rd, cm, 2) == ESP_OK)
            ESP_LOGW(TAG, "COLMOD after force=%02X %02X", cm[0], cm[1]);
    }

    s_inited = true;
    ESP_LOGI(TAG, "ST77916(LCD) 就绪 %ux%u QSPI BL=GPIO%d PCLK=%dMHz",
             SW, SH, BL_GPIO, CONFIG_MP_LCD_PCLK_HZ / 1000000);

    return ESP_OK;
}

void display_set_orientation(bool swap_xy, bool mirror_x, bool mirror_y)
{
    if (!s_inited || !s_panel) return;
    esp_lcd_panel_swap_xy(s_panel, swap_xy);
    esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y);
}

void display_wait_tx_idle(void)
{
    if (!s_inited) return;
    if (tx_slot_take()) tx_slot_give();
}

bool display_tx_busy(void)
{
    if (!s_inited || !s_tx_slots) return false;
    return uxSemaphoreGetCount(s_tx_slots) == 0;
}

esp_err_t display_blit(int x, int y, int w, int h, const uint8_t *rgb565_be)
{
    if (!s_inited || !rgb565_be) return ESP_ERR_INVALID_STATE;
    if (s_refresh_on) return ESP_OK;   /* 持续刷新为唯一显示通路：脏区直推冗余且会交替闪烁 */
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > SW || y + h > SH)
        return ESP_ERR_INVALID_ARG;

    int x1 = x, y1 = y, x2 = x + w - 1, y2 = y + h - 1;
    even_round(&x1, &y1, &x2, &y2);
    int aw = x2 - x1 + 1, ah = y2 - y1 + 1;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t err;
    bool slot = tx_slot_take();
    if (aw == w && ah == h) {
        err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, (void *)rgb565_be);
    } else {
        size_t sz = (size_t)aw * ah * 2u;
        uint8_t *tmp = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp) { if (slot) tx_slot_give(); xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
        memset(tmp, 0, sz);
        for (int ry = 0; ry < ah; ry++) {
            int sy = ry + (y1 - y);
            if (sy < 0) sy = 0;
            if (sy > h - 1) sy = h - 1;
            const uint8_t *src = rgb565_be + (size_t)sy * w * 2;
            uint8_t *dst = tmp + (size_t)ry * aw * 2;
            for (int rx = 0; rx < aw; rx++) {
                int sx = rx + (x1 - x);
                if (sx < 0) sx = 0;
                if (sx > w - 1) sx = w - 1;
                dst[rx * 2] = src[sx * 2];
                dst[rx * 2 + 1] = src[sx * 2 + 1];
            }
        }
        err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, tmp);
        heap_caps_free(tmp);
    }
    if (slot && err != ESP_OK) tx_slot_give();

    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        static int64_t s_last_err_log;
        int64_t now = esp_timer_get_time();
        if (now - s_last_err_log > 5000000) {
            ESP_LOGE(TAG, "draw_bitmap 失败: %s", esp_err_to_name(err));
            s_last_err_log = now;
        }
    }
    return err;
}

esp_err_t display_brightness(uint8_t pct)
{
    if (!s_inited) return ESP_OK;
    backlight_set(pct > 0);
    return ESP_OK;
}

esp_err_t display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t rgb565)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (s_refresh_on) return ESP_OK;   /* 同 blit：刷新流统一呈现 */
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > SW || y + h > SH)
        return ESP_ERR_INVALID_ARG;

    static uint8_t line[4][360 * 2] __attribute__((aligned(64)));
    int x1 = x, y1 = y, x2 = x + w - 1, y2 = y + h - 1;
    even_round(&x1, &y1, &x2, &y2);
    const int aw = x2 - x1 + 1;
    if (aw > (int)sizeof(line[0]) / 2) return ESP_ERR_INVALID_ARG;

    const uint8_t hi = (uint8_t)(rgb565 >> 8);
    const uint8_t lo = (uint8_t)(rgb565 & 0xFF);
    for (int i = 0; i < aw; i++) {
        line[0][2 * i] = hi;  line[0][2 * i + 1] = lo;
        line[1][2 * i] = hi;  line[1][2 * i + 1] = lo;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t err = ESP_OK;
    for (int yy = y1; yy <= y2 && err == ESP_OK; yy += 2) {
        bool slot = tx_slot_take();
        err = esp_lcd_panel_draw_bitmap(s_panel, x1, yy, x2 + 1, yy + 2, line);
        if (slot && err != ESP_OK) tx_slot_give();
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t display_set_sleep(bool sleep)
{
    if (!s_inited || !s_panel) return ESP_ERR_INVALID_STATE;
    backlight_set(!sleep);
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, !sleep);
    if (!sleep) vTaskDelay(pdMS_TO_TICKS(120));
    return err;
}

