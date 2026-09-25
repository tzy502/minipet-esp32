/**
 * asset_dl.h — 素材同步：manifest diff → 下载校验 → TF 落盘 → LRU 淘汰
 *              + hash→路径 查询面（渲染层 API 按路径绑定素材）
 *
 * TF 布局（software-design 4.4）：
 *   /minipet/manifest.json      本地生效清单（hash 集 + 元数据 + clock_table）
 *   /minipet/parts/<hash>.mpk   部件包（kind=1；fontTime 时钟数字也是 PARTS）
 *   /minipet/layout/<hash>.mpk  布局表（kind=2，按 action 一包）
 *   /minipet/bg/<hash>.mpk      背景烘焙（kind=3，含条带头）
 *   /minipet/font/<hash>.mpk    字体（kind=4；包内 size_px=16/24/32）
 *   /minipet/audio/<hash>.mpk   BGM 曲目元数据（kind=5，audio 目录为本层扩展）
 *   /minipet/firmware/          OTA bin 暂存（ota.c 用）
 *
 * 校验（algorithm-asset-format v2.2 §二）：MAGIC "MPAK" → 尾部 crc32c
 * （覆盖 header+payload）→ 信封 content_hash == manifest 键。
 * 任一失败 = E11 损坏：弃用重拉 + dam 表情 + 事件上报。
 *
 * 淘汰（E7/E11）：TF 用量 ≥85% 触发，LRU + 收藏保护 + 每类最新保护，
 * 清到 ≤80%。
 */
#ifndef MP_ASSET_DL_H
#define MP_ASSET_DL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_HASH_LIST_CAP   4096   /* hash 列表缓冲（events.c cache 上报） */
#define MP_MPK_PATH_MAX    96     /* /sdcard/minipet/xxxx/<16hex>.mpk */

/* 启动同步任务（PRO 核，优先级低于 poller——4.1） */
void asset_dl_start(void);

/* 请求一次 manifest diff（boot / poller 发现 mrev 变化 / 状态机回网） */
void asset_dl_request_sync(void);

/* 本地是否已有生效清单（自检决定 POKER/OFFLINE/FATAL 走向） */
bool asset_dl_have_local_manifest(void);

/* 本地 hash 集（逗号分隔写入 buf，返回长度；0=无缓存） */
size_t asset_dl_collect_hashes(char *buf, size_t cap);

/* ---------------- hash → 路径查询（app_cmd_dispatch 在 render 任务内调用）--- */

/* LAYOUT：按动作名（"stand1"/"walk1"...）；优先实体 paperdoll:* */
bool asset_dl_layout_path(const char *action, char *path, size_t cap);

/* PARTS：entity NULL = 默认纸娃娃（selector paperdoll / entity paperdoll:*） */
bool asset_dl_parts_path(const char *entity_or_null, char *path, size_t cap);

/* FONT：size_px = 16/24/32（包内首字节识别，下载时登记） */
bool asset_dl_font_path(int size_px, char *path, size_t cap);

/* fontTime 时钟数字 PARTS（selector == "clock"，E9/七.5） */
bool asset_dl_fonttime_path(char *path, size_t cap);

/* BGMAP：hash 精确匹配；NULL = 最近使用的 selector==map 条目 */
bool asset_dl_map_path(const char *hash_or_null, char *path, size_t cap);

/* 解析 BGMAP 条带头（part_ref u64 hash）→ 条带小 PARTS 包路径数组。
 * 返回条带数（0=无条带地图；<0=解析失败）。 */
int asset_dl_map_strips(const char *bg_path,
                        char (*strip_paths)[MP_MPK_PATH_MAX], int max_strips);

/* 当前地图的 clock_table 锚点（世界 1x 坐标，E9/R15）；
 * 无当前地图/表无项 → false（渲染层自行居中待机时钟） */
bool asset_dl_clock_anchor(int16_t *world_x, int16_t *world_y);

/* 记录当前地图（cmd SET_MAP 时调用；驱动 clock 锚点与地图 LRU） */
void asset_dl_set_active_map(const char *hash);

/* ---------------- 收藏 / LRU / rev ------------------------------------ */
void asset_dl_set_favorite(const char *hash, bool fav);   /* E7 收藏保护 */
void asset_dl_touch(const char *hash);                    /* 最近使用 */
uint32_t asset_dl_local_rev(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_ASSET_DL_H */
