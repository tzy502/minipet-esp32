#!/usr/bin/env bash
#
# build-fonts.sh — 字体种子包一键构建（E12：宋体 → 三档 4bpp 位图 → Server/seed/fonts/）
#
# 流程：
#   1. gen_charset.js 生成 charset-16/24/32.txt
#   2. 从 macOS 系统 Songti.ttc 抽出单 TTF（Songti SC Regular，index 6）——opentype.js 不支持 TTC
#   3. lv_font_conv（dump+FreeType）按三档字号栅格化
#   4. convert.js 转「服务端 FontPackWriter 友好」中间 JSON → ../../Server/seed/fonts/font-{16,24,32}.json
#
# 依赖：node>=18 + npm；python3 + fonttools（pip3 install --user fonttools，仅用于拆 TTC）
# 可重跑：幂等，中间产物在 build/ 下，源字体缺失时自动重新抽取。
#
set -euo pipefail
cd "$(dirname "$0")"

SYSTEM_TTC="${SYSTEM_SONGTI_TTC:-/System/Library/Fonts/Supplemental/Songti.ttc}"
TTC_FONT_INDEX="${TTC_FONT_INDEX:-6}"   # 6 = Songti SC Regular
TTF=build/songti.ttf
OUT_DIR=../../Server/seed/fonts

mkdir -p build "$OUT_DIR"

echo "== [1/4] 字符集 =="
node gen_charset.js

echo "== [2/4] 宋体源（${SYSTEM_TTC} #${TTC_FONT_INDEX}）=="
if [ ! -f "$TTF" ] || [ "${FORCE_EXTRACT:-0}" = "1" ]; then
  python3 - "$SYSTEM_TTC" "$TTC_FONT_INDEX" "$TTF" <<'PY'
import sys
try:
    from fontTools.ttLib import TTCollection
except ImportError:
    sys.exit("缺 fonttools：请执行  python3 -m pip install --user fonttools  后重跑")
src, idx, dst = sys.argv[1], int(sys.argv[2]), sys.argv[3]
ttc = TTCollection(src)
name = ttc.fonts[idx]['name'].getDebugName(4)
ttc.fonts[idx].save(dst)
print(f"抽出 #{idx} {name} -> {dst}")
PY
else
  echo "复用 ${TTF}（FORCE_EXTRACT=1 可强制重抽）"
fi

echo "== [3/4] lv_font_conv 栅格化（4bpp，dump+FreeType）=="
for SIZE in 16 24 32; do
  CHARSET="charset-${SIZE}.txt"
  DUMP="build/lv-font-${SIZE}"
  rm -rf "$DUMP"
  # 只剔除换行，不可按空白剥离（字符集内含空格 0x20）
  SYMBOLS="$(tr -d '\n\r' < "$CHARSET")"
  npx --no-install lv_font_conv \
    --font "$TTF" \
    --size "$SIZE" \
    --bpp 4 \
    --no-compress --no-prefilter --no-kerning \
    --format dump --full-info \
    --symbols "$SYMBOLS" \
    -o "$DUMP"
  echo "lv_font_conv size=$SIZE 完成（dump: $DUMP/font_info.json）"
done

echo "== [4/4] 转中间 JSON -> $OUT_DIR =="
SRC_DESC="Songti.ttc#${TTC_FONT_INDEX}(Songti SC Regular) via lv_font_conv 1.5.3 dump/FreeType 8bpp→4bpp"
for SIZE in 16 24 32; do
  node convert.js "build/lv-font-${SIZE}/font_info.json" "$SIZE" "$OUT_DIR/font-${SIZE}.json" "$SRC_DESC"
done

echo "== 完成 =="
ls -l "$OUT_DIR"
