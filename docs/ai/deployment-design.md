# MiniPet-ESP32 部署设计（NAS Docker · 群晖）

> 状态：设计稿（2026-09-25，胶水确认部署目标）
> 目标主机：**<NAS_IP> NAS（群晖）**，Docker = **Container Manager**，可出外网
> 参考实部署：NAS 上已有 hermes（镜像拉取+volume2 模式）、java（Mac 编译→SMB 投递 jar→容器跑）两套范例

---

## 一、部署形态（前后端分离 · 两容器）

```
<NAS_IP> (NAS /volume2/docker/minipet/)
├─ api          minipet-server（ASP.NET Core 9）
│                挂载：WZ数据(只读) + data/（配置/缓存/设备表/OTA）
│                出口：内部 8080
└─ qqmusic      Rain120/qq-music-api（Node，可选启用）
                 出口：内部 3300，仅 api 访问
（原设计的 nginx web 容器已砍——ASP.NET Core 直接托管 Vue 产物与 API，单容器单端口，无中间层）

访问路径（浏览器与 ESP32 同源同端口）：
  浏览器     http://<NAS_IP>:38090/            （Web UI = api 托管的 Vue 产物）
  ESP32      http://<NAS_IP>:38090/api/device/*（素材/指令/BGM 流）
  健康检查   http://<NAS_IP>:38090/api/health
```

**为什么砍掉 nginx**（胶水定稿）：ASP.NET Core 内置静态文件托管，Vue 构建产物直接打进 api 镜像 wwwroot——少一个容器、少一跳转发、BGM 流不再需要 proxy_buffering 调优。开发期跨域由 Vite proxy 解决，生产同源无 CORS。

---

## 二、配置体系（WZ 路径双通道——胶水定稿）

**同一份配置文件，两个编辑入口：**

| 入口 | 场景 | 说明 |
|---|---|---|
| **Web 设置页** | 开源后其他用户 | 表单：WZ Data 目录（容器内路径）、端口、QQ cookie、阈值等；保存即热重载（WzService 重新 LoadWz） |
| **文本直改** | 自己改方便 | 同一文件 `data/config/appsettings.json`（NAS 上直接编辑，api 监听变更热重载） |

要点：
- **Web 写的就是这个文件**——两个入口不会分叉，文本改完 Web 界面同步显示
- 默认值（我们自己的部署）：`"WzDataPath": "/wz/Data"`（容器内路径，见下方挂载）
- 开源用户按 README 改成自己的路径即可，**代码零硬编码路径**（延续桌面版铁律）
- QQ cookie / HA 令牌等敏感项同文件但独立节，.gitignore 已排除 secrets

---

## 三、docker-compose.yml（设计稿）

```yaml
version: '3.8'
services:
  api:
    image: ghcr.io/tzy502/minipet-server:latest   # 或 docker.io 镜像
    container_name: minipet-api
    restart: unless-stopped
    ports: ["8080:8080"]            # 仅调试期暴露，稳定后可去掉走内部网络
    volumes:
      - /volume2/homes/<user>/Backup/MS/客户端/冒险岛online/mxd:/wz:ro   # WZ 只读
      - /volume2/docker/minipet/data:/app/data                                # 配置/缓存/设备表/OTA
    environment:
      - TZ=Asia/Shanghai
      - MINIPET_WZ_PATH=/wz/Data        # 环境变量兜底（appsettings 可覆盖）
    deploy:
      resources:
        limits: { memory: 2048M, cpus: "2.0" }

  web:
    image: ghcr.io/tzy502/minipet-web:latest
    container_name: minipet-web
    restart: unless-stopped
    ports: ["${MINIPET_PORT:-38090}:80"]   # 五位数端口，.env 可改
    depends_on: [api]

  qqmusic:                             # 可选：注释掉即纯 WZ 曲库
    image: ghcr.io/tzy502/minipet-qqmusic:latest
    container_name: minipet-qqmusic
    restart: unless-stopped
    expose: ["3300"]
```

### 端口配置（胶水定稿：五位数 + 可修改）

- **默认端口 38090**（五位数，避开群晖常用低位端口段）
- 修改方式：编辑 `/volume2/docker/minipet/.env` 里 `MINIPET_PORT=xxxxx` → Container Manager 重建项目（容器端口映射属 Docker 层，改完 up -d 生效）
- api 容器直接对外暴露唯一五位数端口（内部 8080 → 映射 ${MINIPET_PORT:-38090}），**无 web 中间层**
- ESP32 配网页输入框预填 `http://<NAS_IP>:38090`，端口改了就在配网页/设备设置里改地址（协议里服务器地址本来就是配置项）
- 开源用户：README 写明改 .env 即可换端口

**群晖路径要点**（对齐 hermes 现有部署惯例）：
- 项目目录：`/volume2/docker/minipet/`（compose + data/）
- WZ 数据**不在 docker 共享里**，在 homes 共享——Container Manager 里 compose 挂 homes 路径需确认权限（hermes 未挂过 homes；首次部署验证清单第 ② 项）
- `:ro` 只读挂载保护 WZ 原始数据

---

## 四、镜像分发（两条路，主备）

**主路（GitHub Actions → GHCR）**
```
git push → Actions 构建 api/web/qqmusic 三镜像（多架构按需）→ ghcr.io/tzy502/*
NAS：Container Manager → 项目 → 拉取 compose → up -d
```
- NAS 可出外网 ✅（已确认）；GHCR 若慢，走 hermes 同款镜像加速（`docker.xuanyuan.run` 前缀）

**备路（Mac 直投，对齐 java 项目的 upload.sh 惯例）**
```
Mac: docker buildx build → docker save tar → cp 到 ~/nas-out/minipet/images/
NAS: docker load < tar && compose up -d
```
- 不依赖 NAS 出网与 CI，应急/调试用；脚本放 `scripts/deploy-nas.sh`

---

## 五、数据与持久化

| 数据 | 位置（NAS） | 备份策略 |
|---|---|---|
| 配置 appsettings.json | `/volume2/docker/minipet/data/` | 随 data 目录快照 |
| 素材导出缓存（部件包/布局/缩略图） | `data/cache/` | 可重建（删了重导） |
| 设备表 / 曲库配置 / 最近使用记录 | `data/`（json 文件，不引入数据库） | 同上 |
| OTA 固件包 | `data/firmware/` | 版本化保留 |
| QQ cookie | `data/config/`（600 权限） | 不入 git |
| WZ 原始数据 | homes 共享（只读挂载） | 已是备份本体 |

---

## 六、开发 ↔ 部署环境对照

| 环境 | api | web | WZ 数据 |
|---|---|---|---|
| 本地开发 | `dotnet run`（:8080） | `npm run dev`（:5173, proxy /api） | /Volumes/SSD/mxd（Mac 直读，快） |
| NAS 生产 | 容器 8080（内部） | 容器 80（外部 8090） | /volume2/homes/...（容器内 /wz，本地盘，快） |

- **不复制 WZ 数据进镜像**（7.9GB+ 且涉及版权）；镜像 ≤200MB
- 开发期不强制走 Docker（Mac 直跑更快），compose 保证「同一份代码两种跑法」

---

## 七、首次部署验证清单（硬件/环境就绪后执行）

1. ✅ NAS 出外网（已确认）
2. ⬜ **SkiaSharp 在群晖容器内可运行**（cpu 架构先确认：`uname -m` 或 Container Manager 关于页——arm64 与 amd64 的 native 库都要备）
   - fallback：渲染层换 ImageSharp（纯托管，慢 ~30% 但零 native 风险）
3. ⬜ compose 挂载 homes 路径权限（群晖对 homes 共享的容器访问 ACL）
4. ⬜ .NET 9 runtime 在该架构的镜像可用性
5. ⬜ ESP32 配网页默认值预填 `http://<NAS_IP>:38090`（端口可改，配网页是输入框不是写死）
6. ⬜ BGM 流式直出（api 容器自身响应，无 nginx 中间层，确认无缓冲即可）

---

## 八、开源准备（Web 配置化带来的）

- README（开源版）：三步走「装 WZ 数据 → 起 compose → 浏览器配置路径」
- 配置项全部有默认值 + Web 表单校验（路径存在性/读写权限预检）
- 不含任何胶水私人路径/IP 的默认值——**我们的默认值走环境变量注入，开源用户改 .env 即可**
