# fontpack — 字体种子包工具链（E12）

宋体 → 三档 4bpp 位图 → `Server/seed/fonts/font-{16,24,32}.json`（服务端 FontPackWriter / kind=4 FONT 的源）。

## 产出

| 文件 | 说明 |
|---|---|
| `gen_charset.js` | 生成 `charset-16/24.txt`（ASCII 95 + GB2312 一级 3755 + 常用标点 42，共 3892 字）与 `charset-32.txt`（标题子集 666 字：ASCII + 一级表前 400 + 界面词手补） |
| `build-fonts.sh` | 一键构建：字符集 → 拆 TTC → lv_font_conv 栅格化 → convert.js 转中间 JSON |
| `convert.js` | lv_font_conv dump 输出 → FontPackWriter 友好 JSON（灰度 8bpp → 4bpp 量化） |

## 用法

```bash
cd scripts/fontpack
npm install                 # 首次：装 lv_font_conv（1.5.3）
python3 -m pip install --user fonttools   # 首次：拆 TTC 用（仅 macOS 系统 .ttc 需要）
./build-fonts.sh            # 全流程重跑（幂等）
```

依赖 macOS 系统宋体 `/System/Library/Fonts/Supplemental/Songti.ttc`（实测存在）。

## 三档体积实测（2026-09-26，Songti SC Regular）

| 档 | 字符数 | 中间 JSON | 位图区 | **bin 包估算**（位图 + 12B/glyph 索引） | 预算对照（algorithm-asset-format.md 第九节） |
|---|---|---|---|---|---|
| 16px | 3892 | 1247.7KB | 468.1KB | **513.7KB** | 估算 448KB |
| 24px | 3892 | 2311.0KB | 999.7KB | **1045.3KB** | 估算 1008KB（3500 字全量） |
| 32px | 666 | 599.4KB | 273.0KB | **280.8KB** | 估算 256KB（~500 字） |

> bin 包估算 = 服务端 FontPackWriter 实际产出的 kind=4 payload 期望值；中间 JSON 大小仅供参考。
> 32px 子集 666 字（略超 ~500 目标）：界面词手补宁超勿缺，标题缺字设备端只能显豆腐块且 Web 不可修。多出 ~25KB 在预算内。

## 中间 JSON 格式（`Server/seed/fonts/font-N.json`）

```json
{
  "size_px": 24, "bpp": 4, "glyph_count": 3892,
  "bitmap_layout": "4bpp，行按字节对齐（行字节数=ceil(w/2)），行内左像素在高半字节，hex 每字节 2 字符",
  "glyphs": [
    { "unicode": 65, "w": 18, "h": 17, "advance": 16, "off_x": -1, "bearing_y": 17,
      "bitmap": "00f0d600..." }
  ]
}
```

- 字段命名对齐 asset 格式第六节（unicode u32 / w,h u16 / advance u8 / off_x i8 / bearing_y i8）
- `bitmap`：4bpp 十六进制；空格（U+0020）w=h=0、仅 advance
- 已含 `ascent`/`descent`（行距计算可用）

## 替换字体（如换回 Windows SimSun 或其他宋体）

1. 取得单 TTF 文件（**TTC 需先拆**：lv_font_conv 内置 opentype.js 不支持 `ttcf` 签名；
   `python3 -c "from fontTools.ttLib import TTCollection; TTCollection('SimSun.ttc').fonts[0].save('simsun.ttf')"`)
2. 改环境变量重跑（不动脚本）：
   ```bash
   SYSTEM_SONGTI_TTC=/path/to/your.ttc TTC_FONT_INDEX=0 ./build-fonts.sh
   ```
   （TTF 单文件时 index 恒为 0；`FORCE_EXTRACT=1` 强制重抽缓存）
3. 校验 `Server/seed/fonts/font-24.json` 的 `glyph_count` 仍为 3892（缺字说明新字体覆盖不全，需换源或补 fallback）

## 字符集调整

- 16/24 档：改 `gen_charset.js` 顶部 `CJK_PUNCT`（常用标点）即可增删；一级表按编码区 `0xB0A1-0xD7F9` 算法生成，勿手改
- 32 档：改 `TITLE_EXTRA_WORDS`（界面词手补表），去重逻辑自动处理
- charset 文件为**单行无分隔符字符序列（含空格）**，消费时只可剔除 `\n`/`\r`，不可按空白剥离

## 已知事项

- lv_font_conv 无原生 `--format json`；实际用 `--format dump --full-info`（FreeType 8bpp 灰度 + 整数像素 metrics），由 convert.js 自行量化 4bpp——与任务要求的 JSON 产物等价
- dump 模式会顺带在 `build/lv-font-N/` 写每字形 PNG（调试用，已 gitignore）
