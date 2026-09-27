# MiniPet-ESP32

> 基于 ESP32 的冒险岛桌宠 —— 硬件分支线（F19）
> 桌面版仓库：mapleStoryMiniPet（C# + Avalonia，独立维护）

## 项目结构

```
minipet-esp32/
├── Server/       # PC 服务端（C# / .NET）：复用桌面版 Services 核心（WZ解析/纸娃娃/地图烘焙/BGM）
├── Web/          # 配置前端（Vue 3 + Naive UI）：设置中心/素材浏览/纸娃娃编辑/曲库/设备管理
├── Firmware/     # ESP32-S3 固件（ESP-IDF + LVGL）：哑终端，只做渲染/播放/事件上报
└── docs/         # 文档
    └── ai/       # 决策文档、算法规格
```

## 架构一句话

**NAS Docker 单容器部署**（群晖）：ASP.NET Core 一体化容器（托管 Vue 前端 + 设备 API + BGM 流，对外唯一五位数端口 38090，.env 可改；QQ 音乐网关为容器内可选 node 子进程，最终镜像含 node 运行时 ≤400MB）；ESP32 哑终端（零 WZ / 零 Hermes / 零 HA，只收素材包和指令，设备永远是 HTTP client）。
配置双通道：Web 设置页（开源用户）与文本 appsettings.json（自己改）写同一份文件。

## 三步部署（开源用户）

前置：一台能跑 Docker 的机器（NAS / PC / 树莓派均可）、现有冒险岛 WZ 数据目录、一块刷好固件的 ESP32-S3-Touch-AMOLED-2.16。

```bash
# ① 放 WZ：把 WZ 数据目录准备好（只读挂载进容器，例：/volume1/wz）
#    数据一切从 WZ 走 —— 服务端不内置任何素材。

# ② 起服务：
cp docker-compose.example.yml docker-compose.yml
cp .env.example .env
#   改 compose 两处占位符：镜像名 ghcr.io/<owner>/minipet 与 WZ 宿主路径
#   改 .env：MINIPET_PORT（默认 38090）
docker compose up -d

# ③ 浏览器配置：打开 http://<服务器IP>:38090
#   「设置」页填 WZ 路径（有存在性校验）→ 保存即热重载
#   首页会出现设备卡片；设备首次开机 hello 入册后，屏显 6 位配对码 → 在此输入完成绑定
```

固件烧录（开发者）：

```bash
source ~/esp/esp-idf/export.sh          # IDF v5.5
cd Firmware && idf.py build             # 必须 0 error
idf.py -p /dev/cu.usbmodemXXXX flash    # 板子串口按实际改
```

设备首次上电无 WiFi 配置 → 开热点 `MiniPet-XXXX` → 手机连上自动弹配网页 →
填家里 WiFi（密码别多输一位）+ 服务器地址 `http://<服务器IP>:38090`。
换网络时无需连电脑：菜单 → `Reset WiFi` → 确认（清凭据并重启，重新配网）。

## 硬件

- 桌面主力：Waveshare ESP32-S3-Touch-AMOLED-2.16（480×480 AMOLED / 8MB PSRAM / 16MB Flash）
- 冰箱贴（二期）：ESP32-S3-Touch-LCD-1.28-B（240×240 圆屏）

## 文档索引

| 文档 | 内容 |
|---|---|
| [docs/ai/README.md](docs/ai/README.md) | F19 分支线全部决策（架构/硬件/渲染/表情/BGM/不做清单） |
| [docs/ai/clock-display-spec.md](docs/ai/clock-display-spec.md) | WZ 地图时钟显示规格（已定稿） |
| [docs/ai/waveshare-wiki-ESP32-S3-Touch-AMOLED-2.16.md](docs/ai/waveshare-wiki-ESP32-S3-Touch-AMOLED-2.16.md) | 微雪官方 wiki 全量（GPIO 引脚表/外设速查） |
| [docs/ai/deployment-design.md](docs/ai/deployment-design.md) | NAS Docker 部署设计（单容器定稿，文内含历史三容器稿） |

## 开发状态

**Phase 1 已完成**（E1–E14 需求分析 + 设计评审，见 `docs/ai/requirements-analysis.md`）；
服务端 / Web / 固件三条线均有实现，当前处于**真机联调收尾**阶段：

| 线 | 状态 |
|---|---|
| 服务端（Server/） | 核心服务迁入 + 导出器 + 8 个设备端点 + 管理端点（素材推送/字体/指令/OTA） |
| Web（Web/） | 设备卡片 / 素材浏览器 / 纸娃娃编辑器 / 曲库 / 设置 / 表情调试 / BGM 控制 |
| 固件（Firmware/） | 渲染合成 + 触摸/IMU 交互 + 菜单选择器 + BGM 解码 + OTA 双分区回滚 |

真机联调遗留问题与逐条证据见 [docs/ai/selftest-report-2026-09-27.md](docs/ai/selftest-report-2026-09-27.md)
与 [docs/ai/keys-touch-handoff.md](docs/ai/keys-touch-handoff.md)。
