/*
 * compositor.c — 自研合成器（POKER 场景全权写屏）+ render.h 公共 API 实现
 *
 * 帧循环（render_tick，30fps 由 app 定时器驱动）：
 *   1) 实体帧 delay 到 → 重合成实体层（piece 按 z 序 blit；表情件替换：
 *      piece.expr_index≠255 → 取 face 件 expr_group 组内第 active_expr 个变体）
 *   2) 条带 offset_x = (speed_x*ms/1000 + tilt*rx*K) mod 图宽（1x 世界）
 *   3) 增量合成候选区域：static_back → 条带 → tile → 时钟 → 实体 → 气泡
 *   4) 16×16 网格 hash diff → 行程合并 → display_blit（唯一写屏出口）
 *
 * 内存（PSRAM，heap_caps）：见文件尾 RENDER_PSRAM_BUDGET 注释。
 */
#include "compositor.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dirent.h>       /* 截图保留最近 3 张：扫描/删除旧 shot_*.bmp */
#include <errno.h>        /* 截图 open/write 失败原因 */
#include <fcntl.h>        /* 截图流式写文件（POSIX open/write，免 stdio 缓冲分配） */
#include <sys/stat.h>     /* mkdir /sdcard/debug */
#include <unistd.h>       /* write/close/unlink */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"  /* UDP 帧倾倒（远程取证第二通道，免拔卡） */

#include <lvgl.h>

#include "drivers.h"
#include "entity_anim.h"
#include "clock_digits.h"
#include "font_lazy.h"
#include "font5x7.h"
#include "lvgl_bridge.h"
#include "bgm.h"          /* E6：半屏 BGM 控制条读真实播放态/音量/曲目名 */
#include "render.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "rc";

/* ================= 静态状态 ================= */

static bool g_inited, g_menu;

/* ══════════════════════════════════════════════════════════════════════════
 * 【合成器互斥 2026-09-27 真机根因修复：面板"一条明显的分割线 + 头缺一块"】
 *
 * 取证证据（开机抓串口，flush 分块指纹与主机端同算法合成结果逐块对拍）：
 *   同一笔 flush rect=(240,240 160x160) 的 5 个分块里，
 *   前 2 块 = fly 帧 0 的像素，后 3 块 = fly 帧 1 的像素（FNV 与主机端逐块全等）。
 *   ⇒ g_fb 在 blit_be 分块之间被【另一个任务】重新合成了一次：
 *     同一屏上出现两个不同时刻的画面 = 用户照片里的水平分割线 + 人物某块内容错帧。
 *
 * 机理：compositor 全静态状态（g_fb / g_ent_px / g_mark / s_blit_stage）无任何锁，
 * 而 render.h 的调用约定（"所有函数必须与 render_tick 同任务调用"）在应用层被违反：
 *   · 输入任务：input_dispatch → state_machine_tick_1hz → CLOCK_DOZE/render_set_clock
 *     → full_recompose()；
 *   · 输入/状态机任务：render_set_map / render_set_clock / render_exit_menu /
 *     render_force_redraw → full_recompose()；
 *   · render_set_parts / render_set_layout / render_set_expression → recompose_entity()
 *     + mark_rect()（写实体缓冲与脏区网格）。
 * 于是渲染任务的 flush（compose_region + blit_be，屏上实测约 10ms）会被上述调用
 * 从中间截断 → 面板收到跨帧混合内容（且共用 s_blit_stage 还会串数据）。
 *
 * 修法：**递归互斥**把"合成+上屏"与"实体重合成/标脏"整体串行化（同任务嵌套可取，
 * 不会自锁）。这不是把并发"藏起来"，而是把 render.h 里那句约定变成运行时保证。
 * ══════════════════════════════════════════════════════════════════════════ */
static SemaphoreHandle_t s_rlock;

static void rc_lock(void)
{
    if (s_rlock) xSemaphoreTakeRecursive(s_rlock, portMAX_DELAY);
}
static void rc_unlock(void)
{
    if (s_rlock) xSemaphoreGiveRecursive(s_rlock);
}


/* 帧数据锁: 持续刷新拷贝 g_fb 前必须持锁, 防读写撕裂花屏 */
void compositor_frame_lock(void) { rc_lock(); }
void compositor_frame_unlock(void) { rc_unlock(); }
static int32_t g_sw, g_sh;                 /* 屏幕尺寸（480×480） */

static uint16_t *g_fb;                     /* framebuffer（单一所有权） */

/* 地图场景 */
static bool g_map_ok;
static uint16_t *g_static;                 /* 屏幕尺寸（装载时按需 2x 展开） */
static uint16_t *g_tile;
static uint8_t  *g_tile_mask;              /* 屏幕尺寸 1bit */
static int64_t   g_map_epoch_us;

typedef struct {
    bool      ok;
    uint16_t *px;                          /* 1x 存储 */
    uint8_t  *mask;                        /* NULL=不透明 */
    uint16_t  w, h;
    uint32_t  stride_b;
    int16_t   y, speed_x;
    uint8_t   rx, blend;
    int32_t   last_off;
} rc_strip_t;
static rc_strip_t *g_strips;
static int         g_strip_n;

/* 实体 */
static uint16_t *g_ent_px;                 /* RC_ENT_W×RC_ENT_H（内容=2x 展开图，像素=屏幕像素） */
static uint8_t  *g_ent_cov;                /* 1bit 覆盖 */
static int32_t   g_ent_base_wx, g_ent_base_wy;   /* 世界 1x 附加偏移（默认 0,0） */

/* 实体画布（对齐桌面版 GetBounds 联合画布，见 LayoutPackWriter 语义注释）：
 * 导出 x/y = FinalX - body锚点（FinalX 已含 part origin，即位图左上角相对 body 锚点
 * 的偏移；不含帧位移 move）。设备画布 = 该动作全部帧 piece 矩形的联合包围盒，
 * 原点 = min(x,y)（等价桌面 bounds.Left/Top 画布原点，manifest 不带 bounds 时按
 * piece 联合包围盒现算）。绑定 parts+layout 后懒计算一次。 */
static bool    g_ent_cbox_ok;
static int32_t g_ent_cx0, g_ent_cy0;       /* 联合包围盒左上（世界 1x） */
/* 画布内 body 锚点（= 桌面 PaperdollService.RenderFrame 返回的 origin，
 * manifest LAYOUT 条目 origin=[x,y] 下发；缺省 0,0 = 旧摆放行为）。
 * 摆放契约：**画布左上角屏幕坐标 = 屏心 - origin×scale** ⇒ origin 恒在屏心，
 * 换动作/换画布尺寸时人物不跳（桌面侧靠"窗口位置 -= Δorigin"达到同一效果）。 */
static int32_t g_ent_ox, g_ent_oy;
/* 【交互期冻结视差 2026-09-27】用户在摸/拖时（触摸、按键、IMU 晃动）把地图条带的
 * 滚动刷新让出去：条带整幅重合成 ~70ms/笔，与拖拽抢同一份渲染预算就是"拖动卡顿"。
 * 交互结束后 1.5s 自动恢复滚动（观感上只是"手一碰，背景先停一下"）。 */
static volatile int64_t g_ui_active_until_ms;
void render_note_activity(void) { g_ui_active_until_ms = esp_timer_get_time() / 1000 + 1500; }
static int32_t g_ent_cw,  g_ent_ch;        /* 联合包围盒宽高（世界 1x，已 clamp 到缓冲） */

static mpak_t g_parts;   static bool g_parts_ok;
static mpak_t g_lt_loop; static bool g_lt_loop_ok;   /* stand1 等循环动作 */
static mpak_t g_lt_once; static bool g_lt_once_ok;   /* 单次动作 */
static rc_anim_t g_anim;

/* 部件位图缓存（当前装扮；TF 懒读） */
typedef struct rc_part_img {
    const mpak_part_t *meta;
    uint16_t *px;
    uint8_t  *mask;
    uint32_t px_hash;      /* 位图指纹（可见性探针）：part_id 不同但指纹相同
                            * = PARTS 包内数据共享/坏偏移（帧静止嫌疑判据） */
    uint32_t mask_hash;    /* 掩码指纹（取证探针：主机端 mpk 解码对拍用） */
    uint32_t mask_bits;    /* 掩码非零位数（= 该件实际覆盖像素数） */
    struct rc_part_img *next;
} rc_part_img_t;
static rc_part_img_t *g_pc;
static uint32_t g_pc_bytes;
static bool g_pc_cap_logged;

/* 【取证探针 2026-09-27】>0 = 对接下来 N 次 flush 的每个分块打印
 * (rect,len,FNV)：与主机端按同一合成结果算出的分块指纹对拍——
 * 全等 ⇒ 交给 panel 的字节完全正确，故障必在面板同步/时序；
 * 不等 ⇒ blit 源行/列错位（"头部缺一块"的候选）。 */
static int s_forensic_chunks;

/* 【混行竞态探针】blit 填暂存时"上一笔仍在飞"的次数（修复前会持续增长；
 * 修复后（先等 idle 再填）该计数不再增长，只有等待动作发生）。 */
volatile uint32_t g_blit_race_hits;
volatile uint32_t g_blit_verify_fail;

/* 拖影自检开关（排障置 1；见 ghost_probe 定义处注释） */
#define MP_GHOST_PROBE 0   /* 2026-09-27 竖条纹/黑斑三处根因修复并真机逐字节自证后关闭（排障再置 1） */

/* 拖拽上下限自检（开机跑一次，打印极限位置；排障用） */
#define MP_DRAG_LIMIT_SELFTEST 0  /* 地面表下界已于真机自检 ✓（脚底 y=408=表中地面线），常态关 */


/* 气泡 */
static struct { bool active; uint16_t *px; int32_t w, h, x, y; } g_bub;

/* 未配网常驻横幅（问题4：POKER 态顶部深色底白字，compose 最顶层） */
static bool g_banner_on;
static char g_banner_text[48];
static int64_t g_banner_expire_us;         /* 定时横幅到期时刻（us；0=常驻/无定时，
                                            * render_banner_show_for 用，render_tick 到期自动隐藏） */

static void mark_rect(int32_t x, int32_t y, int32_t w, int32_t h);   /* 前向声明（校准层先于定义使用） */

/* ===== BGM 半屏控制条（E6 定稿）=====
 * 需求原文：「BGM 播放控制 = 触摸（半屏控制条：播放/暂停/切歌/音量），由选择器内
 * BGM 入口或宠物区长按呼出」。
 *
 * 此前实现是「长按 → 打开全屏菜单 BGM 页」——功能可达但不是"半屏控制条"，
 * 需求缺口核对列为 E6 未实现项。这里按定稿做成真正的半屏叠加层：
 *   - 屏幕下半 220px 深色面板（上半仍看得见宠物），呼吸式 0.6s 淡出
 *   - 5 个控件：播放/暂停 · 上一首 · 下一首 · 音量- · 音量+，横排
 *   - 触摸点选 + 侧键（中键移光标、顶键确认、底键退出），与菜单同一套侧键语义
 *   - 操作后 3s 无动作自动收起；上半屏触摸 = 收起（不穿透到宠物交互）
 * 状态全部读 bgm_* 真实接口，无本地副本。 */
#define RC_OV_H          220        /* 面板高度（屏幕下半） */
#define RC_OV_PANEL_BG   0x101018u
#define RC_OV_TITLE_FG   0xE6E6F0u
#define RC_OV_INFO_FG    0x9AA0B4u
#define RC_OV_DIVIDER    0x3C4658u
#define RC_OV_ITEM_BG    0x1E2433u
#define RC_OV_ITEM_SEL   0x2E7D6Bu
#define RC_OV_ITEM_FG    0xD8DEE9u
#define RC_OV_HINT_FG    0x6E7686u
#define RC_OV_ITEMS      5
#define RC_OV_AUTO_HIDE_MS 3000     /* 操作后无动作自动收起 */
#define RC_OV_TAP_MOVE_PX  24       /* 触摸移动超过此值不算点选（防拖拽误触） */

static bool     g_ov_on;
static int      g_ov_sel;                       /* 0..4 控件光标 */
static int64_t  g_ov_expire_us;                 /* 自动收起时刻（us） */
static char     g_ov_title[32];                 /* 曲目名快照（bgm_current_title 非线程安全时兜底） */
static uint8_t  g_ov_vol;                       /* 音量快照 */
static uint8_t  g_ov_state;                     /* mp_bgm_state_t 快照 */
static bool     g_ov_offline;                   /* 离线：面板提示 SOURCE DOWN */
static char     g_ov_cmd[48];                   /* 最近一次操作回显（"NEXT"/"VOL 60"…） */
static int64_t  g_ov_cmd_until_us;

/* ---------------- BGM 控制条辅助（仅合成/渲染任务调用） ---------------- */

static void ov_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_sw) w = g_sw - x;
    if (y + h > g_sh) h = g_sh - y;
    if (w <= 0 || h <= 0) return;
    for (int32_t r = y; r < y + h; r++) {
        uint16_t *drow = g_fb + (size_t)r * g_sw;
        for (int32_t c = x; c < x + w; c++) drow[c] = color;
    }
}

/* 5×7 字模文本（与横幅同库同缩放口径；仅 ASCII，小写转大写）
 * 先整趟 1px 描边再整趟填充：AMOLED 上纯色细字可读性差 */
static void ov_text(const char *s, int32_t x0, int32_t y0, int32_t scale,
                    uint16_t fg, uint16_t outline)
{
    if (!s) return;
    for (int pass = 0; pass < 2; pass++) {
        int32_t gx = x0;
        for (const char *p = s; *p && gx < g_sw; p++) {
            unsigned char u = (unsigned char)*p;
            if (u >= 'a' && u <= 'z') u -= 32;
            if (u >= 128) u = '?';
            const uint8_t *cols = MP_FONT5X7[u];
            for (int col = 0; col < MP_FONT_GLYPH_W; col++) {
                for (int row = 0; row < MP_FONT_GLYPH_H; row++) {
                    if (!(cols[col] & (1u << row))) continue;
                    int32_t px = gx + col * scale;
                    int32_t py = y0 + row * scale;
                    if (pass == 0) ov_fill(px - 1, py - 1, scale + 2, scale + 2, outline);
                    else           ov_fill(px, py, scale, scale, fg);
                }
            }
            gx += (MP_FONT_GLYPH_W + 1) * scale;
        }
    }
}

static int32_t ov_text_w(const char *s, int32_t scale)
{
    int32_t n = 0;
    if (s) for (const char *p = s; *p; p++) n++;
    return n > 0 ? n * (MP_FONT_GLYPH_W + 1) * scale : 0;
}

static const char *const RC_OV_LABELS[RC_OV_ITEMS] = {
    "PLAY", "PREV", "NEXT", "VOL -", "VOL +",
};

/* 光标切换（侧键/触摸） */
void render_bgm_bar_nav(int dir)
{
    rc_lock();
    if (!g_ov_on) { rc_unlock(); return; }
    g_ov_sel = (g_ov_sel + (dir > 0 ? 1 : RC_OV_ITEMS - 1)) % RC_OV_ITEMS;
    g_ov_expire_us = esp_timer_get_time() + (int64_t)RC_OV_AUTO_HIDE_MS * 1000;
    mark_rect(0, g_sh - RC_OV_H, g_sw, RC_OV_H);
    rc_unlock();
}

/** 执行第 idx 个控件（触摸点选与顶键确认共用）。 */
void render_bgm_bar_activate(int idx)
{
    rc_lock();
    if (!g_ov_on || idx < 0 || idx >= RC_OV_ITEMS) { rc_unlock(); return; }
    g_ov_sel = idx;
    switch (idx) {
    case 0: {
        bool playing = bgm_toggle_pause();
        snprintf(g_ov_cmd, sizeof(g_ov_cmd), "%s", playing ? "PLAYING" : "PAUSED");
        ESP_LOGI("bgmbar", "控制条：播放/暂停 → %s", g_ov_cmd);
        break;
    }
    case 1:
        bgm_prev();
        snprintf(g_ov_cmd, sizeof(g_ov_cmd), "PREV");
        ESP_LOGI("bgmbar", "控制条：上一首");
        break;
    case 2:
        bgm_next();
        snprintf(g_ov_cmd, sizeof(g_ov_cmd), "NEXT");
        ESP_LOGI("bgmbar", "控制条：下一首");
        break;
    case 3:
        bgm_volume_add(-10);
        snprintf(g_ov_cmd, sizeof(g_ov_cmd), "VOL %u", (unsigned)bgm_volume_get());
        ESP_LOGI("bgmbar", "控制条：音量 - → %u", (unsigned)bgm_volume_get());
        break;
    default:
        bgm_volume_add(+10);
        snprintf(g_ov_cmd, sizeof(g_ov_cmd), "VOL %u", (unsigned)bgm_volume_get());
        ESP_LOGI("bgmbar", "控制条：音量 + → %u", (unsigned)bgm_volume_get());
        break;
    }
    g_ov_cmd_until_us = esp_timer_get_time() + 1200000;   /* 回显 1.2s */
    g_ov_expire_us    = esp_timer_get_time() + (int64_t)RC_OV_AUTO_HIDE_MS * 1000;
    mark_rect(0, g_sh - RC_OV_H, g_sw, RC_OV_H);
    rc_unlock();
}

/** 呼出半屏控制条（宠物区长按 / 选择器 BGM 入口）。 */
void render_bgm_bar_show(void)
{
    rc_lock();
    if (!g_inited) { rc_unlock(); return; }
    if (!g_ov_on) {
        g_ov_sel = 0;
        ESP_LOGI("bgmbar", "半屏控制条呼出（上半屏仍显示宠物，高 %dpx）", RC_OV_H);
    }
    g_ov_on = true;
    g_ov_cmd[0] = 0;
    g_ov_cmd_until_us = 0;
    /* 快照 BGM 真实状态 */
    g_ov_state   = (uint8_t)bgm_get_state();
    g_ov_vol     = bgm_volume_get();
    g_ov_offline = bgm_source_greyed(bgm_get_source());
    strlcpy(g_ov_title, bgm_current_title(), sizeof(g_ov_title));
    g_ov_expire_us = esp_timer_get_time() + (int64_t)RC_OV_AUTO_HIDE_MS * 1000;
    mark_rect(0, g_sh - RC_OV_H, g_sw, RC_OV_H);
    rc_unlock();
}

void render_bgm_bar_hide(void)
{
    rc_lock();
    if (!g_ov_on) { rc_unlock(); return; }
    g_ov_on = false;
    ESP_LOGI("bgmbar", "控制条收起");
    mark_rect(0, g_sh - RC_OV_H, g_sw, RC_OV_H);
    rc_unlock();
}

bool render_bgm_bar_showing(void) { return g_ov_on; }
int  render_bgm_bar_sel(void)     { return g_ov_sel; }

/* 【E7】选择器内 BGM 入口的跨任务请求旗标（渲染任务置位 / input 任务消费） */
static volatile bool s_bar_req_from_menu;
void render_bgm_bar_request_from_menu(void) { s_bar_req_from_menu = true; }
bool render_bgm_bar_take_menu_request(void)
{
    if (!s_bar_req_from_menu) return false;
    s_bar_req_from_menu = false;
    return true;
}

/** 触摸命中测试：返回控件号（0..4）或 -1（未命中按钮；调用方按"点空白=收起"处理）。
 *  布局常量与 ov_draw 第 3 行严格同源（bx=16 / gap=8 / by=+86 / bh=62）。 */
int render_bgm_bar_item_at(int x, int y)
{
    if (!g_ov_on) return -1;
    int32_t py0 = g_sh - RC_OV_H;
    const int32_t gap = 8, bx = 16, by = py0 + 86, bh = 62;
    const int32_t bw = (g_sw - bx * 2 - gap * (RC_OV_ITEMS - 1)) / RC_OV_ITEMS;
    if (y < by || y >= by + bh) return -1;
    for (int i = 0; i < RC_OV_ITEMS; i++) {
        int32_t ix = bx + i * (bw + gap);
        if (x >= ix && x < ix + bw) return i;
    }
    return -1;
}

/** 控制条周期维护（渲染任务 tick）：过期收起 + 状态变化重绘。 */
static void ov_tick(int64_t now_us)
{
    if (!g_ov_on) return;
    /* 离线：BGM 静音降级中，控制条无意义 → 自动收起 */
    if (bgm_source_greyed(bgm_get_source())) {
        render_bgm_bar_hide();
        return;
    }
    bool need = false;
    /* 3s 无动作自动收起（上半屏仍要看宠物，不常驻占屏） */
    if (now_us >= g_ov_expire_us) {
        render_bgm_bar_hide();
        return;
    }
    uint8_t st  = (uint8_t)bgm_get_state();
    uint8_t vol = bgm_volume_get();
    const char *ti = bgm_current_title();
    if (st != g_ov_state || vol != g_ov_vol || strcmp(ti, g_ov_title) != 0) {
        g_ov_state = st;
        g_ov_vol   = vol;
        strlcpy(g_ov_title, ti, sizeof(g_ov_title));
        need = true;
    }
    if (g_ov_cmd_until_us && now_us >= g_ov_cmd_until_us) {
        g_ov_cmd[0] = 0;
        g_ov_cmd_until_us = 0;
        need = true;
    }
    if (need) mark_rect(0, g_sh - RC_OV_H, g_sw, RC_OV_H);
}

/* 面板绘制（在 compose_region 内被调用；x/y/w/h 是本次脏区裁剪框） */
static void ov_draw(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!g_ov_on) return;
    int32_t py0 = g_sh - RC_OV_H;
    int32_t py1 = g_sh;
    if (y + h <= py0 || y >= py1) return;      /* 脏区不涉及面板 */
    (void)w;

    ov_fill(0, py0, g_sw, RC_OV_H, RC_OV_PANEL_BG);
    ov_fill(0, py0, g_sw, 2, RC_OV_DIVIDER);   /* 顶边分隔线：与宠物区的视觉边界 */

    /* 第 1 行：曲目名（无表/未播放 → NO TRACK） */
    char line[64];
    const char *title = g_ov_title[0] ? g_ov_title : "NO TRACK";
    snprintf(line, sizeof(line), "BGM  %.26s", title);
    ov_text(line, 16, py0 + 14, 3, RC_OV_TITLE_FG, RC_OV_PANEL_BG);

    /* 第 2 行：真实状态（播放态 / 音量 / 音源） */
    {
        static const char *const stn[] = { "IDLE", "PLAYING", "PAUSED", "FAILED" };
        const char *st = (g_ov_state <= 3) ? stn[g_ov_state] : "?";
        snprintf(line, sizeof(line), "%s  VOL %u  SRC %.4s",
                 st, (unsigned)g_ov_vol, bgm_source_name());
        ov_text(line, 16, py0 + 46, 2, RC_OV_INFO_FG, RC_OV_PANEL_BG);
    }
    /* 操作回显（1.2s）右对齐第 2 行 */
    if (g_ov_cmd[0]) {
        int32_t wpx = ov_text_w(g_ov_cmd, 2);
        ov_text(g_ov_cmd, g_sw - 16 - wpx, py0 + 46, 2, RC_OV_ITEM_SEL, RC_OV_PANEL_BG);
    }

    /* 第 3 行：5 个控件横排（等宽；选中项实心底 + 前缀光标） */
    {
        const int32_t gap = 8;
        const int32_t bx  = 16;
        const int32_t bw  = (g_sw - bx * 2 - gap * (RC_OV_ITEMS - 1)) / RC_OV_ITEMS;
        const int32_t by  = py0 + 86;
        const int32_t bh  = 62;
        for (int i = 0; i < RC_OV_ITEMS; i++) {
            int32_t ix = bx + i * (bw + gap);
            bool sel = (i == g_ov_sel);
            ov_fill(ix, by, bw, bh, sel ? RC_OV_ITEM_SEL : RC_OV_ITEM_BG);
            /* 控件文字居中（> 光标额外占 2 字符宽，避免选中项文字跳动） */
            const char *lab = RC_OV_LABELS[i];
            int32_t lw = ov_text_w(lab, 2);
            int32_t tx = ix + (bw - lw) / 2;
            ov_text(lab, tx, by + (bh - MP_FONT_GLYPH_H * 2) / 2, 2,
                    RC_OV_ITEM_FG, sel ? RC_OV_ITEM_SEL : RC_OV_ITEM_BG);
            if (sel) ov_text(">", ix + 6, by + (bh - MP_FONT_GLYPH_H * 2) / 2, 2,
                             RC_OV_TITLE_FG, RC_OV_ITEM_SEL);
        }
    }

    /* 第 4 行：操作提示（侧键口径与菜单一致） */
    ov_text("TOP:OK  MID:MOVE  BOT/UP-TAP:EXIT", 16, py0 + RC_OV_H - 30, 2,
            RC_OV_HINT_FG, RC_OV_PANEL_BG);
}


/* IMU 视差（input 任务异步写；对齐 int32 写原子） */
static volatile int32_t g_tilt_mdeg;
static volatile int32_t g_drag_off_x;
static bool     g_calib_on;                 /* 【校准模式】红线坐标系 + 触摸落点回显 */
static int16_t  g_calib_touch_x = -1;       /* 最近一次触摸落点（-1=无） */
static int16_t  g_calib_touch_y = -1;       /* 最近一次触摸落点 y（-1=无） */
static volatile int32_t g_drag_off_y;   /* 拖拽：人物屏幕 y 偏移 */
static bool     g_stand_done;              /* 首次画布就绪是否已"站到地面 tile 线上" */
static int32_t s_last_ent_tilt;            /* 实体已按此 tilt 值摆放（问题7 跟随标脏） */

/* 脏区网格（16×16 标记；问题1：合帧后合并为单一包围盒上屏） */
static uint8_t  g_mark[RC_GRID_MAX];
static volatile uint32_t g_mark_calls;   /* 标脏调用计数（兜底清屏判据） */
static int      g_gw, g_gh;
static time_t   g_last_clock_t;

/* ================= 小工具 ================= */

static void *psram(size_t n);

/* LE framebuffer → 驱动大端 RGB565 的上屏边界（定义见脏区节） */
static void blit_be(int32_t x, int32_t y, int32_t w, int32_t h,
                    const uint16_t *src, int32_t src_stride);

static void *psram(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) ESP_LOGE(TAG, "PSRAM alloc %zu failed", n);
    return p;
}

/* 实体显示尺寸：画布宽高 ×2（clamp 到缓冲与屏幕；画布未算出前为 0 → 无显示区） */
static void ent_disp_size(int32_t *dw, int32_t *dh)
{
    *dw = g_ent_cw * RC_SCALE;
    *dh = g_ent_ch * RC_SCALE;
    if (*dw > RC_ENT_W) *dw = RC_ENT_W;
    if (*dh > RC_ENT_H) *dh = RC_ENT_H;
    if (*dw > g_sw) *dw = g_sw;
    if (*dh > g_sh) *dh = g_sh;
}

/* 倾斜/拖拽 → 实体 x 偏移可见反馈（问题6/7）：±8° ↔ ±8px */
static int32_t ent_tilt_off_px(int32_t tilt_mdeg)
{
    int32_t px = (tilt_mdeg / 1000) * RC_TILT_ENT_PX_PER_DEG;
    if (px > RC_TILT_ENT_MAX_PX) px = RC_TILT_ENT_MAX_PX;
    if (px < -RC_TILT_ENT_MAX_PX) px = -RC_TILT_ENT_MAX_PX;
    return px;
}

/* ══ 地面线（降级方案：把 000010000 的 foothold 直接写进板子）══════════════════
 * 用户口径：「读取 000010000 的 foothold」「foothold 是红线，但你在红框」「我只要求
 * 初始化的时候看起来像（站在地上），后面不需要这个功能」「先不考虑 NAS，先实验做一个，
 * 直接烧录到板子里」。
 *
 * 数据来源：本仓库 Exporter CLI `--dump-footholds 000010000`（只读 WZ，不动服务端、
 * 不重新导出素材），取 **foothold 第 0 层里位于地面高度带的那 9 段**（world→设备视口
 * 1x 坐标，固件 ×RC_SCALE 得设备像素）。**已剔除**高处的树冠/屋顶平台（视口 y≈-380，
 * 那会把人物顶到屏幕上方）。
 *
 * 实测（主机端把整层渲染在候选相机上逐一出图，与设备实际画面比差）：
 *   最接近的相机 = camY 11.5（平均差 2.96，邻档 16~31）→ 设备画面就是
 *   世界 y ∈ [-108.5, 131.5] 这 240 世界像素（2x = 480）。而本图地面 foothold 在
 *   world y≈245.5 = 视口 y 354 = **设备 y 708 —— 比屏幕下沿还低 228px**，
 *   即"红线那层地面"根本不在画面里（画面底部是它上面的山坡草地）。
 *   ⇒ 固件把 foothold 线**夹进画面**（下沿留 RC_GROUND_UP_PX=20px）→ 脚底 y=460。
 *   要让真 foothold 出现在画面里，必须把导出相机下移 ~114 世界像素重导地图包（NAS 那一步，
 *   用户已指示先不做）。 */
#define RC_GROUND_UP_PX 20                 /* 站立线距屏底的最小留白 */
typedef struct { int16_t x1, y1, x2, y2; } mp_ground_seg_t;   /* 设备像素坐标 */
/* 000010000 地面 foothold（9 段，设备像素；x 已 ×2、y 已 ×2） */
static const mp_ground_seg_t kGround000010000[] = {
    {  320, 708,  500, 708 },
    {  -40, 708,  140, 708 },
    {  140, 708,  320, 708 },
    { -580, 708, -400, 708 },
    { -220, 708,  -40, 708 },
    { -400, 708, -220, 708 },
    {  500, 708,  680, 708 },
    {  860, 828, 1040, 948 },
    {  680, 708,  860, 828 },
};
#define MP_GROUND_SEG_N ((int)(sizeof kGround000010000 / sizeof kGround000010000[0]))
static const char kGroundMapId[] = "000010000";

/* ══ 【相机下移实验 2026-09-27】把画面下移到真 foothold 那层地面 ═══════════════
 * 用户问："不能把整张地图都渲染进去，然后调整位置？"——整图放不下（2270x1807 世界像素
 * ×2B = 8.2MB > PSRAM 8MB / 素材分区 6MB），但缺的只是**当前视口下方那一条**：
 * 主机端用 Exporter（同一个相机、视口加高到 500 行后裁剪）烘出 240x130 的"地面带"
 * （见 /tmp/ground_band.png：泥土路沿 + 草 + 枫叶），以二进制内嵌进固件
 * （main/CMakeLists.txt target_add_binary_data），装载地图时把 static/tile/掩码整体
 * **上移 MP_GROUND_CAM_SHIFT_PX 行**、底部缺失行用地面带补上，条带 y 相应上移半个位移
 * （世界 1x）。于是画面 = 世界 y ∈ [15.5, 255.5]，真 foothold（world 245.5）正好落到
 * 设备 y=460 = 屏底-20 ✓ 人物脚底就踩在红线那层地面上。
 * 关掉本实验：把 MP_GROUND_CAM_SHIFT_PX 置 0 即可（代码路径整体跳过）。 */
/* 【按板定相机下移 2026-10-01】原为全局宏 MP_GROUND_CAM_SHIFT_PX——两个开发
 * 会话共用工作区时互相覆盖（板B实验改 248，板A构建被动带上 → 板A"背景变了"
 * 用户报障）。改为 profile 字段 ground_cam_shift_px：本板（lcd185b）=248
 * 保留其实验；216 板定稿值 0（用户定稿：图2 蘑菇屋场景+宠物站地面线即正确）。
 * render_init 从 profile 装载；185B 单板分支恒取 profile 值。 */
static int32_t s_ground_shift_px;       /* 0=实验关闭（216 板定稿值） */

extern const uint8_t ground_band_000010000_bin_start[] asm("_binary_ground_band_000010000_bin_start");
#define MP_GROUND_BAND_W 240
#define MP_GROUND_BAND_H 130
static bool g_ground_tbl_on;               /* 当前地图是否命中地面表 */

/* ══ 拖拽范围：**整只宠物必须留在屏内** ═══════════════════════════════════
 * 【2026-09-27 用户报障"头明显锁了一块 + 一条明显的分割线"的第二个真凶】
 * 取证（真机串口探针）：故障时刻 `drag=(61,172)`、`落点=(301,412 179x68)`——
 * 即人物被上一次拖拽留在右下角，显示矩形 214x168 只有 179x68 落在屏内，
 * 头像被屏幕边缘切出一个**直角块**（黑底上就是"头缺一块 + 分割线"）。
 * 旧的 ±屏宽/±屏高 夹取允许人物被拖到几乎完全出屏且**松手后原地保留**，
 * 用户无法区分"被屏幕边缘裁掉"与"渲染坏了"。
 *
 * 现口径：跟手拖动 1:1，但显示矩形整块夹在屏内——屏幕四边都能到达
 * （214x168 的人物在 480x480 上仍有 266x312 的活动范围，"可以全屏拖动"成立），
 * 且任何位置都不会被切成块。tilt 的 ±8px 视差偏移一并计入，避免倾斜时越界。 */
static void drag_clamp(int32_t *px, int32_t *py)
{
    int32_t dw, dh;
    ent_disp_size(&dw, &dh);
    if (dw <= 0 || dh <= 0) return;              /* 画布未就绪：不限制 */
    /* 未加 drag 时的基准（与 ent_screen_pos_at 同源，含 tilt 与 base 偏移） */
    int32_t base_x = g_sw / 2 - (g_ent_ox - g_ent_cx0) * RC_SCALE + RC_ENT_CENTER_OFF_X
                     + (g_ent_base_wx << RC_SCALE_SHIFT)
                     + ent_tilt_off_px(g_tilt_mdeg);
    int32_t base_y = g_sh / 2 - (g_ent_oy - g_ent_cy0) * RC_SCALE + RC_ENT_CENTER_OFF_Y
                     + (g_ent_base_wy << RC_SCALE_SHIFT);
    if (px) {
        int32_t lo = -base_x, hi = g_sw - dw - base_x;
        if (hi < lo) hi = lo;                    /* 画布比屏还宽：贴左 */
        if (*px < lo) *px = lo;
        if (*px > hi) *px = hi;
    }
    if (py) {
        /* 【拖动下限 = 地面 tile 表面线 2026-09-27】口径演进：
         *   ① 最初按**画布矩形**夹取（脚底悬空 ~22px）；
         *   ② 后改为按 body 锚点夹到**屏幕最底**（脚底踩屏底 480）；
         *   ③ 【已回滚 2026-09-27】曾把下界改成"该 x 的地面线"——用户明确未要求
         *      （"拖拽下界/松手落回地面线 这两个我没要求不要乱加"），故恢复为
         *      **下界 = 屏幕最底**（脚底可一直拖到屏底，可全屏拖动）。
         * 上界 lo 仍让画布顶边不越屏顶（不许把整只宠物拖出画面）。 */
        int32_t anchor_off = (g_ent_oy - g_ent_cy0) * RC_SCALE;   /* 锚点在显示矩形内的 y */
        int32_t lo = -base_y;
        int32_t hi = g_sh - anchor_off - base_y;
        if (hi < lo) hi = lo;
        if (*py < lo) *py = lo;
        if (*py > hi) *py = hi;
    }
}

void render_set_drag_off(int32_t px)
{
    /* 与 render_tick 的"标脏→合成→上屏"互斥：否则标脏用的是旧位置、合成读到的
     * 却是新位置 → 差集区域永久残留（真机拖影自检实证：手指一按就开始报残留）。 */
    rc_lock();
    drag_clamp(&px, NULL);
    g_drag_off_x = px;
    rc_unlock();
}

int32_t render_get_drag_off(void) { return g_drag_off_x; }

void render_set_drag_off_y(int32_t py)
{
    rc_lock();
    drag_clamp(NULL, &py);
    g_drag_off_y = py;
    rc_unlock();
}

int32_t render_get_drag_off_y(void) { return g_drag_off_y; }

/* ══ 站立线 / 地面线（详见上方 kGroundY000010000 的降级说明）═════════════════
 * 人物 body 锚点 origin（= 脚底基准，与桌面版 RenderFrame 同一契约）恒落在地面线上：
 *   · 初始上电 / 换地图 → 站到地面线；
 *   · 拖拽下界 = 地面线（脚底不会沉进地面），上界仍到屏顶（"全屏拖动"保持）；
 *   · **松手落回地面线**（render_settle_on_ground）→ 任何时候都像站在地上。 */

/* 锚点(脚底)屏幕 x = 屏心 + CENTER_OFF_X + base_wx×2 + tilt + drag_x
 * （与 ent_screen_pos_at 同源：画布原点项在锚点上相互抵消） */
static int32_t ent_anchor_screen_x(int32_t drag_x)
{
    return g_sw / 2 + RC_ENT_CENTER_OFF_X + (g_ent_base_wx << RC_SCALE_SHIFT)
           + ent_tilt_off_px(g_tilt_mdeg) + drag_x;
}

/* 该 x 处的地面线（设备像素 y）：foothold 段内取最高（最小 y）的折线，按 x 线性插值；
 * 命中不到段 → 通用线；最后**夹进画面**（下沿留 RC_GROUND_UP_PX）——见上方实测说明。 */
static int32_t ground_line_y_at(int32_t dev_x)
{
    int32_t best = -1;
    if (g_ground_tbl_on) {
        for (int i = 0; i < MP_GROUND_SEG_N; i++) {
            const mp_ground_seg_t *g = &kGround000010000[i];
            int32_t xa = g->x1, xb = g->x2, ya = g->y1, yb = g->y2;
            if (xa > xb) { int32_t t = xa; xa = xb; xb = t; t = ya; ya = yb; yb = t; }
            if (dev_x < xa || dev_x > xb) continue;
            int32_t y = (xb == xa) ? ya : ya + (int32_t)((int64_t)(dev_x - xa) * (yb - ya) / (xb - xa));
            if (best < 0 || y < best) best = y;      /* 最高 = 站得住的那条 */
        }
    }
    if (best < 0) best = g_sh;                        /* 无段覆盖 → 通用线（屏底） */
    int32_t lim = g_sh - RC_GROUND_UP_PX;             /* 夹进画面：最多到"屏底-20" */
    if (best > lim) best = lim;
    if (best < 0) best = 0;
    return best;
}

static int32_t ground_line_y(void) { return ground_line_y_at(ent_anchor_screen_x(g_drag_off_x)); }

/* 站到地面线上（需持锁）。返回是否真的改了位置。 */
static bool ent_stand_on_ground_locked(void)
{
    if (!g_inited || !g_ent_cbox_ok) return false;
    /* 锚点屏幕 y = 屏心 + CENTER_OFF_Y + base_wy×2 + drag_y ⇒ 令其等于地面线即得 drag_y */
    int32_t line = ground_line_y();
    int32_t dy = line - (g_sh / 2 + RC_ENT_CENTER_OFF_Y + (g_ent_base_wy << RC_SCALE_SHIFT));
    drag_clamp(NULL, &dy);            /* 兜底夹取（下界=同一地面线） */
    if (dy == g_drag_off_y) return false;
    g_drag_off_y = dy;
    mark_rect(0, 0, g_sw, g_sh);      /* 位置变了：整屏重合成（罕见事件） */
    ESP_LOGI(TAG, "站位：脚底 y=%d（地面线来源=%s，锚点 x=%d）drag_y=%d",
             (int)line, g_ground_tbl_on ? "本图地面表" : "通用线(屏底-20)",
             (int)ent_anchor_screen_x(g_drag_off_x), (int)dy);
    return true;
}


/* 【校准模式】红线=固件认为的底边(y=440)+竖直中线(x=240)；白点=最近触摸落点 */
void render_calib_set(bool on, int16_t tx, int16_t ty)
{
    rc_lock();
    g_calib_on = on;
    if (tx >= 0) { g_calib_touch_x = tx; g_calib_touch_y = ty; }
    mark_rect(0, 0, g_sw, g_sh);             /* 叠加层变化 → 全屏重绘 */
    rc_unlock();
}

/* 实体缓冲 → 屏幕摆放（问题2 修复）：
 * 世界 1x body 锚点 (0,0)（= 人物脚底基准，piece x/y 的原点）2x 后——
 *   水平钉在屏幕中心（+调参偏移 + tilt 可见偏移），
 *   垂直钉在 屏底-40px。
 * 不再按画布联合包围盒居中/贴底（武器/翅膀大件撑大包围盒会把人物挤偏左上）。
 * g_ent_base_wx/wy 为世界 1x 附加偏移（render_set_entity_pos，默认 0,0）。 */
static void ent_screen_pos_at(int32_t tilt_mdeg, int32_t *sx, int32_t *sy)
{
    /* ══ 落点定稿（用户三条要求，2026-09-27 最终版）══
     * 要求：① 初始化时 **origin 与屏幕中点重合**；② 能全屏拖动；③ 正常渲染。
     *
     * 关键事实（此前几版都栽在这里）：实体缓冲固定 480×440，而**画布坐标
     * (cx0,cy0) 与缓冲坐标是同一坐标系**——缓冲内下标 = (piece - cx0)*2。
     * 所以"可见内容"占据缓冲的 (0,0)-(2*cw,2*ch)，而缓冲其余部分是空的。
     * 之前把缓冲**左上角**摆到"屏心 + 画布原点"处：当 cy0=0 时缓冲顶边被摆到
     * sy=440，整块 buffer 只有最上面 40px 落在屏内 → 屏幕上只剩"底边一条"。
     *
     * 正确摆法分两步：
     *   1) 缓冲整体居中：bx = (480 - RC_ENT_W)/2 = 0、by = (480 - RC_ENT_H)/2 = 20
     *   2) 让**人物 origin（画布坐标原点）**落在屏幕中点：
     *      origin 在缓冲内的位置 = (0 - cx0)*2, (0 - cy0)*2
     *      ⇒ sx = 240 + (0-cx0)*2 = 240 - 2*cx0，sy = 240 - 2*cy0
     *
     * 于是：origin 恒在屏心 ✓  内容按真实包围盒在四周展开（不再被顶到屏外）✓
     * 拖动由 drag_off 叠加，可全屏拖（范围在 render_set_drag_off* 里放开）。 */
    int32_t drag_x = g_ent_cbox_ok ? g_drag_off_x : 0;
    int32_t drag_y = g_ent_cbox_ok ? g_drag_off_y : 0;

    /* 锚点在缓冲内的位置 = (origin - 画布左上)×scale；把它钉到屏心 ⇒
     * 缓冲左上角屏幕坐标 = 屏心 - (origin - 画布左上)×scale。 */
    *sx = g_sw / 2 - (g_ent_ox - g_ent_cx0) * RC_SCALE + RC_ENT_CENTER_OFF_X
          + (g_ent_base_wx << RC_SCALE_SHIFT) + ent_tilt_off_px(tilt_mdeg)
          + drag_x;
    *sy = g_sh / 2 - (g_ent_oy - g_ent_cy0) * RC_SCALE + RC_ENT_CENTER_OFF_Y
          + (g_ent_base_wy << RC_SCALE_SHIFT) + drag_y;
}

static void ent_screen_pos(int32_t *sx, int32_t *sy)
{
    ent_screen_pos_at(g_tilt_mdeg, sx, sy);
}

/* 实体屏幕矩形（唯一权威）：mark 网格标脏 / compose 区域绘制 / display_blit
 * 上窗三者必须同源。规则：「先 clamp 实体矩形到屏幕内，再交给后续路径」——
 * 右/下缘向内收尾（clamp 到 g_sw/g_sh），绝不向外扩张越界。
 * 右缘 480 残影根修：此前 mark_rect 内部 clamp、compose_region 的 min/max
 * 收缩、display_blit 的 even_round 向外取偶三路各自为政，实体矩形右缘越过
 * x=480 时（drag_off 靠近 +160）矩形分歧 → 残影。
 * 输出：bx/by = 实体缓冲原点（未 clamp，可负/越屏，仅供 eidx=sx-bx 索引缓冲）；
 *       ex/ey/dw/dh = 屏内显示窗 [ex,ex+dw)×[ey,ey+dh)（完全出屏时 dw/dh=0）。 */
static void ent_screen_rect_at(int32_t tilt_mdeg,
                               int32_t *bx, int32_t *by,
                               int32_t *ex, int32_t *ey,
                               int32_t *dw, int32_t *dh)
{
    ent_screen_pos_at(tilt_mdeg, bx, by);
    ent_disp_size(dw, dh);
    *ex = *bx; *ey = *by;
    int32_t x1 = *ex + *dw, y1 = *ey + *dh;
    if (*ex < 0) *ex = 0;               /* 左/上缘：向内收 */
    if (*ey < 0) *ey = 0;
    if (x1 > g_sw) x1 = g_sw;           /* 右/下缘：向内收尾，先 clamp 再谈对齐 */
    if (y1 > g_sh) y1 = g_sh;
    *dw = x1 - *ex;
    *dh = y1 - *ey;
    if (*dw <= 0 || *dh <= 0) { *dw = 0; *dh = 0; }
}

static void mark_rect_locked(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!g_inited) return;
    int32_t x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_sw) x1 = g_sw;
    if (y1 > g_sh) y1 = g_sh;
    if (x >= x1 || y >= y1) return;
    int cx0 = (int)(x / RC_CELL), cy0 = (int)(y / RC_CELL);
    int cx1 = (int)((x1 - 1) / RC_CELL), cy1 = (int)((y1 - 1) / RC_CELL);
    if (cx1 >= g_gw) cx1 = g_gw - 1;
    if (cy1 >= g_gh) cy1 = g_gh - 1;
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++)
            g_mark[cy * g_gw + cx] = 1;
    g_mark_calls++;          /* 兜底清屏用：见 render_tick 的 1s 全屏重合成 */
}

/* 跨任务标脏入口（互斥见文件头 s_rlock 说明）：与 flush 的清零/合成互斥，
 * 否则"标脏→合并 bbox"之间被截断会丢脏区 → 残留像素永不重绘。 */
static void mark_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    rc_lock();
    mark_rect_locked(x, y, w, h);
    rc_unlock();
}

static void mark_ent_at(int32_t tilt_mdeg)
{
    int32_t bx, by, ex, ey, dw, dh;
    ent_screen_rect_at(tilt_mdeg, &bx, &by, &ex, &ey, &dw, &dh);
    (void)bx; (void)by;                  /* 缓冲原点仅 compose 索引用 */
    /* 探针（上屏侧嫌疑）：标脏包围盒变化才打 LOGD（1s 限频，防 tilt 连续
     * 变化刷屏；诊断期已结束，默认级别不输出）——帧静止排查时看这里：
     * bbox 应稳定覆盖实体显示区 */
    static int32_t lx, ly, lw, lh;
    static int64_t llog_us;
    int64_t now_us = esp_timer_get_time();
    if (ex != lx || ey != ly || dw != lw || dh != lh) {
        lx = ex; ly = ey; lw = dw; lh = dh;
        if (now_us - llog_us > 1000000) {
            llog_us = now_us;
            ESP_LOGD(TAG, "mark_ent bbox (%" PRId32 ",%" PRId32 ") %" PRId32
                     "x%" PRId32, ex, ey, dw, dh);
        }
    }
    /* 已 clamp 的屏内矩形（与 compose_region 实体步同一来源）；
     * 完全出屏（dw/dh=0）时 mark_rect 内部越界门同样拦截，双保险 */
    if (dw > 0 && dh > 0) mark_rect_locked(ex, ey, dw, dh);
}

static void mark_ent(void)
{
    mark_ent_at(g_tilt_mdeg);
}

/* ================= 可见性自证探针 =================
 * 真机「rc_anim_advance 帧推进正常（dbg 0→1→2 @300ms）但屏上人物静止」的
 * 二分判据。诊断期已结束：全部降为 DEBUG 级（默认 INFO 不输出），
 * 仅指纹连续 20 帧不变仍保留 WARN 一条（动画真坏时不至于无声），不改变渲染行为：
 *   1) 数据侧：recompose 后对实体缓冲取 FNV-1a 指纹（ent_fb_hash）——
 *      hash 变化 → 帧数据链正确，嫌疑上移 标脏/合成/blit（mark_ent_at /
 *      flush_dirty 探针）；hash 连续 20 帧不变 → WARN 一条「位图与上帧相同」
 *      并列出本帧 piece 的 part_id + 位图指纹：part_id 相同 = 设备上 LAYOUT
 *      帧表坍缩（解析/数据问题）；part_id 不同但位图指纹相同 = PARTS 包数据
 *      共享/坏偏移（导出端问题）；part_id 不同且指纹不同 = 合成器未重绘
 *      （不可能，指纹即证明）。
 *   2) 上屏侧：mark_ent_at 包围盒变化打 LOGD（1s 限频）；flush_dirty 首次
 *      执行打 LOGD 一次（证明 标脏→合成→blit_be→display_blit 链路跑通），
 *      之后 rect 稳定时静默（DEBUG）。 */

static uint32_t rc_bytes_fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* 实体缓冲指纹（帧推进后应变化；memset 清零 + 覆盖位同写保证指纹唯一对应内容） */
static uint32_t ent_fb_hash(void)
{
    return rc_bytes_fnv((const uint8_t *)g_ent_px,
                        (uint32_t)RC_ENT_W * RC_ENT_H * 2u);
}
static uint32_t s_ent_fb_hash_last;

/* ================= 部件缓存（懒加载） ================= */

static const rc_part_img_t *pc_get(const mpak_part_t *meta)
{
    for (rc_part_img_t *e = g_pc; e; e = e->next)
        if (e->meta->id == meta->id) return e;

    uint32_t pb = (uint32_t)meta->h * rc_align4((uint32_t)meta->w * 2u);
    uint32_t mb = meta->has_alpha ? meta->mask_bytes : 0;
    if (g_pc_bytes + pb + mb > RC_PART_CACHE_CAP) {
        if (!g_pc_cap_logged) {
            ESP_LOGE(TAG, "part cache cap %" PRIu32 " exceeded", (uint32_t)RC_PART_CACHE_CAP);
            g_pc_cap_logged = true;
        }
        return NULL;
    }

    rc_part_img_t *e = malloc(sizeof *e);
    if (!e) return NULL;
    e->meta = meta;
    e->px   = psram(pb);
    e->mask = mb ? psram(mb) : NULL;
    if (!e->px || (mb && !e->mask)) {
        if (e->px) heap_caps_free(e->px);
        if (e->mask) heap_caps_free(e->mask);
        free(e);
        return NULL;
    }
    if (mpak_part_read_pixels(&g_parts, meta, (uint8_t *)e->px, pb) != MPAK_OK ||
        (mb && mpak_part_read_mask(&g_parts, meta, e->mask, mb) != MPAK_OK)) {
        ESP_LOGE(TAG, "part %u read failed", meta->id);
        heap_caps_free(e->px);
        if (e->mask) heap_caps_free(e->mask);
        free(e);
        return NULL;
    }
    e->next = g_pc;
    e->px_hash = rc_bytes_fnv((const uint8_t *)e->px, pb);
    /* 【取证探针 2026-09-27】掩码指纹 + 非零位数：主机端用同一 mpk 文件解码
     * 出的同名字段应完全一致。位数为 0 = 该件不出像素；has_alpha=0（mask==NULL）
     * 时整块位图按不透明贴 = 会把位图里的黑底当内容画上去（真机"头缺一块"嫌疑）。 */
    e->mask_hash = e->mask ? rc_bytes_fnv(e->mask, mb) : 0u;
    e->mask_bits = 0;
    for (uint32_t i = 0; e->mask && i < mb; i++)
        e->mask_bits += (uint32_t)__builtin_popcount(e->mask[i]);
    g_pc = e;
    g_pc_bytes += pb + mb;
    return e;
}

static void pc_flush(void)
{
    rc_part_img_t *e = g_pc;
    while (e) {
        rc_part_img_t *n = e->next;
        heap_caps_free(e->px);
        if (e->mask) heap_caps_free(e->mask);
        free(e);
        e = n;
    }
    g_pc = NULL;
    g_pc_bytes = 0;
    g_pc_cap_logged = false;
}

/* ================= 实体画布/布局接线 ================= */

static const mpak_layout_t *active_layout(void)
{
    if (g_lt_once_ok) return g_lt_once.u.layout;
    if (g_lt_loop_ok) return g_lt_loop.u.layout;
    return NULL;
}

/* parts/layout 任一变化后失效，下次重合成前按新数据现算 */
static void ent_canvas_invalidate(void)
{
    g_ent_cbox_ok = false;
}

/* 扫描当前布局全部帧的 piece 矩形（x/y 与 part w/h），求联合包围盒。
 * 语义（LayoutPackWriter）：设备画布 = 全 piece 矩形联合，起点 = min(x/y)；
 * 画布尺寸不落 manifest 时按此联合包围盒取（对齐桌面 GetBounds 的 union 画布）。 */
static void ent_canvas_update(void)
{
    if (g_ent_cbox_ok) return;
    g_ent_cw = 0; g_ent_ch = 0;
    const mpak_layout_t *lt = active_layout();
    if (!lt || !g_parts_ok) return;

    int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool any = false;
    for (uint32_t f = 0; f < lt->frame_count; f++) {
        const mpak_frame_t *fr = &lt->frames[f];
        for (uint32_t k = 0; k < fr->piece_count; k++) {
            const mpak_piece_t *pc = &lt->pieces[fr->piece_off + k];
            const mpak_part_t *meta = mpak_parts_find(&g_parts, pc->part_id);
            if (!meta) continue;
            int32_t x0 = pc->x, y0 = pc->y;
            int32_t x1 = x0 + (int32_t)meta->w, y1 = y0 + (int32_t)meta->h;
            if (!any) {
                minX = x0; minY = y0; maxX = x1; maxY = y1;
                any = true;
            } else {
                if (x0 < minX) minX = x0;
                if (y0 < minY) minY = y0;
                if (x1 > maxX) maxX = x1;
                if (y1 > maxY) maxY = y1;
            }
        }
    }
    if (!any) return;
    int32_t cw = maxX - minX, ch = maxY - minY;
    int32_t cx0 = minX, cy0 = minY;
    /* 画布超出缓冲窗口的退化情形：横向取以 body 锚点(0,0)为中心的窗口
     * （人物保持居中，武器/翅膀大件外溢裁剪）；纵向取底部窗口（保脚底对齐） */
    int32_t max_w = RC_ENT_W / RC_SCALE, max_h = RC_ENT_H / RC_SCALE;
    if (cw > max_w) { cw = max_w; cx0 = -max_w / 2; }
    if (ch > max_h) { ch = max_h; cy0 = maxY - max_h; }
    g_ent_cx0 = cx0;
    g_ent_cy0 = cy0;
    g_ent_cw = cw;
    g_ent_ch = ch;
    g_ent_cbox_ok = true;
    /* 画布内 body 锚点：从清单里按动作名取（桌面 RenderFrame 的 origin 同口径）。
     * 缺字段（旧清单/未下发）→ 0,0，等价于"画布左上角对齐屏心"的旧行为。 */
    {
        extern bool asset_dl_layout_origin(const char *action, int16_t *x, int16_t *y);
        int16_t ox = 0, oy = 0;
        if (asset_dl_layout_origin(lt->action, &ox, &oy)) {
            g_ent_ox = ox; g_ent_oy = oy;
        } else {
            g_ent_ox = 0; g_ent_oy = 0;
        }
    }
    /* 画布尺寸/原点变了 → 旧 drag 偏移可能已把人物顶出屏（换动作/换装后
     * 尺寸不同）：按新尺寸重新夹取，保证任何时刻都整只留在屏内。
     * 【首次就绪 = 站到地面线上 2026-09-27】上电后画布第一次算出来时，人物应
     * 直接站在地面 tile 表面线上（而不是悬在屏心）；此后只有在换地图时才再次
     * 归位（见 render_set_map_nolock），用户拖过的位置不会被动作切换重置。 */
    {
        bool first_ready = !g_stand_done;
        int32_t dx = g_drag_off_x, dy = g_drag_off_y;
        drag_clamp(&dx, &dy);
        g_drag_off_x = dx; g_drag_off_y = dy;
        if (first_ready) { g_stand_done = true; ent_stand_on_ground_locked(); }
    }
    ESP_LOGD(TAG, "ent canvas union origin(%" PRId32 ",%" PRId32 ") %"
             PRId32 "x%" PRId32, g_ent_cx0, g_ent_cy0, g_ent_cw, g_ent_ch);
}

static void bind_active_layout(int64_t now_us, bool reset_expr)
{
    const mpak_layout_t *lt = active_layout();
    if (!lt) return;
    rc_anim_bind(&g_anim, lt, !g_lt_once_ok, reset_expr, now_us);
    ent_canvas_invalidate();
}

/* ================= 实体层合成 ================= */

static const rc_part_img_t *resolve_piece(const mpak_piece_t *piece)
{
    const mpak_part_t *meta = mpak_parts_find(&g_parts, piece->part_id);
    if (!meta) {
        /* 【串口阻塞熔断修复 2026-09-27】这里原来是 ESP_LOGW，且**每帧每个部件**
         * 都会命中：PARTS 缺失时 30fps × 16 件 = 480 条/秒。真机在没人读串口时
         * UART TX 环形缓冲会塞满，日志写变成阻塞 → 渲染任务卡死 → E14 看门狗
         * 三振熔断（`已关屏待机，需物理断电`）。降为 DEBUG：默认级别下零输出，
         * 排障时把 LOG_DEFAULT_LEVEL 调到 Debug 才可见。 */
        ESP_LOGD(TAG, "piece part %u not in PARTS pkg", piece->part_id);
        return NULL;
    }
    if (piece->expr_index != MPAK_EXPR_NONE && meta->expr_group != 0) {
        int32_t expr = rc_anim_active_expr(&g_anim);
        const mpak_part_t *v = mpak_parts_variant(&g_parts, meta, (uint32_t)expr);
        if (v) {
            /* 探针（帧静止嫌疑1）：变体替换把 piece 映射到组内其他位图。
             * 正常仅 face 表情件走这里（导出契约：body 件 expr_index=255/
             * expr_group=0）；若不同帧的 part 被映射到同一位图，由 ent fb
             * hash 探针的 WARN 暴露 */
            if (v->id != meta->id)
                ESP_LOGD(TAG, "piece part %u -> expr variant %u (expr %d)",
                         piece->part_id, v->id, (int)expr);
            meta = v;
        } else {
            ESP_LOGW(TAG, "piece part %u expr group %u variant %d missing"
                     " (keep base bitmap)", piece->part_id, meta->expr_group,
                     (int)expr);
        }
    }
    return pc_get(meta);
}

/* 2x nearest blit 进实体缓冲（含 1bit 掩码、水平翻转、2×2 块展开、覆盖位） */
static void blit_ent_2x(const rc_part_img_t *img, bool hflip, int32_t bx, int32_t by)
{
    const uint16_t w = img->meta->w, h = img->meta->h;
    const uint32_t stride_el = rc_align4((uint32_t)w * 2u) / 2u;

    for (uint32_t sy = 0; sy < h; sy++) {
        const uint16_t *srow = img->px + (size_t)sy * stride_el;
        int32_t Y0 = by + (int32_t)(sy << RC_SCALE_SHIFT);
        if (Y0 + 1 < 0 || Y0 >= RC_ENT_H) continue;
        for (uint32_t sx = 0; sx < w; sx++) {
            if (img->mask && !rc_mask_bit(img->mask, sy * w + sx)) continue;
            uint32_t sxx = hflip ? (uint32_t)(w - 1 - sx) : sx;
            uint16_t c = srow[sxx];
            int32_t X0 = bx + (int32_t)(sx << RC_SCALE_SHIFT);
            for (int32_t dy = 0; dy < RC_SCALE; dy++) {
                int32_t Y = Y0 + dy;
                if (Y < 0 || Y >= RC_ENT_H) continue;
                for (int32_t dx = 0; dx < RC_SCALE; dx++) {
                    int32_t X = X0 + dx;
                    if (X < 0 || X >= RC_ENT_W) continue;
                    uint32_t idx = (uint32_t)Y * RC_ENT_W + (uint32_t)X;
                    g_ent_px[idx] = c;
                    rc_mask_set(g_ent_cov, idx);
                }
            }
        }
    }
}

static void recompose_entity_locked(void)
{
    memset(g_ent_px, 0, (size_t)RC_ENT_W * RC_ENT_H * 2u);
    memset(g_ent_cov, 0, RC_ENT_COV_BYTES);

    /* RAMless 面板（185B）的持续刷新帧源注册 */
    display_set_frame_locks(compositor_frame_lock, compositor_frame_unlock);
    display_set_frame_source((const uint16_t *)g_fb, g_sw);

    const mpak_layout_t *lt = active_layout();
    if (!lt || !g_parts_ok) return;
    if (g_anim.frame_idx >= lt->frame_count) return;
    ent_canvas_update();

    const mpak_frame_t *fr = &lt->frames[g_anim.frame_idx];
    /* 【渲染取证 2026-09-27】用户报"人物渲染不对（白块+红线）"且无 not-in-PARTS
     * 告警 → 说明部件都能解析，但画出来的东西不对。这里每 3s 打一条"本帧画了多少
     * piece、画布多大、覆盖多少像素"，把"是不是只画了少数几件/画布尺寸不对"
     * 变成事实（限频避免刷屏）。 */
    {
        static int64_t s_rp_ms;
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - s_rp_ms > 30000) {
            s_rp_ms = now_ms;
            int32_t pbx, pby, pex, pey, pdw, pdh;
            ent_screen_rect_at(g_tilt_mdeg, &pbx, &pby, &pex, &pey, &pdw, &pdh);
            ESP_LOGW(TAG, "实体渲染：action=%s 帧 %u/%u pieces=%u 画布=(%d,%d %dx%d) "
                          "落点=(%d,%d %dx%d) tilt=%d drag=(%d,%d)",
                     lt->action, (unsigned)g_anim.frame_idx, (unsigned)lt->frame_count,
                     (unsigned)fr->piece_count,
                     (int)g_ent_cx0, (int)g_ent_cy0, (int)g_ent_cw, (int)g_ent_ch,
                     (int)pex, (int)pey, (int)pdw, (int)pdh, (int)g_tilt_mdeg,
                     (int)g_drag_off_x, (int)g_drag_off_y);
            ESP_LOGW(TAG, "实体锚点：origin=(%d,%d) 屏心=(%d,%d)（画布左上=屏心-origin×2）",
                     (int)g_ent_ox, (int)g_ent_oy, (int)g_sw / 2, (int)g_sh / 2);
        }
    }
    /* 帧内 piece 列表顺序 = 权威绘制序（导出端按桌面 RenderFrame 底→顶排列：
     * OrderByDescending(ZIndex)，z 字段仅诊断参考）→ 顺序画，不再排序 */
    uint32_t miss = 0, total = 0;
    for (uint32_t k = 0; k < fr->piece_count; k++) {
        const mpak_piece_t *piece = &lt->pieces[fr->piece_off + k];
        const rc_part_img_t *img = resolve_piece(piece);
        total++;
        if (!img) { miss++; continue; }
        /* 导出 x/y = 位图左上角相对 body 锚点坐标（FinalX 已含 part origin，
         * 不再减 origin）；+帧位移 move（桌面同轴：画布内绝对位移，非累计），
         * -联合画布原点 → 实体缓冲内位置（世界 1x → 2x 移位展开） */
        /* LAYOUT x/y 已含帧位移（导出契约：画布绝对坐标），move 字段仅参考，勿重复叠加 */
        int32_t bx = ((int32_t)piece->x - g_ent_cx0) << RC_SCALE_SHIFT;
        int32_t by = ((int32_t)piece->y - g_ent_cy0) << RC_SCALE_SHIFT;
        blit_ent_2x(img, piece->flip & 1u, bx, by);
    }

    /* 【人物消失自愈 2026-09-27】LAYOUT 与 PARTS 是两份独立资产、分开下载：
     * 真机实测会出现"LAYOUT 换新了、配套 PARTS 还在路上"的窗口——此时整帧
     * piece 全部解析失败，画出来就是"人物没了"（用户报障）。这里在渲染期兜底：
     * 单帧命中率过低即判定错配，立刻请求一次素材全量同步（带节流），
     * 让设备几秒内自己把配套 PARTS 拉回来，不需要用户做任何事。 */
    {
        static int64_t s_rp2_ms;
        int64_t now_ms2 = esp_timer_get_time() / 1000;
        if (now_ms2 - s_rp2_ms > 30000) {
            s_rp2_ms = now_ms2;
            ESP_LOGW(TAG, "实体渲染结果：total=%u miss=%u（部件解析失败数）",
                     (unsigned)total, (unsigned)miss);
        }
    }
    if (total > 0 && miss * 2 > total) {
        static int64_t s_mismatch_last_ms;
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - s_mismatch_last_ms > 15000) {   /* 15s 节流，防刷请求 */
            s_mismatch_last_ms = now_ms;
            ESP_LOGE(TAG, "实体帧部件错配（%u/%u 解析失败）→ 请求素材全量同步"
                          "（LAYOUT 与 PARTS 版本不一致）", (unsigned)miss, (unsigned)total);
            extern void asset_dl_request_sync(void);
            asset_dl_request_sync();
        }
    }

    /* ══ 取证探针（2026-09-27，用户报"头部缺一块 + 一条分割线"）══════════
     * 目的：把「设备上画出来的到底是什么」变成可对拍的事实——主机端用同一
     * mpk 文件解码 + 同一 2x/掩码算法合成，逐件/逐像素比对，判定故障在
     * ①素材（包内容与主机不同）②解码（掩码/像素读错）③合成（画布/坐标）
     * ④上屏（blit 路径）中的哪一环。默认 DEBUG 级不输出；只有把
     * RC_FORENSIC 打开（下面宏）才在 8s/24s 各打一次。 */
#define RC_FORENSIC 0
#if RC_FORENSIC
    {
        static int s_dump_n;
        static int64_t s_dump_ms;
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (s_dump_n < 2 && now_ms > 8000 && now_ms - s_dump_ms > 5000) {
            s_dump_ms = now_ms;
            s_dump_n++;
            s_forensic_chunks = 60;  /* 顺手抓 60 次 flush 的分块指纹（回归：跨帧混合必须为 0） */
            ESP_LOGW(TAG, "取证#%d：PARTS hash=%016llx LAYOUT hash=%016llx "
                          "画布=(%d,%d %dx%d) 缓冲=(%d,%d %dx%d) drag=(%d,%d) tilt=%d",
                     s_dump_n,
                     (unsigned long long)mpak_content_hash(&g_parts),
                     (unsigned long long)mpak_content_hash(g_lt_once_ok ? &g_lt_once : &g_lt_loop),
                     (int)g_ent_cx0, (int)g_ent_cy0, (int)g_ent_cw, (int)g_ent_ch,
                     (int)g_ent_base_wx, (int)g_ent_base_wy, g_sw, g_sh,
                     (int)g_drag_off_x, (int)g_drag_off_y, (int)g_tilt_mdeg);
            for (uint32_t k = 0; k < fr->piece_count; k++) {
                const mpak_piece_t *piece = &lt->pieces[fr->piece_off + k];
                const mpak_part_t *raw_meta = mpak_parts_find(&g_parts, piece->part_id);
                const rc_part_img_t *im = resolve_piece(piece);
                ESP_LOGW(TAG, "取证件[%u] lay_id=%u expr=%u xy=(%d,%d) flip=%u z=%d "
                              "| img_id=%u %dx%d alpha=%d maskbits=%u px_hash=%08x mask_hash=%08x",
                         (unsigned)k, (unsigned)piece->part_id, (unsigned)piece->expr_index,
                         (int)piece->x, (int)piece->y, (unsigned)(piece->flip & 1u), (int)piece->z,
                         im ? (unsigned)im->meta->id : 0u,
                         im ? (int)im->meta->w : 0, im ? (int)im->meta->h : 0,
                         im ? (int)(im->mask != NULL) : -1,
                         im ? (unsigned)im->mask_bits : 0u,
                         im ? (unsigned)im->px_hash : 0u, im ? (unsigned)im->mask_hash : 0u);
                (void)raw_meta;
            }
            /* 实体缓冲 ASCII 图（1 字符 = 画布 1x 像素 = 2x2 屏像素）。
             * ' ' 透明/未覆盖、'.' 黑、'W' 亮灰(白)、'w' 中灰、'd' 暗灰、
             * 'R' 红主导、'G' 绿主导、'B' 蓝主导。每行前带画布 y 号。 */
            for (int32_t cy = 0; cy < g_ent_ch; cy++) {
                char line[RC_ENT_W / RC_SCALE + 1];
                int32_t n = 0;
                for (int32_t cx = 0; cx < g_ent_cw && n < (int32_t)sizeof line - 1; cx++) {
                    int32_t sx = (int32_t)(cx << RC_SCALE_SHIFT);
                    int32_t sy = (int32_t)(cy << RC_SCALE_SHIFT);
                    uint32_t idx = (uint32_t)sy * RC_ENT_W + (uint32_t)sx;
                    if (!rc_mask_bit(g_ent_cov, idx)) { line[n++] = ' '; continue; }
                    uint16_t v = g_ent_px[idx];
                    int32_t r = (int32_t)((v >> 11) & 0x1Fu) << 3;
                    int32_t g = (int32_t)((v >> 5) & 0x3Fu) << 2;
                    int32_t b = (int32_t)(v & 0x1Fu) << 3;
                    int32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
                    char ch;
                    if (mx < 40) ch = '.';
                    else if (mx - r < 24 && mx - g < 24 && mx - b < 24)
                        ch = mx > 200 ? 'W' : (mx > 120 ? 'w' : '.');
                    else if (r >= g && r >= b) ch = 'R';
                    else if (g >= r && g >= b) ch = 'G';
                    else ch = 'B';
                    line[n++] = ch;
                }
                line[n] = 0;
                ESP_LOGW(TAG, "取证画布 y=%02d |%s|", (int)cy, line);
            }
        }
    }
#endif
}

/* 跨任务入口（render_set_parts/layout/expression 会被输入/状态机任务调用）：
 * 与 flush 的 ent_compose（读 g_ent_px/g_ent_cov）互斥——否则读到"画了一半"的
 * 实体缓冲 = 人物某块内容错帧/缺块（用户照片里的"头明显缺一块"）。 */
static void recompose_entity(void)
{
    rc_lock();
    recompose_entity_locked();
    rc_unlock();
}

/* ================= 条带 ================= */

/* 世界 1x 偏移（已折叠周期：mod 图宽；speed + IMU 视差合成） */
static int32_t strip_offset(const rc_strip_t *s, int64_t now_us)
{
    int64_t ms = (now_us - g_map_epoch_us) / 1000;
    int64_t t_off = (int64_t)s->speed_x * ms / 1000;
    int64_t v_off = ((int64_t)g_tilt_mdeg * s->rx * RC_TILT_PX_PER_DEG) /
                    (100 * 1000);
    int64_t w = s->w;
    int64_t off = (t_off + v_off) % w;
    if (off < 0) off += w;
    return (int32_t)off;
}

static void strip_blit(const rc_strip_t *s, int32_t rx0, int32_t ry0,
                       int32_t rw, int32_t rh)
{
    if (!s->ok) return;
    int32_t band_y = (int32_t)s->y << RC_SCALE_SHIFT;
    int32_t band_h = (int32_t)s->h << RC_SCALE_SHIFT;
    int32_t y0 = band_y > ry0 ? band_y : ry0;
    int32_t y1 = (band_y + band_h < ry0 + rh) ? band_y + band_h : ry0 + rh;
    if (y0 >= y1) return;

    int32_t period = (int32_t)s->w << RC_SCALE_SHIFT;
    if (period <= 0) return;
    int32_t off2 = s->last_off << RC_SCALE_SHIFT;
    const uint32_t stride_el = s->stride_b / 2u;

    /* 【合成提速 · 修正版 2026-09-27】掩码是 **tight 按位打包**（bit = y*w + x，
     * 与导出端 PartPackWriter 一致），**不是行对齐** —— 上一版提速误按行对齐
     * (`mask + y*((w+7)/8)`) 取字节、并把旋转后的 bit 位置算错，真机表现为整片
     * **竖条纹**（列可见性乱掉）。现按 tight 位索引取字节，只做"整字节对齐组"
     * 的快速通道；非对齐组走逐像素 tight 判定（与原实现完全等价）。 */
    for (int32_t sy = y0; sy < y1; sy++) {
        int32_t src_y = (sy - band_y) >> RC_SCALE_SHIFT;
        const uint16_t *srow = s->px + (size_t)src_y * stride_el;
        uint16_t *drow = g_fb + (size_t)sy * g_sw;
        int32_t sx = rx0;
        while (sx < rx0 + rw) {
            int32_t m = (sx + off2) % period;
            if (m < 0) m += period;
            int32_t src_x = m >> RC_SCALE_SHIFT;
            uint32_t bit0 = (uint32_t)src_y * s->w + (uint32_t)src_x;   /* tight 位索引 */
            int32_t max_src = (rx0 + rw - sx) >> 1;                     /* 本组最多几个源列 */
            if (max_src <= 0) max_src = 1;
            int32_t run = 8 - (int32_t)(bit0 & 7u);                     /* 到字节边界 */
            if (run > max_src) run = max_src;
            if (src_x + run > (int32_t)s->w) run = (int32_t)s->w - src_x;
            if (run <= 0) run = 1;
            if (run == 8 && (bit0 & 7u) == 0u) {
                uint8_t mb = s->mask[bit0 >> 3];
                if (mb == 0x00) { sx += 2 * run; continue; }            /* 整组透明：跳过 */
                if (mb == 0xFF) {                                       /* 整组不透明：按 2x 展开直写 */
                    /* ══ 【竖条纹真凶 2026-09-27】此处原是
                     *      memcpy(drow + sx, srow + src_x, 2*run*2)
                     * —— 它把 **2*run 个源像素** 1:1 平铺进 2*run 个目标像素，
                     * 漏掉了 RC_SCALE=2 的**横向复制**；而下面的逐像素回退分支
                     * 是写两份（drow[sx] 与 drow[sx+1] 同源）。于是同一张条带：
                     * 混合掩码组按 2x 画（对），整不透明组按 1x 压扁（错），并且
                     * **每 8 个源像素相位重置一次** ⇒ 内容重复曝光 + 每 16 设备像素
                     * 一次的错位竖条。真机实证：默认地图条带 ce2218365f6f4767 /
                     * cdaf31a153002311 是 240×240 全屏背景层，掩码整字节 0xFF 占比
                     * 85% / 49% ⇒ 几乎**整屏**都走这条错路径 = 用户照片里"地图区
                     * 整片细竖条纹"。现按 2x 展开：每源像素写两份。 */
                    uint16_t *d = drow + sx;
                    const uint16_t *sp = srow + src_x;
                    for (int32_t k = 0; k < run; k++) {
                        uint16_t v = sp[k];
                        d[2 * k] = v;
                        d[2 * k + 1] = v;
                    }
                    sx += 2 * run;
                    continue;
                }
            }
            for (int32_t k = 0; k < run; k++) {
                uint32_t b = bit0 + (uint32_t)k;
                if (!s->mask || ((s->mask[b >> 3] >> (7 - (b & 7))) & 1u)) {
                    drow[sx] = srow[src_x + k];
                    if (sx + 1 < rx0 + rw) drow[sx + 1] = srow[src_x + k];
                }
                sx += 2;
            }
        }
    }
}

/* ================= 区域合成（全层重算，幂等） ================= */

/* 横幅字形像素块填充（问题3 描边用）：scaled 块向四周外扩 grow px 涂 color，
 * 严格裁剪到本合成区域列范围 [clip_x, clip_x+clip_w) 与横幅带行 [by0, by1)
 * ——g_fb 常驻，绝不可写出区域外（会污染后续增量合成的残留画面） */
static void banner_fill_block(int32_t px0, int32_t py0, int32_t grow,
                              uint16_t color, int32_t clip_x, int32_t clip_w,
                              int32_t by0, int32_t by1)
{
    for (int32_t py = py0 - grow; py < py0 + RC_BANNER_SCALE + grow; py++) {
        if (py < by0 || py >= by1) continue;
        uint16_t *drow = g_fb + (size_t)py * g_sw;
        for (int32_t px = px0 - grow; px < px0 + RC_BANNER_SCALE + grow; px++) {
            if (px < clip_x || px >= clip_x + clip_w || px >= g_sw) continue;
            drow[px] = color;
        }
    }
}

/* 实体层合成（POKER 常规路径与 CLOCK_DOZE 睡眠态共用）。
 * darken_pct>0：整体降亮 = 睡眠观感（RGB565 各通道按比例缩放，黑底上近似
 * 屏幕调光，省电语义不变）。覆盖位图 g_ent_cov 为 1bit，未覆盖处保持底层像素。 */
static void ent_compose(int32_t x, int32_t y, int32_t w, int32_t h, int darken_pct)
{
    int32_t bx, by, ex, ey, dw, dh;
    ent_screen_rect_at(g_tilt_mdeg, &bx, &by, &ex, &ey, &dw, &dh);
    if (dw <= 0 || dh <= 0) return;
    int32_t X0 = ex > x ? ex : x, Y0 = ey > y ? ey : y;
    int32_t X1 = (ex + dw < x + w) ? ex + dw : x + w;
    int32_t Y1 = (ey + dh < y + h) ? ey + dh : y + h;
    for (int32_t sy = Y0; sy < Y1; sy++) {
        uint32_t erow = (uint32_t)(sy - by) * RC_ENT_W;
        uint16_t *drow = g_fb + (size_t)sy * g_sw;
        for (int32_t sx = X0; sx < X1; sx++) {
            uint32_t eidx = erow + (uint32_t)(sx - bx);
            if (!rc_mask_bit(g_ent_cov, eidx)) continue;
            uint16_t c = g_ent_px[eidx];
            if (darken_pct > 0) {
                uint32_t r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
                r5 = r5 * (uint32_t)darken_pct / 100u;
                g6 = g6 * (uint32_t)darken_pct / 100u;
                b5 = b5 * (uint32_t)darken_pct / 100u;
                c = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            }
            drow[sx] = c;
        }
    }
}

static void compose_region(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_sw) w = g_sw - x;
    if (y + h > g_sh) h = g_sh - y;
    if (w <= 0 || h <= 0) return;

    /* 0) CLOCK_DOZE（问题3/E9）：AMOLED 纯黑背景只数字发光——
     * 时钟激活即 doze 语义（enable 仅由 CLOCK_DOZE 进出指令驱动）。
     *
     * 【E9 缺口补齐 2026-09-27】需求原文：「无人交互 N 分钟 → **宠物睡眠态**
     * + WZ 数字时钟浮现 … AMOLED 纯黑背景只数字发光（省电）」。
     * 此前本分支画完数字就 return：宠物完全不画（缺口核对记为 E9 未实现项
     * 「sleep-pet state」），真机表现是黑屏上只有时间，用户看不到宠物睡了。
     * 现改为：黑底 → 时钟数字 → 宠物（darken 降亮=睡眠观感，省电语义保持）。 */
    if (clock_digits_active()) {
        for (int32_t r = y; r < y + h; r++)
            memset(g_fb + (size_t)r * g_sw + x, 0, (size_t)w * 2u);
        clock_digits_compose(g_fb, g_sw, RC_SCALE, x, y, w, h);
        ent_compose(x, y, w, h, RC_SLEEP_DARKEN);
        return;
    }

    /* 1) static_back（不动底；无地图 → 黑底）
     * 根因修复（拖动小人后旧位置图像永久残留，三轮未修的真凶）：脏区从第 x 列
     * 开始，铺底必须同样从第 x 列起写——此前 drow/源均从第 0 列起，脏区
     * [x, x+w) 的 framebuffer 内容从未被重铺，blit 发出的是上一帧陈旧像素
     * （= 旧位置实体图像）→ 残留。背景接近纯色时错位拷贝肉眼看不出，
     * 实体移动后才暴露。 */
    for (int32_t r = y; r < y + h; r++) {
        uint16_t *drow = g_fb + (size_t)r * g_sw + x;
        if (g_static) memcpy(drow, g_static + (size_t)r * g_sw + x, (size_t)w * 2u);
        else memset(drow, 0, (size_t)w * 2u);
    }

    /* 2) 条带（x 向循环平铺） */
    for (int i = 0; i < g_strip_n; i++)
        strip_blit(&g_strips[i], x, y, w, h);

    /* 3) tile_layer（1bit alpha 叠加；列偏移与 static_back 同理必须 +x，
     * 否则脏区 [x, x+w) 铺的是第 0 列起的陈旧内容） */
    if (g_tile) {
        for (int32_t r = y; r < y + h; r++) {
            const uint16_t *srow = g_tile + (size_t)r * g_sw;
            const uint8_t  *mrow = g_tile_mask ?
                g_tile_mask + (size_t)r * ((g_sw + 7) / 8) : NULL;
            uint16_t *drow = g_fb + (size_t)r * g_sw;
            if (!mrow) {
                memcpy(drow + x, srow + x, (size_t)w * 2u);
            } else {
                /* 【合成提速 2026-09-27】逐像素读掩码位 → 改按 8 像素组：
                 * 掩码字节 0x00 整组跳过、0xFF 整组 memcpy、混合才逐像素。
                 * 真机实测 flush compose=157ms（4 层整屏重算）拖垮拖拽手感，
                 * 掩码组快速通道是其中最大头（tile/段/条带三层都吃这个）。 */
                int32_t c = 0;
                while (c < w) {
                    int32_t px = x + c;
                    uint32_t bit = (uint32_t)px;
                    int32_t grp = (int32_t)(8 - (bit & 7));       /* 到字节边界 */
                    if (grp > w - c) grp = w - c;
                    uint8_t mb = mrow[bit >> 3];
                    if (grp == 8 && mb == 0x00) { c += 8; continue; }
                    if (grp == 8 && mb == 0xFF) { memcpy(drow + px, srow + px, 16u); c += 8; continue; }
                    for (int32_t k = 0; k < grp; k++) {
                        uint32_t b = bit + (uint32_t)k;
                        if ((mrow[b >> 3] >> (7 - (b & 7))) & 1u) drow[px + k] = srow[px + k];
                    }
                    c += grp;
                }
            }
        }
    }

    /* 4) 地图时钟（场景层，实体之下） */
    clock_digits_compose(g_fb, g_sw, RC_SCALE, x, y, w, h);

    /* 【校准模式】方向/镜像标定层（实体之下、时钟之上；POKER/OFFLINE 触摸 down
     * 沿由 render_calib_set 置位）。用户判读方法（对照顶部 M 序号横幅拍照回报）：
     *   ① 顶部横幅文字正立、不左右镜像 → 该 mirror 组合方向正确；
     *   ② 人物脚底踩在红线(y=440)上、红线与蓝线(y=478)间留出 40px 间隙 → 底边
     *      正确（蓝线=面板真实底边；红蓝间隙 =「脚底距屏底 40px」规格可视化）；
     *   ③ 红三角出现在画面【右下】角 → 左右未镜像；跑到【左下】角 → mirror
     *      翻转，该组合不对。
     * 文字正立 + 脚踩红线 + 三角在右下，三者同时成立的 M 序号即为定稿握持
     * （USB 朝右、USB 对面竖边为底）的正确组合。 */
    if (g_calib_on) {
        const uint16_t red  = 0xF800;
        const uint16_t blue = 0x001F;        /* 纯蓝：真实底边线专用，与红线区分 */

        /* ① 脚底目标线：红横线 y=440（= 屏底-40px，人物脚底应踩在此线上） */
        if (440 >= y && 440 < y + h)
            for (int32_t c = x; c < x + w && c < g_sw; c++)
                g_fb[(size_t)440 * g_sw + c] = red;

        /* ② 真实底边：蓝横线 y=478（面板可见最底两行 478/479 的上一行）。
         * 与红线间隔 40px：脚踩红线 = 规格满足；脚踩到蓝线 = 底边判定偏了 */
        if (478 >= y && 478 < y + h)
            for (int32_t c = x; c < x + w && c < g_sw; c++)
                g_fb[(size_t)478 * g_sw + c] = blue;

        /* 脚线标签：红线最左端 x=2..40 加粗 3px（y=439..441）成小红块，
         * 远拍照片里快速定位脚底线的位置 */
        for (int32_t r = 439; r <= 441; r++) {
            if (r < y || r >= y + h) continue;
            int32_t bx0 = 2 > x ? 2 : x;
            int32_t bx1 = 41 < x + w ? 41 : x + w;   /* x=2..40 → [2,41) */
            for (int32_t c = bx0; c < bx1 && c < g_sw; c++)
                g_fb[(size_t)r * g_sw + c] = red;
        }

        /* ③ 镜像判读：右下角实心红三角（y=460..476、x=440..476；直角边贴右缘
         * x=476 与下缘 y=476，斜边在左，逐行画 x=440+(476-r)..476，越往下越宽）。
         * 未镜像 → 三角在右下；左右镜像 → 三角跑到左下，一眼可辨 */
        for (int32_t r = 460; r <= 476; r++) {
            if (r < y || r >= y + h) continue;
            int32_t tx0 = 440 + (476 - r);
            int32_t tx1 = 477;                       /* 含 x=476 → [..,477) */
            if (tx0 < x) tx0 = x;
            if (tx1 > x + w) tx1 = x + w;
            for (int32_t c = tx0; c < tx1 && c < g_sw; c++)
                g_fb[(size_t)r * g_sw + c] = red;
        }

        /* ④ 水平中线：红竖线 x=240，只画下段 y=200..476（顶部留给横幅文本，
         * 不与之打架）；居中与左右镜像的横向基准 */
        if (240 >= x && 240 < x + w) {
            int32_t ry0 = 200 > y ? 200 : y;
            int32_t ry1 = 477 < y + h ? 477 : y + h; /* y=200..476 → [200,477) */
            for (int32_t r = ry0; r < ry1 && r < g_sh; r++)
                g_fb[(size_t)r * g_sw + 240] = red;
        }

        /* ⑤ 触摸落点回显：8×8 白点（面板坐标系，逻辑保持不变） */
        if (g_calib_touch_x >= 0) {
            int32_t tx = g_calib_touch_x & ~1, ty = g_calib_touch_y & ~1;
            for (int dy2 = 0; dy2 < 8; dy2++)
                for (int dx2 = 0; dx2 < 8; dx2++)
                    if (tx + dx2 < g_sw && ty + dy2 < g_sh)
                        g_fb[(size_t)(ty + dy2) * g_sw + tx + dx2] = 0xFFFF;
        }
    }

    /* 5) 实体缓冲（1bit 覆盖；显示区 = clamp 后的摆放矩形，与 mark_ent_at
     * 完全同源（ent_screen_rect_at），右缘 480 处标脏/绘制不再分歧） */
    ent_compose(x, y, w, h, 0);

    /* 6) 气泡（不透明矩形位图） */
    if (g_bub.active) {
        int32_t X0 = g_bub.x > x ? g_bub.x : x, Y0 = g_bub.y > y ? g_bub.y : y;
        int32_t X1 = (g_bub.x + g_bub.w < x + w) ? g_bub.x + g_bub.w : x + w;
        int32_t Y1 = (g_bub.y + g_bub.h < y + h) ? g_bub.y + g_bub.h : y + h;
        for (int32_t r = Y0; r < Y1; r++) {
            memcpy(g_fb + (size_t)r * g_sw + X0,
                   g_bub.px + (size_t)(r - g_bub.y) * RC_BUBBLE_MAX_W +
                       (X0 - g_bub.x),
                   (size_t)(X1 - X0) * 2u);
        }
    }

    /* 7) 未配网常驻横幅（问题4：顶部 480×28 深色底白字，compose 最顶层）
     * 取字模（与 font5x7.h 数据格式核对一致）：下标=字符 ASCII 值（指定初始化器，
     * 等价 字符-' ' 起点式表），每字符 5 列字节、无 stride，bit0=顶行。
     * 问题3 可读性加固：先整趟 1px 黑描边（块外扩 1px）再整趟白字填充。 */
    if (g_banner_on) {
        /* 【用户反馈 2026-09-27】"横幅下次调低一点 肉眼看不到"——真机照片实证：
         * y=0 起画的横幅被 AMOLED 圆角/边框切掉，只看到一条发光边。现整体下移到
         * RC_BANNER_Y（圆角安全区），文字行按同一偏移画。 */
        int32_t by0 = (y > RC_BANNER_Y) ? y : RC_BANNER_Y;
        int32_t by1 = (y + h < RC_BANNER_Y + RC_BANNER_H) ? y + h : RC_BANNER_Y + RC_BANNER_H;
        if (by0 < by1) {
            for (int32_t r = by0; r < by1; r++) {
                uint16_t *drow = g_fb + (size_t)r * g_sw;
                for (int32_t c = x; c < x + w; c++) drow[c] = RC_BANNER_BG;
            }
            for (int pass = 0; pass < 2; pass++) {
                int32_t gx = RC_BANNER_PAD_X;
                for (const char *p = g_banner_text; *p && gx < g_sw; p++) {
                    unsigned char u = (unsigned char)*p;
                    if (u >= 'a' && u <= 'z') u -= 32;   /* 5x7 字库仅大写：小写转大写（SSID 小写会渲染成空白） */
                    if (u >= 128) u = '?';
                    const uint8_t *cols = MP_FONT5X7[u];
                    for (int col = 0; col < MP_FONT_GLYPH_W; col++) {
                        for (int row = 0; row < MP_FONT_GLYPH_H; row++) {
                            if (!(cols[col] & (1u << row))) continue;
                            int32_t px0 = gx + col * RC_BANNER_SCALE;
                            int32_t py0 = RC_BANNER_Y + RC_BANNER_PAD_Y + row * RC_BANNER_SCALE;
                            banner_fill_block(px0, py0,
                                              pass == 0 ? 1 : 0,
                                              pass == 0 ? RC_BANNER_OUTLINE
                                                        : RC_BANNER_FG,
                                              x, w, by0, by1);
                        }
                    }
                    gx += (MP_FONT_GLYPH_W + 1) * RC_BANNER_SCALE;
                }
            }
        }
    }

    /* 8) BGM 半屏控制条（E6）：最顶层叠加，仅占屏幕下半（上半宠物照常合成） */
    ov_draw(x, y, w, h);
}

/* ================= 脏区：标脏 16×16 块 → 单一包围盒（问题1） =================
 * 旧实现：逐行行程分别 compose+blit，且 blit 前按 hash 过滤把行内连续变化段
 * 拆成多段——相邻矩形之间的间隙像素漏刷，叠加 display_blit 内部 2px 向外取偶
 * 的外扩错位，真机表现为行/列裂纹与残影。现改为：本帧所有标脏块合并为一个
 * 包围盒，一次 compose_region + 一次 blit（480 宽全帧重绘成本可接受，
 * 先正确后优化）；实体/条带每帧整体重绘各自包围盒由 mark_ent/mark_rect 保证。 */
static void flush_dirty(void)
{
    /* 整笔"标脏清零 → compose_region → 分块 blit"必须原子：真机取证已证
     * 分块之间被跨任务 full_recompose 截断 → 同屏两帧内容（分割线/错帧块）。 */
    rc_lock();
    int32_t cx0 = -1, cy0 = -1, cx1 = -1, cy1 = -1;
    int32_t cells = 0;
    for (int32_t cy = 0; cy < g_gh; cy++) {
        for (int32_t cx = 0; cx < g_gw; cx++) {
            if (!g_mark[cy * g_gw + cx]) continue;
            g_mark[cy * g_gw + cx] = 0;
            cells++;
            if (cx0 < 0 || cx < cx0) cx0 = cx;
            if (cy0 < 0 || cy < cy0) cy0 = cy;
            if (cx > cx1) cx1 = cx;
            if (cy > cy1) cy1 = cy;
        }
    }
    if (cx0 < 0) { rc_unlock(); return; }

    int32_t x = cx0 * RC_CELL, y = cy0 * RC_CELL;
    int32_t w = (cx1 - cx0 + 1) * RC_CELL, h = (cy1 - cy0 + 1) * RC_CELL;
    if (x + w > g_sw) w = g_sw - x;
    if (y + h > g_sh) h = g_sh - y;
    /* 探针（上屏侧）：证明 标脏→合成→blit_be→display_blit 真的跑过。
     * 诊断期已结束：首次执行也只打 DEBUG，rect 变化由 mark 探针反映 */
    static bool s_flush_logged;
    ESP_LOGD(TAG, "flush rect (%" PRId32 ",%" PRId32 ") %" PRId32 "x%" PRId32,
             x, y, w, h);
    if (!s_flush_logged) {
        s_flush_logged = true;
        ESP_LOGD(TAG, "first dirty flush rect (%" PRId32 ",%" PRId32 ") %"
                 PRId32 "x%" PRId32, x, y, w, h);
    }
    (void)cells;
    if (s_forensic_chunks > 0)
        ESP_LOGW(TAG, "取证flush rect=(%d,%d %dx%d)", (int)x, (int)y, (int)w, (int)h);
    /* 【卡顿量化探针 2026-09-27】用户报障"拖动久了卡顿"。flush 分两段计时：
     * compose（各图层重算）与 blit（字节序转换 + SPI 分块上屏）各占多少 ms，
     * 每 2s 汇总一条（均/最大）。据此决定优化哪一段，而不是猜。 */
    int64_t t_c0 = esp_timer_get_time();
    compose_region(x, y, w, h);
    int64_t t_c1 = esp_timer_get_time();
    blit_be(x, y, w, h, g_fb + (size_t)y * g_sw + x, g_sw);
    int64_t t_c2 = esp_timer_get_time();
    if (s_forensic_chunks > 0) s_forensic_chunks--;
    {
        static int64_t s_perf_ms;
        static uint32_t n, csum, bsum, cmax, bmax;
        n++;
        uint32_t cd = (uint32_t)((t_c1 - t_c0) / 1000), bd = (uint32_t)((t_c2 - t_c1) / 1000);
        csum += cd; bsum += bd;
        if (cd > cmax) cmax = cd;
        if (bd > bmax) bmax = bd;
        int64_t now_ms = t_c2 / 1000;
        if (now_ms - s_perf_ms > 30000) {   /* 30s 一条：仅作竞态/完整性哨兵（原 2s 太吵） */
            s_perf_ms = now_ms;
            ESP_LOGW(TAG, "flush 哨兵（%u 笔/30s）：compose 均 %u 最大 %u ms | blit 均 %u 最大 %u ms | "
                          "末笔区域 %dx%d | 暂存在飞命中 %u | 完整性失败 %u",
                     (unsigned)n, (unsigned)(csum / (n ? n : 1)), (unsigned)cmax,
                     (unsigned)(bsum / (n ? n : 1)), (unsigned)bmax, (int)w, (int)h,
                     (unsigned)g_blit_race_hits, (unsigned)g_blit_verify_fail);
            n = 0; csum = bsum = cmax = bmax = 0;
        }
    }

    /* ══ 残留自检（2026-09-27，用户报障"错帧块/拖影"的兜底回归）══════════
     * 无地图（g_static==NULL）时屏幕底色应恒为纯黑：**实体显示矩形之外**出现
     * 非黑像素 ⇒ 脏区漏标（旧位置没被重铺）或跨任务写入残留。
     * 真机取证里"头缺一块 + 上下错帧的横条"正是这类残留的典型形态，而它无法从
     * 分块指纹发现（指纹只覆盖本次 flush 的区域）。这里以 1s 限频统计残留像素数
     * 与包围盒并打 WARN，把"屏上是否干净"变成可回归的事实。
     * 排除：当前实体矩形（外扩 16）、顶部横幅带、BGM 半屏条、气泡矩形。 */
    if (!g_static) {
        static int64_t s_ghost_ms;
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - s_ghost_ms > 1000) {
            s_ghost_ms = now_ms;
            int32_t ex, ey, dw, dh, ebx, eby;
            ent_screen_rect_at(g_tilt_mdeg, &ebx, &eby, &ex, &ey, &dw, &dh);
            int32_t gx0 = 99999, gy0 = 99999, gx1 = -1, gy1 = -1;
            uint32_t ghost = 0;
            for (int32_t ry = 0; ry < g_sh; ry++) {
                if (ry >= RC_BANNER_Y && ry < RC_BANNER_Y + RC_BANNER_H) continue;
                if (g_banner_on && ry >= RC_BANNER_Y && ry < RC_BANNER_Y + RC_BANNER_H) continue;
                if (ry >= g_sh - RC_OV_H) continue;              /* BGM 半屏条 */
                if (g_bub.active && ry >= g_bub.y - 2 && ry < g_bub.y + g_bub.h + 2) continue;
                if (dw > 0 && ry >= ey - 2 && ry < ey + dh + 2) continue;  /* 实体行 */
                const uint16_t *row = g_fb + (size_t)ry * g_sw;
                for (int32_t rx = 0; rx < g_sw; rx++) {
                    if (row[rx] != 0) {
                        ghost++;
                        if (rx < gx0) gx0 = rx;
                        if (rx > gx1) gx1 = rx;
                        if (ry < gy0) gy0 = ry;
                        if (ry > gy1) gy1 = ry;
                    }
                }
            }
            if (ghost) {
                ESP_LOGW(TAG, "残留自检：实体矩形外非黑像素 %u 个 bbox=(%d,%d)-(%d,%d) "
                              "实体=(%d,%d %dx%d)（脏区漏标嫌疑）",
                         (unsigned)ghost, (int)gx0, (int)gy0, (int)gx1, (int)gy1,
                         (int)ex, (int)ey, (int)dw, (int)dh);
            }
        }
    }
    rc_unlock();
}

/* ================= 上屏边界：LE framebuffer → 驱动大端 RGB565 =================
 * 全管线按小端 u16 处理（资产小端 + LVGL 小端一致）；
 * display_blit 要求大端字节序，故在此唯一边界做逐像素字节交换，
 * 分块经内部 RAM 暂存，避免 PSRAM 二份帧缓冲。
 * 暂存 24KB（480 宽 × 25 行/块）：480 宽全帧从 60 次 display_blit（= 60 次
 * SPI 队列传输）降到 20 次，缓解拖动期高频 blit 的 ESP_ERR_NO_MEM 风暴；
 * 24KB 静态内部 RAM 已评估（LVGL 大缓冲已走 PSRAM）。 */
/* 对齐 64B（SPI 驱动硬性要求）：ESP-IDF spi_master setup_priv_desc 对
 * 【非 DMA-capable 或地址/长度未对齐】的 TX 缓冲会在每次排队时临时
 * malloc 一整块 MALLOC_CAP_DMA 暂存——内部堆被 WiFi/LVGL 挤压时分配
 * 失败 → esp_lcd draw_bitmap 返回 ESP_ERR_NO_MEM（真机风暴根因：
 * "send color data failed" 与 WiFi GOT_IP 强相关即此）。静态 64B 对齐
 * = 驱动零分配，风暴根除。 */
/* 【为什么必须是 24KB（= 480 宽 × 25 行 × 2B）2026-09-27 真机定案】
 * 块行数 = chunk_px / w 会向下取整：12KB 时 480 宽只能 12 行，于是
 * `rows_per & 1` 修正后仍可能在"全帧重绘"这类非整块场景下产生奇数块高；
 * 而 display_blit 对奇数行高会做 even_round(+1)，导致 (aw!=w || ah!=h)
 * 掉进 PSRAM 暂存路径 → esp_lcd 对 PSRAM 缓冲**每次强行 malloc ~25KB 内部
 * DMA**（真机溯源：blit len=25024 与 12288 缓冲矛盾即此）→ 内部堆一次性
 * 见底（实测"联网后 空闲=423B / 最大块=244B"）→ socket/帧缓冲全部分配失败
 * → 心跳停、指令收不到、BGM 不出声。
 * 24KB 取 480×25×2，保证 480 宽下 rows_per=25 → 仍是奇数，故这里**主动取偶数
 * 行数**（见下方 rows_per 计算），让每块恒为整行高，快速通道恒命中、驱动零分配。 */
static uint8_t s_blit_stage[12288] __attribute__((aligned(64)));   /* 回退到已知稳定值：24KB 版本与脸部分割线报障同期 */

static void blit_be(int32_t x, int32_t y, int32_t w, int32_t h,
                    const uint16_t *src, int32_t src_stride)
{
    /* 【整宽零拷贝直发实验已回滚 2026-09-27】曾试过 w==g_sw 时一次 display_blit
     * 发完整屏（460800B）以排除"分块窗口写入"嫌疑 —— 真机 display_blit 直接
     * ret=257（ESP_ERR_INVALID_ARG，超 esp_lcd 单笔 max_transfer_sz），整屏区域
     * 反而完全不刷新。结论：竖条纹不在"分块窗口"这一层（真凶见 strip_blit 的
     * 0xFF 分支 2x 展开缺失），故回滚为经校验的分块暂存路径。 */
    const int32_t chunk_px = (int32_t)sizeof s_blit_stage / 2;
    int32_t rows_per = (w > 0) ? chunk_px / w : 0;
    if (rows_per < 2) rows_per = 2;
    if (rows_per & 1) rows_per--;         /* 恒偶：避免 display_blit 的 even_round 把块高改奇 */
    /* 行高恒偶（+h 本身恒偶：脏区 16px 网格）→ display_blit 内 even_round
     * 不再外扩出行 → aw==w && ah==h 快速通道恒命中（大端缓冲直推 SPI，
     * 零 PSRAM 暂存、零驱动侧 DMA 拷贝分配）。此前 rows_per=25（奇）使
     * 每个 chunk 被外扩 +1 行踢进 PSRAM 暂存路径 → esp_lcd 对 PSRAM 缓冲
     * 每次强制 malloc ~25KB 内部 DMA 拷贝 → 内部堆见底 → NO_MEM 风暴。
     * （真机溯源 2026-09-26：blit len=25024=480行×26 与 24576 缓冲矛盾即此） */

    for (int32_t r0 = 0; r0 < h; r0 += rows_per) {
        int32_t hh = (r0 + rows_per < h) ? rows_per : (h - r0);
        /* 【混行竞态根修 2026-09-27】填暂存前先等上一笔传输读完它。
         * 此前顺序是"填→发→填→发…"，而等槽发生在 display_blit 内部 ⇒ 上一笔 DMA
         * 仍在读 s_blit_stage 时就被本块覆盖 → 面板收到两块混合数据 =
         * 用户照片里的横彩条/竖条纹/分割线。 */
        /* 探针语义：本块填暂存时"上一笔传输仍在飞"= 修复前必然出混行的那一笔。
         * 计数 >0 即**证明该竞态真实存在**（真机实测 ~150 次/s，几乎每块都命中）。 */
        if (display_tx_busy()) g_blit_race_hits++;
        display_wait_tx_idle();
        uint8_t *d = s_blit_stage;
        for (int32_t r = 0; r < hh; r++) {
            const uint16_t *s = src + (size_t)(r0 + r) * src_stride;
            for (int32_t c = 0; c < w; c++) {
                uint16_t v = s[c];
                *d++ = (uint8_t)(v >> 8);
                *d++ = (uint8_t)v;
            }
        }
        /* 【传输完整性校验 2026-09-27】发送前算 stage 指纹 → display_blit → 等传输
         * 结束 → 再算一次；两者必须相等（否则说明"传输还在读时缓冲被改写"，
         * 面板就会收到混行数据 —— 这正是用户照片竖条纹/横彩条的根因）。
         * 修复后该计数应恒为 0；留作永久回归哨兵。 */
        uint32_t h_before = 2166136261u;
        for (int32_t i = 0; i < w * hh * 2; i++) { h_before ^= s_blit_stage[i]; h_before *= 16777619u; }
        if (s_forensic_chunks > 0) {
            uint32_t fh = 2166136261u;
            for (int32_t i = 0; i < w * hh * 2; i++) {
                fh ^= s_blit_stage[i];
                fh *= 16777619u;
            }
            esp_err_t derr = display_blit((int)x, (int)(y + r0), (int)w, (int)hh, s_blit_stage);
            ESP_LOGW(TAG, "取证blit rect=(%d,%d %dx%d) len=%d ret=%d fnv=%08x",
                     (int)x, (int)(y + r0), (int)w, (int)hh, (int)(w * hh * 2),
                     (int)derr, (unsigned)fh);
        } else {
            display_blit((int)x, (int)(y + r0), (int)w, (int)hh, s_blit_stage);
        }
        display_wait_tx_idle();              /* 等这一笔读完，再做校验/重填 */
        {
            uint32_t h_after = 2166136261u;
            for (int32_t i = 0; i < w * hh * 2; i++) { h_after ^= s_blit_stage[i]; h_after *= 16777619u; }
            if (h_after != h_before) {
                g_blit_verify_fail++;
                if (g_blit_verify_fail < 4)
                    ESP_LOGE(TAG, "上屏完整性校验失败：第 %d 块暂存被传输期间改写（%dx%d）",
                             (int)(r0 / (rows_per ? rows_per : 1)), (int)w, (int)hh);
            }
        }
    }
}

/* 【相机下移实验】把已装载的 static/tile/掩码整体上移 MP_GROUND_CAM_SHIFT_PX 行，
 * 底部缺失行用内嵌"地面带"补齐（tile 层补透明）；条带 y 上移 SHIFT/2 世界像素。
 * 只在命中地面表（= 这张图）且开了实验开关时执行。 */
static void ground_cam_shift_layers(void)
{
    if (s_ground_shift_px <= 0) return;    /* 板A=0（定稿原景）；板B profile 可开 */
    {
    if (!g_ground_tbl_on) return;
    const int32_t sh = s_ground_shift_px;
    if (sh <= 0 || sh >= g_sh) return;

    if (g_static) {
        for (int32_t r = 0; r + sh < g_sh; r++)
            memcpy(g_static + (size_t)r * g_sw, g_static + (size_t)(r + sh) * g_sw,
                   (size_t)g_sw * 2u);
        /* 底部 sh 行 ← 地面带（1x → 2x 最近邻展开） */
        const uint16_t *band = (const uint16_t *)(const void *)ground_band_000010000_bin_start;
        for (int32_t r = g_sh - sh; r < g_sh; r++) {
            int32_t by = (r - (g_sh - sh)) >> 1;
            if (by >= MP_GROUND_BAND_H) by = MP_GROUND_BAND_H - 1;
            uint16_t *drow = g_static + (size_t)r * g_sw;
            const uint16_t *brow = band + (size_t)by * MP_GROUND_BAND_W;
            for (int32_t x = 0; x < g_sw; x++)
                drow[x] = brow[(x >> 1) < MP_GROUND_BAND_W ? (x >> 1) : MP_GROUND_BAND_W - 1];
        }
    }
    if (g_tile) {
        for (int32_t r = 0; r + sh < g_sh; r++)
            memcpy(g_tile + (size_t)r * g_sw, g_tile + (size_t)(r + sh) * g_sw,
                   (size_t)g_sw * 2u);
        memset(g_tile + (size_t)(g_sh - sh) * g_sw, 0, (size_t)sh * g_sw * 2u);
    }
    if (g_tile_mask) {
        const size_t stride = (size_t)((g_sw + 7) / 8);
        for (int32_t r = 0; r + sh < g_sh; r++)
            memcpy(g_tile_mask + (size_t)r * stride, g_tile_mask + (size_t)(r + sh) * stride, stride);
        memset(g_tile_mask + (size_t)(g_sh - sh) * stride, 0, (size_t)sh * stride);
    }
    for (int i = 0; i < g_strip_n; i++) {
        if (!g_strips[i].ok) continue;
        g_strips[i].y = (int16_t)(g_strips[i].y - (sh >> 1));
    }
    ESP_LOGI(TAG, "相机下移实验：各层上移 %d 行（设备像素），底部用内嵌地面带补齐；条带 y -= %d",
             (int)sh, (int)(sh >> 1));
    }
}

static void full_recompose(void)
{
    /* 同 flush_dirty：跨任务（render_set_map/clock/exit_menu/force_redraw）可达，
     * 必须与渲染任务的增量 flush 串行化，否则整屏 blit 与增量 blit 交错上屏。 */
    rc_lock();
    compose_region(0, 0, g_sw, g_sh);
    memset(g_mark, 0, (size_t)g_gw * g_gh);
    blit_be(0, 0, g_sw, g_sh, g_fb, g_sw);
    rc_unlock();
}

/* ================= 地图装载 ================= */

static void scene_free(void)
{
    if (g_static)    { heap_caps_free(g_static);    g_static = NULL; }
    if (g_tile)      { heap_caps_free(g_tile);      g_tile = NULL; }
    if (g_tile_mask) { heap_caps_free(g_tile_mask); g_tile_mask = NULL; }
    if (g_strips) {
        for (int i = 0; i < g_strip_n; i++) {
            if (g_strips[i].px)   heap_caps_free(g_strips[i].px);
            if (g_strips[i].mask) heap_caps_free(g_strips[i].mask);
        }
        heap_caps_free(g_strips);
        g_strips = NULL;
    }
    g_strip_n = 0;
    g_map_ok = false;
}

/* 读整幅 RGB565 层进屏幕尺寸缓冲（预烘焙直接用；1x 存储则 2x 展开） */
static uint16_t *layer_rgb_load(const mpak_t *m, uint32_t off, uint32_t len,
                                uint16_t vw, uint16_t vh)
{
    uint32_t stride_b = rc_align4((uint32_t)vw * 2u);
    if (len != (uint32_t)vh * stride_b) return NULL;

    uint16_t *dst = psram((size_t)g_sw * g_sh * 2u);
    if (!dst) return NULL;

    if (vw == (uint16_t)g_sw && vh == (uint16_t)g_sh &&
        stride_b == (uint32_t)g_sw * 2u) {
        if (mpak_read_at((mpak_t *)m, m->payload_off + off, dst, len) != MPAK_OK) {
            heap_caps_free(dst);
            return NULL;
        }
        return dst;
    }

    /* 【任意比例最近邻 2026-09-30】原仅接受 2x（vw*RC_SCALE==屏宽）；1.85B 360 屏
     * 用 480 档素材（240 bg → 1.5x）被拒=背景缺失。泛化为 vw/vh→g_sw/g_sh
     * 最近邻采样（2x 是其特例）。 */
    if (vw == 0 || vh == 0) {
        heap_caps_free(dst);
        return NULL;
    }
    uint8_t *raw = psram(len);
    if (!raw) { heap_caps_free(dst); return NULL; }
    if (mpak_read_at((mpak_t *)m, m->payload_off + off, raw, len) != MPAK_OK) {
        heap_caps_free(raw); heap_caps_free(dst);
        return NULL;
    }
    uint32_t stride_el = stride_b / 2u;
    ESP_LOGW(TAG, "[取证] raw[0..3]=%02x%02x %02x%02x %02x%02x %02x%02x dst[0..3]=%04x %04x %04x %04x",
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
             ((uint16_t *)raw)[0], ((uint16_t *)raw)[1], ((uint16_t *)raw)[2], ((uint16_t *)raw)[3]);
    for (int32_t dy = 0; dy < g_sh; dy++) {
        const uint16_t *srow = (const uint16_t *)raw + (size_t)((int64_t)dy * vh / g_sh) * stride_el;
        uint16_t *drow = dst + (size_t)dy * g_sw;
        for (int32_t dx = 0; dx < g_sw; dx++) drow[dx] = srow[(int32_t)((int64_t)dx * vw / g_sw)];
    }
    ESP_LOGW(TAG, "[取证] dst after: %04x %04x %04x %04x | row165*360: %04x %04x %04x %04x",
             dst[0], dst[1], dst[2], dst[3],
             dst[165 * g_sw], dst[165 * g_sw + 1], dst[165 * g_sw + 2], dst[165 * g_sw + 3]);
    heap_caps_free(raw);
    return dst;
}

static uint8_t *tile_mask_load(const mpak_t *m, uint32_t off, uint32_t len,
                               uint16_t vw, uint16_t vh)
{
    if (len == 0) return NULL;
    size_t raw_bits = (size_t)vw * vh;
    if (len != (raw_bits + 7) / 8) return NULL;
    uint8_t *raw = psram(len);
    if (!raw) return NULL;
    if (mpak_read_at((mpak_t *)m, m->payload_off + off, raw, len) != MPAK_OK) {
        heap_caps_free(raw);
        return NULL;
    }
    uint8_t *dst = psram(((size_t)g_sw * g_sh + 7) / 8);
    if (!dst) { heap_caps_free(raw); return NULL; }
    /* 【未初始化掩码 = 黑斑/竖条纹残留 2026-09-27】下面只做 rc_mask_set，**从不
     * 清位**：PSRAM 不清零 ⇒ 目标里未被置位的 bit 保留上一任占用者的内容，
     * 于是 tile 层在这些位置被当作"不透明"画出来 —— tile 的空区颜色是 RGB565 0
     * （纯黑）⇒ 真机表现就是地图上随机**黑斑**；若那块 PSRAM 上一任是帧缓冲/条带
     * 之类有结构的缓冲，残留 bit 还会是**成排的竖条纹**（上一版实体缓冲漏 memset
     * 时用户照片同样是"黑色竖条 + 随机竖条纹"，同一病灶）。必须先整体清零。 */
    memset(dst, 0, ((size_t)g_sw * g_sh + 7) / 8);
    for (int32_t dy = 0; dy < g_sh; dy++)
        for (int32_t dx = 0; dx < g_sw; dx++) {
            uint32_t sidx = (uint32_t)((int64_t)dy * vh / g_sh) * vw +
                            (uint32_t)((int64_t)dx * vw / g_sw);
            if (rc_mask_bit(raw, sidx))
                rc_mask_set(dst, (uint32_t)dy * g_sw + dx);
        }
    heap_caps_free(raw);
    return dst;
}

static int strip_load(rc_strip_t *s, const char *path, const mpak_strip_t *hdr)
{
    memset(s, 0, sizeof *s);
    mpak_t pm;
    if (mpak_open(&pm, path, 0, MPAK_KIND_PARTS) != MPAK_OK) return MPAK_ERR_IO;
    if (pm.parts_count < 1 || !pm.parts_tab) { mpak_close(&pm); return MPAK_ERR_FMT; }
    const mpak_part_t *p = &pm.parts_tab[0];

    s->stride_b = rc_align4((uint32_t)p->w * 2u);
    s->w = p->w; s->h = p->h;
    s->px = psram((size_t)p->h * s->stride_b);
    if (!s->px) { mpak_close(&pm); return MPAK_ERR_NOMEM; }
    if (mpak_part_read_pixels(&pm, p, (uint8_t *)s->px,
                              (size_t)p->h * s->stride_b) != MPAK_OK) {
        heap_caps_free(s->px); s->px = NULL;
        mpak_close(&pm);
        return MPAK_ERR_IO;
    }
    if (p->has_alpha && (hdr->blend & 1u)) {
        s->mask = psram(p->mask_bytes);
        if (s->mask)
            mpak_part_read_mask(&pm, p, s->mask, p->mask_bytes);
    }
    mpak_close(&pm);   /* 条带小图已全量入 RAM */

    s->y       = hdr->y;
    s->speed_x = hdr->speed_x;
    s->rx      = hdr->rx_parallax;
    s->blend   = hdr->blend;
    s->ok      = true;
    return MPAK_OK;
}

/* ================= render.h 公共 API ================= */

/* _nolock 实现的前向声明（定义在文件后段；包装只负责加解锁） */
static int  render_set_parts_nolock(const char *mpk_path);
static int  render_set_layout_nolock(const char *mpk_path, bool loop);
static int  render_set_expression_nolock(const char *name);
static void render_force_redraw_nolock(void);
static int  render_enter_menu_nolock(void);
static int  render_exit_menu_nolock(void);
static int  render_set_map_nolock(const char *bgmap_path,
                                 const char *strip_parts_paths[], int strip_count);
static int  render_set_clock_nolock(const char *fonttime_parts_path,
                                    int16_t anchor_world_x, int16_t anchor_world_y,
                                    bool enable);

/* ══ 公共写 API 的加锁包装（2026-09-27 互斥修复，详见文件头 s_rlock 说明）══
 * 这些入口会改资产句柄/图层指针/实体缓冲/脏区，且**会被渲染任务之外的调用者调用**
 * （输入任务、状态机、asset_dl）。整段持锁 = 渲染任务的 flush 不会读到
 * "缓存已释放/图层已换/实体画一半"的中间态。同任务嵌套持锁安全（递归锁）。 */
int render_set_parts(const char *mpk_path)
{
    rc_lock();
    int r = render_set_parts_nolock(mpk_path);
    rc_unlock();
    return r;
}

int render_set_layout(const char *mpk_path, bool loop)
{
    rc_lock();
    int r = render_set_layout_nolock(mpk_path, loop);
    rc_unlock();
    return r;
}

int render_set_expression(const char *name)
{
    rc_lock();
    int r = render_set_expression_nolock(name);
    rc_unlock();
    return r;
}

void render_force_redraw(void)
{
    rc_lock();
    render_force_redraw_nolock();
    rc_unlock();
}

int render_enter_menu(void)
{
    rc_lock();
    int r = render_enter_menu_nolock();
    rc_unlock();
    return r;
}

int render_exit_menu(void)
{
    rc_lock();
    int r = render_exit_menu_nolock();
    rc_unlock();
    return r;
}

int render_set_map(const char *bgmap_path,
                   const char *strip_parts_paths[], int strip_count)
{
    rc_lock();
    int r = render_set_map_nolock(bgmap_path, strip_parts_paths, strip_count);
    rc_unlock();
    return r;
}

int render_set_clock(const char *fonttime_parts_path,
                     int16_t anchor_world_x, int16_t anchor_world_y, bool enable)
{
    rc_lock();
    int r = render_set_clock_nolock(fonttime_parts_path, anchor_world_x,
                                    anchor_world_y, enable);
    rc_unlock();
    return r;
}

int render_init(const minipet_profile_t *profile)
{
    if (!profile) return RENDER_ERR_ARG;
    if (g_inited) return RENDER_OK;
    if (!s_rlock) {
        s_rlock = xSemaphoreCreateRecursiveMutex();
        if (!s_rlock) return RENDER_ERR_NOMEM;
    }

    g_sw = profile->width;
    g_sh = profile->height;
    s_ground_shift_px = profile->ground_cam_shift_px;   /* 相机下移按板定稿 */
    if (g_sw <= 0 || g_sh <= 0 || g_sw > 512 || g_sh > 512)
        return RENDER_ERR_ARG;

    g_gw = (int)((g_sw + RC_CELL - 1) / RC_CELL);
    g_gh = (int)((g_sh + RC_CELL - 1) / RC_CELL);
    if (g_gw * g_gh > RC_GRID_MAX) return RENDER_ERR_ARG;

    if (display_init() != 0) {
        ESP_LOGE(TAG, "display_init failed");
        return RENDER_ERR_STATE;
    }

    g_fb     = psram((size_t)g_sw * g_sh * 2u);
    g_ent_px = psram((size_t)RC_ENT_W * RC_ENT_H * 2u);
    g_ent_cov = psram(RC_ENT_COV_BYTES);
    if (!g_fb || !g_ent_px || !g_ent_cov) return RENDER_ERR_NOMEM;
    /* PSRAM 不保证清零：覆盖位/像素必须先清空，否则首次 full_recompose 会把
     * 未初始化覆盖位当已画像素上屏（真机表现为黑色竖条 + 随机竖条纹残留） */
    memset(g_ent_px, 0, (size_t)RC_ENT_W * RC_ENT_H * 2u);
    memset(g_ent_cov, 0, RC_ENT_COV_BYTES);

    rc_anim_init(&g_anim);

    /* 问题3：屏尺寸/比例先行告知时钟模块（默认居中锚点与 get_rect 标脏依赖；
     * 旧实现 scale 在首次 compose 才赋值 → enable 后时钟矩形恒 0 永不标脏） */
    clock_digits_set_screen(g_sw, g_sh);

    int rc = bridge_init(g_sw, g_sh);
    if (rc != RENDER_OK) return rc;

    g_inited = true;
    full_recompose();   /* 黑底首帧 + hash 基线 + 全幅上屏 */
    /* 【拖拽极限自检 2026-09-27】用户要求核对"屏幕最底"与"拖动最低点"是否同一高度。
     * 这里把 drag 分别推到上下极限，打印：显示矩形（画布矩形）屏幕范围 + 锚点屏幕 y
     * + 屏幕底边，跑完复位。数字对得上就说明脚底正好踩屏底。 */

    ESP_LOGI(TAG, "render_init ok %dx%d", (int)g_sw, (int)g_sh);
    return RENDER_OK;
}

/* ══ 【拖影自检 2026-09-27】══════════════════════════════════════════════════
 * 用户报障"人物有拖影/双影"。脏区机制只要漏标一次，旧像素就会永久留在 AMOLED 上
 * ——肉眼极明显、但日志里什么都看不到。这里给出**可回归的硬判据**：
 *   把整屏重新合成到影子缓冲（g_fb 指针临时切过去），再与"线上"g_fb 逐像素比对：
 *   差异像素 = 脏区漏标留下的陈旧内容（真实拖影），打 WARN + 包围盒。
 * 1s 一次；MP_GHOST_PROBE=1 时启用（排障用，常态关）。 */
/* 【条带像素自证 2026-09-27】用户照片出现"竖条纹"= 掩码/取样写错。这里把**设备实际
 * 帧缓冲**里条带区域的若干像素按 hex 打出（附 last_off / y / w / h），主机端用同一
 * mpk + 同一算法算预期值逐点核对——不靠肉眼、不靠"看起来对"。 */
#define MP_STRIP_PIXEL_PROBE 0  /* 条带/地图层自证已 7/7+12/12 全等，常态关 */
#if MP_STRIP_PIXEL_PROBE
static void strip_pixel_probe(void)
{
    /* 【条带像素自证 2026-09-27】把每条带**单独**合成到零底暂存（只走 strip_blit），
     * 按 32 像素块打 FNV-1a 指纹 → 主机端用「正确 2x 展开」逐块对拍。
     * 竖条纹真凶（0xFF 掩码组漏 2x 复制、每 8 源像素相位重置）修复前这些指纹
     * 必然对不上；修复后应 15/15 块全等。 */
    if (!g_strips || g_strip_n <= 0) return;
    /* 装载缓冲指纹：证明设备 RAM 里的 px/mask 与主机端同文件读出的字节一致
     * （若不一致 ⇒ 设备上那份 .mpk 与本地不同，先查素材同步再谈渲染）。 */
    for (int i = 0; i < g_strip_n && i < 4; i++) {
        rc_strip_t *st = &g_strips[i];
        if (!st->ok) continue;
        uint32_t mb = ((uint32_t)st->w * st->h + 7u) / 8u;
        ESP_LOGW(TAG, "条带载入[%d] w=%d h=%d stride=%u px_fnv=%08x mask_fnv=%08x mask=%d",
                 i, (int)st->w, (int)st->h, (unsigned)st->stride_b,
                 (unsigned)rc_bytes_fnv((const uint8_t *)st->px,
                                        (uint32_t)st->h * st->stride_b),
                 st->mask ? (unsigned)rc_bytes_fnv(st->mask, mb) : 0u,
                 st->mask ? 1 : 0);
    }
    /* 地图层指纹：static/tile 已 2x 展开到屏尺寸、掩码同屏尺寸。与主机端按
     * 「实际布局偏移」展开后的字节对拍 = BGMAP 8 字节偏移自愈的端到端验证。 */
    if (g_static) {
        ESP_LOGW(TAG, "地图层自证 static_fnv=%08x",
                 (unsigned)rc_bytes_fnv((const uint8_t *)g_static,
                                        (uint32_t)g_sw * g_sh * 2u));
    }
    if (g_tile) {
        ESP_LOGW(TAG, "地图层自证 tile_fnv=%08x tile_mask_fnv=%08x",
                 (unsigned)rc_bytes_fnv((const uint8_t *)g_tile,
                                        (uint32_t)g_sw * g_sh * 2u),
                 g_tile_mask ? (unsigned)rc_bytes_fnv(g_tile_mask,
                                        ((uint32_t)g_sw * g_sh + 7u) / 8u) : 0u);
    }
    uint16_t *scratch = psram((size_t)g_sw * g_sh * 2u);
    if (!scratch) { ESP_LOGW(TAG, "条带自证：暂存分配失败"); return; }
    rc_lock();
    uint16_t *saved = g_fb;
    for (int i = 0; i < g_strip_n && i < 4; i++) {
        rc_strip_t *st = &g_strips[i];
        if (!st->ok) continue;
        memset(scratch, 0, (size_t)g_sw * g_sh * 2u);
        g_fb = scratch;
        strip_blit(st, 0, 0, g_sw, g_sh);
        g_fb = saved;
        for (int k = 0; k < 3; k++) {
            int32_t sy = ((int32_t)st->y << RC_SCALE_SHIFT) + 8 + k * 48;
            if (sy < 0 || sy >= g_sh) continue;
            const uint16_t *row = scratch + (size_t)sy * g_sw;
            char line[320];
            int n = 0;
            for (int32_t b = 0; b < g_sw / 32 && n < 250; b++) {
                n += snprintf(line + n, sizeof(line) - (size_t)n, "%08x",
                              (unsigned)rc_bytes_fnv((const uint8_t *)(row + b * 32), 64));
            }
            line[n < (int)sizeof line ? n : (int)sizeof line - 1] = 0;
            ESP_LOGW(TAG, "条带自证[%d] w=%d h=%d y=%d off=%d row=%d px0=%04x blk=%s",
                     i, (int)st->w, (int)st->h, (int)st->y, (int)st->last_off, (int)sy,
                     (unsigned)(row[0] & 0xFFFF), line);
        }
    }
    rc_unlock();
    heap_caps_free(scratch);
}
#endif

#if MP_GHOST_PROBE
static uint16_t *g_shadow;
static bool ghost_probe(void)
{
    if (!g_shadow) {
        g_shadow = psram((size_t)g_sw * g_sh * 2u);
        if (!g_shadow) return true;                 /* 分配失败：跳过检查 */
    }
    uint16_t *saved = g_fb;
    g_fb = g_shadow;
    compose_region(0, 0, g_sw, g_sh);
    g_fb = saved;
    uint32_t diff = 0;
    int32_t x0 = 99999, y0 = 99999, x1 = -1, y1 = -1;
    for (int32_t r = 0; r < g_sh; r++) {
        /* 条带（地图视差）按时间滚动，两次合成之间 offset 可能差 1px —— 那属于
         * 设计行为不是拖影。整条带行排除（`滚动的条带` 与 `漏标残留` 必须分开）。 */
        bool in_strip = false;
        for (int i = 0; i < g_strip_n; i++) {
            if (!g_strips[i].ok) continue;
            int32_t y0s = (int32_t)g_strips[i].y << RC_SCALE_SHIFT;
            int32_t y1s = y0s + ((int32_t)g_strips[i].h << RC_SCALE_SHIFT);
            if (r >= y0s && r < y1s) { in_strip = true; break; }
        }
        if (in_strip) continue;
        const uint16_t *a = g_fb + (size_t)r * g_sw;
        const uint16_t *b = g_shadow + (size_t)r * g_sw;
        for (int32_t c = 0; c < g_sw; c++) {
            if (a[c] == b[c]) continue;
            diff++;
            if (c < x0) x0 = c;
            if (c > x1) x1 = c;
            if (r < y0) y0 = r;
            if (r > y1) y1 = r;
        }
    }
    if (diff) {
        ESP_LOGW(TAG, "拖影自检：线上帧与重合成基准差 %u px bbox=(%d,%d)-(%d,%d) "
                      "（脏区漏标残留）", (unsigned)diff, (int)x0, (int)y0, (int)x1, (int)y1);
        /* 残留形状取证（前 2 次）：8×8 块打点图，'#'=线上≠基准。 */
        static int s_dump_n;
        if (s_dump_n < 2) {
            s_dump_n++;
            for (int32_t by = (y0 / 8) * 8; by <= y1 && by < g_sh; by += 8) {
                char line[64];
                int n = 0;
                for (int32_t bx = 0; bx < g_sw && n < 60; bx += 8) {
                    bool any = false;
                    for (int32_t r = by; r < by + 8 && r < g_sh && !any; r++) {
                        const uint16_t *a = g_fb + (size_t)r * g_sw;
                        const uint16_t *b = g_shadow + (size_t)r * g_sw;
                        for (int32_t c = bx; c < bx + 8 && c < g_sw; c++)
                            if (a[c] != b[c]) { any = true; break; }
                    }
                    line[n++] = any ? '#' : '.';
                }
                line[n] = 0;
                ESP_LOGW(TAG, "拖影形状 y=%03d |%s|", (int)by, line);
            }
        }
        return false;
    }
    return true;
}
#endif

#if MP_DRAG_LIMIT_SELFTEST
/* 【拖拽极限自检 2026-09-27】用户要求核对"屏幕最底"与"拖动最低点"是否同一高度。
 * 把 drag 推到上下极限，打印画布矩形屏幕范围 + 锚点屏幕 y + 屏底，跑完复位。
 * 必须等画布就绪（g_ent_cbox_ok）——否则 ent_disp_size()=0，夹取会提前返回。 */
static void drag_limit_selftest(void)
{
    int32_t keep_x = g_drag_off_x, keep_y = g_drag_off_y;
    int32_t bx, by, ex, ey, dw, dh;
    render_set_drag_off_y(-100000);
    ent_screen_rect_at(g_tilt_mdeg, &bx, &by, &ex, &ey, &dw, &dh);
    int32_t anchor_top = by + (g_ent_oy - g_ent_cy0) * RC_SCALE;
    ESP_LOGW(TAG, "拖拽极限自检 上界：drag_y=%d 画布 y=%d..%d 锚点屏 y=%d（屏顶=0）",
             (int)g_drag_off_y, (int)by, (int)(by + dh * 0 + (dh ? dh : 0)), (int)anchor_top);
    render_set_drag_off_y(100000);
    ent_screen_rect_at(g_tilt_mdeg, &bx, &by, &ex, &ey, &dw, &dh);
    int32_t anchor_bot = by + (g_ent_oy - g_ent_cy0) * RC_SCALE;
    ESP_LOGW(TAG, "拖拽极限自检 下界：drag_y=%d 画布 y=%d..%d 锚点屏 y=%d（地面线=%d，来源=%s）→ %s",
             (int)g_drag_off_y, (int)by, (int)(by + (dh ? dh : 0)),
             (int)anchor_bot, (int)ground_line_y(),
             g_ground_tbl_on ? "本图地面表" : "通用线(屏底-20)",
             anchor_bot == ground_line_y() ? "脚底正好踩在地面线上 ✓" : "与地面线不一致 ✗");
    g_drag_off_x = keep_x;
    g_drag_off_y = keep_y;
}
#endif

/* ══ 【越屏弹回 2026-10-01】用户定稿：宠物在屏幕外的面积 >50% → 弹回边框 ══
 * 触发场景：动作/表情切换（fly 画布 368×258 ≫ stand 214×168）时画布绕 origin
 * 重展，setter 时夹好的 drag 对新画布失效 → 大半出屏；快速甩动同理。
 * 每帧合成前跑：可见面积 <50% → 按当前画布把 drag 夹回"矩形完全在屏内"
 * （与 drag_clamp 同数学）= 贴边弹回；可见 ≥50% 不干预（全屏拖拽保留）。 */
static void ent_bounce_if_offscreen(void)
{
    if (!g_ent_cbox_ok || !g_inited) return;
    int32_t dw, dh, bx, by;
    ent_disp_size(&dw, &dh);
    if (dw <= 0 || dh <= 0) return;
    ent_screen_pos_at(g_tilt_mdeg, &bx, &by);
    int32_t x1 = bx + dw, y1 = by + dh;
    int32_t ox = (x1 < g_sw ? x1 : g_sw) - (bx > 0 ? bx : 0);
    int32_t oy = (y1 < g_sh ? y1 : g_sh) - (by > 0 ? by : 0);
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    if ((int64_t)ox * oy * 2 >= (int64_t)dw * dh) return;   /* 可见 ≥50%：不弹 */
    int32_t px = g_drag_off_x, py = g_drag_off_y;
    drag_clamp(&px, &py);
    if (px != g_drag_off_x || py != g_drag_off_y) {
        mark_ent();                            /* 旧位置标脏（防瞬移残影） */
        g_drag_off_x = px;
        g_drag_off_y = py;
        mark_ent();                            /* 新位置标脏 */
        ESP_LOGI(TAG, "越屏弹回：可见(%dx%d)/画布(%dx%d)<50%% → drag 夹回(%d,%d)",
                 ox, oy, (int)dw, (int)dh, (int)px, (int)py);
    }
}

void render_tick(void)
{
    if (!g_inited) return;
    int64_t now_us = esp_timer_get_time();

    if (g_menu) {
        /* MENU：LVGL 整屏离屏 → 直拷 framebuffer → 上屏（合成器让路）。
         * 问题4 加固：每帧全屏重绘。DIRECT 模式下 LVGL 只重绘失效区，
         * menu_buf 常驻持有完整画面；若按脏 bbox 增量上屏，LVGL「本帧无
         * 失效区」时无 flush → 不 blit，未刷新区域与残留叠加会闪烁。 */
        /* 【卡死取证探针 2026-09-27】用户报"菜单里按键全不动、选中不变"，
         * 且采样见 uptime 停滞 → 渲染任务疑似卡在菜单路径。此处分三段计时，
         * 卡死时日志停在哪个字（lvgl/memcpy/blit）即可定位。每 2s 一条，
         * 修复后降级为 DEBUG。 */
        int64_t t0 = esp_timer_get_time();
        lv_timer_handler();
        int64_t t1 = esp_timer_get_time();
        const uint16_t *mb = bridge_menu_buf();
        if (mb) {
            /* 菜单整屏拷贝+上屏同样要与跨任务 compose/blit 互斥（否则菜单顶上
             * 叠一块 POKER 画面；s_blit_stage 亦为共用静态）。 */
            rc_lock();
            for (int32_t r = 0; r < g_sh; r++)
                memcpy(g_fb + (size_t)r * g_sw, mb + (size_t)r * g_sw,
                       (size_t)g_sw * 2u);
            int64_t t2 = esp_timer_get_time();
            blit_be(0, 0, g_sw, g_sh, g_fb, g_sw);
            rc_unlock();
            int64_t t3 = esp_timer_get_time();
            (void)t0; (void)t1; (void)t2; (void)t3;
        } else {
            static int64_t s_mb_null_us;
            if (t1 - s_mb_null_us > 2000000) {
                s_mb_null_us = t1;
                ESP_LOGE("menu", "bridge_menu_buf() 为 NULL：菜单缓冲缺失（lvgl=%lldms）",
                         (long long)((t1 - t0) / 1000));
            }
        }
        return;
    }

#if MP_DRAG_LIMIT_SELFTEST
    {
        static bool s_drag_selftest_done;
        if (!s_drag_selftest_done && g_ent_cbox_ok) {
            s_drag_selftest_done = true;
            drag_limit_selftest();
        }
    }
#endif

    bool any = false;

    /* 【越屏弹回】动作切换后画布重展可能让宠物大半出屏 → 每帧先纠偏 */
    ent_bounce_if_offscreen();

    /* 1) 实体动画帧/表情/blink */
    rc_anim_ev_t ev;
    static uint32_t s_dbg_frames; static int64_t s_dbg_last;
    if (rc_anim_advance(&g_anim, now_us, &ev)) {
        s_dbg_frames++;
        if (now_us - s_dbg_last > 3000000) {   /* 3s 一次：帧推进实证（诊断期探针，DEBUG 级） */
            ESP_LOGD("dbg", "帧推进: 3s 内 %u 次, 当前帧 %u/%u", s_dbg_frames, g_anim.frame_idx, g_anim.layout->frame_count);
            s_dbg_frames = 0; s_dbg_last = now_us;
        }
        if (ev.finished) {
            /* 单次动作播完 → 回退 standby 循环布局（stand1） */
            mark_ent();
            mpak_close(&g_lt_once);
            g_lt_once_ok = false;
            bind_active_layout(now_us, false);
            recompose_entity();
            mark_ent();
            any = true;
        } else {
            mark_ent();                          /* 旧位置 */
            /* 帧位移 move 已在实体画布内逐帧绝对叠加（recompose_entity，
             * 对齐桌面 +mv 语义），不再累计到屏幕锚点 */
            recompose_entity();
            mark_ent();                          /* 新位置 */
            /* 探针（数据侧判据）：帧推进后实体缓冲指纹应变化。
             * 连续 20 帧指纹相同才 WARN 一条（防动画真坏时无声，又避免
             * 每帧刷屏）；指纹恢复变化即清零重新计数 */
            {
                uint32_t h = ent_fb_hash();
                static uint32_t s_hash_same_streak;
                if (h == s_ent_fb_hash_last) {
                    if (++s_hash_same_streak == 20) {
                        const mpak_layout_t *lt = active_layout();
                        const mpak_frame_t *fr = &lt->frames[g_anim.frame_idx];
                        ESP_LOGW(TAG, "帧 %" PRIu32 " 连续 20 帧实体位图与上帧相同 hash=%08"
                                 PRIx32 " pieces=%" PRIu32,
                                 g_anim.frame_idx, h, fr->piece_count);
                        for (uint32_t k = 0; k < fr->piece_count && k < 8; k++) {
                            const mpak_piece_t *pc = &lt->pieces[fr->piece_off + k];
                            const rc_part_img_t *im = resolve_piece(pc);
                            ESP_LOGW(TAG, "  piece[%" PRIu32 "] part=%" PRIu32
                                     " expr=%u flip=%u img_hash=%08" PRIx32,
                                     (uint32_t)k, pc->part_id,
                                     (unsigned)pc->expr_index,
                                     (unsigned)(pc->flip & 1u),
                                     im ? im->px_hash : 0);
                        }
                    }
                } else {
                    s_hash_same_streak = 0;
                    ESP_LOGD(TAG, "ent fb hash %08" PRIx32 " -> %08" PRIx32
                             " (frame %" PRIu32 ")",
                             s_ent_fb_hash_last, h, g_anim.frame_idx);
                }
                s_ent_fb_hash_last = h;
            }
            any = true;
        }
    }

    /* 2) 条带偏移（时间驱动 + IMU 视差；offset 不变则零成本） */
    /* 【刷新限频 2026-09-27】条带整幅 480×444 重合成实测 compose 157ms/笔：
     * 5~10px/s 的滚动若每变化 1px 就刷一次，等于每秒 5~10 笔整屏 → 拖拽必卡。
     * 这里限到最多 8Hz（每笔 ≥125ms），观感上仍是连续滚动（滚动速度未变，
     * 只是位移以更少的大步长呈现），但整屏重合成笔数下降数倍。 */
    {
        static int64_t s_strip_ms;
        /* 整屏条带 flush 实测 compose 80ms + blit 58ms ≈ 138ms/笔 ⇒ 8Hz 会吃掉
         * 全部渲染预算（人物呼吸都会卡）。修完上屏竞态后 blit 变成"诚实耗时"（以前
         * 与 DMA 重叠所以显得快），这里把条带刷新降到 4Hz；滚动速度不变，只是步长变大。 */
        bool strip_window = (now_us / 1000 - s_strip_ms) > 250;
        if (g_ui_active_until_ms > now_us / 1000) strip_window = false;   /* 交互期冻结 */
        if (strip_window) s_strip_ms = now_us / 1000;
        for (int i = 0; i < g_strip_n; i++) {
            if (!g_strips[i].ok) continue;
            int32_t off = strip_offset(&g_strips[i], now_us);
            if (off != g_strips[i].last_off && strip_window) {
                g_strips[i].last_off = off;
                mark_rect(0, (int32_t)g_strips[i].y << RC_SCALE_SHIFT,
                          g_sw, (int32_t)g_strips[i].h << RC_SCALE_SHIFT);
                any = true;
            }
        }
    }

    /* 3) 地图时钟（每秒标脏；comma 偶显奇隐） */
    if (clock_digits_active()) {
        time_t t = time(NULL);
        if (t != g_last_clock_t) {
            g_last_clock_t = t;
            int32_t cx, cy, cw, ch;
            if (clock_digits_get_rect(&cx, &cy, &cw, &ch))
                mark_rect(cx, cy, cw, ch);
            any = true;
        }
    }

    /* 4) 倾斜/拖拽视差 → 实体 x 偏移跟随（问题6/7 可见反馈：±8° ↔ ±8px；
     * 条带偏移变化已在步骤 2 标脏，实体需另行以新旧位置标脏防残影） */
    static int32_t s_last_drag_x, s_last_drag_y;
    if (g_tilt_mdeg != s_last_ent_tilt) {
        mark_ent_at(s_last_ent_tilt);        /* 旧位置 */
        s_last_ent_tilt = g_tilt_mdeg;
        mark_ent();                          /* 新位置 */
        any = true;
    }
    /* 拖拽：旧位置必须用【旧偏移】标记（此前 mark_ent 两次都读新值，
     * 旧位置永不重绘 → 拖动轨迹残影，真机照片实证） */
    if (g_drag_off_x != s_last_drag_x || g_drag_off_y != s_last_drag_y) {
        int32_t ox = g_drag_off_x, oy = g_drag_off_y;
        int32_t poldx = s_last_drag_x, poldy = s_last_drag_y;  /* 探针：覆盖前先存真旧值 */
        g_drag_off_x = poldx; g_drag_off_y = poldy;
        mark_ent();                          /* 旧位置 */
        g_drag_off_x = ox; g_drag_off_y = oy;
        mark_ent();                          /* 新位置 */
        s_last_drag_x = ox; s_last_drag_y = oy;
        any = true;
        /* 【拖拽探针】pold→new 真实偏移变化（此前在 s_last 已被更新成新值
         * 之后才算 s_last-ox，恒为 0；根因已定位，降 INFO 级 + 1s 限频） */
        static int64_t s_drag_probe_us;
        if (now_us - s_drag_probe_us > 1000000) {
            s_drag_probe_us = now_us;
            ESP_LOGI("probe", "drag old(%" PRId32 ",%" PRId32 ")->new(%"
                     PRId32 ",%" PRId32 ") tilt=%" PRId32,
                     poldx, poldy, ox, oy, g_tilt_mdeg / 1000);
        }
    }

    /* 5) 定时横幅到期自动隐藏（render_banner_show_for；expire_us=0 的常驻
     * 横幅——配网横幅——不走此路径） */
    if (g_banner_on && g_banner_expire_us != 0 && now_us >= g_banner_expire_us) {
        g_banner_expire_us = 0;
        g_banner_on = false;
        mark_rect(0, RC_BANNER_Y, g_sw, RC_BANNER_H);   /* 横幅带整条标脏，下层重铺 */
        any = true;
    }

    /* 6) BGM 半屏控制条（E6）：自动收起 / BGM 状态变化重绘 */
    ov_tick(now_us);

    /* 【拖影兜底清屏 2026-09-27】用户报"人物有拖影（双影）"且反复未被脏区修复
     * 覆盖住。脏区机制一旦有任一路径漏标（实体位移/条带/气泡/时钟切换），
     * 旧像素就会永久留在 AMOLED 上形成双影——排查成本高、用户可见度极高。
     * 这里加一条"兜底全屏重合成"：空闲 1s 无新脏区时强制整屏重绘一次，
     * 把任何残留像素抹掉（整屏 blit 约 20 次 SPI 传输，1s 一次对 AMOLED
     * 无感）。有脏区的帧不受影响，动画流畅度不变。 */

    if (any) flush_dirty();

#if MP_STRIP_PIXEL_PROBE
    {   /* 一次性：开机 12s 后打一组条带实际像素（主机端核对用） */
        static bool s_spp_done;
        int64_t now_us2 = esp_timer_get_time();
        if (!s_spp_done && now_us2 > 12000000) { s_spp_done = true; strip_pixel_probe(); }
    }
#endif

#if MP_GHOST_PROBE
    /* 拖影自检：1s 一次（重合成整屏有成本，别每帧做） */
    {
        static int64_t s_gp_ms;
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - s_gp_ms > 1000) {
            s_gp_ms = now_ms;
            ghost_probe();
        }
    }
#endif

    /* ENTPOS 探针已移除（诊断期结束） */

}

static int render_set_parts_nolock(const char *mpk_path)
{
    if (!g_inited || !mpk_path) return RENDER_ERR_ARG;
    mpak_t tmp;
    int rc = mpak_open(&tmp, mpk_path, 0, MPAK_KIND_PARTS);
    if (rc != MPAK_OK) return rc;

    pc_flush();
    if (g_parts_ok) mpak_close(&g_parts);
    g_parts = tmp;             /* FILE* 所有权转移 */
    g_parts_ok = true;
    ent_canvas_invalidate();   /* part 尺寸可能变化 → 画布联合包围盒重算 */

    mark_ent();                /* 下一 tick 以新部件重合成实体层 */
    return RENDER_OK;
}

/* 【换装错配保护 2026-09-27】新 LAYOUT 与当前 PARTS 包必须成对：服务端按外观
 * hash 同时产出两者，但设备可能只拉到 LAYOUT（TF 挂载失败走 Flash 出厂素材、
 * 或 PARTS 下载失败）→ 帧里引用的 part_id 在 PARTS 里一个都找不到，
 * 渲染出来就是"人物整个消失"（真机实证：piece part 1106/821/1755… not in PARTS pkg，
 * 用户症状"人物也没了"）。
 *
 * 这里在**换绑之前**校验：新 LAYOUT 的部件命中率过低就拒绝切换（保留原画面），
 * 同时请素材任务做一次全量同步（拿回与之配套的 PARTS）。宁可暂时显示旧形象，
 * 也不给用户一片空白。 */
static bool layout_matches_current_parts(const mpak_t *lt)
{
    const mpak_layout_t *l = lt->u.layout;   /* mpak_t.u.layout 本就是指针 */
    if (!g_parts_ok) return true;            /* PARTS 未加载：交给原有降级路径 */
    if (!l->frames || l->frame_count == 0 || !l->pieces) return true;
    uint32_t total = 0, hit = 0;
    uint32_t shown = 0;
    for (uint32_t f = 0; f < l->frame_count && f < 4; f++) {   /* 抽样前 4 帧足够判配 */
        const mpak_frame_t *fr = &l->frames[f];
        for (uint32_t k = 0; k < fr->piece_count; k++) {
            const mpak_piece_t *pc = &l->pieces[fr->piece_off + k];
            total++;
            if (mpak_parts_find(&g_parts, pc->part_id)) hit++;
            else if (shown < 4) {
                shown++;
                ESP_LOGW(TAG, "  [错配] 帧%u piece[%u] part=%u 不在当前 PARTS 包",
                         (unsigned)f, (unsigned)k, (unsigned)pc->part_id);
            }
        }
    }
    if (total == 0) return true;
    /* 命中率 < 50% 判为错配（正常换装应接近 100%；表情变体缺失不算错配，
     * 因为 part_id 本身仍在包里） */
    bool ok = (hit * 2 >= total);
    if (!ok) {
        ESP_LOGE(TAG, "LAYOUT 与 PARTS 不匹配（命中 %u/%u）→ 拒绝换绑，保留当前形象",
                 (unsigned)hit, (unsigned)total);
    }
    return ok;
}

static int render_set_layout_nolock(const char *mpk_path, bool loop)
{
    if (!g_inited || !mpk_path) return RENDER_ERR_ARG;
    mpak_t tmp;
    int rc = mpak_open(&tmp, mpk_path, 0, MPAK_KIND_LAYOUT);
    if (rc != MPAK_OK) return rc;

    /* 换绑前先校验与当前 PARTS 的配套性（不匹配则不切、并请求全量同步） */
    if (!layout_matches_current_parts(&tmp)) {
        mpak_close(&tmp);
        extern void asset_dl_request_sync(void);
        asset_dl_request_sync();      /* 拉回配套 PARTS，下次 SET_ACTION/SET_MAP 会重试 */
        return RENDER_ERR_STATE;
    }

    mark_ent();
    if (loop) {
        if (g_lt_loop_ok) mpak_close(&g_lt_loop);
        g_lt_loop = tmp;
        g_lt_loop_ok = true;
    } else {
        if (g_lt_once_ok) mpak_close(&g_lt_once);
        g_lt_once = tmp;
        g_lt_once_ok = true;
    }
    bind_active_layout(esp_timer_get_time(), false);
    recompose_entity();
    mark_ent();
    return RENDER_OK;
}

static int render_set_expression_nolock(const char *name)
{
    if (!g_inited || !name) return RENDER_ERR_ARG;
    int rc = rc_anim_set_expression(&g_anim, name);
    if (rc == 0) {
        mark_ent();            /* 旧覆盖区（表情件形状可能缩小） */
        recompose_entity();
        mark_ent();
    }
    return rc;
}

/* 强制一次全屏重合成 + 全幅上屏（脏区基线同步重建）。
 * 用于外部直写面板（面板自检色块等）或素材全量重绑后清除残留：
 * 无 BGMAP → 全屏填黑；有 BGMAP → static_back+条带+tile 一次铺满。 */
static void render_force_redraw_nolock(void)
{
    if (!g_inited) return;
    full_recompose();
}

/* 【花屏二分已结案 2026-10-01】true=只装载静态层（调试遗留，fbceb82 带入）：
 * 真机实证后果=条带段（树冠/房子/丘陵）与 tile 地形全不装载 → 只剩天空
 * static（用户报障"地图渲染有问题/背景只剩天空"）。新分段导出格式下
 * static 本身就只是天空段，场景中间层全在条带里——此开关必须为 false。 */
static bool g_map_static_only = false;

static int render_set_map_nolock(const char *bgmap_path,
                                const char *strip_parts_paths[], int strip_count)
{
    if (!g_inited || !bgmap_path) return RENDER_ERR_ARG;

    mpak_t bm;
    int rc = mpak_open(&bm, bgmap_path, 0, MPAK_KIND_BGMAP);
    if (rc != MPAK_OK) return rc;
    const mpak_bgmap_t *bg = bm.u.bgmap;

    if ((int)bg->strip_count != strip_count) {
        ESP_LOGE(TAG, "strip count mismatch: bgmap=%u given=%d",
                 bg->strip_count, strip_count);
        mpak_close(&bm);
        return RENDER_ERR_ARG;
    }

    ESP_LOGW(TAG, "[取证] bg vw=%u vh=%u static_off=%u static_len=%u tile_len=%u tile_off=%u strips=%u",
             bg->vw, bg->vh, bg->static_back_off, bg->static_back_len,
             bg->tile_layer_len, bg->tile_layer_off, bg->strip_count);

    scene_free();

    g_static = layer_rgb_load(&bm, bg->static_back_off, bg->static_back_len,
                              bg->vw, bg->vh);
    if (!g_static) {
        ESP_LOGE(TAG, "static_back load failed (vw=%u vh=%u)", bg->vw, bg->vh);
        mpak_close(&bm);
        full_recompose();
        return MPAK_ERR_FMT;
    }
    if (bg->tile_layer_len && !g_map_static_only) {
        uint32_t stride_b = rc_align4((uint32_t)bg->vw * 2u);
        g_tile = layer_rgb_load(&bm, bg->tile_layer_off,
                                (uint32_t)bg->vh * stride_b, bg->vw, bg->vh);
        g_tile_mask = tile_mask_load(&bm,
                                     bg->tile_layer_off + (uint32_t)bg->vh * stride_b,
                                     bg->tile_layer_len - (uint32_t)bg->vh * stride_b,
                                     bg->vw, bg->vh);
    }

    if (strip_count > 0 && !g_map_static_only) {
        g_strips = psram((size_t)strip_count * sizeof(rc_strip_t));
        if (!g_strips) { strip_count = 0; }
        g_strip_n = strip_count;
        for (int i = 0; i < strip_count; i++) {
            rc = strip_load(&g_strips[i], strip_parts_paths[i], &bg->strips[i]);
            if (rc != MPAK_OK) {
                ESP_LOGE(TAG, "strip %d load failed (%s)", i, strip_parts_paths[i]);
                g_strips[i].ok = false;   /* 跳过该条带，其余照常 */
            }
        }
    }

    /* 【地面表选择：必须在 mpak_close 之前！】bg 指向 bm.u.bgmap，close 会 free 掉它，
     * 之后再读 bg->map_id 就是 use-after-free（真机首版实测：打印出成串乱码、
     * strcmp 命中失败 → 地面表没启用、站位仍是通用线 460）。 */
    g_ground_tbl_on = (strcmp(bg->map_id, kGroundMapId) == 0);
    ESP_LOGI(TAG, "地面线来源：%s（map_id=%s）",
             g_ground_tbl_on ? "本图内置地面表" : "通用线(屏底-20)", bg->map_id);

    /* 【相机下移实验】装载完成后把各层上移，底部缺失行用内嵌地面带补 */
    ground_cam_shift_layers();
    /* 【缩放器取证】打印静态层首行前 8 像素（与 PC 端导出文件期望值比对） */
    ESP_LOGW(TAG, "static row0: %04x %04x %04x %04x %04x %04x %04x %04x | row1: %04x %04x %04x %04x",
             g_static[0], g_static[1], g_static[2], g_static[3],
             g_static[4], g_static[5], g_static[6], g_static[7],
             g_static[g_sw], g_static[g_sw + 1], g_static[g_sw + 2], g_static[g_sw + 3]);

    g_map_epoch_us = esp_timer_get_time();
    g_map_ok = true;
    mpak_close(&bm);           /* 场景已全量入 PSRAM */

    /* 【换地图 → 人物归位到地面 tile 线上 2026-09-27】新地图的地面/视口不同，
     * 人物应重新站在"屏底往上 20px"的地面线上（与上电首次就绪同一条线）。
     * 画布还没就绪（开机时地图先于 LAYOUT 绑定）就留给 ent_canvas_update 的
     * 首次就绪路径，不要在这里把 g_stand_done 提前置位。 */
    if (g_ent_cbox_ok) { g_stand_done = true; ent_stand_on_ground_locked(); }

    full_recompose();
    return RENDER_OK;
}

void render_set_entity_pos(int16_t world_x, int16_t world_y)
{
    if (!g_inited) return;
    rc_lock();
    mark_ent();
    g_ent_base_wx = world_x;
    g_ent_base_wy = world_y;
    mark_ent();
    rc_unlock();
}

static int render_set_clock_nolock(const char *fonttime_parts_path,
                                  int16_t anchor_world_x, int16_t anchor_world_y,
                                  bool enable)
{
    if (!g_inited) return RENDER_ERR_ARG;
    int rc = clock_digits_configure(fonttime_parts_path, anchor_world_x,
                                    anchor_world_y, enable);
    if (rc != MPAK_OK) return rc;
    g_last_clock_t = 0;        /* 下一 tick 立即标脏 */
    /* 问题3：enable/disable 都全幅重合成——doze 语义是「纯黑底+时钟」，
     * 必须清掉进 doze 前的场景残留（局部标脏会留下旧画面） */
    full_recompose();
    return RENDER_OK;
}

int render_set_font(render_font_t id, const char *mpk_path)
{
    if (!g_inited || !mpk_path) return RENDER_ERR_ARG;
    return font_lazy_init((font_id_t)id, mpk_path);
}

const lv_font_t *render_get_font(render_font_t id)
{
    return font_lazy_get((font_id_t)id);
}

static int render_enter_menu_nolock(void)
{
    if (!g_inited) return RENDER_ERR_STATE;
    if (g_menu) return RENDER_OK;
    int rc = bridge_mode_menu();
    if (rc != RENDER_OK) return rc;
    g_menu = true;
    return RENDER_OK;
}

static int render_exit_menu_nolock(void)
{
    if (!g_inited) return RENDER_ERR_STATE;
    if (!g_menu) return RENDER_OK;
    int rc = bridge_mode_poker();
    if (rc != RENDER_OK) return rc;
    g_menu = false;
    bind_active_layout(esp_timer_get_time(), false);  /* 重启帧时钟，防快进 */
    rc_anim_kick_blink(&g_anim, esp_timer_get_time());
    full_recompose();
    return RENDER_OK;
}

lv_display_t *render_lvgl_display(void)
{
    return bridge_display();
}

/* 桥接字体缺失时的内置 5x7 兜底：配对码=纯数字，绝不能因服务端字体链
 * 断供（E12）而让配对流程卡死。行距用 RC_BUBBLE_MAX_W（compose 读取步长）。 */
static int bubble_render_fallback(const char *text, uint16_t *buf,
                                  int32_t max_w, int32_t max_h,
                                  int32_t *out_w, int32_t *out_h)
{
    const int32_t scale = 3, pad = 6, border = 2, gap = 3;
    int32_t n = 0;
    for (const char *p = text; *p; p++) n++;
    if (n == 0) return RENDER_ERR_ARG;
    int32_t inner_w = n * (MP_FONT_GLYPH_W * scale + gap) - gap;
    int32_t inner_h = MP_FONT_GLYPH_H * scale;
    int32_t bw = inner_w + 2 * (pad + border);
    int32_t bh = inner_h + 2 * (pad + border);
    if (bw > max_w || bh > max_h) return RENDER_ERR_ARG;

    for (int32_t y = 0; y < bh; y++)
        for (int32_t x = 0; x < bw; x++) {
            bool bd = x < border || y < border || x >= bw - border || y >= bh - border;
            buf[(size_t)y * RC_BUBBLE_MAX_W + x] = bd ? 0x303030 : 0xF7F7F2;
        }
    int32_t cx = pad + border;
    for (const char *p = text; *p; p++) {
        unsigned char u = (unsigned char)*p;
        if (u >= 'a' && u <= 'z') u -= 32;
        if (u < ' ' || u > 126) u = '?';
        const uint8_t *cols = MP_FONT5X7[u];
        for (int32_t col = 0; col < MP_FONT_GLYPH_W; col++)
            for (int32_t row = 0; row < MP_FONT_GLYPH_H; row++) {
                if (!(cols[col] & (1u << row))) continue;
                for (int32_t dy = 0; dy < scale; dy++)
                    for (int32_t dx = 0; dx < scale; dx++)
                        buf[(size_t)(pad + border + row * scale + dy) * RC_BUBBLE_MAX_W +
                            cx + col * scale + dx] = 0x101010;
            }
        cx += MP_FONT_GLYPH_W * scale + gap;
    }
    *out_w = bw;
    *out_h = bh;
    return RENDER_OK;
}

int render_bubble_show(const char *text, render_font_t font)
{
    if (!g_inited) return RENDER_ERR_STATE;
    rc_lock();
    if (g_menu) { rc_unlock(); return RENDER_ERR_STATE; }
    if (!text || !text[0]) { render_bubble_hide(); rc_unlock(); return RENDER_OK; }

    uint16_t *buf = psram((size_t)RC_BUBBLE_MAX_W * RC_BUBBLE_MAX_H * 2u);
    if (!buf) { rc_unlock(); return RENDER_ERR_NOMEM; }

    int32_t bw = 0, bh = 0;
    int rc = bridge_bubble_render(text, (int)font, buf, RC_BUBBLE_MAX_W,
                                  RC_BUBBLE_MAX_W, RC_BUBBLE_MAX_H, &bw, &bh);
    if (rc != RENDER_OK) {
        rc = bubble_render_fallback(text, buf, RC_BUBBLE_MAX_W, RC_BUBBLE_MAX_H, &bw, &bh);
        if (rc != RENDER_OK) {
            heap_caps_free(buf);
            rc_unlock();
            return rc;
        }
        ESP_LOGW(TAG, "气泡走内置 5x7 兜底（桥接字体缺失）");
    }

    if (g_bub.active) mark_rect(g_bub.x, g_bub.y, g_bub.w, g_bub.h);
    if (g_bub.px) heap_caps_free(g_bub.px);
    g_bub.px = buf;
    g_bub.w = bw;
    g_bub.h = bh;
    g_bub.active = true;

    /* 锚在实体上方（实体显示区顶部居中；越界落到实体下方） */
    int32_t ex, ey, dw, dh;
    ent_screen_pos(&ex, &ey);
    ent_disp_size(&dw, &dh);
    int32_t bx = ex + dw / 2 - bw / 2;
    if (bx < 0) bx = 0;
    if (bx + bw > g_sw) bx = g_sw - bw;
    int32_t by = ey - bh - 8;
    if (by < 0) by = ey + dh + 8;
    if (by + bh > g_sh) by = g_sh - bh;
    if (by < 0) by = 0;
    g_bub.x = bx;
    g_bub.y = by;

    mark_rect(bx, by, bw, bh);
    rc_unlock();
    return RENDER_OK;
}

void render_bubble_hide(void)
{
    if (!g_inited || !g_bub.active) return;
    rc_lock();
    mark_rect(g_bub.x, g_bub.y, g_bub.w, g_bub.h);
    heap_caps_free(g_bub.px);
    g_bub.px = NULL;
    g_bub.active = false;
    rc_unlock();
}

/* ---------------- 未配网常驻横幅（问题4） ----------------
 * POKER 态顶部 480×28 深色底白字（5x7 内嵌字体 ×2，文本需 ASCII 大写），
 * compose_region 最顶层绘制；CLOCK_DOZE（时钟激活）态自动让位不画。 */

int render_banner_show(const char *text)
{
    rc_lock();
    if (!g_inited) { rc_unlock(); return RENDER_ERR_STATE; }
    if (g_banner_on) mark_rect(0, RC_BANNER_Y, g_sw, RC_BANNER_H);
    g_banner_on = true;
    g_banner_expire_us = 0;    /* 常驻：清定时横幅计时，防 show_for 残留到期误隐藏配网横幅 */
    g_banner_text[0] = 0;
    if (text) strlcpy(g_banner_text, text, sizeof(g_banner_text));
    mark_rect(0, RC_BANNER_Y, g_sw, RC_BANNER_H);
    rc_unlock();
    return RENDER_OK;
}

void render_banner_hide(void)
{
    if (!g_inited || !g_banner_on) return;
    rc_lock();
    g_banner_on = false;
    g_banner_expire_us = 0;
    mark_rect(0, RC_BANNER_Y, g_sw, RC_BANNER_H);
    rc_unlock();
}

/* 定时横幅（输入层 VOL± 反馈）：显示 text 并在 duration_ms 后由 render_tick
 * 自动隐藏；重复调用刷新文本与计时。签名勿动（输入层按此调用）。
 * 常驻横幅（配网）仍走 render_banner_show（内部清到期时刻，不受定时影响）。 */
int render_banner_show_for(const char *text, uint32_t duration_ms)
{
    rc_lock();
    int rc = render_banner_show(text);      /* 递归锁：同任务嵌套安全 */
    if (rc != RENDER_OK) { rc_unlock(); return rc; }
    g_banner_expire_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    rc_unlock();
    return rc;
}

void render_input_tilt(float tilt_deg)
{
    if (tilt_deg > 8.0f) tilt_deg = 8.0f;
    if (tilt_deg < -8.0f) tilt_deg = -8.0f;
    rc_lock();
    g_tilt_mdeg = (int32_t)(tilt_deg * 1000.0f);   /* 对齐 32bit 原子写 */
    /* tilt 视差（±8px）改变实体基准 → 重新夹取拖拽偏移，防倾斜时被边缘裁切 */
    {
        int32_t dx = g_drag_off_x, dy = g_drag_off_y;
        drag_clamp(&dx, &dy);
        g_drag_off_x = dx; g_drag_off_y = dy;
    }
    rc_unlock();
}

/* ================= 调试取证：framebuffer → TF 卡 BMP 截图 =================
 * 用途：真机渲染问题（残影/错帧/缺块/色偏）的照片取证不如位图可对拍——
 * 主机端把 BMP 逐像素与合成器预期结果比对，故障在上屏链路还是合成阶段一目了然。
 *
 * 实现（约束：内部堆仅 ~17KB，**禁止**分配整帧 24 位缓冲）：
 *   · 流式写：static 一行缓冲（≤1543B，内部 BSS 不占运行堆）逐行
 *     RGB565→BGR888 转换后 write 到 FATFS；480×480×3 ≈ 675KB 写入实测
 *     量级 100–200ms，渲染任务内执行（main.c render_task 排空 cmd_q），
 *     且每条指令后有 watchdog_kick()，不会触发 E14 熔断。
 *   · BMP 布局：54B 头 + 自下而上（biHeight>0）的 24 位 BGR 行；
 *     行按 4 字节对齐（480×3=1440 天然对齐，pad 逻辑仍按通用公式兜底）。
 *   · 路径 /sdcard/debug/shot_<序号>.bmp：mkdir 层级；序号 = 既有最大 +1，
 *     保留最近 3 张（按序号最大=最新），写新图前先删最旧的超出部分。
 *   · sd_tf_is_flash_fallback()==true（TF 缺失回退出厂 Flash 分区）→ 拒绝：
 *     出厂素材分区仅 6MB，1MB/张 的取证写损耗不可接受。
 *   · 整程持合成互斥 rc_lock()：本函数虽约定渲染任务调用，但持锁后即使被
 *     误从其他任务调用也不会读到撕裂帧（与 flush/full_recompose 同一保证）。
 *     代价是截图 ~150ms 内增量 flush 被串行让路——调试功能可接受。 */

#define MP_SHOT_DIR        "/sdcard/debug"      /* sd_tf.h：挂载点恒为 /sdcard */
#define MP_SHOT_KEEP       3                    /* 保留最近 3 张 */
#define MP_SHOT_MAX_W      512                  /* render_init 校验屏宽上限 512 */

/* 一行 BGR 缓冲：最大屏宽 ×3B + 3B 对齐余量（static，不占运行时堆） */
static uint8_t s_shot_row[MP_SHOT_MAX_W * 3 + 3];

/* ══ 【UDP 帧倾倒 2026-10-01】远程取证第二通道（TF 截图要拔卡，远程看不见屏） ══
 * 把 g_fb（最终合成帧 RGB565）经 UDP 广播倾倒到局域网 :9999。Mac 侧一条命令收帧：
 *   nc -ulnw 9999 > /tmp/fb.raw   （先收 10B 头 "MPFB"+w+h+pkts，其后为像素包）
 * 协议：第 0 包 = "MPFB" + w(u16le) + h(u16le) + data_pkts(u16le)；其后每包 =
 * seq(u16le，从 0 起) + ≤1400B 像素（g_fb 行优先小端，480×480×2=460800B→330 包）。
 * 每 8 包歇 1 tick 防 lwip pbuf 枯竭；任何失败静默 return——取证通道绝不反噬
 * 渲染主流程。须持 rc_lock 调用（g_fb 读保护）。 */
static void frame_dump_udp_locked(void)
{
    const size_t total = (size_t)g_sw * g_sh * 2u;
    const int payload_max = 1400;
    const int data_pkts = (int)((total + payload_max - 1) / payload_max);

    uint8_t hdr[10];
    memcpy(hdr, "MPFB", 4);
    hdr[4] = (uint8_t)(g_sw & 0xff);      hdr[5] = (uint8_t)(g_sw >> 8);
    hdr[6] = (uint8_t)(g_sh & 0xff);      hdr[7] = (uint8_t)(g_sh >> 8);
    hdr[8] = (uint8_t)(data_pkts & 0xff); hdr[9] = (uint8_t)(data_pkts >> 8);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;
    int br = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &br, sizeof(br));
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons(9999);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    if (sendto(sock, hdr, sizeof hdr, 0, (struct sockaddr *)&dst, sizeof dst) < 0) {
        close(sock);
        return;
    }
    const uint8_t *fb = (const uint8_t *)g_fb;
    static uint8_t pkt[2 + 1400];          /* 1.4KB 在 bss，不占运行时堆 */
    int sent = 0;
    for (int i = 0; i < data_pkts; i++) {
        size_t off = (size_t)i * payload_max;
        size_t len = total - off;
        if (len > (size_t)payload_max) len = payload_max;
        pkt[0] = (uint8_t)(i & 0xff);
        pkt[1] = (uint8_t)((i >> 8) & 0xff);
        memcpy(pkt + 2, fb + off, len);
        /* 失败退避重试：lwip pbuf 瞬时枯竭（真机：每 8 包歇 1 tick 仍 330 包只
         * 成 100）。每 2 包歇 1 tick + 单包最多 3 试（2/4ms），最坏 ~3.5s。 */
        for (int t = 0; t < 3; t++) {
            if (sendto(sock, pkt, 2 + (int)len, 0,
                       (struct sockaddr *)&dst, sizeof dst) >= 0) { sent++; break; }
            vTaskDelay(pdMS_TO_TICKS(2 + 2 * t));
        }
        if ((i & 1) == 1) vTaskDelay(1);
    }
    close(sock);
    ESP_LOGI(TAG, "📡 UDP 帧倾倒：1+%d 包（成 %d）→ 广播:9999", data_pkts, sent);
}

/* 远程取证入口（public）：不依赖 TF，仅倾倒当前合成帧。 */
void render_frame_dump_udp(void)
{
    if (!g_inited || !g_fb) return;
    rc_lock();
    frame_dump_udp_locked();
    rc_unlock();
}

/* BMP 头小端字段写入（FATFS 上直接按字节序写，不做结构体对齐假设） */
static void shot_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}
static void shot_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

int render_screenshot_to_tf(void)
{
    if (!g_inited || !g_fb) {
        ESP_LOGE(TAG, "截图拒绝：渲染层未初始化");
        return RENDER_ERR_STATE;
    }
    /* 出厂 Flash 素材分区模式（TF 缺失/挂载失败的回退）：6MB 分区经不起
     * 每张 ~675KB 的取证写损耗（素材区写满会破坏出厂渲染能力）→ 拒绝 */
    if (sd_tf_is_flash_fallback()) {
        ESP_LOGW(TAG, "截图拒绝：当前为出厂 Flash 分区模式（无 TF 卡），禁止写入");
        return RENDER_ERR_UNSUPPORTED;
    }

    /* 行跨度 4 字节对齐（BMP 规范）；480×3=1440 天然对齐，pad=0 */
    const size_t row_bytes = (size_t)g_sw * 3u;
    const size_t stride    = (row_bytes + 3u) & ~(size_t)3u;
    const size_t pad_bytes = stride - row_bytes;
    const uint32_t img_size = (uint32_t)stride * (uint32_t)g_sh;

    int64_t t0 = esp_timer_get_time();
    rc_lock();                             /* 截图期间整屏 flush 串行让路（见函数头注释） */

    /* 1) 建目录（/sdcard 根已由 sd_mount 挂载；debug 层忽略已存在错误） */
    mkdir(MP_SHOT_DIR, 0775);

    /* 2) 扫描既有 shot_<n>.bmp：求最大序号 + 删最旧（保留最近 MP_SHOT_KEEP 张，
     *    本次新图计入后仍为 KEEP 张 → 写之前留 KEEP-1 张） */
    int idxs[MP_SHOT_KEEP + 8];            /* 实际有效个数 ≤ KEEP；容量留余量防竞态 */
    int n = 0, max_idx = 0;
    DIR *d = opendir(MP_SHOT_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            unsigned v;
            if (sscanf(e->d_name, "shot_%u.bmp", &v) != 1) continue;
            if ((int)v > max_idx) max_idx = (int)v;
            if (n < (int)(sizeof idxs / sizeof idxs[0])) idxs[n++] = (int)v;
        }
        closedir(d);
    }
    while (n > MP_SHOT_KEEP - 1) {         /* 删到只剩 KEEP-1 张（新图写入后共 KEEP 张） */
        int mi = 0;
        for (int i = 1; i < n; i++) if (idxs[i] < idxs[mi]) mi = i;
        char old[64];
        snprintf(old, sizeof(old), MP_SHOT_DIR "/shot_%03d.bmp", idxs[mi]);
        unlink(old);
        ESP_LOGI(TAG, "截图轮替：删除最旧 %s", old);
        idxs[mi] = idxs[--n];
    }

    /* 3) 流式写 BMP：54B 头 + 自下而上逐行 RGB565→BGR888 */
    char path[48];
    snprintf(path, sizeof(path), MP_SHOT_DIR "/shot_%03d.bmp", max_idx + 1);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        ESP_LOGE(TAG, "截图失败：open %s errno=%d", path, errno);
        rc_unlock();
        return MPAK_ERR_IO;
    }

    uint8_t hdr[54] = { 0 };
    hdr[0] = 'B'; hdr[1] = 'M';                    /* bfType */
    shot_put_u32(hdr + 2,  54u + img_size);        /* bfSize    = 头 + 像素数据 */
    shot_put_u32(hdr + 10, 54u);                   /* bfOffBits = 像素紧跟头 */
    shot_put_u32(hdr + 14, 40u);                   /* biSize    = BITMAPINFOHEADER */
    shot_put_u32(hdr + 18, (uint32_t)g_sw);        /* biWidth */
    shot_put_u32(hdr + 22, (uint32_t)g_sh);        /* biHeight>0 = 自下而上 */
    shot_put_u16(hdr + 26, 1u);                    /* biPlanes */
    shot_put_u16(hdr + 28, 24u);                   /* biBitCount = 24 位 BGR */
    shot_put_u32(hdr + 30, 0u);                    /* biCompression = BI_RGB */
    shot_put_u32(hdr + 34, img_size);              /* biSizeImage */
    /* bfReserved1/2、biXPelsPerMeter/biYPelsPerMeter/biClrUsed/biClrImportant 恒 0 */

    bool io_err = false;
    if (write(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) io_err = true;
    memset(s_shot_row + row_bytes, 0, pad_bytes);  /* 行尾对齐填充字节（480 宽时恒 0 字节） */
    /* BMP 行序 = 自下而上：从最后一屏行写到第 0 行 */
    for (int32_t y = g_sh - 1; y >= 0 && !io_err; y--) {
        const uint16_t *src = g_fb + (size_t)y * g_sw;
        uint8_t *dst = s_shot_row;
        for (int32_t x = 0; x < g_sw; x++) {
            uint16_t v = src[x];
            /* RGB565 → 8bit 各通道（与文件内取证 ASCII 探针同口径的左移展开），
             * BMP 像素序 = B,G,R（小端位图即字节序 BGR） */
            dst[0] = (uint8_t)((v & 0x1Fu) << 3);          /* B */
            dst[1] = (uint8_t)(((v >> 5) & 0x3Fu) << 2);   /* G */
            dst[2] = (uint8_t)(((v >> 11) & 0x1Fu) << 3);  /* R */
            dst += 3;
        }
        if (write(fd, s_shot_row, stride) != (ssize_t)stride) io_err = true;
    }
    close(fd);
    frame_dump_udp_locked();               /* TF 截图成功 → 顺手 UDP 倾倒一份（同锁） */
    rc_unlock();

    if (io_err) {
        ESP_LOGE(TAG, "截图失败：write %s 中断（errno=%d）→ 删除不完整文件",
                 path, errno);
        unlink(path);                      /* 不完整文件不留作"最近 3 张"之一 */
        return MPAK_ERR_IO;
    }

    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "📸 截图已存 %s 用时%lldms", path, (long long)ms);
    return RENDER_OK;
}


/*
 * ================= PSRAM 预算（480×480，软件设计 4.2 口径） =================
 * RENDER_PSRAM_BUDGET:
 *   framebuffer            460,800 B   合成器独占（单一所有权）
 *   static_back            460,800 B   屏幕尺寸（装载时按需 2x 展开）
 *   tile_layer             460,800 B   屏幕尺寸
 *   tile_mask               28,800 B   (480*480+7)/8
 *   实体缓冲 480×440×2     422,400 B   摆放上限（画布 240×220 世界 @2x，底部留 40px）
 *   实体覆盖 1bit           26,400 B
 *   LVGL MENU 整屏         460,800 B   render_mode_menu 离屏（DIRECT）
 *   LVGL POKER 双缓冲       92,160 B   2 × 1/10 屏（46,080 B each）
 *   气泡位图 460×160×2      147,200 B   显示时分配，隐藏即释放
 *   条带图（每条）           ~70 KB    480×64×2.25B（按条带实际尺寸）
 *   部件缓存（懒加载）       ≤2 MB cap 典型 ~700KB（整装扮+25 表情变体）
 *   时钟 13 小图              ~24 KB
 *   字形缓存 3×16 槽          ~40 KB   16/24/32px 各 max_glyph×16
 *   LAYOUT/PARTS 索引        ~120 KB   头+索引常驻
 *   ------------------------------------------------------------------
 *   固定峰值（无气泡）     ≈ 2.41 MB；典型全负载（含缓存/条带）≈ 3.4-4.0 MB
 *   （8MB PSRAM，与网络/音频共享；menu 与 poker 的 LVGL 缓冲常驻不切换释放）
 */
