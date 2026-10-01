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
bool asset_dl_sync_attempted(void);
void asset_dl_reload_local(void);
bool asset_dl_render_use_factory(void);        /* 渲染根切 /factory（TF 继续作下载根） */
bool asset_dl_render_root_is_factory(void);
bool asset_dl_sync_idle(void);        /* sync_once（含下载）是否完全结束 */
bool asset_dl_critical_ready(void);   /* 关键素材（纸娃娃+stand1）已落盘 TF */        /* sync_once（含下载）是否完全结束 */   /* 重读磁盘 manifest 重建内存表（切工厂分区后调） */   /* 首次清单同步已出结果（供空卡降级判停） */

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

/* LAYOUT 条目的画布内 body 锚点（origin，桌面 RenderFrame 同口径）：
 * 摆放 = 画布左上角对齐「屏心 - origin×scale」⇒ origin 恒在屏心。
 * 返回 false = 清单里没有该动作/没有 origin 字段（调用方回退 0,0）。 */
bool asset_dl_layout_origin(const char *action, int16_t *out_x, int16_t *out_y);

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

/* ---------------- E7 选择器列表（菜单数据面；渲染任务可调用）--------------
 * 跨任务安全：三个 list 函数内部都取 s_lock（旧实现直接读 s_files[] 无锁，
 * 与 asset_dl 任务的 upsert/淘汰并发时会看到半更新行）→ 允许从 LVGL
 * 渲染任务（菜单重建）直接调用。单次持锁含 access(F_OK) 少量 IO。
 *
 * label 口径（2026-10-01 中文化改造）：manifest label 只要是「可显示串」
 * （非空 + 无控制字符，**UTF-8 中文原名原样透传**——服务端 L1 修好后地图名
 * 就是中文）即采用；否则回退
 *   ① map/entity 字段（ASCII，如 "001010000" / "npc:2100000"）
 *   ② hash 前 8 位。
 * 兜底（§3.2 L2）：label 形如 "map_<纯数字>"（push 端点没接 WZ 地图名）时
 * 自动剥掉 "map_" 前缀只显示数字原名（不做中文数字转换，用户已拍板）。
 * 注意：菜单字体是烘焙 CJK 子集，动态中文名缺字时由菜单层回落数字键
 * （lvgl_bridge.c menu_label_font_safe），本层不做字体判断。
 * cached[i]（可 NULL）= <kind_dir>/<hash>.mpk 是否已在 TF（access F_OK）。
 * 注意：s_files[] 里含「已登记元数据但未下载成功」的条目，故 list 结果
 * 是「清单全集 + 缓存标记」，不是「仅已缓存」（菜单需据此提供下载入口）。
 * **BGMAP 列表另过滤用户隐藏的图**（asset_dl_map_set_hidden）。
 * 返回条数（≤ max）；hashes 为 manifest 原文（小写 16 hex），可直接回填
 * MP_CMD_SET_MAP / MP_CMD_SET_PARTS 的 s 通道。 */
int asset_dl_bgmap_list(char hashes[][20], char labels[][32], bool *cached, int max);
/* PARTS 装扮类：selector=="paperdoll" 或 entity "paperdoll*"；排除 fontTime
 * （selector=="clock"）与地图条带小包（条带 PARTS 无 selector——旧实现会把
 * 条带混进换装列表） */
int asset_dl_parts_list(char hashes[][20], char labels[][32], bool *cached, int max);
/* NPC 实体列表（T2）：selector=="npc" 或 entity "npc:*"，按 entity 去重
 * （服务端一个 NPC = 1 个 PARTS + N 个 LAYOUT）。hashes[i] = 该 NPC 的 PARTS
 * 包 hash；entities[i] = "npc:<id>"。无 NPC 条目 → 返回 0（菜单显示空态）。 */
int asset_dl_npc_list(char entities[][40], char hashes[][20], char labels[][32],
                      bool *cached, int max);
/* LAYOUT：该动作是否已有本地文件（元数据 + access(F_OK)）。不能用
 * asset_dl_layout_path 代替——它只查元数据，文件被 LRU 淘汰后仍返回 true。 */
bool asset_dl_layout_cached(const char *action);
/* 该 hash 的 .mpk 是否已在 TF（strcmp 精确匹配，口径同 manifest 键大小写） */
bool asset_dl_file_cached(const char *hash);
/* T4：按单个 hash 请求下载（复用 asset_dl_task 唤醒 + download_one 落盘，
 * 不新造队列/任务）。hash 须已在本地清单登记且 kind 有目录（THUMB 等无目录
 * kind 一律 false）。返回 true = 已受理（本地已有文件也返回 true，视为完成）；
 * false = 参数非法 / 未登记 / 同步任务未启动。
 * 完成判定：调用方轮询 asset_dl_file_cached(hash)（菜单 100ms tick）。 */
bool asset_dl_request_one(const char *hash);

/* ---------------- 地图"删除" = 本地隐藏标识（§4.4 拍板方案）--------------
 * 语义：**不物理删文件**——NVS（namespace "maphide"）打 per-map 隐藏标识：
 *   · 置位后 asset_dl_bgmap_list() 不再返回该图 → 菜单列表消失；文件/清单/
 *     LRU 全部照常（天然规避"本地删除被 sync 复活"的对账死循环）；
 *   · **服务端再次推送该图**（poll 收到 map 指令，poller.c 调 set_hidden(false)）
 *     → 自动解除隐藏、列表恢复可选用；
 *   · 重启保持（NVS，掉电不丢）。
 * 键 = per-map 键（map_id 如 "000010000" 优先；无 map_id 时 "h"+hash 前 14 位，
 * 恒 ≤15 字符 = NVS 键长上限）。
 * hash_or_id：内容 hash（列表项）或地图 id（服务端指令）两种口径都认
 * （与 asset_dl_map_path 同款双键解析）。 */
bool asset_dl_map_set_hidden(const char *hash_or_id, bool hidden);   /* true=NVS 落地成功 */
bool asset_dl_map_hidden(const char *hash_or_id);                     /* 该图当前是否被隐藏 */
int  asset_dl_bgmap_visible_count(void);                              /* 未被隐藏的 BGMAP 条数 */
bool asset_dl_map_is_active(const char *hash_or_id);                  /* 是否正在渲染的当前图 */

/* 【活动地图持久化 2026-10-01（自 216 迁移）】hash/id → map_id；以及 map_id 是否还在当前清单里。
 * 用途：活动地图存 map_id（不是 hash——重导会换 hash），开机校验它仍在清单，不在才回默认。 */
bool asset_dl_map_id_of(const char *hash_or_id, char *out, size_t cap);
bool asset_dl_map_exists(const char *map_id);

/* per-map 键查询（相机 agent 的 NVS per-map 键同口径，见 requirements §5.3）：
 * out = map_id 优先，无则 "h"+hash 前 14 位；找不到该 BGMAP 条目返回 false */
bool asset_dl_map_key(const char *hash_or_id, char *out, size_t cap);

void asset_dl_touch(const char *hash);                    /* 最近使用 */
uint32_t asset_dl_local_rev(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_ASSET_DL_H */
