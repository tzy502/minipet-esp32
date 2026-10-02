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
static int  utf8_step(const char *s, uint32_t *cp);   /* 定义在气泡段；菜单缺字判定复用 */

/* ══ 【契约 §3.2 相机接口】═══════════════════════════════════════════════════
 * render_cam_supported/range/set/get/center + render_ground_screen_y 的声明在
 * compositor.h（compositor.h 明示"相机 UX 层（F3）请 #include compositor.h，
 * render.h 不转出本组接口"）；本文件是 §3.3 相机 UX 层，直接消费这组接口。
 * 命名与 render.h 的 render_* 同前缀，勿与 state_machine.c 的 NVS（sm_cam_*）混淆。 */
#include "compositor.h"

/* state_machine.c 提供的 NVS per-map 相机持久化 + 横幅收尾（契约 §3.3 的"NVS"）
 * + 相机入口专用内部通道（MENU→POKER，不吃 MENU_KEY 的 400ms 限速，见其注释） */
extern bool sm_cam_nvs_get(const char *key, int32_t *x, int32_t *y);
extern bool sm_cam_nvs_set(const char *key, int32_t x, int32_t y);
extern bool sm_cam_nvs_erase(const char *key);
extern void state_machine_banner_restore(void);
extern bool state_machine_cam_enter_poker(void);

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
    MENU_PAGE_MAP_FN,        /* 地图功能子页：①设为背景 ②改相机 ③删除 ④返回列表（§4.1） */
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
#define MENU_MAP_FN_CNT 4       /* 地图功能子页选项数（3 功能 + 返回地图列表） */
#define MENU_STATUS_FLASH_MS 2500 /* 状态行一次性提示驻留时长（动作反馈/相机占位） */

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
    /* 落盘后要切换到的实体（MP_CMD_SET_ENTITY 专用："mob:100100"/"npc:2100000"）。
     * 为什么不能复用 dl_hash：实体指令的载荷是 entity 字符串，而下载请求要的是
     * PARTS 包 hash —— 两者不是一回事（旧实现"NPC 页只下载不派发"正是缺这个字段）。 */
    char          dl_entity[40];
    mp_cmd_type_t dl_cmd;       /* 落盘后要发的指令（MP_CMD_NONE = 只下载） */
    int64_t       dl_deadline_ms;
    char          hint_once[40];/* 一次性提示（下载完成 → 重建后显示一格） */
    /* 下载完成后回哪一页：列表页=原地重建（保持选中），地图功能子页=回地图列表 */
    menu_page_t   dl_back;
    bool          dl_goto_back;

    /* 地图功能子页（MENU_PAGE_MAP_FN）目标地图：进子页时快照，独立于列表重建
     * （隐藏后列表条目会消失，子页操作对象不能跟着丢） */
    char          fn_hash[20];  /* 目标图内容 hash（MP_CMD_* 的 s 通道） */
    char          fn_key[16];   /* per-map 键（asset_dl_map_key：map_id 或 h+hash14） */
    char          fn_label[32]; /* 目标图显示名（标题/状态行回显） */

    /* 状态行一次性提示（动作反馈/占位提示）：到期前优先于「选中：」回显 */
    char          flash_msg[48];
    int64_t       flash_until_ms;
    bool          flash_shown;  /* 本次 tick 状态行是否正显示提示（到期要还原） */

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
 *   Maps     子页：asset_dl_bgmap_list() 真实 BGMAP 条目（hash + label）
 *            label 口径见 asset_dl.h：真中文名原样（服务端 L1 修好后）、
 *            ^map_\\d+$ 剥前缀显示数字；缺字（烘焙子集覆盖不到）→ 回落数字键。
 *            [v]=已缓存 [ ]=未缓存 [x]=未缓存且离线（T3 拒绝确认）。
 *            **点条目 = 进地图功能子页**（§4.1，不再立即切图）：
 *              ①选择此地图为背景 = 原切图行为（含 T4 未缓存先下载再派发）
 *              ②修改当前地图的摄像头 = 相机 agent 接手点
 *                （menu_map_fn_camera_hook，占位只提示"摄像机开发中"）
 *              ③删除此地图 = §4.4 本地隐藏标识（asset_dl NVS per-map；
 *                隐藏当前图/全部图 → 立即切回默认图；服务端重推自动解除）
 *              ④返回地图列表 = 返回上级（menu_parent_page；底键长按仍=退出菜单）
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
 * 反馈通道：底部状态行既有「选中：xxx」回显，另有 menu_status_flash() 一次性
 * 提示（2.5s，压过选中回显后自动还原）——子页动作结果/相机占位/下载进度可见。
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
extern const lv_font_t menu_font_cn;   /* 菜单中文字体（烘焙子集，见 render/font_cn/） */

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
    /* 【中文化 2026-09-29】菜单文字统一用烘焙的 CJK 子集字体（22px，
     * 覆盖菜单全部汉字 + ASCII）；f_small 仅用于纯英文提示行 */
    if (1) {
        s_menu.f_title = &menu_font_cn;
        s_menu.f_item  = &menu_font_cn;
    }
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
    /* 【诊断 2026-09-29】按下沿必打 + 拖动 1/8 采样，定位触摸断在哪层 */
    static bool last_p; static int skip;
    if (pressed != last_p) {
        ESP_LOGW("mtouch", "feed 沿 x=%d y=%d pressed=%d menu_mode=%d", x, y, pressed, (int)s_br.menu_mode);
        last_p = pressed; skip = 0;
    } else if (pressed && ++skip >= 8) {
        skip = 0;
        ESP_LOGI("mtouch", "feed 拖 x=%d y=%d", x, y);
    }
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

/* 菜单字体（menu_font_cn = regen.sh 按源码用字烘焙的 CJK 子集）覆盖判定：
 * UTF-8 逐码点取字形，任一码点无字形即 false。 */
static bool menu_font_covers(const char *s)
{
    const lv_font_t *f = s_menu.f_item ? s_menu.f_item : &menu_font_cn;
    const char *p = s;
    while (p && *p) {
        uint32_t cp = 0;
        int step = utf8_step(p, &cp);
        if (step <= 0) { p++; continue; }      /* 非法字节：不判缺字，交 LVGL 原样处理 */
        lv_font_glyph_dsc_t dsc;
        if (!lv_font_get_glyph_dsc(f, &dsc, cp, 0)) return false;
        p += step;
    }
    return true;
}

/* 【动态中文名缺字兜底】asset_dl 已放行 UTF-8 label 并把它落库（服务端 L1 修好后
 * 地图名就是中文原名）。烘焙子集覆盖不到该名字时回落 fallback（地图=数字 id、
 * 装扮/怪物=entity 或 hash 前 8 位）——宁可显示编号，也不出整行空白/方块。
 * F2 TF 全量字库上线后覆盖判定自然放行，中文名无需再改代码。 */
static void menu_label_font_safe(const char *label, const char *fallback,
                                 char *out, size_t cap)
{
    if (!label || !label[0]) {
        strlcpy(out, (fallback && fallback[0]) ? fallback : "?", cap);
        return;
    }
    if (menu_font_covers(label)) {
        strlcpy(out, label, cap);
        return;
    }
    strlcpy(out, (fallback && fallback[0]) ? fallback : "?", cap);
    ESP_LOGW(TAG, "menu: label \"%.24s\" 烘焙子集缺字 → 回落显示 %s", label, out);
}

/* Maps 页：本地清单里的 BGMAP 条目（hash + label + 缓存标记）
 * label 由 asset_dl 给：真中文名原样、^map_\\d+$ 已剥前缀显示数字；
 * 用户隐藏（"删除"）的图已被 asset_dl 过滤，不在此列。 */
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
        /* 缺字兜底回落 per-map 键（= 数字 id）；"map_" 前缀已在 asset_dl 剥除 */
        char key[16];
        if (!asset_dl_map_key(hashes[i], key, sizeof(key))) snprintf(key, sizeof(key), "%.8s", hashes[i]);
        menu_label_font_safe(labels[i], key, s_menu.items[i].label,
                             sizeof(s_menu.items[i].label));
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
        /* 服务端装扮名若为中文且烘焙子集缺字 → 回落 hash 前 8 位（asset_dl 原 ③ 兜底），
         * 保持既有"每行都有可读文本"的观感，不出整行空白 */
        char fb[12];
        snprintf(fb, sizeof(fb), "%.8s", hashes[i]);
        menu_label_font_safe(labels[i], fb, s_menu.items[i].label,
                             sizeof(s_menu.items[i].label));
        s_menu.items[i].entity[0] = 0;
        s_menu.items[i].action[0] = 0;
        s_menu.items[i].cached = cached[i];
    }
    menu_items_cached_first();
}

/* 怪物页（原 Monsters/NPC）：manifest 里的**实体形象**条目 —— selector=="mob"
 * （Mob.wz，Web 怪物 tab 📤 推送）与 "npc"（Npc.wz）都在列，按 entity 去重。
 * 【2026-10-02 从"只缓存"升级为"可用"】点选 = MP_CMD_SET_ENTITY → 状态机把宠物
 * 形象切成该实体（PARTS + 默认动作 LAYOUT，与纸娃娃同一条渲染通道）。
 * 未缓存条目仍走"先下载再派发"（menu_dispatch_entity）。 */
static void menu_entity_collect(void)
{
    char entities[MENU_COLLECT_MAX][40] = { { 0 } };
    char hashes[MENU_COLLECT_MAX][20] = { { 0 } };
    char labels[MENU_COLLECT_MAX][32] = { { 0 } };
    bool cached[MENU_COLLECT_MAX] = { false };
    int n = asset_dl_entity_list(entities, hashes, labels, cached, MENU_COLLECT_MAX);
    if (n < 0) n = 0;
    s_menu.truncated = (n > MENU_LIST_MAX);
    if (n > MENU_LIST_MAX) n = MENU_LIST_MAX;
    s_menu.item_cnt = n;
    for (int i = 0; i < n; i++) {
        strlcpy(s_menu.items[i].hash, hashes[i], sizeof(s_menu.items[i].hash));
        /* 同装扮页：中文名缺字 → 回落 entity（如 "npc:2100000"，asset_dl 原 ② 兜底） */
        menu_label_font_safe(labels[i], entities[i], s_menu.items[i].label,
                             sizeof(s_menu.items[i].label));
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
    { "地图",     MENU_PAGE_MAPS      },
    { "纸娃娃",   MENU_PAGE_PAPERDOLL },
    { "动作",     MENU_PAGE_ACTIONS   },
    { "怪物",     MENU_PAGE_NPC       },
    { "BGM",       MENU_PAGE_BGM       },
    { "重置WiFi", MENU_PAGE_RESET     },   /* E14：清凭据重配网（无需连电脑擦 NVS） */
    { "退出",     -1                  },   /* -1 = 收菜单（状态机 MENU_KEY 通道） */
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

/* 【返回上级 2026-10-01】上级页映射：地图功能子页 → 地图列表；其余子页 → 根页。
 * 「返回」语义统一走这里（滚筒末行 < 返回 / 返回地图列表 / 底部 Back 按钮），
 * 底键长按=退出整个菜单的既有语义不变。 */
static menu_page_t menu_parent_page(menu_page_t p)
{
    return (p == MENU_PAGE_MAP_FN) ? MENU_PAGE_MAPS : MENU_PAGE_ROOT;
}

/* 状态行一次性提示（子页动作反馈/相机占位/下载进度）：MENU_STATUS_FLASH_MS 内
 * 压过「选中：xxx」回显，到期由 menu_apply_selection 自动还原。
 * 状态行是各页 chrome 都有的可见控件（hint_label 历来为 NULL，旧提示肉眼看
 * 不到——此处用它做可见反馈通道）。 */
static void menu_status_flash(const char *text)
{
    strlcpy(s_menu.flash_msg, text, sizeof(s_menu.flash_msg));
    s_menu.flash_until_ms = mp_now_ms() + MENU_STATUS_FLASH_MS;
    s_menu.flash_shown = true;
    if (s_menu.status_label) lv_label_set_text(s_menu.status_label, s_menu.flash_msg);
    ESP_LOGI(TAG, "menu: 状态行提示：%s", s_menu.flash_msg);
}

static bool menu_status_flash_active(void)
{
    return (s_menu.flash_msg[0] && mp_now_ms() < s_menu.flash_until_ms);
}

/* T4：条目激活（hash 版）。列表页 items[idx] 与地图功能子页的 fn_hash 共用同一
 * 条路径（避免两份下载/派发逻辑漂移）。
 *   已缓存 → 直接 post 既有指令（state_machine 查路径/条带后落地）→ 回 back 页；
 *   未缓存 → asset_dl_request_one(hash) 入队 → 提示「下载中」→ 菜单 tick
 *            轮询 asset_dl_file_cached() 落盘 → 成功再 post 同一指令 + 回 back 页。
 * cmd == MP_CMD_NONE = 只下载不派发（NPC 页：固件无 NPC 渲染通道）。 */
static void menu_dispatch_hash(const char *hash, const char *label, bool cached,
                               int log_idx, mp_cmd_type_t cmd, menu_page_t back)
{
    /* 【派发探针 2026-09-27】用户报"无论怎么点选中的都是第一个 map"：
     * 记录 行号→hash→cmd→cached，用于区分「选中索引没生效」与「派发静默失败」。 */
    ESP_LOGI(TAG, "menu: activate idx=%d cmd=%d cached=%d hash=%.16s label=%.24s back=%d",
             log_idx, (int)cmd, (int)cached, hash ? hash : "",
             label ? label : "", (int)back);

    if (cached) {
        if (cmd != MP_CMD_NONE) {
            mp_cmd_t c = { .type = cmd };
            strlcpy(c.s, hash, sizeof(c.s));
            mp_post_cmd(&c);
            ESP_LOGI(TAG, "menu: 已缓存直接派发 cmd=%d hash=%.16s", (int)cmd, hash);
        } else {
            ESP_LOGI(TAG, "menu: 已缓存（NPC 页无渲染通道，不派发）%.16s", hash);
        }
        menu_goto(back);
        return;
    }

    /* 未缓存：离线已被置灰（双保险再挡一次）；在线则拉包 */
    if (state_machine_offline_mode()) {
        menu_hint_set("OFFLINE: NOT CACHED");
        menu_status_flash("离线且未缓存");
        ESP_LOGW(TAG, "menu: 离线且未缓存，拒绝下载 %.16s", hash);
        return;
    }
    if (s_menu.dl_active) {                    /* 同屏只挂一个下载：避免请求互相覆盖 */
        menu_hint_set("BUSY: DOWNLOAD IN PROGRESS");
        menu_status_flash("已有下载在途");
        ESP_LOGW(TAG, "menu: 已有下载在途，忽略 %.16s", hash);
        return;
    }
    if (!asset_dl_request_one(hash)) {
        menu_hint_set("REQ FAILED (NOT IN MANIFEST)");
        menu_status_flash("下载请求被拒");
        ESP_LOGW(TAG, "menu: request_one 被拒 %.16s", hash);
        return;
    }
    s_menu.dl_active     = true;
    s_menu.dl_cmd        = cmd;
    strlcpy(s_menu.dl_hash, hash, sizeof(s_menu.dl_hash));
    s_menu.dl_deadline_ms = mp_now_ms() + MENU_DL_TIMEOUT_MS;
    s_menu.dl_back       = back;
    s_menu.dl_goto_back  = (back != s_menu.page);
    if (s_menu.hint_label) {
        lv_label_set_text_fmt(s_menu.hint_label, "DOWNLOADING %.8s ... WAIT",
                              s_menu.dl_hash);
    }
    menu_status_flash("下载中，请稍候");
    ESP_LOGI(TAG, "menu: 下载中 %.16s → cmd=%d（完成后回页 %d）",
             s_menu.dl_hash, (int)cmd, (int)back);
}

/* 列表页条目激活（Maps/Paperdoll 执行后回根页=既有行为） */
static void menu_activate_item(int idx, mp_cmd_type_t cmd)
{
    const menu_item_t *it = &s_menu.items[idx];
    menu_dispatch_hash(it->hash, it->label, it->cached, idx, cmd, MENU_PAGE_ROOT);
}

/* 怪物页条目激活：点选 = 把宠物形象切成该实体（MP_CMD_SET_ENTITY）。
 * 与 menu_dispatch_hash 的分工：下载要的是 PARTS 包 hash，而指令载荷要的是
 * entity 字符串 —— 故多带一个 entity；其余（离线置灰/在途去重/超时）同一条路径。 */
static void menu_dispatch_entity(const char *entity, const char *hash, const char *label,
                                 bool cached, int log_idx)
{
    ESP_LOGI(TAG, "menu: 实体激活 idx=%d entity=%s cached=%d label=%.24s",
             log_idx, entity ? entity : "", (int)cached, label ? label : "");
    if (!entity || !entity[0]) return;

    if (cached) {
        mp_cmd_t c = { .type = MP_CMD_SET_ENTITY };
        strlcpy(c.s, entity, sizeof(c.s));
        mp_post_cmd(&c);
        menu_goto(MENU_PAGE_ROOT);
        return;
    }
    if (state_machine_offline_mode()) {
        menu_hint_set("OFFLINE: NOT CACHED");
        menu_status_flash("离线且未缓存");
        return;
    }
    if (s_menu.dl_active) {
        menu_hint_set("BUSY: DOWNLOAD IN PROGRESS");
        menu_status_flash("已有下载在途");
        return;
    }
    if (!hash || !hash[0] || !asset_dl_request_one(hash)) {
        menu_hint_set("REQ FAILED (NOT IN MANIFEST)");
        menu_status_flash("下载请求被拒");
        ESP_LOGW(TAG, "menu: 实体下载请求被拒 entity=%s hash=%.16s", entity, hash ? hash : "");
        return;
    }
    s_menu.dl_active     = true;
    s_menu.dl_cmd        = MP_CMD_SET_ENTITY;
    strlcpy(s_menu.dl_hash, hash, sizeof(s_menu.dl_hash));
    strlcpy(s_menu.dl_entity, entity, sizeof(s_menu.dl_entity));
    s_menu.dl_deadline_ms = mp_now_ms() + MENU_DL_TIMEOUT_MS;
    s_menu.dl_back       = MENU_PAGE_ROOT;
    s_menu.dl_goto_back  = true;
    if (s_menu.hint_label) {
        lv_label_set_text_fmt(s_menu.hint_label, "DOWNLOADING %.8s ... WAIT", hash);
    }
    menu_status_flash("下载中，请稍候");
    ESP_LOGI(TAG, "menu: 实体下载中 %s（hash %.16s）→ 完成后切换形象", entity, hash);
}

/* ---------------- 地图功能子页（§4.1：三个功能 + 返回地图列表） ----------------
 * 进子页不再"立即切图"（旧行为）——切图降为子页①，②相机入口占位、③删除=隐藏。
 * 目标地图快照进 s_menu.fn_*（列表可能因隐藏而变短，子页操作对象不能丢）。 */
static void menu_map_fn_open(int idx)
{
    const menu_item_t *it = &s_menu.items[idx];
    strlcpy(s_menu.fn_hash, it->hash, sizeof(s_menu.fn_hash));
    strlcpy(s_menu.fn_label, it->label, sizeof(s_menu.fn_label));
    if (!asset_dl_map_key(it->hash, s_menu.fn_key, sizeof(s_menu.fn_key))) {
        snprintf(s_menu.fn_key, sizeof(s_menu.fn_key), "%.8s", it->hash);
    }
    ESP_LOGI(TAG, "menu: 进地图功能子页 hash=%.16s key=%s label=%.24s cached=%d",
             s_menu.fn_hash, s_menu.fn_key, s_menu.fn_label, (int)it->cached);
    /* 目标图回显（状态行提示）：动态中文名可能缺字，不进标题栏防方块 */
    if (s_menu.fn_label[0]) {
        char t[48];
        snprintf(t, sizeof(t), "目标地图：%s", s_menu.fn_label);
        menu_status_flash(t);
    }
    menu_goto(MENU_PAGE_MAP_FN);
}

/* 子页①选择此地图为背景 = 原"点列表项立即切图"的行为（MP_CMD_SET_MAP +
 * asset_dl_touch LRU 在 state_machine dispatch_map 内），完成后回地图列表。 */
static void menu_map_fn_background(void)
{
    bool cached = asset_dl_file_cached(s_menu.fn_hash);
    ESP_LOGI(TAG, "menu: 子页①选择背景 hash=%.16s cached=%d", s_menu.fn_hash, (int)cached);
    menu_dispatch_hash(s_menu.fn_hash, s_menu.fn_label, cached, -1,
                       MP_CMD_SET_MAP, MENU_PAGE_MAPS);
}

/* ============================================================================
 * 【契约 §3.3 相机 UX 层】子页②：修改当前地图的摄像头 → 相机调参态
 * ----------------------------------------------------------------------------
 * 状态机与任务归属（跨任务纪律见本文件头）：
 *   ① 进入（**渲染任务**·菜单 100ms tick 内）：menu_map_fn_camera_hook()
 *        · 校验 render_cam_supported()（旧窗口包 → 状态行提示，不进调参态）
 *        · 校验目标图 = 当前已渲染的图（render_cam_supported 只反映已装载图）
 *        · 快照进入前相机（取消/中断的复原基准）→ 状态行提示
 *        · state_machine_cam_enter_poker()（MENU→POKER 内部通道，**不吃** MENU_KEY 的
 *          400ms 限速）→ 迁到 POKER 后才置 s_cam.on（顺序红线，见函数内注释）
 *        · 入队常驻横幅（POKER 态唯一文字通道 = 5x7 ASCII 横幅）
 *   ② 平移（**input 任务**·touch_tick）：bridge_cam_adjust_drag_begin/move/end
 *        · 拖动 = 相机跟手：屏位移 Δ → 相机反向 Δ/2（屏 2× = 世界 1×）
 *        · 夹取在 render_cam_set 内部（§3.2，越界永不出图）；本层只算目标值
 *        · 66ms 节流（与宠物拖拽同口径）；抬指补最后一帧（节流窗内位移不丢）
 *   ③ 确认/取消（**input 任务**·key0_tick/key_tick）：
 *        · 中键短按 / 顶键短按 = 确认 → NVS 写入（ns=cam）+ 重合成
 *        · 中键长按 / 顶键长按 = 取消 → 复原进入前相机
 *   ④ 收尾（确认或取消都走）：宠物水平偏移归零（render_set_drag_off(0) = 锚点回屏心 x）
 *        + 重新派发当前图（→ render_set_map → 既有 ent_stand_on_ground_locked 站位链）
 *        + **置 settle_pending**：等重派发落地、站位链跑完、NVS 相机重新应用之后，
 *          才结算「脚踩地面线」（bridge_cam_settle_after_reload → cam_settle_on_ground）——
 *          这就是契约 §5.2「固定回归屏幕正中间、脚踩该处地面线（x=屏心）」的落点：
 *          x=屏心（drag_x=0）+ 脚底=render_ground_screen_y(屏心 x)（越界/无表 → 不落，
 *          沿用站位链口径=锚点回屏心）。实现细节与两种口径的辨析见 cam_settle_on_ground()。
 *   ⑤ 兜底（**input 任务**主循环·bridge_cam_adjust_poll）：状态机离开
 *        POKER/OFFLINE（待机时钟/OTA/配网…）→ 未确认即取消并复原相机；
 *        另兼 settle 超时兜底（重投一次地图 / 二次超时明确放弃并报 ERROR）
 *
 * 宠物策略：**冻结**（不隐藏）——调参期整段触摸手势归相机，宠物既不响应拖动
 *   也不响应抚摸/长按控制条，"拖动=拖宠物 vs 拖相机"的歧义在输入路由上一刀切断；
 *   相机平移只动背景与视差条带，宠物当前站位保持不动，收尾时统一回屏心
 *   （需求 §5.2「屏中回归制」）。选冻结而非隐藏的原因：渲染层没有"隐藏实体"的
 *   公开接口（compositor.c 由另一 agent 独占，本层不得改），冻结零依赖且可逆。
 *
 * 可见反馈：POKER 态文字通道只有 5x7 ASCII 横幅（render.h §横幅，中文经
 *   compose 会被替换成 '?'）；菜单状态行的中文受烘焙子集（182 字）限制——
 *   「相机调整中：拖动平移 / 中键短按确认 · 长按取消」里的 调/整/短/按/长/移/平/
 *   ：/· 均不在子集内（硬写会渲染成空白），故屏上用全字形覆盖的短句 + ASCII
 *   横幅（语义完全等价），完整中文串打在串口日志里可查。若要上原串，需跑
 *   main/render/font_cn/regen.sh 重烘子集（不在本 agent 的 3 文件边界内）。
 * ========================================================================== */

#define CAM_PAN_SCALE        2      /* 屏 = 世界 1x ×2（compositor.h RC_SCALE，勿改口径） */
#define CAM_PAN_THROTTLE_MS  66     /* 拖动节流：与宠物拖拽同口径（15fps 下发） */
#define CAM_PAN_SNAP_PX      8      /* 【极限档吸附】拖动期相机位移吸附到 8 世界 px 的整数倍
                                     * （= 屏上 16px 步进）：一次整屏重合成才换一次画面，
                                     * 避免"每个世界 px 都整屏重合成"把 15Hz 节流吃掉。
                                     * 松手（drag_end）时**不带吸附**再下发一次精确值，
                                     * 所以最终保存的相机仍是用户手指的精确位置。 */
#define CAM_SETTLE_TIMEOUT_MS 3000  /* 「脚踩地面线」结算等待重派发落地的上限（超时重投一次） */
/* 【调参横幅 2026-10-01】用户要求"内容明确告诉用户：调整中：拖动移动 · 顶键短按保存 ·
 * 长按取消"。5x7 横幅字库只有 ASCII（中文会被画成 '?'），故用等价英文；完整中文串
 * 同时打在串口日志与菜单状态行（后者走烘焙中文子集）。37 字符 ×12px = 444 ≤ 480。 */
#define CAM_BANNER_HINT      "CAM: DRAG=MOVE TAP=SAVE HOLD=CANCEL"
#define CAM_BANNER_SAVED     "CAM SAVED"
#define CAM_BANNER_CANCELED  "CAM CANCELED"
/* 收尾重派发地图（>5s 装载）期间的 loading 横幅：这段时间按键/触摸被丢弃（见
 * bridge_cam_busy 与 input_dispatch 的忙判定）。 */
#define CAM_BANNER_LOADING   "CAM SAVING - PLEASE WAIT"
/* 状态行中文提示：只用烘焙子集内字形（见上"可见反馈"说明） */
#define CAM_FLASH_HINT       "CAM: DRAG=MOVE TAP=SAVE HOLD=CANCEL"
#define CAM_FLASH_NO_FULLMAP "此图相机不可用 (FULLMAP PKG)"
#define CAM_FLASH_NEED_BG    "请选择此图为背景"

static struct {
    volatile bool on;           /* 调参态激活（渲染任务写、input 任务读/清，单字旗标） */
    char     hash[20];          /* 目标图内容 hash（收尾重派发用） */
    char     key[16];           /* per-map NVS 键（asset_dl_map_key 口径，≤15 字符） */
    int32_t  cx0, cy0;          /* 进入前相机（世界 px）：取消/中断复原基准 */
    int32_t  dx0, dy0;          /* 本次拖拽起点的相机（跟手换算基准） */
    int32_t  fx0, fy0;          /* 本次拖拽起点的手指位置（屏幕 px） */
    int32_t  lx, ly;            /* 最近一次手指位置（抬指补最后一帧） */
    int64_t  last_apply_ms;     /* 节流时刻（CAM_PAN_THROTTLE_MS） */
    bool     dragging;          /* 本次手势是否在拖动中 */
    /* 收尾「脚踩地面线」结算（§5.2）：站位链只在 render_set_map 里跑，故结算必须等
     * 重派发**落地**——这里只置 pending，结算点见 bridge_cam_settle_after_reload() */
    bool     settle_pending;
    bool     settle_retried;    /* 兜底只重投一次地图 */
    int64_t  settle_by_ms;      /* 结算 deadline（超时重投/放弃） */
} s_cam;

/* 跟手换算（纯函数，无副作用）：手指位移 Δ → 相机反向 Δ/2。
 * 不在此夹取——越界夹取由 render_cam_set 内部按 §3.2 负责（"永不出图边界"）。 */
static void cam_pan_target(int32_t dx0, int32_t dy0, int32_t fx0, int32_t fy0,
                           int32_t sx, int32_t sy, int32_t *wx, int32_t *wy)
{
    *wx = dx0 - (sx - fx0) / CAM_PAN_SCALE;
    *wy = dy0 - (sy - fy0) / CAM_PAN_SCALE;
}

/* 吸附到 CAM_PAN_SNAP_PX 的整数倍（四舍五入；世界 px）。
 * 只在拖动**进行中**用（极限档），松手那次不带吸附 → 精确落点。 */
static int32_t cam_snap_world(int32_t v)
{
    const int32_t h = CAM_PAN_SNAP_PX / 2;
    if (v >= 0) return ((v + h) / CAM_PAN_SNAP_PX) * CAM_PAN_SNAP_PX;
    return -(((-v + h) / CAM_PAN_SNAP_PX) * CAM_PAN_SNAP_PX);
}

/* 立即下发一次相机（跟手）；记录节流时刻与最后手指位置。
 * snap=true = 拖动进行中（步长吸附 + 进/续极限档）；false = 松手补精确位置。
 * 返回 true = 相机**真的动了**（调用方据此判定"本拍有运动"并刷新交互计时）。 */
static bool cam_apply_pan(int32_t sx, int32_t sy, bool snap)
{
    int32_t wx = 0, wy = 0;
    cam_pan_target(s_cam.dx0, s_cam.dy0, s_cam.fx0, s_cam.fy0, sx, sy, &wx, &wy);
    if (snap) {
        wx = cam_snap_world(wx);
        wy = cam_snap_world(wy);
    }
    int32_t ox = 0, oy = 0, nx = 0, ny = 0;
    render_cam_get(&ox, &oy);              /* 下发前读回：判断"是否真的动了"（夹取后） */
    render_cam_set(wx, wy);
    render_cam_get(&nx, &ny);
    s_cam.last_apply_ms = mp_now_ms();
    s_cam.lx = sx;
    s_cam.ly = sy;
    if (nx == ox && ny == oy) return false;   /* 吸附后同格/已到边界：本拍无运动 */
    /* 【极限档】真的动了才进/续 level 2：拖动期只画 static 底图 + 零窗口缓存 IO，
     * 松手 200ms 后由渲染层自动回 level 1 → level 0（"松手出全图"）。 */
    /* 拖动期维持 level 1（static+tile 全渲染、仅跳条带层），不再往下推到
     * level 2（那会连 tile 都不画，用户明确要"单纯的 tile"）。 */
    render_cam_adjust_motion_notify();
    return true;
}

bool bridge_cam_adjust_active(void) { return s_cam.on; }

/* 触摸按下沿：记手指起点 + 快照当前相机（跟手基准；相机未变时 Δ=0 → 无位移） */
void bridge_cam_adjust_drag_begin(int32_t sx, int32_t sy)
{
    if (!s_cam.on) return;
    s_cam.fx0 = sx;
    s_cam.fy0 = sy;
    s_cam.lx  = sx;
    s_cam.ly  = sy;
    s_cam.dragging = true;
    s_cam.last_apply_ms = 0;                 /* 首个 move 立即生效（跟手零迟滞） */
    render_cam_get(&s_cam.dx0, &s_cam.dy0);
}

/* 拖动中：跟手平移（66ms 节流 + 极限档步长吸附）。返回 true = 本次真的下发了相机
 * （调用方据此决定是否刷新交互活动计时，避免 50Hz 无谓调用）。 */
bool bridge_cam_adjust_drag_move(int32_t sx, int32_t sy)
{
    if (!s_cam.on || !s_cam.dragging) return false;
    s_cam.lx = sx;
    s_cam.ly = sy;
    int64_t now = mp_now_ms();
    if (s_cam.last_apply_ms != 0 &&
        (now - s_cam.last_apply_ms) < CAM_PAN_THROTTLE_MS) {
        return false;
    }
    return cam_apply_pan(sx, sy, /*snap=*/true);
}

/* 抬起：补最后一帧（节流窗内松手的位移不丢；**不带吸附** = 精确落点），本次手势结束 */
void bridge_cam_adjust_drag_end(void)
{
    if (!s_cam.on) return;
    if (s_cam.dragging) {
        s_cam.dragging = false;
        cam_apply_pan(s_cam.lx, s_cam.ly, /*snap=*/false);
    }
}

/* 【忙判定】input 任务在按键/触摸入口调用：true = 丢弃本次输入（不排队、不延后）。
 * 三个来源：
 *   ① render_busy()：重活（整屏重合成/窗口缓存补读/地图装载）+ 忙尾 + 拖动极限档；
 *   ② settle_pending：相机确认/取消后的**地图重派发窗口**（>5s 装载，屏上
 *      loading 横幅" CAM SAVING - PLEASE WAIT"）——期间按键一律丢弃，用户的
 *      "多按几下"不会攒成装完地图后的一串动作。
 * ② 额外带**超时上限**：重派发若 3s 没落地（清单缺失/路径查询失败/cmd_q 满），
 *    输入必须恢复，否则"地图没装成"会连带把设备变成整段不响应（poll 侧的重投
 *    与放弃逻辑照旧，只是不再无限期扣着输入）。 */
bool bridge_cam_busy(void)
{
    if (render_busy()) return true;
    return s_cam.settle_pending && mp_now_ms() < s_cam.settle_by_ms;
}

/* ══ 【§5.2「脚踩地面线」接线】═══════════════════════════════════════════════
 * 现状能力盘点（读了 compositor.c 的站位链后下的结论）：
 *   · ent_stand_on_ground_locked()（compositor.c:742）只做一件事——把**锚点(=脚底基准)**
 *     钉到屏心（drag_y = −(CENTER_OFF_Y + base_wy×2)），**完全不看地面表**；
 *     即"屏中回归制"只兑现了"屏中"，`ground_line_y_at()` 那个内部函数没有任何调用者。
 *   · 能接的公开接口有两个：render_ground_screen_y(screen_x)（该列脚踩线屏 y，
 *     -1=越界/无表）与 render_set_drag_off_y(py)（锚点 y 偏移，内部 drag_clamp 夹取）。
 * 接线方式（x=屏心 + 脚底=该处地面线，是需求 §5.2「固定回归屏幕正中间、脚踩该处
 * 地面线（…x=屏心）」唯一几何自洽的读法）：
 *   ① 收尾已把水平偏移归零（render_set_drag_off(0)）→ 锚点 x=屏心；
 *   ② 重派发落地时站位链刚把锚点 y 钉到屏心（= s_br.sh/2，这是本函数的前置假设）；
 *   ③ 取 gy = render_ground_screen_y(屏心x)，把锚点平移 Δ = gy − 屏心 y：
 *      render_set_drag_off_y(drag0 + Δ) → 脚底落到该处地面线，水平仍是屏心。
 *   ④ gy < 0（越界/非整图无地面表）→ **不动**，沿用站位链口径（锚点=屏心），只打日志。
 * 为什么不在 finish 里直接采：地面线随相机变，而相机要等重派发把 NVS 值重新应用；
 * 且站位链只在 render_set_map 内跑 → 早采会拿到"重载前"的旧值（对抗审查 P1-2 指出）。
 * 越界保护：drag_clamp 优先（整只宠物不出屏），夹取发生时日志标注实际落点。
 * ⚠️ 若主线程最终判定"宠物必须钉在屏心、不要落地面线"，删掉 cam_settle_on_ground()
 *    的 render_set_drag_off_y() 一行即可（其余取证日志保留）。 */
static void cam_settle_on_ground(const char *why)
{
    int32_t cx = s_br.sw / 2;                     /* 屏心 x（§5.2 指定用屏心列） */
    int32_t anchor0 = s_br.sh / 2;                /* 前置：站位链刚把锚点钉在屏心 */
    int32_t drag0 = render_get_drag_off_y();      /* 站位链写下的 drag_y（视为"屏心"基准） */
    int32_t gy = render_ground_screen_y(cx);      /* 该处脚踩线（-1=越界/无表） */

    if (gy < 0) {
        ESP_LOGW(TAG, "相机收尾结算（%s）：屏心 x=%d 无地面线（越界/非整图无地面表）→ 不落地面线，"
                      "沿用站位链口径（锚点=屏心 y=%d, drag_y=%d）",
                 why, (int)cx, (int)anchor0, (int)drag0);
        return;
    }
    int32_t delta = gy - anchor0;
    render_set_drag_off_y(drag0 + delta);         /* 内部 drag_clamp 夹取（越界保护优先） */
    int32_t drag1 = render_get_drag_off_y();
    int32_t anchor1 = anchor0 + (drag1 - drag0);
    /* 位置改在 cmd 排空期（render_tick 的"drag 变化检测"这一帧还没跑），而上一拍的
     * full_recompose 已按站位链的屏心位画过一帧 → 若入口时宠物不在屏心（drag_y≠0），
     * 新旧两矩形未必覆盖那一帧 → 强制一次全屏重合成彻底清残影（一次性代价 ~10ms）。 */
    render_force_redraw();
    if (drag1 == drag0 + delta) {
        ESP_LOGW(TAG, "相机收尾结算（%s）：脚踩地面线 → 屏心 x=%d 处 screen_y=%d；"
                      "锚点 y %d→%d（drag_y %d→%d）",
                 why, (int)cx, (int)gy, (int)anchor0, (int)anchor1, (int)drag0, (int)drag1);
    } else {
        ESP_LOGW(TAG, "相机收尾结算（%s）：脚踩地面线 screen_y=%d 超出可动行程 → drag_clamp 夹到"
                      " drag_y=%d（锚点 y=%d；「整只宠物不出屏」优先）",
                 why, (int)gy, (int)drag1, (int)anchor1);
    }
}

/* 地图重派发落地回调（state_machine.c dispatch_map 装载成功路径调用；唯一结算点）。
 * pending 未置位时是空操作 → 常规切图（菜单子页①/服务端推送）行为零变化。 */
void bridge_cam_settle_after_reload(void)
{
    if (!s_cam.settle_pending) return;
    s_cam.settle_pending = false;
    render_busy_banner(NULL, false);   /* 装载落地：撤 loading 横幅（忙尾再兜 200ms） */
    cam_settle_on_ground("地图重派发落地");
}

/* 收尾核心：confirm=写 NVS（失败自动降级为取消）；full=false = 被外部状态打断
 * （待机时钟/OTA…）只复原相机与横幅，**不**重派发地图（避免在待机/升级里重载包）。 */
static void cam_finish_core(bool confirm, bool full, const char *why)
{
    /* 收尾先撤性能降级：后面要重合成完整画面（含条带）。
     * level 0 = 正常全层合成（与进调参态之前的渲染路径逐像素一致）。 */
    render_cam_adjust_set(RC_CAM_ADJ_OFF);

    if (!s_cam.on) return;
    s_cam.on = false;
    s_cam.dragging = false;

    bool saved = false;
    if (confirm) {
        int32_t cx = s_cam.cx0, cy = s_cam.cy0;
        render_cam_get(&cx, &cy);                           /* 当前（= 最终）相机 */
        saved = sm_cam_nvs_set(s_cam.key, cx, cy);
        if (saved) {
            ESP_LOGW(TAG, "相机确认保存：key=%s (x=%d,y=%d) ns=cam ← %s",
                     s_cam.key, (int)cx, (int)cy, why);
        } else {
            ESP_LOGE(TAG, "相机确认：NVS 写入失败（key=%s）→ 撤销本次调整", s_cam.key);
        }
    }
    if (!saved) {
        render_cam_set(s_cam.cx0, s_cam.cy0);               /* 取消 / 中断 / NVS 失败 */
        ESP_LOGW(TAG, "相机复原进入前状态：key=%s (x=%d,y=%d) ← %s",
                 s_cam.key, (int)s_cam.cx0, (int)s_cam.cy0, why);
    }

    if (full) {
        /* 宠物回归屏幕正中间 + 脚踩地面线（§5.2）：
         *   · 水平：drag 偏移归零 → 锚点回屏心（锚点即脚底基准），立即生效；
         *   · 垂直：重新派发当前图 → render_set_map → 既有站位链
         *     ent_stand_on_ground_locked()（compositor.c 内，唯一外部可达触发点；
         *     同时把 NVS 相机按「全局加载」口径再应用一次）；
         *   · 地面线：**不能在这里采**——站位链要等重派发落地才跑，相机也要等 NVS
         *     重新应用，此刻采样拿到的是重载前的旧值（对抗审查 P1-2）。故只置 pending，
         *     结算点 = dispatch_map 成功路径的 bridge_cam_settle_after_reload()。 */
        render_set_drag_off(0);
        mp_cmd_t c = { .type = MP_CMD_SET_MAP };
        strlcpy(c.s, s_cam.hash, sizeof(c.s));
        if (c.s[0]) {
            s_cam.settle_pending = true;
            s_cam.settle_retried = false;
            s_cam.settle_by_ms   = mp_now_ms() + CAM_SETTLE_TIMEOUT_MS;
            /* 【loading 横幅 + 忙窗】重派发 = 一次完整地图装载（>5s）。横幅必须在
             * 入队前就亮（渲染任务下一拍画上），忙窗同时把按键/触摸全丢掉——
             * 这就是用户口径"地图装载窗口内不接受按钮信息"的落点。 */
            render_busy_banner(CAM_BANNER_LOADING, true);
            if (!mp_post_cmd(&c)) {
                s_cam.settle_pending = false;
                render_busy_banner(NULL, false);
                ESP_LOGE(TAG, "相机收尾：cmd_q 满 → 重派发未入队（站位链/地面线不结算）");
            } else {
                ESP_LOGI(TAG, "相机收尾：重派发地图 %s（宠物回屏心 + 站位链复算；"
                              "落地后结算「脚踩地面线」）", c.s);
            }
        } else {
            s_cam.settle_pending = false;
            ESP_LOGW(TAG, "相机收尾：目标图 hash 为空 → 跳过重派发（站位链与地面线均无法结算）");
        }
    }

    /* 可见反馈收尾：已配网 → 1.5s 定时横幅；未配网 → 恢复常驻配网横幅 */
    if (provision_has_config()) {
        render_banner_show_for(saved ? CAM_BANNER_SAVED : CAM_BANNER_CANCELED, 1500);
    } else {
        state_machine_banner_restore();
    }
    s_cam.hash[0] = 0;
    s_cam.key[0]  = 0;
}

/* 用户确认/取消（input 任务：中键短按/长按、顶键短按/长按） */
void bridge_cam_adjust_finish(bool confirm)
{
    cam_finish_core(confirm, true, confirm ? "按键确认" : "按键取消");
}

/* 调参态兜底（input 任务主循环 ~20ms）：
 *   ① 「脚踩地面线」结算超时兜底——重派发没落地（清单未就绪/cmd_q 满/路径查询失败）
 *      时站位链不会跑：先重投一次地图；再超时则明确放弃（绝不猜锚点位置乱放宠物，
 *      宁可维持现状并留 ERROR，等下次装载该图自然归位）；
 *   ② 状态机离开 POKER/OFFLINE（待机时钟/OTA/配网/FATAL）→ 未确认即取消并复原相机，
 *      杜绝"模式悬空"（横幅留屏、相机半套用）。 */
void bridge_cam_adjust_poll(void)
{
    if (s_cam.settle_pending && mp_now_ms() > s_cam.settle_by_ms) {
        if (!s_cam.settle_retried && s_cam.hash[0]) {
            s_cam.settle_retried = true;
            s_cam.settle_by_ms = mp_now_ms() + CAM_SETTLE_TIMEOUT_MS;
            mp_cmd_t c = { .type = MP_CMD_SET_MAP };
            strlcpy(c.s, s_cam.hash, sizeof(c.s));
            mp_post_cmd(&c);
            ESP_LOGW(TAG, "相机收尾结算超时 → 重投地图 %s（站位链/地面线再试一次）", c.s);
        } else {
            s_cam.settle_pending = false;
            render_busy_banner(NULL, false);   /* 放弃结算：撤 loading 横幅、解忙窗 */
            ESP_LOGE(TAG, "相机收尾结算放弃：地图 %s 两次未装载落地 → 宠物未归屏心、未落地面线"
                          "（下次装载该图会自然归位）", s_cam.hash);
        }
    }
    if (!s_cam.on) return;
    mp_state_t st = state_machine_current();
    if (st == MP_ST_POKER || st == MP_ST_OFFLINE) return;
    char why[32];
    snprintf(why, sizeof(why), "状态机=%s", state_machine_name(st));
    cam_finish_core(false, false, why);
}

/* 子页②入口：真正进入相机调参态。返回 true = 已进入（菜单随即被收起）。 */
static bool menu_map_fn_camera_enter(void)
{
    ESP_LOGW(TAG, "menu: 子页②相机入口 hash=%.16s key=%s label=%.24s",
             s_menu.fn_hash, s_menu.fn_key, s_menu.fn_label);

    /* ① 能力门（§3.3）：render_cam_supported()==false = 非整图包（无可平移余量）
     *    → 状态行提示、**不进入调参态**（旧包视觉与交互逐字节不变）。 */
    if (!render_cam_supported()) {
        ESP_LOGW(TAG, "menu: 相机不可用——当前图非整图包（需整图包才能平移相机）");
        menu_status_flash(CAM_FLASH_NO_FULLMAP);
        return false;
    }
    /* ② 调参对象必须是**当前正在渲染的图**：render_cam_supported() 只反映已装载图，
     *    且用户必须看得见它才能调（相机=可见窗口左上角）。不满足 → 引导走子页① */
    if (!asset_dl_map_is_active(s_menu.fn_hash)) {
        ESP_LOGW(TAG, "menu: %.24s 不是当前背景图 → 拒绝调参（先走子页①「选择此地图为背景」）",
                 s_menu.fn_label);
        menu_status_flash(CAM_FLASH_NEED_BG);
        return false;
    }
    if (!s_menu.fn_key[0]) {
        ESP_LOGW(TAG, "menu: 目标图无 per-map 键（清单未登记？）→ 拒绝调参");
        menu_status_flash(CAM_FLASH_NO_FULLMAP);
        return false;
    }

    /* ③ 快照进入前相机（取消/中断复原基准）+ 范围取证（契约 §五 验收锚点） */
    s_cam.cx0 = s_cam.cy0 = 0;
    s_cam.dx0 = s_cam.dy0 = 0;
    render_cam_get(&s_cam.cx0, &s_cam.cy0);
    strlcpy(s_cam.hash, s_menu.fn_hash, sizeof(s_cam.hash));
    strlcpy(s_cam.key,  s_menu.fn_key,  sizeof(s_cam.key));
    s_cam.dragging = false;
    s_cam.last_apply_ms = 0;
    s_cam.settle_pending = false;      /* 新一轮调参：清上一轮可能残留的结算旗标 */
    s_cam.settle_retried = false;
    int32_t mdx = 0, mdy = 0;
    render_cam_range(&mdx, &mdy);
    ESP_LOGW(TAG, "相机调参态进入：hash=%.16s key=%s 进入前相机=(%d,%d) 支持=1 范围 dx[0,%d] dy[0,%d]",
             s_cam.hash, s_cam.key, (int)s_cam.cx0, (int)s_cam.cy0, (int)mdx, (int)mdy);
    /* 需求原文（串口可查）：相机调整中：拖动平移 / 中键短按确认 · 长按取消 */
    ESP_LOGW(TAG, "menu: 相机调整中：拖动平移 / 中键短按确认 · 长按取消（此串含烘焙子集外"
                  "字形，屏上用 " CAM_BANNER_HINT " 等价呈现）");

    /* ④ 菜单内可见提示（状态行）+ 收菜单：MENU→POKER 让出全屏，露出地图/宠物。
     * 【顺序红线】s_cam.on 必须在状态机切到 POKER **之后**才置位——input 任务的
     * bridge_cam_adjust_poll() 判据是"调参态开着但状态不在 POKER/OFFLINE 就取消"，
     * 若先置位再切态（本函数在渲染任务里可能与 input 任务抢 state_machine 的锁），
     * 会被 poll 误判成"悬空态"当场取消。
     * 【限速红线】走内部通道 state_machine_cam_enter_poker()，**不吃** MP_SM_EV_MENU_KEY
     * 的 400ms 硬限速（那条限速防的是实体键抖动把菜单关了又开；菜单里点②是显式 UI
     * 动作，被吞掉的表现就是屏上 CAM BUSY - RETRY 且调参态进不去）。 */
    menu_status_flash(CAM_FLASH_HINT);
    if (!state_machine_cam_enter_poker()) {
        ESP_LOGW(TAG, "menu: 收菜单未生效（MENU→POKER 失败）→ 不进入调参态");
        menu_status_flash("CAM BUSY - RETRY");
        return false;
    }
    s_cam.on = true;
    /* 【2026-10-01 用户定稿（第二次更正）】在"选择/调整摄像机这个流程"里
     * **不渲染 bac（背景装饰条带层）**，只画 static + tile：
     *   "拖动的时候还是卡住，还是把 bac 渲染了；单纯的 tile 应该是不消耗性能"
     *   "应该是在选择摄像头这个流程的时候不进行渲染 bac"
     * 因此进流程即落 level 1（跳条带层）：
     *   · **地形本体（static+tile）照常全渲染** —— 相机要对准的是地形，够用；
     *   · 条带层（天空之城的 14 条视差装饰带）既不合成也不补缓存 ⇒ 拖动期间
     *     不再有"每 2~3s 一次 2~3s 的条带重填"（那是"卡住"的主因）；
     *   · 保存/取消（cam_finish_core）立即回 level 0，全层恢复。
     * 注：不改 level 2（只画 static）——tile 是地形，必须画。 */
    render_cam_adjust_set(RC_CAM_ADJ_NO_STRIP);

    /* ⑤ 调参态常驻横幅：入队放在 MENU_EXIT/POKER on_enter 之后，保证压过未配网
     *    横幅（cmd_q FIFO，同一渲染任务帧内顺序落地） */
    mp_cmd_t c = { .type = MP_CMD_BANNER, .a = 1 };
    strlcpy(c.s, CAM_BANNER_HINT, sizeof(c.s));
    if (!mp_post_cmd(&c)) ESP_LOGW(TAG, "cmd_q 满：调参横幅未入队（模式仍生效）");
    return true;
}

static void menu_map_fn_camera_hook(void)
{
    (void)menu_map_fn_camera_enter();   /* 失败路径已在状态行给出可见提示，留在菜单 */
}

/* 子页③删除此地图 = §4.4 本地隐藏标识（不物理删文件）：
 *   · asset_dl_map_set_hidden(true) → NVS per-map 置位（重启保持）；
 *   · 列表立即不再显示该图（asset_dl_bgmap_list 过滤）；
 *   · 隐藏的是**当前正在渲染的图**、或隐藏后**已无可见图** → 立即切回默认图
 *     MP_DEFAULT_MAP_ID（000010000）；
 *   · 服务端再次推送该图 → poller 自动解除隐藏（列表恢复）。 */
static void menu_map_fn_delete(void)
{
    bool was_active = asset_dl_map_is_active(s_menu.fn_hash);
    bool ok = asset_dl_map_set_hidden(s_menu.fn_hash, true);
    int visible = asset_dl_bgmap_visible_count();
    ESP_LOGW(TAG, "menu: 子页③删除(隐藏) hash=%.16s key=%s nvs=%d was_active=%d 剩余可见=%d",
             s_menu.fn_hash, s_menu.fn_key, (int)ok, (int)was_active, visible);
    /* 【契约 §3.3】隐藏/删除该图 → 清它的 per-map 相机键（与隐藏标识同一处收尾，
     * 见 state_machine.c sm_cam_nvs_* 的注释："删除地图时清除"）。 */
    if (sm_cam_nvs_erase(s_menu.fn_key)) {
        ESP_LOGW(TAG, "menu: 子页③已清相机键 key=%s（ns=cam）", s_menu.fn_key);
    } else {
        ESP_LOGW(TAG, "menu: 子页③相机键清除失败 key=%s（ns=cam，残留不影响渲染）",
                 s_menu.fn_key);
    }
    /* 边界（§4.4）：隐藏当前图 / 隐藏全部图 → 立即切回默认图。
     * 默认图自身被隐藏时同样切（等于保持渲染默认图）：隐藏只是"列表不显示"，
     * 默认图仍是兜底渲染源，不做黑屏。 */
    if (was_active || visible == 0) {
        mp_cmd_t c = { .type = MP_CMD_SET_MAP };
        strlcpy(c.s, MP_DEFAULT_MAP_ID, sizeof(c.s));
        mp_post_cmd(&c);
        ESP_LOGW(TAG, "menu: 隐藏的是当前图/已无可见图 → 切回默认图 %s（was_active=%d visible=%d）",
                 MP_DEFAULT_MAP_ID, (int)was_active, visible);
        menu_status_flash("已隐藏，切回默认图");
    } else {
        menu_status_flash("已隐藏，推送可恢复");
    }
    menu_goto(MENU_PAGE_MAPS);
}

static void menu_activate(int idx)
{
    if (idx < 0 || idx >= s_menu.row_cnt) return;
    if (!s_menu.row_enabled[idx]) {          /* T3：置灰行侧键确认也不派发 */
        ESP_LOGI("menu", "row %d 置灰（离线未缓存），忽略确认", idx);
        return;
    }

    /* 列表页 Back 行（各页共用）：回**上级页**（地图功能子页 → 地图列表；
     * 其余子页 → 根页），语义见 menu_parent_page */
    if (s_menu.back_idx >= 0 && idx == s_menu.back_idx) {
        menu_goto(menu_parent_page(s_menu.page));
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
        /* 【§4.1 2026-10-01】点列表项不再"立即切图"，改为进功能子页
         * （①选为背景 ②改相机 ③删除 ④返回地图列表） */
        if (idx < s_menu.item_cnt) menu_map_fn_open(idx);
        break;

    case MENU_PAGE_MAP_FN:
        /* 地图功能子页三项；末行「返回地图列表」= back_idx，已在上面按上级页
         * （menu_parent_page）处理，这里不再重复分支 */
        if      (idx == 0) menu_map_fn_background();
        else if (idx == 1) menu_map_fn_camera_hook();      /* ← 相机 agent 接手点 */
        else if (idx == 2) menu_map_fn_delete();
        break;

    case MENU_PAGE_PAPERDOLL:
        if (idx < s_menu.item_cnt) menu_activate_item(idx, MP_CMD_SET_PARTS);
        break;

    case MENU_PAGE_NPC:
        /* 【2026-10-02 可用化】点选 = 把宠物形象切成该怪物/NPC（先下载后切换）。
         * 菜单里同时保留"只缓存"的语义：已缓存→立刻切换；未缓存→下载完自动切换。 */
        if (idx < s_menu.item_cnt) {
            const menu_item_t *it = &s_menu.items[idx];
            menu_dispatch_entity(it->entity, it->hash, it->label, it->cached, idx);
        }
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
            /* 实体切换的载荷是 entity 字符串（不是包 hash）；其余指令仍是 hash */
            if (s_menu.dl_cmd == MP_CMD_SET_ENTITY && s_menu.dl_entity[0]) {
                strlcpy(c.s, s_menu.dl_entity, sizeof(c.s));
            } else {
                strlcpy(c.s, s_menu.dl_hash, sizeof(c.s));
            }
            mp_post_cmd(&c);
        }
        if (s_menu.dl_cmd != MP_CMD_NONE) {
            snprintf(s_menu.hint_once, sizeof(s_menu.hint_once),
                     "下载完成，已应用");
        } else {
            snprintf(s_menu.hint_once, sizeof(s_menu.hint_once),
                     "下载完成，已缓存");
        }
        ESP_LOGI(TAG, "menu: 下载完成 %.16s → cmd=%d", s_menu.dl_hash, (int)s_menu.dl_cmd);
        s_menu.dl_active = false;
        /* 回页：列表页=原地重建（保持选中，既有行为）；地图功能子页=回地图列表 */
        if (s_menu.dl_goto_back) {
            s_menu.pend_page = s_menu.dl_back;
            s_menu.pend_sel  = 0;
            s_menu.dl_goto_back = false;
        } else {
            s_menu.pend_page = s_menu.page;
            s_menu.pend_sel  = s_menu.sel;
        }
        s_menu.req_rebuild = true;
        return;
    }

    if (mp_now_ms() > s_menu.dl_deadline_ms) {
        ESP_LOGW(TAG, "menu: 下载超时 %.16s", s_menu.dl_hash);
        s_menu.dl_active = false;
        s_menu.dl_goto_back = false;
        menu_hint_set("DL TIMEOUT (SERVER?)");
        menu_status_flash("下载超时");
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

/* 行文本（实体页）：在通用 [v] 标记前再加一个 ">" = **当前正在显示的实体**
 * （state_machine 的 NVS 形象状态）。[v] 仍表示包已缓存。 */
static void menu_row_text_entity(char *out, size_t cap, bool cached, bool enabled,
                                 const char *label, bool active)
{
    snprintf(out, cap, "%s[%s] %s",
             active ? ">" : " ",
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
    /* 【禁滚动 2026-09-29】菜单控件微量超出 480×480 会让屏幕可滚——
     * 真机横向拖拽把整个菜单平移出屏（露出空白）。锁死滚动。 */
    lv_obj_set_scrollable(scr, false);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
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
    lv_obj_set_style_text_font(st, &menu_font_cn, 0);   /* 选中回显是中文：用 CJK 子集字体 */
    lv_label_set_text(st, "");
    lv_obj_center(st);
    s_menu.status_label = st;
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
    ESP_LOGW("mtouch", "roller VALUE_CHANGED sel=%d", sel);
    if (sel >= 0 && sel < MENU_ROWS_MAX) s_menu.sel = sel;
}

/* 底部 OK 按钮：顶键同通道（req_ok 旗标，tick 排空派发 menu_activate(sel)） */
static void menu_btn_ok_cb(lv_event_t *e)
{
    (void)e;
    s_menu.req_ok = true;
}

/* 底部 Back 按钮：根页=退出菜单（req_exit 走状态机 MENU_KEY 通道）；
 * 子页=回上级页（menu_goto 只落旗标，tick 重建，不在此动控件树） */
static void menu_btn_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_menu.page == MENU_PAGE_ROOT) s_menu.req_exit = true;
    else menu_goto(menu_parent_page(s_menu.page));
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
    lv_obj_set_style_text_font(lb, &menu_font_cn, 0);   /* 按钮文案是中文：用 CJK 字体 */
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
    } else {
    }
}

/* 怪物（实体形象）页的行式列表：与 menu_build_list 同构，多一个 ">" 标记 =
 * **当前正在显示的那个实体**（state_machine 的 NVS 形象状态，sm_active_entity()）。
 * 为什么需要：同一页里既有怪物又有 NPC，用户点完切走再回来必须一眼看出现在是哪只
 * （旧页只有 [v] 缓存标记，无法区分"已缓存"和"正在用"）。 */
static void menu_build_list_entities(lv_obj_t *scr, const char *empty_text, const char *hint)
{
    (void)hint;
    int row = 0;
    const char *cur = sm_active_entity();
    if (s_menu.item_cnt <= 0) {
        menu_add_row(scr, row++, empty_text, 78, 42, false);   /* 明确空态，非假数据 */
    } else {
        for (int i = 0; i < s_menu.item_cnt && row < MENU_ROWS_MAX; i++) {
            bool en = s_menu.items[i].cached || !s_menu.offline;   /* T3 置灰判据 */
            bool active = (cur && cur[0] && strcmp(cur, s_menu.items[i].entity) == 0);
            char text[48];
            menu_row_text_entity(text, sizeof(text), s_menu.items[i].cached, en,
                                 s_menu.items[i].label, active);
            menu_add_row(scr, row++, text, 78 + i * 46, 42, en);
        }
    }
    menu_add_row(scr, row, "< Back", 78 + row * 46, 42, true);
    s_menu.back_idx = row;
    s_menu.row_cnt  = row + 1;
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
    /* 【不硬填满 2026-09-29】条目少于可见行数（如纸娃娃页仅 1 项）时用
     * NORMAL：不回绕不重复刷屏；条目多才 INFINITE 循环 */
    lv_roller_set_options(r, opts,
                          (cnt >= MENU_ROLLER_VIS) ? LV_ROLLER_MODE_INFINITE
                                                   : LV_ROLLER_MODE_NORMAL);
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


    /* 行契约映射：滚筒选项回填 row_cnt/row_enabled，menu_activate 原语义零改动 */
    s_menu.row_cnt = cnt;
    for (int i = 0; i < cnt && i < MENU_ROWS_MAX; i++)
        s_menu.row_enabled[i] = en ? en[i] : true;

}

/* 选择页（Maps/Paperdoll/Actions）公共装配：收集已在 *_collect 完成，
 * 这里打包选项串 → 建滚筒（含截断 (+MORE) 提示）。 */
static void menu_build_selection_page(lv_obj_t *scr, const char *empty_text,
                                      const char *hint)
{
    (void)hint;   /* 提示行已随 UI 精简移除 */
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
        menu_chrome_build(scr, "主菜单");
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
        menu_chrome_build(scr, "选择地图");
        menu_build_selection_page(scr, "NO MAP IN LOCAL MANIFEST",
                                  "TAP: SWITCH/DL   [v] CACHED");
        break;
    case MENU_PAGE_MAP_FN: {
        /* 地图功能子页（§4.1）：滚筒三项 + 「返回地图列表」。
         * 目标地图身份/名字在 menu_map_fn_open 已快照（进页时状态行提示一次），
         * 这里只建控件——重复重建（离线/rev 翻转）不重复刷提示。 */
        menu_chrome_build(scr, "地图功能");
        static char opts[MENU_MAP_FN_CNT * 48];
        static bool en[MENU_MAP_FN_CNT];
        static const char *const fn_rows[MENU_MAP_FN_CNT] = {
            "选择此地图为背景",
            "修改当前地图的摄像头",
            "删除此地图",
            "返回地图列表",
        };
        size_t o = 0;
        for (int i = 0; i < MENU_MAP_FN_CNT; i++) {
            en[i] = true;                      /* 相机项也保持可点：占位提示可见（hook 可测） */
            o += (size_t)snprintf(opts + o, sizeof(opts) - o, "%s%s",
                                  i ? "\n" : "", fn_rows[i]);
        }
        s_menu.row_cnt = MENU_MAP_FN_CNT;
        menu_build_roller(scr, opts, MENU_MAP_FN_CNT, en, NULL);
        /* 末行「返回地图列表」走 back_idx 分支 → menu_parent_page(MAP_FN)=MAPS */
        s_menu.back_idx = MENU_MAP_FN_CNT - 1;
        break;
    }
    case MENU_PAGE_PAPERDOLL:
        menu_parts_collect();
        menu_chrome_build(scr, "纸娃娃");
        menu_build_selection_page(scr, "NO OUTFIT PACK (SYNC NEEDED)",
                                  "TAP: WEAR PARTS  [v] CACHED");
        break;
    case MENU_PAGE_ACTIONS:
        menu_actions_collect();
        menu_chrome_build(scr, "选择动作");
        menu_build_selection_page(scr, "NO ACTION PACK (SYNC NEEDED)",
                                  "TAP: PLAY  [v] CACHED  [ ] NO PACK");
        break;
    case MENU_PAGE_NPC:
        /* 怪物页（mob + npc 实体形象）：行式页 + ">" 当前形象 / [v] 已缓存标记 */
        menu_entity_collect();
        menu_chrome_build(scr, "怪物");
        menu_build_list_entities(scr, "NO MOB ASSET (WEB PUSH)",
                                "TAP: USE AS PET  [v] CACHED  > CURRENT");
        break;
    case MENU_PAGE_BGM: {
        /* 行式页（控制项非"选择"语义），配色统一羊皮纸；
         * 状态行=底部骨架行（tick 500ms 刷新 BGM 态） */
        menu_chrome_build(scr, "BGM");
        menu_bgm_status_refresh();
        menu_add_row(scr, 0, "播放/暂停", 72, 44, true);
        menu_add_row(scr, 1, "上一首",    122, 44, true);
        menu_add_row(scr, 2, "下一首",    172, 44, true);
        menu_add_row(scr, 3, "音量−",     222, 44, true);
        menu_add_row(scr, 4, "音量+",     272, 44, true);
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
        break;
    }

    case MENU_PAGE_RESET: {
        /* 【E14】免插线重配网：清配网凭据（WiFi + 服务器地址）后重启 →
         * 设备进 SoftAP portal（MiniPet-XXXX）。行式确认页 + 羊皮纸配色 */
        menu_chrome_build(scr, "Reset WiFi");
        menu_add_row(scr, 0, "CONFIRM RESET", 170, 52, true);
        menu_add_row(scr, 1, "< Back",        240, 52, true);
        s_menu.row_cnt = 2;
        break;
    }
    }

    if (s_menu.sel >= s_menu.row_cnt) s_menu.sel = 0;   /* 页内行数变化（截断/空态） */
    s_menu.sel_applied = -1;   /* 交下一 tick 统一重贴高亮（与夹取后的 sel 严格一致） */

    /* T4 一次性提示（下载完成）：本次重建消费后清空。hint_label 现为 NULL
     * （UI 精简后无页脚提示行）→ 同时走状态行可见通道 */
    if (s_menu.hint_once[0]) {
        menu_hint_set(s_menu.hint_once);
        menu_status_flash(s_menu.hint_once);
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
    /* 提示态翻转也要走一遍（否则提示到期后状态行永远停在提示文本上）。
     * 未选中变化且提示态未翻转 = 无事可做（防 10Hz 无谓失效） */
    bool flash = menu_status_flash_active();
    if (s_menu.sel_applied == s_menu.sel && flash == s_menu.flash_shown) return;
    s_menu.flash_shown = flash;

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
            if (flash) {
                lv_label_set_text(s_menu.status_label, s_menu.flash_msg);
            } else {
                char opt[48];
                lv_roller_get_selected_str(s_menu.roller, opt, sizeof(opt));
                lv_label_set_text_fmt(s_menu.status_label, "选中：%s", opt);
            }
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
    /* 【卡顿修复 2026-09-29】移除 100ms 强制整屏失效——滚筒拖拽期 LVGL
     * 本就按失效区局部重绘，整屏失效使菜单态常驻 10fps 全屏重绘 → 极卡。
     * 叠影风险：行文本/选中带由 LVGL 失效区自行覆盖，真机验证。 */
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

    /* 【契约 §3.3 兜底】进菜单时若相机调参态还开着（顶键已被调参态拦截，正常不可达；
     * 防御状态机侧的其他入口）→ 强制取消：留在地图画面里的是"半套用"的相机状态，
     * 不能带进菜单。这里在渲染任务，复原相机是安全的。 */
    if (s_cam.on) {
        ESP_LOGW(TAG, "mode_menu: 相机调参态未收尾 → 强制取消");
        cam_finish_core(false, false, "进菜单");
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
    s_menu.dl_goto_back = false;
    s_menu.hint_once[0] = 0;
    s_menu.fn_hash[0] = 0;            /* 地图功能子页目标图快照：进菜单即清 */
    s_menu.fn_key[0] = 0;
    s_menu.fn_label[0] = 0;
    s_menu.flash_msg[0] = 0;          /* 状态行提示：不带上一轮的残留 */
    s_menu.flash_until_ms = 0;
    s_menu.flash_shown = false;
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
