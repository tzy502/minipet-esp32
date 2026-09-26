/*
 * lvgl_bridge.c — LVGL 合流桥实现
 *
 * 注意（LVGL 9.x API 假设点，联调时核对）：
 *   - flush：void (*)(lv_display_t*, const lv_area_t*, uint8_t*)
 *   - flush 末尾必须 lv_display_flush_ready(disp)（同步 flush 契约；
 *     缺失 = 下一次带失效区的刷新在 wait_for_flushing 死循环 → 卡死喂狗
 *     → watchdog.c 软件看门狗 esp_restart，表现为进菜单一帧后整机重启）
 *   - lv_font_get_glyph_bitmap_cb_t：(font, letter, glyph_dsc) 三参
 *   - lv_font_glyph_dsc_t 含 resolved_font 字段
 */
#include "lvgl_bridge.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "font_lazy.h"
#include "render.h" /* RENDER_* 错误码 */

/* 菜单真实化（E7）跨模块消费面（只读调用，不改这些模块）：
 *   app_core.h      mp_post_cmd / mp_post_audio 三队列 + 指令/音频消息枚举 + MP_ACTION_*
 *   asset_dl.h      本地清单查询（当前地图/PARTS 路径，MP_MPK_PATH_MAX）
 *   bgm.h           BGM 状态回显（bgm_get_state/get_source）
 *   state_machine.h 菜单 Exit 行经 MP_SM_EV_MENU_KEY 走既有状态机通道收菜单 */
#include "app_core.h"
#include "asset_dl.h"
#include "bgm.h"
#include "state_machine.h"

static const char *TAG = "bridge";

static void touch_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

#define BR_BUBBLE_PAD    8
#define BR_BUBBLE_BORDER 2
#define BR_BUBBLE_TEXT_MAX 384   /* 显示串缓冲（含插入的 \n） */

static struct {
    lv_display_t *disp;
    int32_t sw, sh;

    uint8_t *poker_buf[2];       /* 各 1/10 屏（PARTIAL 双缓冲） */
    uint32_t poker_buf_sz;
    uint8_t *menu_buf;           /* 整屏（DIRECT 单缓冲） */

    bool menu_mode;

    /* MENU 脏区 bbox（tick 内累积、take 清零；全在渲染任务） */
    bool     md_valid;
    int32_t  md_x1, md_y1, md_x2, md_y2;

    /* 气泡捕获 */
    struct {
        bool      active;
        uint16_t *dst;
        int32_t   stride, w, h;
    } cap;
} s_br;

/* ---------------- 菜单选择器状态（E7；函数体在下方 menu 段） ---------------- */

typedef enum {
    MENU_PAGE_ROOT = 0,
    MENU_PAGE_MAPS,
    MENU_PAGE_PAPERDOLL,
    MENU_PAGE_BGM,
} menu_page_t;

#define MENU_ROWS_MAX   8       /* 单页可选行上限（含 Back/Exit 行） */
#define MENU_MAPS_MAX   3       /* Maps 页真实条目位（当前 + 2 示例） */

typedef struct {
    char label[32];             /* 列表显示串（ASCII，Montserrat 可渲染） */
    char hash[24];              /* MP_CMD_SET_MAP 的 s（16 hex / 示例名） */
} map_item_t;

typedef struct {
    menu_page_t page;
    int      row_cnt;           /* 当前页可选行数 */
    int      sel;               /* 选中行（input 任务单字写，渲染任务读） */
    int      sel_applied;       /* 已贴高亮的行号（变化才重贴，防 10Hz 失效） */
    lv_obj_t *rows[MENU_ROWS_MAX];

    map_item_t maps[MENU_MAPS_MAX];
    int        map_cnt;

    lv_obj_t *status_label;     /* BGM 页状态行（tick 500ms 刷新） */

    const lv_font_t *f_title, *f_item, *f_small;

    /* 跨任务请求旗标（input 任务置位 / 菜单 tick 在渲染任务排空） */
    volatile bool req_ok;
    volatile bool req_exit;
    volatile bool req_rebuild;          /* 仅渲染任务写：点击/tick 换页统一延后 */
    menu_page_t   pend_page;
    int           pend_sel;

    /* 触摸喂入：input 任务写，渲染任务 indev 读。两任务同钉 APP 核
     * （main.c xTaskCreatePinnedToCore(...,1)），单写者单读者 + 先坐标后
     * pressed 的写序，volatile 单字读写即安全（同 render_input_tilt 约定） */
    volatile int32_t t_x, t_y;
    volatile bool    t_pressed;

    lv_indev_t *indev;                  /* 唯一 pointer indev（bridge_init 建） */
    lv_timer_t *tick;                   /* 菜单态 100ms 节拍（进菜单建/出菜单删） */
    int         bgm_refr_div;           /* 状态行 500ms 分频计数 */
} menu_ui_t;

static menu_ui_t s_menu;

/* ---------------- flush ---------------- */

static void bridge_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* 本 flush 为同步完成（像素已落在 LVGL 缓冲/或仅记录 bbox，无 DMA 在途）。
     * LVGL 9 契约：flush_cb 必须调用 lv_display_flush_ready() 终止本次 flush，
     * 否则 disp->flushing 恒为 1，下一次「带失效区」的刷新会在
     * wait_for_flushing() 的 while(disp->flushing) 处死循环（lv_refr.c:1500）
     * → render 任务卡死不再喂狗 → watchdog.c 软件看门狗 esp_restart()
     * （真机症状：菜单亮一帧后 ~5s 整机重启）。
     * 在入口即清标志：对同步 flush 与出口清除等价（该标志只会在下一次刷新
     * 被检查），且任何 early-return 分支都不可能漏标。 */
    if (disp) lv_display_flush_ready(disp);

    if (!area) return;

    if (s_br.menu_mode) {
        /* MENU：记录 bbox，拷贝与上屏由合成器完成（单写屏者）。
         * 探针：每次进菜单的首个 flush（md_valid 仍未置位）打印一次区域，
         * 真机日志可确认「第 1 帧已渲染完成」。 */
        if (!s_br.md_valid) {
            ESP_LOGI(TAG, "flush[menu] first (%d,%d)-(%d,%d)",
                     area->x1, area->y1, area->x2, area->y2);
            s_br.md_x1 = area->x1; s_br.md_y1 = area->y1;
            s_br.md_x2 = area->x2; s_br.md_y2 = area->y2;
            s_br.md_valid = true;
        } else {
            if (area->x1 < s_br.md_x1) s_br.md_x1 = area->x1;
            if (area->y1 < s_br.md_y1) s_br.md_y1 = area->y1;
            if (area->x2 > s_br.md_x2) s_br.md_x2 = area->x2;
            if (area->y2 > s_br.md_y2) s_br.md_y2 = area->y2;
        }
        return;
    }

    /* POKER：无屏可写——气泡捕获模式则收切片 */
    if (s_br.cap.active && px_map) {
        int32_t cx1 = area->x1, cy1 = area->y1;
        int32_t cx2 = area->x2, cy2 = area->y2;
        if (cx1 < 0) cx1 = 0;
        if (cy1 < 0) cy1 = 0;
        if (cx2 > s_br.cap.w - 1) cx2 = s_br.cap.w - 1;
        if (cy2 > s_br.cap.h - 1) cy2 = s_br.cap.h - 1;
        if (cx1 <= cx2 && cy1 <= cy2) {
            int32_t area_w = area->x2 - area->x1 + 1;
            for (int32_t y = cy1; y <= cy2; y++) {
                const uint8_t *src = px_map +
                    (size_t)(y - area->y1) * area_w * 2u + (size_t)(cx1 - area->x1) * 2u;
                uint16_t *dst = s_br.cap.dst + (size_t)y * s_br.cap.stride + cx1;
                memcpy(dst, src, (size_t)(cx2 - cx1 + 1) * 2u);
            }
        }
    }
}

/* ---------------- 生命周期 / 模式 ---------------- */

int bridge_init(int32_t screen_w, int32_t screen_h)
{
    memset(&s_br, 0, sizeof s_br);
    memset(&s_menu, 0, sizeof s_menu);
    s_br.sw = screen_w;
    s_br.sh = screen_h;

    /* LVGL 核心初始化——必须在任何 lv_* 调用之前（缺失 = tlsf 空池崩溃） */
    lv_init();
    lv_tick_set_cb((lv_tick_get_cb_t)xTaskGetTickCount);

    s_br.disp = lv_display_create((uint32_t)screen_w, (uint32_t)screen_h);
    if (!s_br.disp) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return RENDER_ERR_NOMEM;
    }
    lv_display_set_flush_cb(s_br.disp, bridge_flush);
    lv_display_set_color_format(s_br.disp, LV_COLOR_FORMAT_RGB565);

    /* 触摸 indev（菜单真实化）：数据源 = input 任务经 lv_bridge_touch_feed
     * 喂入的最新帧（input_dispatch touch_read_frame 直读，本侧不做 I2C）。
     * 创建失败不阻断渲染（菜单退化为纯侧键操作） */
    s_menu.indev = lv_indev_create();
    if (s_menu.indev) {
        lv_indev_set_type(s_menu.indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_menu.indev, touch_indev_read_cb);
        lv_indev_set_display(s_menu.indev, s_br.disp);
        ESP_LOGI(TAG, "touch indev created (menu mode)");
    } else {
        ESP_LOGW(TAG, "lv_indev_create failed: menu touch disabled");
    }

    /* POKER：2 × 1/10 屏双缓冲 */
    s_br.poker_buf_sz = (uint32_t)screen_w * (uint32_t)(screen_h / 10) * 2u;
    s_br.poker_buf[0] = heap_caps_malloc(s_br.poker_buf_sz,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_br.poker_buf[1] = heap_caps_malloc(s_br.poker_buf_sz,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_br.poker_buf[0] || !s_br.poker_buf[1]) {
        ESP_LOGE(TAG, "poker bufs alloc failed (%u B each)", s_br.poker_buf_sz);
        return RENDER_ERR_NOMEM;
    }

    /* MENU：整屏离屏（450KB @480×480） */
    s_br.menu_buf = heap_caps_malloc((size_t)screen_w * screen_h * 2u,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_br.menu_buf) {
        ESP_LOGE(TAG, "menu buf alloc failed");
        return RENDER_ERR_NOMEM;
    }

    return bridge_mode_poker();
}

void bridge_deinit(void)
{
    if (s_menu.tick) { lv_timer_del(s_menu.tick); s_menu.tick = NULL; }
    if (s_menu.indev) { lv_indev_delete(s_menu.indev); s_menu.indev = NULL; }
    if (s_br.disp) lv_display_delete(s_br.disp);
    if (s_br.poker_buf[0]) heap_caps_free(s_br.poker_buf[0]);
    if (s_br.poker_buf[1]) heap_caps_free(s_br.poker_buf[1]);
    if (s_br.menu_buf) heap_caps_free(s_br.menu_buf);
    memset(&s_br, 0, sizeof s_br);
    memset(&s_menu, 0, sizeof s_menu);
}

int bridge_mode_poker(void)
{
    if (!lv_is_initialized() || !s_br.disp || !s_br.poker_buf[0]) {
        ESP_LOGE(TAG, "mode_poker: not ready (lv=%d disp=%p)",
                 lv_is_initialized(), (void *)s_br.disp);
        return RENDER_ERR_STATE;
    }
    if (s_br.menu_mode) ESP_LOGI(TAG, "mode_poker: exit menu -> PARTIAL");
    /* 菜单资源收尾：节拍定时器删除；indev 复位并断掉残留按下态
     * （抬指前退出菜单时防止 indev 把 PRESSED 带进 POKER 态误触） */
    if (s_menu.tick) { lv_timer_del(s_menu.tick); s_menu.tick = NULL; }
    s_menu.t_pressed = false;
    s_menu.req_ok = false;
    s_menu.req_exit = false;
    s_menu.req_rebuild = false;
    if (s_menu.indev) lv_indev_reset(s_menu.indev, NULL);
    lv_display_set_buffers(s_br.disp, s_br.poker_buf[0], s_br.poker_buf[1],
                           s_br.poker_buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    s_br.menu_mode = false;
    s_br.md_valid = false;
    return RENDER_OK;
}

/* ---------------- MENU 真实选择器（E7 菜单真实化） ----------------
 * 旧实现只有黑底占位文字。现为真实选择器：
 *   主菜单：Maps / Paperdoll / BGM / Exit 四行（触摸点选 + 侧键矩阵）
 *   Maps     子页：当前缓存地图（asset_dl_map_path(NULL)）+ 2 个示例条目
 *            点选 → MP_CMD_SET_MAP（hash 通道，state_machine.dispatch_map 查
 *            路径+条带并切图）→ 回主菜单
 *   Paperdoll 子页：现有通道只有动作切换（MP_CMD_SET_ACTION）→ 列出五个真实
 *            动作；「列出 PARTS 条目 + 换装指令」缺失见汇报
 *   BGM      子页：状态行（曲目数/bgm_get_state/get_source）+ 播放暂停/
 *            上一首/下一首三按钮（bgm_toggle_pause/prev/next，audio_q 异步）
 *
 * 输入接线（跨任务）：
 *   触摸：input 任务菜单态调 lv_bridge_touch_feed(x,y,pressed)（读 I2C 帧后
 *         喂坐标）→ 本文件 pointer indev 在渲染任务 lv_timer_handler 里消费。
 *   侧键矩阵（短按）：顶键=确认 → render_menu_ok()；中键=上移/底键=下移 →
 *         render_menu_nav(0/1)。跨任务安全：nav 只写单字 sel（渲染任务 100ms
 *         tick 贴高亮），ok 置 req_ok 旗标由同一 tick 排空——绝不在 indev/
 *         外任务上下文直接动控件树；触摸点击虽在渲染任务 indev 上下文，
 *         换页/重建也统一经 req_rebuild 延到 tick 排空（防 indev 压着对象时
 *         lv_obj_clean 自删）。长按顶键（转时钟）归 input_dispatch，不在此处理。
 * 须与 render_tick 同任务构建（render_enter_menu → bridge_mode_menu）。 */

/* 字体：内置 Montserrat（sdkconfig.defaults 三行 + 主线程 regen 后生效）；
 * 未 regen 时 #if 短路 → 回退 TF FONT 包（16/24/32），再退 LVGL 默认主题字体 */
static void menu_fonts_refresh(void)
{
    s_menu.f_title = NULL;
    s_menu.f_item  = NULL;
    s_menu.f_small = NULL;
#if LV_FONT_MONTSERRAT_28
    s_menu.f_title = &lv_font_montserrat_28;
#endif
#if LV_FONT_MONTSERRAT_20
    s_menu.f_item = &lv_font_montserrat_20;
#elif LV_FONT_MONTSERRAT_16
    s_menu.f_item = &lv_font_montserrat_16;
#endif
#if LV_FONT_MONTSERRAT_16
    s_menu.f_small = &lv_font_montserrat_16;
#endif
    if (!s_menu.f_title) s_menu.f_title = font_lazy_get(FONT_ID_32);
    if (!s_menu.f_item)  s_menu.f_item  = font_lazy_get(FONT_ID_24);
    if (!s_menu.f_small) s_menu.f_small = font_lazy_get(FONT_ID_16);
}

/* 触摸 indev 读回调（渲染任务，lv_timer_handler 驱动 ~30ms） */
static void touch_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    /* POKER/气泡态恒报释放：菜单外触摸归 input_dispatch 宠物交互，LVGL
     * 不消费（默认屏无可点控件）；残留按下在进/出菜单时由 reset 清掉 */
    if (!s_br.menu_mode || !s_menu.t_pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->point.x = s_menu.t_x;
    data->point.y = s_menu.t_y;
    data->state   = LV_INDEV_STATE_PRESSED;
}

void lv_bridge_touch_feed(int x, int y, bool pressed)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > s_br.sw - 1) x = s_br.sw - 1;
    if (y > s_br.sh - 1) y = s_br.sh - 1;
    /* 写序：先坐标后 pressed——读侧见 pressed=true 时坐标必已有效 */
    s_menu.t_x = x;
    s_menu.t_y = y;
    s_menu.t_pressed = pressed;
}

/* ---------------- 侧键矩阵钩子（input 任务调用） ---------------- */

/* 侧键导航：dir=0 上移（中键）/ 1 下移（底键）。根页移动选中项，子页移动
 * 列表高亮，越界回绕。只写单字 sel，高亮由菜单 tick 统一贴（跨任务安全） */
void render_menu_nav(int dir)
{
    if (!s_br.menu_mode || s_menu.row_cnt <= 0) return;
    if (dir) s_menu.sel = (s_menu.sel + 1) % s_menu.row_cnt;
    else     s_menu.sel = (s_menu.sel + s_menu.row_cnt - 1) % s_menu.row_cnt;
}

/* 顶键短按=确认/进入：置请求旗标，菜单 tick 在渲染任务排空
 * （根页进入选中子页/Exit；Maps/Paperdoll 执行选中条目并回根页；
 *  BGM 页执行按钮动作；Back 行回根页）。非菜单态返回不动作 */
void render_menu_ok(void)
{
    if (!s_br.menu_mode) return;
    s_menu.req_ok = true;
}

/* 菜单内部收起（Exit 行 / 兼容单键入口）：复用实体键同一状态机事件——
 * MENU→POKER 迁移先出菜单态再经 cmd_q 调 render_exit_menu，状态与渲染
 * 严格同步。直接 post MP_CMD_MENU_EXIT 会让状态机滞留 MENU 态（input
 * 触摸从此停读 → 死菜单），禁止 */
static void menu_request_exit(void)
{
    ESP_LOGI(TAG, "menu: exit (state machine MENU_KEY path)");
    state_machine_handle(MP_SM_EV_MENU_KEY);
}

/* ---------------- 数据收集（Maps 数据面） ---------------- */

/* /sdcard/minipet/bg/<hash>.mpk → <hash>（MP_CMD_SET_MAP 通道按 hash 派发） */
static void menu_hash_from_path(const char *path, char *out, size_t cap)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
}

/* 本地缓存地图清单。数据面现状：asset_dl 只有「当前地图」单条查询
 * （asset_dl_map_path(NULL)），无「列出全部 BGMAP 条目」接口 →
 * 当前地图 1 条真实条目 + 2 个写死示例条目兜底演示（示例 hash 未缓存时
 * dispatch_map 内 asset_dl_map_path 查不到自动短路，无副作用）。 */
static void menu_maps_collect(void)
{
    char path[MP_MPK_PATH_MAX];
    s_menu.map_cnt = 0;

    if (asset_dl_map_path(NULL, path, sizeof(path))) {
        map_item_t *it = &s_menu.maps[s_menu.map_cnt++];
        menu_hash_from_path(path, it->hash, sizeof(it->hash));
        char nowlbl[16];
        snprintf(nowlbl, sizeof(nowlbl), "Now %.8s", it->hash);
        strlcpy(it->label, nowlbl, sizeof(it->label));   /* 经临时缓冲避 -Wrestrict 误报 */
    }
    static const char * const demo[2] = { "Demo map A", "Demo map B" };
    for (int i = 0; i < 2 && s_menu.map_cnt < MENU_MAPS_MAX; i++) {
        map_item_t *it = &s_menu.maps[s_menu.map_cnt++];
        strlcpy(it->label, demo[i], sizeof(it->label));
        snprintf(it->hash, sizeof(it->hash), "demo_map_%c", (char)('a' + i));
    }
}

/* ---------------- Paperdoll / BGM 动作表 ---------------- */

/* 现有通道只有 MP_CMD_SET_ACTION（s=动作名 → render_set_layout）；
 * 真换装（PARTS 列表 + MP_CMD_SET_PARTS）缺失见汇报 */
static const struct { const char *label, *action; } PD_ITEMS[] = {
    { "Stand", MP_ACTION_STAND },
    { "Walk",  MP_ACTION_WALK  },
    { "Fly",   MP_ACTION_FLY   },
    { "Alert", MP_ACTION_ALERT },
    { "Hit",   MP_ACTION_HIT   },
};
#define PD_CNT ((int)(sizeof(PD_ITEMS) / sizeof(PD_ITEMS[0])))

static void menu_bgm_post(mp_audio_msg_type_t type, int32_t a)
{
    mp_audio_msg_t m = { .type = type, .a = a };
    mp_post_audio(&m);          /* audio_q → bgm 任务（E8 控制权在设备） */
}

/* 播放/暂停 + 起播：bgm.h 真实控制接口（本地曲目表优先，自动回退服务端） */
static void menu_bgm_toggle(void)
{
    mp_bgm_state_t st = bgm_get_state();
    if (st == MP_BGM_PLAYING || st == MP_BGM_PAUSED) {
        bgm_toggle_pause();          /* 播放↔暂停（停 feeder+PA 静音同口径） */
        return;
    }
    /* IDLE/FAILED → 起播：本地曲目表（AUDIO_META 包）有曲则选首曲；
     * 表不可用回退 MP_AUDIO_PLAY(a=0) 服务端定曲 */
    uint32_t id = 0;
    if (bgm_list(&id, NULL, 1) > 0) {
        bgm_play_id(id);
    } else {
        menu_bgm_post(MP_AUDIO_PLAY, 0);
    }
}

static void menu_bgm_status_refresh(void)
{
    if (!s_menu.status_label) return;
    static const char *const st_name[] = { "Idle", "Playing", "Paused", "Failed" };
    mp_bgm_state_t st = bgm_get_state();
    mp_bgm_source_t src = bgm_get_source();
    int tracks = bgm_list(NULL, NULL, INT_MAX);   /* 曲目表容量查询（0=无本地表） */
    /* 「当前曲目名」查询接口缺失（bgm 无 get_current，见汇报）→ 只回显
     * 本地表曲目数 + 播放态 + 音源，三个数据点全部真实 */
    lv_label_set_text_fmt(s_menu.status_label, "Tracks:%d  State: %s  Src: %s",
                          tracks,
                          (st >= MP_BGM_IDLE && st <= MP_BGM_FAILED) ? st_name[st] : "?",
                          (src == MP_BGM_SRC_QQ) ? "QQ" : "WZ");
}

/* ---------------- 行为分发 ---------------- */

static void menu_goto(menu_page_t page)
{
    s_menu.pend_page  = page;
    s_menu.pend_sel   = 0;
    s_menu.req_rebuild = true;      /* tick 排空重建（防 indev 上下文自删） */
}

static void menu_activate(int idx)
{
    if (idx < 0 || idx >= s_menu.row_cnt) return;

    switch (s_menu.page) {
    case MENU_PAGE_ROOT:
        if (idx == 3) { menu_request_exit(); break; }   /* Exit 行 */
        menu_goto((menu_page_t)(MENU_PAGE_MAPS + idx));
        break;

    case MENU_PAGE_MAPS:
        /* 点选 → SET_MAP（hash）→ 回主菜单；示例条目 hash 未缓存时
         * dispatch_map 短路，界面仍回根页（演示路径可见） */
        if (idx < s_menu.map_cnt) {
            mp_cmd_t c = { .type = MP_CMD_SET_MAP };
            strlcpy(c.s, s_menu.maps[idx].hash, sizeof(c.s));
            mp_post_cmd(&c);
        }
        menu_goto(MENU_PAGE_ROOT);
        break;

    case MENU_PAGE_PAPERDOLL:
        if (idx < PD_CNT) {
            mp_cmd_t c = { .type = MP_CMD_SET_ACTION };
            strlcpy(c.s, PD_ITEMS[idx].action, sizeof(c.s));
            mp_post_cmd(&c);
        }
        menu_goto(MENU_PAGE_ROOT);
        break;

    case MENU_PAGE_BGM:
        if      (idx == 0) menu_bgm_toggle();
        else if (idx == 1) bgm_prev();                  /* 本地曲目表循环，表空回退服务端 */
        else if (idx == 2) bgm_next();
        else if (idx == 3) menu_goto(MENU_PAGE_ROOT);   /* Back 行 */
        break;
    }
}

static void row_click_cb(lv_event_t *e)
{
    menu_activate((int)(intptr_t)lv_event_get_user_data(e));
}

/* ---------------- 控件构建 ---------------- */

static void menu_style_row(lv_obj_t *btn, bool selected)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? 0x1E2A3A : 0x121216), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(selected ? 0x4DA3FF : 0x34343C), 0);
}

static lv_obj_t *menu_add_row(lv_obj_t *parent, int idx, const char *text,
                              int32_t y, int32_t h)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, 48, y);
    lv_obj_set_size(btn, s_br.sw - 96, h);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    menu_style_row(btn, idx == s_menu.sel);

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_color(lb, lv_color_hex(0xFFFFFF), 0);
    if (s_menu.f_item) lv_obj_set_style_text_font(lb, s_menu.f_item, 0);
    lv_label_set_text(lb, text);
    lv_obj_center(lb);

    lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    if (idx < MENU_ROWS_MAX) s_menu.rows[idx] = btn;
    return btn;
}

static lv_obj_t *menu_add_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_obj_set_style_text_color(lb, lv_color_hex(0xFFFFFF), 0);
    if (s_menu.f_title) lv_obj_set_style_text_font(lb, s_menu.f_title, 0);
    lv_label_set_text(lb, text);
    lv_obj_align(lb, LV_ALIGN_TOP_MID, 0, 36);
    return lb;
}

static void menu_add_hint(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_obj_set_style_text_color(lb, lv_color_hex(0x8A8A94), 0);
    if (s_menu.f_small) lv_obj_set_style_text_font(lb, s_menu.f_small, 0);
    lv_label_set_text(lb, text);   /* ASCII：Montserrat 内置字体只含拉丁字形 */
    lv_obj_align(lb, LV_ALIGN_BOTTOM_MID, 0, -16);
}

/* 重建当前页控件树（只在渲染任务菜单 tick / bridge_mode_menu 里调用）。
 * 幂等：进/出菜单与换页多轮后默认屏会残留控件，重建前先清空 */
static void menu_rebuild(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr) {
        ESP_LOGE(TAG, "menu_rebuild: no active screen");
        return;
    }
    if (lv_obj_get_child_count(scr)) lv_obj_clean(scr);
    memset(s_menu.rows, 0, sizeof s_menu.rows);
    s_menu.status_label = NULL;
    s_menu.row_cnt = 0;
    s_menu.sel_applied = -1;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    menu_fonts_refresh();

    switch (s_menu.page) {
    case MENU_PAGE_ROOT: {
        static const char *const rows[4] = { "Maps", "Paperdoll", "BGM", "Exit" };
        menu_add_title(scr, "MiniPet");
        for (int i = 0; i < 4; i++)
            menu_add_row(scr, i, rows[i], 118 + i * 70, 56);
        s_menu.row_cnt = 4;
        menu_add_hint(scr, "UP:MID DOWN:BOT LONG:CLOCK");
        break;
    }
    case MENU_PAGE_MAPS: {
        menu_add_title(scr, "Maps");
        menu_maps_collect();
        for (int i = 0; i < s_menu.map_cnt; i++)
            menu_add_row(scr, i, s_menu.maps[i].label, 118 + i * 70, 56);
        menu_add_row(scr, s_menu.map_cnt, "< Back", 118 + s_menu.map_cnt * 70, 56);
        s_menu.row_cnt = s_menu.map_cnt + 1;
        menu_add_hint(scr, "TAP: PICK   TOP:OK");
        break;
    }
    case MENU_PAGE_PAPERDOLL: {
        menu_add_title(scr, "Paperdoll");
        for (int i = 0; i < PD_CNT; i++)
            menu_add_row(scr, i, PD_ITEMS[i].label, 104 + i * 56, 48);
        menu_add_row(scr, PD_CNT, "< Back", 104 + PD_CNT * 56, 48);
        s_menu.row_cnt = PD_CNT + 1;
        menu_add_hint(scr, "ACTION DEMO - OUTFIT LISTS NEED SYNC");
        break;
    }
    case MENU_PAGE_BGM: {
        menu_add_title(scr, "BGM");
        s_menu.status_label = lv_label_create(scr);
        lv_obj_set_style_text_color(s_menu.status_label, lv_color_hex(0xB9B9C4), 0);
        if (s_menu.f_small) lv_obj_set_style_text_font(s_menu.status_label, s_menu.f_small, 0);
        lv_label_set_text(s_menu.status_label, "Tracks:-");
        lv_obj_align(s_menu.status_label, LV_ALIGN_TOP_MID, 0, 96);
        menu_bgm_status_refresh();

        menu_add_row(scr, 0, "Play / Pause", 170, 54);
        menu_add_row(scr, 1, "Prev",          238, 54);
        menu_add_row(scr, 2, "Next",          306, 54);
        menu_add_row(scr, 3, "< Back",        374, 54);
        s_menu.row_cnt = 4;
        menu_add_hint(scr, "TOUCH OR TOP KEY");
        break;
    }
    }

    s_menu.sel_applied = s_menu.sel;   /* 构建时已按 sel 贴高亮 */
    ESP_LOGI(TAG, "menu_rebuild: page=%d rows=%d widgets=%u",
             (int)s_menu.page, s_menu.row_cnt,
             (unsigned)lv_obj_get_child_count(scr));
    lv_obj_invalidate(scr);            /* DIRECT 模式强制整屏重绘入 menu_buf */
}

/* 高亮跟随 sel（菜单 tick；sel 变化才重贴，避免无谓失效区） */
static void menu_apply_selection(void)
{
    if (s_menu.sel_applied == s_menu.sel) return;
    for (int i = 0; i < s_menu.row_cnt && i < MENU_ROWS_MAX; i++)
        if (s_menu.rows[i]) menu_style_row(s_menu.rows[i], i == s_menu.sel);
    s_menu.sel_applied = s_menu.sel;
}

/* 菜单态 100ms 节拍（渲染任务）：排空侧键/换页请求 → 贴高亮 → 500ms 刷 BGM 状态。
 * 所有会动控件树的操作都收敛到本回调（渲染任务、非 indev 上下文）执行 */
static void menu_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_br.menu_mode) return;

    if (s_menu.req_exit) {
        s_menu.req_exit = false;
        menu_request_exit();
        return;
    }
    if (s_menu.req_rebuild) {
        s_menu.req_rebuild = false;
        s_menu.page = s_menu.pend_page;
        s_menu.sel  = s_menu.pend_sel;
        menu_rebuild();
        return;
    }
    if (s_menu.req_ok) {
        s_menu.req_ok = false;
        menu_activate(s_menu.sel);
        return;
    }
    menu_apply_selection();
    if (++s_menu.bgm_refr_div >= 5) {   /* 100ms×5 = 500ms */
        s_menu.bgm_refr_div = 0;
        menu_bgm_status_refresh();
    }
}

int bridge_mode_menu(void)
{
    /* 前置验证：LVGL 未初始化/无 display/无整屏缓冲时绝不切换（防
     * 后续 lv_* 调用在空指针/LVGL 空池上炸机）——任何失败都原样返回，
     * render 层保持 POKER 态，不产生重启路径 */
    if (!lv_is_initialized() || !s_br.disp || !s_br.menu_buf) {
        ESP_LOGE(TAG, "mode_menu: not ready (lv=%d disp=%p buf=%p)",
                 lv_is_initialized(), (void *)s_br.disp, (void *)s_br.menu_buf);
        return RENDER_ERR_STATE;
    }
    /* 幂等：已在菜单态直接成功返回，不重复 set_buffers / 重建控件
     * （旧实现重复 enter 每次在默认屏上再叠 5 个控件） */
    if (s_br.menu_mode) {
        ESP_LOGW(TAG, "mode_menu: already in menu (idempotent no-op)");
        return RENDER_OK;
    }

    ESP_LOGI(TAG, "mode_menu: enter (%dx%d DIRECT buf=%u B)",
             (int)s_br.sw, (int)s_br.sh,
             (unsigned)((size_t)s_br.sw * s_br.sh * 2u));
    memset(s_br.menu_buf, 0, (size_t)s_br.sw * s_br.sh * 2u);
    lv_display_set_buffers(s_br.disp, s_br.menu_buf, NULL,
                           (uint32_t)s_br.sw * (uint32_t)s_br.sh * 2u,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    s_br.menu_mode = false;   /* 构建成功后才切菜单态：失败路径保持 POKER，绝不半切换 */
    s_br.md_valid = false;
    /* 选择器每次进入都回根页首行（真机口径：菜单是临时全屏窗口），
     * 并清掉上一轮残留的按下/请求态（触摸边沿进菜单时 t_pressed 已随
     * input 侧复位，这里再兜底一次） */
    s_menu.page = MENU_PAGE_ROOT;
    s_menu.sel = 0;
    s_menu.sel_applied = -1;
    s_menu.t_pressed = false;
    s_menu.req_ok = false;
    s_menu.req_exit = false;
    s_menu.req_rebuild = false;
    menu_rebuild();           /* E7：进入菜单即构建真实选择器（防白屏/黑屏） */
    if (!s_menu.tick) {
        s_menu.tick = lv_timer_create(menu_tick_cb, 100, NULL);  /* 高亮/请求/BGM 状态节拍 */
    }
    s_br.menu_mode = true;    /* 构建成功后才切换态，失败路径保持 POKER */
    ESP_LOGI(TAG, "mode_menu: built, first tick will render frame 1");
    return RENDER_OK;
}

bool bridge_is_menu(void) { return s_br.menu_mode; }

const uint16_t *bridge_menu_buf(void)
{
    return (const uint16_t *)s_br.menu_buf;
}

lv_display_t *bridge_display(void)
{
    return s_br.disp;
}

bool bridge_menu_take_dirty(int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    if (!s_br.menu_mode || !s_br.md_valid) return false;
    if (x) *x = s_br.md_x1;
    if (y) *y = s_br.md_y1;
    if (w) *w = s_br.md_x2 - s_br.md_x1 + 1;
    if (h) *h = s_br.md_y2 - s_br.md_y1 + 1;
    s_br.md_valid = false;
    return true;
}

/* ---------------- 气泡离屏渲染 ---------------- */

/* UTF-8 解码：返回消耗字节数（*cp 出码点）；串尾返回 0；非法返回 -1 */
static int utf8_step(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!*p) return 0;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) |
              ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    return -1;
}

/*
 * 排版：贪心换行（空格处优先断行，行满紧急断行——CJK 风格）。
 * 产出含显式 \n 的显示串；返回行数/最宽行宽；行数超 max_lines 截断。
 */
static int bubble_layout(const char *text, const lv_font_t *font,
                         int32_t max_text_w, int max_lines,
                         char *disp, size_t disp_cap,
                         int32_t *out_max_line_w, int *out_lines)
{
    uint32_t size_px = font->line_height ? (uint32_t)font->line_height : 16u;
    int32_t line_w = 0, max_line_w = 0;
    int lines = 1;
    size_t dl = 0;

    const char *s = text;
    while (*s) {
        uint32_t cp = 0xFFFDu;
        int step = utf8_step(s, &cp);
        if (step <= 0) { cp = 0xFFFDu; step = 1; }

        uint32_t adv = 0;
        lv_font_glyph_dsc_t dsc;
        if (lv_font_get_glyph_dsc(font, &dsc, cp, 0))   /* v9 签名：dsc 第 2 参 */
            adv = dsc.adv_w;
        else
            adv = size_px; /* 缺字按全宽占位（可见反馈） */

        bool brk = false;
        if (cp == (uint32_t)' ') {
            if (line_w + (int32_t)adv > max_text_w) brk = true; /* 断行并丢弃空格 */
        } else if (line_w + (int32_t)adv > max_text_w) {
            brk = true; /* 紧急断行 */
        }

        if (brk) {
            if (lines >= max_lines) break; /* 截断（丢弃剩余） */
            if (dl + 1 >= disp_cap) break;
            disp[dl++] = '\n';
            lines++;
            line_w = 0;
            if (cp == (uint32_t)' ') { s += step; continue; }
        }

        if (dl + (size_t)step >= disp_cap) break;
        memcpy(disp + dl, s, (size_t)step);
        dl += (size_t)step;
        line_w += (int32_t)adv;
        if (line_w > max_line_w) max_line_w = line_w;
        s += step;
    }
    disp[dl] = '\0';
    *out_max_line_w = max_line_w;
    *out_lines = lines;
    return 0;
}

int bridge_bubble_render(const char *text, int font_id,
                         uint16_t *dst, int32_t dst_stride,
                         int32_t dst_max_w, int32_t dst_max_h,
                         int32_t *out_w, int32_t *out_h)
{
    if (!text || !*text || !dst || !out_w || !out_h) return RENDER_ERR_ARG;
    if (!lv_is_initialized() || !s_br.disp) return RENDER_ERR_STATE;
    if (s_br.menu_mode) return RENDER_ERR_STATE;
    ESP_LOGI(TAG, "bubble_render: font=%d text_len=%u", font_id, (unsigned)strlen(text));

    const lv_font_t *font = font_lazy_get((font_id_t)font_id);
    if (!font) {
        ESP_LOGE(TAG, "font %d not loaded (render_set_font first)", font_id);
        return RENDER_ERR_STATE;
    }

    static char disp[BR_BUBBLE_TEXT_MAX + 8];
    int32_t inner_max_w = dst_max_w - 2 * (BR_BUBBLE_PAD + BR_BUBBLE_BORDER);
    int32_t inner_max_h = dst_max_h - 2 * (BR_BUBBLE_PAD + BR_BUBBLE_BORDER);
    uint32_t line_h = font->line_height ? (uint32_t)font->line_height : 16u;
    int max_lines = (int)(inner_max_h / line_h);
    if (inner_max_w < 8 || max_lines < 1) return RENDER_ERR_ARG;

    int32_t max_line_w;
    int lines;
    bubble_layout(text, font, inner_max_w, max_lines,
                  disp, sizeof disp, &max_line_w, &lines);

    int32_t bw = max_line_w + 2 * (BR_BUBBLE_PAD + BR_BUBBLE_BORDER);
    int32_t bh = (int32_t)lines * (int32_t)line_h + 2 * (BR_BUBBLE_PAD + BR_BUBBLE_BORDER);
    if (bw > dst_max_w || bh > dst_max_h) return RENDER_ERR_ARG;

    /* 离屏渲染：临时 screen + 气泡盒（不透明、直角），仅 invalidate 盒区域 */
    lv_obj_t *prev_scr = lv_screen_active();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_pos(box, 0, 0);
    lv_obj_set_size(box, (int32_t)bw, (int32_t)bh);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xF7F7F2), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, BR_BUBBLE_BORDER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x303030), 0);
    lv_obj_set_style_pad_all(box, BR_BUBBLE_PAD, 0);

    lv_obj_t *lbl = lv_label_create(box);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x101010), 0);
    lv_label_set_text(lbl, disp);
    lv_obj_set_width(lbl, inner_max_w);
    lv_obj_center(lbl);

    lv_screen_load(scr);
    s_br.cap.active = true;
    s_br.cap.dst    = dst;
    s_br.cap.stride = dst_stride;
    s_br.cap.w      = bw;
    s_br.cap.h      = bh;
    lv_obj_invalidate(box);
    lv_refr_now(NULL);           /* 同步渲染；partial 切片经 flush 捕获 */
    s_br.cap.active = false;

    lv_screen_load(prev_scr);
    lv_obj_delete(scr);

    *out_w = bw;
    *out_h = bh;
    ESP_LOGI(TAG, "bubble_render: done %dx%d", (int)bw, (int)bh);
    return RENDER_OK;
}
