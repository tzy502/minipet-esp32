# MiniPet-ESP32 需求分析

> 状态：**Phase 1 进行中**（2026-09-25 开始，逐条确认）
> 编号：**E 系列**（分支线独立编号，与桌面版 F 系列互不干扰）
> 流程：每条功能一句话 + 核心要求，胶水逐条确认；确认后追加到本文档

---

## 新增需求 v1.0

### E1 — 服务端核心与素材导出（P0）✅ 已确认（2026-09-25）

把桌面版核心服务层（WZ 解析 / 纸娃娃合成 / 地图烘焙 / 气泡 / 曲库，共 10.4K 行）迁入本仓库 Server/，新增「布局导出器」：把纸娃娃部件导出为 RGB565 部件图 + origin，把每动作每帧导出为布局表（帧 × 部件 × x,y,z + expression 维度），把地图导出为静态背景双层烘焙（static_back + tile_layer）+ 视差条带元数据（偏移=f(time)）。

- 素材格式服务于设备端方案 C（部件图下发 + 设备端合成）
- 数据一切从 WZ 走，路径经配置注入（禁止硬编码）
- 表情按 25 个实证表情全量导出（expression 维度）
- 480 屏部件 1x + scale 2x nearest 由导出器标注
- 迁移时清理 17 处 Avalonia Dispatcher 引用（改普通回调/Channel）

### E2 — 设备协议（P0）✅ 已确认（2026-09-25）

服务端与 ESP32 之间的 HTTP 协议（设备永远是 client）。核心端点：

```
设备上线与素材
  POST /api/device/hello        设备注册：profile(w,h,shape,psram,audio) + UUID + 固件版本 → 返回 deviceId 与配置
  GET  /api/device/manifest     素材版本表（部件/布局/背景/字体/BGM 清单 + hash），设备 diff 后按需拉
  GET  /api/device/asset/{hash} 单个素材包（部件图/布局表/背景烘焙图）

指令与事件（长轮询）
  GET  /api/device/poll?since=  拉指令队列（切动作/表情/气泡文本/BGM 控制/亮度/重启），纯桌宠指令
  POST /api/device/event        设备上报事件（触摸/IMU 力度分级/倾斜角度/低电/错误）

BGM
  GET  /api/device/bgm/stream   MP3 流（服务端实时取链转发，设备只见此 URL）
  POST /api/device/bgm/cmd      播放/暂停/切歌/音量（设备端现场控制的回传）
```

- 协议里 **entities[]** 数组结构（首版 N=1，未来同屏多宠不改协议）
- **无 Hermes 字段、无 HA 字段**（隔离铁律）
- 无网络降级：所有端点不可达时设备用 TF 卡缓存独立运行
- 协议版本号字段预留（`proto`），v2 加东西不破坏 v1 设备

### E3 — 服务端部署与配置（P0）✅ 已确认（2026-09-25）

- **单容器**：api（ASP.NET Core 直接托管 Vue 产物 + API + BGM 流）；QQ 音乐网关 = 容器内 node 子进程（配置开关控制拉起，不启用则不存在）
- **对外唯一五位数端口 38090**（`.env` 的 `MINIPET_PORT` 可改），无 web 中间层
- **配置双通道**：Web 设置页与文本编辑写同一份 `data/config/appsettings.json`（WZ 路径 / 端口 / QQ cookie / 阈值），改后热重载（WzService 重新 LoadWz）
- **WZ 数据只读挂载**（NAS homes → 容器 `/wz`），开源用户路径自定义；仓库内禁止出现真实 IP/用户名（占位符）
- **镜像分发**：GitHub Actions 构建单镜像 → GHCR；备路 `scripts/deploy-nas.sh`（Mac buildx → save/load 直投 NAS）
- 验证清单随代码交付：SkiaSharp 容器内可用（fallback ImageSharp）、homes 挂载权限、双架构镜像

### E4 — Web 配置前端（P0）✅ 已确认（2026-09-25）

Vue 3 + Naive UI 管理界面（构建产物打进 api 镜像，单容器同源）：

- **设备卡片列表**（首页）：在线状态 / 当前宠物缩略图 / 换宠换装入口 / 配对码入册
- **素材浏览器**：地图 / 纸娃娃部件 / 怪物 NPC 浏览（服务端渲染缩略图）
- **纸娃娃编辑器**：换装组合 → 存为预设（供设备选择器「纸娃娃 tab」用）
- **曲库管理**：WZ 曲库浏览 + QQ 音源卡片（cookie 导入 / 有效期告警 / 启停开关）
- **设置页**：WZ 路径（含存在性校验）/ 端口 / 各阈值（IMU 灵敏度 / 死区 / 力度分级）——写 appsettings.json
- **表情入口**：25 表情手动指定（调试 + 演示用）
- 地图选择含**收藏**（喂给设备选择器的「最近+收藏」）

不做：设备端搜索输入、Web 点歌按钮（BGM 控制在设备上，已定稿）

### E5 — 设备端渲染与动画（P0）✅ 已确认（2026-09-25）

固件核心渲染能力（方案 C：部件图下发 + 设备端合成）：

- **部件合成**：按布局表（帧 × 部件 × x,y,z）在动作帧切换时合成一次，平时只 blit 合成结果；表情切换 = 只换 face 类部件重合成
- **动画时序**：按布局表 delay 序列推帧；动作播完回 stand（回退链）
- **缩放**：480 屏 1x 部件 + scale 2x nearest（像素风无损）
- **背景**：三层绘制——static_back → 视差条带（偏移=f(time) 或 IMU 倾角）→ tile_layer → 宠物 → 气泡
- **脏区上传**：只把变化区域推 QSPI（宠物区 30fps 局部刷新 ≤31% 带宽）
- **blink 本地循环**：随机 3~8s 播 blink，断网也眨眼（生命感地基）
- **动画铁律**：只播服务端下发的布局表（源头是 WZ 真实 action），固件不自创任何动画
