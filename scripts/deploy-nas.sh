#!/usr/bin/env bash
# =====================================================================
# MiniPet-ESP32 NAS 直投部署（备路，E3 定稿——不依赖 NAS 出网与 CI）
#
# 流程：Mac 本地 buildx --load → docker save → scp → NAS docker load
#       → docker compose up -d
# 前提：本机有 Docker（含 buildx）；NAS 上已按 docker-compose.example.yml
#       准备好项目目录（docker-compose.yml + .env）。
# 主路（GHCR，推荐）见同目录 README.md。
# =====================================================================
set -euo pipefail

# ==================== 部署配置（改这里） ====================
NAS_HOST="<NAS_IP>"                  # NAS 的 SSH 地址（IP 或 ~/.ssh/config 里的别名）
NAS_DIR="/volume2/docker/minipet"    # NAS 上 compose 项目目录（docker-compose.yml 所在）
IMAGE="ghcr.io/<owner>/minipet"      # 镜像名，必须与 NAS 上 compose 里的 image: 一致
TAG="${TAG:-latest}"                 # 可用环境变量覆盖，如：TAG=dev ./scripts/deploy-nas.sh
PLATFORM="${PLATFORM:-linux/amd64}"  # NAS CPU 架构：x86_64→linux/amd64，aarch64→linux/arm64
                                    # （ssh 到 NAS 执行 uname -m 查看）
# ============================================================

# ---- 占位符守卫：防止没改配置就跑 ----
for v in NAS_HOST IMAGE NAS_DIR; do
  # shellcheck disable=SC2223
  val="${!v}"
  if [[ "$val" == *"<"* ]]; then
    echo "错误：变量 ${v} 仍是 <...> 占位符（当前值：${val}）。请先编辑本脚本顶部的部署配置。" >&2
    exit 1
  fi
done
if ! command -v docker >/dev/null 2>&1; then
  echo "错误：本机未安装 Docker（需要 buildx 与 docker save）。" >&2
  exit 1
fi

TAR_NAME="minipet-${TAG}.tar"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "==> [1/5] 本地构建镜像 ${IMAGE}:${TAG}（${PLATFORM}）"
docker buildx build \
  --platform "$PLATFORM" \
  --tag "${IMAGE}:${TAG}" \
  --load \
  .

echo "==> [2/5] docker save"
docker save -o "${TMP_DIR}/${TAR_NAME}" "${IMAGE}:${TAG}"

echo "==> [3/5] 传到 ${NAS_HOST}:${NAS_DIR}/images/"
ssh "$NAS_HOST" "mkdir -p '${NAS_DIR}/images'"
if ! ssh "$NAS_HOST" "test -f '${NAS_DIR}/docker-compose.yml'"; then
  echo "错误：${NAS_DIR}/docker-compose.yml 不存在。请先按 docker-compose.example.yml 准备 NAS 项目目录（含 .env）。" >&2
  exit 1
fi
scp "${TMP_DIR}/${TAR_NAME}" "${NAS_HOST}:${NAS_DIR}/images/"

echo "==> [4/5] NAS 端 docker load"
ssh "$NAS_HOST" "docker load -i '${NAS_DIR}/images/${TAR_NAME}'"

echo "==> [5/5] NAS 端 compose up -d"
ssh "$NAS_HOST" "cd '${NAS_DIR}' && docker compose up -d"

echo
echo "部署完成。验证（端口取 NAS 上 .env 的 MINIPET_PORT，默认 38090）："
echo "  curl http://${NAS_HOST}:38090/api/health"
