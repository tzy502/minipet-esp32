#!/bin/bash
# 重新烘焙菜单中文字体：自动提取 lvgl_bridge.c 全部菜单用字（防缺字）
# 用法：在仓库根执行 bash Firmware/board216/main/render/font_cn/regen.sh
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"   # = Firmware/board216/
SRC="$ROOT/main/render/lvgl_bridge.c"
OUT_DIR="$(dirname "${BASH_SOURCE[0]}")"
FONT="/Library/Fonts/Arial Unicode.ttf"

# 提取源码所有字符串字面量中的 CJK 字符（注释不进来）
python3 - "$SRC" > /tmp/menu_chars.txt <<'PY'
import re, sys
s = open(sys.argv[1]).read()
strs = re.findall(r'"((?:[^"\\]|\\.)*)"', s)
cjk = set()
for t in strs:
    for ch in t:
        if '\u4e00' <= ch <= '\u9fff':
            cjk.add(ch)
print(''.join(sorted(cjk)))
PY
CHARS=$(cat /tmp/menu_chars.txt)
echo "菜单用字 $(echo -n "$CHARS" | wc -c) 个：$CHARS"

cd "$OUT_DIR"
npx -y lv_font_conv --font "$FONT" --size 22 --bpp 4 --format lvgl \
  --range 0x20-0x7E --symbols "$CHARS" --no-compress \
  -o menu_font_cn.c --lv-font-name menu_font_cn

# lv_font_conv 生成的 include 路径与 IDF 组件不符，固定为 lvgl.h
python3 - "$OUT_DIR/menu_font_cn.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '''#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif'''
assert old in s
s = s.replace(old, '#include "lvgl.h"')
open(p, 'w').write(s)
PY
echo "完成：menu_font_cn.c（重新构建固件生效）"
