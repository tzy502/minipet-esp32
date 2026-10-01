#!/bin/bash
# 重新烘焙菜单中文字体：自动提取 lvgl_bridge.c 全部菜单用字（防缺字）
# 用法：bash Firmware/board185b/main/render/font_cn/regen.sh [额外字符集文件]
#
# 【2026-10-01 扩展】可选字符集文件：烘「源码用字 ∪ 该文件用字」。
#   用途：地图列表要显示**服务端下发的动态中文地图名**（WZ 真名，如「彩虹岛：枫树山丘」），
#   这些字在源码里没有 → 缺字回落数字 id。把 WZ 全量地图名用字落到 extra_chars.txt 即可。
#   · 缺省入参 = 与本脚本同目录的 extra_chars.txt；
#   · 该文件不存在 / 未传参且缺省文件不存在 → 行为与旧版**完全一致**（只烘源码用字，向后兼容）；
#   · 文件格式：一段纯文本，'#' 开头的行是注释（忽略），其余所有非 ASCII 字符都算用字。
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"   # = Firmware/board185b/
SRC="$ROOT/main/render/lvgl_bridge.c"
# 【2026-10-01 修复】OUT_DIR 必须是绝对路径：脚本中段会 cd "$OUT_DIR"，
# 相对路径下末尾的 include 补丁步骤会找不到生成文件（真机踩过：
# npx 已生成 → 补丁步骤 FileNotFoundError 静默留下 lvgl/lvgl.h 错误包含
# → 固件编译失败）。绝对路径下"仓库根相对调用"与"任意目录调用"都成立。
OUT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FONT="/Library/Fonts/Arial Unicode.ttf"
# 可选额外字符集：$1 显式指定，否则用同目录 extra_chars.txt（两者都不存在 = 旧行为）
EXTRA="${1:-$OUT_DIR/extra_chars.txt}"

# 合并「源码字符串字面量 CJK 用字 ∪ 额外字符集非 ASCII 用字」（注释不进来），输出排序去重
python3 - "$SRC" "$EXTRA" > /tmp/menu_chars.txt <<'PY'
import os, re, sys
src, extra = sys.argv[1], sys.argv[2]
s = open(src, encoding='utf-8').read()
strs = re.findall(r'"((?:[^"\\]|\\.)*)"', s)
src_cjk = set()
for t in strs:
    for ch in t:
        if '\u4e00' <= ch <= '\u9fff':
            src_cjk.add(ch)

extra_set = set()
if extra and os.path.isfile(extra):
    for line in open(extra, encoding='utf-8'):
        if line.lstrip().startswith('#'):      # 注释行
            continue
        for ch in line.rstrip('\r\n'):
            if ord(ch) > 0x7e:            # 非 ASCII 都算用字（含全角空格 U+3000；ASCII 0x20-0x7E 恒烘）
                extra_set.add(ch)
    print(f"[regen] 额外字符集 {extra}：{len(extra_set)} 个非 ASCII 用字"
          f"（新增 {len(extra_set - src_cjk)}）", file=sys.stderr)
else:
    print(f"[regen] 未使用额外字符集（{extra} 不存在）——与旧行为一致，仅烘源码用字", file=sys.stderr)

merged = ''.join(sorted(src_cjk | extra_set))
print(f"[regen] 源码 CJK 用字 {len(src_cjk)} + 额外非 ASCII 用字 {len(extra_set)}"
      f" → 合并去重 {len(merged)}", file=sys.stderr)
sys.stdout.write(merged)
PY
CHARS=$(cat /tmp/menu_chars.txt)
echo "菜单用字 $(echo -n "$CHARS" | wc -c | tr -d ' ') 字节 / $(python3 -c "import sys;print(len(open('/tmp/menu_chars.txt',encoding='utf-8').read().strip()))") 字符"

cd "$OUT_DIR"
npx -y lv_font_conv --font "$FONT" --size 22 --bpp 4 --format lvgl \
  --range 0x20-0x7E --symbols "$CHARS" --no-compress \
  -o menu_font_cn.c --lv-font-name menu_font_cn

# lv_font_conv 生成的 include 路径与 IDF 组件不符，固定为 lvgl.h
python3 - "$OUT_DIR/menu_font_cn.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
old = '''#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif'''
assert old in s
s = s.replace(old, '#include "lvgl.h"')
open(p, 'w', encoding='utf-8').write(s)
PY
echo "完成：menu_font_cn.c（重新构建固件生效）"
