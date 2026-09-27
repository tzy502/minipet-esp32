#!/usr/bin/env bash
# =====================================================================
# Dockerfile [assert-wwwroot] 构建期断言的本地可复现校验（本机无 docker 时的替代验证）
#
# 做法：用 awk 把 Dockerfile 里 [assert-wwwroot] 标记后的 RUN 块**原文抽出来**
#       （不是手抄一份），在临时目录里模拟三种层序后执行同一段断言：
#   ① 修复后层序（publish 已剔除 wwwroot → rm -rf wwwroot → cp 本次 dist）  → 期望通过
#   ② 旧层序（先 COPY dist，再 COPY publish 目录合并回旧 wwwroot —— 线上跑旧 UI 的根因）→ 期望被拦截
#   ③ 占位首页路径（Web/ 无 package.json，index.html 无 assets/*.js 引用）    → 期望通过（不断言 JS）
# 另外打印真实仓库产物对照：Web/dist（本次构建）vs Server/MinipetServer/wwwroot（旧残留）。
#
# 依赖：sh/grep/find/sort/cat/awk（Linux 与 macOS 都有）。sha256sum 是 GNU coreutils 的，
#       macOS 只有 shasum → 本脚本自动放一个 shasum 包装（输出格式与 sha256sum 一致：
#       "<64hex>  <path>"），保证抽取出来的断言片段在两侧行为一致。
# 用法：bash Server/tools/check-wwwroot-assert.sh
# =====================================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DOCKERFILE="$REPO_ROOT/Dockerfile"
REAL_DIST="$REPO_ROOT/Web/dist"
REAL_STALE="$REPO_ROOT/Server/MinipetServer/wwwroot"

SIM="$(mktemp -d)"
trap 'rm -rf "$SIM"; rm -f /tmp/dist.sha256 /tmp/wwwroot.sha256' EXIT

# ── sha256sum 兼容层（macOS 只有 shasum -a 256）────────────────────────
if ! command -v sha256sum >/dev/null 2>&1; then
    mkdir -p "$SIM/bin"
    printf '#!/bin/sh\nexec shasum -a 256 "$@"\n' > "$SIM/bin/sha256sum"
    chmod +x "$SIM/bin/sha256sum"
    PATH="$SIM/bin:$PATH"
    export PATH
    echo "[i] 本机无 sha256sum，已用 shasum -a 256 包装替代（输出格式相同）"
fi

# ── 抽取 Dockerfile 里的断言片段（原文，去掉行首 "RUN "）───────────────
FRAGMENT="$SIM/assert-fragment.sh"
awk '
  /\[assert-wwwroot\]/      { flag = 1; next }
  flag && /^#/              { next }
  flag && /^$/              { exit }
  flag                      { if (!done) { sub(/^RUN /, ""); done = 1 } print }
' "$DOCKERFILE" > "$FRAGMENT"

if ! grep -q 'set -eux' "$FRAGMENT"; then
    echo "FAIL：没能从 Dockerfile 抽出 [assert-wwwroot] 断言块"; exit 1
fi
echo "[i] 已从 Dockerfile 抽取断言片段 $(wc -l < "$FRAGMENT" | tr -d ' ') 行（含续行）"
echo

# ── 工具：造一份"本次前端构建产物"（合成）────────────────────────────
make_dist() { # $1=目录 $2=js hash
    mkdir -p "$1/assets"
    cat > "$1/index.html" <<EOF
<!DOCTYPE html><html><head>
<script type="module" crossorigin src="/assets/index-$2.js"></script>
<link rel="stylesheet" crossorigin href="/assets/index-$2.css">
</head><body><div id="app"></div></body></html>
EOF
    echo "console.log('$2')" > "$1/assets/index-$2.js"
    echo ".a{color:#000}"    > "$1/assets/index-$2.css"
}

fingerprint() { # $1=目录 → 与 Dockerfile webbuild 阶段同一口径写入 /tmp/dist.sha256
    ( cd "$1" && for f in $(find . -type f | sort); do printf '%s %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"; done ) > /tmp/dist.sha256
}

run_assert() { # $1=模拟的 /app 目录；返回断言退出码
    ( cd "$1" && sh "$FRAGMENT" ) > "$SIM/out.txt" 2>&1
}

PASS=0; FAIL=0
verdict() { # $1=期望(0/非0) $2=实际 $3=标题
    if { [ "$1" = "0" ] && [ "$2" = "0" ]; } || { [ "$1" != "0" ] && [ "$2" != "0" ]; }; then
        echo "  ✔ PASS：$3（断言退出码 $2，符合预期）"; PASS=$((PASS + 1))
    else
        echo "  ✘ FAIL：$3（断言退出码 $2，期望 $1 语义）"; FAIL=$((FAIL + 1))
        sed 's/^/      | /' "$SIM/out.txt" | tail -12
    fi
}

# ── 场景①：修复后层序（真实 Web/dist 作为本次构建产物）──────────────
echo "── 场景① 修复后层序（publish 无 wwwroot → 清空重建 → 本次 dist）"
APP="$SIM/app1"; mkdir -p "$APP"
cp -a "$REAL_DIST" "$APP/wwwroot"                 # 等价 COPY --from=webbuild /src/dist ./wwwroot
rm -rf "$APP/wwwroot"                             # 等价 RUN rm -rf ./wwwroot（清掉 publish 残留）
cp -a "$REAL_DIST" "$APP/wwwroot"
fingerprint "$REAL_DIST"
run_assert "$APP"; rc=$?
grep -a '断言通过' "$SIM/out.txt" | sed 's/^/      | /'
verdict 0 "$rc" "修复后层序通过断言（真实 Web/dist）"

# ── 场景②：旧层序（本次 dist 先落，再被 publish 带回来的旧 wwwroot 目录合并）──
echo "── 场景② 旧层序（先 dist 后 publish 目录合并 = 旧 UI 覆盖新 UI）"
APP="$SIM/app2"; mkdir -p "$APP"
cp -a "$REAL_DIST" "$APP/wwwroot"                 # 旧 Dockerfile:86
cp -a "$REAL_STALE/." "$APP/wwwroot/"             # 旧 Dockerfile:89（目录合并语义，旧 index.html 覆盖）
fingerprint "$REAL_DIST"
run_assert "$APP"; rc=$?
echo "      | wwwroot/index.html 现在引用：$(grep -oE 'assets/[A-Za-z0-9._-]+\.js' "$APP/wwwroot/index.html" | head -n1)"
sed 's/^/      | /' "$SIM/out.txt" | tail -6
verdict 1 "$rc" "旧层序被断言拦截（线上跑旧 Web UI 的根因）"

# ── 场景③：占位首页路径（Web/ 无 package.json 时不该因断言而构建失败）──
echo "── 场景③ 占位首页路径（index.html 无 assets/*.js 引用）"
APP="$SIM/app3"; DIST="$SIM/placeholder"; mkdir -p "$APP"
printf '<!doctype html><meta charset="utf-8"><title>MiniPet</title><p>Web UI not built yet.' > "$SIM/placeholder-index"
mkdir -p "$DIST"; cp "$SIM/placeholder-index" "$DIST/index.html"
cp -a "$DIST" "$APP/wwwroot"
fingerprint "$DIST"
run_assert "$APP"; rc=$?
grep -a '警告' "$SIM/out.txt" | sed 's/^/      | /'
verdict 0 "$rc" "占位首页路径不误伤（仅警告，不做 JS 断言）"

# ── 真实产物对照（信息输出，不参与判定）─────────────────────────────
echo "── 真实仓库产物对照（信息）"
echo "      Web/dist（本次前端构建）     index.html → $(grep -oE 'assets/index-[A-Za-z0-9_-]+\.js' "$REAL_DIST/index.html" | head -n1)  [$(ls -l "$REAL_DIST/index.html" | awk '{print $6, $7, $8}')]"
if [ -f "$REAL_STALE/index.html" ]; then
    echo "      Server/…/wwwroot（旧残留）   index.html → $(grep -oE 'assets/index-[A-Za-z0-9_-]+\.js' "$REAL_STALE/index.html" | head -n1)  [$(ls -l "$REAL_STALE/index.html" | awk '{print $6, $7, $8}')]"
    echo "      旧残留 assets 文件数=$(find "$REAL_STALE" -type f | wc -l | tr -d ' ')（多代叠加）  vs  本次 dist 文件数=$(find "$REAL_DIST" -type f | wc -l | tr -d ' ')"
else
    echo "      Server/…/wwwroot 不存在（干净检出的 CI 场景）→ 旧层序不会触发，但断言对本地构建仍生效"
fi
echo "      源码树旧 wwwroot 是否在本次 dist 指纹里：$(grep -qF "$(grep -oE 'assets/index-[A-Za-z0-9_-]+\.js' "$REAL_STALE/index.html" 2>/dev/null | head -n1)" /tmp/dist.sha256 2>/dev/null && echo '是（异常）' || echo '否（正常：本次 dist 不含旧代文件）')"

echo
echo "===== 汇总：PASS=$PASS FAIL=$FAIL ====="
[ "$FAIL" = "0" ] || exit 1
