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
#include "watchdog.h"   /* 长任务喂狗：整图装载/窗口缓存填充可远超 5s 帧预算 */

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
#include "mp_psram.h"
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

/* ══ R2 整图窗口缓存（世界 px；契约 §3.2）════════════════════════════════════
 * 每层一块 PSRAM 常驻缓存，覆盖「可见窗口 ± 余量」。相机平移时缓存锚点跟着走：
 *   命中 → 零 IO；小幅平移 → memmove 搬移 + 只读新露出的边条；跳变超窗 → 整窗重载。
 * cov = 1bit/px（行按 (cw+7)/8 字节对齐，MSB first，与 rc_mask_bit 同序）；
 * NULL = 该层不透明（static）。 */
#define RC_WC_STATIC 0
#define RC_WC_TILE   1
#define RC_WC_STRIP  2
#define RC_STRIP_PATH_MAX  160             /* 条带包路径（/sdcard/minipet/bg/<hash>.mpk 量级） */

typedef struct {
    uint16_t *px;                          /* cw×ch RGB565（源像素；无数据=0） */
    uint8_t  *cov;                         /* cw×ch 1bit（NULL=不透明） */
    int32_t   cw, ch;                      /* 缓存尺寸（世界 px） */
    int32_t   ax, ay;                      /* 缓存左上角 = 源坐标（wrap 层 ax ∈ [0,sw)） */
    bool      valid;                       /* 内容已装载 */
    uint8_t   kind;                        /* RC_WC_* */
    int32_t   sw, sh;                      /* 源尺寸（static/tile=vw×vh；strip=带图 w×h） */
    bool      wrap;                        /* x 方向周期平铺（带宽 < vw 的滚动条带） */
    /* 源 IO 描述 */
    mpak_t   *pkg;                         /* 句柄（static/tile = g_bgmap） */
    uint32_t  px_off;                      /* 源像素区绝对文件偏移（strip 直读） */
    uint32_t  cov_off;                     /* 源掩码区绝对文件偏移（strip / tile 桥接） */
    uint32_t  row_bytes;                   /* 源行字节（align4(源宽×2)）；瓦片层忽略（契约 §5） */
    int       fd;                          /* strip 专用：本次补读期间打开的文件 fd（-1=未开） */
    /* ── 瓦片存储（契约 docs/ai/map-tiled-format-contract.md；2026-10-01）──
     * tiled=true 时像素/掩码**只能**经 mpak_tile_read_* 按瓦片整块取（缓存命中
     * 即纯 memcpy），绝不再用 px_off+row_bytes 逐行寻址：两种布局的行距口径不同
     * （逐行 = vh×align4(vw×2)，瓦片 = gx*gy*T*T*2），混用 = 整屏错位。 */
    bool      tiled;
    uint16_t  tile;                        /* 瓦片边长（世界 px，包内反解） */
    int32_t   tile_gx, tile_gy;            /* 瓦片网格 */
    uint32_t  tile_fid;                    /* 瓦片缓存的文件身份（strip = 路径哈希） */
} rc_wincache_t;

typedef struct {
    bool      ok;
    uint16_t *px;                          /* 1x 存储（旧窗口口径；整图模式不用） */
    uint8_t  *mask;                        /* NULL=不透明 */
    uint16_t  w, h;
    uint32_t  stride_b;
    int16_t   y, speed_x;
    uint8_t   rx, blend;
    int32_t   last_off;
    /* ── 整图（整图包）字段：y = 世界系带顶；水平世界对齐采样 ── */
    bool      world;
    int32_t   delta;                       /* 当前水平相位 Δ（世界 px，见 strip_delta_world） */
    int32_t   off_q;                       /* 量化后的时间/IMU 相位（刷新窗口推进；与 last_off 同源） */
    /* ── 扁平缓冲用的相位冻结（2026-10-01，见「地图扁平缓冲」模块）──
     * fb 是一张**压平**的图：静止时它必须与"当前相机 + 当时相位"逐像素一致。
     * 相机一动，整张 fb 只能按同一个位移平移；而条带的相位含 rx 视差项（随相机变），
     * 各带位移量并不相同 ⇒ 平移期间把各带相位**冻结**在 fb 建立时的值（此时所有层
     * 位移完全一致 = 平移精确），相机停下/相位推进时再单独重算该带所在的行回正。 */
    bool      frozen;
    int32_t   delta_frozen;
    char      path[RC_STRIP_PATH_MAX];     /* 源包路径（按需 pread，不常开句柄） */
    bool      has_cov;                     /* 源有掩码（wc_free 会清 wc->cov，故单独记） */
    bool      wrap;                        /* 周期平铺（同上：wc_alloc 会清 wc 里的标志） */
    rc_wincache_t wc;                      /* 源列/行窗口缓存（**进视野才分配**） */
} rc_strip_t;
static rc_strip_t *g_strips;
static int         g_strip_n;
/* 相机调参性能模式：0=正常 1=跳条带 2=极限（只画 static 底图 + 零 TF 读）。
 * 语义与自动回落时序见 compositor.h 的 API 注释块。 */
static int         g_cam_adjust_lvl;
/* 装载期直落相机（见 compositor.h）：valid=true 时 cam_scene_load 用它当初始相机 */
static bool        g_cam_pending_valid;
static int32_t     g_cam_pending_x, g_cam_pending_y;
static int64_t     g_cam_adj_motion_us;   /* 最近一次相机位移下发（level 2 续命） */
static int64_t     g_cam_adj_l1_us;       /* 进入 level 1 的时刻（1→0 倒计时起点） */

/* ══ 忙状态（输入侧"忙时丢弃"判据；见 compositor.h）════════════════════════
 * 全是 volatile 标量：写方 = 渲染任务/持锁的 UX 调用，读方 = input 任务，
 * 不加锁也不撕裂（最坏只是多丢/少丢一次输入，不会读到坏指针）。 */
static volatile int32_t g_busy_depth;         /* 重活嵌套计数 */
static volatile int64_t g_busy_until_ms;      /* 忙尾下限（重活结束 + RC_BUSY_TAIL_MS） */
static volatile bool    g_busy_banner_on;     /* 显式 loading 窗口（地图装载/相机收尾） */
static char             g_busy_why[40];       /* 当前重活名（横幅文案，ASCII 大写） */
static bool             g_busy_banner_drawn;  /* 上一帧忙横幅是否已画（擦除标脏用） */
static int64_t          g_busy_t0_us;         /* 重活起点：只有真"重"（≥60ms）才置忙尾/亮横幅 */
/* 【阈值修正 2026-10-01】原 60ms。扁平缓冲落地后"整屏笔次"常态就是
 * compose 19ms + blit 64ms ≈ 83ms（blit 是 SPI 上屏地板，460KB@~7MB/s），
 * 于是**每一帧都被判成"重活"→ 忙尾永不结束 → input 被永久丢弃**（真机日志
 * 满屏"忙窗结算：LOADING ... 82 ms ≥ 60 ms → 之后 200ms 内丢弃按键/触摸"）。
 * 提到 250ms：只有真正的长任务（地图装载、相机落盘重派发、整幅失效重建）
 * 才置忙；常态帧不再锁输入。 */
#define RC_BUSY_HEAVY_US 250000
#define RC_BANNER_TEXT_CAM_MOVE "CAM MOVING - PLEASE WAIT"
#define RC_BANNER_TEXT_LOADING  "LOADING - PLEASE WAIT"

/* ── R2 整图相机状态（契约 §3.2；详见 compose_region 前的相机块）── */
static bool    g_cam_on;                   /* true = 当前包为整图（相机可用） */
static int32_t g_cam_vw, g_cam_vh;         /* 整图世界尺寸 */
static int32_t g_cam_fov_w, g_cam_fov_h;   /* 可见窗口（世界 px）= 屏/RC_SCALE */
static int32_t g_cam_x, g_cam_y;           /* 相机 = 可见窗口左上角世界坐标（已夹取） */
static int32_t g_cam_max_x, g_cam_max_y;   /* 可平移上限（0 = 无余量/居中） */
static int32_t g_cam_ref_cx;               /* 导出参考相机中心（= vw/2） */
static mpak_t  g_bgmap; static bool g_bgmap_ok;   /* BGMAP 句柄常开（流式读 + 地面表） */
static uint32_t g_cam_static_off, g_cam_tile_off;         /* payload 相对 */
static uint32_t g_cam_mask_off;                            /* tile 掩码 payload 相对 */
static uint32_t g_cam_ground_off, g_cam_ground_len;        /* 地面表 payload 相对 */
static bool    g_cam_tile_on, g_cam_ground_on;
static rc_wincache_t g_wc_static, g_wc_tile;
static uint32_t s_cam_io_rows, s_cam_io_cols, s_cam_io_bytes;   /* 本轮同步的实际读量 */
static uint32_t s_cam_strip_fopens;                             /* 本轮条带按需开文件次数 */

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
/* 【自适应闸门 2026-10-01】最近一笔 compose+blit 的耗时（ms）。
 * 用途：条带时间滚动每推进一次就要"补缓存 + 重算带行"，在**逐行包**下这笔钱是
 * 1.8~2.4s（真机：天空之城每 ~2.2s 一次 2.4s 重填 ⇒ 设备常态饱和 ⇒ 整体像卡死）。
 * 这里把"上一笔是否已经超过 80ms"作为闸门：**忙就不推进滚动相位**（滚动让位于
 * 交互流畅；画面内容照常全层渲染，不隐藏任何图层）。等分块(tile)包到位、单笔降到
 * 十几毫秒后，闸门自然长期放开，滚动动画自动恢复满速。 */
static volatile uint32_t g_last_frame_ms;
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
/* 【按板定相机下移 2026-10-01】原为全局宏 MP_GROUND_CAM_SHIFT_PX——多个开发
 * 会话共用工作区时互相覆盖（一方实验改值被另一方构建被动带上 → "背景变了"
 * 用户报障）。改为 profile 字段 ground_cam_shift_px：amoled216=0 恢复
 * 原景（用户定稿：图2 蘑菇屋场景+宠物站地面线即正确）。render_init 从
 * profile 装载。 */
static int32_t s_ground_shift_px;       /* 0=实验关闭（定稿值） */

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

/* 站位（需持锁）。返回是否真的改了位置。
 * 【屏中回归制 2026-10-01 用户定稿】开机/换图后宠物固定回归**屏幕正中间**
 * （锚点=屏心），不再追地面线——旧地面线站位在新装扮包布局下把画布顶放到
 * y=460，角色沉底只露头发（真机帧实测：落点=(240,460 194x20)）。调参收尾
 * 同样回到这里（需求文档 §5.2 宠物锚定语义）。 */
static bool ent_stand_on_ground_locked(void)
{
    if (!g_inited || !g_ent_cbox_ok) return false;
    /* 锚点屏幕 y = 屏心 + CENTER_OFF_Y + base_wy×2 + drag_y ⇒ 令其等于屏心 */
    int32_t dy = -(RC_ENT_CENTER_OFF_Y + (g_ent_base_wy << RC_SCALE_SHIFT));
    drag_clamp(NULL, &dy);            /* 兜底夹取 */
    if (dy == g_drag_off_y) return false;
    g_drag_off_y = dy;
    mark_rect(0, 0, g_sw, g_sh);      /* 位置变了：整屏重合成（罕见事件） */
    ESP_LOGI(TAG, "站位：宠物回归屏幕正中间（锚点 y=%d，drag_y=%d）",
             (int)(g_sh / 2), (int)dy);
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

    /* 【内部 RAM 腾挪 2026-10-02】部件缓存链表节点（48B/个，纸娃娃全装扮
     * 可达上百个 = 数 KB）原走默认堆 → 内部 RAM。节点只是指针+指纹元数据，
     * 像素在 psram()（PSRAM）里 → 节点本身放 PSRAM 完全等价。 */
    rc_part_img_t *e = mp_psram_malloc(sizeof *e);
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

    /* 持续刷新帧源注册（RAMless 面板用）；216（GRAM）为空操作 */
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
    /* 【除零护栏 2026-10-01】w 来自包内 part 宽（u16）：损坏/异常包给出 0 时，
     * `% 0` 在 Xtensa 上是**整数除零异常 = panic**（本文件历史上没有这层校验；
     * strip_load 现在也拒收 w/h<=0 的包，这里再兜一次，双保险）。 */
    if (w <= 0) return 0;
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
                /* 【空指针根因 2026-10-01】mask==NULL（该件无 alpha / blend 未置位 /
                 * 掩码分配或读取失败）时，这里原来直接 s->mask[...] = **空指针解引用
                 * → Guru Meditation(LoadProhibited)**；而下面的逐像素回退分支是
                 * 有 `!s->mask ||` 判空的（按不透明处理）。两处口径必须一致：
                 * 无掩码 = 整组不透明（0xFF），走同一条 2x 展开直写。 */
                uint8_t mb = s->mask ? s->mask[bit0 >> 3] : 0xFFu;
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

/* ══════════════════════════════════════════════════════════════════════════
 * R2 整图相机 + 分块流式窗口缓存（契约 docs/ai/map-fullmap-firmware-contract.md §3.2）
 *
 * 用户硬约束（需求 §5.2）：整图包（vw/vh ≫ 屏，如 000010000 = 2270×1807）下
 *   · 相机可任意平移覆盖整张地图（永不出图边界；图小于窗口时居中）；
 *   · **世界像素比例恒定**：世界 1x → 屏 2x 整倍最近邻，零插值/零半像素/零重采样；
 *   · 拖动只补新露出的边条（增量读）；相机静止时**零 TF 读**。
 *
 * 三层结构：
 *   ① 相机状态 g_cam_x/y = 可见窗口左上角世界坐标；窗口 = fov = 屏/RC_SCALE；
 *   ② 每层一块 PSRAM 窗口缓存（rc_wincache_t）= 可见窗口 ± 余量（336 ⇒ 余 48）；
 *      锚点不变 → 零 IO；小幅平移 → memmove + 只读新露出的边条；跳变 → 整窗重载；
 *   ③ 合成：static → 条带 → tile（与旧路径同序），逐屏像素按
 *      world = (cam + screen/2) 采样（⇒ 每个世界像素正好铺 2×2 同色块）。
 *
 * 条带世界系语义（服务端 AssetExporter.RenderFullMapBand 明示的设备规则）：
 *   · y = 带图顶边世界 y ⇒ 屏 y = (y − cam_y)·2；
 *   · 带宽 == vw 的带 = **世界对齐层**（整幅从世界 x=0 起绘制，不周期平铺）；
 *     带宽 < vw 的滚动带仍按周期平铺 —— 但相位锚在**世界原点**（旧实现锚在
 *     屏幕 x=0 ⇒ 相位随相机漂移，"沿用周期平铺会相位错"即此）；
 *   · 视差 Δ = floor((ref_cx − cam_cx)·rx/100) + 时间滚动 + IMU 视差：导出带图
 *     已含"参考相机（地图中心 = vw/2）"下的视差位移，故设备只叠加相对量
 *     （与桌面 DrawBackViewport `worldX += camCenterX·(100+rx)/100` 同源同向）。
 *
 * 内存（PSRAM 常驻，见文件尾 RENDER_PSRAM_BUDGET；整图模式**不再分配**
 * g_static/g_tile/g_tile_mask 的屏尺寸 949KB ⇒ 相对旧口径净省 ≈384KB）：
 *   static 336×336×2 = 226KB；tile 226KB + 掩码 14KB；
 *   4 条带（000010000 实测 850×222 / 2270×260 / 613×125 / 2270×508）
 *   窗口缓存 634KB + 掩码 40KB ⇒ 合计 ≈ **1.14MB**（硬预算 ≤1.2MB）。
 *   ⚠️ 条带源图全驻留需 5.9MB（实测：2270×508 单条 2.45MB）⇒ 必须流式，
 *   条带 PARTS 句柄常开、按行直读（mpak_read_at，布局见 mpak.h）。
 *
 * 性能（SDMMC 1-bit 实效 2-4MB/s）：1 世界 px 平移 ≈ 读 336×2B×层 ≈1.7KB；
 *   100 世界 px 平移 ≈ 67KB/层 ⇒ 拖动 8-15fps（帧内增量读 + 整屏重合成）；
 *   静止 = 零 TF 读、30fps 满帧。
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── 纯计算段（无 ESP/PSRAM 依赖）：/tmp/cam_math_test.c 按 RC_PURE_BEGIN/END
 * 标记抽取**本段原文**编译断言 ⇒ host 自证的就是固件在跑的数学。 ── */
/* RC_PURE_BEGIN */

/* 非负取模 */
static int32_t rc_mod(int32_t v, int32_t m)
{
    if (m <= 0) return 0;
    int32_t r = v % m;
    if (r < 0) r += m;
    return r;
}

/* 向下取整除法（负数亦然；视差相位按 floor 口径对齐桌面/服务端） */
static int32_t rc_floor_div(int32_t a, int32_t b)
{
    int32_t q = a / b, r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) q--;
    return q;
}

/* 屏 px ↔ 世界 px：恒 ×RC_SCALE 整倍（屏上成对像素共享同一世界像素 = 无插值） */
static int32_t rc_world_of_screen(int32_t screen_px, int32_t cam_world)
{
    return cam_world + (screen_px >> RC_SCALE_SHIFT);
}
static int32_t rc_screen_of_world(int32_t world_px, int32_t cam_world)
{
    return (world_px - cam_world) << RC_SCALE_SHIFT;
}

/* ══ 相机口径三件套（**同源**：起始相机 / 置中 / 参考相机中心，P0-2）══
 *  · rc_cam_home(sw,fov)      = 窗口居中时的相机（可见窗口左上角）；
 *  · rc_cam_ref_center(sw,fov)= 该居中的**窗口中心** = 导出参考相机中心（服务端整图导出
 *    相机 = 地图中心：worldW/2；bbox 原点下两者恒等，见下恒等式断言）；
 *  · 采样相位用的 cam_cx = g_cam_x + fov/2（当前窗口中心）。
 * 恒等式（**对所有 sw/fov 成立，含图 ≤ 窗口**）：rc_cam_home + fov/2 == rc_cam_ref_center == sw/2。
 * （sw ≤ fov 时 rc_cam_home 为居中负值，故 sw=200/fov=240 得 −20，中心 100 = sw/2 ✓）
 * 旧代码三处分别写 max/2、vw/2 —— 数值相等但非一处来源，本组函数把它们钉成同源。 */
static int32_t rc_cam_home(int32_t sw, int32_t fov)
{
    if (sw <= fov) return -((fov - sw) / 2);   /* 图 ≤ 窗口：居中（相机为负，图心=屏心） */
    return (sw - fov) / 2;                     /* 图 > 窗口：留出两侧等量余量 */
}
static int32_t rc_cam_ref_center(int32_t sw, int32_t fov)
{
    return rc_cam_home(sw, fov) + fov / 2;
}

/* 相机单轴夹取：图 > 窗口 → [0, sw-fov]（永不出边界）；图 ≤ 窗口 → 居中（cam 负值） */
static int32_t rc_cam_clamp_axis(int32_t want, int32_t sw, int32_t fov)
{
    if (sw <= fov) return -((fov - sw) / 2);
    int32_t hi = sw - fov;
    if (want < 0) return 0;
    if (want > hi) return hi;
    return want;
}

/* 线性窗口锚点：保证 need [n, n+fov) ⊆ [a, a+cw)（源 [0,sw)）；
 * 源 ≤ 缓存 → a=0（整源入窗）；否则 a = clamp(n − margin, 0, sw−cw) 并做覆盖校正。 */
static int32_t rc_anchor_linear(int32_t n, int32_t sw, int32_t cw, int32_t fov, int32_t margin)
{
    if (sw <= cw) return 0;
    int32_t hi = sw - cw;
    int32_t a = n - margin;
    if (a < 0) a = 0;
    if (a > hi) a = hi;
    if (a > n) a = n;                                    /* 锚点必须 ≤ need */
    if (n + fov > a + cw) a = n + fov - cw;              /* need 末尾必须被覆盖 */
    if (a > hi) a = hi;
    if (a < 0) a = 0;
    return a;
}

/* 锚点保持（命中零 IO）或重定位：
 * 当前缓存仍覆盖 need [n, n+fov) → **锚点不动**（相机在余量内小幅移动 = 零 TF 读）；
 * 否则重定位到 need − margin（线性夹取 / 循环 mod）。这是"拖动只补新露出边条"的关键：
 * 每 margin 个世界 px 才触发一次补边条（读 margin 列），摊销 = 1 列/世界 px。 */
static int32_t rc_anchor_hold(int32_t cur, bool valid, int32_t n, int32_t sw, int32_t cw,
                              int32_t fov, int32_t margin, bool wrap)
{
    if (valid) {
        if (!wrap) {
            if (cur <= n && n + fov <= cur + cw) return cur;
        } else if (sw > 0) {
            if (rc_mod(n - cur, sw) + fov <= cw) return cur;
        }
    }
    if (wrap) {
        if (sw <= cw) return 0;
        return rc_mod(n - margin, sw);
    }
    return rc_anchor_linear(n, sw, cw, fov, margin);
}

/* 窗口平移决策（返回**内容搬移量**）：
 *   0 = 命中（零 IO）；>0 = 内容向低索引搬 d（新内容补在尾部）；
 *   <0 = 向高索引搬 |d|（新内容补在首部）；RC_WC_RELOAD = 跳变超窗 → 整窗重载。
 * wrap=true：锚点在 [0,span) 循环（周期平铺带），取较小搬移方向。 */
#define RC_WC_RELOAD 0x3FFF
static int32_t rc_shift_plan(int32_t old_a, int32_t new_a, int32_t span, int32_t cw, bool wrap)
{
    if (old_a == new_a) return 0;
    if (!wrap) {
        int32_t d = new_a - old_a;
        if (d >= cw || d <= -cw) return RC_WC_RELOAD;
        return d;
    }
    int32_t fwd = rc_mod(new_a - old_a, span);
    if (fwd == 0) return 0;
    if (fwd <= cw / 2) return fwd;
    int32_t bwd = span - fwd;
    if (bwd <= cw / 2) return -bwd;
    return RC_WC_RELOAD;
}

/* 增量读的"新露出边条"（实现与自证共用）：shift>0 → 缓存尾段 [cw-shift, cw)；
 * shift<0 → 缓存首段 [0, -shift)；返回该段在**新锚点**下的源坐标区间。 */
static void rc_band_rect(int32_t shift, int32_t cw, int32_t new_a, int32_t span, bool wrap,
                         int32_t *dst_lo, int32_t *dst_hi, int32_t *src_lo, int32_t *src_hi)
{
    if (shift > 0) {
        *dst_lo = cw - shift; *dst_hi = cw;
        *src_lo = new_a + cw - shift; *src_hi = new_a + cw;
    } else {
        *dst_lo = 0; *dst_hi = -shift;
        *src_lo = new_a; *src_hi = new_a - shift;
    }
    if (wrap && span > 0) *src_lo = rc_mod(*src_lo, span);
    *src_hi = *src_lo + (*dst_hi - *dst_lo);
}

/* 视差符号（真机看感判定点，一处可翻转）：
 *   +1 = 条带世界位置随相机同向偏移 rx%（⇒ 屏上比 static 慢 rx%，远端"滞后"观感）；
 *   -1 = 反向（条带越过 static，"超前"观感）。
 * 依据：契约 §3.2「rx_parallax（相机偏移×系数）」+ 带图世界锚定（服务端四条带
 * 实测列 0..宽-1 全不透明 ⇒ 世界对齐带）。桌面 DrawBackViewport 是**屏幕锚定**模型
 * （worldX += camCenterX·(100+rx)/100，rx=0 时层钉在屏上），设备用世界锚定带无法
 * 逐式复刻，只能按契约在世界系里加"相机偏移×rx%"这一项；符号取"远端滞后"= 物理常规。 */
#define RC_CAM_PARALLAX_SIGN  (+1)

/* 条带水平相位（世界 px）= 相机视差项（逐帧实时）+ 量化时间/IMU 项（刷新窗口推进）。
 * 量化理由（P1-1）：相位若逐帧实时推进，而脏区标脏受 4Hz/交互冻结限制 ⇒ 局部脏区
 * （如宠物区）会按新相位重画条带、而带内其余部分还是旧相位 = 带内 1px 相位缝。
 * 故时间/IMU 项一律走 off_q（与旧口径 last_off 同款"量化值"），**绘制与缓存同源**；
 * 相机视差项必须逐帧（拖拽期条带与 static 同移），其变化一律伴随整屏标脏。 */
static int32_t rc_strip_delta_q(int32_t ref_cx, int32_t cam_cx, int32_t rx, int32_t off_q)
{
    return RC_CAM_PARALLAX_SIGN * rc_floor_div((int32_t)((int64_t)(ref_cx - cam_cx) * rx), 100)
           + off_q;
}

/* 条带源列：**世界对齐**采样（旧实现锚在屏幕 x=0 ⇒ 相位随相机漂移）。
 * wrap=false（带宽 ≥ vw 的世界对齐带）越界返回 -1 = 该列不画（露出下层）。 */
static int32_t rc_strip_src_col(int32_t cam_x, int32_t screen_x, int32_t delta,
                                int32_t bw, bool wrap)
{
    int32_t sc = cam_x + (screen_x >> RC_SCALE_SHIFT) + delta;
    if (wrap) return rc_mod(sc, bw);
    return (sc >= 0 && sc < bw) ? sc : -1;
}
/* RC_PURE_END */

/* 屏幕列 → 缓存列映射表（每层合成前重建：480 次迭代，零堆分配） */
static int16_t  s_xmap[RC_MAX_W];
static uint8_t  s_wc_maskline[RC_CAM_CACHE_W];                    /* 掩码 1B/px 行暂存 */
static uint8_t  s_wc_bitbuf[(RC_CAM_CACHE_W + 7) / 8 + 2];        /* tight 位图行暂存 */
#define RC_WC_COV_ROW_MAX  ((RC_CAM_CACHE_W + 7) / 8)              /* 掩码行搬移暂存上限 */

/* ── 契约 §3.1 分块读接口：弱引用 ─────────────────────────────────────────
 * F1（mpak 层）并行施工中：接口未落地时符号为 NULL → 走下方"桥接行读"
 * （同样只用 mpak_read_at 公开原语 + mpak.h 已冻结的整图行布局），
 * 落地后链接自动绑定强符号 ⇒ 本文件零改动切到真接口。 */
#pragma weak mpak_bgmap_read_static_rect
#pragma weak mpak_bgmap_read_tile_rect
#pragma weak mpak_bgmap_read_tile_mask_rect
#pragma weak mpak_bgmap_ground_y

static bool cam_rect_api_ready(void)
{
    return mpak_bgmap_read_static_rect && mpak_bgmap_read_tile_rect &&
           mpak_bgmap_read_tile_mask_rect;
}

/* ── 1bit 掩码（缓存内）存取 ── */
static inline uint32_t wc_cov_rowbytes(const rc_wincache_t *wc)
{
    return ((uint32_t)wc->cw + 7u) / 8u;
}
static inline bool wc_cov_get(const rc_wincache_t *wc, int32_t cx, int32_t cy)
{
    if (!wc->cov) return true;
    /* 【边界护栏 2026-10-01】掩码行是 (cw+7)/8 字节/行的紧凑位图：越界列会
     * 读到**下一行**的位（表现为地图上随机"该透明处画了东西"），越界行直接
     * 越出分配块（用户报过一次调参期 Guru/panic，这里是必须钉死的一类下标）。
     * 越界一律按"透明"处理——合成结果与"缓存未覆盖"同义，不会画错内容。 */
    if (cx < 0 || cy < 0 || cx >= wc->cw || cy >= wc->ch) return false;
    uint32_t bit = (uint32_t)cy * wc_cov_rowbytes(wc) * 8u + (uint32_t)cx;
    return (wc->cov[bit >> 3] >> (7 - (bit & 7))) & 1u;
}
static inline void wc_cov_put(rc_wincache_t *wc, int32_t cx, int32_t cy, bool v)
{
    if (!wc->cov) return;
    if (cx < 0 || cy < 0 || cx >= wc->cw || cy >= wc->ch) return;   /* 同上：越界丢弃 */
    uint32_t bit = (uint32_t)cy * wc_cov_rowbytes(wc) * 8u + (uint32_t)cx;
    if (v) wc->cov[bit >> 3] |= (uint8_t)(1u << (7 - (bit & 7)));
    else   wc->cov[bit >> 3] &= (uint8_t)~(1u << (7 - (bit & 7)));
}
static void wc_cov_clear_range(rc_wincache_t *wc, int32_t cy, int32_t cx0, int32_t n)
{
    for (int32_t i = 0; i < n; i++) wc_cov_put(wc, cx0 + i, cy, false);
}
/* 行内搬移（mv>0 左移）：新列 i ← 旧列 i+mv */
static void wc_cov_shift_row(rc_wincache_t *wc, int32_t cy, int32_t mv)
{
    if (!wc->cov || mv == 0) return;
    uint32_t rb = wc_cov_rowbytes(wc);
    uint8_t tmp[RC_WC_COV_ROW_MAX];
    /* 结构化护栏：缓存加宽到超过本暂存（RC_CAM_CACHE_W 改大）时**不静默丢掩码**——
     * 调用方 wc_sync_to 已按同一常量预判并走整窗 RELOAD；此处仅兜底。 */
    if (rb > RC_WC_COV_ROW_MAX) return;
    uint8_t *row = wc->cov + (size_t)cy * rb;
    memcpy(tmp, row, rb);
    for (int32_t i = 0; i < wc->cw; i++) {
        int32_t j = i + mv;
        bool v = false;
        if (j >= 0 && j < wc->cw) {
            uint32_t b = (uint32_t)j;
            v = (tmp[b >> 3] >> (7 - (b & 7))) & 1u;
        }
        uint32_t bi = (uint32_t)i;
        if (v) row[bi >> 3] |= (uint8_t)(1u << (7 - (bi & 7)));
        else   row[bi >> 3] &= (uint8_t)~(1u << (7 - (bi & 7)));
    }
    for (uint32_t b = (uint32_t)wc->cw; b < rb * 8u; b++)       /* 行尾填充位清零 */
        row[b >> 3] &= (uint8_t)~(1u << (7 - (b & 7)));
}
/* 行搬移（按行字节；行对齐 ⇒ 纯 memmove） */
static void wc_cov_move_rows(rc_wincache_t *wc, int32_t dst_row, int32_t src_row, int32_t rows)
{
    if (!wc->cov || rows <= 0) return;
    uint32_t rb = wc_cov_rowbytes(wc);
    memmove(wc->cov + (size_t)dst_row * rb, wc->cov + (size_t)src_row * rb,
            (size_t)rows * rb);
}
/* 把 1B/像素掩码行写进缓存行 */
static void wc_cov_put_1b(rc_wincache_t *wc, int32_t cy, int32_t cx0, const uint8_t *src, int32_t n)
{
    for (int32_t i = 0; i < n; i++) wc_cov_put(wc, cx0 + i, cy, src[i] != 0);
}
/* 把 tight 位图（bit0 = buf 内起始位）一段写进缓存行 */
static void wc_cov_put_bits(rc_wincache_t *wc, int32_t cy, int32_t cx0,
                            const uint8_t *buf, int32_t bit0, int32_t n)
{
    for (int32_t i = 0; i < n; i++) {
        int32_t b = bit0 + i;
        wc_cov_put(wc, cx0 + i, cy, (buf[b >> 3] >> (7 - (b & 7))) & 1u);
    }
}

/* ── 源行读（一层一行）─────────────────────────────────────────────────── */
/* 【行指针护栏 2026-10-01】dst = px + (sy−ay)*cw + cx0 是本文件最容易越界的
 * 一处（窗口缓存行指针）：sy/cx0/n 由"锚点+边条"算出来，一旦锚点搬移与坐标系
 * 不同源就会写穿 PSRAM（用户报过一次调参期 Guru/panic）。这里在真正读写前
 * 把行号与列区间夹一遍：越界直接返回错误码（调用方按"本层缺失"处理，不写内存）。 */
static bool wc_row_args_ok(const rc_wincache_t *wc, int32_t sy, int32_t sx0,
                           int32_t n, int32_t cx0)
{
    if (!wc->px || n <= 0) return false;
    int32_t cy = sy - wc->ay;
    if (cy < 0 || cy >= wc->ch) return false;
    if (cx0 < 0 || cx0 + n > wc->cw) return false;
    if (sx0 < 0) return false;
    return true;
}

/* ══ 【条带层预读 2026-10-01】═════════════════════════════════════════════
 * 真机取数：整图窗口缓存填充 `3144 行 / 1112 KB / 9.4~14 s`，条带层占大头。
 * 条带像素走**裸 pread**（包 <4MB，mpak_open 每次都会全量 CRC32C，拖动期不可行），
 * 而原实现是**每行一次 pread**（672~800B/次）：开销全在 syscall+FATFS 簇定位
 * + SD 命令往返上，与字节数无关。修法与 mpak 的预读块同思路：**按 64KB 预读、
 * 行从块内 memcpy**。像素与掩码各一份单例（两者逐行交替读，共用一份会互冲）。
 * 命中判定：同 fd 且请求区间落在块内 → 零 syscall。 */
#define WC_RA_PX_CAP  (64u * 1024u)
#define WC_RA_CV_CAP  (8u * 1024u)
static uint8_t *s_ra_px; static size_t s_ra_px_cap;
static uint8_t *s_ra_cv; static size_t s_ra_cv_cap;
static int      s_ra_px_fd = -1; static uint32_t s_ra_px_off, s_ra_px_len;
static int      s_ra_cv_fd = -1; static uint32_t s_ra_cv_off, s_ra_cv_len;

static uint8_t *wc_ra(uint8_t **buf, size_t *cap, int *fd, uint32_t *boff, uint32_t *blen,
                      size_t want_cap, int want_fd, uint32_t off, size_t need)
{
    if (!*buf) {
        *buf = heap_caps_malloc(want_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!*buf) return NULL;
        *cap = want_cap;
    }
    if (*fd == want_fd && off >= *boff && (off + need) <= (*boff + *blen)) {
        return *buf + (off - *boff);                     /* 命中：零 syscall */
    }
    size_t want = *cap;
    if (want < need) want = need;
    ssize_t got = pread(want_fd, *buf, want, (off_t)off);
    if (got <= 0) { *fd = -1; *blen = 0; return NULL; }
    *fd = want_fd; *boff = off; *blen = (uint32_t)got;
    if ((size_t)got < need) return NULL;                 /* 文件尾不足 */
    return *buf;
}

static int wc_read_px_row(rc_wincache_t *wc, int32_t sy, int32_t sx0, int32_t n, int32_t cx0)
{
    if (n <= 0) return 0;
    if (!wc_row_args_ok(wc, sy, sx0, n, cx0)) {
        ESP_LOGE(TAG, "窗口缓存行越界（kind=%u sy=%d ay=%d ch=%d sx0=%d n=%d cx0=%d cw=%d）→ 丢弃该行",
                 (unsigned)wc->kind, (int)sy, (int)wc->ay, (int)wc->ch,
                 (int)sx0, (int)n, (int)cx0, (int)wc->cw);
        return MPAK_ERR_RANGE;
    }
    uint16_t *dst = wc->px + (size_t)(sy - wc->ay) * (size_t)wc->cw + cx0;
    s_cam_io_rows++; s_cam_io_bytes += (uint32_t)n * 2u;
    if (wc->kind == RC_WC_STRIP) {
        /* ── 瓦片存储的条带（契约 §2/§4/§6）────────────────────────────────
         * 与 static/tile 同一套口径：按**条带自身**的网格取，只有整块 pread
         * （T*T*2），命中即 memcpy。条带包不走 mpak 句柄（见下方裸 pread 的理由），
         * 所以这里用 wc 上的 fd + 瓦片身份手工构造瓦片源。 */
        if (wc->tiled) {
            if (wc->fd < 0) return MPAK_ERR_IO;
            mpak_tile_src_t ts;
            mpak_tile_src_init(&ts, wc->tile_fid, wc->fd, wc->px_off,
                               wc->sw, wc->sh, (int32_t)wc->tile);
            return mpak_tile_read_px(&ts, sx0, sy, n, 1, dst, wc->cw);
        }
        /* 裸 pread：装载期已由 mpak_open 校验过信封/长度（含 CRC），这里只按偏移取像素。
         * 原因：条带包 0.15~2.45MB < MPAK_CRC_SKIP_BYTES(4MB) ⇒ 每次 mpak_open 都会
         * 全量 CRC32C（秒级），拖动期开合不可行；裸读一次几十 µs 且不占常驻句柄。 */
        if (wc->fd < 0) return MPAK_ERR_IO;
        uint32_t off = wc->px_off + (uint32_t)sy * wc->row_bytes + (uint32_t)sx0 * 2u;
        const uint8_t *p = wc_ra(&s_ra_px, &s_ra_px_cap, &s_ra_px_fd, &s_ra_px_off,
                                 &s_ra_px_len, WC_RA_PX_CAP, wc->fd, off, (size_t)n * 2u);
        if (p) { memcpy(dst, p, (size_t)n * 2u); return MPAK_OK; }
        return pread(wc->fd, dst, (size_t)n * 2u, (off_t)off) == (ssize_t)((size_t)n * 2u)
               ? MPAK_OK : MPAK_ERR_IO;                   /* 预读失败退回逐行 */
    }
    if (mpak_bgmap_read_static_rect) {
        return (wc->kind == RC_WC_TILE)
            ? mpak_bgmap_read_tile_rect(wc->pkg, sx0, sy, n, 1, dst, wc->cw)
            : mpak_bgmap_read_static_rect(wc->pkg, sx0, sy, n, 1, dst, wc->cw);
    }
    /* 桥接：直读整图行（static_back_off/tile_layer_off + 行 4B 对齐，与 layer_rgb_load 同口径） */
    {
        uint32_t layer_off = (wc->kind == RC_WC_TILE) ? g_cam_tile_off : g_cam_static_off;
        uint32_t off = wc->pkg->payload_off + layer_off
                     + (uint32_t)sy * wc->row_bytes + (uint32_t)sx0 * 2u;
        return mpak_read_at(wc->pkg, off, dst, (size_t)n * 2u);
    }
}

static int wc_read_cov_row(rc_wincache_t *wc, int32_t sy, int32_t sx0, int32_t n, int32_t cx0)
{
    if (!wc->cov || n <= 0) return 0;
    if (!wc_row_args_ok(wc, sy, sx0, n, cx0)) {
        ESP_LOGE(TAG, "窗口缓存掩码行越界（kind=%u sy=%d ay=%d ch=%d sx0=%d n=%d cx0=%d cw=%d）→ 丢弃该行",
                 (unsigned)wc->kind, (int)sy, (int)wc->ay, (int)wc->ch,
                 (int)sx0, (int)n, (int)cx0, (int)wc->cw);
        return MPAK_ERR_RANGE;
    }
    s_cam_io_rows++; s_cam_io_bytes += (uint32_t)((n + 7) / 8);
    if (wc->kind == RC_WC_STRIP && wc->tiled) {
        /* 瓦片存储的条带掩码：块内 T/8 字节/行、MSB-first（契约 §4，口径同 tile 层掩码） */
        if (wc->fd < 0) return MPAK_ERR_IO;
        mpak_tile_src_t ts;
        mpak_tile_src_init(&ts, wc->tile_fid, wc->fd, wc->cov_off,
                           wc->sw, wc->sh, (int32_t)wc->tile);
        int rc = mpak_tile_read_mask(&ts, sx0, sy, n, 1, s_wc_maskline, n);
        if (rc != MPAK_OK) return rc;
        wc_cov_put_1b(wc, sy - wc->ay, cx0, s_wc_maskline, n);
        return 0;
    }
    if (wc->kind != RC_WC_STRIP && mpak_bgmap_read_tile_mask_rect) {
        int rc = mpak_bgmap_read_tile_mask_rect(wc->pkg, sx0, sy, n, 1, s_wc_maskline, n);
        if (rc != MPAK_OK) return rc;
        wc_cov_put_1b(wc, sy - wc->ay, cx0, s_wc_maskline, n);
        return 0;
    }
    /* tight 位图直读：strip = 带内 bit(y*w+x)；tile 桥接 = 整图 bit(y*vw+x) */
    {
        int32_t span = (wc->kind == RC_WC_STRIP) ? wc->sw : g_cam_vw;
        uint32_t bit0 = (uint32_t)sy * (uint32_t)span + (uint32_t)sx0;
        uint32_t nb = ((uint32_t)n + 7u) / 8u + 1u;
        if (nb > sizeof s_wc_bitbuf) nb = sizeof s_wc_bitbuf;
        int rc;
        if (wc->kind == RC_WC_STRIP) {                 /* 裸 pread + 预读（同像素行口径） */
            if (wc->fd < 0) return MPAK_ERR_IO;
            uint32_t off = wc->cov_off + (bit0 >> 3);
            const uint8_t *p = wc_ra(&s_ra_cv, &s_ra_cv_cap, &s_ra_cv_fd, &s_ra_cv_off,
                                     &s_ra_cv_len, WC_RA_CV_CAP, wc->fd, off, nb);
            if (p) { memcpy(s_wc_bitbuf, p, nb); rc = MPAK_OK; }
            else rc = (pread(wc->fd, s_wc_bitbuf, nb, (off_t)off) == (ssize_t)nb)
                      ? MPAK_OK : MPAK_ERR_IO;
        } else {
            rc = mpak_read_at(wc->pkg, wc->cov_off + (bit0 >> 3), s_wc_bitbuf, nb);
        }
        if (rc != MPAK_OK) return rc;
        wc_cov_put_bits(wc, sy - wc->ay, cx0, s_wc_bitbuf, (int32_t)(bit0 & 7u), n);
        return 0;
    }
}

/* 把缓存矩形 [cx0,cx0+w)×[cy0,cy0+h) 填成对应源像素（源越界 = 0/透明）。
 * 先清后读：保证越界与无数据处不含上一任残留（历史坑：未初始化掩码 = 黑斑/竖条纹）。 */
static int wc_fill_cache(rc_wincache_t *wc, int32_t cx0, int32_t cy0, int32_t w, int32_t h)
{
    if (!wc->px) return MPAK_ERR_ARG;
    int32_t x0 = cx0 < 0 ? 0 : cx0, y0 = cy0 < 0 ? 0 : cy0;
    int32_t x1 = cx0 + w, y1 = cy0 + h;
    if (x1 > wc->cw) x1 = wc->cw;
    if (y1 > wc->ch) y1 = wc->ch;
    if (x0 >= x1 || y0 >= y1) return 0;
    int rc = 0;

    /* ── 【按带读 2026-10-01】非 wrap 层（static/tile/世界对齐条带）整段整段读 ──
     * 原实现每行一次读（static/tile 走 mpak 分块接口 h=1）⇒ 预读块只能覆盖"这一行"，
     * 每次都 miss（真机：全窗填充 4696 行 / 11.35s，仍 2.4ms/行）。
     * 改成一次请求 band 行（h=band）⇒ 预读块一次 64KB 覆盖 ~14 行，
     * 命令数下降一个数量级。wrap（周期平铺）条带仍走下面的逐行分支（语义复杂），
     * 但它的像素/掩码读已由 wc_ra 预读兜住。 */
    if (!wc->wrap) {
        const int32_t s0 = wc->ax + x0, s1 = wc->ax + x1;
        int32_t sx0 = s0 < 0 ? 0 : s0, sx1 = s1 > wc->sw ? wc->sw : s1;
        if (sx0 < sx1) {
            const int32_t cstart = x0 + (sx0 - s0);
            for (int32_t cy = y0; cy < y1; ) {
                watchdog_kick();
                int32_t band = y1 - cy;
                if (band > 32) band = 32;
                int32_t sy = wc->ay + cy;
                /* 与源有效行求交（越界行留 0，已在上面的清零步骤做过） */
                int32_t sy0 = sy, sy1 = sy + band;
                if (sy0 < 0) sy0 = 0;
                if (sy1 > wc->sh) sy1 = wc->sh;
                if (sy0 < sy1) {
                    int32_t dy = cy + (sy0 - sy);           /* 目标起始行（缓存行号） */
                    uint16_t *dst = wc->px + (size_t)dy * (size_t)wc->cw + cstart;
                    int32_t rows = sy1 - sy0;
                    s_cam_io_rows += (uint32_t)rows;
                    s_cam_io_bytes += (uint32_t)rows * (uint32_t)(sx1 - sx0) * 2u;
                    if (wc->kind == RC_WC_STRIP) {
                        /* 条带：裸 pread 也不逐行了 —— 用 wc_ra 预读逐行 memcpy */
                        for (int32_t r = 0; r < rows; r++) {
                            rc |= wc_read_px_row(wc, sy0 + r, sx0, sx1 - sx0, cstart);
                            rc |= wc_read_cov_row(wc, sy0 + r, sx0, sx1 - sx0, cstart);
                        }
                    } else {
                        rc |= (wc->kind == RC_WC_TILE)
                            ? mpak_bgmap_read_tile_rect(wc->pkg, sx0, sy0, sx1 - sx0, rows,
                                                        dst, wc->cw)
                            : mpak_bgmap_read_static_rect(wc->pkg, sx0, sy0, sx1 - sx0, rows,
                                                          dst, wc->cw);
                        if (wc->cov) {
                            for (int32_t r = 0; r < rows; r++) {
                                int32_t n = sx1 - sx0;
                                rc |= mpak_bgmap_read_tile_mask_rect(
                                        wc->pkg, sx0, sy0 + r, n, 1, s_wc_maskline, n);
                                wc_cov_put_1b(wc, dy + r, cstart, s_wc_maskline, n);
                                s_cam_io_rows++;
                                s_cam_io_bytes += (uint32_t)((n + 7) / 8);
                            }
                        }
                    }
                }
                cy += band;
            }
            return rc;
        }
        /* 整段都在源外：全 0（上面已清零） */
        return rc;
    }

    for (int32_t cy = y0; cy < y1; cy++) {
        /* 【长任务喂狗 2026-10-01 真机根因】整图包(17MB)首帧窗口缓存填充 =
         * 336 行 × 多层 fread，实测 static 层就 ~2s、整场 >5s → 触发 E14
         * 三振熔断（监控任务 vTaskSuspend(render_task)）→ 设备"卡死"。
         * 这是**合法长任务**，必须主动喂狗：每 16 行一次（≈100ms 粒度）。 */
        if ((cy & 15) == 0) watchdog_kick();
        memset(wc->px + (size_t)cy * wc->cw + x0, 0, (size_t)(x1 - x0) * 2u);
        if (wc->cov) wc_cov_clear_range(wc, cy, x0, x1 - x0);
        int32_t sy = wc->ay + cy;
        if (sy < 0 || sy >= wc->sh) continue;
        if (!wc->wrap) {
            int32_t s0 = wc->ax + x0, s1 = wc->ax + x1;
            if (s0 < 0) s0 = 0;
            if (s1 > wc->sw) s1 = wc->sw;
            if (s0 < s1) {
                int32_t cstart = x0 + (s0 - (wc->ax + x0));
                rc |= wc_read_px_row(wc, sy, s0, s1 - s0, cstart);
                rc |= wc_read_cov_row(wc, sy, s0, s1 - s0, cstart);
            }
        } else {
            /* 护栏：wrap 层的周期 = wc->sw；为 0（包损坏）时下面的 run 会恒为 0
             * → cx 不前进 = 死循环（看门狗熔断黑屏级）。这里直接放弃该行。 */
            if (wc->sw <= 0) continue;
            int32_t s = rc_mod(wc->ax + x0, wc->sw), cx = x0;
            while (cx < x1) {
                int32_t run = wc->sw - s;
                if (run > x1 - cx) run = x1 - cx;
                rc |= wc_read_px_row(wc, sy, s, run, cx);
                rc |= wc_read_cov_row(wc, sy, s, run, cx);
                cx += run;
                s = 0;
            }
        }
    }
    return rc;
}

/* 锚点搬到 (nax,nay)：命中零 IO / 增量搬移+补边条 / 跳变整窗重载 */
static int wc_sync_to(rc_wincache_t *wc, int32_t nax, int32_t nay)
{
    if (!wc->px) return 0;
    if (wc->valid && nax == wc->ax && nay == wc->ay) return 0;
    if (!wc->valid) {
        wc->ax = nax; wc->ay = nay; wc->valid = true;
        return wc_fill_cache(wc, 0, 0, wc->cw, wc->ch);
    }
    int rc = 0;
    /* ① 横向：内容搬移 + 只补新露出的列条（整高） */
    int32_t mvx = rc_shift_plan(wc->ax, nax, wc->sw, wc->cw, wc->wrap);
    if (mvx == RC_WC_RELOAD) {
        wc->ax = nax; wc->ay = nay;
        return wc_fill_cache(wc, 0, 0, wc->cw, wc->ch);
    }
    if (mvx != 0 && wc->cov && wc_cov_rowbytes(wc) > RC_WC_COV_ROW_MAX) {
        wc->ax = nax; wc->ay = nay;                 /* 掩码行缓冲不够 → 整窗重载（不丢掩码） */
        return wc_fill_cache(wc, 0, 0, wc->cw, wc->ch);
    }
    if (mvx != 0) {
        int32_t a = mvx > 0 ? mvx : -mvx;
        for (int32_t cy = 0; cy < wc->ch; cy++) {
            uint16_t *row = wc->px + (size_t)cy * wc->cw;
            if (mvx > 0) memmove(row, row + a, (size_t)(wc->cw - a) * 2u);
            else         memmove(row + a, row, (size_t)(wc->cw - a) * 2u);
            wc_cov_shift_row(wc, cy, mvx);
        }
        int32_t dl, dh, sl, sh2;
        rc_band_rect(mvx, wc->cw, nax, wc->sw, wc->wrap, &dl, &dh, &sl, &sh2);
        wc->ax = nax;                       /* 先定新锚点，补边条按新锚点解源坐标 */
        s_cam_io_cols += (uint32_t)(dh - dl);
        rc |= wc_fill_cache(wc, dl, 0, dh - dl, wc->ch);
        (void)sl; (void)sh2;
    }
    /* ② 纵向：行搬移 + 只补新露出的行条（整宽） */
    int32_t mvy = rc_shift_plan(wc->ay, nay, wc->sh, wc->ch, false);
    if (mvy == RC_WC_RELOAD) {
        wc->ay = nay;
        return wc_fill_cache(wc, 0, 0, wc->cw, wc->ch);
    }
    if (mvy != 0) {
        int32_t a = mvy > 0 ? mvy : -mvy;
        int32_t rows = wc->ch - a;
        if (mvy > 0) {
            memmove(wc->px, wc->px + (size_t)a * wc->cw, (size_t)rows * wc->cw * 2u);
            wc_cov_move_rows(wc, 0, a, rows);
        } else {
            memmove(wc->px + (size_t)a * wc->cw, wc->px, (size_t)rows * wc->cw * 2u);
            wc_cov_move_rows(wc, a, 0, rows);
        }
        wc->ay = nay;
        rc |= wc_fill_cache(wc, 0, mvy > 0 ? wc->ch - a : 0, wc->cw, a);
    }
    return rc;
}

/* 分配窗口缓存（越界部分留 0 = 透明/黑底；sw ≤ CACHE → 整源入窗） */
static bool wc_alloc(rc_wincache_t *wc, uint8_t kind, int32_t sw, int32_t sh, bool cov, bool wrap)
{
    memset(wc, 0, sizeof *wc);
    wc->kind = kind;
    wc->sw = sw;
    wc->sh = sh;
    wc->wrap = wrap;
    wc->cw = (sw < RC_CAM_CACHE_W) ? sw : RC_CAM_CACHE_W;
    wc->ch = (sh < RC_CAM_CACHE_H) ? sh : RC_CAM_CACHE_H;
    if (wc->cw <= 0 || wc->ch <= 0) return false;
    wc->px = psram((size_t)wc->cw * (size_t)wc->ch * 2u);
    if (!wc->px) return false;
    memset(wc->px, 0, (size_t)wc->cw * (size_t)wc->ch * 2u);
    if (cov) {
        size_t rb = (((size_t)wc->cw + 7u) / 8u) * (size_t)wc->ch;
        wc->cov = psram(rb);
        if (!wc->cov) {
            heap_caps_free(wc->px);
            wc->px = NULL;
            return false;
        }
        memset(wc->cov, 0, rb);
    }
    return true;
}

static void wc_free(rc_wincache_t *wc)
{
    if (wc->px) { heap_caps_free(wc->px); wc->px = NULL; }
    if (wc->cov) { heap_caps_free(wc->cov); wc->cov = NULL; }
    wc->valid = false;
}

/* ══ 条带窗口缓存：按需分配 / 离开视野即释放（"每层只驻留可见块"）══════════════
 * 【为什么改】原实现装载期给每条带都分配 cw×ch 的窗口缓存：14 条带图实测常驻
 * 1749KB，而整图预算 ≤1200KB —— 超出的部分全是"没进视野也占着"的死内存；
 * 同一时刻屏上的条带通常只有 1~3 条 ⇒ 直接砍掉十几条带的常驻。
 * 【为什么必须"先存后写"】wc_alloc 首行是 memset(wc,0,sizeof *wc)，会把装载期
 * 落好的源 IO 描述（px_off/row_bytes/cov_off/tiled/tile/…）全清掉。本仓库栽过
 * 一次同类根因（条带全灭：所有 pread 落到偏移 0+行距 0），所以这里把"保存 →
 * wc_alloc → 回写"封在一个函数里，调用点不可能写漏。                        */
static bool strip_wc_alloc(rc_strip_t *s)
{
    rc_wincache_t *wc = &s->wc;
    if (wc->px) return true;
    rc_wincache_t keep = *wc;                  /* 装载期落下的几何 + 源 IO 描述 */
    if (!wc_alloc(wc, RC_WC_STRIP, keep.sw, keep.sh, s->has_cov, keep.wrap)) {
        *wc = keep;                            /* 失败：保留描述，下次可见时再试 */
        return false;
    }
    wc->px_off     = keep.px_off;
    wc->row_bytes  = keep.row_bytes;
    wc->cov_off    = keep.cov_off;
    wc->tiled      = keep.tiled;
    wc->tile       = keep.tile;
    wc->tile_gx    = keep.tile_gx;
    wc->tile_gy    = keep.tile_gy;
    wc->tile_fid   = keep.tile_fid;
    wc->fd         = -1;
    return true;
}

/* ══ 相机同步：把三层缓存搬到当前相机需要的窗口（命中则零 IO）══ */
static int32_t cam_margin_x(const rc_wincache_t *wc)
{
    int32_t m = (wc->cw - g_cam_fov_w) / 2;
    return m > 0 ? m : 0;
}
static int32_t cam_margin_y(const rc_wincache_t *wc)
{
    int32_t m = (wc->ch - g_cam_fov_h) / 2;
    return m > 0 ? m : 0;
}

/* 条带水平相位（世界 px）：相机视差（逐帧）+ 量化时间/IMU 项 s->off_q（刷新窗口推进） */
static int32_t strip_delta_true(const rc_strip_t *s)
{
    int32_t cam_cx = g_cam_x + g_cam_fov_w / 2;
    return rc_strip_delta_q(g_cam_ref_cx, cam_cx, (int32_t)s->rx, s->off_q);
}

/* 扁平缓冲冻结版：frozen=true 时一律用 fb 建立时记下的相位（否则相机平移期间
 * 各层位移不一致 = 条带与地面错位；见 rc_strip_t.frozen 的说明）。 */
static int32_t strip_delta_world(const rc_strip_t *s)
{
    if (s->frozen) return s->delta_frozen;
    return strip_delta_true(s);
}

/* 本层本次是否需要读源（锚点要动 / 首载）—— 句柄只在"确实要读"时才开 */
static bool wc_needs_io(const rc_wincache_t *wc, int32_t nax, int32_t nay)
{
    if (!wc->px) return false;
    if (!wc->valid) return true;
    return (nax != wc->ax || nay != wc->ay);
}

/* ══ 相机同步：把三层缓存搬到当前相机需要的窗口（命中则零 IO）══ */
static int cam_strip_sync(rc_strip_t *s)
{
    if (!s->ok || !s->world) return 0;
    /* 【不在可见范围的条带不读源 · 也不占内存 2026-10-01 二次定稿】
     * 装载/切图耗时 ∝ 读的行数，而多带图（真机 200000000 = 14 条带）里同时落在
     * 屏上的通常只有 1~3 条。原实现给**每条**带都在装载期分配 288×h 的窗口缓存
     * （14 条实测常驻 1749KB > 整图预算 1200KB）——没进视野的层白占 PSRAM。
     * 现在：① 可见性判定用**潜在**几何（不依赖已分配缓存，否则释放后就再也判不进来）；
     * ② 不在可见范围 ⇒ 立即把窗口缓存还给 PSRAM（valid=false，进屏时按同一份
     * 源 IO 描述重新分配 + 整窗补读）。语义与"内容作废、进屏再补"完全一致。 */
    const int32_t mch = (s->h < RC_CAM_CACHE_H) ? (int32_t)s->h : RC_CAM_CACHE_H;
    const int32_t my  = (mch - g_cam_fov_h) / 2 > 0 ? (mch - g_cam_fov_h) / 2 : 0;
    const int32_t mtop = g_cam_y - my;
    const int32_t mbot = g_cam_y + g_cam_fov_h + my;
    const int32_t stop = (int32_t)s->y;
    const int32_t sbot = (int32_t)s->y + (int32_t)s->h;
    if (sbot <= mtop || stop >= mbot) {
        s->wc.valid = false;
        if (s->wc.px) wc_free(&s->wc);         /* 离开视野 → 归还 PSRAM（只驻留可见块） */
        return 0;
    }
    if (!s->wc.px && !strip_wc_alloc(s)) {     /* 进视野 → 这时才分配 */
        ESP_LOGE(TAG, "条带窗口缓存分配失败（%dx%d）→ 本条带本帧不画", (int)s->w, (int)s->h);
        return MPAK_ERR_NOMEM;
    }
    s->delta = strip_delta_world(s);
    int32_t q = g_cam_x + s->delta;                    /* 屏 x=0 对应的源列（未取模/未裁剪） */
    int32_t nay = rc_anchor_hold(s->wc.ay, s->wc.valid, g_cam_y - (int32_t)s->y,
                                 s->wc.sh, s->wc.ch, g_cam_fov_h, cam_margin_y(&s->wc), false);
    int32_t nax = rc_anchor_hold(s->wc.ax, s->wc.valid, q, s->wc.sw, s->wc.cw,
                                 g_cam_fov_w, cam_margin_x(&s->wc), s->wc.wrap);
    if (!wc_needs_io(&s->wc, nax, nay)) return 0;      /* 命中：零 IO、零开文件 */

    /* 按需开合（P0-1）：窗口缓存只在"相机越过 48px 余量"时才补读，完全可以在这一次
     * 补读里 open → pread 若干行 → close。稳态常开句柄因此 = 1（仅 BGMAP）。
     * 不用 mpak_open：条带包 0.15~2.45MB < MPAK_CRC_SKIP_BYTES(4MB) ⇒ 每次 open 都全量
     * CRC32C（秒级），拖动期开合不可行；裸 pread 无 CRC（信封已在装载期校验过）。 */
    int fd = open(s->path, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "整图条带开文件失败 %s（errno=%d）→ 本帧不画该带", s->path, errno);
        s->wc.valid = false;                           /* 下次重试整窗 */
        return MPAK_ERR_IO;
    }
    s->wc.fd = fd;
    s_cam_strip_fopens++;
    int rc = wc_sync_to(&s->wc, nax, nay);
    s->wc.fd = -1;
    close(fd);
    return rc;
}

static void full_recompose(void);     /* 前向：调参模式切换后整屏重合成 */
static void mark_banner_band_dirty(void);   /* 前向：忙横幅擦除/重绘标脏 */
static void mapfb_invalidate(const char *why);   /* 前向：地图扁平缓冲失效 */

/* ── 忙状态实现（见 compositor.h 的口径说明）──
 * 关键取舍：**不是**每个重活窗口都置忙尾。一次性整屏重合成（退出菜单/切时钟/
 * 相机置中）平时只要十几 ms，若每次都置 200ms 忙尾 + 亮横幅，正常操作下按键会
 * 频繁"没反应"、屏上也会闪横幅。所以这里在重活**结束时**结算耗时：≥60ms 才算
 * "真卡了"，才置忙尾（并让 loading 横幅显示到忙尾结束）；快操作只在其自身
 * 执行期间算忙（几 ms，用户感知不到）。 */
void render_busy_enter(const char *why)
{
    if (g_busy_depth == 0) {
        g_busy_t0_us = esp_timer_get_time();
        /* 显式 loading 窗口（地图装载/相机收尾）的文案优先：嵌套进来的
         * full_recompose("RENDERING") 不能把 "MAP LOADING" 顶掉——用户看到的
         * 应该是"正在装地图"，而不是"正在合成"。 */
        if (!g_busy_banner_on && why && why[0]) strlcpy(g_busy_why, why, sizeof g_busy_why);
    }
    g_busy_depth++;
}
void render_busy_leave(void)
{
    if (g_busy_depth <= 0) { g_busy_depth = 0; return; }
    if (--g_busy_depth == 0) {
        int64_t dur = esp_timer_get_time() - g_busy_t0_us;
        g_busy_until_ms = (dur >= RC_BUSY_HEAVY_US)
                          ? (esp_timer_get_time() / 1000 + RC_BUSY_TAIL_MS)
                          : 0;
        if (dur >= RC_BUSY_HEAVY_US) {
            /* 【栈水位探针】本函数可能跑在任何任务里（渲染任务 flush / input 任务
             * 收尾重合成）。用户报过一次"调参中闪退（Guru/panic）"，而本工程历史上
             * 的 panic 有一半是任务栈溢出（bgm 任务 mp3 scratch 16KB 那例）——渲染任务
             * 栈在内部堆紧张时只拿到 3584B（见 main.c render_stacks 降档表）。这条日志
             * 让"是不是栈爆了"在真机上一次重活就能读出来：剩余字节越小越危险。 */
            ESP_LOGW(TAG, "忙窗结算：%s %lld ms ≥ %d ms → 之后 %d ms 内丢弃按键/触摸（防积压）；"
                          "[%s] 栈剩余 %u B",
                     g_busy_why[0] ? g_busy_why : "(未命名)", (long long)(dur / 1000),
                     (int)(RC_BUSY_HEAVY_US / 1000), RC_BUSY_TAIL_MS,
                     pcTaskGetName(NULL),
                     (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
        }
    }
}
bool render_busy(void)
{
    if (g_busy_depth > 0) return true;                        /* 重活进行中 */
    if (g_busy_banner_on) return true;                        /* 显式 loading 窗口 */
    if (g_cam_adjust_lvl >= RC_CAM_ADJ_MINIMAL) return true;   /* 拖动/吸附进行中 */
    return (esp_timer_get_time() / 1000) < g_busy_until_ms;    /* 忙尾 */
}
void render_busy_banner(const char *why, bool on)
{
    if (on) {
        if (why && why[0]) strlcpy(g_busy_why, why, sizeof g_busy_why);
        g_busy_banner_on = true;
    } else {
        g_busy_banner_on = false;
        g_busy_until_ms = esp_timer_get_time() / 1000 + RC_BUSY_TAIL_MS;
    }
    /* 横幅带立刻重绘/擦除（本函数可能从 input 任务调用：拿锁标脏，渲染任务下一拍落地） */
    rc_lock();
    mark_banner_band_dirty();
    rc_unlock();
}

/* 调参档位切换。note：
 *  · level>0 → 相机调参性能模式（条带层不再合成）；
 *  · level>=2 → 极限档（只画 static 底图 + 窗口缓存零 TF 读 + 边缘延展）；
 *  · 进出都整屏重合成一帧（避免"上一层留下的像素"残影）。唯一例外：**进
 *    level 2 不重合成**——level 2 只可能由 render_cam_adjust_motion() 触发，
 *    而那必然紧跟一次相机平移（render_cam_set 已把整屏标脏），重复整屏合成
 *    只是白扔一帧（15Hz 拖动下每帧都贵）。 */
static void cam_adjust_apply_level(int lvl, bool recompose)
{
    if (lvl < RC_CAM_ADJ_OFF) lvl = RC_CAM_ADJ_OFF;
    if (lvl > RC_CAM_ADJ_MINIMAL) lvl = RC_CAM_ADJ_MINIMAL;
    if (g_cam_adjust_lvl == lvl) return;
    g_cam_adjust_lvl = lvl;
    int64_t now_us = esp_timer_get_time();
    if (lvl == RC_CAM_ADJ_MINIMAL) g_cam_adj_motion_us = now_us;
    if (lvl == RC_CAM_ADJ_NO_STRIP) g_cam_adj_l1_us = now_us;
    if (lvl == RC_CAM_ADJ_OFF) g_cam_adj_l1_us = 0;
    ESP_LOGW(TAG, "相机调参性能模式 → level %d（%s）", lvl,
             lvl == 0 ? "正常：全层合成" :
             lvl == 1 ? "跳条带层：static+tile，条带缓存零 TF 读" :
                        "极限：只画 static 底图，窗口缓存零 TF 读，边缘延展");
    if (recompose) full_recompose();
}

void render_cam_set_pending(int32_t x, int32_t y)
{
    g_cam_pending_valid = true;
    g_cam_pending_x = x;
    g_cam_pending_y = y;
}
void render_cam_clear_pending(void) { g_cam_pending_valid = false; }

void render_cam_adjust_set(int level)
{
    if (!g_inited) return;
    rc_lock();
    cam_adjust_apply_level(level, /*recompose=*/level != RC_CAM_ADJ_MINIMAL);
    rc_unlock();
}
int render_cam_adjust_get(void) { return g_cam_adjust_lvl; }

/* UX 层每下发一次相机位移调一次：进极限档并把 2→1 的倒计时续上。
 * 由 input 任务调用（render_cam_set 的同一条路径上）。 */
void render_cam_adjust_motion_notify(void)
{
    /* 仅时戳：渲染层据此认为"拖动仍在进行"（忙窗/横幅），但**不降级**渲染内容。 */
    g_cam_adj_motion_us = esp_timer_get_time();
}

void render_cam_adjust_motion(void)
{
    if (!g_inited) return;
    if (g_cam_adjust_lvl >= RC_CAM_ADJ_MINIMAL) {
        g_cam_adj_motion_us = esp_timer_get_time();     /* 续命：拖动中不回落 */
        return;
    }
    rc_lock();
    cam_adjust_apply_level(RC_CAM_ADJ_MINIMAL, /*recompose=*/false);
    rc_unlock();
}

/* 自动回落（渲染任务每帧调；见 compositor.h 时序说明）。 */
static void cam_adjust_tick(int64_t now_us)
{
    if (g_cam_adjust_lvl == RC_CAM_ADJ_MINIMAL) {
        if (now_us - g_cam_adj_motion_us >= (int64_t)RC_CAM_ADJ_L2_IDLE_MS * 1000) {
            rc_lock();
            cam_adjust_apply_level(RC_CAM_ADJ_NO_STRIP, /*recompose=*/true);
            rc_unlock();
        }
    }
    /* 【不再自动回 level 0】用户定稿：整个"摄像机流程"内都不渲染条带层，
     * 直到保存/取消（lvgl_bridge 的 cam_finish_core → render_cam_adjust_set(OFF)）。
     * 否则松手 300ms 后条带就回来并触发 2~3s 重填，用户看到的还是"卡住"。 */
}

static void cam_scene_sync(void)
{
    if (!g_cam_on) return;
    /* level 2（拖动/吸附进行中）：整层零 TF 读。拖动期 SD 小块读是最大尖峰
     * （336 行 × 逐行 fseek+fread ≈ 100ms 级/次），拿它换"缓存外黑边"在拖动期
     * 不划算：合成侧改用边缘延展兜住，松手 200ms 后回 level 1 再补真内容。
     * level 1：static/tile 照常补读（**这是拖动结束后的补内容点**：不补的话缓存
     * 还留在拖动前的锚点，屏幕边缘就是黑带），只有条带层既不合成都不同步
     * （条带是最贵的一层：每条带一份 336×N 缓存 + 逐像素掩码 + 时间相位）。 */
    /* 【摄像机流程内不碰条带 2026-10-01 用户定稿】level ≥1（进流程即置）时
     * 条带缓存**既不补读也不搬移**：天空之城 14 条带下，补一次要 2~3s（逐行包）
     * 且每 2~3s 来一次 —— 这就是用户口径"拖动还是卡住"的主因。
     * 流程内只画 static+tile（地形本体），保存/取消立即回 level 0 全层恢复。 */
    if (g_cam_adjust_lvl >= RC_CAM_ADJ_NO_STRIP) return;
    s_cam_io_rows = s_cam_io_cols = s_cam_io_bytes = 0;
    s_cam_strip_fopens = 0;
    mpak_tile_stat_reset();           /* 瓦片口径统计：本轮补边实际读了几块/命中几块 */
    int64_t t0 = esp_timer_get_time();
    int rc = 0;
    if (g_wc_static.px) {
        rc |= wc_sync_to(&g_wc_static,
                         rc_anchor_hold(g_wc_static.ax, g_wc_static.valid, g_cam_x,
                                        g_cam_vw, g_wc_static.cw, g_cam_fov_w,
                                        cam_margin_x(&g_wc_static), false),
                         rc_anchor_hold(g_wc_static.ay, g_wc_static.valid, g_cam_y,
                                        g_cam_vh, g_wc_static.ch, g_cam_fov_h,
                                        cam_margin_y(&g_wc_static), false));
    }
    if (g_wc_tile.px) {
        rc |= wc_sync_to(&g_wc_tile,
                         rc_anchor_hold(g_wc_tile.ax, g_wc_tile.valid, g_cam_x,
                                        g_cam_vw, g_wc_tile.cw, g_cam_fov_w,
                                        cam_margin_x(&g_wc_tile), false),
                         rc_anchor_hold(g_wc_tile.ay, g_wc_tile.valid, g_cam_y,
                                        g_cam_vh, g_wc_tile.ch, g_cam_fov_h,
                                        cam_margin_y(&g_wc_tile), false));
    }
    /* 条带层同步：level 0/1 **都做**（用户口径：调参期背景必须全层正常渲染，不许靠
     * 跳图层省时间）。level ≥ MINIMAL 已在函数入口整段返回 = 零 TF 读兜底档。 */
    {
        for (int i = 0; i < g_strip_n; i++) {
            if (!g_strips) break;                  /* 护栏：条带表指针与计数必须同源 */
            rc |= cam_strip_sync(&g_strips[i]);
        }
    }

    if (s_cam_io_rows) {
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGI(TAG, "rc: 相机 → (%d,%d) 窗口读 %u 行 / 补 %u 列 / %u KB（%lld ms）开条带文件 %u 次%s",
                 (int)g_cam_x, (int)g_cam_y, (unsigned)s_cam_io_rows,
                 (unsigned)s_cam_io_cols, (unsigned)(s_cam_io_bytes / 1024u),
                 (long long)ms, (unsigned)s_cam_strip_fopens,
                 rc == 0 ? "" : "（含读错误）");
        /* 【瓦片口径真机判读】上面那行"窗口读 N 行 / N KB"是**逻辑**读量（缓存里
         * memcpy 出来的量），与 SD 无关；真正落到 SD 的只有下面这行：读块数 =
         * 整块 pread 次数（每块 32KB/2KB），命中块数 = 纯 PSRAM memcpy 的次数。
         * 判据：整窗首载 = 读 N 块 × 32KB；拖动补边 = 读 0~4 块（新露出的那一列
         * 瓦片），其余全命中 ⇒ 100ms 以内。 */
        uint32_t tb = 0, th = 0, tby = 0;
        mpak_tile_stat_get(&tb, &th, &tby);
        if (tb || th) {
            int32_t tt = (g_bgmap_ok && g_bgmap.u.bgmap) ? g_bgmap.u.bgmap->tile : 0;
            if (tt > 0)
                ESP_LOGI(TAG, "rc: 瓦片读 %u 块 / 命中 %u 块 / %u KB（%lld ms）块 %d×%d",
                         (unsigned)tb, (unsigned)th, (unsigned)(tby / 1024u),
                         (long long)ms, (int)tt, (int)tt);
            else
                ESP_LOGI(TAG, "rc: 瓦片读 %u 块 / 命中 %u 块 / %u KB（%lld ms）",
                         (unsigned)tb, (unsigned)th, (unsigned)(tby / 1024u), (long long)ms);
        }
    }
    s_cam_strip_fopens = 0;
    if (rc != 0) ESP_LOGE(TAG, "整图窗口读失败 rc=%d（本帧该层按缺失处理）", rc);
}

/* ── 整图合成（世界 1x → 屏 2x 整倍最近邻；与旧路径同序 static→条带→tile）── */
static void cam_static_compose(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (g_sw > RC_MAX_W) return;   /* 结构性护栏：列映射表只有 RC_MAX_W 项（见 render_init） */
    const rc_wincache_t *wc = &g_wc_static;
    for (int32_t sx = 0; sx < g_sw; sx++) {
        int32_t cx = rc_world_of_screen(sx, g_cam_x) - wc->ax;
        s_xmap[sx] = (cx >= 0 && cx < wc->cw) ? (int16_t)cx : (int16_t)-1;
    }
    for (int32_t sy = y; sy < y + h; sy++) {
        uint16_t *drow = g_fb + (size_t)sy * g_sw;
        int32_t cy = rc_world_of_screen(sy, g_cam_y) - wc->ay;
        if (!wc->px || cy < 0 || cy >= wc->ch) {          /* 图外/缓存外 = 黑底 */
            memset(drow + x, 0, (size_t)w * 2u);
            continue;
        }
        const uint16_t *crow = wc->px + (size_t)cy * wc->cw;
        for (int32_t sx = x; sx < x + w; sx++) {
            int16_t cx = s_xmap[sx];
            drow[sx] = (cx >= 0) ? crow[cx] : 0;
        }
    }
}

/* ── level 2 极限档的 static 底图（与 cam_static_compose 的唯一差异）──
 * 拖动期窗口缓存**不补读**（见 cam_scene_sync 的说明），相机一旦走出缓存的
 * ±48 世界 px 余量，按老口径那些列/行就是纯黑 —— 拖动几秒后屏幕边缘会出现
 * 黑带，继续拖会整屏黑（观感像坏了，用户会以为死机）。
 * 这里改成**最近列/行延展**（clamp 到缓存边界）：拖动期看到的是一张"边缘被
 * 拉伸的底图"，位置/幅度仍然跟手，松手 200ms 后回 level 1 补回真内容。
 * 语义边界：这是刻意只在 level 2 出现的画质折中，level 0/1 一字不改。 */
static void cam_static_compose_minimal(int32_t x, int32_t y, int32_t w, int32_t h)
{
    const rc_wincache_t *wc = &g_wc_static;
    if (g_sw > RC_MAX_W) return;   /* 结构性护栏：列映射表只有 RC_MAX_W 项（见 render_init） */
    if (!wc->px || wc->cw <= 0 || wc->ch <= 0) return;    /* 护栏：无缓存则保持上层内容 */
    for (int32_t sx = 0; sx < g_sw; sx++) {
        int32_t cx = rc_world_of_screen(sx, g_cam_x) - wc->ax;
        if (cx < 0) cx = 0;
        if (cx >= wc->cw) cx = wc->cw - 1;
        s_xmap[sx] = (int16_t)cx;
    }
    for (int32_t sy = y; sy < y + h; sy++) {
        int32_t cy = rc_world_of_screen(sy, g_cam_y) - wc->ay;
        if (cy < 0) cy = 0;
        if (cy >= wc->ch) cy = wc->ch - 1;
        const uint16_t *crow = wc->px + (size_t)cy * wc->cw;
        uint16_t *drow = g_fb + (size_t)sy * g_sw;
        for (int32_t sx = x; sx < x + w; sx++) drow[sx] = crow[s_xmap[sx]];
    }
}

static void cam_tile_compose(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (g_sw > RC_MAX_W) return;   /* 结构性护栏：列映射表只有 RC_MAX_W 项（见 render_init） */
    const rc_wincache_t *wc = &g_wc_tile;
    if (!wc->px) return;
    for (int32_t sx = 0; sx < g_sw; sx++) {
        int32_t cx = rc_world_of_screen(sx, g_cam_x) - wc->ax;
        s_xmap[sx] = (cx >= 0 && cx < wc->cw) ? (int16_t)cx : (int16_t)-1;
    }
    for (int32_t sy = y; sy < y + h; sy++) {
        int32_t cy = rc_world_of_screen(sy, g_cam_y) - wc->ay;
        if (cy < 0 || cy >= wc->ch) continue;
        const uint16_t *crow = wc->px + (size_t)cy * wc->cw;
        uint16_t *drow = g_fb + (size_t)sy * g_sw;
        for (int32_t sx = x; sx < x + w; sx++) {
            int16_t cx = s_xmap[sx];
            if (cx < 0) continue;
            if (wc->cov && !wc_cov_get(wc, cx, cy)) continue;
            drow[sx] = crow[cx];
        }
    }
}

static void cam_strip_compose(const rc_strip_t *s, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const rc_wincache_t *wc = &s->wc;
    if (g_sw > RC_MAX_W) return;   /* 结构性护栏：列映射表只有 RC_MAX_W 项（见 render_init） */
    if (!s->ok || !wc->px || wc->cw <= 0 || wc->ch <= 0) return;
    for (int32_t sx = 0; sx < g_sw; sx++) {
        int32_t sc = rc_strip_src_col(g_cam_x, sx, s->delta, wc->sw, wc->wrap);
        if (sc < 0) { s_xmap[sx] = -1; continue; }
        /* 周期平铺带：缓存列 i ↔ 源列 mod(ax+i, sw)（见 wc_fill_cache 的 wrap 分支），
         * 故源列 sc 的缓存列为 mod(sc−ax, sw)——**整带入窗（cw ≥ sw）也一样**：
         * 旧写法此处直接用 sc，锚点 ax≠0 时整条带相位错位一整段（ax 段）。 */
        int32_t cx = wc->wrap ? rc_mod(sc - wc->ax, wc->sw) : (sc - wc->ax);
        s_xmap[sx] = (cx >= 0 && cx < wc->cw) ? (int16_t)cx : (int16_t)-1;
    }
    for (int32_t sy = y; sy < y + h; sy++) {
        int32_t srow = rc_world_of_screen(sy, g_cam_y) - (int32_t)s->y;
        if (srow < 0 || srow >= wc->sh) continue;
        int32_t cy = srow - wc->ay;
        if (cy < 0 || cy >= wc->ch) continue;
        const uint16_t *crow = wc->px + (size_t)cy * wc->cw;
        uint16_t *drow = g_fb + (size_t)sy * g_sw;
        for (int32_t sx = x; sx < x + w; sx++) {
            int16_t cx = s_xmap[sx];
            if (cx < 0) continue;
            if (wc->cov && !wc_cov_get(wc, cx, cy)) continue;
            drow[sx] = crow[cx];
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * 地图扁平缓冲 g_map_fb（2026-10-01 二次定稿）——"渲染卡死"的正面修法
 *
 * 【问题】相机每动一次、每个脏区，compose_region 都要把 static + 条带 + tile
 * **逐像素重算一遍**。真机取数（000010000，4 条带）：`调参合成 160ms（compose 128
 * + blit 32）`；天空之城（200000000，**14 条带**，4040×1996）按同一口径线性叠加
 * ⇒ 单帧 200~400ms，相机一拖就"卡死"。块读取（瓦片缓存）解决的是 SD 读放大，
 * 解决不了这份**逐像素合成**成本 —— 两者必须一起做。
 *
 * 【做法】PSRAM 一张 480×480 RGB565 的**压平背景图**（= static + 条带 + tile 的
 * 合成结果，**不含**宠物/气泡/时钟/横幅——那些照旧每帧画在上面）：
 *   · 相机移动：按屏位移量做行 memmove（世界 1px = 屏 2px；整幅 460KB ≈ 5~10ms），
 *     然后**只重算新露出的那一条**（左/右/上/下四条按需），其余像素原地复用；
 *   · 每帧上屏：compose_region 的底图步 = 从 fb 拷该脏区（脏区小就是几次 memcpy）；
 *   · 失效（换图 / 分配尺寸变化 / level2 兜底档）→ 整幅重建一次。
 *
 * 【为什么位移是精确的（相位冻结）】fb 是"压平"的一张图，相机一动它只能按**同一个**
 * 位移平移；而条带的水平相位含 rx 视差项（= f(相机中心)），各带位移并不相同。
 * 所以平移期间把各带相位冻结在 fb 建立时的值（rc_strip_t.frozen）——此时 static /
 * tile / 所有条带的世界位移**完全一致**，平移逐像素精确；等相机停下（或条带时间
 * 相位推进 = 原来的 4Hz 刷新窗口）再把该带所在的行单独重算回正（不是整幅重建）。
 * 代价：拖动进行中的那几帧，条带按"与地面同速"绘制（视差微分项旁路），松手后回正。
 * 这是刻意的折中，不是隐藏图层：三层**全都在画**（与"跳条带/跳 tile"的降级档有本质区别）。
 * ══════════════════════════════════════════════════════════════════════════ */
static uint16_t *g_map_fb;                 /* 480×480 RGB565（PSRAM 单例，懒分配） */
static int32_t   g_map_fb_w, g_map_fb_h;   /* 分配时的屏幕尺寸（尺寸变即重分配，防越界写） */
static bool      g_map_fb_valid;            /* 内容 == (g_map_fb_cam_*, 冻结相位) */
static bool      g_map_fb_failed;           /* 分配失败过：不再每帧重试/刷日志（回退逐层） */
static int32_t   g_map_fb_cam_x, g_map_fb_cam_y;
static int64_t   g_map_last_move_us;        /* 最近一次相机位移（视差回正的判据） */
static int64_t   g_map_fb_log_us;           /* 日志限频 */

static void mapfb_invalidate(const char *why)
{
    if (!g_map_fb_valid) return;
    g_map_fb_valid = false;
    ESP_LOGI(TAG, "地图扁平缓冲：失效（%s）→ 下一拍整幅重建", why ? why : "?");
}

/* 背景三层合成进指定缓冲（dst 默认 g_fb；扁平缓冲用 g_map_fb）。
 * 说明：三个合成函数都只经 g_fb 这个全局指针写目标，这里临时换指针即可复用，
 * 不必为"写进 fb"再抄一份逐像素逻辑（抄一份 = 两条路径必然漂移）。 */
static void map_bg_compose_into(uint16_t *dst, int32_t x, int32_t y, int32_t w, int32_t h)
{
    uint16_t *save = g_fb;
    g_fb = dst;
    cam_static_compose(x, y, w, h);
    if (g_cam_adjust_lvl < RC_CAM_ADJ_NO_STRIP) {   /* 摄像机流程内：跳过条带层 */
        for (int i = 0; i < g_strip_n; i++) {
            if (!g_strips) break;                   /* 护栏：条带表与计数同源 */
            if (g_strips[i].world) cam_strip_compose(&g_strips[i], x, y, w, h);
            else                   strip_blit(&g_strips[i], x, y, w, h);
        }
    }
    cam_tile_compose(x, y, w, h);
    g_fb = save;
}

/* 内容整体位移 (dxs,dys) 屏 px（dxs<0 = 内容左移 = 相机右移）。
 * 只搬有效区域，露出的那一条留给调用方重算。 */
static void mapfb_shift(int32_t dxs, int32_t dys)
{
    if (dys < 0) {
        memmove(g_map_fb, g_map_fb + (size_t)(-dys) * g_sw,
                (size_t)(g_sh + dys) * g_sw * 2u);
    } else if (dys > 0) {
        memmove(g_map_fb + (size_t)dys * g_sw, g_map_fb,
                (size_t)(g_sh - dys) * g_sw * 2u);
    }
    if (dxs < 0) {
        for (int32_t r = 0; r < g_sh; r++) {
            uint16_t *row = g_map_fb + (size_t)r * g_sw;
            memmove(row, row - dxs, (size_t)(g_sw + dxs) * 2u);
        }
    } else if (dxs > 0) {
        for (int32_t r = 0; r < g_sh; r++) {
            uint16_t *row = g_map_fb + (size_t)r * g_sw;
            memmove(row + dxs, row, (size_t)(g_sw - dxs) * 2u);
        }
    }
}

/* 整幅重建：解冻相位 → 铺满窗口/块缓存 → 全屏合成 → 重新冻结。
 * 只在"失效"（换图/首帧/尺寸变化/level2 兜底）时付这份钱。 */
static void mapfb_rebuild(const char *why)
{
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < g_strip_n; i++)
        if (g_strips) g_strips[i].frozen = false;
    (void)cam_scene_sync();                      /* 全层窗口缓存铺到当前相机（含块补读） */
    map_bg_compose_into(g_map_fb, 0, 0, g_sw, g_sh);
    for (int i = 0; i < g_strip_n; i++) {
        if (!g_strips) break;
        g_strips[i].delta_frozen = g_strips[i].delta;   /* cam_strip_sync 刚算好的真实相位 */
        g_strips[i].frozen = true;
    }
    g_map_fb_cam_x = g_cam_x;
    g_map_fb_cam_y = g_cam_y;
    g_map_fb_valid = true;
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "地图扁平缓冲：平移 dx=%d 边条 %dx%d 重建 %lld ms（全幅重建=1 %s）",
             0, g_sw, g_sh, (long long)ms, why ? why : "");
}

/* 相位推进/视差回正：把"已解冻"的条带各自所在的行重算一遍（**不是**整幅重建）。
 * ⚠️ 必须在 mapfb_pan 之后调用：合成用的是**当前相机**，只有先平移、fb 内部与
 * 当前相机对齐之后，重算出来的行才与左右邻居同源（顺序反了 = 该行整体错位一次
 * 平移量，表现为一条横带里的内容跳一下）。
 * 先 cam_scene_sync（把该带窗口缓存搬到新相位对应的锚点 + 补块），再合成。 */
static void mapfb_patch_unfrozen(void)
{
    int n_un = 0;
    for (int i = 0; i < g_strip_n; i++) {
        if (!g_strips) break;
        if (g_strips[i].ok && g_strips[i].world && !g_strips[i].frozen) n_un++;
    }
    if (n_un == 0) return;
    int64_t t0 = esp_timer_get_time();
    (void)cam_scene_sync();                  /* 一次同步覆盖所有解冻带（含块补读） */
    for (int i = 0; i < g_strip_n; i++) {
        if (!g_strips) break;
        rc_strip_t *s = &g_strips[i];
        if (!s->ok || !s->world || s->frozen) continue;
        int32_t sy0 = rc_screen_of_world((int32_t)s->y, g_cam_y);
        int32_t sy1 = sy0 + ((int32_t)s->h << RC_SCALE_SHIFT);
        if (sy0 < 0) sy0 = 0;
        if (sy1 > g_sh) sy1 = g_sh;
        if (sy1 > sy0) map_bg_compose_into(g_map_fb, 0, sy0, g_sw, sy1 - sy0);
        s->delta_frozen = s->delta;          /* cam_strip_sync 刚算好的真实相位 */
        s->frozen = true;                    /* 重算完再冻回去（平移期间各层同速） */
    }
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "地图扁平缓冲：平移 dx=%d 边条 %dx%d 重建 %lld ms（全幅重建=0 相位回正 %d 带）",
             0, g_sw, g_sh, (long long)ms, n_un);
}

/* 相机移动：平移 + 只重算新露出的边条 */
static void mapfb_pan(void)
{
    int32_t dxs = (g_map_fb_cam_x - g_cam_x) << RC_SCALE_SHIFT;   /* 内容位移（屏 px） */
    int32_t dys = (g_map_fb_cam_y - g_cam_y) << RC_SCALE_SHIFT;
    if (dxs == 0 && dys == 0) return;
    if (dxs <= -g_sw || dxs >= g_sw || dys <= -g_sh || dys >= g_sh) {
        mapfb_rebuild("相机跳变超窗");       /* 整幅换画面：平移没有意义 */
        return;
    }
    int64_t t0 = esp_timer_get_time();
    mapfb_shift(dxs, dys);
    (void)cam_scene_sync();                  /* 命中零 IO；跨瓦片/块边界才补一列 */
    /* 上下边条（整宽）→ 左右边条（整高）。拐角会重算两次，无害。 */
    if (dys < 0)      map_bg_compose_into(g_map_fb, 0, g_sh + dys, g_sw, -dys);
    else if (dys > 0) map_bg_compose_into(g_map_fb, 0, 0, g_sw, dys);
    if (dxs < 0)      map_bg_compose_into(g_map_fb, g_sw + dxs, 0, -dxs, g_sh);
    else if (dxs > 0) map_bg_compose_into(g_map_fb, 0, 0, dxs, g_sh);
    g_map_fb_cam_x = g_cam_x;
    g_map_fb_cam_y = g_cam_y;
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    /* 限频 5/s（拖动 15Hz 时否则刷屏）；判据：真机看"重建 ms"应 ≤30ms */
    int64_t now = esp_timer_get_time();
    if (now - g_map_fb_log_us > 200000) {
        g_map_fb_log_us = now;
        ESP_LOGI(TAG, "地图扁平缓冲：平移 dx=%d 边条 %dx%d 重建 %lld ms（全幅重建=0）",
                 (int)dxs, (dxs ? (dxs < 0 ? -dxs : dxs) : 0),
                 (dys ? (dys < 0 ? -dys : dys) : 0), (long long)ms);
    }
}

/* 每帧底图：必要时（失效/相位/相机变化）先更新 fb，然后把脏区拷进 g_fb。
 * 返回 true = 底图已由 fb 提供（compose_region 的 static/条带/tile 三步整段跳过）。 */
static bool mapfb_base_region(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!g_cam_on) return false;
    if (g_map_fb_failed) return false;               /* 分配失败过：走回退路径，不刷日志 */
    if (g_map_fb && (g_map_fb_w != g_sw || g_map_fb_h != g_sh)) {
        /* 屏幕尺寸变了（render_init 重配）：旧缓冲尺寸/行距全不对，必须重分配，
         * 否则下面的按 g_sw 行距拷贝会越界读写。 */
        heap_caps_free(g_map_fb);
        g_map_fb = NULL;
        g_map_fb_valid = false;
    }
    if (!g_map_fb) {
        g_map_fb = psram((size_t)g_sw * g_sh * 2u);
        if (!g_map_fb) {
            ESP_LOGE(TAG, "地图扁平缓冲分配失败（%dx%d，%u KB）→ 回退逐层合成（性能档降级）",
                     (int)g_sw, (int)g_sh, (unsigned)((size_t)g_sw * g_sh * 2u / 1024u));
            g_map_fb_failed = true;
            return false;
        }
        g_map_fb_w = g_sw;
        g_map_fb_h = g_sh;
        ESP_LOGI(TAG, "地图扁平缓冲：分配 %dx%d RGB565 = %u KB（PSRAM 单例，"
                      "static+条带+tile 压平；宠物/气泡/时钟/横幅仍每帧叠加）",
                 (int)g_sw, (int)g_sh, (unsigned)((size_t)g_sw * g_sh * 2u / 1024u));
        g_map_fb_valid = false;
    }
    if (!g_map_fb_valid) { mapfb_rebuild("首帧/失效"); }
    else {
        mapfb_pan();                         /* 相机移动（平移 + 边条） */
        /* 【逐行包不做相位回正 2026-10-01】"解冻该带"= 把条带相位从冻结值改回真实
         * 值，这一步要**补条带窗口缓存的新列**（SD 读）。逐行包下实测一次 ~2.6~3.2s
         * （天空之城 9~10 带；日志 `地图扁平缓冲：… 重建 3005 ms（相位回正 9 带）`），
         * 每十几秒砸一次，用户口径就是"卡死"。
         * 分块(tile)包下同一步只需 ~0.1~0.2s ⇒ 正常解冻、动画照常。
         * 因此这里按**包口径**分流：逐行包保持"冻结相位"显示（**条带层照常全渲染**，
         * 只是不做时间/视差动画），分块包走原逻辑。用户已定稿：不许靠隐藏图层省时间，
         * 但"装饰层不动画"是可接受的代价；等分块包到位动画自动恢复。 */
        bool pkg_tiled = (g_bgmap_ok && g_bgmap.u.bgmap && g_bgmap.u.bgmap->tiled);
        if (pkg_tiled) {
            mapfb_patch_unfrozen();          /* 相位推进/视差回正（只重算该带的行） */
        } else {
            static int64_t s_log_ms;
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - s_log_ms > 30000) {
                s_log_ms = now_ms;
                ESP_LOGW(TAG, "逐行包：条带保持冻结相位显示（不做相位回正，避免每次 ~3s 重填）；"
                              "分块包到位后自动恢复动画");
            }
        }
    }
    if (!g_map_fb_valid) return false;       /* 重建中出意外 → 回退逐层 */
    for (int32_t r = y; r < y + h; r++)
        memcpy(g_fb + (size_t)r * g_sw + x, g_map_fb + (size_t)r * g_sw + x, (size_t)w * 2u);
    return true;
}

/* ── 整图条带装载（流式：句柄常开 + 行直读，不整幅入 PSRAM）── */
static int strip_load_world(rc_strip_t *s, const char *path, const mpak_strip_t *hdr)
{
    memset(s, 0, sizeof *s);
    mpak_t pm;
    int rc = mpak_open(&pm, path, 0, MPAK_KIND_PARTS);
    if (rc != MPAK_OK) return rc;
    if (pm.parts_count < 1 || !pm.parts_tab) { mpak_close(&pm); return MPAK_ERR_FMT; }
    const mpak_part_t *p = &pm.parts_tab[0];
    int32_t pw = p->w, ph = p->h;              /* 先落值：mpak_close 会释放 parts_tab */
    /* 瓦片存储的条带（契约 §2/§4）：像素区 = gx*gy*T*T*2（右/下补 0），掩码紧随其后。
     * 瓦片边长由**包自身**的长度反解（parse_parts），不依赖 BGMAP 的 bit1。 */
    const bool tiled = p->tiled;
    const int32_t tgx = p->gx, tgy = p->gy;
    const int32_t ttile = p->tile;
    const uint32_t tpx_bytes = p->pixel_bytes;

    strlcpy(s->path, path, sizeof s->path);   /* 像素走按需 pread（见 cam_strip_sync） */
    s->w = (uint16_t)pw;
    s->h = (uint16_t)ph;
    /* 【整图条带全不绘制 · 真机根因 2026-10-01】源 IO 描述（px_off/row_bytes/cov_off）
     * **必须在 wc_alloc 之后落值**：wc_alloc 首行是 memset(wc,0,sizeof *wc)，此前写入的
     * 三个字段会被清零 → 所有裸 pread 落到「偏移 0 + 行距 0」（每行都读文件头同一段
     * 字节）⇒ 掩码读成近乎全 0（真机探针：应 48518 位为 1 的窗口实读 3427）
     * ⇒ cam_strip_compose 逐像素被掩码跳过 ⇒ 4 条条带一个都不画（只见 static+tile）。
     * 探针实证：首个掩码读的绝对偏移 byte_off=14（正确值 377478 = cov_off+14）。
     * 【2026-10-01 二次改动】窗口缓存改成**按需分配**（strip_wc_alloc），装载期不再
     * wc_alloc ⇒ 这里就是那"之后"：描述落在 wc 上，strip_wc_alloc 会在真正分配时
     * 先存后写把它们带过去（同一坑的第二道保险）。 */
    uint32_t px_off    = pm.payload_off + pm.parts_bmp_base + p->offset;
    uint32_t row_bytes = rc_align4((uint32_t)pw * 2u);
    uint32_t cov_off   = px_off + (tiled ? tpx_bytes
                                         : (uint32_t)ph * row_bytes);   /* 掩码紧随像素区 */
    bool has_cov = ((hdr->blend & 1u) != 0) && p->has_alpha;

    s->y       = hdr->y;
    s->speed_x = hdr->speed_x;
    s->rx      = hdr->rx_parallax;
    s->blend   = hdr->blend;
    s->world   = true;
    /* 带宽 ≥ vw = 世界对齐层（服务端 AssetExporter 明示"整幅从世界 x=0 起绘制，不平铺"）；
     * 带宽 < vw 的滚动带：周期平铺，但相位锚在世界原点（见 rc_strip_src_col）。 */
    bool wrap = (pw < g_cam_vw);
    s->ok      = false;

    mpak_close(&pm);                   /* 信封/长度校验已完成；不留常开句柄（p 至此失效） */
    /* 几何 + 源 IO 描述落进 wc（**不分配缓冲**；strip_wc_alloc 首次可见时才分配）。
     * 几何即"潜在窗口缓存尺寸"：cw=min(带宽,CACHE_W)、ch=min(带高,CACHE_H)。 */
    s->wc.kind     = RC_WC_STRIP;
    s->wc.sw       = pw;
    s->wc.sh       = ph;
    s->wc.wrap     = wrap;
    s->wc.cw       = (pw < RC_CAM_CACHE_W) ? pw : RC_CAM_CACHE_W;
    s->wc.ch       = (ph < RC_CAM_CACHE_H) ? ph : RC_CAM_CACHE_H;
    s->wc.px_off   = px_off;
    s->wc.row_bytes = row_bytes;
    s->wc.cov_off  = cov_off;
    s->wc.tiled    = tiled;
    s->wc.tile     = (uint16_t)(tiled ? ttile : 0);
    s->wc.tile_gx  = tiled ? tgx : 0;
    s->wc.tile_gy  = tiled ? tgy : 0;
    /* 瓦片缓存身份 = 路径哈希：条带是按需 open/pread 的，用句柄身份会让"两次补边
     * 之间缓存全失效"；路径是内容哈希命名（immutable），同一路径 ⇒ 同一份内容。 */
    s->wc.tile_fid = tiled ? mpak_path_id(path) : 0;
    s->wc.fd       = -1;
    s->has_cov     = has_cov;
    s->wrap        = wrap;
    s->ok = true;
    /* 每条带装载含 mpak_open（信封 + ≤4MB 包的全量 CRC）；整图 4 条带叠加可 >5s
     * → 同样喂狗（见 wc_fill_cache 的说明）。 */
    watchdog_kick();
    ESP_LOGI(TAG, "整图条带：y=%d h=%d speed=%d rx=%d%s 源 %dx%d px_off=%u row_bytes=%u "
                  "cov_off=%u%s 窗口缓存 %dx%d%s（%u KB，进视野才分配）",
             (int)s->y, (int)s->h, (int)s->speed_x, (int)s->rx,
             wrap ? " 周期平铺" : " 世界对齐",
             (int)pw, (int)ph, (unsigned)px_off, (unsigned)row_bytes, (unsigned)cov_off,
             tiled ? " 瓦片存储" : "",
             (int)s->wc.cw, (int)s->wc.ch, has_cov ? " +掩码" : "",
             (unsigned)(((size_t)s->wc.cw * s->wc.ch * 2u +
                         (has_cov ? (((size_t)s->wc.cw + 7u) / 8u) * s->wc.ch : 0)) / 1024u));
    if (tiled) {
        ESP_LOGI(TAG, "  条带瓦片：tile=%d 网格 %dx%d 像素区 %u B（行口径同带需 %u B）",
                 (int)ttile, (int)tgx, (int)tgy, (unsigned)tpx_bytes,
                 (unsigned)((uint32_t)ph * row_bytes));
    }
    return MPAK_OK;
}

/* ── 整图场景装载（RENDER_OK = 接管 bm 所有权：g_bgmap 常开供流式读 + 地面表）── */
static int cam_scene_load(mpak_t *bm, const mpak_bgmap_t *bg,
                          const char *strip_parts_paths[], int strip_count,
                          uint32_t ground_off, uint32_t ground_len)
{
    g_cam_fov_w = g_sw / RC_SCALE;
    g_cam_fov_h = g_sh / RC_SCALE;
    if (RC_CAM_CACHE_W < g_cam_fov_w || RC_CAM_CACHE_H < g_cam_fov_h) {
        ESP_LOGE(TAG, "窗口缓存 %dx%d < 可见窗口 %dx%d（屏 %dx%d 过宽）→ 不支持相机",
                 RC_CAM_CACHE_W, RC_CAM_CACHE_H, (int)g_cam_fov_w, (int)g_cam_fov_h,
                 (int)g_sw, (int)g_sh);
        return RENDER_ERR_UNSUPPORTED;
    }
    g_cam_vw = bg->vw;
    g_cam_vh = bg->vh;
    /* 参考相机中心 = 窗口居中时的窗口中心（P0-2 同源口径；服务端整图导出相机 = 地图中心，
     * bbox 原点下二者恒等，见 rc_cam_ref_center 的恒等式注释与 host 断言）。 */
    g_cam_ref_cx = rc_cam_ref_center((int32_t)bg->vw, g_cam_fov_w);
    g_cam_max_x = (int32_t)bg->vw - g_cam_fov_w;
    g_cam_max_y = (int32_t)bg->vh - g_cam_fov_h;
    if (g_cam_max_x < 0) g_cam_max_x = 0;
    if (g_cam_max_y < 0) g_cam_max_y = 0;
    g_cam_static_off = bg->static_back_off;
    g_cam_tile_off   = bg->tile_layer_off;
    uint32_t stride_b = rc_align4((uint32_t)bg->vw * 2u);
    /* 【掩码区偏移：瓦片/逐行两个口径不可互换】逐行 = tile_off + vh×align4(vw×2)；
     * 瓦片 = tile_off + gx*gy*T*T*2（差着补 0 的 padding 倍数）。用 mpak 解析出来的
     * bg->tile_mask_off 一份口径，避免第二处再推公式推错（真机错位级 bug）。 */
    g_cam_mask_off   = bg->tile_mask_off;
    /* 瓦片层是否存在：瓦片口径下 RGB 区长度 = gx*gy*T*T*2（不是 vh×行距） */
    uint32_t tile_rgb_bytes = bg->tiled ? bg->tile_px_bytes
                                        : (uint32_t)bg->vh * stride_b;
    g_cam_ground_off = ground_off;
    g_cam_ground_len = ground_len;
    g_cam_ground_on  = (ground_off != 0 && ground_len >= (uint32_t)bg->vw * 2u);
    /* 起始相机：优先"装载期直落"（NVS 记忆位置，见 render_cam_set_pending），
     * 否则窗口居中（与 rc_cam_ref_center/置中同一口径）。
     * 直落的意义：避免"先置中填一遍缓存 → 再应用 NVS 相机 → 整窗重填"
     * （真机 10.7s + 14.2s = 25s 就卡在这）。 */
    if (g_cam_pending_valid) {
        g_cam_x = rc_cam_clamp_axis(g_cam_pending_x, g_cam_vw, g_cam_fov_w);
        g_cam_y = rc_cam_clamp_axis(g_cam_pending_y, g_cam_vh, g_cam_fov_h);
        g_cam_pending_valid = false;                 /* 一次性 */
        ESP_LOGI(TAG, "整图装载直落相机 (%d,%d)（NVS 记忆，省一次整窗重填）",
                 (int)g_cam_x, (int)g_cam_y);
    } else {
        g_cam_x = rc_cam_clamp_axis(rc_cam_home(g_cam_vw, g_cam_fov_w), g_cam_vw, g_cam_fov_w);
        g_cam_y = rc_cam_clamp_axis(rc_cam_home(g_cam_vh, g_cam_fov_h), g_cam_vh, g_cam_fov_h);
    }

    uint32_t cache_kb = 0;
    if (!wc_alloc(&g_wc_static, RC_WC_STATIC, bg->vw, bg->vh, false, false)) {
        ESP_LOGE(TAG, "static 窗口缓存分配失败（%dx%d）", RC_CAM_CACHE_W, RC_CAM_CACHE_H);
        return RENDER_ERR_NOMEM;
    }
    cache_kb += (uint32_t)((size_t)g_wc_static.cw * g_wc_static.ch * 2u / 1024u);

    g_cam_tile_on = false;
    if (bg->tile_layer_len > tile_rgb_bytes) {
        if (wc_alloc(&g_wc_tile, RC_WC_TILE, bg->vw, bg->vh, true, false)) {
            g_cam_tile_on = true;
            cache_kb += (uint32_t)(((size_t)g_wc_tile.cw * g_wc_tile.ch * 2u +
                                    ((size_t)g_wc_tile.cw + 7u) / 8u * g_wc_tile.ch) / 1024u);
        } else {
            ESP_LOGE(TAG, "tile 窗口缓存分配失败 → 降级（static + 条带，无 tile）");
        }
    }

    /* 条带窗口缓存**装载期不分配**（只落几何/源 IO 描述）：14 条带图原来在这里
     * 一次要了 1749KB，而屏幕上同时可见的通常只有 1~3 条 —— 现在进视野才分配，
     * 离开视野就归还（见 strip_wc_alloc / cam_strip_sync）。这里只统计"潜在上限"
     * 给真机判读，不真占内存。 */
    uint32_t strip_max_kb = 0;
    if (strip_count > 0) {
        g_strips = psram((size_t)strip_count * sizeof(rc_strip_t));
        if (!g_strips) {
            ESP_LOGE(TAG, "条带表分配失败（%d 条）→ 无条带", strip_count);
            strip_count = 0;
        }
        g_strip_n = strip_count;
        for (int i = 0; i < strip_count; i++) {
            int rc = strip_load_world(&g_strips[i], strip_parts_paths[i], &bg->strips[i]);
            if (rc != MPAK_OK) {
                ESP_LOGE(TAG, "整图条带 %d 装载失败（%s）rc=%d", i, strip_parts_paths[i], rc);
                g_strips[i].ok = false;
            } else {
                uint32_t cw = (uint32_t)g_strips[i].wc.cw, ch = (uint32_t)g_strips[i].wc.ch;
                strip_max_kb += (uint32_t)((cw * ch * 2u +
                                            (g_strips[i].has_cov ? ((cw + 7u) / 8u) * ch : 0)) / 1024u);
            }
        }
    }

    g_bgmap = *bm;                    /* FILE* 所有权转移：常开（勿再 mpak_close(bm)） */
    g_bgmap_ok = true;
    g_wc_static.pkg = &g_bgmap;
    g_wc_static.row_bytes = stride_b;
    g_wc_tile.pkg = &g_bgmap;
    g_wc_tile.row_bytes = stride_b;
    g_wc_tile.cov_off = g_bgmap.payload_off + g_cam_mask_off;   /* 桥接位图直读用 */
    g_cam_on = true;
    /* 换图 ⇒ 瓦片缓存键全失效：条带身份用路径哈希，同路径文件被重新下载（内容换新）
     * 时旧键会命中旧像素 —— 装载新场景时清一次键（缓冲保留复用）。 */
    mpak_tile_cache_flush();

    ESP_LOGI(TAG, "整图相机：vw=%d vh=%d 窗口 %dx%d 相机支持=1 范围 dx[0,%d] dy[0,%d] "
                  "起始相机 (%d,%d) 地面表=%s 分块读接口=%s 存储=%s",
             (int)g_cam_vw, (int)g_cam_vh, (int)g_cam_fov_w, (int)g_cam_fov_h,
             (int)g_cam_max_x, (int)g_cam_max_y, (int)g_cam_x, (int)g_cam_y,
             g_cam_ground_on ? "有" : "无",
             cam_rect_api_ready() ? "mpak(真)" : "桥接行读(等价)",
             bg->tiled ? "瓦片(整块读)" : "逐行(预读块)");
    ESP_LOGI(TAG, "整图缓存：static %dx%d + tile %s = %u KB 常驻（预算 ≤1200KB；旧口径屏尺寸层 "
                  "949KB 不再分配）；%d 条带窗口缓存改为按需分配（潜在上限 %u KB，进视野才占、"
                  "离开视野即还）",
             (int)g_wc_static.cw, (int)g_wc_static.ch, g_cam_tile_on ? "有" : "无",
             (unsigned)cache_kb, g_strip_n, (unsigned)strip_max_kb);
    if (bg->tiled) {
        ESP_LOGI(TAG, "整图瓦片：tile=%d 网格 %dx%d 窗口 %dx%d 覆盖 %d 列 × %d 行 ≈ %d 块/层"
                      "（瓦片缓存 PSRAM 按需 LRU，只驻留最近读过的块）",
                 (int)bg->tile, (int)bg->gx, (int)bg->gy,
                 (int)g_cam_fov_w, (int)g_cam_fov_h,
                 (g_cam_fov_w + bg->tile - 1) / bg->tile + 1,
                 (g_cam_fov_h + bg->tile - 1) / bg->tile + 1,
                 ((g_cam_fov_w + bg->tile - 1) / bg->tile + 1) *
                 ((g_cam_fov_h + bg->tile - 1) / bg->tile + 1));
    }
    /* P0-1 判据：条带像素走按需 pread（读毕即关），稳态常开句柄只有 BGMAP 这 1 个；
     * 既有常驻 7（时钟/FONT×3/纸娃娃/LAYOUT≤2）+ 1 = 8 ≤ SD_MAX_FILES 10（见 sd_tf.c）。 */
    ESP_LOGI(TAG, "整图句柄：常开 1 个（budget ≤3；BGMAP 供分块读+地面表，%d 条带 = 0 常开，"
                  "像素按需 pread）+ 既有常驻 7 = 8 ≤ SD_MAX_FILES 10", g_strip_n);
    return RENDER_OK;
}

/* ── 整图相机对外 API（契约 §3.2）── */
bool render_cam_supported(void) { return g_cam_on; }

void render_cam_range(int32_t *max_dx, int32_t *max_dy)
{
    int32_t mx = g_cam_max_x, my = g_cam_max_y;
    if (!g_cam_on) { mx = 0; my = 0; }
    if (max_dx) *max_dx = mx;
    if (max_dy) *max_dy = my;
}

void render_cam_get(int32_t *world_x, int32_t *world_y)
{
    rc_lock();                        /* 成对读（x/y 同一相机时刻；跨任务调用方） */
    int32_t x = g_cam_x, y = g_cam_y;
    rc_unlock();
    if (world_x) *world_x = x;
    if (world_y) *world_y = y;
}

void render_cam_set(int32_t world_x, int32_t world_y)
{
    rc_lock();
    if (!g_cam_on) { rc_unlock(); return; }
    int32_t cx = rc_cam_clamp_axis(world_x, g_cam_vw, g_cam_fov_w);
    int32_t cy = rc_cam_clamp_axis(world_y, g_cam_vh, g_cam_fov_h);
    if (cx != g_cam_x || cy != g_cam_y) {
        g_cam_x = cx;
        g_cam_y = cy;
        g_map_last_move_us = esp_timer_get_time();
        /* 相机平移 = 全屏变 ⇒ 整屏标脏走整屏重合成路径（R2 §5.4.6）；只标局部必留残影。
         * 不做同帧内联重合成：本函数由 input 任务按触摸频率调用，内联 TF 读 + 整屏 blit
         * 会阻塞输入；标脏后由渲染任务在下一个 33ms tick 完成（≤1 帧延迟，视觉即时）。 */
        mark_rect_locked(0, 0, g_sw, g_sh);
        ESP_LOGI(TAG, "rc: 相机 → (%d,%d) 夹取范围 dx[0,%d] dy[0,%d]",
                 (int)cx, (int)cy, (int)g_cam_max_x, (int)g_cam_max_y);
    }
    rc_unlock();
}

void render_cam_center(void)
{
    rc_lock();
    if (g_cam_on) {
        g_cam_x = rc_cam_clamp_axis(rc_cam_home(g_cam_vw, g_cam_fov_w), g_cam_vw, g_cam_fov_w);
        g_cam_y = rc_cam_clamp_axis(rc_cam_home(g_cam_vh, g_cam_fov_h), g_cam_vh, g_cam_fov_h);
        g_map_last_move_us = esp_timer_get_time();
        mark_rect_locked(0, 0, g_sw, g_sh);
        ESP_LOGI(TAG, "rc: 相机置中 (%d,%d) 窗口中心 %d（= 参考相机中心 %d）",
                 (int)g_cam_x, (int)g_cam_y, (int)(g_cam_x + g_cam_fov_w / 2), (int)g_cam_ref_cx);
    }
    rc_unlock();
}

int32_t render_ground_screen_y(int32_t screen_x)
{
    if (!g_inited) return -1;
    if (screen_x < 0) screen_x = 0;
    if (screen_x >= g_sw) screen_x = g_sw - 1;
    rc_lock();
    int32_t ret = -1;
    if (g_cam_on && g_bgmap_ok && g_cam_ground_on) {
        int32_t wx = rc_world_of_screen(screen_x, g_cam_x);
        int32_t wy = INT32_MIN;
        if (mpak_bgmap_ground_y) {
            wy = mpak_bgmap_ground_y(&g_bgmap, wx);
        } else if (g_cam_ground_off && g_cam_ground_len >= (uint32_t)(wx + 1) * 2u) {
            /* 桥接：地面表 = vw × u16 小端（0xFFFF = 该列无 foothold），F1 接口未落地时直读 */
            uint16_t v = 0xFFFFu;
            if (wx >= 0)
                mpak_read_at(&g_bgmap, g_bgmap.payload_off + g_cam_ground_off + (uint32_t)wx * 2u,
                             &v, sizeof v);
            if (v != 0xFFFFu) wy = (int32_t)v;
        }
        if (wy != INT32_MIN) {
            int32_t sy = rc_screen_of_world(wy, g_cam_y);
            int32_t lim = g_sh - RC_GROUND_UP_PX;      /* 夹进画面（与旧地面表同口径） */
            if (sy > lim) sy = lim;
            if (sy < 0) sy = 0;
            ret = sy;
            /* 限频：站位链可能逐帧问（值不变时不重复打日志） */
            static int32_t s_glog_wx = INT32_MIN, s_glog_wy = INT32_MIN;
            if (wx != s_glog_wx || wy != s_glog_wy) {
                s_glog_wx = wx;
                s_glog_wy = wy;
                ESP_LOGI(TAG, "rc: 地面线来源：整图地面表 world_x=%d → world_y=%d（屏 y=%d）",
                         (int)wx, (int)wy, (int)sy);
            }
        }
    } else if (!g_cam_on && g_ground_tbl_on) {
        ret = ground_line_y_at(screen_x);              /* 旧包内置段表（设备像素，口径不变） */
    }
    rc_unlock();
    return ret;
}

/* 相机平移时的条带标脏（世界系 y）：按当前相机算该带在屏上可见的行区间。
 * 整段持锁（rc_lock 可重入）：读 g_cam_y 与标脏之间若被 input 任务的
 * render_cam_set 插入，会按旧相机标脏 → 新相机下的那几行漏标 = 残影。 */
static bool cam_strip_mark_dirty(const rc_strip_t *s)
{
    rc_lock();
    int32_t sy0 = rc_screen_of_world((int32_t)s->y, g_cam_y);
    int32_t sy1 = sy0 + ((int32_t)s->h << RC_SCALE_SHIFT);
    if (sy0 < 0) sy0 = 0;
    if (sy1 > g_sh) sy1 = g_sh;
    bool any = (sy1 > sy0);
    if (any) mark_rect_locked(0, sy0, g_sw, sy1 - sy0);
    rc_unlock();
    return any;
}

/* 场景释放（整图部分；旧包字段不受影响） */
static void cam_scene_free(void)
{
    mapfb_invalidate("整图场景释放");     /* 扁平缓冲内容属于旧图/旧相机 */
    if (g_map_fb) { heap_caps_free(g_map_fb); g_map_fb = NULL; }   /* 460KB 还给 PSRAM */
    wc_free(&g_wc_static);
    wc_free(&g_wc_tile);
    if (g_strips) {
        for (int i = 0; i < g_strip_n; i++) wc_free(&g_strips[i].wc);   /* 条带无常开句柄 */
    }
    if (g_bgmap_ok) { mpak_close(&g_bgmap); g_bgmap_ok = false; }
    g_cam_on = false;
    g_cam_tile_on = false;
    g_cam_ground_on = false;
    g_cam_vw = g_cam_vh = 0;
    g_cam_x = g_cam_y = 0;
    g_cam_max_x = g_cam_max_y = 0;
    g_cam_static_off = g_cam_tile_off = g_cam_mask_off = 0;
    g_cam_ground_off = g_cam_ground_len = 0;
}

/* ══ 横幅带绘制 + loading 横幅选文（compose_region 第 7 步用）══════════════════
 * banner_draw_band 就是原来的内联绘制，逐字节等价（深色底整带 → 1px 黑描边 → 白字），
 * 抽出来是为了让"常驻/定时横幅"与"忙 loading 横幅"共用同一份字形/描边口径。 */
static void banner_draw_band(const char *text, int32_t x, int32_t w,
                             int32_t by0, int32_t by1)
{
    if (!text || !text[0] || by0 >= by1) return;
    for (int32_t r = by0; r < by1; r++) {
        uint16_t *drow = g_fb + (size_t)r * g_sw;
        for (int32_t c = x; c < x + w; c++) drow[c] = RC_BANNER_BG;
    }
    for (int pass = 0; pass < 2; pass++) {
        int32_t gx = RC_BANNER_PAD_X;
        for (const char *p = text; *p && gx < g_sw; p++) {
            unsigned char u = (unsigned char)*p;
            if (u >= 'a' && u <= 'z') u -= 32;   /* 5x7 字库仅大写：小写转大写（SSID 小写会渲染成空白） */
            if (u >= 128) u = '?';
            const uint8_t *cols = MP_FONT5X7[u];
            for (int col = 0; col < MP_FONT_GLYPH_W; col++) {
                for (int row = 0; row < MP_FONT_GLYPH_H; row++) {
                    if (!(cols[col] & (1u << row))) continue;
                    int32_t px0 = gx + col * RC_BANNER_SCALE;
                    int32_t py0 = RC_BANNER_Y + RC_BANNER_PAD_Y + row * RC_BANNER_SCALE;
                    banner_fill_block(px0, py0, pass == 0 ? 1 : 0,
                                      pass == 0 ? RC_BANNER_OUTLINE : RC_BANNER_FG,
                                      x, w, by0, by1);
                }
            }
            gx += (MP_FONT_GLYPH_W + 1) * RC_BANNER_SCALE;
        }
    }
}

/* 忙/loading 横幅该不该显示（= 用户看得见的"卡顿窗口"）：
 *   · 相机调参极限档（拖动/吸附进行中）→ 显示；
 *   · 显式 loading 窗口（地图装载 / 相机收尾重派发）→ 显示；
 *   · 重活忙尾（≥60ms 的重活结束后的 RC_BUSY_TAIL_MS）→ 显示。
 * 注意**不含** g_busy_depth 本身：一次 10ms 的整屏重合成不该闪横幅（见 render_busy_leave）。 */
static bool busy_banner_wanted(void)
{
    if (g_cam_adjust_lvl >= RC_CAM_ADJ_MINIMAL) return true;
    if (g_busy_banner_on) return true;
    return (esp_timer_get_time() / 1000) < g_busy_until_ms;
}

/* 当前该显示的横幅文本；NULL = 不画。忙横幅优先于常驻/定时横幅（同一时刻只一条）。 */
static const char *banner_active_text(void)
{
    if (g_cam_adjust_lvl >= RC_CAM_ADJ_MINIMAL) return RC_BANNER_TEXT_CAM_MOVE;
    if (busy_banner_wanted()) {
        return g_busy_why[0] ? g_busy_why : RC_BANNER_TEXT_LOADING;
    }
    return g_banner_on ? g_banner_text : NULL;
}

/* 横幅带标脏（忙横幅进出/文案变化时；渲染任务下一拍重绘该带） */
static void mark_banner_band_dirty(void)
{
    if (!g_inited) return;
    mark_rect(0, RC_BANNER_Y, g_sw, RC_BANNER_H);
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
     * 实体移动后才暴露。
     * 整图口径（g_cam_on）：优先走**地图扁平缓冲**（static+条带+tile 一张压平的图，
     * 相机移动只平移 + 重算边条）——下面第 2/3 步据此整段跳过；fb 不可用（分配失败）
     * 时回退原来的逐层合成，画面口径完全一致。旧包路径（else 分支）逐字节不变。 */
    bool bg_from_fb = false;
    /* level 2 兜底档（只画 static）：整图路径下 2/3 步整段跳过。放在这里显式算一次，
     * 避免"fb 没生效"时（bg_from_fb=false）把条带/tile 又画回来 —— 那会让 level 2
     * 既失去"零 TF 读"语义、又把错位缓存叠上去。 */
    bool cam_minimal = (g_cam_on && g_cam_adjust_lvl >= RC_CAM_ADJ_MINIMAL);
    if (g_cam_on) {
        if (cam_minimal) {
            /* level 2（拖动/吸附进行中）：只画 static 底图 —— 跳条带、跳 tile
             * （含其 1bit 掩码逐像素判定，230K 次位测试/帧），跳窗口缓存 IO。
             * 下面第 2/3 步据此整段跳过。
             * 【2026-10-01】这是**兜底/排障档**：正常渲染（level 0/1）已不靠它 ——
             * 扁平缓冲 + 瓦片块读让"全层 + 只读可见块"的每步成本降到 30ms 级。
             * 进本档会把扁平缓冲置失效（它只画了 static，内容不完整）。 */
            mapfb_invalidate("level2 只画 static");
            cam_static_compose_minimal(x, y, w, h);
        } else {
            bg_from_fb = mapfb_base_region(x, y, w, h);
            if (!bg_from_fb) {
                cam_scene_sync();
                cam_static_compose(x, y, w, h);
            }
        }
    } else {
        for (int32_t r = y; r < y + h; r++) {
            uint16_t *drow = g_fb + (size_t)r * g_sw + x;
            if (g_static) memcpy(drow, g_static + (size_t)r * g_sw + x, (size_t)w * 2u);
            else memset(drow, 0, (size_t)w * 2u);
        }
    }

    /* 2) 条带（旧包：x 向循环平铺；整图：世界系 y + 世界对齐采样）
     * 整图且在扁平缓冲上 = 已在 fb 里压平，这里整段跳过（否则等于重算一遍）。 */
    if (!bg_from_fb && !cam_minimal && g_cam_adjust_lvl < RC_CAM_ADJ_NO_STRIP) {
        for (int i = 0; i < g_strip_n; i++) {
            if (!g_strips) break;               /* 护栏：条带表与计数同源 */
            if (g_strips[i].world) cam_strip_compose(&g_strips[i], x, y, w, h);
            else                   strip_blit(&g_strips[i], x, y, w, h);
        }
    }

    /* 3) tile_layer（1bit alpha 叠加；列偏移与 static_back 同理必须 +x，
     * 否则脏区 [x, x+w) 铺的是第 0 列起的陈旧内容） */
    if (bg_from_fb) {
        /* 已在扁平缓冲里压平 */
    } else if (g_cam_on) {
        if (!cam_minimal) cam_tile_compose(x, y, w, h);
    } else if (g_tile) {
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
    /* 7) 顶部横幅（问题4 常驻/定时横幅 + 【调参 loading】忙窗横幅）
     * 取字模（与 font5x7.h 数据格式核对一致）：下标=字符 ASCII 值（指定初始化器，
     * 等价 字符-' ' 起点式表），每字符 5 列字节、无 stride，bit0=顶行。
     * 问题3 可读性加固：先整趟 1px 黑描边（块外扩 1px）再整趟白字填充。
     * 【2026-10-01】忙/loading 横幅（拖动/装载/相机收尾）**优先于**常驻横幅：
     * 同一时刻只画一条，忙窗结束自动回到常驻文案（如调参提示串）。 */
    {
        const char *bt = banner_active_text();
        if (bt) {
            /* 【用户反馈 2026-09-27】"横幅下次调低一点 肉眼看不到"——真机照片实证：
             * y=0 起的横幅被 AMOLED 圆角/边框切掉，只看到一条发光边。现整体下移到
             * RC_BANNER_Y（圆角安全区），文字行按同一偏移画。 */
            int32_t by0 = (y > RC_BANNER_Y) ? y : RC_BANNER_Y;
            int32_t by1 = (y + h < RC_BANNER_Y + RC_BANNER_H) ? y + h : RC_BANNER_Y + RC_BANNER_H;
            banner_draw_band(bt, x, w, by0, by1);
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
    /* 【忙窗 + 调参计时 2026-10-01】半屏以上的脏区 = 事实上的"整屏重合成"（相机
     * 平移/换图/退菜单都走这里）。这种笔次记入忙窗：≥60ms 才置忙尾（见
     * render_busy_leave），忙窗内 input 任务丢弃按键/触摸 → 用户不必"等卡完再按"，
     * 也不会把卡顿期的连点攒成一次爆发。 */
    bool big_rect = ((int64_t)w * h >= (int64_t)g_sw * g_sh / 2);
    int64_t t_c0 = esp_timer_get_time();
    if (big_rect) render_busy_enter(RC_BANNER_TEXT_LOADING);
    compose_region(x, y, w, h);
    int64_t t_c1 = esp_timer_get_time();
    blit_be(x, y, w, h, g_fb + (size_t)y * g_sw + x, g_sw);
    int64_t t_c2 = esp_timer_get_time();
    /* 【每帧】本笔 compose+blit 耗时：条带滚动自适应闸门 + 慢帧取证都用它。
     * 注意必须**每帧**写（曾误放进 30s 哨兵块 ⇒ 闸门读到陈旧值，形同虚设）。 */
    g_last_frame_ms = (uint32_t)((t_c2 - t_c0) / 1000);
    if (big_rect) render_busy_leave();
    /* 【调参合成计时】用户口径："调参的时候卡顿很严重，需要能确认到底慢在哪"。
     * 只在调参性能模式（level>0）或重活（≥60ms）时打印，且 500ms 限频——
     * 常态渲染路径一条都不多。判据：compose（图层重算）vs blit（上屏）各多少 ms。 */
    {
        static int64_t s_adj_log_ms;
        uint32_t cd = (uint32_t)((t_c1 - t_c0) / 1000), bd = (uint32_t)((t_c2 - t_c1) / 1000);
        if ((g_cam_adjust_lvl > 0 || (cd + bd) >= (uint32_t)(RC_BUSY_HEAVY_US / 1000)) &&
            (t_c2 / 1000 - s_adj_log_ms) >= 500) {
            s_adj_log_ms = t_c2 / 1000;
            ESP_LOGW(TAG, "调参合成 %u ms（compose %u + blit %u）level %d 区域 %dx%d %s",
                     (unsigned)(cd + bd), (unsigned)cd, (unsigned)bd, g_cam_adjust_lvl,
                     (int)w, (int)h, big_rect ? "[整屏]" : "");
        }
    }
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
    if (!g_static && !g_cam_on) {
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
/* 【3 缓冲流水 2026-10-01】见 blit_be 注释：单块会让 SPI 串行化（满屏 63ms）。
 * 块大小沿用已验证值 12288B（早前 24KB 版本与"脸部分割线"报障同期，不要动）。 */
/* 【内部 RAM 红线 2026-10-01】曾设 3（36KB 内部 RAM）⇒ 真机内部堆掉到
 * 15KB/最大块 7.6KB ⇒ 素材绑定被门限跳过（**纸娃娃消失**）+ 60s 内 5 次重启。
 * 本板内部 RAM 只有 ~133KB 动态可用，blit 暂存必须是**内部**（PSRAM 源会让
 * esp_lcd 每次传输 malloc 内部 DMA 拷贝，更糟）。定稿 2 块（24KB，比原来多 12KB）：
 * 双缓冲已足够让 SPI 连续（填 B 与传 A 重叠），第 3 块只平滑抖动、不值 12KB。 */
#define RC_BLIT_STAGES 2
/* 【块大小 12288→6144 2026-10-01】双缓冲 × 12KB = 24KB 内部 RAM 仍把开机堆压到
 * 31KB/最大块 7.6KB（原基线 48KB/22.5KB），素材绑定/动作切换会被门限拦。
 * 改 2 × 6144B = 12KB —— **与改动前的单块 12KB 完全同量**，同时保住双缓冲流水。
 * 代价：满屏块数 38→75，每笔多一次命令开销（~50~100µs）≈ +3~6ms，可接受。 */
static uint8_t s_blit_stage[RC_BLIT_STAGES][6144] __attribute__((aligned(64)));
static int     s_blit_stage_idx;

static void blit_be(int32_t x, int32_t y, int32_t w, int32_t h,
                    const uint16_t *src, int32_t src_stride)
{
    /* 【整宽零拷贝直发实验已回滚 2026-09-27】曾试过 w==g_sw 时一次 display_blit
     * 发完整屏（460800B）以排除"分块窗口写入"嫌疑 —— 真机 display_blit 直接
     * ret=257（ESP_ERR_INVALID_ARG，超 esp_lcd 单笔 max_transfer_sz），整屏区域
     * 反而完全不刷新。结论：竖条纹不在"分块窗口"这一层（真凶见 strip_blit 的
     * 0xFF 分支 2x 展开缺失），故回滚为经校验的分块暂存路径。 */
    /* 【3 缓冲流水 2026-10-01】原来单块暂存 + 每块前 display_wait_tx_idle()
     * ⇒ 每块都等上一笔传完才填，SPI 完全不流水：真机满屏 blit 63ms
     * （QSPI 40MHz 四线理论 20MB/s ⇒ 460KB 应 ~23ms）。
     * 改为 3 块轮转：填之前只等**这一块**自己的上一笔传完（display_blit_buf_busy），
     * 于是发送队列始终有活 ⇒ 传输连续。竞态防护不降级：仍然是"确认该缓冲不在飞才填"。 */
    const int32_t chunk_px = (int32_t)sizeof s_blit_stage[0] / 2;
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
        const int64_t t_wait0 = esp_timer_get_time();
        /* 【混行竞态根修 2026-09-27】填暂存前先等上一笔传输读完它。
         * 此前顺序是"填→发→填→发…"，而等槽发生在 display_blit 内部 ⇒ 上一笔 DMA
         * 仍在读 s_blit_stage 时就被本块覆盖 → 面板收到两块混合数据 =
         * 用户照片里的横彩条/竖条纹/分割线。 */
        /* 探针语义：本块填暂存时"上一笔传输仍在飞"= 修复前必然出混行的那一笔。
         * 计数 >0 即**证明该竞态真实存在**（真机实测 ~150 次/s，几乎每块都命中）。 */
        if (display_tx_busy()) g_blit_race_hits++;
        uint8_t *stage = s_blit_stage[s_blit_stage_idx];
        s_blit_stage_idx = (s_blit_stage_idx + 1) % RC_BLIT_STAGES;
        /* 只等"这一块暂存"的上一笔传完（其余在飞无妨，正好重叠） */
        while (display_blit_buf_busy(stage)) {
            if (esp_timer_get_time() - t_wait0 > 2000000) break;   /* 2s 兜底，绝不死等 */
            vTaskDelay(1);
        }
        uint8_t *d = stage;
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
        for (int32_t i = 0; i < w * hh * 2; i++) { h_before ^= stage[i]; h_before *= 16777619u; }
        if (s_forensic_chunks > 0) {
            uint32_t fh = 2166136261u;
            for (int32_t i = 0; i < w * hh * 2; i++) {
                fh ^= stage[i];
                fh *= 16777619u;
            }
            esp_err_t derr = display_blit((int)x, (int)(y + r0), (int)w, (int)hh, stage);
            ESP_LOGW(TAG, "取证blit rect=(%d,%d %dx%d) len=%d ret=%d fnv=%08x",
                     (int)x, (int)(y + r0), (int)w, (int)hh, (int)(w * hh * 2),
                     (int)derr, (unsigned)fh);
        } else {
            display_blit((int)x, (int)(y + r0), (int)w, (int)hh, stage);
        }
        display_wait_tx_idle();              /* 等这一笔读完，再做校验/重填 */
        {
            uint32_t h_after = 2166136261u;
            for (int32_t i = 0; i < w * hh * 2; i++) { h_after ^= stage[i]; h_after *= 16777619u; }
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
    if (g_cam_on) {                        /* 整图口径：相机由 render_cam_* 管，不做整层位移 */
        ESP_LOGW(TAG, "相机下移实验跳过：整图包（ground_cam_shift_px=%d 仅旧窗口包生效）",
                 (int)s_ground_shift_px);
        return;
    }
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
    render_busy_enter("RENDERING - PLEASE WAIT");
    compose_region(0, 0, g_sw, g_sh);
    memset(g_mark, 0, (size_t)g_gw * g_gh);
    blit_be(0, 0, g_sw, g_sh, g_fb, g_sw);
    render_busy_leave();
    rc_unlock();
}

/* ================= 地图装载 ================= */

static void scene_free(void)
{
    cam_scene_free();                  /* 整图：窗口缓存 + 常开句柄（含条带 PARTS） */
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

    /* （旧"vw==屏宽直接 1:1 拷贝"路径已删 2026-10-01：那正是旧版整屏包
     * vw=480 图 1:1 上屏、地图相对角色小一半的 bug 载体——见下 legacy 分支。） */

    /* 【任意比例最近邻 2026-09-30】原仅接受 2x（vw*RC_SCALE==屏宽）；1.85B 360 屏
     * 用 480 档素材（240 bg → 1.5x）被拒=背景缺失。泛化为 vw/vh→g_sw/g_sh
     * 最近邻采样（2x 是其特例，216 板行为不变）。 */
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

    /* 【旧版整屏口径兼容 2026-10-01】旧导出器（NAS 在跑的镜像）vw=屏幕宽、
     * zoom=1 整屏出图：480 图 = 480 世界 px（视场是新契约 240 的 2 倍），头里
     * vw=480 恰好命中上面的"直接 1:1 拷贝"路径 → 地图按 1:1 上屏，而角色按
     * 契约 1x 素材 ×2 上屏 —— 地图相对角色小了一半（用户双端照片实测：桌面
     * 角色:招牌面板=1.3、设备=2.4）。旧图按契约视场 g_sw/RC_SCALE 中心裁窗、
     * ×RC_SCALE 展开：src = (vw−FOV)/2 + d/RC_SCALE，与 240 档新包同一观感。 */
    if ((int32_t)vw >= g_sw && (int32_t)vh >= g_sh) {
        const int32_t fov_w = g_sw / RC_SCALE, fov_h = g_sh / RC_SCALE;
        const int32_t ox = ((int32_t)vw - fov_w) / 2, oy = ((int32_t)vh - fov_h) / 2;
        ESP_LOGW(TAG, "[旧包兼容] vw=%u vh=%u 整屏口径 → 中心裁 %dx%d 世界窗 ×%d 展开（offset %d,%d）",
                 vw, vh, fov_w, fov_h, RC_SCALE, ox, oy);
        for (int32_t dy = 0; dy < g_sh; dy++) {
            const uint16_t *srow = (const uint16_t *)raw + (size_t)(oy + dy / RC_SCALE) * stride_el;
            uint16_t *drow = dst + (size_t)dy * g_sw;
            for (int32_t dx = 0; dx < g_sw; dx++) drow[dx] = srow[ox + dx / RC_SCALE];
        }
        heap_caps_free(raw);
        return dst;
    }

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
    /* 【旧版整屏口径兼容 2026-10-01】与 layer_rgb_load 同步：vw≥屏宽 = 旧包
     * 世界 1:1 掩码，按契约视场中心裁窗采样，与 static 的裁剪完全对齐。 */
    if ((int32_t)vw >= g_sw && (int32_t)vh >= g_sh) {
        const int32_t fov_w = g_sw / RC_SCALE, fov_h = g_sh / RC_SCALE;
        const int32_t ox = ((int32_t)vw - fov_w) / 2, oy = ((int32_t)vh - fov_h) / 2;
        for (int32_t dy = 0; dy < g_sh; dy++)
            for (int32_t dx = 0; dx < g_sw; dx++) {
                uint32_t sidx = (uint32_t)(oy + dy / RC_SCALE) * vw +
                                (uint32_t)(ox + dx / RC_SCALE);
                if (rc_mask_bit(raw, sidx))
                    rc_mask_set(dst, (uint32_t)dy * g_sw + dx);
            }
        heap_caps_free(raw);
        return dst;
    }
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
    if (p->w == 0 || p->h == 0) {          /* 包损坏：宽/高为 0 会让 strip_offset 除零 */
        mpak_close(&pm);
        ESP_LOGE(TAG, "条带 %s 宽高非法（%ux%u）→ 拒收", path, p->w, p->h);
        return MPAK_ERR_FMT;
    }
    /* 瓦片存储的条带只能走整图相机口径（按世界矩形 + 整块读）；本函数是"整层
     * 入 RAM"的旧窗口口径，会把瓦片块当逐行图读成错位画面 —— 明确拒绝并说明。 */
    if (p->tiled) {
        mpak_close(&pm);
        ESP_LOGW(TAG, "条带 %s 为瓦片存储（tile=%u 网格 %ux%u）→ 旧窗口口径不支持，"
                      "需整图相机路径（见 render_set_map_body 的分流）",
                 path, (unsigned)p->tile, (unsigned)p->gx, (unsigned)p->gy);
        return MPAK_ERR_FMT;
    }

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
        if (s->mask) {
            /* 读失败必须**释放并按 NULL（=不透明）**处理：留一块只写了一部分的
             * PSRAM 当掩码用，strip_blit 会按垃圾位决定透明/不透明（真机表现是
             * 随机竖条纹/黑斑；本仓库历史上已两次栽在"未初始化掩码"上）。 */
            if (mpak_part_read_mask(&pm, p, s->mask, p->mask_bytes) != MPAK_OK) {
                ESP_LOGE(TAG, "条带掩码读取失败（%s part=%u）→ 该层按不透明绘制", path, p->id);
                heap_caps_free(s->mask);
                s->mask = NULL;
            }
        }
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
static int  render_set_map_body(const char *bgmap_path,
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

    /* 【调参档自动回落 2026-10-01】拖动停止 → level 2 回 1（补 tile/条带缓存内容）
     * → 再静止 → 回 0 并整屏重合成（条带重新出现 = 用户口径"松手出全图"）。
     * 每次档位变化内部都会 full_recompose（level 2 除外，见 cam_adjust_apply_level），
     * 所以这里不需要额外标脏。菜单态不合成画面，回落没有意义 → 跳过。 */
    if (!g_menu) cam_adjust_tick(now_us);

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
        static int64_t s_strip_last_heavy_ms;   /* 最近一次 >400ms 笔次的时刻（长时窗闸门） */
        /* 整屏条带 flush 实测 compose 80ms + blit 58ms ≈ 138ms/笔 ⇒ 8Hz 会吃掉
         * 全部渲染预算（人物呼吸都会卡）。修完上屏竞态后 blit 变成"诚实耗时"（以前
         * 与 DMA 重叠所以显得快），这里把条带刷新降到 4Hz；滚动速度不变，只是步长变大。 */
        bool strip_window = (now_us / 1000 - s_strip_ms) > 250;
        if (g_ui_active_until_ms > now_us / 1000) strip_window = false;   /* 交互期冻结 */
        /* 【自适应闸门】上一笔 compose+blit 超过 80ms ⇒ 本轮不推进滚动相位。
         * 目的：不让"条带滚动"把渲染任务喂饱（真机逐行包下单次推进要 ~2.4s）。
         * 分块包到位后单笔降到 ~十几 ms，闸门自动长期放开。 */
        /* 【长时窗闸门】只看"上一笔"不够：逐行包下序列是
         * [便宜帧, 便宜帧, ..., 一次 2.4s 的相位回正] —— 判断时上一笔往往是便宜的，
         * 闸门形同虚设。改成看 **近 10s 内是否出现过 >400ms 的笔次**：
         *   · 逐行包：每次相位推进要 ~2.4s ⇒ 记入"贵"，之后 10s 内不再推进
         *     ⇒ 滚动动画自动让位（画面照常全层渲染，不隐藏任何图层）；
         *   · 分块包：推进 ~100~200ms < 400ms ⇒ 不触发，滚动恢复满速。 */
        if (g_last_frame_ms > 400u) s_strip_last_heavy_ms = now_us / 1000;
        bool strip_budget_ok = (now_us / 1000 - s_strip_last_heavy_ms) > 10000;
        if (!strip_budget_ok) strip_window = false;
        if (g_last_frame_ms > 150u) {   /* 150ms：明显饱和才让位（正常帧 ~80ms） */
            static int64_t s_gate_log_ms;
            if (now_us / 1000 - s_gate_log_ms > 5000) {
                s_gate_log_ms = now_us / 1000;
                ESP_LOGW(TAG, "条带滚动暂缓（上一笔 %u ms > 150ms 闸门）：滚动让位流畅，"
                              "分块包到位后自动恢复", (unsigned)g_last_frame_ms);
            }
            strip_window = false;
        }
        /* 【拖动期也冻结相位 2026-10-01】相机在动（最近 400ms 内有位移）时不推进
         * 条带时间相位：相位一变就要重算"该带所在的行"（14 条带图 ≈ 100~300ms），
         * 正好砸在拖动的那几拍上 = 拖动卡顿；而且"拖地图的同时背景自己还在滚"
         * 观感也乱。松手 400ms 后自动恢复滚动（速度不变，只是暂停一下）。 */
        if (now_us - g_map_last_move_us < 400000) strip_window = false;
        if (strip_window) s_strip_ms = now_us / 1000;
        for (int i = 0; i < g_strip_n; i++) {
            if (!g_strips[i].ok) continue;
            int32_t off = strip_offset(&g_strips[i], now_us);
            if (off == g_strips[i].last_off) continue;
            if (!strip_window) continue;      /* 刷新窗口未开：相位保持不动（缓存/绘制同源） */
            g_strips[i].last_off = off;
            if (g_strips[i].world) {
                /* 整图：off_q = 量化后的时间/IMU 相位（P1-1）——缓存搬移与绘制都用它，
                 * 变化时按"该带在屏上可见的那段行"（相机决定，世界系 y）标脏。 */
                g_strips[i].off_q = off;
                /* 【扁平缓冲 2026-10-01】相位变了 ⇒ 该带在扁平缓冲里被冻结的那份作废：
                 * 解冻它，mapfb_base_region 会在下一拍只重算"该带所在的行"（不是整幅），
                 * 重算后再冻结回去（否则相机平移又会把各带画成同速）。 */
                g_strips[i].frozen = false;
                if (cam_strip_mark_dirty(&g_strips[i])) any = true;
            } else {
                mark_rect(0, (int32_t)g_strips[i].y << RC_SCALE_SHIFT,
                          g_sw, (int32_t)g_strips[i].h << RC_SCALE_SHIFT);
                any = true;
            }
        }
    }

    /* 2b) 【视差回正 2026-10-01】相机停下后，把"因平移而被冻结相位"的条带解冻一次：
     * 冻结期条带与地面同速（见 rc_strip_t.frozen 的取舍说明），停下时该带应回到它
     * 真实的视差位置；mapfb_base_region 会只重算"该带所在的行"，不是整幅重建。
     * 判据：相机静止 ≥400ms 且真实相位与冻结值不同（rx=0 的带永不触发，零成本）。 */
    if (g_cam_on && g_map_fb_valid &&
        now_us - g_map_last_move_us > 400000) {
        for (int i = 0; i < g_strip_n; i++) {
            if (!g_strips) break;
            rc_strip_t *s = &g_strips[i];
            if (!s->ok || !s->world || !s->frozen) continue;
            if (strip_delta_true(s) == s->delta_frozen) continue;   /* rx=0 或相位没变 */
            s->frozen = false;
            if (cam_strip_mark_dirty(s)) any = true;
        }
    }
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

    /* 4b) 【整图相机】render_cam_set 由 input 任务调用（只改状态 + 标脏），
     * 本 tick 必须据此把整屏纳入本次重合成 —— 否则"相机变了但本帧无其它脏区"
     * 时 flush 会被 any=false 跳过（画面停在旧相机，拖拽表现为不跟手）。
     * 相移 = 全屏变 ⇒ 整屏重合成（R2 §5.4.6；只标局部必留残影）。 */
    if (g_cam_on) {
        static int32_t s_last_cam_x = INT32_MIN, s_last_cam_y = INT32_MIN;
        if (g_cam_x != s_last_cam_x || g_cam_y != s_last_cam_y) {
            s_last_cam_x = g_cam_x;
            s_last_cam_y = g_cam_y;
            mark_rect(0, 0, g_sw, g_sh);
            any = true;
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

    /* 5b) 忙/loading 横幅的进出与文案变化 → 横幅带标脏（渲染任务唯一判据点：
     * 忙尾到期、level 2 进出、显式 loading 窗口开关都会在这里被发现）。
     * 文案变化（如 RENDERING → CAM MOVING）在画面上表现为一次重绘，无需额外分支。 */
    {
        bool want = busy_banner_wanted();
        if (want != g_busy_banner_drawn) {
            g_busy_banner_drawn = want;
            mark_rect(0, RC_BANNER_Y, g_sw, RC_BANNER_H);
            any = true;
        }
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

/* 旧口径（非整图包）单层一次性装载上限：合法旧包 ≤ 整屏口径 480×480×2 = 460KB，
 * 留 6× 余量；超过即拒绝（必然失败的 8MB 级分配 + PSRAM 碎片，见 P0-3）。 */
#define RC_LEGACY_LAYER_MAX  (3u * 1024u * 1024u)

/* 【花屏二分已结案 2026-10-01】true=只装载静态层（调试遗留，fbceb82 带入）：
 * 真机实证后果=条带段（树冠/房子/丘陵）与 tile 地形全不装载 → 只剩天空
 * static（用户报障"地图渲染有问题/背景只剩天空"）。新分段导出格式下
 * static 本身就只是天空段，场景中间层全在条带里——此开关必须为 false。 */
static bool g_map_static_only = false;

static int render_set_map_body(const char *bgmap_path,
                               const char *strip_parts_paths[], int strip_count)
{
    mpak_t bm;
    int rc = mpak_open(&bm, bgmap_path, 0, MPAK_KIND_BGMAP);
    if (rc != MPAK_OK) return rc;
    const mpak_bgmap_t *bg = bm.u.bgmap;

    /* 【缺条带降级 2026-10-01】given < declared = 部分条带部件未下载（清单有
     * 引用、文件未到，真机：200000000 声明 9 条、本地只解析出 8）→ 硬失败会
     * 让整图永远装不上（sync 又只把它当"非关键资产"慢慢补）。改为警告+降级：
     * 缺的段不画（场景略不完整），等补齐后下次切图自然完整。given > declared
     * 仍是调用方 bug，维持硬失败。 */
    if (strip_count > (int)bg->strip_count) {
        ESP_LOGE(TAG, "strip count mismatch: bgmap=%u given=%d",
                 bg->strip_count, strip_count);
        mpak_close(&bm);
        return RENDER_ERR_ARG;
    }
    if (strip_count < (int)bg->strip_count) {
        ESP_LOGW(TAG, "条带不全：bgmap 声明 %u、本地就绪 %d → 缺段暂不绘制（等同步补齐）",
                 bg->strip_count, strip_count);
    }

    ESP_LOGW(TAG, "[取证] bg vw=%u vh=%u static_off=%u static_len=%u tile_len=%u tile_off=%u strips=%u",
             bg->vw, bg->vh, bg->static_back_off, bg->static_back_len,
             bg->tile_layer_len, bg->tile_layer_off, bg->strip_count);

    /* ══ 整图（R2）扩展块探测（契约 §3.1 / mpak.h 冻结布局）══════════════════
     * 位置 = align4(tile_layer_off + tile_layer_len)；[magic u32][ground_len u32]
     * [ground_off u32][flags u32]，flags bit0 = 整图包。旧包此处越界 ⇒ 不是整图。
     * 自证解析与 F1 的 parse_bgmap 等价（两侧都认：bg->full_map || 自证），
     * 于是 mpak 层未落地时整图路径也能真机跑通；两者同时可用时取真接口读像素。 */
    uint32_t det_ground_off = 0, det_ground_len = 0;
    bool full = bg->full_map;
    if (bg->tile_layer_len > 0) {
        uint32_t ext_off = rc_align4(bg->tile_layer_off + bg->tile_layer_len);
        if (ext_off + MPAK_BGMAP_EXT_HDR_LEN <= bm.payload_len) {
            uint32_t hdr[4] = { 0, 0, 0, 0 };
            if (mpak_read_at(&bm, bm.payload_off + ext_off, hdr, sizeof hdr) == MPAK_OK &&
                hdr[0] == MPAK_BGMAP_EXT_MAGIC) {
                det_ground_len = hdr[1];
                det_ground_off = hdr[2];
                if (hdr[3] & MPAK_BGMAP_FLAG_FULL_MAP) full = true;
            }
        }
    }
    uint32_t ground_off = bg->ground_off ? bg->ground_off : det_ground_off;
    uint32_t ground_len = bg->ground_len ? bg->ground_len : det_ground_len;

    scene_free();

    /* ══ 整图口径（相机可平移）══════════════════════════════════════════════
     * 条带必须先算好 vw（strip 的 wrap 判定依赖），装载成功即接管 bm；失败则
     * 落到下面的旧窗口口径兜底（宁可给中心窗，也不黑屏）。
     * 【瓦片包一律走整图口径 2026-10-01】瓦片布局与"屏尺寸整层装载"（layer_rgb_load
     * 要求 len == vh×align4(vw×2)）互斥：瓦片包若走旧口径会直接被拒（背景缺失）。
     * 而窗口缓存路径本来就按世界矩形 + 瓦片整块读，vw 小于可见窗口时相机会被夹成
     * 单点（= 静态窗口），语义正确。bit1 与长度口径不一致时 mpak 已按长度判定，
     * 这里的 bg->tiled 就是那个判定结果。 */
    if ((full || bg->tiled) && !g_map_static_only && bg->static_back_len > 0) {
        rc = cam_scene_load(&bm, bg, strip_parts_paths, strip_count,
                            ground_off, ground_len);
        if (rc == RENDER_OK) {
            g_ground_tbl_on = false;      /* 整图走 mpak 世界系地面表（非内置段表） */
            ESP_LOGI(TAG, "地面线来源：%s（map_id=%s vw=%u）",
                     g_cam_ground_on ? "整图地面表(vw 列)" : "通用线(屏底-20)",
                     bg->map_id, bg->vw);
            g_map_epoch_us = esp_timer_get_time();
            g_map_ok = true;
            if (g_ent_cbox_ok) { g_stand_done = true; ent_stand_on_ground_locked(); }
            full_recompose();
            return RENDER_OK;
        }
        cam_scene_free();
        /* 【P0-3】整图包失败**绝不**回退旧口径：旧路径会把整幅层读进一次性临时缓冲
         * （len = vh×align4(vw×2)，本例 1807×4540 = 8,203,780 B），8MB PSRAM 必然分配失败
         * → 静默黑屏 + 大块分配失败留下的 PSRAM 碎片。改为：黑底 + 明确 ERROR（带原因与
         * 内存水位），把失败码交回上层（asset_dl/状态机可重试或提示）。 */
        ESP_LOGE(TAG, "整图装载失败 rc=%d（map=%s vw=%u vh=%u static=%u B）→ 黑底，"
                      "不做整幅回退（旧口径需 %u B 临时缓冲）。PSRAM 空闲 %u B / 最大块 %u B",
                 rc, bg->map_id, bg->vw, bg->vh, bg->static_back_len,
                 (unsigned)((uint32_t)bg->vh * rc_align4((uint32_t)bg->vw * 2u)),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        scene_free();
        g_map_ok = false;
        g_map_epoch_us = 0;
        mpak_close(&bm);
        full_recompose();
        return rc;
    }

    /* 【P0-3 附带】非整图包但单层体量超"一次性装载上限"：拒绝，避免必然失败的大分配
     * 与 PSRAM 碎片（合法旧包最大 = 整屏口径 480×480×2 ≈ 460KB，远低于此上限）。 */
    if (bg->static_back_len > RC_LEGACY_LAYER_MAX) {
        ESP_LOGE(TAG, "旧口径拒绝：static_back %u B > 上限 %u B（vw=%u vh=%u 疑似整图包但缺扩展块）",
                 bg->static_back_len, (unsigned)RC_LEGACY_LAYER_MAX, bg->vw, bg->vh);
        mpak_close(&bm);
        full_recompose();
        return MPAK_ERR_FMT;
    }

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
    if (g_static)
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

/* ══ 地图装载的统一出入口（loading 横幅 + 忙窗）══════════════════════════════
 * 用户口径："同时调整的时候需要横幅之类的 loading 提示，并且不接受按钮信息"。
 * 整图包装载要建窗口缓存并逐行 fread，多条带叠加实测 >5s（见 wc_fill_cache 的
 * 喂狗说明）——这是最长的一次卡顿，横幅必须在**装载开始**就亮（而不是结束才亮），
 * 同时把忙窗打开：input 任务在按键/触摸入口直接丢弃（不排队、不延后执行）。
 * 用统一出口包住 body（body 内部有 5 处 return），避免漏关横幅。 */
static int render_set_map_nolock(const char *bgmap_path,
                                 const char *strip_parts_paths[], int strip_count)
{
    if (!g_inited || !bgmap_path) return RENDER_ERR_ARG;
    render_busy_banner("MAP LOADING - PLEASE WAIT", true);
    int rc = render_set_map_body(bgmap_path, strip_parts_paths, strip_count);
    render_busy_banner(NULL, false);      /* 关横幅 + 置忙尾（终结期按键同样丢弃） */
    return rc;
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

/* 一行 BGR 缓冲：最大屏宽 ×3B + 3B 对齐余量。
 * 【内部 RAM 腾挪 2026-10-02】原为内部 .bss 常驻 1539B —— 截图是**排障**功能，
 * 不该让它常占内部 DRAM。改 PSRAM 懒分配（首次截图时分配，之后复用）：
 *   · 该缓冲只被 CPU 逐字节填 BGR，再交给 POSIX write()；落盘时 FatFS 会把行
 *     拷进它自己的扇区窗口（ff_memalloc 优先 PSRAM）再走 SDMMC，
 *     本缓冲**不直接进 DMA**，所以放 PSRAM 不影响 SPI/SDMMC 路径；
 *   · 分配失败 = 本次截图返回 NOMEM（绝不改渲染主流程，本函数只在 debug 路径）。 */
static uint8_t *s_shot_row;

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
    /* 【内部 RAM 腾挪 2026-10-02】原为内部 .bss 常驻 1402B。此缓冲只做
     * memcpy(g_fb) → sendto()，lwIP 会把它拷进自己的 pbuf，**不直接进 DMA**
     * → PSRAM 懒分配（取证通道，首次倾倒时分配，之后复用）。
     * 分配失败静默 return：取证通道绝不反噬渲染主流程（本函数既有纪律）。 */
    static uint8_t *pkt;
    if (!pkt) pkt = mp_psram_malloc(2 + 1400);
    if (!pkt) { close(sock); return; }
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

    if (!s_shot_row) s_shot_row = mp_psram_malloc(MP_SHOT_MAX_W * 3 + 3);
    if (!s_shot_row) {
        ESP_LOGE(TAG, "截图行缓冲分配失败（PSRAM/内部都拿不到）→ 放弃本次截图");
        close(fd);
        unlink(path);                      /* 不留 54B 空壳文件 */
        rc_unlock();
        return MPAK_ERR_NOMEM;
    }

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
 *
 * 【R2 整图相机（2026-10-01）另计 —— 与上面 static/tile/条带全驻留互斥】
 *   整图模式**不分配**上面那三项（static 460,800 + tile 460,800 + mask 28,800
 *   = 950,400 B），改分配：
 *     地图扁平缓冲 480×480×2                            460,800 B（R3 2026-10-01）
 *     static 窗口 288×288×2                             165,888 B
 *     tile   窗口 288×288×2 + 掩码 288×288/8             176,256 B
 *     条带窗口（**只给"当前在屏"的带**；离开视野即释放，见 strip_wc_alloc）
 *       天空之城 14 条带：以前每条都常驻 = 1749KB（超 1200KB 预算）；
 *       现在只驻留可见的 1~3 条 ≈ 150~450KB
 *     瓦片块缓存（LRU，按需分配：没读过的块一个字节都不占）
 *       ≤16×32KB + 16×2KB = 544KB（只在"确实用到瓦片块"时逐槽分配）
 *     ------------------------------------------------------------------
 *     典型常驻 ≈ 460(fb) + 342(两层窗口) + 450(3 条可见带) + ≤544(块缓存)
 *              ≈ 1.7MB 峰值 / 实际按需低于此（块缓存与条带窗口都是懒分配）
 *   ⇒ 相对"瓦片改造前"的 1749KB(14 条带窗口常驻) + 950KB(旧屏尺寸层) 是净降；
 *     换来的是相机每步 ≤30ms（平移+边条）而不是 128~400ms 全屏重算。
 *   条带源图全驻留需 5.9MB（2270×508 单条 2.45MB 实测）⇒ 采用句柄常开 + 行直读。
 */
