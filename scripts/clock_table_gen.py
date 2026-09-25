#!/usr/bin/env python3
"""
clock_table_gen.py — 地图时钟坐标种子表生成（E9：魔法值表初始值）

解析 ~/Desktop/wz-clock-maps.md（WzComparer2 全量扫描 21,422 张地图的实测结果，26 张含 clock 配置），
产出 Server/seed/clock_table.json：{ "<mapId>": [x, y], ... }

要点：
  - 「同上」行继承上一显式坐标行（md 中枫叶路/编年史/神秘岛码头系列大量使用）
  - 地图 ID 归一化为无前导零的十进制字符串（md 中 002000100 为 9 位零填充展示；
    与 manifest 示例 "200000100" 及 C# 侧 int mapId 序列化一致）
  - 坐标为 WZ 世界锚点原值；语义按 R15 = 「烘焙视口屏幕坐标」的初始建议值，Web 可改
  - 899000000 的 countdown=1（倒计时）不进表结构——设备端按 clock-display-spec.md 第六节单独分支

宁准勿快：内置 5 个典型地图锚点断言 + 条数必须恰为 26，任一不符即失败退出、不写文件。

用法：python3 clock_table_gen.py [--source <md路径>] [--output <json路径>]
"""
import argparse
import json
import re
import sys
from pathlib import Path

DEFAULT_SOURCE = Path.home() / "Desktop" / "wz-clock-maps.md"
DEFAULT_OUTPUT = Path(__file__).resolve().parent.parent / "Server" / "seed" / "clock_table.json"

EXPECTED_COUNT = 26

# 锚点自校验（md 中最典型的 5 张，人工核对过）
ANCHORS = {
    "200000100": (110, -225),   # 神秘岛 · 天空之城售票处
    "220000100": (-148, -206),  # 玩具城 · 玩具城售票处
    "240000100": (208, 126),    # 神木村 · 神木村售票处
    "104020110": (635, -226),   # 金银岛 · 天空之城方向升降场
    "200000111": (-793, -170),  # 神秘岛 · 码头〈开往金银岛〉
}

ROW_RE = re.compile(r"^\|\s*(\d{9})\s*\|")       # | 002000100 | ...
XY_RE = re.compile(r"x=(-?\d+)\s+y=(-?\d+)")
SAME_RE = re.compile(r"同上")


def parse(md_text: str):
    table = {}
    last_xy = None
    for line in md_text.splitlines():
        m = ROW_RE.match(line.strip())
        if not m:
            continue
        map_id = str(int(m.group(1)))  # 去前导零
        if map_id in table:
            sys.exit(f"错误：地图 {map_id} 在 md 中重复出现")
        cell = line.rpartition("|")[0]  # 最后一个单元格（clock 字段列）
        xy = XY_RE.search(cell)
        if xy:
            last_xy = (int(xy.group(1)), int(xy.group(2)))
        elif SAME_RE.search(cell):
            if last_xy is None:
                sys.exit(f"错误：{map_id} 为「同上」但之前无显式坐标行")
        else:
            sys.exit(f"错误：{map_id} 行既无 x/y 也无「同上」：{line.strip()}")
        table[map_id] = last_xy
    return table


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    ap.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = ap.parse_args()

    if not args.source.is_file():
        sys.exit(f"错误：找不到源文件 {args.source}")

    table = parse(args.source.read_text(encoding="utf-8"))

    # ---- 校验（宁准勿快）----
    if len(table) != EXPECTED_COUNT:
        sys.exit(f"错误：解析出 {len(table)} 条，期望 {EXPECTED_COUNT} 条")
    for mid, xy in ANCHORS.items():
        if table.get(mid) != xy:
            sys.exit(f"错误：锚点 {mid} 解析为 {table.get(mid)}，期望 {xy}")

    out = {
        "_comment": "视口屏幕坐标初始建议值，Web 可改，来源 wz-clock-maps.md 2026-09-24 实测"
    }
    for mid in sorted(table, key=int):
        out[mid] = list(table[mid])

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(out, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    for mid in sorted(table, key=int):
        print(f"{mid}: {table[mid]}")
    print(f"共 {len(table)} 条 -> {args.output}")


if __name__ == "__main__":
    main()
