/*
 * mpak.h — MPAK 素材包流式解析器（FATFS FILE*）
 *
 * 依据 docs/ai/algorithm-asset-format.md v2.2（格式圣经）。
 * 设计要点：
 *   - 只用 FILE* 顺序/偏移读：头+索引一次读入；位图按 offset fseek+fread 懒加载，
 *     包永不整载内存（TF 直读流式）。
 *   - 信封校验序：MAGIC → version → 文件长度 → 尾部 crc32c（覆盖 MAGIC..payload 全量）
 *     → content_hash 与 manifest 期望值比对 → kind 校验。任一失败返回 MPAK_ERR_*，
 *     由上层按 E11 损坏处理（弃用重拉 + dam 表情）。
 *   - 端序：格式统一小端；ESP32-S3 为小端主机，字段读取统一走 LE 字节游标
 *     （编译器折叠为单次访存），与 wire 结构体 static_assert 共同钉死格式。
 *
 * ⚠️ 两处格式文档的口径裁定（已按最自洽解释实现，需与导出器联调对齐）：
 *   1. 信封首块标 [16B]：'MPAK'(4)+version(2)+flags(2)=8B，余 8B 视为 0 填充
 *      （"4 字节对齐"原则），故头全长 16+8+8+4+4 = 40B。
 *   2. BGMAP 的 static_back_off/tile_layer_off 解释为 payload 起点相对偏移；
 *      PARTS 的 part.offset 为位图数据区（索引区之后）相对偏移（文档明示）。
 */
#ifndef RENDER_MPAK_H
#define RENDER_MPAK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>      /* FILE* 流式解析 */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 信封常量                                                            */
/* ------------------------------------------------------------------ */

#define MPAK_MAGIC0          0x4B41504Du /* "MPAK" 小端 u32                 */
#define MPAK_VERSION         1u
#define MPAK_HEADER_LEN      40u         /* magic 块 16 + kind 8 + hash 8   */
                                         /* + payload_len 4 + reserved 4    */
#define MPAK_TRAILER_LEN     8u          /* crc32c u32 + zero u32           */
#define MPAK_MAX_PAYLOAD     (8u * 1024u * 1024u) /* FONT 32px 全量 ~1.8MB 上限 */

typedef enum {
    MPAK_OK          = 0,
    MPAK_ERR_IO      = -1,   /* FATFS 打开/读失败                              */
    MPAK_ERR_MAGIC   = -2,   /* MAGIC 不符                                     */
    MPAK_ERR_VERSION = -3,   /* version != 1                                   */
    MPAK_ERR_LEN     = -4,   /* 文件长度 != 40+payload_len+8 / 超上限           */
    MPAK_ERR_CRC     = -5,   /* 尾部 crc32c 不符（包损坏）                      */
    MPAK_ERR_HASH    = -6,   /* content_hash 与期望不符                         */
    MPAK_ERR_KIND    = -7,   /* kind 未知 / 与期望不符                         */
    MPAK_ERR_FMT     = -8,   /* payload 内部结构非法（计数/偏移越界等）          */
    MPAK_ERR_NOMEM   = -9,
    MPAK_ERR_ARG     = -10,
    MPAK_ERR_RANGE   = -11   /* 查询的 part/glyph 不存在                       */
} mpak_err_t;

/* kind u64 取值 */
#define MPAK_KIND_PARTS      1ull
#define MPAK_KIND_LAYOUT     2ull
#define MPAK_KIND_BGMAP      3ull
#define MPAK_KIND_FONT       4ull
#define MPAK_KIND_AUDIO_META 5ull

/* ------------------------------------------------------------------ */
/* wire 结构（与磁盘字节一一对应；packed + static_assert 钉死尺寸）      */
/* ------------------------------------------------------------------ */

/* 信封头：40B。首 16B = 'MPAK' + version + flags + 8B 0 填充（口径裁定 #1） */
typedef struct __attribute__((packed)) {
    char     magic[4];
    uint16_t version;
    uint16_t flags;
    uint8_t  pad8[8];
    uint64_t kind;
    uint64_t content_hash;
    uint32_t payload_len;
    uint32_t reserved;
} mpak_wire_header_t;

/* PARTS 索引项：20B（doc 标定 20B；字段实体 18B + 尾 2B 填充） */
typedef struct __attribute__((packed)) {
    uint32_t part_id;
    uint16_t expr_group;
    uint16_t w;
    uint16_t h;
    int16_t  origin_x;
    int16_t  origin_y;
    uint32_t offset;         /* 位图数据区内偏移 */
    uint16_t pad;
} mpak_wire_part_t;

/* LAYOUT 帧头：12B = delay_ms(4) + [dx,dy](4) + piece_count(4) */
typedef struct __attribute__((packed)) {
    uint32_t delay_ms;
    int16_t  move_dx;
    int16_t  move_dy;
    uint32_t piece_count;
} mpak_wire_frame_hdr_t;

/* LAYOUT piece：12B（尾 1B 填充） */
typedef struct __attribute__((packed)) {
    uint32_t part_id;
    uint8_t  expr_index;     /* 255 = 非表情件 */
    int16_t  x;
    int16_t  y;
    uint8_t  flip;           /* bit0 = 水平翻转 */
    int8_t   z;              /* 帧内相对序，小者先画 */
    uint8_t  pad;
} mpak_wire_piece_t;

/* BGMAP 条带头：14B */
typedef struct __attribute__((packed)) {
    uint64_t part_ref;       /* 指向独立小 PARTS 包的 content_hash */
    int16_t  y;
    int16_t  speed_x;        /* px/s（世界 1x 坐标） */
    uint8_t  rx_parallax;    /* IMU 视差系数（%/100） */
    uint8_t  blend;          /* bit0=带 1bit alpha 掩码；其余保留 */
} mpak_wire_strip_t;

/* BGMAP payload 头：56B */
typedef struct __attribute__((packed)) {
    char     map_id[32];
    uint16_t vw;
    uint16_t vh;
    uint32_t static_back_len;
    uint32_t static_back_off; /* payload 相对（口径裁定 #2） */
    uint32_t tile_layer_len;
    uint32_t tile_layer_off;
    uint32_t strip_count;
} mpak_wire_bgmap_hdr_t;

/* FONT payload 头：8B（size_px + bpp + 2B 填充保 4 对齐） */
typedef struct __attribute__((packed)) {
    uint8_t  size_px;
    uint8_t  bpp;
    uint16_t pad;
    uint32_t glyph_count;
} mpak_wire_font_hdr_t;

/* FONT 字形索引项：12B */
typedef struct __attribute__((packed)) {
    uint32_t unicode;
    uint16_t w;
    uint16_t h;
    uint8_t  advance;
    int8_t   off_x;
    int8_t   bearing_y;      /* 基线→字形顶，向上为正 */
    uint8_t  pad;
} mpak_wire_glyph_t;

/* AUDIO_META 曲目项：108B（105B + 3B 填充保 4 对齐） */
typedef struct __attribute__((packed)) {
    uint32_t id;
    char     title[96];
    uint8_t  source;         /* 0=WZ 1=QQ */
    uint8_t  pad[3];
    uint32_t duration_s;
} mpak_wire_audio_track_t;

/* 尺寸钉死（小端磁盘格式契约） */
_Static_assert(sizeof(mpak_wire_header_t)       == 40,  "MPAK envelope header must be 40B");
_Static_assert(sizeof(mpak_wire_part_t)         == 20,  "PARTS index entry must be 20B");
_Static_assert(sizeof(mpak_wire_frame_hdr_t)    == 12,  "LAYOUT frame header must be 12B");
_Static_assert(sizeof(mpak_wire_piece_t)        == 12,  "LAYOUT piece must be 12B");
_Static_assert(sizeof(mpak_wire_strip_t)        == 14,  "BGMAP strip header must be 14B");
_Static_assert(sizeof(mpak_wire_bgmap_hdr_t)    == 56,  "BGMAP payload header must be 56B");
_Static_assert(sizeof(mpak_wire_font_hdr_t)     == 8,   "FONT payload header must be 8B");
_Static_assert(sizeof(mpak_wire_glyph_t)        == 12,  "FONT glyph index entry must be 12B");
_Static_assert(sizeof(mpak_wire_audio_track_t)  == 108, "AUDIO_META track entry must be 108B");

/* ------------------------------------------------------------------ */
/* 运行时结构（自然对齐，解析后的宿主视图）                              */
/* ------------------------------------------------------------------ */

#define MPAK_NAME_LEN 32            /* 所有定长 UTF-8 名字区 */
#define MPAK_EXPR_NONE 255u

typedef struct {
    uint32_t id;
    uint16_t expr_group;            /* face 件表情组号；非 face = 0 */
    uint16_t w, h;
    int16_t  origin_x, origin_y;
    uint32_t offset;                /* 位图数据区内偏移 */
    uint32_t extent;                /* 至下一 part 起点的字节数（推导用） */
    bool     has_alpha;             /* 由 extent 推导：pixels+1bit mask */
    uint32_t pixel_bytes;           /* h * align4(w*2)                     */
    uint32_t mask_bytes;            /* (w*h+7)/8（has_alpha 时有效）        */
} mpak_part_t;

typedef struct {
    uint32_t part_id;
    uint8_t  expr_index;            /* MPAK_EXPR_NONE = 非表情件 */
    int16_t  x, y;                  /* 世界 1x 锚点坐标（减 origin 后为左上） */
    uint8_t  flip;                  /* bit0 = 水平翻转 */
    int8_t   z;
} mpak_piece_t;

typedef struct {
    uint32_t delay_ms;
    int16_t  move_dx, move_dy;      /* 进入该帧时实体位移（世界 1x px） */
    uint32_t piece_count;
    uint32_t piece_off;             /* -> layout->pieces 数组下标 */
} mpak_frame_t;

typedef struct {
    uint32_t entity_id;
    char     action[MPAK_NAME_LEN + 1];
    uint32_t frame_count;
    uint32_t expression_count;      /* 本动作支持的表情数 */
    char   (*expr_names)[MPAK_NAME_LEN + 1]; /* [expression_count] */
    mpak_frame_t *frames;           /* [frame_count]（解析后按 z 升序排 piece 索引另行处理） */
    mpak_piece_t *pieces;           /* [sum(piece_count)] */
    uint32_t pieces_total;
} mpak_layout_t;

typedef struct {
    uint64_t part_ref;
    int16_t  y;                     /* 世界 1x */
    int16_t  speed_x;
    uint8_t  rx_parallax;
    uint8_t  blend;
} mpak_strip_t;

typedef struct {
    char     map_id[MPAK_NAME_LEN + 1];
    uint16_t vw, vh;                /* 导出视口（profile 定制） */
    uint32_t static_back_len, static_back_off;
    uint32_t tile_layer_len, tile_layer_off;
    uint32_t strip_count;
    mpak_strip_t *strips;           /* [strip_count] */
} mpak_bgmap_t;

typedef struct {
    uint32_t unicode;
    uint16_t w, h;
    uint8_t  advance;
    int8_t   off_x;
    int8_t   bearing_y;
} mpak_glyph_t;

typedef struct {
    uint8_t  size_px;
    uint8_t  bpp;                   /* 固定 4（A4），否则 MPAK_ERR_FMT */
    uint32_t glyph_count;
    mpak_glyph_t *glyphs;           /* 按 unicode 升序（加载时排序） */
    uint32_t *bmp_prefix;           /* [glyph_count+1] 位图区前缀和（索引序紧凑存放） */
    uint32_t bmp_base_off;          /* 位图数据区 payload 相对偏移 = 8 + 12*n */
    uint32_t max_bmp_bytes;         /* 单字形位图最大字节数（缓存分配用） */
} mpak_font_t;

typedef struct {
    uint32_t id;
    char     title[97];
    uint8_t  source;
    uint32_t duration_s;
} mpak_track_t;

typedef struct {
    uint32_t track_count;
    mpak_track_t *tracks;
} mpak_audio_t;

/* ------------------------------------------------------------------ */
/* 句柄                                                                */
/* ------------------------------------------------------------------ */

typedef struct mpak {
    FILE    *f;
    uint64_t content_hash;
    uint64_t kind;
    uint32_t payload_len;
    uint32_t payload_off;
    union {
        mpak_layout_t *layout;
        mpak_bgmap_t  *bgmap;
        mpak_font_t   *font;
        mpak_audio_t  *audio;
        void          *any;         /* PARTS 索引表见下 */
    } u;
    /* PARTS 专用（避免 union 深层嵌套） */
    mpak_part_t *parts_tab;         /* [parts_count] */
    uint32_t     parts_count;
    uint32_t     parts_bmp_base;    /* payload 相对：4 + 20*n */
} mpak_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/*
 * 打开并校验一个 .mpk，解析头+索引到 PSRAM。FILE* 由句柄持有直至 mpak_close。
 * expect_hash：manifest 期望的 xxhash64（0 = 跳过比对）。
 * expect_kind：MPAK_KIND_*（0 = 任意）。
 * 返回 MPAK_OK 或 MPAK_ERR_*；失败时句柄被关闭清零，可安全 mpak_close。
 */
int  mpak_open(mpak_t *m, const char *path, uint64_t expect_hash, uint64_t expect_kind);
void mpak_close(mpak_t *m);

uint64_t mpak_content_hash(const mpak_t *m);

/* 文件绝对偏移读（懒加载原语） */
int  mpak_read_at(mpak_t *m, uint32_t file_off, void *dst, size_t len);

/* ---- PARTS ---- */
/* 线性查找（n≈百级，帧内 ~15 次查找，足够） */
const mpak_part_t *mpak_parts_find(const mpak_t *m, uint32_t part_id);
/*
 * 表情变体：给定组内任一成员 p（其 expr_group!=0），返回组内第 idx 个变体
 * （组内顺序 = parts 索引顺序 = LAYOUT expression 列表顺序）。
 * idx 越界或组不存在时返回 NULL（调用方回退用 p 本身）。
 */
const mpak_part_t *mpak_parts_variant(const mpak_t *m, const mpak_part_t *p, uint32_t idx);
/* 懒读位图像素 / alpha 掩码到 dst（cap 不足返回 MPAK_ERR_ARG） */
int mpak_part_read_pixels(const mpak_t *m, const mpak_part_t *p, uint8_t *dst, size_t cap);
int mpak_part_read_mask(const mpak_t *m, const mpak_part_t *p, uint8_t *dst, size_t cap);

/* ---- LAYOUT：直接访问 m->u.layout（frames/pieces/expr_names） ---- */

/* ---- BGMAP ---- */
int mpak_bgmap_read_static(const mpak_t *m, uint8_t *dst, size_t cap);
int mpak_bgmap_read_tile(const mpak_t *m, uint8_t *dst, size_t cap);

/* ---- FONT ---- */
/* unicode 二分查找；找到填充 *g_out 并返回 MPAK_OK */
int mpak_font_find_glyph(const mpak_t *m, uint32_t unicode, const mpak_glyph_t **g_out);
/* 懒读字形位图（4bpp，行按字节对齐 = LVGL A4 兼容） */
int mpak_font_read_glyph_bmp(const mpak_t *m, const mpak_glyph_t *g, uint8_t *dst, size_t cap);

/* ---- AUDIO_META：直接访问 m->u.audio ---- */

/* crc32c（Castagnoli，反射多项式 0x82F63B78，init/final 0xFFFFFFFF），
 * 供回放校验工具与测试复用 */
uint32_t mpak_crc32c(uint32_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RENDER_MPAK_H */
