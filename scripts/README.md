# MiniPet 部署脚本说明（NAS · 群晖 Container Manager）

单容器形态（E3 定稿）：api（ASP.NET Core 9）直接托管 Web UI + API + BGM 流，
QQ 网关是容器内 node 子进程。WZ 数据不进镜像，部署时只读挂载 `/wz`。

仓库与镜像里**不含任何真实 IP / 用户名 / 私有路径**，部署时替换占位符：
`<NAS_IP>`、`<owner>`、`<WZ_DATA_HOST_PATH>`。

## 一次性准备（NAS 上）

在 NAS 建项目目录（示例 `/volume2/docker/minipet/`，按需修改）：

```
/volume2/docker/minipet/
├── docker-compose.yml   ← 由 docker-compose.example.yml 复制，替换两处占位符
├── .env                 ← 由 .env.example 复制（MINIPET_PORT）
├── images/              ← 备路 tar 存放处（脚本自动 mkdir）
└── data/                ← 配置/缓存/设备表/OTA（首次 up 自动创建亦可）
```

compose 里 `image:` 的镜像名必须与实际来源一致（见下面两路）。

## 主路（推荐）：GitHub Actions → GHCR

1. `git push` 到 `main` → CI（`.github/workflows/ci.yml` 的 image job）自动
   构建 **linux/amd64 + linux/arm64** 并推送到 GHCR：
   `ghcr.io/<owner>/<仓库>:latest`（镜像名取自仓库名，CI 里是动态的
   `${{ github.repository }}`，不写死用户名）。
2. NAS 上：`docker compose pull && docker compose up -d`
   （compose 的 `image:` 写成与 CI 产物一致的完整镜像名）。

GHCR 拉取慢可在镜像名前加自备的加速前缀。CI 另含 server/web 构建测试与
smoke 冒烟（起容器重试 curl `/api/health`），镜像推送前先过这三关。

## 备路：Mac 直投（`deploy-nas.sh`，不依赖 NAS 出网与 CI）

编辑 `scripts/deploy-nas.sh` 顶部 4 个变量后执行：

```bash
./scripts/deploy-nas.sh
```

| 变量 | 说明 |
|---|---|
| `NAS_HOST` | NAS 的 SSH 地址（IP 或 ssh config 别名），默认占位 `<NAS_IP>` |
| `NAS_DIR` | NAS 上 compose 项目目录 |
| `IMAGE` | 镜像名，须与 NAS compose 里 `image:` 一致 |
| `PLATFORM` | NAS CPU：`x86_64` → `linux/amd64`，`aarch64` → `linux/arm64`（`uname -m` 查看）；也可用环境变量 `TAG=dev PLATFORM=linux/arm64 ./scripts/deploy-nas.sh` 覆盖 |

流程：buildx `--load` → `docker save` tar → `scp` → NAS `docker load` →
`docker compose up -d`。应急 / 调试用。

## 改端口

编辑 NAS 项目目录的 `.env`：`MINIPET_PORT=xxxxx`，然后
`docker compose up -d` 重建（端口属部署层，只在 .env；Web 设置页只读展示）。
ESP32 配网页的预填地址同步改为 `http://<NAS_IP>:xxxxx`。

## 部署后验证

```bash
curl http://<NAS_IP>:38090/api/health     # 健康检查
# 浏览器打开 http://<NAS_IP>:38090/       # 应见 Web UI（设备卡片）
```
