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

/* 包能力上限。唯一改动 = 8MB → 64MB：
 *   旧上限 8MB 的依据是「FONT 32px 全量 ~1.8MB」；R2 整图 BGMAP payload
 *   = 16,924,968 B（16.9MB）会被旧上限直接 MPAK_ERR_LEN 拒收。
 *   放宽只影响「接受更长的 payload」，不改任何字段口径/校验序；
 *   ≤8MB 的旧包在新上限下走完全相同的代码路径（长度校验是上界判断）。 */
#define MPAK_MAX_PAYLOAD     (64u * 1024u * 1024u)

/* BGMAP 世界尺寸上限（vw/vh）。旧硬编码 512（= 240 屏窗口口径的 2× 余量）；
 * 整图包 vw=2270/vh=1807 需要放过。放宽只影响上界判断，240×240 旧包不受影响。 */
#define MPAK_MAX_BGMAP_DIM   8192u

/* 大包 CRC 策略阈值（详见 mpak.c envelope_check 注释）：
 * payload_len > MPAK_CRC_SKIP_BYTES 时跳过全量 CRC32C（否则 16.9MB 每次 open
 * 都要把整包读一遍，真机实测 ≈ 1.06MB/s ⇒ ≈ 16 秒/次）。
 * 逃生阀：编译期 -DMPAK_CRC_SKIP_BYTES=0 覆盖（#ifndef 包裹，命令行优先生效）
 * 即恢复"永远全量 CRC"的旧语义。 */
#ifndef MPAK_CRC_SKIP_BYTES
#define MPAK_CRC_SKIP_BYTES  (4u * 1024u * 1024u)
#endif
/* 跳过大包 CRC 时仍读入的 payload 头部字节数（指纹，不是可与尾部比对的校验和） */
#define MPAK_CRC_HEAD_BYTES  4096u

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

/* BGMAP 整图扩展块（R2；**可选**，位于 tile 数据之后 4B 对齐处）：
 *   ext_off = align4(tile_layer_off + tile_layer_len)
 *   [u32 magic = 0x4D504745（文件字节 45 47 50 4D）]
 *   [u32 ground_len = vw*2（字节）]
 *   [u32 ground_off = payload 相对地面表首字节]
 *   [u32 flags] bit0 = 1 = 整图包（vw/vh = 整图世界尺寸、strip.y = 世界系 y）
 *   之后紧跟 ground_len 字节的 u16 小端地面表（每列 = 世界系地面 y）。
 * 旧包此处越界/无 magic ⇒ 按无扩展块处理（full_map=false），行为与改动前一致。
 * 服务端权威定义：Server/MinipetServer/Export/BgmapPackWriter.cs。 */
#define MPAK_BGMAP_EXT_MAGIC      0x4D504745u
#define MPAK_BGMAP_EXT_HDR_LEN    16u
#define MPAK_BGMAP_FLAG_FULL_MAP  0x1u
/* flags bit1 = 瓦片存储（TILED）。契约 docs/ai/map-tiled-format-contract.md §1：
 * 置位 ⇒ static / tile / 条带三类像素层（及 tile 掩码）按 128×128 世界像素瓦片布局存。
 * **不作为唯一判据**：瓦片布局还有一条硬证据「层长度恒等式」——逐行口径的长度集合
 * 与瓦片口径的长度集合不相交（唯一例外是层恰好等于一整块瓦片，此时两种布局逐字节相同），
 * 所以固件按长度反解瓦片边长，bit1 只用来做一致性交叉校验（见 parse_bgmap）。 */
#define MPAK_BGMAP_FLAG_TILED     0x2u
/* 瓦片边长候选表首项（契约 §2 现为 128）。固件**不硬编码**：边长从包内层长度
 * 恒等式反解（gx*gy*TILE*TILE*2 == 声明的层长度），候选表按 128 优先。 */
#define MPAK_BGMAP_TILE_DEFAULT   128
/* 地面表「该列无 foothold」哨兵 */
#define MPAK_BGMAP_GROUND_NONE    0xFFFFu
/* 地面表常驻缓存的字节上限（= MPAK_MAX_BGMAP_DIM × 2 = 16KB；见 mpak.c 取舍说明）。
 * 逃生阀：-DMPAK_BGMAP_GROUND_CACHE_MAX=0 即完全不缓存 → mpak_bgmap_ground_y
 * 逐列按需读 2B（PSRAM 更紧时可用；两条路径都实测过）。 */
#ifndef MPAK_BGMAP_GROUND_CACHE_MAX
#define MPAK_BGMAP_GROUND_CACHE_MAX (MPAK_MAX_BGMAP_DIM * 2u)
#endif

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
    uint32_t pixel_bytes;           /* 行口径：h * align4(w*2)；瓦片口径：gx*gy*TILE*TILE*2 */
    uint32_t mask_bytes;            /* 行口径：(w*h+7)/8；瓦片口径：gx*gy*(TILE*TILE/8) */
    /* ── 瓦片（契约 §2；2026-10-01）——只增字段，行口径读者不受影响 ──
     * 真机上只有"整图条带"（BGMAP 引用的小 PARTS 包，part 0 = 带图）会是瓦片存储：
     * 服务端把 static/tile/条带三类像素层一起改瓦片，条带像素在独立小包里，
     * 所以**包自身**必须能自证瓦片布局（不能依赖 BGMAP 的 bit1，那是另一个文件）。 */
    bool     tiled;                 /* true = 本件像素/掩码按瓦片布局存 */
    uint16_t tile;                  /* 瓦片边长（世界 px，包内长度反解，128 档） */
    uint16_t gx, gy;                /* 瓦片网格 = ceil(w/tile) × ceil(h/tile) */
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
    uint16_t vw, vh;                /* 导出视口（profile 定制）；整图包 = 整图世界尺寸 */
    uint32_t static_back_len, static_back_off;
    uint32_t tile_layer_len, tile_layer_off;
    uint32_t strip_count;
    mpak_strip_t *strips;           /* [strip_count] */

    /* ══ 整图扩展（R2 2026-10-01）——**只增字段，现有字段语义/顺序一律不动**，
     *    并行的 compositor 读者按原字段读取不受影响。契约 §3.1 把这组字段
     *    描述为 `mpak_bgmap_ext_t`；此处按契约的字段清单**平铺**在 mpak_bgmap_t 上
     *    （少一层嵌套，避免同一状态两处存放）。无扩展块的旧包：全为 0/NULL。 ══ */
    bool     full_map;              /* ext flags bit0：整图包（vw/vh=世界尺寸、strip.y=世界系 y） */
    uint32_t ground_off;            /* payload 相对地面表首字节；0=无 */
    uint32_t ground_len;            /* 地面表字节数（= vw*2）；0=无 */
    int32_t  ext_off;               /* payload 相对扩展块首字节；<0 = 无扩展块 */
    uint32_t ext_flags;             /* 扩展块 flags 原值（诊断用） */
    const uint16_t *ground;         /* 地面表常驻缓存（**host 序** u16[vw]，MPAK_BGMAP_GROUND_NONE=无）；
                                     * NULL = 未缓存（>16KB 或旧包）→ 逐列按需读 */
    /* ══ 瓦片布局（契约 docs/ai/map-tiled-format-contract.md；2026-10-01）══════
     * 只增字段；tiled=false 时下面全部为 0，逐行路径逐字节不变。 */
    bool     tiled;                 /* true = static/tile 层按瓦片存储（长度反解 + bit1 交叉校验） */
    int32_t  tile;                  /* 瓦片边长（世界 px；从层长度恒等式反解，非硬编码） */
    int32_t  gx, gy;                /* 网格 = ceil(vw/tile) × ceil(vh/tile)（行主序 idx = ty*gx+tx） */
    uint32_t tile_px_bytes;         /* 像素区长度 = gx*gy*tile*tile*2（static 与 tile 层同值） */
    uint32_t tile_mask_bytes;       /* tile 掩码区长度 = gx*gy*(tile*tile/8) */
    uint32_t tile_mask_off;         /* tile 掩码区 payload 相对偏移（= tile_layer_off + tile_px_bytes；
                                     * 行口径请继续用 vh*align4(vw*2)，两者不可混用） */
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
    uint32_t file_id;               /* 本句柄的文件身份（open 时全局自增分配）。
                                     * 瓦片缓存键用它而**不是 fd**：fd 会被 close/reopen 复用，
                                     * 用 fd 做键会把"另一个文件的同号 fd"命中成旧瓦片。 */
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

/* ══ 整图（R2）分块读原语 + 世界系地面表（契约 §3.1；2026-10-01）══
 * 语义：把**世界矩形** [x, x+w) × [y, y+h) 读到 dst。
 *   · dst 行距由调用方给出（**像素**计，必须 ≥ w；越界区域补 0，不报错）；
 *   · 源行距与 tile 掩码位序按包内真实布局解析（奇数宽整图 = 行 4B 对齐），
 *     调用方**不需要**知道 vw 是否为偶数；
 *   · layer 选择靠函数名：static（RGB565）/ tile（RGB565）/ tile 掩码（1B/像素 0|1）；
 *   · tile 掩码与 tile 颜色**分开读**（一次调用只碰一层，避免多读一倍字节）。
 * 返回 MPAK_OK 或 MPAK_ERR_*（ARG/FMT/RANGE/IO）。窗口完全越界 = 全 0 + MPAK_OK。 */
int mpak_bgmap_read_static_rect(const mpak_t *m, int32_t x, int32_t y,
                                int32_t w, int32_t h, uint16_t *dst, int32_t dst_stride_px);
int mpak_bgmap_read_tile_rect(const mpak_t *m, int32_t x, int32_t y,
                              int32_t w, int32_t h, uint16_t *dst, int32_t dst_stride_px);
int mpak_bgmap_read_tile_mask_rect(const mpak_t *m, int32_t x, int32_t y,
                                   int32_t w, int32_t h, uint8_t *dst /*每像素 1B，0/1*/,
                                   int32_t dst_stride_px);

/* 世界系地面 Y：world_x 越界 / 无地面表 / 该列为 0xFFFF → INT32_MIN（调用方回落通用线）。
 * 有地面表时返回原始世界 y（不夹取；调用方自行判断是否在 [0,vh) 内）。 */
int32_t mpak_bgmap_ground_y(const mpak_t *m, int32_t world_x);

/* ══ 瓦片(tile)整块读原语 + PSRAM 瓦片缓存（契约 §2/§6）═════════════════════
 * 给两个消费方共用：
 *   · BGMAP 的 static/tile/掩码层 —— mpak_bgmap_read_*_rect 内部自动分流（见下）；
 *   · 整图条带（独立小 PARTS 包，compositor 自己 open/pread）—— 用下面三个函数，
 *     由调用方给出 fd + 层基址 + 层尺寸 + 瓦片边长。
 * 为什么要有缓存（而不是每次直接 pread 到目标）：相机平移是一小条一小条补的
 * （补 24 列世界 px），而瓦片是 128×128 —— 同一块瓦片在一次拖动里会被反复需要。
 * 缓存命中时取像素是**纯 PSRAM memcpy**，一次 SD 命令都不发（契约 §6 硬要求：
 * 只允许整块 pread，绝不再按行/按像素读）。
 * 线程契约：只在渲染任务（持 rc_lock 的合成/同步路径）里访问，无锁。
 * 内存纪律：全部 MALLOC_CAP_SPIRAM（内部堆只剩几十 KB，抢内部堆 = newlib abort）。 */
typedef struct {
    uint32_t file_id;               /* 文件身份（mpak_t.file_id 或 mpak_path_id(path)） */
    int      fd;                    /* 该文件的 fd（pread 用；本模块不 open/close/dup） */
    uint32_t base_off;              /* 层像素区/掩码区首字节**绝对文件偏移** */
    int32_t  lw, lh;                /* 层矩形尺寸（世界 px，= 该层自身宽高） */
    int32_t  tile;                  /* 瓦片边长（世界 px） */
    int32_t  gx, gy;                /* 网格 = ceil(lw/tile) × ceil(lh/tile) */
} mpak_tile_src_t;

/* 填 src（gx/gy 由 lw/lh/tile 算）。tile ≤ 0 或 lw/lh ≤ 0 → gx/gy = 0（读取会报 FMT）。
 * block = tile*tile（像素 2B/px、掩码 1bit/px 的块内行均按 tile 算）。 */
void mpak_tile_src_init(mpak_tile_src_t *s, uint32_t file_id, int fd, uint32_t base_off,
                        int32_t lw, int32_t lh, int32_t tile);

/* 取层上 [x,x+w)×[y,y+h)：像素（RGB565）/ 掩码（1B/px，0|1）。
 * 越界（<0 或 ≥lw/lh）补 0，不报错 —— 与逐行口径的 rect 读语义一致。
 * 内部：每块一次 pread(tile*tile*2 或 tile*tile/8) → memcpy 出行段。 */
int mpak_tile_read_px(const mpak_tile_src_t *s, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint16_t *dst, int32_t dst_stride_px);
int mpak_tile_read_mask(const mpak_tile_src_t *s, int32_t x, int32_t y, int32_t w, int32_t h,
                        uint8_t *dst, int32_t dst_stride_px);

/* 预取覆盖 [x,x+w)×[y,y+h) 的全部瓦片到缓存（命中零 IO）。返回 MPAK_OK / MPAK_ERR_*。 */
int mpak_tile_prefetch(const mpak_tile_src_t *s, int32_t x, int32_t y, int32_t w, int32_t h);

/* BGMAP 层预取（装载期"一次性预取相机窗口覆盖的瓦片"，契约 §6 末条）。
 * tiled=false 时是 no-op（返回 MPAK_OK），旧包调用安全。 */
int mpak_bgmap_prefetch_static(const mpak_t *m, int32_t x, int32_t y, int32_t w, int32_t h);
int mpak_bgmap_prefetch_tile(const mpak_t *m, int32_t x, int32_t y, int32_t w, int32_t h,
                             bool with_mask);

/* 瓦片缓存统计（渲染侧日志口径：瓦片读 N 块 / 命中 M 块 / N KB）：
 * blocks = 实际发 SD 命令的整块读次数，hits = 命中缓存的块次数，bytes = 前者字节数。 */
void mpak_tile_stat_reset(void);
void mpak_tile_stat_get(uint32_t *blocks, uint32_t *hits, uint32_t *bytes);
/* 缓存全失效（换图/卸载时调用）：只清键不释放缓冲（下次直接复用，免得反复 malloc）。
 * 为什么必须清：条带文件身份用 path 哈希，同路径文件被替换（重新下载）后旧键会命中错像素。 */
void mpak_tile_cache_flush(void);

/* 路径 → 文件身份（FNV-1a 32；条带层用。返回 ≥1，0 保留给"无身份"）。 */
uint32_t mpak_path_id(const char *path);

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
