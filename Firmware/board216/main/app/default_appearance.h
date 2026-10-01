/**
 * default_appearance.h — 板载默认纸娃娃：神子（2026-09-26 胶水定稿）
 *
 * 用途：设备离线/首次启动、服务端 manifest 尚未同步时的兜底装扮身份。
 * 与 Server/seed/default-appearance.json、Web/src/utils/defaultAppearance.js
 * 三处同源，改动必须三处同步。
 *
 * 注意：固件端不做纸娃娃合成——这份配置只用于：
 *   1. asset_dl 请求 manifest 时携带的 entity 提示（"paperdoll:default"）
 *   2. 配网页/串口日志展示设备身份
 *   3. 服务端可按此判断设备是否需要重发装扮
 * 真正的部件/布局以 manifest 下发为准（方案 C，部件图+设备端 LVGL 合成）。
 */
#ifndef MP_DEFAULT_APPEARANCE_H
#define MP_DEFAULT_APPEARANCE_H

/* 实体名：服务端 manifest 里默认纸娃娃的 entity 键 */
#define MP_DEFAULT_PET_ENTITY   "paperdoll:default"

/* 神子（从桌面版 savedPaperdolls.神子 迁移，仅身份展示用） */
#define MP_DEFAULT_PET_NAME     "神子"
#define MP_DEFAULT_GENDER       0
#define MP_DEFAULT_SKIN         0
#define MP_DEFAULT_BODY_ID      2000
#define MP_DEFAULT_EAR          "humanEar"
#define MP_DEFAULT_HAIR_ID      "36633"
#define MP_DEFAULT_FACE_ID      "20094"
#define MP_DEFAULT_CAP_ID       "1003953"
#define MP_DEFAULT_OVERALL_ID   "1050863"   /* 套服（替代 coat+pants） */
#define MP_DEFAULT_SHOES_ID     "1074299"
#define MP_DEFAULT_WEAPON_ID    "1572011"

#endif /* MP_DEFAULT_APPEARANCE_H */
