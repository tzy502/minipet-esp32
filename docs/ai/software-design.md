# MiniPet-ESP32 软件设计

> 状态：**Phase 2 设计稿 v1**（2026-09-25）
> 依据：requirements-analysis.md（E1-E14）+ algorithm-asset-format.md（素材包格式）
> C# 实现细节由 Agent 定稿；胶水将开独立 agent 对抗验证
> 评审：完成后过 3 agent 并行评审

---

## 一、总体结构（三模块一仓库）

```
minipet-esp32/
├── Server/                    # .NET 9 解决方案（ASP.NET Core 单容器）
│   ├── MinipetServer/         # 主项目：API + 静态托管 + 导出器宿主 + QQ网关管理
│   ├── MinipetServer.Tests/   # 单元测试（服务层迁移后跑通既有测试的移植版）
│   └── tools/Exporter/        # 导出 CLI（可独立跑，CI/手动）
├── Web/                       # Vue 3 + Naive UI + Vite
└── Firmware/                  # ESP-IDF 5.5 + LVGL 9
    ├── main/
    │   ├── app/               # 状态机与业务（poll/事件/降级）
    │   ├── render/            # 合成器、脏区、条带
    │   ├── net/               # http client、长轮询、ota
    │   ├── audio/             # minimp3 + i2s + PA_CTRL
    │   └── drivers/           # bsp 封装（屏/触摸/IMU/RTC/TF/电源）
    └── profiles/              # amoled216/（二期 fridge128/）
```

## 二、Server 设计

### 2.1 服务层迁移（E1）

| 桌面版类 | 去向 | 改造点 |
|---|---|---|
| WzService / MapCatalogService / WzError | 原样迁入 `MinipetServer/Services/` | 无（零 UI 依赖） |
| PaperdollService / SpriteService | 原样迁入 | 无 |
| MapService + MapService.Render | 原样迁入 | 删 1 处 `Dispatcher` 封送 → 回调 |
| BalloonService | 迁入 | 参数供导出器用（设备端渲染文本，不迁渲染部分） |
| MusicCatalogService / MusicPlayerService / MusicDecisions | 迁入 | `IMusicPlayer` 换成「流式转发器」实现（不再本地播） |
| BassMusicPlayer | **不迁** | 设备播；服务端只转发流 |
| CacheManager | 重写薄版 | 桌面版管磁盘 PNG 缓存 → 新版管素材包缓存（hash→文件） |
| ConfigService | 重写 | appsettings.json 读写 + 热重载 + Web 校验端点 |
| AnimService / EventBus / PetManager | 部分迁 | AnimService 时序规则进导出器；PetManager 概念并入 DeviceRegistry |
| TrayService / EffectLayerService / Adapters / AgentState* / Walk* | **不迁** | 桌面专属 |
| PngEncoder (Utils) | 原样迁入 | 导出器核心依赖 |

迁移验收：`MinipetServer.Tests` 跑通 WZ 加载 / 纸娃娃合成 / 地图渲染三个移植测试（黑盒：真实 WZ 数据进出图）。

### 2.2 新增服务

```
AssetExporter      (E1)  按 algorithm-asset-format.md 产出五类包；输入=装扮JSON+设备profile
DeviceRegistry     (E13) 设备表 CRUD（JSON 文件，读写锁）；配对码生命周期（6位,10分钟过期）
CommandQueue       (E2)  每设备指令队列（内存 + 持久化兜底），poll 按序取走
BgmRouter          (E8)  IMusicSource(WZ|QQ) → 统一流式响应；同源降级状态机；failover 事件
QqGatewayProcess   (E3)  node 子进程生命周期（配置开关拉起/停止/健康检查/自动重启1次）
ConfigWatcher      (E3)  appsettings.json 变更 → 热重载事件（WZ 重载/阈值下发）
HealthReport       (E11) 设备上报事件聚合 → Web 健康 API
```

### 2.3 API 面（E2 落地）

设备端（前缀 /api/device）：hello / manifest / asset/{hash} / poll / event / bgm/stream / bgm/cmd / firmware/{ver}.bin
Web 端（前缀 /api/admin）：设备列表与配置 / 素材浏览与缩略图 / 纸娃娃预设 / 曲库 / 音源（cookie 导入+健康）/ 设置读写（含 WZ 路径存在性校验）/ 日志拉取 / OTA 触发
认证：局域网信任模型（v1 无鉴权，预留 API key 字段）；开源 README 注明「勿暴露公网」

### 2.4 配置模型（E3）

`data/config/appsettings.json` 单一来源，双通道写：
```json
{
  "Server":   { "Port": 38090 },
  "Wz":       { "DataPath": "/wz/Data" },
  "QqMusic":  { "Enabled": false, "Cookie": "", "GatewayPort": 3300 },
  "Bgm":      { "DefaultSource": "wz" },
  "Device":   { "ImuDeadzoneDeg": 8, "TapLightG": 2.0, "TapHardG": 4.0, "IdleToClockMin": 5 },
  "Clock":    { "MapOffsets": { "200000100": [123,240] } }
}
```
- Web 保存 = 序列化回写（保留注释的策略：改用 JSON5 读写或注释字段 `_comment`，实现期定）
- 热重载：FileSystemWatcher → ConfigWatcher → 受影响服务 reload（WZ 重载有锁，桌面版模式复用）

### 2.5 部署（E3）

- Dockerfile（多阶段：node 构建 Web → sdk 构建 Server → runtime 最终层，单镜像）
- `.env`：`MINIPET_PORT`；compose 挂载 WZ（:ro）+ data/
- CI：GitHub Actions → GHCR 单镜像（linux/arm64+amd64）；`scripts/deploy-nas.sh`（buildx save/load 直投）
- 验证清单落在 CI 冒烟 job：容器内跑 Tests（SkiaSharp 可用性即被覆盖，失败自动提示 fallback ImageSharp 分支）

## 三、Web 设计（E4）

```
路由（Vue Router）：
/               设备卡片列表（轮询在线态）
/materials      素材浏览器（tab: 地图/纸娃娃部件/怪物NPC；缩略图懒加载）
/paperdoll      纸娃娃编辑器（部件选择 → 服务端合成预览 → 存预设）
/music          曲库（WZ 浏览 + QQ 音源卡片）
/settings       设置（分组表单 ↔ appsettings.json）
/device/:id     单设备详情（换宠换装/阈值/固件）
```
- 组件库 Naive UI；状态 Pinia；请求 Axios（统一 /api 前缀，同源零配置）
- 缩略图 = 服务端 `/api/admin/thumb?type=map&id=...`（SkiaSharp 渲染 64×64，磁盘缓存）
- 构建产物进 Server wwwroot（CI 同步，开发期 Vite proxy → localhost:8080）

## 四、Firmware 设计

### 4.1 任务架构（双核）

```
PRO_CPU(0)：网络与后台
  http_poll 任务（长轮询+指数退避）
  asset_dl 任务（manifest diff → 逐包下载 → TF 落盘，优先级低于 poll）
  ota 任务
  bgm 任务（HTTP 流 → minimp3 解码 → 环形缓冲）
APP_CPU(1)：渲染与交互
  lvgl 任务（timer 驱动：合成器/脏区/上传 QSPI）
  input 任务（触摸/按键/IMU 采样+力度分级算法）
  看门狗喂狗点（渲染心跳；3 次熔断逻辑在 app/，NVS 计数）
```
- 队列：event_q（input→net 上报）、cmd_q（net→render 执行指令）、audio_q（bgm→i2s DMA）
- IMU 双中断（GPIO17/21）唤醒采样，非轮询

### 4.2 渲染管线（E5）

```
帧循环（30fps 定时器）：
  1) 到 delay？→ 合成器重画实体层（PARTS+LAYOUT，含表情件替换）到实体缓冲
  2) 条带偏移 = f(time) 或 IMU 倾角 → 移动条带
  3) 合成最终帧区域 = static_back(不动，不重绘) + tile_layer + 实体缓冲 + 气泡label
  4) 脏区计算（与上帧 diff 的包围盒）→ QSPI 上传
blink：本地定时器插播（断网可用）
```
- 内存布局（PSRAM）：static_back 450KB + tile_layer 450KB + 实体缓冲 200×260×2≈100KB + 条带图 + LVGL 双缓冲 2×(1/10屏)；全部 heap_caps 分配，TF 直读流式（包不整载内存）

### 4.3 状态机（app/）

```
BOOT → 自检 → (无配置?) WIFI_PROVISION(SoftAP captive portal)
正常态 POKER ←→ MENU（独立全屏，E6/E7）→ 子页(地图/纸娃娃/怪物/BGM)
POKER --闲置N分钟--> CLOCK_DOZE（待机时钟，RTC 走时）
任意交互 → POKER
降级态 OFFLINE（无网：TF 缓存继续跑，BGM 静音）/ FATAL（看门狗熔断后：文本提示+关屏）
OTA 态（双分区，失败回滚）
```

### 4.4 TF 卡布局

```
/mnipet/
  manifest.json        当前生效清单+本地hash集
  parts/<hash>.mpk     部件包
  layout/<hash>.mpk
  bg/<hash>.mpk
  font/<hash>.mpk
  firmware/            下载的 OTA bin（校验后写分区）
```
- 淘汰：LRU + 收藏保护（E7），容量水位 85% 触发

## 五、开发里程碑（顺序 = 依赖序）

| M# | 内容 | 验收 |
|---|---|---|
| M1 | Server 服务层迁移 + Tests 绿 | WZ/合成/渲染三测试真实数据通过 |
| M2 | AssetExporter + 包格式 | 导出默认装扮全动作包；设备侧无，先做 PC 端「解包回放」校验工具（PNG 重建对比 PaperdollService 直渲染逐像素一致） |
| M3 | 设备协议 + DeviceRegistry | curl 全端点联调通过（hello/manifest/poll/event/bgm） |
| M4 | Web 骨架 + 设置页 + 设备卡片 | 浏览器完成配置闭环（WZ 路径校验/保存/热重载） |
| M5 | Docker + CI + NAS 首部署 | NAS 上跑通 /api/health + Web |
| M6 | Firmware 最小渲染（点屏+PARTS/LAYOUT 播放） | 屏上播放 walk1/stand1/blink |
| M7 | 固件接入协议（poll/下载/缓存） | 换装换地图端到端 |
| M8 | BGM 链路（WZ 源） | 设备出声、触控控制 |
| M9 | IMU/力度/待机时钟/选择器 | E6/E7/E9 验收 |
| M10 | QQ 源 + 降级矩阵 + OTA | E8/E11 验收；异常注入测试 |

M1-M5（服务端线）与 M6 前期（固件 bring-up）可并行——固件不依赖服务端即可先用本地写死的包调试渲染。

## 六、风险与对策

| 风险 | 对策 |
|---|---|
| SkiaSharp 在群晖 arm64 容器缺 native | CI 冒烟测试先暴露；fallback：ImageSmapeSharp 替换渲染层（接口隔离在 IRenderBackend） |
| QSPI 带宽被 BGM 下载挤占 | asset_dl 任务限速 + 渲染优先（任务优先级）；BGM 流式恒定 16KB/s 影响小 |
| QQ 网关 node 子进程崩溃 | QqGatewayProcess 自动重启 1 次→仍失败则源置灰（E8 降级） |
| TF 卡写入寿命 | 包写入后只读；淘汰删除批量做；日志环形缓冲限内存 |
| manifest 频繁全量拉 | rev 未变返回 304 语义（ETag=rev） |
