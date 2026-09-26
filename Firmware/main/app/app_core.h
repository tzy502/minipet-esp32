/**
 * app_core.h — 固件应用层公共类型 / 三队列拓扑 / 表情与动作名表
 *
 * 对应 docs/ai/software-design.md 4.1（双核任务三队列）：
 *   event_q  (input → net 上报)     消费者: net/events.c
 *   cmd_q    (net → render 执行)    消费者: render 任务每帧排空 → app_cmd_dispatch()
 *   audio_q  (UI/net → bgm 控制)    消费者: audio/bgm.c 任务
 *                                    PCM 数据通路: bgm 解码 → PSRAM 环形缓冲 → I2S DMA
 *
 * 协议端点前缀 /api/device（E2）：hello / manifest / asset/{hash} / poll / event /
 *   bgm/stream / bgm/cmd / firmware/{ver}.bin —— 见各 net/ 模块头注释。
 */
#ifndef MP_APP_CORE_H
#define MP_APP_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 常量                                                                */
/* ------------------------------------------------------------------ */

#define MP_FIRMWARE_VERSION   "0.1.0"
#define MP_PROTO_VER          1            /* E2: 协议版本号预留 */

#define MP_TF_ROOT            "/sdcard"    /* drivers sd_mount() 挂载点（main.c 传入） */
#define MP_TF_MINIPET_DIR     MP_TF_ROOT "/minipet"
#define MP_TF_MANIFEST        MP_TF_MINIPET_DIR "/manifest.json"
#define MP_TF_FIRMWARE_DIR    MP_TF_MINIPET_DIR "/firmware"   /* 4.4 */

#define MP_EVENT_Q_LEN        16
#define MP_CMD_Q_LEN          16
#define MP_AUDIO_Q_LEN        8

/* NVS 命名空间（本层唯一入口，键在各模块内定义） */
#define MP_NVS_NS             "minipet"

/* ------------------------------------------------------------------ */
/* 25 表情名（E5/E6/E10 实证集；与导出器 LAYOUT expression 列表同名对齐， */
/* 缺失由导出器保证回退 default。blink 由渲染层本地随机 3~8s 插播，      */
/* 应用层不发 blink。）                                                  */
/* ------------------------------------------------------------------ */
#define MP_EXPR_DEFAULT      "default"
#define MP_EXPR_BLINK        "blink"
#define MP_EXPR_SMILE        "smile"
#define MP_EXPR_LOVE         "love"
#define MP_EXPR_ALERT        "alert"
#define MP_EXPR_BEWILDERED   "bewildered"
#define MP_EXPR_HIT          "hit"
#define MP_EXPR_STUNNED      "stunned"
#define MP_EXPR_OOPS         "oops"
#define MP_EXPR_HUM          "hum"
#define MP_EXPR_DESPAIR      "despair"
#define MP_EXPR_CHEERS       "cheers"
#define MP_EXPR_DAM          "dam"
#define MP_EXPR_HOT          "hot"
#define MP_EXPR_TROUBLED     "troubled"
#define MP_EXPR_WINK         "wink"
#define MP_EXPR_CHU          "chu"
#define MP_EXPR_QBLUE        "qBlue"

/* 动作名（WZ 真实 action，仅引用不自创——E5 动画铁律） */
#define MP_ACTION_STAND      "stand1"
#define MP_ACTION_WALK       "walk1"
#define MP_ACTION_FLY        "fly"
#define MP_ACTION_ALERT      "alert"
#define MP_ACTION_HIT        "hit"
/* CLOCK_DOZE 的视觉（睡眠宠物 + fontTime 数字时钟）由渲染层 render_set_clock
 * 实现（E9），排布硬编码按 docs/ai/clock-display-spec.md 定稿值；
 * 应用层只发 MP_CMD_CLOCK 开关与 clock_table 锚点（asset_dl 查表）。 */

/* 倾斜状态位（input_dispatch 计算，渲染层读走做视差——E6 倾斜视觉全本地驱动） */
#define MP_TILT_LEFT         0x01
#define MP_TILT_RIGHT        0x02
#define MP_TILT_UP           0x04

/* ------------------------------------------------------------------ */
/* 事件（input/app → net/events.c → POST /api/device/event，E2）        */
/* ------------------------------------------------------------------ */
typedef enum {
    MP_EVT_NONE = 0,
    MP_EVT_TOUCH_PET,     /* 轻点抚摸                   a=b=x,y */
    MP_EVT_TAP_LIGHT,     /* IMU 轻拍 <2g               a=mG    */
    MP_EVT_TAP_HARD,      /* IMU 大力拍打 >=4g          a=mG    */
    MP_EVT_SHAKE,         /* 剧烈摇晃                           */
    MP_EVT_PICKUP,        /* 拿起/翻转                          */
    MP_EVT_TILT_ENTER,    /* 倾斜进入（只报变迁）        a=tilt 位 */
    MP_EVT_TILT_EXIT,     /* 倾斜退出（只报变迁）        a=tilt 位 */
    MP_EVT_BATTERY_LOW,   /* 低电                        a=pct   */
    MP_EVT_ASSET_ERROR,   /* 素材包损坏（E11）           s=hash  */
    MP_EVT_BGM_FAILOVER,  /* 音源整体不可用（E8）        s=source*/
    MP_EVT_BOOT,          /* 开机上报                    a=原因  */
    MP_EVT_ERROR,         /* 通用错误                    a=code  */
} mp_event_type_t;

typedef struct {
    mp_event_type_t type;
    int32_t a;
    int32_t b;
    int64_t ts_ms;        /* 毫秒时间戳（RTC/系统时钟） */
    char    s[24];        /* 泛型字符串：hash（16 hex）/ source（"wz"/"qq"） */
} mp_event_t;

/* ------------------------------------------------------------------ */
/* 指令（net/poller → cmd_q → render 任务 → app_cmd_dispatch，E2）      */
/* ------------------------------------------------------------------ */
typedef enum {
    MP_CMD_NONE = 0,
    MP_CMD_SET_ACTION,      /* s=动作名            → render_set_layout        */
    MP_CMD_SET_EXPRESSION,  /* s=表情名            → render_set_expression    */
    MP_CMD_BUBBLE,          /* s=UTF-8 文本        → render_bubble_show       */
    MP_CMD_SET_MAP,         /* s=bg hash           → render_set_map(+条带)    */
    MP_CMD_BRIGHTNESS,      /* a=0..100            → display_brightness       */
    MP_CMD_REBOOT,          /* 服务端指令重启                                 */
    MP_CMD_OTA_BEGIN,       /* a=0..100 阶段提示   → render 气泡               */
    MP_CMD_OTA_FAIL,
    MP_CMD_OTA_DONE,        /* 即将重启升级                                   */
    MP_CMD_PAIRING_CODE,    /* s=6 位配对码（E13） → bubble + cheers          */
    MP_CMD_MANIFEST_SYNCED, /* 素材就绪 → render 任务重绑（fonts/parts/stand1）*/
    MP_CMD_NET_STATE,       /* a=1 online / 0 offline（渲染层可选置灰）       */
    MP_CMD_BGM_STATE,       /* a=播放态 b=source（渲染层控制条回显/置灰）     */
    MP_CMD_MENU_ENTER,      /* render_enter_menu（E7 独立全屏，单写屏者让路） */
    MP_CMD_MENU_EXIT,       /* render_exit_menu                              */
    MP_CMD_CLOCK,           /* a=1 待机时钟浮现 / 0 隐藏（E9；锚点查 clock_table） */
    MP_CMD_BANNER,          /* a=1 顶部未配网横幅 s=文本 / a=0 隐藏（问题4）   */
} mp_cmd_type_t;

typedef struct {
    mp_cmd_type_t type;
    int32_t a;
    int32_t b;
    char    s[96];        /* action/expression/hash/气泡文本（≈31 汉字） */
} mp_cmd_t;

/* ------------------------------------------------------------------ */
/* 音频控制消息（任意上下文 → audio_q → bgm 任务；E8 控制权在设备）      */
/* PCM 数据不进队列：bgm 解码线程 → PSRAM 环形缓冲 → I2S DMA feeder。   */
/* ------------------------------------------------------------------ */
typedef enum {
    MP_AUDIO_NONE = 0,
    MP_AUDIO_PLAY,        /* a=track_id（0=让服务端决定/继续） */
    MP_AUDIO_PAUSE,
    MP_AUDIO_RESUME,
    MP_AUDIO_STOP,
    MP_AUDIO_NEXT,        /* 同源内切歌（E8 短路规则） */
    MP_AUDIO_PREV,
    MP_AUDIO_VOL,         /* a=0..100 线性音量 */
    MP_AUDIO_SOURCE,      /* a=0 WZ 曲库 / 1 QQ 音乐（手动切类型才换源） */
} mp_audio_msg_type_t;

typedef enum { MP_BGM_SRC_WZ = 0, MP_BGM_SRC_QQ = 1 } mp_bgm_source_t;

typedef struct {
    mp_audio_msg_type_t type;
    int32_t a;
} mp_audio_msg_t;

/* ------------------------------------------------------------------ */
/* 三队列（main.c 创建）                                                */
/* ------------------------------------------------------------------ */
extern QueueHandle_t mp_event_q;
extern QueueHandle_t mp_cmd_q;
extern QueueHandle_t mp_audio_q;

/* ------------------------------------------------------------------ */
/* 运行配置（默认值按 software-design 2.4；hello/poll 可下发覆盖）       */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  idle_to_clock_min;  /* E9  闲置 N 分钟 → CLOCK_DOZE（默认 5） */
    float    imu_deadzone_deg;   /* E6  ±8° 死区 */
    uint32_t tilt_debounce_ms;   /* E6  300ms 防抖 */
    float    tap_light_g;        /* E6  <2g alert+bewildered */
    float    tap_hard_g;         /* E6  ≥4g hit */
    uint8_t  brightness;         /* 默认背光 */
} mp_app_config_t;

extern mp_app_config_t g_mp_cfg;   /* main.c 定义并赋默认值；字段小且容忍撕裂 */

/* ------------------------------------------------------------------ */
/* 小工具                                                               */
/* ------------------------------------------------------------------ */
static inline int64_t mp_now_ms(void) { return esp_timer_get_time() / 1000; }

static inline bool mp_post_event(const mp_event_t *e)
{
    return xQueueSend(mp_event_q, e, 0) == pdTRUE;
}

static inline bool mp_post_cmd(const mp_cmd_t *c)
{
    return xQueueSend(mp_cmd_q, c, 0) == pdTRUE;
}

static inline bool mp_post_audio(const mp_audio_msg_t *m)
{
    return xQueueSend(mp_audio_q, m, 0) == pdTRUE;
}

static inline void mp_post_event_simple(mp_event_type_t t, int32_t a, int32_t b, const char *s)
{
    mp_event_t e = { .type = t, .a = a, .b = b, .ts_ms = mp_now_ms() };
    if (s) strlcpy(e.s, s, sizeof(e.s));
    mp_post_event(&e);
}

/* NVS 便捷读写（命名空间固定 MP_NVS_NS；失败/不存在返回 false 且不改动 out） */
static inline bool mp_nvs_get_str(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(MP_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    bool ok = (nvs_get_str(h, key, out, &len) == ESP_OK);
    nvs_close(h);
    return ok;
}

static inline esp_err_t mp_nvs_set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MP_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static inline bool mp_nvs_get_u32(const char *key, uint32_t *out)
{
    nvs_handle_t h;
    if (nvs_open(MP_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_u32(h, key, out) == ESP_OK);
    nvs_close(h);
    return ok;
}

static inline esp_err_t mp_nvs_set_u32(const char *key, uint32_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MP_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* cmd 队列消费者入口（state_machine.c 实现，render 任务每帧调用） */
void app_cmd_dispatch(const mp_cmd_t *cmd);

#ifdef __cplusplus
}
#endif

#endif /* MP_APP_CORE_H */
