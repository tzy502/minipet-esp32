/*
 * render.h — 固件渲染层对外总接口（Firmware/main/render/）
 *
 * 面向 app/（状态机）与 net/（指令执行）。除 render_input_tilt 外，
 * 所有函数必须与 render_tick 同任务调用（APP_CPU lvgl 任务，软件设计 4.1）。
 * 素材路径按 TF 布局（软件设计 4.4）：/minipet/{parts,layout,bg,font}/<hash>.mpk。
 *
 * 依赖契约（并行开发接口，已按 components/drivers/ 与 profiles/ 实测对齐）：
 *   - #include "drivers.h"（components/drivers/include/）：
 *       esp_err_t display_init(void);
 *       esp_err_t display_blit(int x, int y, int w, int h,
 *                              const uint8_t *rgb565_be);  // 大端 RGB565！
 *       esp_err_t display_brightness(uint8_t pct);          // render 层不调用
 *     渲染管线全小端，blit_be() 在唯一上屏边界做字节交换（compositor.c）。
 *   - #include "profiles/amoled216.h" 提供 minipet_profile_t：
 *     渲染层仅读 .width / .height（480/480）；
 *     板卡实例 MINIPET_PROFILE_AMOLED216（app 传其地址给 render_init）。
 *
 * 返回码：RENDER_OK=0；负数为错误（MPAK_ERR_* 原样透传或 RENDER_ERR_*）。
 */
#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>
#include <stdbool.h>

#include <lvgl.h>                  /* lv_font_t / lv_display_t（菜单 UI 用） */

#include "amoled216.h"             /* profiles 组件根目录导出（与 drivers.h 同约） */

#ifdef __cplusplus
extern "C" {
#endif

#define RENDER_OK          0
#define RENDER_ERR_ARG     -100
#define RENDER_ERR_NOMEM   -101
#define RENDER_ERR_STATE   -102   /* 顺序错误（如未 init / 模式冲突） */
#define RENDER_ERR_UNSUPPORTED -103

/* 字体档位（FONT 包三实例：16/24/32px） */
typedef enum {
    RENDER_FONT_16 = 0,
    RENDER_FONT_24 = 1,
    RENDER_FONT_32 = 2,
} render_font_t;

/*
 * 初始化渲染层：display_init()、PSRAM 场景缓冲、LVGL 桥（POKER 模式）。
 * 需在 FATFS 挂载后、首个 render_tick 前调用一次。
 */
int  render_init(const minipet_profile_t *profile);

/* 帧节拍（30fps 定时器驱动；内部完成合成/脏区/display_blit/LVGL） */
void render_tick(void);

/* ---------------- 素材绑定（TF 路径；失败保留旧画面） ---------------- */

/* 换装：PARTS 包（一整套装扮） */
int  render_set_parts(const char *mpk_path);
/* 换动作：LAYOUT 包；loop=false 的单次动作播完自动回退最近一次 loop 布局（stand1） */
int  render_set_layout(const char *mpk_path, bool loop);
/* 表情切换（按名，作用于当前动作的 expression 列表；blink 由本地定时器插播） */
int  render_set_expression(const char *name);
/*
 * 换地图：BGMAP 包 + 条带小 PARTS 包路径数组（顺序 = BGMAP 条带头顺序；
 * 冰箱贴类无条带地图 strip_count=0 → strip_paths 传 NULL/0）。
 * 时钟锚点来自 manifest clock_table（世界 1x 坐标），由 app 查表后另行下发。
 */
int  render_set_map(const char *bgmap_path,
                    const char *strip_parts_paths[], int strip_count);

/* 强制一次全屏重合成 + 全幅上屏（外部直写面板/素材重绑后清残留；
 * 无 BGMAP → 全屏填黑 blit 一次，有 BGMAP → static_back+tile 铺满） */
void render_force_redraw(void);

/* 实体屏幕锚点（世界 1x 坐标，映射到屏幕中心偏移；默认 0,0） */
void render_set_entity_pos(int16_t world_x, int16_t world_y);

/* 地图时钟：fontTime PARTS 包 + clock_table 锚点（世界 1x）；path=NULL 仅改锚点/开关 */
int  render_set_clock(const char *fonttime_parts_path,
                      int16_t anchor_world_x, int16_t anchor_world_y, bool enable);

/* 字体装载（气泡/菜单用；FONT 包） */
int  render_set_font(render_font_t id, const char *mpk_path);
const lv_font_t *render_get_font(render_font_t id);

/* ---------------- 模式切换（POKER ⇄ MENU，单写屏者） ---------------- */

/* 进菜单：LVGL 整屏离屏（450KB DIRECT），合成器让路；宠物暂停 */
int  render_enter_menu(void);
/* 回宠物场景：恢复 POKER 部分缓冲，全幅重合成 */
int  render_exit_menu(void);
/* 菜单 UI 挂载点（enter_menu 后使用；POKER 下勿画） */
lv_display_t *render_lvgl_display(void);

/* ---------------- 气泡（POKER 专用；离屏位图由合成器 blit 到宠物上方） --- */
int  render_bubble_show(const char *text, render_font_t font);
void render_bubble_hide(void);

/* ---------------- 未配网常驻横幅（POKER 态顶部 480×28，合成器最顶层） ------
 * text 为 ASCII 大写串（内嵌 5x7 字体）；CLOCK_DOZE 态自动让位不画 */
int  render_banner_show(const char *text);
void render_banner_hide(void);

/* ---------------- IMU 视差（input 任务可异步调用；int32 对齐写原子） --- */
void render_input_tilt(float tilt_deg);
/* 拖拽跟手：人物屏幕 x 偏移（px，1:1，clamp ±160）；get 供输入侧增量累计 */
void render_set_drag_off(int32_t px);
int32_t render_get_drag_off(void);
void render_set_drag_off_y(int32_t py);
/* 【校准】红线坐标系 + 触摸落点回显（tx,ty <0 = 不更新落点） */
void render_calib_set(bool on, int16_t tx, int16_t ty);
int32_t render_get_drag_off_y(void);

/* ==================================================================== */
/* 【菜单真实化 2026-09】菜单选择器对外钩子（lvgl_bridge.c 实现）          */
/*                                                                      */
/* 输入接线分工（主线程落地）：                                           */
/*   - input_dispatch 在菜单态（state_machine_menu_open()）读触摸帧后调   */
/*     lv_bridge_touch_feed(x, y, pressed)——touch_read_frame 本体不动；   */
/*     LVGL pointer indev 已由 bridge_init 创建并消费喂入坐标。           */
/*   - 侧键矩阵（短按，MENU 态）：顶键=确认 → render_menu_ok()；          */
/*     中键=上移 → render_menu_nav(0)；底键=下移 → render_menu_nav(1)。   */
/*     长按顶键（转 CLOCK_DOZE）归 input_dispatch 统一处理，不经本层。     */
/*   - 非菜单态调用全部为安全 no-op。                                     */
/*                                                                      */
/* 跨任务约定：feed 写坐标+按下标志（单写者，先坐标后 pressed）；         */
/*   nav 只写单字 sel；ok 置请求旗标——控件树操作全部由渲染任务内          */
/*   菜单 100ms tick 排空执行，输入任务侧永不动 LVGL 对象。               */
/* ==================================================================== */

/* 触摸喂入（input 任务菜单态调用）：屏幕坐标 0..479（同 touch_read_frame
 * 映射口径），pressed=false = 抬起帧 */
void lv_bridge_touch_feed(int x, int y, bool pressed);

/* 侧键导航（input 任务调用）：dir=0 选中项上移 / 1 下移（越界回绕）；
 * 根页移动选中项，子页移动列表高亮 */
void render_menu_nav(int dir);

/* 侧键确认（input 任务调用，顶键短按）：根页=进入选中子页（Exit 行=收菜单，
 * 经状态机 MP_SM_EV_MENU_KEY 通道）；Maps/Paperdoll 子页=执行选中条目并回
 * 根页；BGM 子页=执行选中按钮动作 */
void render_menu_ok(void);

#ifdef __cplusplus
}
#endif

#endif /* RENDER_H */
