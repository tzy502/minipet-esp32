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
#include "provision.h"   /* E14：Reset WiFi */
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
    MENU_PAGE_RESET,         /* 【E14 补齐】Reset WiFi：清 NVS 配网凭据并重启（免插线重配网） */
} menu_page_t;

#define MENU_ROWS_MAX   8       /* 单页可选行上限（含 Back/Exit 行） */
#define MENU_LIST_MAX   6       /* 列表页真实条目上限（+Back(+空态行) ≤ 8） */
#define MENU_COLLECT_MAX (MENU_LIST_MAX + 1)  /* 多收 1 条用于探测「还有更多」 */
#define MENU_DL_TIMEOUT_MS 30000  /* T4：单包下载轮询超时（tick 100ms 轮询落盘） */

/* ---------------- 羊皮纸滚筒主题常量（2026-09-30 定稿，色彩对照原型 HTML） ---------------- */
#define MENU_SCR_BORDER_W  12       /* 整屏棕金描边框宽（内圈再叠 2px #43331F 细线） */
#define MENU_TITLE_H       46       /* 标题栏高 */
#define MENU_STATUS_H      22       /* 底部状态行高 */
#define MENU_ROLLER_W      360      /* 滚筒宽（水平居中） */
#define MENU_ROLLER_VIS    5        /* 可见行数 */
#define MENU_ROLLER_ROW_H  58       /* 行高 = 主字体行高 + text_line_space（反解行距锁定） */
#define MENU_ROLLER_H      (MENU_ROLLER_VIS * MENU_ROLLER_ROW_H)  /* 290 */
#define MENU_FADE_H        50       /* 上下渐隐遮罩高 */
#define MENU_BTN_W         92       /* OK/Back 蓝色渐变按钮 */
#define MENU_BTN_H         34
#define MENU_BTN_GAP       14
#define MENU_SCROLL_W      7        /* 右侧自绘滚动条宽 */

#define MENU_C_PARCH_HI  0xF6EBD2   /* 羊皮纸渐变亮端 */
#define MENU_C_PARCH_LO  0xEEDCB4   /* 羊皮纸渐变暗端 */
#define MENU_C_FRAME     0x8A6D35   /* 棕金外框 / thumb */
#define MENU_C_FRAME_IN  0x43331F   /* 内圈细线 / 标题栏渐变暗端 */
#define MENU_C_TITLE_A   0x6B5432   /* 标题栏渐变亮端 */
#define MENU_C_GOLD      0xFFE9B0   /* 标题金字 */
#define MENU_C_DIAMOND   0xD9A93F   /* 标题两侧菱形装饰 */
#define MENU_C_TEXT      0x4A3826   /* 正文深棕 */
#define MENU_C_SELBAND   0xFFF6D8   /* 选中行背景带 */
#define MENU_C_BLUE_A    0x4F7CD6   /* 按钮蓝渐变亮端 */
#define MENU_C_BLUE_B    0x2C4F9E   /* 按钮蓝渐变暗端 */
#define MENU_C_TRACK     0xD8C8A0   /* 滚动条 track */

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
    bool     row_enabled[MENU_ROWS_MAX];   /* T3：置灰行（离线未缓存）不派发；滚筒页=选项可否确认 */

    /* 滚筒选择页（ROOT/MAPS/PAPERDOLL/ACTIONS）控件；NULL = 行式页 */
    lv_obj_t *roller;           /* lv_roller INFINITE（触摸拖拽/惯性/吸附原生） */
    lv_obj_t *roller_track;     /* 右侧自绘滚动条 track（原生条 INFINITE 回绕会跳变，R4） */
    lv_obj_t *roller_thumb;     /* thumb：高度按 可见行/总行数，y 按选中比例 */

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

/* ---------------- MENU 真实选择器（E7 菜单真实化 → 2026-09-30 羊皮纸滚筒改版） ----------------
 * 呈现（视觉定稿 docs/ai/menu-roller-beauty.html + menu-roller-spec.md）：
 *   整屏羊皮纸渐变 + 12px 棕金描边框（内圈 #43331F 细线）+ 深棕渐变标题栏
 *   （金字 + 双菱形装饰）+ 底部状态行。
 *   ROOT/MAPS/PAPERDOLL/ACTIONS 四个"选择"页 = lv_roller INFINITE 无限滚筒
 *   （5 行 × 58px，触摸拖拽/惯性/吸附由 LVGL 原生承担）+ 上下渐隐遮罩 +
 *   右侧自绘滚动条 + 底部 OK/Back 蓝色渐变按钮（根页 Back=退出，子页 Back=回根页）。
 *   NPC/BGM/RESET 控制/缓存页保留行式布局，只统一羊皮纸/棕金配色。
 * 数据面（全部保留，仅呈现改造）：
 *   Maps     子页：asset_dl_bgmap_list() 真实 BGMAP 条目（hash + ASCII label）
 *            [v]=已缓存 [ ]=未缓存（确认→按 hash 拉包→落盘后 MP_CMD_SET_MAP）
 *            [x]=未缓存且离线 → row_enabled=false 拒绝确认（T3）
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

/* 侧键导航：dir=0 上移（中键）/ 1 下移（底键）。滚筒页（ROOT/MAPS/PAPERDOLL/
 * ACTIONS）：写 sel 后由菜单 tick 在渲染任务里 lv_roller_set_selected(sel,
 * LV_ANIM_ON) 驱动滚筒平滑滚动——本函数在 input 任务上下文被调，绝不直接动
 * 控件树（跨任务纪律）；行式页移动行高亮。越界回绕。只写单字 sel */
void render_menu_nav(int dir)
{
    if (!s_br.menu_mode || s_menu.row_cnt <= 0) {
        ESP_LOGW("menu", "nav 丢弃：menu_mode=%d row_cnt=%d（state≠MENU 或菜单未建）",
                 (int)s_br.menu_mode, s_menu.row_cnt);
        return;
    }
    int old = s_menu.sel;
    /* 【2026-09-27 真机定稿：恢复回绕】此前一版改成"到边界停住"，真机日志
     * 实证反而把用户焊死：中键=光标上移、初始 sel=0 → 每次都命中
     * `nav(0) 已到首行 0/6：停住`，用户观感就是"上下键全不能用了"。
     * 回绕语义下：首行再上移 → 走到末行（Exit），有明确可见反馈；
     * 末行再下移 → 回首行。这是循环列表的标准手感，保留回绕。 */
    if (dir) s_menu.sel = (s_menu.sel + 1) % s_menu.row_cnt;
    else      s_menu.sel = (s_menu.sel + s_menu.row_cnt - 1) % s_menu.row_cnt;
    ESP_LOGI("menu", "nav(%d) sel %d→%d/%d", dir, old, s_menu.sel, s_menu.row_cnt);
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

/* 【用户定稿 2026-09-27】底键在菜单内的两个动作对外暴露（input 任务调用）：
 *   render_menu_nav_down()    短按 → 光标下移（末行回绕到首行）
 *   render_menu_request_exit() 长按 → 退出菜单（复用状态机 MENU→POKER 通道） */
int render_menu_nav_down(void)
{
    if (!s_br.menu_mode || s_menu.row_cnt <= 0) return -1;
    int old = s_menu.sel;
    s_menu.sel = (s_menu.sel + 1) % s_menu.row_cnt;
    ESP_LOGI("menu", "nav(down) sel %d→%d/%d", old, s_menu.sel, s_menu.row_cnt);
    return s_menu.sel;
}

void render_menu_request_exit(void)
{
    menu_request_exit();
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
    { "Reset WiFi", MENU_PAGE_RESET    },   /* E14：清凭据重配网（无需连电脑擦 NVS） */
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

    /* 【派发探针 2026-09-27】用户报"无论怎么点选中的都是第一个 map"：
     * 记录 行号→hash→cmd→cached，用于区分「选中索引没生效」与「派发静默失败」。 */
    ESP_LOGI(TAG, "menu: activate idx=%d cmd=%d cached=%d hash=%.16s label=%.24s",
             idx, (int)cmd, (int)it->cached, it->hash, it->label);

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
        /* 【E7 定稿 2026-09-27】需求：「窗口内含 BGM 入口（呼出 E6 的触摸控制条）」
         * ——BGM 行不再是"进另一个全屏页"，而是呼出半屏控制条：置请求旗标，
         * 由 input 任务收菜单 + render_bgm_bar_show()（退出在状态机侧统一走
         * MENU_KEY，控制条只在 POKER 态绘制，上半屏继续显示宠物）。 */
        if (ROOT_ROWS[idx].page == MENU_PAGE_BGM) {
            ESP_LOGI(TAG, "菜单 BGM 入口 → 请求呼出半屏控制条");
            render_bgm_bar_request_from_menu();
            break;
        }
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
        else if (idx == 3) bgm_volume_add(-10);         /* E6：控制条音量 - */
        else if (idx == 4) bgm_volume_add(+10);         /* E6：控制条音量 + */
        else if (idx == 5) {                            /* E8：设备端先选类型（切源） */
            mp_bgm_source_t cur = bgm_get_source();
            mp_bgm_source_t nxt = (cur == MP_BGM_SRC_WZ) ? MP_BGM_SRC_QQ : MP_BGM_SRC_WZ;
            if (bgm_source_greyed(nxt)) {
                menu_hint_set("SOURCE UNAVAILABLE");
            } else {
                mp_audio_msg_t m = { .type = MP_AUDIO_SOURCE, .a = (int32_t)nxt };
                if (mp_post_audio(&m)) menu_hint_set("SOURCE SWITCHED");
                else ESP_LOGW(TAG, "audio_q 满，切源丢失");
            }
            s_menu.pend_page = s_menu.page;   /* 原地重建以刷新标签/置灰 */
            s_menu.pend_sel  = s_menu.sel;
            s_menu.req_rebuild = true;
        }
        else if (idx == 6) menu_goto(MENU_PAGE_ROOT);   /* Back 行 */
        break;

    case MENU_PAGE_RESET:
        if (idx == 0) {
            ESP_LOGW(TAG, "用户确认 Reset WiFi → 清配网凭据并重启");
            provision_factory_reset();      /* 清 NVS 里的 wifi_ssid/wifi_pass/srv_url */
        } else if (idx == 1) {
            menu_goto(MENU_PAGE_ROOT);
        }
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
    /* 【羊皮纸主题 2026-09-30】行式页（Monsters/BGM/Reset）与滚筒页统一配色：
     * 选中 = #FFF6D8 高亮带 + 棕金 3px 框；未选中 = 浅羊皮底 + 浅棕细框。
     * 三重加固保留：①明显色差 ②状态样式 ③末尾显式 invalidate。 */
    lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? MENU_C_SELBAND : 0xEFE2C0), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(selected ? MENU_C_FRAME : 0xC9B48A), 0);
    lv_obj_set_style_border_width(btn, selected ? 3 : 2, 0);
    if (selected) lv_obj_add_state(btn, LV_STATE_FOCUSED);
    else          lv_obj_remove_state(btn, LV_STATE_FOCUSED);
    lv_obj_invalidate(btn);
}

/* 置灰行（T3）配色：DISABLED 选择器覆盖默认态；选中态只提亮边框，
 * 侧键把光标移到置灰行时仍可见（文本保持暗色） */
static void menu_style_row_disabled(lv_obj_t *btn, bool selected)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xE6D8B6), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(btn, lv_color_hex(selected ? 0x8A7A5C : 0xB5A480),
                                  LV_STATE_DISABLED);
}

static lv_obj_t *menu_add_row(lv_obj_t *parent, int idx, const char *text,
                              int32_t y, int32_t h, bool enabled)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, 48, y);
    lv_obj_set_size(btn, s_br.sw - 96, h);
    lv_obj_set_style_radius(btn, 8, 0);
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
    /* 羊皮纸主题：深棕正文 / 置灰浅棕 */
    lv_obj_set_style_text_color(lb, lv_color_hex(enabled ? MENU_C_TEXT : 0x8A7A5C), 0);
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

/* ---------------- 羊皮纸公共骨架 + 滚筒套件（2026-09-30 定稿改版） ---------------- */

/* 标题两侧金色菱形装饰（8×8 旋转 45°；transform 单位 0.1°，pivot 锁中心） */
static lv_obj_t *menu_add_diamond(lv_obj_t *parent)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_set_clickable(d, false);      /* 装饰件：触摸穿透（9.6 推荐替代 remove_flag） */
    lv_obj_set_scrollable(d, false);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_bg_color(d, lv_color_hex(MENU_C_DIAMOND), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, 1, 0);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_set_scrollbar_mode(d, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_transform_pivot_x(d, 4, 0);
    lv_obj_set_style_transform_pivot_y(d, 4, 0);
    lv_obj_set_style_transform_rotation(d, 450, 0);   /* 450 = 45.0° */
    return d;
}

/* 每页共用骨架：整屏羊皮纸渐变 + 12px 棕金描边 + 内圈 #43331F 细线
 * + 深棕渐变标题栏（金字居中 + 左右菱形）+ 底部状态行（status_label：
 * 滚筒页回显当前选中 / BGM 页回显播放态，刷新见 menu_tick_cb） */
static void menu_chrome_build(lv_obj_t *scr, const char *title)
{
    /* 整屏羊皮纸 + 12px 棕金描边框 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(MENU_C_PARCH_HI), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(MENU_C_PARCH_LO), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(scr, lv_color_hex(MENU_C_FRAME), 0);
    lv_obj_set_style_border_width(scr, MENU_SCR_BORDER_W, 0);

    /* 内圈细线（纯装饰，不可点击：触摸穿透不挡滚筒） */
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_clickable(line, false);   /* 内圈细线：触摸穿透不挡滚筒 */
    lv_obj_set_scrollable(line, false);
    lv_obj_set_pos(line, MENU_SCR_BORDER_W, MENU_SCR_BORDER_W);
    lv_obj_set_size(line, s_br.sw - 2 * MENU_SCR_BORDER_W,
                    s_br.sh - 2 * MENU_SCR_BORDER_W);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(line, lv_color_hex(MENU_C_FRAME_IN), 0);
    lv_obj_set_style_border_width(line, 2, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_scrollbar_mode(line, LV_SCROLLBAR_MODE_OFF);

    /* 标题栏：深棕渐变 + 底缘深色收边 */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_clickable(bar, false);    /* 标题栏纯展示 */
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_pos(bar, MENU_SCR_BORDER_W + 2, MENU_SCR_BORDER_W + 2);
    lv_obj_set_size(bar, s_br.sw - 2 * (MENU_SCR_BORDER_W + 2), MENU_TITLE_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(MENU_C_TITLE_A), 0);
    lv_obj_set_style_bg_grad_color(bar, lv_color_hex(MENU_C_FRAME_IN), 0);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x2E2314), 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *tt = lv_label_create(bar);
    lv_obj_set_style_text_color(tt, lv_color_hex(MENU_C_GOLD), 0);
    if (s_menu.f_title) lv_obj_set_style_text_font(tt, s_menu.f_title, 0);
    lv_obj_set_style_text_letter_space(tt, 2, 0);
    lv_label_set_text(tt, title);
    lv_obj_center(tt);

    /* 菱形贴标题文字两侧（align_to 是一次性定位，先强制布局拿到 label 实宽） */
    lv_obj_update_layout(tt);
    lv_obj_t *dg = menu_add_diamond(bar);
    lv_obj_align_to(dg, tt, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    dg = menu_add_diamond(bar);
    lv_obj_align_to(dg, tt, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    /* 底部状态行 */
    lv_obj_t *stbox = lv_obj_create(scr);
    lv_obj_set_clickable(stbox, false);  /* 状态行纯展示 */
    lv_obj_set_scrollable(stbox, false);
    lv_obj_set_pos(stbox, MENU_SCR_BORDER_W + 2,
                   s_br.sh - MENU_SCR_BORDER_W - 2 - MENU_STATUS_H);
    lv_obj_set_size(stbox, s_br.sw - 2 * (MENU_SCR_BORDER_W + 2), MENU_STATUS_H);
    lv_obj_set_style_bg_color(stbox, lv_color_hex(0xFFF4D4), 0);
    lv_obj_set_style_bg_opa(stbox, LV_OPA_90, 0);
    lv_obj_set_style_border_color(stbox, lv_color_hex(0xC9B48A), 0);
    lv_obj_set_style_border_width(stbox, 1, 0);
    lv_obj_set_style_border_side(stbox, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(stbox, 0, 0);
    lv_obj_set_style_pad_all(stbox, 0, 0);
    lv_obj_set_scrollbar_mode(stbox, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *st = lv_label_create(stbox);
    lv_obj_set_style_text_color(st, lv_color_hex(MENU_C_TEXT), 0);
    if (s_menu.f_small) lv_obj_set_style_text_font(st, s_menu.f_small, 0);
    lv_label_set_text(st, "");
    lv_obj_center(st);
    s_menu.status_label = st;
}

static void menu_add_hint(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_obj_set_style_text_color(lb, lv_color_hex(0x8A7A5C), 0);
    if (s_menu.f_small) lv_obj_set_style_text_font(lb, s_menu.f_small, 0);
    lv_label_set_text(lb, text);   /* ASCII：Montserrat 内置字体只含拉丁字形 */
    /* 底部状态行上方：状态行归 SEL/BGM 回显，提示行让位不占其位 */
    lv_obj_align(lb, LV_ALIGN_BOTTOM_MID, 0,
                 -(MENU_SCR_BORDER_W + 2 + MENU_STATUS_H + 6));
    s_menu.hint_label = lb;
}

/* 滚筒上下渐隐遮罩：羊皮纸色→透明（渐变端点透明度 = bg_main_opa/bg_grad_opa）。
 * 不可点击：触摸穿透到下方滚筒；自身不参与滚动 */
static void menu_add_fade(lv_obj_t *scr, int32_t x, int32_t y, bool top)
{
    lv_obj_t *f = lv_obj_create(scr);
    lv_obj_set_clickable(f, false);      /* 遮罩触摸穿透到滚筒 */
    lv_obj_set_scrollable(f, false);
    lv_obj_set_pos(f, x, y);
    lv_obj_set_size(f, MENU_ROLLER_W, MENU_FADE_H);
    lv_obj_set_style_bg_color(f, lv_color_hex(top ? MENU_C_PARCH_HI : MENU_C_PARCH_LO), 0);
    lv_obj_set_style_bg_grad_color(f, lv_color_hex(top ? MENU_C_PARCH_HI : MENU_C_PARCH_LO), 0);
    lv_obj_set_style_bg_grad_dir(f, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(f, 0, 0);
    lv_obj_set_style_radius(f, 0, 0);
    lv_obj_set_style_pad_all(f, 0, 0);
    lv_obj_set_scrollbar_mode(f, LV_SCROLLBAR_MODE_OFF);
    if (top) {   /* 上遮罩：上端实、向下渐透 */
        lv_obj_set_style_bg_main_opa(f, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_opa(f, LV_OPA_TRANSP, 0);
    } else {     /* 下遮罩：下端实、向上渐透 */
        lv_obj_set_style_bg_main_opa(f, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_grad_opa(f, LV_OPA_COVER, 0);
    }
}

/* lv_roller 样式集中处（羊皮纸滚筒）：
 *   主体 LV_PART_MAIN = 羊皮纸渐变底 + f_item(Montserrat20) 深棕字；
 *   选中行 LV_PART_SELECTED = #FFF6D8 背景带 + f_title(Montserrat28) 深棕大字。
 * 行高锁定 58：roller 行距 = 主字体行高 + text_line_space，反解行距得到，
 * visible_row_count(5) 由此得到精确 290 高（与 LVGL 9.6 lv_roller.c 同式）。 */
static void menu_roller_style(lv_obj_t *r)
{
    lv_obj_set_style_bg_color(r, lv_color_hex(MENU_C_PARCH_HI), 0);
    lv_obj_set_style_bg_grad_color(r, lv_color_hex(MENU_C_PARCH_LO), 0);
    lv_obj_set_style_bg_grad_dir(r, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_pad_top(r, 0, 0);
    lv_obj_set_style_pad_bottom(r, 0, 0);
    lv_obj_set_style_pad_left(r, 0, 0);
    lv_obj_set_style_pad_right(r, 0, 0);
    lv_obj_set_style_shadow_width(r, 0, 0);
    lv_obj_set_style_anim_duration(r, 200, 0);   /* 侧键 set_selected 的平滑滚动时长 */

    const lv_font_t *f = s_menu.f_item;
    int32_t lh = f ? lv_font_get_line_height(f) : 24;
    int32_t ls = MENU_ROLLER_ROW_H - lh;
    if (ls < 4) ls = 4;
    lv_obj_set_style_text_line_space(r, ls, LV_PART_MAIN);
    if (f) lv_obj_set_style_text_font(r, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(r, lv_color_hex(MENU_C_TEXT), LV_PART_MAIN);

    /* 选中行：羊皮纸高亮带横贯滚筒全宽（roller 原生绘制） */
    if (s_menu.f_title) lv_obj_set_style_text_font(r, s_menu.f_title, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(MENU_C_TEXT), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r, lv_color_hex(MENU_C_SELBAND), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_90, LV_PART_SELECTED);
    lv_obj_set_style_radius(r, 10, LV_PART_SELECTED);
    lv_obj_set_style_border_width(r, 0, LV_PART_SELECTED);
}

/* 滚筒 VALUE_CHANGED（触摸释放/点行时刻）：跨任务纪律只写单字 sel，
 * 状态行/滚动条等控件树操作统一由菜单 tick 的 menu_apply_selection 完成。
 * 确认不走这里：点击任意行只滚到该行，确认 = OK 按钮/顶键（两段式防误触） */
static void menu_roller_value_cb(lv_event_t *e)
{
    int sel = (int)lv_roller_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (sel >= 0 && sel < MENU_ROWS_MAX) s_menu.sel = sel;
}

/* 底部 OK 按钮：顶键同通道（req_ok 旗标，tick 排空派发 menu_activate(sel)） */
static void menu_btn_ok_cb(lv_event_t *e)
{
    (void)e;
    s_menu.req_ok = true;
}

/* 底部 Back 按钮：根页=退出菜单（req_exit 走状态机 MENU_KEY 通道）；
 * 子页=回根页（menu_goto 只落旗标，tick 重建，不在此动控件树） */
static void menu_btn_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_menu.page == MENU_PAGE_ROOT) s_menu.req_exit = true;
    else menu_goto(MENU_PAGE_ROOT);
}

/* 蓝色渐变操作按钮（OK/Back），白字 f_small */
static lv_obj_t *menu_add_opbtn(lv_obj_t *scr, const char *text,
                                lv_event_cb_t cb, int32_t x)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_pos(b, x, s_br.sh - MENU_SCR_BORDER_W - 2 - MENU_STATUS_H - 6 - MENU_BTN_H);
    lv_obj_set_size(b, MENU_BTN_W, MENU_BTN_H);
    lv_obj_set_style_bg_color(b, lv_color_hex(MENU_C_BLUE_A), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(MENU_C_BLUE_B), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);

    lv_obj_t *lb = lv_label_create(b);
    lv_obj_set_style_text_color(lb, lv_color_hex(0xFFFFFF), 0);
    if (s_menu.f_small) lv_obj_set_style_text_font(lb, s_menu.f_small, 0);
    lv_label_set_text(lb, text);
    lv_obj_center(lb);

    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* 行式列表页构建（现仅 Monsters/NPC 缓存页使用；Maps/Paperdoll/Actions 已改滚筒）：
 *   行 0..item_cnt-1 = 真实条目；空清单插一行置灰空态；末行 Back。
 *   行高 42、间距 46：y = 78 + i*46（i≤7 → 底 442 < 底部状态行 444）。 */
static void menu_build_list(lv_obj_t *scr, const char *empty_text, const char *hint)
{
    int row = 0;
    if (s_menu.item_cnt <= 0) {
        menu_add_row(scr, row++, empty_text, 78, 42, false);   /* 明确空态，非假数据 */
    } else {
        for (int i = 0; i < s_menu.item_cnt && row < MENU_ROWS_MAX; i++) {
            bool en = s_menu.items[i].cached || !s_menu.offline;   /* T3 置灰判据 */
            char text[48];
            menu_row_text(text, sizeof(text), s_menu.items[i].cached, en,
                          s_menu.items[i].label);
            menu_add_row(scr, row++, text, 78 + i * 46, 42, en);
        }
    }
    menu_add_row(scr, row, "< Back", 78 + row * 46, 42, true);
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

/* 选择页选项串打包：真实条目 + [v]/[x]/[ ] 缓存标记 + 末行 "< Back"。
 * LVGL roller 的选项 = 单个 '\n' 分隔串（asset_dl label 为 ASCII，不含 '\n'）。
 * en[i] = 该选项可否确认（离线未缓存 = false，T3 语义在滚筒上的等价物——
 * roller 无法逐行置灰，确认路径由 menu_activate 的 row_enabled 挡）。
 * 返回选项数；back_idx/row_cnt 由调用方据此写入。 */
static int menu_roller_opts_pack(char *out, size_t cap, bool *en, const char *empty_text)
{
    size_t o = 0;
    int row = 0;
    /* 防御：snprintf 返回"应为"长度，截断时 o 会超前；clamp 防 size_t 下溢
     * （label 结构体定长 32、行数 ≤8，理论不可达，只作兜底） */
#define OPTS_CLAMP() do { if (o > cap - 1) o = cap - 1; } while (0)
    if (s_menu.item_cnt <= 0) {          /* 明确空态，非假数据 */
        o += (size_t)snprintf(out, cap, "%s", empty_text);
        OPTS_CLAMP();
        en[row++] = false;
    } else {
        for (int i = 0; i < s_menu.item_cnt && row < MENU_ROWS_MAX - 1; i++) {
            bool e = s_menu.items[i].cached || !s_menu.offline;   /* T3 置灰判据 */
            char text[48];
            menu_row_text(text, sizeof(text), s_menu.items[i].cached, e,
                          s_menu.items[i].label);
            o += (size_t)snprintf(out + o, cap - o, "%s%s", row ? "\n" : "", text);
            OPTS_CLAMP();
            en[row++] = e;
        }
    }
    /* 末行 "< Back"：保留既有返回路径（与底部 Back 按钮并存，两路都可回根页） */
    o += (size_t)snprintf(out + o, cap - o, "%s< Back", row ? "\n" : "");
    OPTS_CLAMP();
#undef OPTS_CLAMP
    en[row] = true;
    s_menu.back_idx = row;
    return row + 1;
}

/* 选择页滚筒构建：lv_roller INFINITE 无限循环 + 5 行 × 58px（垂直居中）
 * + 上下渐隐遮罩 + 右侧自绘滚动条 + 底部 OK/Back 蓝色按钮。
 * 触摸拖拽/惯性/吸附由 roller + indev 原生承担（lv_bridge_touch_feed 不变）。 */
static void menu_build_roller(lv_obj_t *scr, const char *opts, int cnt,
                              const bool *en, const char *hint)
{
    int32_t rx = (s_br.sw - MENU_ROLLER_W) / 2;
    int32_t btn_top = s_br.sh - MENU_SCR_BORDER_W - 2 - MENU_STATUS_H - 6 - MENU_BTN_H;
    int32_t ry = MENU_SCR_BORDER_W + 2 + MENU_TITLE_H +
                 ((btn_top - (MENU_SCR_BORDER_W + 2 + MENU_TITLE_H)) - MENU_ROLLER_H) / 2;
    if (cnt <= 0) return;

    lv_obj_t *r = lv_roller_create(scr);
    menu_roller_style(r);                       /* 字体/行距必须先于 visible_row_count */
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(r, MENU_ROLLER_VIS);
    lv_obj_set_pos(r, rx, ry);
    lv_obj_set_width(r, MENU_ROLLER_W);
    /* 原生滚动条关闭：INFINITE 回绕瞬间原生 thumb 跳变（规格 §5.1-R4），
     * 位置指示由右侧自绘 track/thumb 承担 */
    lv_obj_set_scrollbar_mode(r, LV_SCROLLBAR_MODE_OFF);
    if (s_menu.sel < 0 || s_menu.sel >= cnt) s_menu.sel = 0;
    lv_roller_set_selected(r, (uint32_t)s_menu.sel, LV_ANIM_OFF);
    lv_obj_add_event_cb(r, menu_roller_value_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_menu.roller = r;

    /* 上下渐隐遮罩（羊皮纸色→透明，触摸穿透） */
    menu_add_fade(scr, rx, ry, true);
    menu_add_fade(scr, rx, ry + MENU_ROLLER_H - MENU_FADE_H, false);

    /* 右侧自绘滚动条：track 固定；thumb 高按 可见行/总行数、y 按选中比例移动 */
    lv_obj_t *track = lv_obj_create(scr);
    lv_obj_set_clickable(track, false);  /* 滚动条纯指示，不抢触摸 */
    lv_obj_set_scrollable(track, false);
    lv_obj_set_pos(track, rx + MENU_ROLLER_W + 12, ry);
    lv_obj_set_size(track, MENU_SCROLL_W, MENU_ROLLER_H);
    lv_obj_set_style_bg_color(track, lv_color_hex(MENU_C_TRACK), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, 4, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_set_scrollbar_mode(track, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *thumb = lv_obj_create(track);
    lv_obj_set_clickable(thumb, false);
    lv_obj_set_scrollable(thumb, false);
    lv_obj_set_pos(thumb, 0, 0);
    lv_obj_set_size(thumb, MENU_SCROLL_W, 16);
    lv_obj_set_style_bg_color(thumb, lv_color_hex(MENU_C_FRAME), 0);
    lv_obj_set_style_bg_grad_color(thumb, lv_color_hex(0x5A4632), 0);
    lv_obj_set_style_bg_grad_dir(thumb, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(thumb, 0, 0);
    lv_obj_set_style_radius(thumb, 3, 0);
    lv_obj_set_style_pad_all(thumb, 0, 0);
    lv_obj_set_scrollbar_mode(thumb, LV_SCROLLBAR_MODE_OFF);
    s_menu.roller_track = track;
    s_menu.roller_thumb = thumb;

    /* 底部 OK/Back 蓝色渐变按钮（根页 Back=退出；子页 Back=回根页） */
    int32_t bx = (s_br.sw - (2 * MENU_BTN_W + MENU_BTN_GAP)) / 2;
    menu_add_opbtn(scr, "OK", menu_btn_ok_cb, bx);
    menu_add_opbtn(scr, "Back", menu_btn_back_cb, bx + MENU_BTN_W + MENU_BTN_GAP);

    /* 行契约映射：滚筒选项回填 row_cnt/row_enabled，menu_activate 原语义零改动 */
    s_menu.row_cnt = cnt;
    for (int i = 0; i < cnt && i < MENU_ROWS_MAX; i++)
        s_menu.row_enabled[i] = en ? en[i] : true;

    menu_add_hint(scr, hint);
}

/* 选择页（Maps/Paperdoll/Actions）公共装配：收集已在 *_collect 完成，
 * 这里打包选项串 → 建滚筒（含截断 (+MORE) 提示）。 */
static void menu_build_selection_page(lv_obj_t *scr, const char *empty_text,
                                      const char *hint)
{
    static char opts[MENU_ROWS_MAX * 48];   /* 仅渲染任务调用，静态免大栈 */
    static bool en[MENU_ROWS_MAX];
    int row = menu_roller_opts_pack(opts, sizeof(opts), en, empty_text);
    s_menu.row_cnt = row;
    if (s_menu.truncated) {
        char more[80];
        snprintf(more, sizeof(more), "%s (+MORE)", hint);
        menu_build_roller(scr, opts, row, en, more);
    } else {
        menu_build_roller(scr, opts, row, en, hint);
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
    s_menu.roller       = NULL;     /* 滚筒/滚动条指针随重建刷新（行式页保持 NULL） */
    s_menu.roller_track = NULL;
    s_menu.roller_thumb = NULL;
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

    menu_fonts_refresh();           /* chrome/滚筒都依赖字体，先于任何控件构建 */

    switch (s_menu.page) {
    case MENU_PAGE_ROOT: {
        /* 滚筒选择页：根页 7 项（Exit 行=收菜单；BGM 行=呼出半屏控制条），
         * Back 按钮=退出菜单（与 Exit 项并存） */
        menu_chrome_build(scr, "MiniPet");
        static char opts[MENU_ROWS_MAX * 48];   /* 仅渲染任务调用，静态免大栈 */
        static bool en[MENU_ROWS_MAX];
        int row = 0;
        size_t o = 0;
        for (int i = 0; i < ROOT_CNT && i < MENU_ROWS_MAX; i++) {
            en[i] = true;
            o += (size_t)snprintf(opts + o, sizeof(opts) - o, "%s%s",
                                  row ? "\n" : "", ROOT_ROWS[i].label);
            row++;
        }
        s_menu.row_cnt = row;
        menu_build_roller(scr, opts, row, en, "UP:MID  DOWN:BOT  LONG:EXIT");
        break;
    }
    case MENU_PAGE_MAPS:
        menu_maps_collect();
        menu_chrome_build(scr, "Maps");
        menu_build_selection_page(scr, "NO MAP IN LOCAL MANIFEST",
                                  "TAP: SWITCH/DL   [v] CACHED");
        break;
    case MENU_PAGE_PAPERDOLL:
        menu_parts_collect();
        menu_chrome_build(scr, "Paperdoll");
        menu_build_selection_page(scr, "NO OUTFIT PACK (SYNC NEEDED)",
                                  "TAP: WEAR PARTS  [v] CACHED");
        break;
    case MENU_PAGE_ACTIONS:
        menu_actions_collect();
        menu_chrome_build(scr, "Actions");
        menu_build_selection_page(scr, "NO ACTION PACK (SYNC NEEDED)",
                                  "TAP: PLAY  [v] CACHED  [ ] NO PACK");
        break;
    case MENU_PAGE_NPC:
        /* 行式页（NPC 缓存用途清单，非"选择"语义），只统一羊皮纸配色 */
        menu_npc_collect();
        menu_chrome_build(scr, "Monsters");
        menu_build_list(scr, "NO NPC ASSET (SERVER PUSH)",
                        "NPC PACKS: TAP TO CACHE (NO RENDER)");
        break;
    case MENU_PAGE_BGM: {
        /* 行式页（控制项非"选择"语义），配色统一羊皮纸；
         * 状态行=底部骨架行（tick 500ms 刷新 BGM 态） */
        menu_chrome_build(scr, "BGM");
        menu_bgm_status_refresh();
        menu_add_row(scr, 0, "Play / Pause", 72, 44, true);
        menu_add_row(scr, 1, "Prev",         122, 44, true);
        menu_add_row(scr, 2, "Next",         172, 44, true);
        menu_add_row(scr, 3, "Vol -",        222, 44, true);
        menu_add_row(scr, 4, "Vol +",        272, 44, true);
        /* 切源行：置灰跟随 bgm_source_greyed()（该源整体不可用 → 不可点） */
        {
            bool wz_grey = bgm_source_greyed(MP_BGM_SRC_WZ);
            bool qq_grey = bgm_source_greyed(MP_BGM_SRC_QQ);
            char src_label[48];
            snprintf(src_label, sizeof(src_label), "Source: %s%s",
                     bgm_source_name(), (wz_grey && qq_grey) ? " (BOTH DOWN)" : "");
            /* 两个源都不可用才整体置灰；否则可点切换 */
            menu_add_row(scr, 5, src_label, 322, 44, !(wz_grey && qq_grey));
        }
        menu_add_row(scr, 6, "< Back",       372, 44, true);
        s_menu.row_cnt = 7;
        menu_add_hint(scr, "TOUCH OR TOP KEY");
        break;
    }

    case MENU_PAGE_RESET: {
        /* 【E14】免插线重配网：清配网凭据（WiFi + 服务器地址）后重启 →
         * 设备进 SoftAP portal（MiniPet-XXXX）。行式确认页 + 羊皮纸配色 */
        menu_chrome_build(scr, "Reset WiFi");
        menu_add_row(scr, 0, "CONFIRM RESET", 170, 52, true);
        menu_add_row(scr, 1, "< Back",        240, 52, true);
        s_menu.row_cnt = 2;
        menu_add_hint(scr, "CLEARS WIFI + SERVER, THEN REBOOT");
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

    ESP_LOGI(TAG, "menu_rebuild: page=%d rows=%d items=%d offline=%d roller=%d widgets=%u",
             (int)s_menu.page, s_menu.row_cnt, s_menu.item_cnt, (int)s_menu.offline,
             (int)(s_menu.roller != NULL),
             (unsigned)lv_obj_get_child_count(scr));
    lv_obj_invalidate(scr);            /* DIRECT 模式强制整屏重绘入 menu_buf */
}

/* 高亮跟随 sel（菜单 tick；sel 变化才动，避免无谓失效区）。
 * 滚筒页：选中行视觉由 LV_PART_SELECTED 原生绘制，这里只做
 *   ① 侧键 nav 改写的 sel → lv_roller_set_selected(LV_ANIM_ON) 平滑滚动
 *     （触摸路径 VALUE_CHANGED 已把 sel 同步，此分支自然旁路）
 *   ② 自绘滚动条 thumb 按选中比例移动（INFINITE 回绕时回到端点，预期行为）
 *   ③ 底部状态行回显当前选中
 * 行式页：逐行贴高亮 + "> " 文字光标（样式之外的光标兜底通道） */
static void menu_apply_selection(void)
{
    if (s_menu.sel_applied == s_menu.sel) return;

    if (s_menu.roller) {
        if (s_menu.sel < 0 || s_menu.sel >= s_menu.row_cnt) s_menu.sel = 0;
        if ((int)lv_roller_get_selected(s_menu.roller) != s_menu.sel)
            lv_roller_set_selected(s_menu.roller, (uint32_t)s_menu.sel, LV_ANIM_ON);

        if (s_menu.roller_thumb) {
            int th = MENU_ROLLER_H * MENU_ROLLER_VIS /
                     (s_menu.row_cnt > 0 ? s_menu.row_cnt : 1);
            if (th < 16) th = 16;
            if (th > MENU_ROLLER_H) th = MENU_ROLLER_H;
            lv_obj_set_height(s_menu.roller_thumb, th);
            lv_obj_set_y(s_menu.roller_thumb,
                         (MENU_ROLLER_H - th) * s_menu.sel /
                         (s_menu.row_cnt > 1 ? s_menu.row_cnt - 1 : 1));
        }
        if (s_menu.status_label) {
            char opt[48];
            lv_roller_get_selected_str(s_menu.roller, opt, sizeof(opt));
            lv_label_set_text_fmt(s_menu.status_label, "SEL: %s", opt);
        }
        s_menu.sel_applied = s_menu.sel;
        return;
    }

    int styled = 0;
    for (int i = 0; i < s_menu.row_cnt && i < MENU_ROWS_MAX; i++) {
        if (!s_menu.rows[i]) continue;
        if (!s_menu.row_enabled[i]) {
            menu_style_row_disabled(s_menu.rows[i], i == s_menu.sel);  /* 光标可见 */
            styled++;
            continue;
        }
        menu_style_row(s_menu.rows[i], i == s_menu.sel);
        styled++;
    }
    s_menu.sel_applied = s_menu.sel;
    /* 【文字光标 2026-09-27】样式高亮在真机上用户判读不出（"高亮框不跟着走"），
     * 加一条不依赖样式的光标通道：选中行标签前缀 "> "，未选中行去掉前缀。
     * 这样即便配色/无效区有问题，光标移动也一定看得见。 */
    for (int i = 0; i < s_menu.row_cnt && i < MENU_ROWS_MAX; i++) {
        lv_obj_t *btn = s_menu.rows[i];
        if (!btn) continue;
        lv_obj_t *lb = lv_obj_get_child(btn, 0);
        if (!lb || !lv_obj_has_class(lb, &lv_label_class)) continue;
        const char *cur = lv_label_get_text(lb);
        if (!cur) continue;
        bool want = (i == s_menu.sel);
        bool have = (cur[0] == '>' && cur[1] == ' ');
        if (want == have) continue;                 /* 已一致，避免每 tick 重设文本 */
        char buf[64];
        if (want) snprintf(buf, sizeof(buf), "> %.58s", cur);
        else      snprintf(buf, sizeof(buf), "%.60s", cur + 2);
        lv_label_set_text(lb, buf);
        lv_obj_invalidate(btn);
    }
    /* 【取证探针 2026-09-27】把"贴到第几行"变成日志事实，便于与用户观察对照 */
    (void)styled;   /* 诊断探针已移除（内部堆/串口带宽优先） */
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
    /* 【重影修复 2026-09-27】DIRECT 模式下 LVGL 只重绘"失效区"，而菜单里
     * 行内文字/高亮底/提示行的失效矩形由 LVGL 自行推导——真机截图可见文字
     * 叠影（旧像素没被覆盖）。合成器每帧 memcpy 整个 menu_buf 上屏，
     * 因此只要 menu_buf 里留了脏像素就会**稳定复现**。这里在 100ms 节拍上
     * 强制整屏失效一次：LVGL 会重绘全屏进 menu_buf，脏像素每 100ms 清一遍。
     * 代价：菜单态 10fps 全屏重绘（LVGL 官方在 DIRECT 模式下的推荐做法），
     * 菜单是静态界面，实测无卡顿。 */
    {
        lv_obj_t *scr = lv_screen_active();
        if (scr) lv_obj_invalidate(scr);
    }
    if (++s_menu.bgm_refr_div >= 5) {   /* 100ms×5 = 500ms */
        s_menu.bgm_refr_div = 0;
        /* 状态行语义分页：滚筒页=SEL 回显（menu_apply_selection 维护），
         * 仅 BGM 页刷 BGM 播放态，避免 500ms 覆盖滚筒页的选中回显 */
        if (s_menu.page == MENU_PAGE_BGM) menu_bgm_status_refresh();
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
    s_menu.roller = NULL;             /* 滚筒/滚动条指针随 rebuild 重建（防悬挂引用） */
    s_menu.roller_track = NULL;
    s_menu.roller_thumb = NULL;
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
