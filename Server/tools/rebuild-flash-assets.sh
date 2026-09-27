#!/usr/bin/env bash
# rebuild-flash-assets.sh — 重建设备「出厂素材」并写入内部 Flash assets 分区
#
# 【为什么需要它】设备日常从 TF 卡拉素材；TF 卡坏/未插时固件回落到内部 Flash 的
# assets FAT 分区（partitions.csv: assets @0x620000 6MB）。该分区里的素材是**出厂
# 快照**：一旦服务端按新的 petConfig 重新生成资产（换装/换宠），设备又拿不到 TF，
# 就会出现「LAYOUT 是新的、配套 PARTS 还是旧的」→ 整帧部件解析失败 →
# **人物整个消失**（真机实证：`piece part 1106 not in PARTS pkg`，用户报障
# 「人物也没了」）。本脚本用服务端当前外观重新导出素材、打成 FAT 镜像、刷进分区，
# 让设备在没有 TF 卡的情况下也能正常显示人物。
#
# 用法：
#   Server/tools/rebuild-flash-assets.sh --server http://<NAS_IP>:38090 --device dev-693ea4
#   # 只生成镜像不刷机：
#   Server/tools/rebuild-flash-assets.sh --server ... --device ... --no-flash
#
# 依赖：dotnet（可在 .env 里用 DOTNET 覆盖）、ESP-IDF（取 fatfsgen.py）、esptool、curl、python3
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERVER_URL=""
DEVICE_ID=""
WZ_PATH="${MINIPET_WZ_DATA:-/Volumes/SSD/mxd/mxd/Data}"
PORT="${ESPPORT:-/dev/cu.usbmodem21201}"
OUT_DIR=""
DO_FLASH=1
DOTNET="${DOTNET:-/Volumes/SSD/C#/dotnet}"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
PY="${PY:-$HOME/.espressif/python_env/idf5.5_py3.9_env/bin/python}"
ASSETS_OFFSET="0x620000"
ASSETS_SIZE="0x600000"

while [ $# -gt 0 ]; do
  case "$1" in
    --server) SERVER_URL="$2"; shift 2 ;;
    --device) DEVICE_ID="$2"; shift 2 ;;
    --wz)     WZ_PATH="$2"; shift 2 ;;
    --port)   PORT="$2"; shift 2 ;;
    --out)    OUT_DIR="$2"; shift 2 ;;
    --no-flash) DO_FLASH=0; shift ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

[ -n "$SERVER_URL" ] || { echo "缺少 --server（例：http://<NAS_IP>:38090）" >&2; exit 2; }
[ -n "$DEVICE_ID" ]  || { echo "缺少 --device（设备 id，例：dev-693ea4）" >&2; exit 2; }
[ -d "$WZ_PATH" ]    || { echo "WZ 数据目录不存在：$WZ_PATH（用 --wz 指定）" >&2; exit 2; }
OUT_DIR="${OUT_DIR:-$(mktemp -d /tmp/minipet-assets.XXXXXX)}"
mkdir -p "$OUT_DIR"

echo "① 取设备外观配置（petConfig）"
APPEARANCE="$OUT_DIR/appearance.json"
curl -fsS "$SERVER_URL/api/admin/devices/$DEVICE_ID" \
  | "$PY" -c "import sys,json; d=json.load(sys.stdin)['device']; pc=d.get('petConfig'); assert pc, '该设备没有 petConfig（先在 Web 换宠换装）'; json.dump(pc, open('$APPEARANCE','w',encoding='utf-8'), ensure_ascii=False)"
echo "   → $APPEARANCE"

echo "② 用同一外观导出素材（哈希须与服务端 manifest 一致）"
EXPORT_ROOT="$OUT_DIR/export"
"$DOTNET" run --project "$ROOT/Server/tools/Exporter" -- \
  --wz "$WZ_PATH" --appearance "$APPEARANCE" --profile amoled216 \
  --device-id "$DEVICE_ID" --no-fonts --no-audio --no-fonttime \
  --out "$EXPORT_ROOT" > "$OUT_DIR/export.log" 2>&1 || { tail -20 "$OUT_DIR/export.log"; exit 1; }
SRC="$EXPORT_ROOT/$DEVICE_ID"
[ -f "$SRC/manifest-assets.json" ] || { echo "导出失败：没有 manifest-assets.json" >&2; tail -20 "$OUT_DIR/export.log"; exit 1; }

echo "③ 组装设备端目录树（minipet/{parts,layout,bg,font,audio} + manifest.json）"
TREE="$OUT_DIR/fs"
mkdir -p "$TREE/minipet"/{parts,layout,bg,font,audio}
"$PY" - "$SRC" "$TREE" <<'PY'
import json, os, shutil, sys
src, tree = sys.argv[1], sys.argv[2]
mf = json.load(open(os.path.join(src, 'manifest-assets.json'), encoding='utf-8'))
sub = {'PARTS':'parts','LAYOUT':'layout','BGMAP':'bg','FONT':'font','AUDIO_META':'audio'}
n = 0
for h, v in mf['assets'].items():
    f = os.path.join(src, h + '.mpak')
    if not os.path.exists(f): continue
    d = sub.get(v.get('kind'))
    if not d: continue
    shutil.copy(f, os.path.join(tree, 'minipet', d, h + '.mpk'))
    n += 1
lm = {'rev': 1, 'assets': {h: {'kind': v['kind'], 'selector': v.get('selector',''),
                               'action': v.get('action','')} for h, v in mf['assets'].items()}}
json.dump(lm, open(os.path.join(tree, 'minipet', 'manifest.json'), 'w', encoding='utf-8'), ensure_ascii=False)
print(f'   拷入 {n} 个资产包')
PY

echo "④ 生成 FAT 镜像（4K 扇区，与 assets 分区 6MB 对齐）"
IMG="$OUT_DIR/assets.bin"
"$PY" "$IDF_PATH/components/fatfs/fatfsgen.py" --output_file "$IMG" \
  --partition_size "$ASSETS_SIZE" --sector_size 4096 --sectors_per_cluster 1 \
  --long_name_support "$TREE" > "$OUT_DIR/fatfsgen.log" 2>&1 \
  || { tail -10 "$OUT_DIR/fatfsgen.log"; exit 1; }
echo "   → $IMG ($(wc -c < "$IMG") bytes)"

if [ "$DO_FLASH" = "1" ]; then
  echo "⑤ 写入 assets 分区（$ASSETS_OFFSET，$(basename "$PORT")）"
  # 串口可能被其他进程占用（串口监视/日志采集）：先给出可读提示
  "$PY" -m esptool --chip esp32s3 -p "$PORT" -b 921600 \
    write_flash "$ASSETS_OFFSET" "$IMG" || {
      echo "刷写失败：确认串口 $PORT 未被占用（lsof $PORT）、设备已连接" >&2; exit 1; }
  echo "⑥ 复位设备并确认落地"
  echo "   看串口应出现：'内部 Flash assets 分区已挂载 /sdcard（出厂素材模式）'"
  echo "   且不再出现 'piece part ... not in PARTS pkg'"
else
  echo "⑤ 跳过刷写（--no-flash）。手动刷："
  echo "   $PY -m esptool --chip esp32s3 -p $PORT -b 921600 write_flash $ASSETS_OFFSET $IMG"
fi

echo "完成。产物目录：$OUT_DIR"
