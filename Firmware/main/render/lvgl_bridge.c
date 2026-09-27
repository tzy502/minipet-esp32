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
    MENU_PAGE_MAPS,          /* BGMAP 本地清单（asset_dl_bgmap_list） */
    MENU_PAGE_PAPERDOLL,     /* 换装 PARTS 清单（asset_dl_parts_list） */
    MENU_PAGE_ACTIONS,       /* 动作演示（MP_ACTION_*，真实指令通道） */
    MENU_PAGE_NPC,           /* Monsters：NPC 素材清单（asset_dl_npc_list，T2） */
    MENU_PAGE_BGM,
} menu_page_t;

#define MENU_ROWS_MAX   8       /* 单页可选行上限（含 Back/Exit 行） */
#define MENU_LIST_MAX   6       /* 列表页真实条目上限（+Back(+空态行) ≤ 8） */
#define MENU_COLLECT_MAX (MENU_LIST_MAX + 1)  /* 多收 1 条用于探测「还有更多」 */
#define MENU_DL_TIMEOUT_MS 30000  /* T4：单包下载轮询超时（tick 100ms 轮询落盘） */

/* 列表条目（四类列表页共用；数据全部来自 asset_dl 本地清单，无写死演示项） */
typedef struct {
    char label[32];             /* 显示串（asset_dl 已保证 ASCII；动作页为动作名） */
    char hash[20];              /* 资产 hash（BGMAP / PARTS / NPC 的 PARTS 包） */
    char entity[40];            /* NPC：entity（"npc:<id>"；含 LAYOUT 双包实体标识） */
    char action[16];            /* Actions 页：MP_ACTION_* */
    bool cached;                /* TF 上是否已有 <kind_dir>/<hash>.mpk */
} menu_item_t;

typedef struct {
    menu_page_t page;
    int      row_cnt;           /* 当前页可选行数 */
    int      sel;               /* 选中行（input 任务单字写，渲染任务读） */
    int      sel_applied;       /* 已贴高亮的行号（变化才重贴，防 10Hz 失效） */
    lv_obj_t *rows[MENU_ROWS_MAX];
    bool     row_enabled[MENU_ROWS_MAX];   /* T3：置灰行（离线未缓存）不派发 */

    menu_item_t items[MENU_LIST_MAX];
    int        item_cnt;        /* 列表页真实条目数（不含 Back/空态行） */
    int        back_idx;        /* Back 行号（列表页；-1 = 无） */

    lv_obj_t *status_label;     /* BGM 页状态行（tick 500ms 刷新） */
    lv_obj_t *hint_label;       /* 页脚提示行（构建时静态文本；T4 运行期改写） */

    /* T4：选中未缓存条目 → 请求单包下载 → tick 轮询落盘 → 成功再 post 指令 */
    bool          dl_active;
    char          dl_hash[20];
    mp_cmd_type_t dl_cmd;       /* 落盘后要发的指令（MP_CMD_NONE = 只下载） */
    int64_t       dl_deadline_ms;
    char          hint_once[40];/* 一次性提示（下载完成 → 重建后显示一格） */

    bool     offline;           /* 本页构建时的网络态（T3 置灰判据） */
    bool     offline_shown;     /* 本次构建时的网络态（置灰依据；tick 比对重建） */
    bool     truncated;         /* 清单条目超 MENU_LIST_MAX（页脚提示 " MORE"） */
    uint32_t rev_shown;         /* 本次构建时的 asset_dl 本地 rev（清单变化重建） */

    const lv_font_t *f_title, *f_item, *f_small;

    /* 跨任务请求旗标（input 任务置位 / 菜单 tick 在渲染任务排空） */
    volatile bool req_ok;
    volatile bool req_exit;
    volatile bool req_rebuild;          /* 仅渲染任务写：点击/tick 换页统一延后 */
    volatile bool req_act;              /* 触摸点击（indev 回调）→ 由 tick 排空派发 */
    volatile int  req_act_row;
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
    s_menu.req_act = false;
    s_menu.dl_active = false;         /* 出菜单不再轮询下载（文件仍会落盘，仅不派发） */
    if (s_menu.indev) lv_indev_reset(s_menu.indev, NULL);
    lv_display_set_buffers(s_br.disp, s_br.poker_buf[0], s_br.poker_buf[1],
                           s_br.poker_buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    s_br.menu_mode = false;
    s_br.md_valid = false;
    return RENDER_OK;
}

/* ---------------- MENU 真实选择器（E7 菜单真实化） ----------------
 * 旧实现只有黑底占位文字。现为真实选择器：
 *   主菜单：Maps / Paperdoll / Actions / Monsters / BGM / Exit 六行
 *           （触摸点选 + 侧键矩阵；Monsters = NPC 素材页，T2）
 *   Maps     子页：asset_dl_bgmap_list() 真实 BGMAP 条目（hash + ASCII label）
 *            [v]=已缓存 [ ]=未缓存（点选→按 hash 拉包→落盘后 MP_CMD_SET_MAP）
 *            [x]=未缓存且离线 → 置灰不可点（T3）
 *   Paperdoll 子页：asset_dl_parts_list() 真实 PARTS 装扮条目 → MP_CMD_SET_PARTS
 *            （hash 通道，state_machine.dispatch_set_parts_by_hash → render_set_parts）
 *   Actions  子页：五个真实动作（MP_CMD_SET_ACTION），[v] 由
 *            asset_dl_layout_cached() 标注该动作布局是否已在 TF
 *   Monsters 子页：asset_dl_npc_list() 真实 NPC 条目（selector=="npc"）。
 *            固件渲染层只有单实体（纸娃娃）通道，无 NPC 实体渲染接口 →
 *            本页只列清单/下载（不 post 渲染指令，避免拿 NPC PARTS 去套
 *            纸娃娃布局渲染出乱码）。空清单显示明确空态。
 *   BGM      子页：状态行（曲目数/bgm_get_state/get_source）+ 播放暂停/
 *            上一首/下一首三按钮（bgm_toggle_pause/prev/next，audio_q 异步）
 *
 * 网络态：state_machine_offline_mode() 每 500ms 轮询（state_machine.c 明示
 * 「查询型状态由渲染层轮询」）；离线且未缓存的行 LV_STATE_DISABLED 置灰。
 *
 * 输入接线（跨任务）：
 *   触摸：input 任务菜单态调 lv_bridge_touch_feed(x,y,pressed)（读 I2C 帧后
 *         喂坐标）→ 本文件 pointer indev 在渲染任务 lv_timer_handler 里消费。
 *   侧键矩阵（短按）：顶键=确认 → render_menu_ok()；中键=上移/底键=下移 →
 *         render_menu_nav(0/1)。跨任务安全：nav 只写单字 sel（渲染任务 100ms
 *         tick 贴高亮），ok 置 req_ok 旗标由同一 tick 排空——绝不在 indev/
 *         外任务上下文直接动控件树；触摸点击（row_click_cb）也只落
 *         req_act/req_act_row 旗标，动作统一由 tick 排空（防 indev 压着对象时
 *         lv_obj_clean 自删）。
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
    if (!s_br.menu_mode || s_menu.row_cnt <= 0) {
        ESP_LOGW("menu", "nav 丢弃：menu_mode=%d row_cnt=%d（state≠MENU 或菜单未建）",
                 (int)s_br.menu_mode, s_menu.row_cnt);
        return;
    }
    int old = s_menu.sel;
    if (dir) s_menu.sel = (s_menu.sel + 1) % s_menu.row_cnt;
    else     s_menu.sel = (s_menu.sel + s_menu.row_cnt - 1) % s_menu.row_cnt;
    ESP_LOGI("menu", "nav(%d) sel %d→%d/%d（100ms 内贴高亮）", dir, old, s_menu.sel, s_menu.row_cnt);
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

/* ---------------- 数据收集（真实数据面：asset_dl 本地清单） ----------------
 * 全部条目来自 asset_dl_*_list()（manifest 登记 + TF access 缓存标记），
 * 无写死演示项；asset_dl 侧已在 s_lock 内取快照（可从渲染任务调用）。 */

/* 已缓存条目排前（组内保持清单顺序）——离线时可用项置顶，减少误点置灰行 */
static void menu_items_cached_first(void)
{
    menu_item_t tmp[MENU_LIST_MAX];
    int n = 0;
    for (int i = 0; i < s_menu.item_cnt; i++)
        if (s_menu.items[i].cached) tmp[n++] = s_menu.items[i];
    for (int i = 0; i < s_menu.item_cnt; i++)
        if (!s_menu.items[i].cached) tmp[n++] = s_menu.items[i];
    memcpy(s_menu.items, tmp, sizeof(menu_item_t) * (size_t)n);
}

/* Maps 页：本地清单里的 BGMAP 条目（hash + ASCII label + 缓存标记） */
static void menu_maps_collect(void)
{
    char hashes[MENU_COLLECT_MAX][20] = { { 0 } };
    char labels[MENU_COLLECT_MAX][32] = { { 0 } };
    bool cached[MENU_COLLECT_MAX] = { false };
    int n = asset_dl_bgmap_list(hashes, labels, cached, MENU_COLLECT_MAX);
    if (n < 0) n = 0;
    s_menu.truncated = (n > MENU_LIST_MAX);
    if (n > MENU_LIST_MAX) n = MENU_LIST_MAX;
    s_menu.item_cnt = n;
    for (int i = 0; i < n; i++) {
        strlcpy(s_menu.items[i].hash, hashes[i], sizeof(s_menu.items[i].hash));
        strlcpy(s_menu.items[i].label, labels[i], sizeof(s_menu.items[i].label));
        s_menu.items[i].entity[0] = 0;
        s_menu.items[i].action[0] = 0;
        s_menu.items[i].cached = cached[i];
    }
    menu_items_cached_first();
}

/* Paperdoll 页：本地清单里的 PARTS 装扮条目 → MP_CMD_SET_PARTS（换装） */
static void menu_parts_collect(void)
{
    char hashes[MENU_COLLECT_MAX][20] = { { 0 } };
    char labels[MENU_COLLECT_MAX][32] = { { 0 } };
    bool cached[MENU_COLLECT_MAX] = { false };
    int n = asset_dl_parts_list(hashes, labels, cached, MENU_COLLECT_MAX);
    if (n < 0) n = 0;
    s_menu.truncated = (n > MENU_LIST_MAX);
    if (n > MENU_LIST_MAX) n = MENU_LIST_MAX;
    s_menu.item_cnt = n;
    for (int i = 0; i < n; i++) {
        strlcpy(s_menu.items[i].hash, hashes[i], sizeof(s_menu.items[i].hash));
        strlcpy(s_menu.items[i].label, labels[i], sizeof(s_menu.items[i].label));
        s_menu.items[i].entity[0] = 0;
        s_menu.items[i].action[0] = 0;
        s_menu.items[i].cached = cached[i];
    }
    menu_items_cached_first();
}

/* Monsters(NPC) 页：selector=="npc" 的实体（entity 去重；服务端 1 PARTS + N LAYOUT）。
 * 固件无 NPC 实体渲染通道 → 本页只列/只下（见页头注释）。 */
static void menu_npc_collect(void)
{
    char entities[MENU_COLLECT_MAX][40] = { { 0 } };
    char hashes[MENU_COLLECT_MAX][20] = { { 0 } };
    char labels[MENU_COLLECT_MAX][32] = { { 0 } };
    bool cached[MENU_COLLECT_MAX] = { false };
    int n = asset_dl_npc_list(entities, hashes, labels, cached, MENU_COLLECT_MAX);
    if (n < 0) n = 0;
    s_menu.truncated = (n > MENU_LIST_MAX);
    if (n > MENU_LIST_MAX) n = MENU_LIST_MAX;
    s_menu.item_cnt = n;
    for (int i = 0; i < n; i++) {
        strlcpy(s_menu.items[i].hash, hashes[i], sizeof(s_menu.items[i].hash));
        strlcpy(s_menu.items[i].label, labels[i], sizeof(s_menu.items[i].label));
        strlcpy(s_menu.items[i].entity, entities[i], sizeof(s_menu.items[i].entity));
        s_menu.items[i].action[0] = 0;
        s_menu.items[i].cached = cached[i];
    }
    menu_items_cached_first();
}

/* ---------------- Actions 页动作表（真实指令通道） ---------------- */

/* MP_CMD_SET_ACTION（s=动作名 → render_set_layout）；[v] 由
 * asset_dl_layout_cached() 判定该动作的 LAYOUT 包是否已在 TF */
static const struct { const char *label, *action; } PD_ITEMS[] = {
    { "Stand", MP_ACTION_STAND },
    { "Walk",  MP_ACTION_WALK  },
    { "Fly",   MP_ACTION_FLY   },
    { "Alert", MP_ACTION_ALERT },
    { "Hit",   MP_ACTION_HIT   },
};
#define PD_CNT ((int)(sizeof(PD_ITEMS) / sizeof(PD_ITEMS[0])))

static void menu_actions_collect(void)
{
    int n = (PD_CNT > MENU_LIST_MAX) ? MENU_LIST_MAX : PD_CNT;
    s_menu.item_cnt = n;
    for (int i = 0; i < n; i++) {
        strlcpy(s_menu.items[i].label, PD_ITEMS[i].label, sizeof(s_menu.items[i].label));
        strlcpy(s_menu.items[i].action, PD_ITEMS[i].action, sizeof(s_menu.items[i].action));
        s_menu.items[i].hash[0] = 0;
        s_menu.items[i].entity[0] = 0;
        s_menu.items[i].cached = asset_dl_layout_cached(PD_ITEMS[i].action);
    }
    /* 动作顺序是语义顺序（Stand→Walk→…），不按缓存重排 */
}

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

/* 根页行表（显式 idx→page 映射，不再用枚举算术耦合顺序） */
static const struct { const char *label; int page; } ROOT_ROWS[] = {
    { "Maps",      MENU_PAGE_MAPS      },
    { "Paperdoll", MENU_PAGE_PAPERDOLL },
    { "Actions",   MENU_PAGE_ACTIONS   },
    { "Monsters",  MENU_PAGE_NPC       },
    { "BGM",       MENU_PAGE_BGM       },
    { "Exit",      -1                  },   /* -1 = 收菜单（状态机 MENU_KEY 通道） */
};
#define ROOT_CNT ((int)(sizeof(ROOT_ROWS) / sizeof(ROOT_ROWS[0])))

static void menu_goto(menu_page_t page)
{
    s_menu.pend_page  = page;
    s_menu.pend_sel   = 0;
    s_menu.req_rebuild = true;      /* tick 排空重建（防 indev 上下文自删） */
}

/* 页脚提示改写（仅渲染任务调用：构建期与菜单 tick） */
static void menu_hint_set(const char *text)
{
    if (s_menu.hint_label) lv_label_set_text(s_menu.hint_label, text);
}

/* T4：列表页条目激活。
 *   已缓存 → 直接 post 既有指令（state_machine 查路径/条带后落地）；
 *   未缓存 → asset_dl_request_one(hash) 入队 → 提示「下载中」→ 菜单 tick
 *            轮询 asset_dl_file_cached() 落盘 → 成功再 post 同一指令。
 * cmd == MP_CMD_NONE = 只下载不派发（NPC 页：固件无 NPC 渲染通道）。 */
static void menu_activate_item(int idx, mp_cmd_type_t cmd)
{
    const menu_item_t *it = &s_menu.items[idx];

    if (it->cached) {
        if (cmd != MP_CMD_NONE) {
            mp_cmd_t c = { .type = cmd };
            strlcpy(c.s, it->hash, sizeof(c.s));
            mp_post_cmd(&c);
            ESP_LOGI(TAG, "menu: 已缓存直接派发 cmd=%d hash=%.16s", (int)cmd, it->hash);
        } else {
            ESP_LOGI(TAG, "menu: 已缓存（NPC 页无渲染通道，不派发）%.16s", it->hash);
        }
        menu_goto(MENU_PAGE_ROOT);
        return;
    }

    /* 未缓存：离线已被置灰（双保险再挡一次）；在线则拉包 */
    if (state_machine_offline_mode()) {
        menu_hint_set("OFFLINE: NOT CACHED");
        ESP_LOGW(TAG, "menu: 离线且未缓存，拒绝下载 %.16s", it->hash);
        return;
    }
    if (s_menu.dl_active) {                    /* 同屏只挂一个下载：避免请求互相覆盖 */
        menu_hint_set("BUSY: DOWNLOAD IN PROGRESS");
        ESP_LOGW(TAG, "menu: 已有下载在途，忽略 %.16s", it->hash);
        return;
    }
    if (!asset_dl_request_one(it->hash)) {
        menu_hint_set("REQ FAILED (NOT IN MANIFEST)");
        ESP_LOGW(TAG, "menu: request_one 被拒 %.16s", it->hash);
        return;
    }
    s_menu.dl_active     = true;
    s_menu.dl_cmd        = cmd;
    strlcpy(s_menu.dl_hash, it->hash, sizeof(s_menu.dl_hash));
    s_menu.dl_deadline_ms = mp_now_ms() + MENU_DL_TIMEOUT_MS;
    if (s_menu.hint_label) {
        lv_label_set_text_fmt(s_menu.hint_label, "DOWNLOADING %.8s ... WAIT",
                              s_menu.dl_hash);
    }
    ESP_LOGI(TAG, "menu: 下载中 %.16s → cmd=%d", s_menu.dl_hash, (int)cmd);
}

static void menu_activate(int idx)
{
    if (idx < 0 || idx >= s_menu.row_cnt) return;
    if (!s_menu.row_enabled[idx]) {          /* T3：置灰行侧键确认也不派发 */
        ESP_LOGI("menu", "row %d 置灰（离线未缓存），忽略确认", idx);
        return;
    }

    /* 列表页 Back 行（各页共用） */
    if (s_menu.back_idx >= 0 && idx == s_menu.back_idx) {
        menu_goto(MENU_PAGE_ROOT);
        return;
    }

    switch (s_menu.page) {
    case MENU_PAGE_ROOT:
        if (idx >= ROOT_CNT) break;
        if (ROOT_ROWS[idx].page < 0) { menu_request_exit(); break; }   /* Exit 行 */
        menu_goto((menu_page_t)ROOT_ROWS[idx].page);
        break;

    case MENU_PAGE_MAPS:
        if (idx < s_menu.item_cnt) menu_activate_item(idx, MP_CMD_SET_MAP);
        break;

    case MENU_PAGE_PAPERDOLL:
        if (idx < s_menu.item_cnt) menu_activate_item(idx, MP_CMD_SET_PARTS);
        break;

    case MENU_PAGE_NPC:
        /* 只下载：NPC 渲染通道缺失（渲染层单实体纸娃娃）→ 不 post 渲染指令 */
        if (idx < s_menu.item_cnt) menu_activate_item(idx, MP_CMD_NONE);
        break;

    case MENU_PAGE_ACTIONS:
        if (idx < s_menu.item_cnt) {
            mp_cmd_t c = { .type = MP_CMD_SET_ACTION };
            strlcpy(c.s, s_menu.items[idx].action, sizeof(c.s));
            mp_post_cmd(&c);
            menu_goto(MENU_PAGE_ROOT);
        }
        break;

    case MENU_PAGE_BGM:
        if      (idx == 0) menu_bgm_toggle();
        else if (idx == 1) bgm_prev();                  /* 本地曲目表循环，表空回退服务端 */
        else if (idx == 2) bgm_next();
        else if (idx == 3) menu_goto(MENU_PAGE_ROOT);   /* Back 行 */
        break;
    }
}

/* 触摸点击：只落旗标，动作由菜单 tick（渲染任务、非 indev 上下文）排空
 * ——indev 事件回调内不动控件树（防 lv_obj_clean 自删） */
static void row_click_cb(lv_event_t *e)
{
    s_menu.req_act_row = (int)(intptr_t)lv_event_get_user_data(e);
    s_menu.req_act = true;
}

/* T4：轮询下载落盘（菜单 tick 100ms）。
 * 落盘 → post 既定指令 + 原地重建（cached 标记刷新 + 一次性提示）；
 * 超时 → 提示并清态（下载失败/服务端不可达）。 */
static void menu_dl_poll(void)
{
    if (!s_menu.dl_active) return;

    if (asset_dl_file_cached(s_menu.dl_hash)) {
        if (s_menu.dl_cmd != MP_CMD_NONE) {
            mp_cmd_t c = { .type = s_menu.dl_cmd };
            strlcpy(c.s, s_menu.dl_hash, sizeof(c.s));
            mp_post_cmd(&c);
        }
        if (s_menu.dl_cmd != MP_CMD_NONE) {
            snprintf(s_menu.hint_once, sizeof(s_menu.hint_once),
                     "DL OK %.8s -> APPLIED", s_menu.dl_hash);
        } else {
            snprintf(s_menu.hint_once, sizeof(s_menu.hint_once),
                     "DL OK %.8s -> CACHED", s_menu.dl_hash);
        }
        ESP_LOGI(TAG, "menu: 下载完成 %.16s → cmd=%d", s_menu.dl_hash, (int)s_menu.dl_cmd);
        s_menu.dl_active = false;
        s_menu.pend_page = s_menu.page;      /* 原地重建：cached 标记刷新 */
        s_menu.pend_sel  = s_menu.sel;
        s_menu.req_rebuild = true;
        return;
    }

    if (mp_now_ms() > s_menu.dl_deadline_ms) {
        ESP_LOGW(TAG, "menu: 下载超时 %.16s", s_menu.dl_hash);
        s_menu.dl_active = false;
        menu_hint_set("DL TIMEOUT (SERVER?)");
    }
}

/* ---------------- 控件构建 ---------------- */

static void menu_style_row(lv_obj_t *btn, bool selected)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? 0x1E2A3A : 0x121216), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(selected ? 0x4DA3FF : 0x34343C), 0);
}

/* 置灰行（T3）配色：DISABLED 选择器覆盖默认态；选中态只提亮边框，
 * 侧键把光标移到置灰行时仍可见（文本保持暗色） */
static void menu_style_row_disabled(lv_obj_t *btn, bool selected)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x18181C), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(btn, lv_color_hex(selected ? 0x3C5A78 : 0x2A2A30),
                                  LV_STATE_DISABLED);
}

static lv_obj_t *menu_add_row(lv_obj_t *parent, int idx, const char *text,
                              int32_t y, int32_t h, bool enabled)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, 48, y);
    lv_obj_set_size(btn, s_br.sw - 96, h);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    menu_style_row(btn, idx == s_menu.sel && enabled);
    if (!enabled) {
        /* T3：离线且未缓存 → 置灰。DISABLED 选择器覆盖默认态配色；
         * LVGL 的 DISABLED 态同时拦住 indev 点击（侧键路径由 row_enabled 挡） */
        menu_style_row_disabled(btn, idx == s_menu.sel);
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_color(lb, lv_color_hex(enabled ? 0xFFFFFF : 0x6A6A74), 0);
    if (s_menu.f_item) lv_obj_set_style_text_font(lb, s_menu.f_item, 0);
    lv_label_set_text(lb, text);   /* ASCII：Montserrat 内置字体只含拉丁字形 */
    lv_obj_center(lb);

    lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    if (idx >= 0 && idx < MENU_ROWS_MAX) {
        s_menu.rows[idx] = btn;
        s_menu.row_enabled[idx] = enabled;
    }
    return btn;
}

/* 列表行显示串：[v]=已缓存 [ ]=未缓存可下载 [x]=未缓存且离线（置灰） */
static void menu_row_text(char *out, size_t cap, bool cached, bool enabled,
                          const char *label)
{
    snprintf(out, cap, "[%s] %s",
             cached ? "v" : (enabled ? " " : "x"),
             (label && label[0]) ? label : "(no name)");
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
    s_menu.hint_label = lb;
}

/* 列表页构建（Maps / Paperdoll / Monsters / Actions 共用布局）：
 *   行 0..item_cnt-1 = 真实条目；空清单插一行置灰空态；末行 Back。
 *   行高 45、间距 50：y = 92 + i*50（i≤6 → 底 437 < 页脚提示 ~448）。 */
static void menu_build_list(lv_obj_t *scr, const char *title, const char *empty_text,
                            const char *hint)
{
    menu_add_title(scr, title);
    int row = 0;
    if (s_menu.item_cnt <= 0) {
        menu_add_row(scr, row++, empty_text, 92, 45, false);   /* 明确空态，非假数据 */
    } else {
        for (int i = 0; i < s_menu.item_cnt && row < MENU_ROWS_MAX; i++) {
            bool en = s_menu.items[i].cached || !s_menu.offline;   /* T3 置灰判据 */
            char text[48];
            menu_row_text(text, sizeof(text), s_menu.items[i].cached, en,
                          s_menu.items[i].label);
            menu_add_row(scr, row++, text, 92 + i * 50, 45, en);
        }
    }
    menu_add_row(scr, row, "< Back", 92 + row * 50, 45, true);
    s_menu.back_idx = row;
    s_menu.row_cnt  = row + 1;
    if (s_menu.truncated) {
        /* 截断必须可见：清单条目多于单页行数（不静默吞数据） */
        char more[80];
        snprintf(more, sizeof(more), "%s (+MORE)", hint);
        menu_add_hint(scr, more);
    } else {
        menu_add_hint(scr, hint);
    }
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
    memset(s_menu.row_enabled, 0, sizeof s_menu.row_enabled);
    s_menu.status_label = NULL;
    s_menu.hint_label   = NULL;
    s_menu.row_cnt = 0;
    s_menu.item_cnt = 0;
    s_menu.back_idx = -1;
    s_menu.truncated = false;
    s_menu.sel_applied = -1;
    /* 选中行先夹到有效范围再建控件（构建期贴高亮用它） */
    if (s_menu.sel < 0 || s_menu.sel >= MENU_ROWS_MAX) s_menu.sel = 0;
    /* 快照网络态与清单 rev：本页所有 cached/置灰判据基于同一时刻（tick 比对重建） */
    s_menu.offline = state_machine_offline_mode();
    s_menu.offline_shown = s_menu.offline;
    s_menu.rev_shown = asset_dl_local_rev();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    menu_fonts_refresh();

    switch (s_menu.page) {
    case MENU_PAGE_ROOT: {
        menu_add_title(scr, "MiniPet");
        for (int i = 0; i < ROOT_CNT && i < MENU_ROWS_MAX; i++)
            menu_add_row(scr, i, ROOT_ROWS[i].label, 100 + i * 56, 50, true);
        s_menu.row_cnt = (ROOT_CNT < MENU_ROWS_MAX) ? ROOT_CNT : MENU_ROWS_MAX;
        menu_add_hint(scr, "UP:MID DOWN:BOT LONG:CLOCK");
        break;
    }
    case MENU_PAGE_MAPS:
        menu_maps_collect();
        menu_build_list(scr, "Maps", "NO MAP IN LOCAL MANIFEST",
                        "TAP: SWITCH/DL   [v] CACHED");
        break;
    case MENU_PAGE_PAPERDOLL:
        menu_parts_collect();
        menu_build_list(scr, "Paperdoll", "NO OUTFIT PACK (SYNC NEEDED)",
                        "TAP: WEAR PARTS  [v] CACHED");
        break;
    case MENU_PAGE_ACTIONS:
        menu_actions_collect();
        menu_build_list(scr, "Actions", "NO ACTION PACK (SYNC NEEDED)",
                        "TAP: PLAY  [v] CACHED  [ ] NO PACK");
        break;
    case MENU_PAGE_NPC:
        menu_npc_collect();
        menu_build_list(scr, "Monsters", "NO NPC ASSET (SERVER PUSH)",
                        "NPC PACKS: TAP TO CACHE (NO RENDER)");
        break;
    case MENU_PAGE_BGM: {
        menu_add_title(scr, "BGM");
        s_menu.status_label = lv_label_create(scr);
        lv_obj_set_style_text_color(s_menu.status_label, lv_color_hex(0xB9B9C4), 0);
        if (s_menu.f_small) lv_obj_set_style_text_font(s_menu.status_label, s_menu.f_small, 0);
        lv_label_set_text(s_menu.status_label, "Tracks:-");
        lv_obj_align(s_menu.status_label, LV_ALIGN_TOP_MID, 0, 96);
        menu_bgm_status_refresh();

        menu_add_row(scr, 0, "Play / Pause", 170, 54, true);
        menu_add_row(scr, 1, "Prev",          238, 54, true);
        menu_add_row(scr, 2, "Next",          306, 54, true);
        menu_add_row(scr, 3, "< Back",        374, 54, true);
        s_menu.row_cnt = 4;
        menu_add_hint(scr, "TOUCH OR TOP KEY");
        break;
    }
    }

    if (s_menu.sel >= s_menu.row_cnt) s_menu.sel = 0;   /* 页内行数变化（截断/空态） */
    s_menu.sel_applied = -1;   /* 交下一 tick 统一重贴高亮（与夹取后的 sel 严格一致） */

    /* T4 一次性提示（下载完成）：本次重建消费后清空 */
    if (s_menu.hint_once[0]) {
        menu_hint_set(s_menu.hint_once);
        s_menu.hint_once[0] = 0;
    }

    ESP_LOGI(TAG, "menu_rebuild: page=%d rows=%d items=%d offline=%d widgets=%u",
             (int)s_menu.page, s_menu.row_cnt, s_menu.item_cnt, (int)s_menu.offline,
             (unsigned)lv_obj_get_child_count(scr));
    lv_obj_invalidate(scr);            /* DIRECT 模式强制整屏重绘入 menu_buf */
}

/* 高亮跟随 sel（菜单 tick；sel 变化才重贴，避免无谓失效区）。
 * 置灰行不贴高亮底（保持 DISABLED 配色；选中框仍可见） */
static void menu_apply_selection(void)
{
    if (s_menu.sel_applied == s_menu.sel) return;
    for (int i = 0; i < s_menu.row_cnt && i < MENU_ROWS_MAX; i++) {
        if (!s_menu.rows[i]) continue;
        if (!s_menu.row_enabled[i]) {
            menu_style_row_disabled(s_menu.rows[i], i == s_menu.sel);  /* 光标可见 */
            continue;
        }
        menu_style_row(s_menu.rows[i], i == s_menu.sel);
    }
    s_menu.sel_applied = s_menu.sel;
}

/* 菜单态 100ms 节拍（渲染任务）：排空侧键/触摸/换页请求 → 贴高亮 → 轮询下载
 * → 500ms 刷 BGM 状态/网络态。所有会动控件树的操作都收敛到本回调
 * （渲染任务、非 indev 上下文）执行 */
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
    if (s_menu.req_act) {                   /* 触摸点击（indev 回调只落旗标） */
        s_menu.req_act = false;
        menu_activate(s_menu.req_act_row);
        return;
    }
    if (s_menu.req_ok) {
        s_menu.req_ok = false;
        menu_activate(s_menu.sel);
        return;
    }
    menu_dl_poll();                         /* T4：下载落盘轮询 → post 切换指令 */
    menu_apply_selection();
    if (++s_menu.bgm_refr_div >= 5) {   /* 100ms×5 = 500ms */
        s_menu.bgm_refr_div = 0;
        menu_bgm_status_refresh();
        /* 网络态翻转（离线置灰跟随）或清单 rev 变化（列表/缓存标记跟随）→ 重建 */
        bool off = state_machine_offline_mode();
        uint32_t rev = asset_dl_local_rev();
        if (off != s_menu.offline_shown || rev != s_menu.rev_shown) {
            s_menu.pend_page = s_menu.page;
            s_menu.pend_sel  = s_menu.sel;
            s_menu.req_rebuild = true;
        }
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
    s_menu.req_act = false;
    s_menu.req_act_row = -1;
    s_menu.dl_active = false;         /* T4：上一轮残留的下载轮询不带进新菜单 */
    s_menu.dl_hash[0] = 0;
    s_menu.dl_cmd = MP_CMD_NONE;
    s_menu.hint_once[0] = 0;
    s_menu.back_idx = -1;
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
