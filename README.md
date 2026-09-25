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

## 硬件

- 桌面主力：Waveshare ESP32-S3-Touch-AMOLED-2.16（480×480 AMOLED / 8MB PSRAM / 16MB Flash）
- 冰箱贴（二期）：ESP32-S3-Touch-LCD-1.28-B（240×240 圆屏）

## 文档索引

| 文档 | 内容 |
|---|---|
| [docs/ai/README.md](docs/ai/README.md) | F19 分支线全部决策（架构/硬件/渲染/表情/BGM/不做清单） |
| [docs/ai/clock-display-spec.md](docs/ai/clock-display-spec.md) | WZ 地图时钟显示规格（已定稿） |
| [docs/ai/waveshare-wiki-ESP32-S3-Touch-AMOLED-2.16.md](docs/ai/waveshare-wiki-ESP32-S3-Touch-AMOLED-2.16.md) | 微雪官方 wiki 全量（GPIO 引脚表/外设速查） |
| [docs/ai/deployment-design.md](docs/ai/deployment-design.md) | NAS Docker 部署设计（三容器/compose/镜像分发/验证清单） |

## 开发状态

**Phase 0**：仓库初始化（2026-09-25）。尚未进入 Phase 1 需求分析。

流程约定：严格瀑布（需求分析 → 软件设计 → 开发），Agent 只分析/规划/验证，实现交 Claude Code。
