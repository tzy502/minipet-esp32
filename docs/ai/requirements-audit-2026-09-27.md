# MiniPet-ESP32 需求验收矩阵（E1–E14 逐条核对）

> 审计日期：2026-09-27（15:00–15:3x CST）
> 审计对象：`docs/ai/requirements-analysis.md` E1–E14 全文
> 起始代码快照：`git HEAD = 67ba3fd`（14:50:26）；**审计期间 HEAD 前进到 `69fe230`**（15:05:41，见下）
> 工作树（审计结束时仍有未提交修改）：`Firmware/main/{app/provision.c,app/state_machine.c,audio/bgm.c,net/http_client.c,net/poller.c,render/compositor.c}`、`Server/MinipetServer/{Api/DeviceEndpoints.cs,MinipetServer.csproj,Music/WzMusicSource.cs}`；本报告 `docs/ai/requirements-audit-2026-09-27.md` 为审计新增
> 运行时验证产物：`Server/MinipetServer/bin/Release/net9.0/MinipetServer.dll`（构建于 **14:50:14**，早于 14:50:26 的 E13 提交）+ `Server/tools/Exporter/bin/Debug/net9.0/MinipetServer.dll`（构建于 15:01，含 92a42af/67ba3fd 两个提交）
> 状态定义：**已实现并有证据** / **部分实现（缺哪部分）** / **未实现** / **不适用（说明理由）**
>
> **审计期间仓库发生了两次提交**（本审计的运行时结论仍以 14:50/15:01 两个构建为准，并已标注差异）：
> - `155ccd1` docs+tool：人物消失根因文档 + 出厂素材重建脚本；
> - `69fe230` fix：**修掉本审计独立发现的 `GET /api/admin/presets` 路由被注释吞掉**（§4.2 实证）+ WZ 未加载返 503——即该缺口在审计过程中已闭环，但**用 14:50 的 Release 产物仍可复现**（本文如实记录「发现时状态」与「当前状态」）。

## 0. 审计方法与前提（必读）

1. **只读**：本次审计未修改任何代码文件；唯一写盘是 `/tmp` 下的临时数据目录、临时导出目录与本报告。
2. **仓库是移动靶**：审计期间其他 agent 正在并发写入（`compositor.c` 从 1914 行涨到 1933 行、`AdminEndpoints.cs` 15:01:09 被改、`DeviceEndpoints.cs`/`state_machine.c` 同期被改）。本报告所有 `file:line` 均标注「快照值 + 引文片段」，行号后续可能漂移，复核请按片段 grep。
3. **两类证据严格分开**（见 §7）：标「跑过」的结论来自真实执行（起服务端 + curl + 真跑导出器 + vite build + 解包分析）；标「只读」的结论来自读码推断，未在真机验证。
4. **未做**：未烧录固件、未碰串口 `/dev/cu.usbmodem*`、未占用 38090 端口、未跑 `git commit/push/checkout/stash`。

---

## 1. 验收矩阵表

### E1 — 服务端核心与素材导出（P0）

| 需求编号 | 要点 | 状态 | 证据（文件:行 / 命令+输出） |
|---|---|---|---|
| E1-1 | 桌面版服务层迁入 `Server/`（WzService/MapCatalogService/PaperdollService/SpriteService/MapService/BalloonService/MusicCatalogService/MusicDecisions/EventBus/PngEncoder） | 已实现并有证据 | `ls Server/MinipetServer/Services/` 含上述全部 20 个 .cs；`Server/MinipetServer/Utils/PngEncoder.cs` 存在（只读） |
| E1-2 | 门禁：`grep -rn "Avalonia" Server/ --include=*.cs` 零命中 | 已实现并有证据 | 跑过：计数 **0** |
| E1-3 | 门禁：`grep -rn "Dispatcher.UIThread" Server/ --include=*.cs` 零命中 | 已实现并有证据 | 跑过：计数 **0**（放宽 `grep -rn Dispatcher` 有 2 处，均为注释 `DispatcherTimer`：PaperdollService.cs:2043/:2372，与 software-design.md:36 口径一致） |
| E1-4 | 迁移时清理 Avalonia/Dispatcher 引用（**改 Channel**） | **部分实现** | EventBus.cs:92-99 `PublishOnUIThread` 直接同步调 `Publish`；`grep -rn "Channel" Server/MinipetServer --include=*.cs` 5 处全是音频「声道」（MusicData.cs:11,17 等），无 `System.Threading.Channels`。缺：字面的 Channel 化（现状是删 Dispatcher 改同步发布，功能上可行） |
| E1-5 | 部件导出为 RGB565 + origin | 已实现并有证据 | PartPackWriter.cs:13-20（20B 索引含 `origin_x/origin_y`）、:121-127 RGB565 量化；AssetExporter.cs:500-509 `_wz.GetOrigin()`（只读）。跑过：解包 PARTS 包 `part_count=294`，索引字段与格式文档一致 |
| E1-6 | 每动作每帧导出 LAYOUT（帧 × 部件 × x,y,z + expression 维度） | 已实现并有证据 | LayoutPackWriter.cs:8-30（`frame_count/expression_count/表情名表`；每 piece `part_id u32｜expr_index u8｜x i16｜y i16｜flip u8｜z i8`）。跑过：解包 stand1 → `frame_count=3 expression_count=25`，`expressions=[blink,hit,smile,troubled,cry,angry,bewildered,stunned,vomit,oops,cheers,chu,wink,pain,glitter,despair,love,shine,blaze,hum,bowing,hot,dam,default,qBlue]`。备注：默认动作集 **11 个**（AssetExporter.cs:96-99 `walk1/stand1/stand2/blink/jump/fly/prone/ladder/rope/alert/heal`），非 WZ 全动作（需求未要求全动作） |
| E1-7 | 地图导出 static_back + tile_layer + 视差条带元数据（偏移=f(time)） | 已实现并有证据 | BgmapPackWriter.cs:12-22（`vw/vh` + static + tile + 每 strip 16B `part_ref/y/speed_x/rx_parallax/blend`）、:80-96 写入；AssetExporter.cs:604-659（static_back 剔除条带项、tile 关 back、条带 `speed_x=rx*5`）。跑过：地图推送后 BGMAP 包 vw/vh 可解出 |
| E1-8 | 数据一切从 WZ 走（无数据库、无硬编码地图数据） | 已实现并有证据 | 无 DB（data/ 全 JSON）；WzService.cs:74 `LoadWz`；Exporter CLI 强制 `--wz`（Server/tools/Exporter/Program.cs:40/:81-83 无路径即退出）；跑过：导出器以 `/Volumes/SSD/mxd/mxd/Data` 真跑成功（21,335 张地图目录、1167 首曲） |
| E1-9 | 路径经配置注入（禁止硬编码） | **部分实现** | 生产路径全注入：ConfigService.cs:17 默认 `/wz/Data`、Program.cs:14-16 `MINIPET_DATA_DIR`、:74 `LoadWz(cfg.DataPath)`、:115-119 路径变更重载。缺：测试夹具硬编码 `/Volumes/SSD/mxd/mxd/Data`（MinipetServer.Tests/TestHelpers/WzFixture.cs:14、WzTestHarness.cs:14；可环境变量覆盖 + 缺目录自动 Skip） |
| E1-10 | 表情按 25 个实证表情全量导出（expression 维度） | 已实现并有证据 | PaperdollService.cs:63-74 `KnownExpressions` 恰 25 项；AssetExporter.cs:322-323 取全 25 作 expression 列表、:404 `allocator.Alloc(p.Category, expressions.Count)` 连续 25 槽、:472-492 缺帧回退 default。**跑过**：解包 PARTS（stand1 同批导出）→ 唯一 face 家族 `expr_group=1` 恰 **25** 个 part，`part_id 100..124` 连续 |
| E1-11 | 480 屏部件 1x（包内存原图，不放大） | 已实现并有证据 | algorithm-asset-format.md:48「2x 缩放在设备端做（nearest），包里存 1x」；AssetExporter.cs:432-433 坐标为 1x 画布坐标（只读） |
| E1-12 | **480 屏 1x + scale 2x nearest 由导出器标注**（争议点 1） | **部分实现** | ✅ 导出器 CLI 真跑：`/tmp/audit_export/default/manifest-assets.json` 每条 LAYOUT 带 `"scale": 2, "bottomMargin": 40`（**number**，类型正确；AssetExporter.cs:302-303 + ManifestBuilder.cs:52-58 是修好的那份序列化器）。<br>❌ **设备实际走的那条链路仍写字符串**：服务端 `GET /api/device/manifest` 返回 `"scale": "2"`（两次不同构建均复现）——`PaperdollPackService.cs:142-167` 与 `DeviceAssetService.cs:174-193` 各有一份 `EntryToJson` 复制品，**没有** ManifestBuilder 那 5 行数值分支（两者都在 :160/:192 保留 `o[k] = v.ToString() ?? ""`）。<br>❌ 固件完全不消费：`grep -rn '"scale"\|bottomMargin' Firmware/main Firmware/profiles` 零命中；固件用编译期 `RC_SCALE 2`（compositor.h:22）+ `RC_ENT_MARGIN_B 40`（compositor.h:44）。nearest 2x 实现在 compositor.c:761-788（`<<1` + 2×2 复制） |
| E1-13 | 五类包 / MPAK 容器与 `algorithm-asset-format.md` 一致 | 已实现并有证据 | Mpak.cs:8-16（Parts1/Layout2/Bgmap3/Font4/AudioMeta5 + Thumb6）、:45-62（头 40B：`MPAK`+ver u16+flags u16+8B 填充+kind u64+hash u64+len u32+reserved）、:63 尾 crc32c、:100-122/:155-188 构建与校验。跑过：`/api/device/asset/{hash}` 首 40 字节 `4d50 414b 0100 ...`（magic+ver=1）与代码一致。备注（非功能缺陷）：algorithm-asset-format.md:29 的校验顺序描述与实现顺序（长度先于 crc）不一致 |

### E2 — 设备协议（P0）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E2-1 | `POST /api/device/hello`：profile(w,h,shape,psram,audio)+UUID+固件版本 → deviceId+配置 | 已实现并有证据 | DeviceEndpoints.cs:31/:36-62（DTO）、:96+ `HandleHello`。**跑过**：`{"deviceId":"dev-9102b2","proto":1,"paired":false,"pairingCode":"942576","config":{"imuSensitivity":1,"imuDeadzoneDeg":8,"tapLightG":2,"tapHardG":4,"idleToClockMin":5,"bgmDefaultSource":"wz","volume":60},"manifestRev":1,"manifestUrl":"...","pollUrl":"...","serverTimeUtc":"..."}` |
| E2-2 | `GET /api/device/manifest`：部件/布局/背景/字体/BGM 清单 + hash | **部分实现** | DeviceEndpoints.cs:32/:170-187（带 ETag `"r{rev}c{cachedVersion}"`，If-None-Match→304）；DeviceManifestService.cs:133-246。**跑过**：`{"proto":1,"rev":3,"assets":{...16 项：LAYOUT 11 + FONT 3 + PARTS 2},"entities":[...],"clock_table":{...}}`；地图推送后追加 `BGMAP 1 + THUMB 1`。缺三处：①需求写「部件/布局/背景/字体/BGM 清单」，实现是**单张 `assets` 映射**（kind 区分），非五个具名数组；②**BGM(AUDIO_META) 只在跑导出器 CLI 时产生**（AssetExporter.cs:193-210 在 `Run()` 内，唯一调用点 tools/Exporter/Program.cs:105），hello 首启 provisioning 路径（DeviceEndpoints.cs:364-426）不含 AUDIO_META；③manifest 的 `firmware` 对象固件不解析（OTA 只走 poll 指令，poller.c:92-95） |
| E2-3 | `GET /api/device/asset/{hash}` 单包分发 | 已实现并有证据（隔离性有缺口） | DeviceEndpoints.cs:33/:190-198（流式 64KB buffer）；查找顺序 DeviceManifestService.cs:252-291。**跑过**：stand1 包 `http=200 bytes=1432`，magic 校验通过。缺：**未按 deviceId 隔离**——本设备目录未命中会扫 `data/cache/export/*` 兜底（:261-269），知道 hash 即可跨设备取包（manifest 本身按设备隔离） |
| E2-4 | `GET /api/device/poll?since=`：切动作/表情/气泡/BGM 控制/亮度/重启 | 已实现并有证据 | DeviceEndpoints.cs:34；`PollMaxWait=55s`（:22）。**跑过**：`{"deviceId":"dev-9102b2","since":0,"lastSeq":7,"commands":[{seq:3,type:"expression",payload:"smile"},{seq:4,type:"bubble",payload:"审计测试气泡"},{seq:5,type:"brightness",payload:{"n":70}},{seq:6,type:"bgm",payload:"play"},{seq:7,type:"reboot",payload:{}}],"mrev":3}`。备注：需求只写 `?since=`，实际**必须带 deviceId**（缺则 404，poller.c:122-126 有真机实证注释） |
| E2-5 | `POST /api/device/event`：触摸/力度/倾斜变迁/低电/错误/缓存 hash 集 | 已实现并有证据 | DeviceEndpoints.cs:54-65（单事件 DTO `{deviceId,type,tsUtc,data,hashes}`）、:35。**跑过**：battery 事件 200；`{"ok":true,"manifestRev":3}`，且下轮 manifest 里 `cached:true` 落到上报 hash 的 PARTS 条目（服务端算 cached 生效）。备注：低电事件名是 `battery`（非 `low_battery`），电量在 `data.a`；固件可携 `data.batch[]`；失败即丢不重放（events.c:179-183，E11 已知取舍） |
| E2-6 | `GET /api/device/bgm/stream` MP3 流 | 已实现并有证据 | DeviceEndpoints.cs:37/:275-294（BgmRouter 流式转发，失败 503 + `bgm_failover`）。**跑过**：`http=200 bytes=1666351 type=audio/mpeg`，`file` 判定 `MPEG ADTS, layer III, v2, 80 kbps` |
| E2-7 | `POST /api/device/bgm/cmd`：播放/暂停/切歌/音量 | 已实现并有证据（白名单有差） | DeviceEndpoints.cs:38/:299-336（白名单 `play/pause/next/prev/select/volume`）。**跑过**：`play/pause/next/prev/volume` 全 200；next→`Bgm00.img/FloralLife`、prev→`Bgm_Picture.img/4350046`（**未跨出 wz 源**，符合 E8 短路规则）。备注：服务端白名单**无 resume/stop**，固件本地动词含之（poller.c:83-90）；Web 音量下发绕开本端点走旧口径 `{t:"bgm",v:"vol",n}`（AdminEndpoints.cs:295-305） |
| E2-8 | 协议里 `entities[]` 数组（首版 N=1） | 已实现并有证据 | DeviceManifestService.cs:187-231 聚合。**跑过**：`entities=[{id:"paperdoll:default",kind:"paperdoll",layouts:[11 hash],partsHash:"abfa...",appearanceHash:"0｜0｜2000｜...",defaultAction:"stand1"}]`。备注：只在 manifest；固件不消费（未知顶层字段忽略），属服务端侧预留 |
| E2-9 | 无 Hermes 字段、无 HA 字段（隔离铁律） | 已实现并有证据 | 跑过 grep：`Server/MinipetServer` + `Firmware/main` + `Web/src` 内 Hermes/HA 仅出现在 PaperdollService.cs:44/:2337/:2614 的**预留注释**（`IExpressionDriver` 未接线）；协议字段零命中；`grep '"state"'`（协议无 state 槽）零命中 |
| E2-10 | 无网络降级：TF 卡缓存独立运行 | 已实现并有证据 | 固件：state_machine.c:292-319（OFFLINE 分支「TF 缓存继续跑（布局表本地播，blink 本地循环）」）、asset_dl.c:118-128 目录 `/minipet/{parts,layout,bg,font,audio}`、`asset_dl_have_local_manifest()`（asset_dl.c:862）（只读） |
| E2-11 | 协议版本号 `proto` 预留，v2 不破坏 v1 | **部分实现** | app_core.h:37 `MP_PROTO_VER 1`；上行携带 hello（http_client.c:384）/event（events.c:123）/本地 manifest（asset_dl.c:329）/log（http_client.c:532）/bgm cmd（bgm.c:235）；服务端 `DeviceManifestService.cs:19 Proto=1`，hello 回显 DeviceEndpoints.cs:147、manifest 根 :145。缺：**poll/event/bgm 响应体无 proto 回显**（DeviceEndpoints.cs:216-229/:245/:335），协商目前单向 |
| E2-12 | 设备永远是 client | 已实现并有证据 | 固件网络层全为 HTTP client（http_client/poller/asset_dl/events/bgm/ota）；唯一 httpd 是配网 captive portal（provision.c:588-592 三个 uri），不监听业务端口 |

### E3 — 服务端部署与配置（P0）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E3-1 | 单容器 api：ASP.NET Core 托管 Vue 产物 + API + BGM 流 | 已实现并有证据 | Dockerfile:26-42（webbuild）/:107（拷 dist→wwwroot）/:84-135（单 final 层）；Program.cs:138-140/:164（静态+SPA fallback）；跑过：Vue 构建成功、服务端 `/api/*` 可用（只读 Dockerfile + 实跑服务端） |
| E3-2 | QQ 音乐网关 = 容器内 node 子进程（配置开关） | 已实现并有证据 | Dockerfile:97-101（runtime 层带 node）；QqGatewayProcess.cs:122-127（未启用即 StopProcess）、:159-195（健康检查+重启 1 次）；Program.cs:33/:59 早绑定。跑过：`/api/admin/music/sources` → `qq.enabled=false, gateway.running=false` |
| E3-3 | 对外唯一五位数端口 38090（`.env MINIPET_PORT`） | 已实现并有证据 | `.env.example MINIPET_PORT=38090`；docker-compose.example.yml:16-19；Dockerfile:132-134（容器内 8080） |
| E3-4 | 无 web 中间层 | 已实现并有证据 | compose 单 service；Dockerfile 无 nginx/Caddy |
| E3-5 | 配置双通道写同一份 `data/config/appsettings.json` | 已实现并有证据 | ServerPaths.cs:16-17；ConfigService.cs:289-295（`AtomicWriteAllText`）；Web 通道 AdminEndpoints.cs:347-357；文本通道 ConfigService.cs:157-167。跑过：`GET /api/admin/settings` → `config.wz.dataPath` 与磁盘文件一致 |
| E3-6 | 改后热重载（WzService 重新 LoadWz） | 已实现并有证据 | ConfigService.cs:256-287（400ms 防抖 + `_selfWrite` 抑制回环）；Program.cs:115-119（DataPath 变更→重载）。备注：无独立 `ConfigWatcher` 类（折进 ConfigService，需求是二选一） |
| E3-7 | 端口不入 appsettings（端口属部署层） | 已实现并有证据 | `MinipetConfig` 无端口字段（ConfigService.cs:101-110）；跑过：`/api/admin/settings` 响应无端口键（`GatewayPort:3300` 是容器内 QQ 网关内部端口） |
| E3-8 | Web 设置页端口只读展示 | 已实现并有证据 | SettingsView.vue:221-224（`label="服务端口（只读）"` + disabled）；AdminEndpoints.cs:232-237 附 `note` |
| E3-9 | WZ 数据只读挂载（容器 `/wz`） | 已实现并有证据 | docker-compose.example.yml:20-27 `- <WZ_DATA_HOST_PATH>:/wz:ro` + `MINIPET_WZ_PATH=/wz/Data` |
| E3-10 | 仓库内禁止真实 IP/用户名（占位符） | **部分实现** | 部署面干净（compose 用 `<owner>`/`<WZ_DATA_HOST_PATH>`；deploy-nas.sh:13-15 `<NAS_IP>` + :22-29 守卫）。缺：开发文档与测试默认值仍含真实路径/用户名——`git grep -l -E "Volumes/SSD\|a502\|/Users/"` 命中 docs/ai/technical-reference.md、docs/ai/selftest-report-2026-09-27.md、Server/T1-T4-server-report.md、WzFixture.cs:14、WzTestHarness.cs:14 |
| E3-11 | GH Actions 构建单镜像 → GHCR | 已实现并有证据 | .github/workflows/ci.yml：`docker/build-push-action@v6` + `ghcr.io/${{ github.repository }}` + `platforms: linux/amd64,linux/arm64`，仅 main/tag |
| E3-12 | `scripts/deploy-nas.sh`（Mac buildx → save/load 直投） | 已实现并有证据 | scripts/deploy-nas.sh：buildx --load → docker save → scp → NAS load → compose up |
| E3-13 | 验证清单：SkiaSharp 容器内可用 | **部分实现** | Dockerfile:87-95 装 libfontconfig1、:68-70 构建期断言 `libSkiaSharp.so` 存在；ci.yml smoke 仅断言日志无 `Unable to load shared library`。缺：容器内**真实渲染**用例（CI 冒烟容器无 WZ，thumb 报错被显式放过；software-design.md:93 说的「容器内跑 Tests」未落地） |
| E3-14 | fallback ImageSharp / `IRenderBackend` | **未实现** | `grep -rn "ImageSharp\|IRenderBackend" Server/ Web/src` 零命中，仅存在于文档（deployment-design.md:139、software-design.md:93/:190） |
| E3-15 | 双架构镜像 | 已实现并有证据 | ci.yml `platforms: linux/amd64,linux/arm64` + QEMU/buildx；Dockerfile:13-15/:69-70 |
| E3-16 | 验证清单：homes 挂载权限等 | **部分实现** | deployment-design.md:135-143 清单：仅第 1 项 ✅，SkiaSharp/homes ACL/runtime/BGM 直出均 ⬜ |

### E4 — Web 配置前端（P0）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E4-1 | Vue 3 + Naive UI | 已实现并有证据 | Web/package.json（vue ^3.5.13 / naive-ui ^2.41.0 / pinia / axios / vue-router）。**跑过**：`cd Web && npx vite build` → `✓ built in 3.73s`，产物 24 个 chunk（DashboardView/MaterialsView/PaperdollView/MusicView/SettingsView 均在） |
| E4-2 | 构建产物进 api 镜像，单容器同源 | 已实现并有证据 | Dockerfile:29-42/:107/:116-128（sha256 指纹断言） |
| E4-3 | 设备卡片列表（首页） | 已实现并有证据 | router/index.js `/`→DashboardView.vue:105-140 |
| E4-4 | 在线状态 | 已实现并有证据 | DashboardView.vue:109-111/:128；stores/devices.js:23/:44-47（5s 轮询）。跑过：`GET /api/admin/devices` → `"online":true` |
| E4-5 | 当前宠物缩略图 | 已实现并有证据 | DashboardView.vue:121-123 `<DevicePetThumb :size="64"/>`；components/DevicePetThumb.vue:1-22（服务端按 petConfig 合成） |
| E4-6 | 换宠换装入口 | 已实现并有证据 | DashboardView.vue:133-137 → `/device/:id`；DeviceDetailView.vue:489（PUT devices/{id} 带 petConfig） |
| E4-7 | 配对码入册 | 已实现并有证据 | DashboardView.vue:16-44/:142-167（6 位校验）；AdminEndpoints.cs:93 `POST /pair`。跑过：hello 返回 `pairingCode:"942576"`；`POST /api/admin/pair` 端点存在 |
| E4-8/9/10 | 素材浏览器（地图 / 纸娃娃部件 / 怪物 NPC） | 已实现并有证据 | MaterialsView.vue:30-33。跑过：`GET /api/admin/materials?kind=map` → `total=21422` |
| E4-11 | 服务端渲染缩略图 | 已实现并有证据 | client.js:37-42 `thumbUrl`；AdminEndpoints.cs:105-108；ThumbService.cs:65-72。**跑过**：`/api/admin/thumb?type=map&id=200000100` → `http=200 type=image/png`，`file` 判定 **64×64** RGBA |
| E4-12 | 纸娃娃编辑器（换装组合 + 服务端合成预览） | 已实现并有证据 | PaperdollView.vue:4-7（16 槽）、:166 合成预览 |
| E4-13 | 换装组合存为预设（供设备选择器纸娃娃 tab） | **部分实现（发现时）→ 已修复（审计期提交）** | 编辑器侧 ✅（PaperdollView.vue:238-241 `createPreset`；AdminEndpoints.cs:123-144 CRUD；Program.cs:121-136 首启种子）。**审计发现**：`GET /api/admin/presets` 在 HEAD 67ba3fd 快照里**未注册**——AdminEndpoints.cs:122 注释与 `g.MapGet(...)` 同一行被 `//` 吞掉（`git show HEAD:Server/MinipetServer/Api/AdminEndpoints.cs \| sed -n '122p'` 可复现）；**跑过**：`GET /api/admin/presets` → 404（SPA fallback 会返 200+HTML，axios 视为成功 → 预设列表静默为空），`POST /api/admin/presets` → 201。**当前状态**：该行已在 commit `69fe230`（15:05:41）拆行修复，需**重建镜像**才生效。缺：设备侧仍无「预设列表」端点（见 E7-4） |
| E4-14 | 曲库管理：WZ 曲库浏览 | 已实现并有证据 | MusicView.vue:613-616/:650-659。跑过：`GET /api/admin/music/tracks?source=wz` → `count=1167` |
| E4-15 | QQ cookie 导入 | 已实现并有证据 | MusicView.vue:562-575；AdminEndpoints.cs:198 |
| E4-16 | cookie 有效期告警 | 已实现并有证据 | MusicView.vue:531-543（`cookieStale`/`cookieAgeDays`，>7 天告警）；ConfigService.cs:36-39。跑过：`/api/admin/music/sources` 返回 `cookieStale` 字段 |
| E4-17 | 音源启停开关 | 已实现并有证据 | MusicView.vue:518-526；AdminEndpoints.cs:182 `PUT /music/sources/{name}` |
| E4-18 | 设置页 WZ 路径（含存在性校验） | 已实现并有证据 | SettingsView.vue:210-219/:118；AdminEndpoints.cs:241-245 → ConfigService.cs:194-202。**跑过**：`POST /api/admin/settings/validate-path {"path":"/nope/wz"}` → `{"ok":false,"message":"WZ 路径不存在：/nope/wz"}` |
| E4-19 | 端口只读展示 | 已实现并有证据 | 同 E3-8 |
| E4-20 | 各阈值（IMU 灵敏度/死区/力度分级） | 已实现并有证据 | SettingsView.vue:231-267；ConfigService.cs:58-73、:319-321（夹取 0.2–3.0）。跑过：hello 下发 `imuSensitivity/imuDeadzoneDeg/tapLightG/tapHardG/idleToClockMin` |
| E4-21 | 写 appsettings.json | 已实现并有证据 | SettingsView.vue:160-186 → PUT /settings；AdminEndpoints.cs:347-357 → ConfigService.Save |
| E4-22 | 25 表情手动指定（调试/演示） | 已实现并有证据 | Web/src/utils/expressions.js:22-47 恰 25 项；DeviceDetailView.vue:701-754 `v-for="ex in EXPRESSIONS"`；AdminEndpoints.cs:257-278 |
| E4-23 | 地图选择含收藏（喂设备选择器「最近+收藏」） | **部分实现** | Web 侧 ✅：utils/favorites.js:20-25/:65-113；DeviceDetailView.vue:262-300；AdminEndpoints.cs:112-118 + FavoritesStore。缺：**服务端没有把「预设+最近+收藏」下发给设备的端点**（`/api/device/*` 仅 9 条路由，无 maps/presets 列表），设备选择器只能看到 manifest 里本设备已有的 BGMAP 条目（§5 争议 3、§2） |
| E4-24 | 不做：设备端搜索输入 | 不适用（确认未实现，符合要求） | `grep -rn "search\|搜索" Firmware/main --include=*.c --include=*.h` 零命中（Web 素材页有搜索框属 Web 侧，不违反） |
| E4-25 | 不做：Web 点歌按钮 | **部分实现（需求为「不做」）** | 曲目表无操作列（MusicView.vue:68-73）→ 逐曲点歌确未做 ✅；但存在**超出需求**的「设备播放控制」卡（MusicView.vue:410-502：▶/⏸/⏭/⏮/音量下发，注释 :496-499 自述超出需求），服务端放行 bgm 指令（AdminEndpoints.cs:282-291）。与「BGM 控制在设备上」的定稿口径有事实偏离，建议产品侧确认是否撤回 |

### E5 — 设备端渲染与动画（P0）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E5-1 | 按 LAYOUT 在动作帧切换时合成一次，平时只 blit | 已实现并有证据 | 实体画布 `g_ent_px`（compositor.c:66）；`recompose_entity()` :790-815（memset→`resolve_piece` :732-757→`blit_ent_2x` :761-788）；仅帧推进 :1415-1450 / 换 LAYOUT :1602-1631 / 换 PARTS :1545-1560 / 换表情 :1633-1643 触发；平时 `ent_compose()` :903-928 只 blit（只读） |
| E5-2 | 表情切换 = **只重合成 face 类部件** | **部分实现** | face 语义正确：`resolve_piece` :739-756 仅当 `expr_index!=255 && expr_group!=0` 时用 `mpak_parts_variant` 同位替换。缺：`render_set_expression`（:1633-1643）走的是 `recompose_entity()` **整画布重合成**，没有「只重画 face 件」的增量路径（输出正确、性能未优化） |
| E5-3 | **图层顺序**（争议点 2）：static_back → 视差条带 → tile_layer → 宠物 → 气泡 | 已实现并有证据 | `compose_region`（compositor.c:930 起，快照 1933 行版）实测顺序：CLOCK_DOZE 特例 :946-951 → ①static_back :954-964 → ②条带 :966-968（`strip_blit` :851-879）→ ③tile_layer :970-988 → ④地图时钟 :990-991 → ⑤校准调试层（仅 `g_calib_on`）→ ⑥实体/宠物 :1059-1061 → ⑦气泡 :1063-1074 → ⑧未配网横幅 :1076 → ⑨BGM 半屏条 :1116-1117。**与需求逐层一致**（无 tile/strip 颠倒、宠物在 tile 之上、气泡在宠物之上）。备注：条带内 piece 的 `z` 字段不参与排序（mpak.c 只存不用；compositor.c:801-802 注释「帧内 piece 列表顺序 = 权威绘制序，z 仅诊断」），依赖导出端按桌面 RenderFrame 底→顶排序（LayoutPackWriter.cs:35-36 同契约） |
| E5-4 | 动画时序按 LAYOUT delay 推帧 | 已实现并有证据 | entity_anim.c:145-184（:146 取 `delay_ms`、:160 deadline、:166-169 循环回绕、:170-175 单次 finished；:147 delay=0→50ms、:148-159 坏值钳 1s）；30fps 节拍 main.c:86-88（`render_tick()` + `vTaskDelay(33ms)`） |
| E5-5 | 动作播完回 stand（**回退链**） | **部分实现** | 单级回退有：render_tick :1421-1430 → `bind_active_layout()` :722-728 → `active_layout()` :661-666 回落 `g_lt_loop`（stand1，由 state_machine dispatch_action :545-555 装载）；LAYOUT 路径解析兜底 asset_dl.c:919-942。缺：**多级回退链**（stand1 缺失 → walk1 → default 之类）未实现；stand1 包缺失时 `dispatch_action` 直接 return 保留旧画面（state_machine.c:529-539） |
| E5-6 | 缩放：480 屏 1x 部件 + scale 2x nearest | **部分实现** | nearest 2x ✅：`RC_SCALE 2`/`RC_SCALE_SHIFT 1`（compositor.h:22-23）、`blit_ent_2x` :766-786（`<<1` + 2×2 复制）、背景 1x→2x `layer_rgb_load` :1243-1266（`srow[dx>>1]`）；无插值（`grep -rn 'interp\|bilinear\|linear_filter\|smooth' Firmware/main/render` 零命中）。缺：LAYOUT 二进制包**没有** scale 字段（mpak.h:93-110 帧头/piece 定长），固件**不读** manifest 的 `scale`（`grep -rn '"scale"\|bottomMargin' Firmware/main Firmware/profiles` 零命中），脚底 40px 也是编译期常量 `RC_ENT_MARGIN_B 40`（compositor.h:44）→ 与 E1-12 的「标注」契约未接通（同 §5 争议 1） |
| E5-7 | 背景三层 + 视差偏移 = f(time) 或 IMU 倾角 | 已实现并有证据 | `strip_offset` compositor.c:839-849 = `speed_x*ms/1000`（时间项）+ `tilt_mdeg*rx*4/(100*1000)`（IMU 项，`RC_TILT_PX_PER_DEG 4` compositor.h:61），再 mod 图宽；`strip_blit` :851-879 循环平铺；三层由 `render_set_map` :1654-1710 载入（含 1x 源展开） |
| E5-8 | 脏区上传：只推变化区域（宠物区 30fps ≤31% 带宽） | **部分实现** | 机制在：16×16 网格 `RC_CELL 16`（compositor.h:54）、`mark_rect` :521-537、`mark_ent_at` :539-561、`flush_dirty` :1126-1160（合并 bbox → 一次 compose + 一次 `blit_be`，`display_blit` 唯一出口 :1202）。缺：(a) **无带宽核算/限流**（`grep -rn 'bandwidth\|bytes_per\|fps_stat\|dirty_bytes' Firmware/main/render` 零命中；「≤31%」只存在于需求/评审文档）；(b) 所有脏格合并成**单一包围盒**，有条带动画时 bbox = 整条带（全宽）∪宠物，实际上传远大于「宠物区」（只读推断） |
| E5-9 | blink 本地循环：随机 3~8s，断网也眨 | 已实现并有证据 | entity_anim.h:19-21（`RC_BLINK_MIN_MS 3000`/`MAX 8000`/`HOLD 250`）；`rc_anim_kick_blink` entity_anim.c:86-91 用 `esp_random()`；状态机 :126-140；纯本地（无网络依赖），OFFLINE 走同一渲染路径（state_machine.c:158-170） |
| E5-10 | 动画铁律：只播服务端下发的布局表，固件不自创动画 | 已实现并有证据 | 固件只按名字引用 WZ 真实 action/表情（app_core.h:87-92「仅引用不自创——E5 动画铁律」）；帧数据唯一来源 = LAYOUT 包解析（mpak.c:515-568）；播放器只按 delay 推帧（entity_anim.c:145-184）；缺包保留旧画面不合成替代动画（state_machine.c:529-539）。备注（如实计入，非动画帧）：倾斜/拖拽像素偏移、睡眠降亮、横幅/时钟/BGM 条、气泡 5x7 兜底字模——均非动画序列 |

### E6 — 触摸 / 重力 / 力度 / 按键（P0）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E6-1 | 任何可用物理键 = 「菜单键」；主力板 GPIO18；键位映射表在 profile；Web 可改键 | **部分实现** | GPIO18 菜单键 ✅：profile_amoled216.c:36 `.key = { .menu = 18 }`、amdl216.h:38/57；驱动 key_gpio18.c:28/50/76-81；扫描 input_dispatch.c:831-883（30ms 防抖 + 门闩 + 长按 700ms→时钟）；状态机 state_machine.c:407-427。缺：profile 只有**单个 `key.menu` 引脚字段**（amdl216.h:26-40），**没有键位映射表**；`grep -rni 'keymap\|键位\|改键\|hotkey' Firmware Web/src Server` 只命中 LVGL 库 xkb → **Web 改键零实现** |
| E6-2 | 无键设备退化为主屏角落常驻小按钮 | **未实现** | `grep -rn '角落\|corner\|SCREEN_BTN\|on_screen_button'` 只命中需求原文；`grep -rn 'has_key' Firmware/main Firmware/components` 零命中（profile 有字段但工程内无按键存在性分支） |
| E6-3 | 菜单 = 独立全屏窗口（宠物与触摸隐藏/冻结、不穿透、关闭恢复） | 已实现并有证据 | 进入：state_machine.c:146-149 → `render_enter_menu` compositor.c:1746-1754；render_tick 菜单分支 :1378-1408（整屏 `menu_buf` 覆盖 + 全屏 blit，且在帧推进前 `return` :1407 → 宠物不画、动画时钟停走）；触摸只喂 LVGL（input_dispatch.c:580-602）、indev 非菜单恒 RELEASED（lvgl_bridge.c:349-361）；关闭：lvgl_bridge.c:410-414 → state_machine.c:111-113 → `render_exit_menu` compositor.c:1756-1767 + POKER on_enter 重发 stand1（:131）。备注：恢复 = 回 stand1 整屏重合成，不是菜单前动作/表情快照；IMU 菜单期仍采样（不可见，退出被 stand1 覆盖） |
| E6-4 | 触摸：轻点宠物 = 抚摸（smile/love） | **部分实现** | input_dispatch.c:770-780（`dur<400ms && dx<30px && dy<30px` → `MP_EVT_TOUCH_PET` + SMILE/LOVE 1500ms + 气泡）。缺：**无宠物区命中测试**（POKER 态全屏任意轻点都算抚摸）；抚摸气泡固定英文 "hello"，不走 E12 台词配置 |
| E6-5 | BGM 播放控制 = 触摸半屏控制条（选择器入口 或 宠物区长按） | 已实现并有证据 | compositor.c:119 `RC_OV_H 220`（下半屏）、:128 5 个控件、:129 3s 自动收起；`render_bgm_bar_activate` :205-240 调真实 `bgm_*`；绘制 `ov_draw` :332+ 最顶层 :1117；呼出两条：宠物区长按 input_dispatch.c:729-747（800ms，离线不呼出）+ 菜单 BGM 行 lvgl_bridge.c:683-687 → compositor.c:275 → input_dispatch.c:1321-1328 |
| E6-6 | 左右倾斜→视差反向+原地 walk1；上倾→fly；回正→stand；死区 ±8°、300ms 防抖 | 已实现并有证据 | 默认 main.c:61-62（`imu_deadzone_deg 8.0f`、`tilt_debounce_ms 300`）、app_core.h:201-202；`tilt_fsm_tick` input_dispatch.c:233-298（有效死区 = 名义/灵敏度 :240-245、防抖 :246-253、左右→`MP_ACTION_WALK` :276-283、上倾→FLY :284-287、退出→STAND :288-295、`render_input_tilt(roll)` :274）；竖握保护 :217-231 |
| E6-7 | 倾斜视觉全本地（只上报状态变迁，无角度流、零网络往返） | 已实现并有证据 | 仅变迁时 `MP_EVT_TILT_ENTER/EXIT`（input_dispatch.c:277/281/285/289/293，仅带倾斜位不带角度）；连续视差直写渲染层（:274 → compositor 视差项）；events.c:36-37 协议映射只含 type/a/b/s |
| E6-8 | 力度分级（设备端算好只上报事件）：轻拍<2g→alert+bewildered；≥4g→hit；摇晃→stunned；拿起/翻转→fly+oops | 已实现并有证据 | `force_tick` input_dispatch.c:303-377：`mag>=tap_hard_g(4.0/灵敏度)`→TAP_HARD+HIT（:317-322）；`mag>1.25f && mag<tap_light_g(2.0)`→TAP_LIGHT+ALERT+BEWILDERED（:323-329）；摇晃 700ms 窗口 x 过零≥4 且幅度≥1.6g→SHAKE+STUNNED（:331-357）；姿态偏离>25° 且 \|z\|<0.85g 持续 600ms→PICKUP+FLY+OOPS（:359-376）。备注：轻拍有 **1.25g 下限**（需求只写「<2g」，属更严的防误触，记录差异） |
| E6-9 | 阈值全部 Web 可配（与 E4 同一份配置） | **部分实现** | 可配：`imuDeadzoneDeg/tapLightG/tapHardG/imuSensitivity/idleToClockMin`（hello 下发 http_client.c:414-431；服务端 Device 段 + ConfigService.cs:58-73 + DeviceEndpoints.cs:143/:151-160；Web SettingsView.vue:160-172）。缺：`tilt_debounce_ms`、SHAKE_*（700/4/1.6）、PICKUP_*（25°/600ms）、`TAP_MAX_MS`、`LONGPRESS_MS` 全是固件常量，服务端无对应键 |

### E7 — 设备选择器（控制中心）（P0）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E7-1 | 菜单键呼出独立全屏窗口，三个 tab（地图/纸娃娃/怪物-NPC） | **部分实现** | 三个必需页都在：Maps lvgl_bridge.c:947-951、Paperdoll :952-956、Monsters(NPC) :962-966；根页 ROOT_ROWS :583-591。缺：形态是「根页垂直列表 + 子页」，**不是 tab 栏**（无 `lv_tabview`）；另多出 Actions 页（:518-539，硬编码 5 个动作名）与 Reset WiFi 页（:1001-1012） |
| E7-2 | 地图 tab：后端返回（预设+最近+收藏）；后端不可用→只显本地缓存项；缩略图 64×64 服务端渲染；`cached` 服务端算；未缓存→拉包落 TF（1~2s）→切换 | **部分实现** | ✅ 列表来自 manifest 的 BGMAP 条目（lvgl_bridge.c:450-468 → asset_dl.c:185-191/:165-182）；三态行文本 `[v]/[ ]/[x]`（:848-853）；离线未缓存置灰不可点（:888 + :664-667/:635-639）；未缓存→`asset_dl_request_one`（:765-788）→菜单 tick 轮询落盘（:758-788，30s 超时）→`MP_CMD_SET_MAP`。❌ 缺：(a) 服务端**没有** `/api/device/maps` 之类「预设+最近+收藏」下发端点（跑过：`/api/device/*` 仅 9 条路由），设备实际看到的是 manifest 里本设备全部 BGMAP 条目 + 本地 cached-first 排序；(b) 「后端不可用→只显本地缓存项」实现为**未缓存项仍列出但置灰**（非过滤隐藏）；(c) 服务端确实算 `cached` 并下发（DeviceManifestService.cs:151-160，输入 = event 上报 hashes），但固件 `upsert_meta`（asset_dl.c:474-503）**不读该字段**，行内 cached 用本地文件存在性；(d) 「1~2s 落包」无代码保证（只有 30s 超时） |
| E7-3 | **缩略图 64×64 服务端渲染**（争议点 3） | **部分实现**（服务端有、设备端未显示） | 服务端 ✅：THUMB 包确实生成并进设备 manifest（AssetExporter.cs:680-704 `SKBitmap(96,96)`→PNG、:697 `Kind=Thumb`、:707 `extra["thumb"]`）；**跑过**：地图推送后 manifest 出现 `BGMAP{selector:map,map:200000100,thumb:"076c31a3228e3dcf"}` + `THUMB{缩略图 200000100}`，磁盘 `076c31a3228e3dcf.png` 为 PNG。❌ 设备端：菜单行只有 `lv_button + lv_label`（lvgl_bridge.c:815-845，:836 `lv_label_set_text`），全固件**无 `lv_image`**（grep 零命中）；PNG 解码未开（sdkconfig:2667-2668 `CONFIG_LV_USE_LODEPNG/LIBPNG is not set`）；THUMB **不下载**（asset_dl.c:118-128 `kind_dir()` 白名单无 THUMB，:775-778 注释「THUMB 等无目录 kind」→ return false）。设计原文 `design-review.md:100`「设备选择器三个 tab 要显示条目名称与缩略图…需加 label 字段与缩略图 asset 引用」→ `label` ✅、缩略图引用 ✅、**设备端呈现 ❌**。另：设备用的 THUMB 是 **96×96**，与需求/Web 的 64×64 不一致 |
| E7-4 | 纸娃娃 tab：预设列表（Web 编辑器存的）+ 最近使用；换装只拉新 PARTS（LAYOUT/背景不动，400-700KB，<1s） | **部分实现** | ✅ 清单来自 manifest 的 PARTS 条目（lvgl_bridge.c:471-489 → asset_dl.c:171-176 过滤 selector=paperdoll，排除 clock 与条带小包）；选中 → `MP_CMD_SET_PARTS`（:696）→ state_machine.c:572-581 **只调 `render_set_parts`**（LAYOUT/背景不动 ✅）。❌ 缺：不是「预设列表」而是本设备全部 PARTS 条目；服务端无设备预设端点（且 `GET /api/admin/presets` 在 HEAD 快照未注册，见 E4-13）；无「最近使用」排序（仅 cached-first）；体积 400-700KB 无服务端校验（实测默认装扮 PARTS 936,588 B） |
| E7-5 | 怪物/NPC tab：同上（预设+最近） | **部分实现** | 清单 lvgl_bridge.c:493-512 → asset_dl.c:203-229（selector=npc / `npc:` 前缀）。❌ 选中**只下载不切换**（`menu_activate_item(idx, MP_CMD_NONE)` :699-702；源码自述「固件无 NPC 实体渲染通道 → 本页只列/只下」:491-492，页脚 `NPC PACKS: TAP TO CACHE (NO RENDER)` :965）；无预设/最近语义 |
| E7-6 | 切换完成后宠物反应 = **随机表情** | **部分实现** | `switch_random_expression` state_machine.c:561-570（8 表情池 + `esp_random()` + 1500ms），调用点 :711（SET_MAP）/:715（SET_PARTS）。缺（只读推断，无真机）：切换都在菜单内发起且**不退出菜单**（lvgl_bridge.c:630/:777-779），此时状态仍 `MP_ST_MENU`，而 `input_trigger_expression` 在菜单期直接 return（input_dispatch.c:144-147）→ 随机表情不下发、不可见；退出菜单也不补发（POKER on_enter 只发 stand1，state_machine.c:131-136） |
| E7-7 | 素材落 TF；淘汰 = 保留最近 N 个 + 收藏 | **部分实现** | ✅ 目录与原子提交：asset_dl.c:118-128/:231-240、`verify_and_commit` :451-471、`download_one` :505-536；水位 85%/80%（:39-40）→ `evict_if_needed` :577-624。❌ 缺：(a) 保留粒度是**每个 kind 哈希桶各保最新 1 个**（:570-575，16 桶），不是「最近 N 个」；(b) **收藏保护实际失效**——`asset_dl_set_favorite`（asset_dl.c:1137-1148）全工程**零调用者**（仅 asset_dl.h:77 声明；`grep -rn asset_dl_set_favorite Firmware/ ｜ grep -v build` 只命中声明+定义），服务端 manifest 也不下发 fav |
| E7-8 | 窗口内含 BGM 入口（呼出 E6 触摸控制条） | 已实现并有证据 | 根页 "BGM" 行 lvgl_bridge.c:588 → `render_bgm_bar_request_from_menu` :683-687 → compositor.c:275 置旗标 → input_dispatch.c:1321-1328（收菜单 + `render_bgm_bar_show`）；口径见 render.h:127-130 |
| E7-9 | 第 4 个 tab「家居」（HA 面板）= P2 预留，一期不实现 | 不适用（确认不存在，符合要求） | `grep -rn '家居\|home_assistant\|homeassistant' Firmware Web/src Server` → 0 命中；页枚举 lvgl_bridge.c:70-78 与根页 :583-591 无 HA 项 |

### E8 — BGM 子系统（P0）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E8-1 | 音源：WZ 曲库（默认）+ QQ 音乐（可选，`IMusicSource` 可插拔） | 已实现并有证据 | Music/IMusicSource.cs:49-63（接口）；WzMusicSource.cs:55-56（`Name="wz"`/`IsEnabled=true`）、QqMusicSource.cs:35-36；Program.cs:27-30 双源注册 + BgmRouter.cs:49-53 收编。**跑过**：`/api/admin/music/tracks?source=wz` → `count=1167`；`/api/admin/music/sources` → `[{name:"wz",enabled:true,health:"1167 首…"},{name:"qq",enabled:false,health:"未启用…"}]` |
| E8-2 | 不同类型源；设备先选类型，切歌/上下曲/随机只在所选类型内 | **部分实现** | 服务端同源链 ✅：BgmRouter.cs:23-24（铁律）、:70-128（同源降级链 `[请求曲,下一首,再下一首]`）、:147-170（链只由 `src.ListTracksAsync` 生成，不跨源）、:126-127（耗尽抛 `BgmSourceUnavailableException`）；固件按源过滤 bgm.c:142、换源清锚点 :565-571/:675-681。**跑过**：`bgm/cmd next` 停在 wz 源内。❌ 缺：**设备端「选类型」入口不可达**——根菜单 BGM 行被拦截去呼出半屏条（lvgl_bridge.c:683-687 → `render_bgm_bar_request_from_menu()`），而含切源行的全屏 BGM 页（:719-732 构建于 :967-991）**没有任何 `menu_goto(MENU_PAGE_BGM)` 调用者**（本审计独立复核：`grep -n "MENU_PAGE_BGM\|menu_goto(" lvgl_bridge.c` → 588 是表项、688 在被 683 拦截的 else 分支）→ 全屏 BGM 页是死代码；半屏条仅 5 项 PLAY/PREV/NEXT/VOL-/VOL+（compositor.c:128/:191-193）。唯一换源通路 = Web/服务端 poll 指令 |
| E8-3 | QQ 接入：容器内 node 子进程（Rain120 网关）；只锁 128k 明文 MP3；vkey 实时取链转发（直链不下发设备）；cookie Web 导入 + 过期告警；该源可整体停用 | **部分实现** | 进程管理 ✅：QqGatewayProcess.cs:229-281（真起 `/usr/bin/node`，Dockerfile:101 装入）、:206-227（30s 巡检 + 自动重启 1 次）；协议锁 ✅：QqGatewayClient.cs:152-155（`br≠128`/非 mp3/加密 一律抛）、:157-184 + :211-234（服务端转发、直链只在本方法栈内）；cookie ✅：AdminEndpoints.cs:198-231 + QqMusicSource.cs:22/:43-49（7 天告警）+ Web MusicView.vue:118-147。**跑过**：`qq.enabled=false`、`gateway.running=false`，源对设备置灰。❌ 缺：**QQ 网关本体不在仓库**（`find . -type d -name qq-gateway`、`find . -name "*.js" -path "*qq*"` 均零命中；自述见 QqGatewayProcess.cs:19-21、QqMusicSource.cs:14-17）→ 开箱 qq 源恒 Degraded，无法端到端跑通 |
| E8-4 | 短路规则：失败只在同源内重试降级；禁止跨源自动换歌；源整体不可用→设备该源入口置灰，手动切类型 | **部分实现** | 服务端 ✅：BgmRouter.cs:67-68、:97（Recovered）、:115（SourceDown）、:119（SkipTrack）、:126-127（耗尽→异常不跨源）。固件 ✅：同源跳曲 bgm.c:448-490；置灰 bgm.c:401-411、菜单显示 lvgl_bridge.c:987-991、置灰拦截 :722-728、控制条遇置灰收起 compositor.c:304-305。❌ 缺：①置灰 UI 不可达（同 E8-2 死代码）；②置灰**不持久化**（bgm.c 内 `mp_nvs_set_u32` 只有 `bgm_vol`/`bgm_src`，重启即清）；③服务端**不向设备下发「该源已停用」**（hello 只给 `bgmDefaultSource`/`volume`：DeviceEndpoints.cs:151-160，而固件 hello 解析 http_client.c:422-439 不读这两项）→ 服务端停用 QQ 时设备入口不会预先置灰 |
| E8-5 | failover 事件上报 + 宠物 `despair` 表情 | **部分实现** | 事件链路 ✅：固件 bgm.c:401-411（`MP_EVT_BGM_FAILOVER` + `MP_EXPR_DESPAIR 2500ms`）；协议 events.c:40 `bgm_failover`；服务端 BgmRouter.cs:46-47 `Failover` 事件 + Program.cs:64-65 订阅；流失败也记（DeviceEndpoints.cs:288-293 503 + failover）；HealthReport.cs:45 白名单。❌ 缺：需求 E8 原文是「**某源**整体不可用→despair」（代码如此，单源即触发），而 E10 写「**双源整体**不可用=despair」——**双源同时判定未实现**（无跨源计数） |
| E8-6 | 控制入口：设备触摸（E7 菜单 BGM 入口 → 半屏控制条）；Web 只管曲库/歌单/cookie | **部分实现** | 设备侧 ✅：lvgl_bridge.c:683-687 请求旗标 → compositor.c:119（220px 下半屏）/ :191-193（5 控件）/ :205-240（落点）/ :243-260（呼出）；触摸命中 input_dispatch.c:695-703、长按呼出 :742、点击外部收起 :760。偏离：Web 侧存在超出需求的「设备播放控制」卡（MusicView.vue:410-502；服务端放行 AdminEndpoints.cs:282-291 / :307-317）。缺：**歌单 CRUD 无实现**（`grep "playlist\|歌单"` 仅注释命中；QQ 侧只有 `SearchKeyword` 搜索，QqMusicSource.cs:114-132） |
| E8-7 | 播放：设备 HTTP 流式拉取 → minimp3 解码 → I2S；断网 = 静音降级（不做本地曲库缓存） | 已实现并有证据（服务端跑过；固件只读） | 服务端：**跑过** `bgm/stream` 返回真 MP3（1.67MB、audio/mpeg、MPEG ADTS layer III 80kbps）。固件（只读）：bgm.c:363-375（拼 URL）、:281-331（`mp3dec_decode_frame`→pcm_ring）、:736-790（feeder→`mp_codec_write`）；**I2S 非 stub**：codec_es8311.c:115-146（`i2s_new_channel`+std 模式，MCLK=256fs/16bit）、:151-157/:219-225（PA GPIO46）；断网静音 bgm.c:934-944、:720；无 MP3 落盘。备注：codec_es8311.c:15 自述「序列仍未真机验证」 |
| E8-8 | 音量/当前源偏好存设备配置（服务端设备表同步） | **部分实现** | **跑过**：`POST /api/device/bgm/cmd {volume:40}` 后 `GET /api/admin/devices` → `"bgm":{"source":"wz","volume":40}`（服务端落设备表 DeviceEndpoints.cs:323-328）；固件 NVS `bgm_vol`（bgm.c:523）/`bgm_src`（:570/:680）。缺：**单向同步**——hello 下发的 `volume`/`bgmDefaultSource` 固件不消费（http_client.c:422-439 只读 idleToClockMin/imuDeadzoneDeg/tapLightG/tapHardG/imuSensitivity） |
| E8-9 | （需求原文）切歌/上下曲/**随机**只在所选类型内转换 | **部分实现（随机未实现）** | `grep -rn "shuffle\|随机" Server/MinipetServer/Music Firmware/main/audio/bgm.c Firmware/main/render/{compositor,lvgl_bridge}.c` → 无 BGM 命中（唯一 shuffle 字样在 minimp3 的 SSE 宏）；半屏控制条 `RC_OV_ITEMS=5` 无随机键（compositor.c:128）。→ **随机/shuffle 切歌未实现**（上下曲只在同源内有，符合前半句） |
| E8-10 | （设备端实际可切换音源）设备能起播 QQ 源 | **未实现（结构性缺口）** | 本地曲目表只含 WZ：AssetExporter.cs:199-205 `Source = 0` 硬编码 + bgm.c:142 按源过滤 → QQ 源表空；`menu_bgm_toggle`（lvgl_bridge.c:548-563）表空回退 `MP_AUDIO_PLAY(a=0)`；服务端 `HandleBgmCmd` 对 `cmd=play` **不选曲**（只有 next/prev 调 `NextTrackAsync`，DeviceEndpoints.cs:317-321）→ 回 `id:null` → 固件 bgm.c:584-590 直接 return。备注：审计期间工作树并发修了两处（`git diff` 可见）——DeviceEndpoints.cs 新增数字 `id` 字段、bgm.c 改 uint32 + `%u` 发 id（此前高位 id 变负数被服务端拒）；修完 **WZ 出声路径通**，QQ 起播仍缺「选曲」一环 |

### E9 — 时间功能（P0）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E9-1 | 待机时钟：无人交互 N 分钟 → 宠物睡眠态 + WZ 数字时钟浮现 | **部分实现**（睡眠宠物已补，见争议 4） | 闲置超时：state_machine.c:535（`idle_ms >= limit_ms`）→ :448-453（IDLE_TIMEOUT→CLOCK_DOZE）；默认 5 分钟 = ConfigService.cs:72 / app_core.h:200（Web SettingsView.vue:245 可改，hello 下发 http_client.c:425-426）；数字时钟：clock_digits.c（13 字形 0-9/am/pm/comma，:27）。**睡眠宠物**：compositor.c:946-952（CLOCK_DOZE 分支 = 整脏区 memset 纯黑 → `clock_digits_compose` → `ent_compose(..., RC_SLEEP_DARKEN)`），`RC_SLEEP_DARKEN 35`（compositor.h:83），降亮实现 compositor.c:903-928（RGB565 各通道 ×35/100）。判断详见 §5 争议 4 |
| E9-2 | fontTime 参数：AM/PM + HH:MM，起点 = clock+(18+3,83)，AMPM_GAP=12px，comma 偶显奇隐，12h 制、午夜 AM 00:xx | 已实现并有证据 | clock_digits.h:5（起点 = 锚点 +(18+3,83)）、:31-33（`CLOCK_OFF_X=21`/`CLOCK_OFF_Y=83`/`AMPM_GAP=12`）；clock_digits.c:279-280 `x = anchor_wx + CLOCK_OFF_X; y0 = anchor_wy + CLOCK_OFF_Y`；`:227-232` 12h（13..23 减 12，0 保持 0 = 午夜 AM 00）；`:232`/`:256` `parity = tm_sec & 1`（偶秒显 comma）；`:283-297` 排布 `am｜pm → GAP=12 → H1 H2 → comma → M1 M2`；与 docs/ai/clock-display-spec.md:24-26/:40-43 逐条一致 |
| E9-3 | 时间源：PCF85063 RTC（配网时 NTP 校准一次，断网独立走时） | **部分实现**（含一处只读推断缺陷） | 链路 ✅：rtc_pcf85063.c:60-107（BCD/OS 标志/UTC 口径）、hal_contract.h:76-83；main.c:213 `rtc_pcf85063_init()`；provision.c:824-865（`ntp.aliyun.com`，15s 等待→写 RTC，配网成功路径调用）+ :714-736（开机用 RTC 种子系统时钟）+ :877-935（常态化 6h 重校 / 无效时间快试）；读时链路 clock_digits.c:185-206（sys≥2020 优先 → RTC → `--:--`）。**推断缺陷（未上机，需真机复验）**：`sntp_and_set_rtc()` 在 provision.c:843 `localtime_r(&now,&tm_now)` **早于** :849 `setenv("TZ","CST-8",1)`——只在同一 boot 的**首次**调用成立；6h 周期重校（:911-914 调用同一函数）或无效时间重试（:928）时 TZ 已持久生效 → 写进 RTC 的「UTC 日历」实为东八区字段（+8h），重启后 RTC 种子再被 clock_digits.c 固定 +8h → 显示快 8 小时。clock_digits.c:159-162 的注释正建立在「全工程仅此一处 TZ 设置」这一假设上（`grep -rn setenv\|tzset Firmware/main` 确认仅 provision.c:849-850） |
| E9-4 | 地图时钟：设备**不解析 WZ clock 配置**；位置为魔法值表（Web 可改） | 已实现并有证据 | 服务端表：`Server/seed/clock_table.json` + ClockTableSeeder.cs:50-64（**服务端**读 WZ `clock/x,y` 换算成烘焙视口坐标写入 `Clock.MapOffsets`）+ ConfigService.cs:94；manifest 下发 DeviceManifestService.cs:233-239；**跑过**：`clock_table:{"200000100":[240,178],"220000100":[240,240],…}`。固件：asset_dl.c:305-321 解析 + :1119-1135 按 `active_map` 查表；state_machine.c:601-607/:615-634。Web 可改 ✅（SettingsView.vue:338 高级 JSON 编辑 → rev+1）。**固件零 WZ 解析**：`grep -rn "clock/x\|\.img\|Sound.wz\|ExtractSound" Firmware/main` 零命中 |
| E9-5 | 交互（触摸/按键/IMU）→ 立即唤醒回桌宠态 | 已实现并有证据 | state_machine.c 任意交互事件 → POKER（`MP_SM_EV_*` 分支 + `s_last_activity_ms` 刷新）；按键长按/短按 :394-427；`clock_digits_active()` 在渲染层随状态开关（compositor.c:946/:1485） |
| E9-6 | 番茄钟/倒计时系：不做 | 不适用（确认未实现，符合要求） | `grep -rni "pomodoro\|番茄\|countdown" Firmware/main Server/MinipetServer Web/src` 仅在 clock-display-spec.md 留档；无实现 |

### E10 — 表情系统（P0）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E10-1 | 25 个实证表情完整落地（不增不减） | **部分实现（固件侧多一个死宏）** | 权威 25 项：PaperdollService.cs:63-74；导出侧 25 槽 ✅（E1-10）；Web 侧 25 项（expressions.js:22-47）。**跑过**：LAYOUT `expression_count=25` + PARTS face 家族 25 连号槽。缺：**固件具名宏 26 个**（`grep -c "^#define MP_EXPR_" Firmware/main/app/app_core.h` → 26）——多出 `MP_EXPR_ALERT "alert"`（app_core.h:60，与动作名冲突），全工程**零使用**（本审计复核：仅定义行命中）；app_core.h:52 注释自述「25 表情名」与实际不符 |
| E10-2 | 触发型表情播完自动回 default | **部分实现** | 触发器路径 ✅：input_dispatch.c:140-153（带 duration）+ :155-167（FSM 到期回 default）。缺：**系统事件路径不自动回**——state_machine.c:727（cheers）、:751（dam）等直接 `render_set_expression()`，无时长无定时器；且布局重绑三处 `reset_expr` 恒 false（compositor.c:722-728 定义，:1426/:1627/:1763 调用）→ 重绑也不复位 |
| E10-3 | blink：设备本地随机 3~8s 循环 | 已实现并有证据 | 同 E5-9（entity_anim.h:19-21 MIN 3000/MAX 8000/HOLD 250；entity_anim.c:86-91 `esp_random()`；:126-137 插播状态机；:133-134 当前非 blink 才插）；与网络无关 |
| E10-4 | 系统事件映射：BGM 播放=hum / 低电=**troubled** / 配对成功=cheers / 素材故障=dam / 双源不可用=despair / 过温=hot | **部分实现** | hum：bgm.c:596 ✅；troubled：input_dispatch.c:1192-1196（`pct<=20` → TROUBLED，注释「despair 留给 BGM failover」）✅；hot：input_dispatch.c:1202-1208 ✅；dam：asset_dl.c:530-532 ✅；despair：bgm.c:410（全仓唯一触发点）✅。与 requirements-analysis.md:149-157 的 2026-09-27 定稿**一致（低电=troubled，低电路径无 despair）**。缺：①**「双源整体不可用=despair」未实现**（单源 failover 即触发，无跨源判定）；②cheers 触发点是「**屏显 6 位配对码**」（state_machine.c:724-728 + http_client.c:441-449）而非 Web 完成绑定之后 |
| E10-5 | 随机轮播：静置时低频播稀有表情（wink/chu/qBlue） | 已实现并有证据 | input_dispatch.c:169-184（idle≥60s、30s 滚动窗口、15% 概率、`rare[]=WINK/CHU/QBLUE`、1600ms）、调度点 :1213；顶部注释 :15 |
| E10-6 | 表情切换 = 只重合成 face 类部件 | **部分实现** | face 变体替换语义正确（compositor.c:732-758，`:739 expr_index!=EXPR_NONE && expr_group!=0` → `mpak_parts_variant`），但 `render_set_expression`（:1633-1643）走 `recompose_entity()` 整画布 memset+逐件重画（:790-793）→ 功能对、优化语义未落地（同 E5-2） |
| E10-7 | 来源约束：25 表情之外不自创；缺素材回退 default（导出器保证） | 已实现并有证据 | 固件只引用宏名（app_core.h:56-85）；导出端补齐 25 槽并对缺帧槽回退 default（AssetExporter.cs:472-492 `slots[defaultIdx] ?? fallback`，ShareDataOf 去重）；固件端再兜一层（entity_anim.c:93-111 查不到→再查 "default"→都没有才 index 0）。**跑过**：25 槽全部存在 |

### E11 — 降级与容错（P0）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E11-1 | 无网络：TF 缓存独立运行（宠物照常动、blink 照常眨）；BGM 静音降级；选择器只显本地缓存项 | **部分实现** | ✅ state_machine.c:158-170（OFFLINE：TF 缓存继续跑 + blink 本地循环）；blink 纯本地（E5-9）；BGM 离线不呼出/静音（input_dispatch.c:741-745）。⚠️「选择器只显本地缓存项」实现为**未缓存项置灰**而非隐藏（同 E7-2） |
| E11-2 | 回网自动重连并同步（manifest 补拉） | 已实现并有证据 | poller.c:352-418（退避 + hello 补发）、:361 `MP_SM_EV_NET_ONLINE`；state_machine.c:439-453（ONLINE 分支 → 补报离线事件 + manifest 同步）；asset_dl `sync_once`（:718-728 按 manifest diff 下载） |
| E11-3 | 服务端重启/升级：长轮询指数退避 1s→2s→…→60s 封顶 | 已实现并有证据 | poller.c:37-38（`BACKOFF_MIN_MS 1000` / `BACKOFF_MAX_MS 60000`）、:385-386/:408-409/:417-418（倍增 + 封顶）、:392（成功复位）；:64 不可达分支另设 5s 封顶（裸 socket 探针，属实现优化） |
| E11-4 | 素材包损坏：hash 校验失败 → 弃用重拉；宠物播 dam 表情 | 已实现并有证据 | asset_dl.c:382-471（流式 crc32c + MAGIC/长度/尾部 crc 校验，:457-460 比对）、:530-533（损坏 → `unlink(tmp)` + `MP_EVT_ASSET_ERROR` + `MP_EXPR_DAM` + 重拉一次）；校验顺序与 mpak.c:5-6 一致 |
| E11-5 | TF 卡满：按淘汰策略清（保最近 N + 收藏） | **部分实现** | 水位与 LRU：asset_dl.c:39-40（85% 触发 / 清到 80%）、:577-624 `evict_if_needed`。缺：粒度是「每 kind 桶各保最新 1」而非「最近 N 个」；**收藏保护是死代码**（同 E7-7） |
| E11-6 | 低电分级：<20% troubled → <10% 强制睡眠（纯时钟态） | 已实现并有证据 | input_dispatch.c:1188-1196（`pct<=10` → `MP_SM_EV_BATTERY_CRIT`；`pct<=20` → TROUBLED + `MP_EVT_BATTERY_LOW`；`pct>25` 复位上报标志）；state_machine.c:495-504（CRIT → `s_force_sleep=true`，充电解除）、:523-524（强制进 CLOCK_DOZE）。**跑过**：`POST /api/device/event {type:"battery",data:{a:15}}` → Web/health 显示 `batteryPercent:15` |
| E11-7 | OTA：设备直接走 WiFi OTA（poll 指令含版本+下载地址），双分区 + 失败回滚 | 已实现并有证据（固件只读） | ota.c:82-113（`esp_ota_get_next_update_partition` → `esp_ota_begin/write/end` → `esp_ota_set_boot_partition`）、:127（失败不切 boot = 等效回滚）、:164-167（`esp_ota_mark_app_valid_cancel_rollback`）；指令侧 `mp_ota_offer`（:155）+ poller 解析 `{ver,url}`（poller.c:249-250）；服务端 `POST /api/admin/ota/{id}` + `GET /api/device/firmware/{ver}.bin`（DeviceEndpoints.cs:39）。**跑过**：`GET /api/device/firmware/0.0.0.bin` → 404（data/firmware 无产物，端点存在） |
| E11-8 | 所有降级事件上报服务端（Web 可见设备健康） | 已实现并有证据 | **跑过**：`POST /api/device/event {type:error,data:{s:"net_offline"}}` → `GET /api/admin/devices/dev-9102b2/health` → `{"byType":{"battery":1,"error":1},"lastError":"net_offline","batteryPercent":15,"recent":[…]}`；`GET /api/admin/logs/{id}` 事件环形日志含 BGM/事件行 |
| E11-9 | 离线态进出事件名（争议点 5） | 已实现并有证据（与需求修订文本一致） | 需求 requirements-analysis.md:167-171 **自己写明**「暂复用已映射的 `MP_EVT_ERROR` + `s="net_offline"/"net_online"`」。固件实现正是如此：state_machine.c:52-72（`post_net_event` 组 `MP_EVT_ERROR` + `s`）、:168-169（离线）、:449-453（回网 + 按原始 ts 补报）；events.c:42 `MP_EVT_ERROR → "error"`，`s` 随 `data` 原样上报。**跑过**：服务端 health 的 `lastError:"net_offline"` 即来自 `data.s`（HealthReport.cs:48-52 专门把 net_online 排除出「最近异常」）。备注：events.c **仍无**专用 `net_offline`/`net_online` 事件名（需求也标注「待切换」） |

### E12 — 字体与文本（P0）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E12-1 | 字体：宋体（SimSun 系），`lv_font_conv` 预转位图 bin（4bpp），随素材包下发缓存 TF | **部分实现**（字体族对齐、具体来源非 SimSun） | 工具链 scripts/fontpack/（build-fonts.sh:17-19 取 `Songti.ttc` index 6、:52-59 `--bpp 4 --format dump`、:64 SRC_DESC）；种子 `Server/seed/fonts/font-{16,24,32}.json` 元数据：`"bpp":4`、`"source":"Songti.ttc#6(Songti SC Regular) via lv_font_conv 1.5.3"`；打包 FontPackWriter.cs:46-72；运行期 FontPackService.cs:48-113（种子优先，缺种子回退 Skia 仅 ASCII）；落盘 asset_dl.c:125（FONT→`/minipet/font`）。**跑过**：manifest 含 3 个 FONT 包（`字体 16/24/32px`，`size` 为 number）。缺：源字体是 macOS **Songti SC** 而非 Windows **SimSun**（同属宋体族；README:52-58 给了换回 SimSun 的流程），需求字面写「SimSun 系」→ 记为平台替代 |
| E12-2 | 字符集：ASCII + 3500 GB2312 一级 + 标点；24px 主用、16px 辅助、32px 可选 | **部分实现（字号分工有偏差）** | 字符集 ✅：README:1-20「ASCII 95 + GB2312 一级 3755 + 标点 42 = 3892」（16/24px）；32px 子集 666。**跑过**：charset-16.txt 3891 字符、charset-32.txt 665；font-16/24.json `glyph_count:3892`、font-32.json `666`；三档包均进 manifest（FontPackService.cs:33）。缺：**菜单列表项用 24px、16px 只用于 hint/status 小字**（lvgl_bridge.c:343-345 `f_title=32/f_item=24/f_small=16`），与「16px 辅助（列表/状态）」字面不完全一致；气泡固定 24（state_machine.c:705）✅ |
| E12-3 | 气泡文字：设备端 LVGL label 渲染，协议传 UTF-8 文本（不传位图） | 已实现并有证据 | 协议：`MP_CMD_BUBBLE s=UTF-8 文本`（app_core.h:136，s[96]≈31 汉字）、poller.c:65-68；AdminEndpoints.cs:257-278（bubble ≤95B 校验）；固件 `render_bubble_show(cmd->s, RENDER_FONT_24)`（state_machine.c:695-705）；离屏 LVGL label 渲染 lvgl_bridge.c:1305-1341。**跑过**：`POST .../command {type:"bubble",value:"审计测试气泡"}` → poll 返回 `payload:"审计测试气泡"`（UTF-8 原样） |
| E12-4 | 断行按 LVGL 自带 + 桌面版参数等比（wordWrap 90px→480 屏，行高 16→等比） | **部分实现** | 桌面版权威参数 BalloonService.cs:173/:184（`wrapW=90`、`lineHeight=16f`）。缺：固件**未用 LVGL 自带 wrap**——是**自研贪心换行**（lvgl_bridge.c:1221-1271 先产显式 `\n` 再喂 label，:1322 设 label 宽度）；气泡宽度 `RC_BUBBLE_MAX_W=460`（compositor.h:64）与桌面 90px/行高 16 **不是同一公式的等比换算**（无换算常量可查） |
| E12-5 | 随机台词：静置久了冒预设台词气泡，文本 Web 配置 | 已实现并有证据 | SpeechScheduler.cs:93-133（在线 + LastSeen 静置≥idleSec + 间隔≥120s + 随机且避免连发）、:138-175（>95B 跳过）、:119 入队 bubble → poller.c:65-68 → state_machine.c:695-705；配置 SpeechConfig（ConfigService.cs:79-86）+ Web SettingsView.vue:280-330（启停/idleSec/逐行台词/≤50 条/单条 ≤95B）。**跑过**：`GET /api/admin/settings` 返回 `config.speech` 在位 |

### E13 — 设备管理（P0）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E13-1 | 首次开机 → hello 上报 profile+UUID → 服务端入册 → 屏显 6 位配对码 → Web 输入配对码完成绑定（命名） | 已实现并有证据 | **跑过**：hello → `deviceId:"dev-9102b2", paired:false, pairingCode:"942576"`（6 位）；`GET /api/admin/devices` → 该设备在册；AdminEndpoints.cs:93 `POST /pair`（PairRequest:545-549）；固件 `MP_CMD_PAIRING_CODE` → 气泡 + cheers（state_machine.c:723-728） |
| E13-2 | 设备表 `deviceId → {profile, petConfig, bgm偏好, 阈值配置}`，JSON 存 data/（不引入数据库） | 已实现并有证据 | DeviceRegistry.cs:9（表结构注释）、:34 `PetConfig`；ServerPaths.cs:19 `DevicesFile`；**跑过**：`/api/admin/devices` 返回 `profile{w,h,shape,psram,audio}` + `bgm{source,volume}` + `health`；全部落 `data/*.json`（无 DB 依赖） |
| E13-3 | 每设备绑一宠 + 纸娃娃配置各不相同；manifest 按设备隔离下发各自部件包；A 换装不影响 B | 已实现并有证据 | DeviceRegistry.cs:9「petConfig（装扮 JSON，按设备隔离）」；导出目录按设备 `ServerPaths.ExportDirFor(deviceId)` → `data/cache/export/{deviceId}/`；DeviceManifestService.cs:12（assets 从该设备目录读）、:66-78（**每设备 rev**）。**跑过**：两个设备（dev-9102b2 / dev-b8ad53）各自独立 export 目录与 manifest |
| E13-4 | 曲库共享、播放状态独立 | 已实现并有证据 | 曲库全局（WzMusicSource/Wz 目录，无设备维度）；播放状态按设备：**跑过** bgm 偏好按设备存（`devices[].bgm`），`bgm/cmd` 带 deviceId（DeviceEndpoints.cs:66-73） |
| E13-5 | Web 设备卡片：在线状态/换宠换装/阈值下发/固件版本与 OTA 触发 | **部分实现** | 在线状态 ✅（DashboardView.vue:109）+ 固件版本 ✅（:126，5s 轮询 devices.js）、换宠换装 ✅（DashboardView.vue:135 + DeviceDetailView.vue:88-158）、阈值 UI+落库 ✅（DeviceDetailView.vue:160-187/:557-613 + AdminEndpoints.cs:32-50）、OTA 触发 ✅（DeviceDetailView.vue:241-259 + AdminEndpoints.cs:398-411）。缺：①**阈值「下发」非实时**——设备端只在 hello 响应 `config` 里取值（http_client.c:418-435），协议**无阈值/config 指令**（`grep -rnE 'Enqueue\([^)]*"(config｜threshold｜settings)' Server/MinipetServer` 零命中），改阈值要等下一次 hello（重启，或连续 3 次 poll 真失败后重 hello，poller.c:377-386）；②OTA 无产物（`data/firmware` 空 → 设备拉 404），端到端未验证；③OTA 按钮在设备详情页而非首页卡片 |
| E13-6 | 未配对设备：可正常跑（匿名模式，本地默认宠物），配对只解锁 Web 管理 | 已实现并有证据 | **跑过**：hello 不带任何配对信息 → `paired:false` 但返回完整 config+manifestUrl+pollUrl（可正常拉素材/指令）；DeviceEndpoints.cs:96 注释「匿名可用（配对只解锁 Web 管理，E13）」；固件有 default_appearance.h 本地默认装扮 |
| E13-7 | 开发基线 = 单设备；冰箱贴等后续机型 = 新 profile 入库即接入，零协议改动 | 已实现并有证据 | **跑过（关键）**：以 profile `w=320,h=240,shape=square` 注册第二台设备 → 推送地图 200000100 → 导出 BGMAP 包解出 `vw=320 vh=240`（payload 316,912B）；对照 480×480 设备同地图为 `vw=480 vh=480`。代码路径：DeviceAssetService.cs:70-73 `_reg.Get(deviceId)?.Profile` → `ToExportProfile()` → `ExportMapAssets(mapId, warnings, profile)`；AssetExporter.cs:798-807；ExportMap 用 `profile.ViewportW/H`（:582-587）→ **设备上报 profile 真的参与烘焙**（该修复属 commit 67ba3fd，需 ≥14:50:26 的构建才含） |

### E14 — 支撑设施（P1）

| 需求编号 | 要点 | 状态 | 证据 |
|---|---|---|---|
| E14-1 | 首次配网：SoftAP captive portal（设备热点→手机弹配网页→填 WiFi + 服务器地址预填 `http://<NAS_IP>:38090`）；NTP 校时一次 | **部分实现** | portal ✅：provision.c:2-16（三件套）、:295-312（DNS:53 劫持应答 192.168.4.1）、:358-368（未知道路 302）、:388-463（AP 扫描）、:465-510（保存并连接）、:151-201（配网页）；NTP ✅（provision.c:824-865，`ntp.aliyun.com` 15s→PCF85063，配网成功路径调用 :1212；另加 6h 重校/开机快校 :877-935，超出需求）。缺：**地址「预填」只有 placeholder**（provision.c:165-169 无 value 预填）；用户留空则静默回落硬编码 `http://192.168.1.100:38090`（:491-493）；mDNS 发现结果明确不写 NVS（mdns_discover.h:6-11）→ 真实服务器地址不会被带出 |
| E14-2 | OTA（E11 已定）：WiFi 直接升级 + 双分区回滚 | 已实现并有证据（固件只读） | ota.c:72-132（`esp_ota_get_next_update_partition`/begin/write/end/set_boot_partition；失败不切分区=等效回滚 :125-131）、:164-168（`esp_ota_mark_app_valid_cancel_rollback`）；分区表 `Firmware/partitions.csv:5-8`（otadata + ota_0 + ota_1，无 factory）；`sdkconfig.defaults:58 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`（生成 sdkconfig:420）；确认有效点 = 首次 NET_ONLINE（state_machine.c:455-459）；服务端 AdminEndpoints.cs:398-411 + DeviceEndpoints.cs:339-346。**跑过**：`GET /api/device/firmware/0.0.0.bin` → 404（端点存在、无产物） |
| E14-3 | **mDNS：设备可发现服务端（可选启用）**（争议点 7） | **部分实现** | 设备侧 ✅：mdns_discover.c:61-103（`mdns_query_ptr` 查 `_minipet._tcp`，PTR 1.1s + 必要时按 hostname 补 A 1.1s，用完 `mdns_free`）；服务名 mdns_discover.h:29-30；调用点 state_machine.c:205-211/:259-290（srv_url 空或 hello 失败才查一次，手输优先）；依赖 `main/idf_component.yml` `espressif/mdns ^1.2.2`。❌ **服务端零实现**：`grep -rniI "mdns\|_minipet\._tcp\|zeroconf" Server --exclude-dir=bin --exclude-dir=obj` → 无输出(exit 1)；`grep -rniI "avahi\|dnssd\|bonjour\|makaretu\|Tmds" Server` → 无输出；csproj 仅 SkiaSharp×2 + System.IO.Hashing；部署件无 UDP 5353 放行、compose 无 `network_mode: host`（docker-compose.example.yml:16-18）。仓库文档自证：keys-touch-handoff.md:197-199「固件已实现，服务端未实现」+ :220-236 四条实现要求。→ 设备会查、局域网无应答者，mDNS 兜底**实际不可用** |
| E14-4 | 日志：设备环形日志缓冲，Web 可拉取；串口仅开发期 | **部分实现** | 固件 ✅：logbuf.c:1-35（PSRAM 零内部堆、不阻塞/不递归）、:49-50（152 槽 ≈32.8KB）、h:1-19；上行 http_client.c:508-560（20s 心跳、E 级立即、失败 60s 退避）；服务端 DeviceLogStore.cs:25（512 行/设备）+ AdminEndpoints.cs:380-395/:660-698；Web 卡片 DeviceDetailView.vue:190-239。**跑过**：上报 1 条 → `{"ok":true,"accepted":1,"skipped":0,"lastSeq":1}`；`GET /api/admin/device-logs/dev-9102b2?sinceSeq=0` → `{"lastSeq":1,"clockSynced":true,"items":[{"seq":1,"lvl":"I","tag":"audit","msg":"hello"}]}`。缺：环缓冲经 `esp_log_set_vprintf` 挂接是「**同时进串口与环**」（logbuf.h:9），发布版未关串口输出，与「串口日志仅开发期」字面略有出入 |
| E14-5 | 看门狗：渲染卡死→自动重启；**连续 >3 次仍卡死 → 停止渲染 + 纯文本错误 + 关屏待机**；计数器持久化 NVS，稳定运行后清零 | 已实现并有证据 | watchdog.c:2-27（设计注释）、:48-56（5s 超时 / 3 振 / 稳定 10min 清零 / 文本 15s / 20s 启动宽限）、:114-124（NVS 键 `wdt_hit` 读写）、:129-166（熔断：黑底 5x7 文本→`vTaskSuspend` 渲染任务→关屏→挂起自身；文本走 `display_fill_rect` 独立通道 :75-109，不进渲染路径）、:171-223（前两振 `esp_restart()`，第 3 振熔断）、:228-280（复位原因判定：用户复位给 1 次机会，软件重启/panic/TWDT/brownout → 熔断锁定）、:282-316（`watchdog_render_allowed()` 渲染闸门 + 喂狗）。与需求逐条吻合（只读，未真机复验） |
| E14-6 | 开机自检：WiFi/内存/TF 卡/服务端连通性，失败进对应降级态 | 已实现并有证据 | state_machine.c:218-320：PSRAM（`state_machine_boot` :333-334 要求 ≥4MB）→ TF → WiFi 配置 → WiFi 连接 20s → hello；失败分流 FATAL 文本（:223-234）/ WIFI_PROVISION（:247-249）/ OFFLINE（:292-319，仅「无本地素材」才 FATAL :294-300）；状态枚举 state_machine.h:27-35（BOOT/SELF_TEST/WIFI_PROVISION/POKER/MENU/CLOCK_DOZE/OFFLINE/OTA/FATAL）；回网由 poller 驱动（poller.c:331-393） |
| E14-7 | 开源就绪：README 三步部署；默认值零私人信息 | **部分实现** | README 三步部署 ✅（README.md:22-40「放 WZ → 起 compose → 浏览器配置」，另 :51-61 烧录与配网）。脱敏 ✅：`.env.example`（仅 MINIPET_PORT=38090）、docker-compose.example.yml（`ghcr.io/<owner>/minipet`、`<WZ_DATA_HOST_PATH>`）、deploy-nas.sh:14-19（`<NAS_IP>` 等占位 + 守卫）、appsettings.json（`/wz/Data`、Cookie 空）、ci.yml:101（`${{ github.repository }}`）。❌ 残留真实私网信息（`git grep -n "192\.168\.3\.46"`）：`Firmware/main/net/http_client.c:164`（**跟踪文件**，裸 socket 探针硬编码开发网段真实 NAS IP）、`Server/tools/rebuild-flash-assets.sh:13,46`、`Web/scripts/preview-with-live-api.mjs:11,44`（默认值）、docs/ai/keys-touch-handoff.md:388,394,441；另有 `/Volumes/SSD`+用户名残留在 docs 与测试夹具（同 E3-10） |

> **审计收尾补记（15:1x）**：E14-3（服务端 mDNS）在审计过程中已被并行修复——**审计结束时工作树出现未提交的 `Server/MinipetServer/Services/MdnsAdvertiser.cs`（15:09 创建，~33KB，自研 Socket 组播 + Makaretu.Dns 报文编解码）、`MinipetServer.csproj` 新增 `Makaretu.Dns 2.0.1`、`Program.cs` 注册并在 `ApplicationStarted` 启动广告**（审计方只读查看，未验证其是否真的能应答 `dns-sd -B _minipet._tcp`）。因此本报告对该条的判定按**审计时快照（Server 内零 mDNS 代码）**记为「部分实现」，实际状态需在重建+真机发现成功后才算闭环（建议用 §3 第 1 条的验证命令复核）。同类情形还有：`AdminEndpoints.cs` presets 路由（`69fe230` 已修）、`DeviceEndpoints.cs` bgm id 字段与 `bgm.c` u32 截断（工作树已修）。

---

## 2. 「部分实现」清单（缺的具体是什么）

| # | 需求点 | 缺的具体内容 | 补的落点 |
|---|---|---|---|
| 1 | **E1-12 / E5-6 / E1 争议 1** | **`GET /api/device/manifest` 下发的 `scale`/`bottomMargin` 是字符串（`"2"`/`"40"`），不是 number**。CLI 导出器（ManifestBuilder）已修，但设备真实链路走的 `PaperdollPackService.EntryOf`（PaperdollPackService.cs:142-167，:160）与 `DeviceAssetService.EntryOf`（DeviceAssetService.cs:174-193，:192）是**未同步修复的复制品**；固件侧则完全不读这两个字段（用编译期 `RC_SCALE`/`RC_ENT_MARGIN_B`） | 把 ManifestBuilder.cs:52-58 的 4 个数值分支抄进那两份 EntryOf（或抽成公共 `AssetEntryJson`），再加一条「server manifest 数值字段必须是 JSON number」的测试 |
| 2 | E1-4 | 无 `System.Threading.Channels`；EventBus 改成了同步 `Publish`（EventBus.cs:92-99） | 若确需异步：`Channel<Event>` + 单消费者任务；若同步发布已够用，改需求文案即可（不必改码） |
| 3 | E1-9 | 测试夹具硬编码 `/Volumes/SSD/mxd/mxd/Data`（WzFixture.cs:14、WzTestHarness.cs:14） | 去掉默认值，仅从 `MINIPET_TEST_WZ` 读；缺则 Skip（现在已有 Skip 逻辑，只差删默认串） |
| 4 | E2-2 | manifest **没有 BGM 曲目清单**（assets 只有 PARTS/LAYOUT/BGMAP/FONT/THUMB） | 要么在 manifest 加 `bgm:{tracks:[…]}`（或 AUDIO_META 包），要么把 E2 文案改成「BGM 走 `/api/device/bgm/*` 实时取」 |
| 5 | E3-10 / E14-7 | docs 与测试里残留真实用户名/本机路径（technical-reference.md、selftest-report-2026-09-27.md、Server/T1-T4-server-report.md、两个测试夹具） | 统一替换为 `<WZ_DATA_HOST_PATH>`/`<USER>` 占位符；CI 加 `git grep -E "a502\|/Users/"` 门禁 |
| 6 | E3-13 / E3-14 / E3-16 | SkiaSharp 仅「库存在 + 无加载失败」断言，无容器内真实渲染用例；`IRenderBackend`/ImageSharp fallback **完全没写**；homes 挂载权限等 5 项验证清单未做 | CI 冒烟容器挂一小份 WZ fixture，断言 `/api/admin/thumb` 返回真 PNG；fallback 按 software-design.md:190 保留为风险项（本期可不做，但需求写进了 E3 验证清单） |
| 7 | **E4-13 / E7-4** | `GET /api/admin/presets` 在 HEAD 快照**未注册**（AdminEndpoints.cs:122 注释与代码同行被 `//` 吞掉）→ Web 纸娃娃预设列表打不开；设备侧也没有「预设列表」端点，选择器看到的是本设备全部 PARTS 条目 | 该行已由其他 agent 在工作树拆行修复（未提交）；需**重建镜像**并在 CI 加一条 `GET /api/admin/presets` 冒烟断言（这类「注释吞代码」只有端点级测试能拦住） |
| 8 | E4-23 / E7-2 | 服务端**没有**给设备下发「地图预设 + 最近 + 收藏」的端点，设备选择器只能列 manifest 里已有条目；收藏只服务 Web；离线时「只显缓存项」实现为置灰；服务端算出的 `cached` 字段固件不读（asset_dl.c:474-503 `upsert_meta` 不收 cached） | ①加 `/api/device/selectors/maps`（预设+最近+收藏，含 `cached`、`thumb`）或把 fav/最近的写入路径接上；②固件 `upsert_meta` 增加 `cached` 字段消费（服务端已算好） |
| 9 | E4-25 / E8-6 | Web 有超出需求的「设备播放控制」卡（MusicView.vue:410-502），与「BGM 控制在设备上」的定稿口径冲突 | 产品决策：撤回该卡，或把 E4/E8 文案改成「Web 可远程下发传输控制（预留）」 |
| 10 | E5-2 / E10-6 | 表情切换走整画布重合成（compositor.c:1633-1643 → `recompose_entity()`），无「只重画 face 件」增量 | 在 `recompose_entity` 加 face-only 分支（只清 face 家族的 piece 矩形并重 blit），或改需求为「表情切换 = 重合成（含 face 替换）」 |
| 11 | E5-5 | 只有**单级**回退（一次性动作 → 最近一次 loop 布局）；stand1 缺失时保留旧画面，没有「stand1→walk1→default」多级链 | 在 `active_layout()`/`bind_active_layout()` 里按优先级列表逐个找（stand1 → stand2 → walk1 → 任一 LAYOUT） |
| 12 | E5-8 | 有脏区机制但**无带宽核算/限流**；所有脏格合并为**单一包围盒**，条带动画时实际上传 = 整条带（全宽）∪宠物，远大于「宠物区 30fps」口径 | ①按行/带分段 flush（条带与宠物分两次 bbox）；②加帧级字节统计（`dirty_bytes`）并真机测 QSPI 占用，把「≤31%」变成可验证指标 |
| 13 | E6-1 | profile 只有单个 `key.menu` 引脚，**没有键位映射表**；**Web 改键零实现** | profile 加 `keys[] {role, gpio}`；Web 设备详情加键位下拉；服务端存设备表并随 hello 下发 |
| 14 | E6-4 | 轻点无「宠物区」命中测试（POKER 全屏任意轻点都算抚摸）；抚摸气泡固定英文 "hello"，不走 E12 台词配置 | 用 `ent_screen_rect_at()` 加命中判断；气泡文本改走 speech lines |
| 15 | E6-9 | `tilt_debounce_ms` / SHAKE_* / PICKUP_* / TAP_MAX_MS / LONGPRESS_MS 不可配 | 在 `Device` 配置段加对应键并随 hello 下发（服务端与 Web 表单同步加字段） |
| 16 | **E7-3** | 设备端**不显示缩略图**：菜单只有文本行、无 `lv_image`、PNG 解码未开（sdkconfig:2667-2668）、THUMB 包不下载（asset_dl.c:118-128/:775-778）；且设备用 THUMB 是 96×96 而需求/Web 是 64×64 | 最小闭环：①服务端 THUMB 改 64×64（或按 profile）；②固件 `kind_dir` 加 `THUMB` 目录 + 开 `CONFIG_LV_USE_LODEPNG`；③菜单行加 `lv_image`（THUMB 是裸 PNG，需按 PNG 直读而非 MPAK 校验路径） |
| 17 | E7-5 | NPC 页只能下载到 TF、**不能切换显示**（固件无 NPC 实体渲染通道；`MP_CMD_NONE`） | 要么复用纸娃娃实体通道渲染 NPC（PARTS+LAYOUT 同构），要么把 E7 NPC tab 的验收口径改成「只缓存」 |
| 18 | E7-6 | 切换后的随机表情被**菜单期表情冻结守卫**吞掉（input_dispatch.c:144-147；切换不退出菜单） | 在菜单内改成「记 pending，退出菜单后播」，或切换后自动退出菜单 |
| 19 | E7-7 / E11-5 | 淘汰粒度是「每 kind 桶各保最新 1 个」而非「最近 N 个」；**收藏保护是死代码**（`asset_dl_set_favorite` 零调用者，fav 无写入路径，服务端也不下发） | ①改 LRU 为按 kind 的最近 N 条（N 可配）；②接通收藏：Web/设备加收藏动作 → 服务端 manifest 下发 `fav` → 固件 `upsert_meta` 收字段 → 淘汰时跳过 |
| 20 | E9-6 旁 | 真机时钟 `--:--`（RTC 未同步）缺口在审计期仍被其他 agent 修复中 | 以 selftest-report-2026-09-27.md 的 T 系列为准复核 |
| 21 | E12-1 | 字体源是 macOS **Songti SC** 而非 Windows SimSun（需求字面「SimSun 系」） | 若要字面合规：按 scripts/fontpack/README.md:52-55 用 SimSun.ttc 重跑 `./build-fonts.sh`（产物覆盖 seed 即可） |
| 22 | E12-4 | 未见显式的「wordWrap 90px → 480 屏」等比换算常量，实际靠 LVGL 自动断行 | 若要与桌面版逐像素一致，需在固件给定 wrap 宽度（如 24px 字体 × 宽上限）并对齐 BalloonService.cs:184-185 |
| 23 | E13-5 | OTA 链路无产物（`data/firmware/latest.json` 缺失 → 设备拉 404），Web 触发按钮存在但端到端未验证 | 补一份固件产物到 `data/firmware/` + `latest.json`，跑一次「Web 触发 → poll 收指令 → 设备拉 bin」 |
| 24 | **E14-3** | **服务端不广告 `_minipet._tcp`**（Server 内 mDNS/zeroconf/bonjour 相关 grep 零命中、无 NuGet 依赖、无 avahi service 文件、无 UDP 5353 放行、compose 无 host 网络），设备 mDNS 兜底实际不可用 | 见 §3 第 1 条 |
| 25 | **E8-2 / E8-4** | **设备端音源「选类型」入口不可达**：根菜单 BGM 行被拦截去呼半屏控制条（lvgl_bridge.c:683-687），含切源行的全屏 BGM 页（:719-732/:967-991）无任何 `menu_goto(MENU_PAGE_BGM)` 调用者 → 死代码；半屏条只有 5 项无切源键。另：源置灰**不持久化**（NVS 只有 bgm_vol/bgm_src），服务端**不下发**「该源已停用」（hello 的 `bgmDefaultSource`/`volume` 固件也不读） | ①去掉 683-687 的拦截（或长按进全屏页）；②置灰状态写 NVS；③hello 响应加 `sources:{wz:true,qq:false}` 并让固件消费（与 E8-8 同一处解析） |
| 26 | **E8-3 / E8-10** | QQ 源**开箱不可用**：网关本体（Rain120 node 脚本）不在仓库/镜像；且本地曲目表只含 WZ（AssetExporter.cs:199-205 `Source=0` 硬编码）→ QQ 源表空；服务端 `cmd=play` 不选曲（DeviceEndpoints.cs:317-321）→ 无法起播 QQ | ①把网关脚本按可选组件落地（或 README 明确「自备」并给出契约）；②服务端 `play` 支持不带 id 时按当前源取首曲/继续曲；③AUDIO_META 导出按源分别产出（或在设备端按 `source` 过滤服务端全量表） |
| 27 | **E8-9** | **「随机」切歌未实现**（需求 E8 明确「切歌/上下曲/随机只在所选类型内转换」）：无 shuffle 代码，半屏条无随机键 | 服务端或设备端加 `MP_AUDIO_SHUFFLE` 开关（同源内 `esp_random()` 选曲，落到 bgm.c 的 next 实现即可） |
| 28 | **E8-6** | **歌单 CRUD 无实现**（需求写「Web 只管曲库/歌单/cookie」）：无 playlist 代码，QQ 侧只有搜索关键词 | 若确认要做：加 `data/playlists.json` + Web 歌单页；若不做：把 E8 文案里的「歌单」删掉 |
| 29 | **E9-3** | **RTC 二次校时 TZ 双重偏移（只读推断，需真机复验）**：`sntp_and_set_rtc()` 里 `localtime_r`（provision.c:843）早于 `setenv("TZ","CST-8")`（:849），首次调用正确；6h 重校（:911-914）与无效时间重试（:928）复用时 TZ 已持久 → 写进 RTC 的「UTC 日历」实为 +8h → 重启后显示快 8 小时 | 把 `setenv/tzset` 提到 `localtime_r` **之前**，或直接用 `gmtime_r` 写 RTC（与 rtc_pcf85063.h 的 UTC 口径一致）；真机验证：同一 boot 触发两次校时后读 RTC 寄存器比对 |
| 30 | **E13-5** | **阈值下发非实时**：设备端只在 hello 响应 `config` 里取阈值，协议无阈值/config 指令（`Enqueue(... "config"/"threshold")` 零命中），改完要等下一次 hello | 加一条 `config` poll 指令（payload 带 Device 段）+ 固件 `MP_CMD_CONFIG` 更新 `g_mp_cfg`；Web 保存阈值时直接下发 |
| 31 | **E14-1** | 配网页**服务器地址只有 placeholder，没有真值预填**；留空静默回落硬编码 `http://192.168.1.100:38090`（provision.c:491-493） | 把 mDNS 发现结果或服务端自报地址回填到配网页（前端 input value），或把回落值改成明确的「必须填写」校验提示 |
| 32 | **E14-4** | 环缓冲日志**同时镜像到串口**，发布版未关（logbuf.h:9），与「串口日志仅开发期」字面不符 | 发布构建用 `esp_log_level_set("*", ESP_LOG_NONE)` 后再只走环缓冲，或以 Kconfig 开关区分（`CONFIG_MINIPET_SERIAL_LOG`） |
| 33 | **E14-7 / E3-10** | 仓库残留真实私网 IP `<NAS_IP>`（**跟踪文件** `Firmware/main/net/http_client.c:164`、`Server/tools/rebuild-flash-assets.sh:13,46`、`Web/scripts/preview-with-live-api.mjs:11,44`）与 `/Volumes/SSD`+用户名 | 探针目标改编译期开关（默认关）或占位符；脚本默认值改 `localhost` 并要求 `--server`；加 CI 门禁 `git grep -E "192\.168\.｜/Users/｜Volumes/SSD"` |
| 34 | **E10-1 / E10-2** | 固件具名表情宏 26 个（多 `MP_EXPR_ALERT`，零使用，与动作名冲突），注释自述 25；**系统事件表情（cheers/dam）不自动回 default**（直接 `render_set_expression` 无时长，布局重绑 `reset_expr` 恒 false） | ①删掉 `MP_EXPR_ALERT` 宏或改名；②系统事件表情统一走 `input_trigger_expression(expr, ms)`（带时长） |

---

## 3. 「未实现」清单 + 最小实现建议

| # | 未实现项 | 影响的需求 | 最小实现建议（1~3 句） |
|---|---|---|---|
| 1 | **服务端 mDNS 广告**（`_minipet._tcp`） | E14-3 | 引入 `Makaretu.Dns`：启动时 `ServiceDiscovery.Advertise(new ServiceProfile("minipet", "_minipet._tcp", port){...})`，TXT 带 `ver=1/path=/api/device`；compose 提示 `network_mode: host`（组播 5353 需同二层）。验证：`dns-sd -B _minipet._tcp` 能看到实例与端口 |
| 2 | **无键设备的主屏角落常驻按钮** | E6-2 | 在渲染层按 profile `has_key==false` 时于角落画 48×48 菜单按钮（合成器最顶层，复用 BGM 控制条那套触摸命中），点击等价 `MP_SM_EV_MENU_KEY` |
| 3 | **键位映射表 + Web 改键** | E6-1 | profile 加 `keys[]{role,gpio}`；键扫描改成表驱动；服务端设备表存 keymap 并随 hello 下发；Web 设备详情加下拉 |
| 4 | **ImageSharp fallback / `IRenderBackend`** | E3-13/E3-14 | 抽 `IRenderBackend`（现在缩略图/纸娃娃渲染散在 ThumbService/PaperdollService），先只做接口 + Skia 实现；ImageSharp 实现留空壳并加 CI 开关（属于风险预案，可显式降级为「不适用」） |
| 5 | **设备端缩略图显示** | E7-3 | 见 §2 第 16 条三步（THUMB 尺寸对齐 64 → 固件开 LODEPNG + 加 THUMB 目录 → 菜单行 `lv_image`） |
| 6 | **NPC 切换显示** | E7-5 | 复用纸娃娃实体通道：把 NPC PARTS+LAYOUT 当作 `entity:npc:*` 的另一套实体缓冲渲染（合成器已支持多 LAYOUT 绑定，主要是实体身份切换） |
| 7 | **设备选择器的服务端「预设/最近/收藏」端点** | E7-2/E7-4/E4-23 | 新增 `GET /api/device/selectors/{kind}`（kind=map｜paperdoll｜npc），返回 `[{id,label,cached,thumb,fav,recent}]`（数据源：DeviceRegistry + FavoritesStore + 本地 manifest 上报的 hashes） |
| 8 | 「家居」HA 面板第 4 tab | E7-9 | **不适用**：需求明确 P2 预留、一期不实现，代码确认无残留 → 无需动作 |
| 9 | **BGM 随机（shuffle）切歌** | E8（「…上下曲/随机只在所选类型内转换」） | 在同源曲目表上加一个随机开关/键：`next` 时以 `esp_random()%count` 取代 `idx+1`，保持源内约束（bgm.c 的 next 实现一处即可） |
| 10 | **设备端起播 QQ 源** | E8（双源可在设备上选） | ①服务端 `cmd=play` 在无 id 时按 `source` 取该源首曲/上一曲；②AUDIO_META 导出不再 `Source=0` 硬编码，或设备端按源请求服务端全量曲表；③补网关脚本或明确「自备」契约 |
| 11 | **「双源整体不可用 = despair」判定** | E10（E8 只要求单源 failover） | 服务端在两源都 Down 时下发一次专用指令（如 `MP_CMD_BGM_FAILOVER_ALL`）或在 `bgm/cmd` 响应里带 `sourcesDown`，固件据此播 despair |
| 12 | **Web 歌单管理** | E8（「Web 只管曲库/歌单/cookie」） | 若不打算做，改需求文案删除「歌单」；若要做，`data/playlists.json` + Web 歌单页 + 设备侧按歌单取曲 |
| 13 | **阈值/config 实时下发指令** | E13（「阈值下发」） | 加 `config` poll 指令类型（服务端 AdminEndpoints 保存阈值时 enqueue，固件 `MP_CMD_CONFIG` 更新 `g_mp_cfg`），Web 保存即生效 |
| 14 | **服务端 mDNS 应答者**（同 §3 第 1 条，E14 唯一完全空缺项） | E14 | 见上 |

---

## 4. 实际跑过的验证命令与关键输出（原样）

> 全部命令在 `/Users/<USER>/IdeaProjects/minipet-esp32` 或 `/tmp` 执行；临时服务端已 kill（见 §4.6）。

### 4.1 导出器真跑（E1 争议点 1 主证据）

```bash
"/Volumes/SSD/C#/dotnet" run --project Server/tools/Exporter -- \
  --wz /Volumes/SSD/mxd/mxd/Data --appearance Server/seed/default-appearance.json \
  --profile amoled216 --no-fonts --no-audio --out /tmp/audit_export
```
关键输出（原样）：
```
[MapCatalogService] 地图目录构建完成: 21335 个
[Exporter] 导出完成 → /tmp/audit_export/default
  Parts      abfa92dadac8183b     936588 B  纸娃娃装扮
  Parts      714f51e22b254c3e      22880 B  时钟数字 fontTime
  Layout     ba04f79ea52b28a3       1612 B  布局 walk1
  ...（共 11 条 LAYOUT：walk1/alert/stand1/stand2/heal/fly/ladder/rope/jump/prone/blink）
  合计: 13 个产物, 0.93 MB
```
`grep -o '"scale":[^,]*' /tmp/audit_export/default/manifest-assets.json | head -3`：
```
"scale": 2
"scale": 2
"scale": 2
```
→ **CLI 导出器是 number**（`"bottomMargin": 40` 同样）。

### 4.2 服务端实测（E2/E3/E4/E7/E8/E11/E13/E14）

起服务（Release 产物，临时 data 目录 + 空闲端口 38101；*38099 已被其他 agent 的进程占用，未使用*）：
```bash
mkdir -p /tmp/audit_data/config   # 预置 Wz.DataPath=/Volumes/SSD/mxd/mxd/Data
cd Server/MinipetServer/bin/Release/net9.0
MINIPET_DATA_DIR=/tmp/audit_data ASPNETCORE_URLS=http://127.0.0.1:38101 \
  ASPNETCORE_ENVIRONMENT=Production "/Volumes/SSD/C#/dotnet" MinipetServer.dll > /tmp/audit_server.log 2>&1 &
```
```
GET /api/health
{"ok":true,"service":"minipet-server","proto":1,"wzPathExists":true,
 "wzDataPath":"/Volumes/SSD/mxd/mxd/Data","devices":0,"qqEnabled":false}
```

```bash
POST /api/device/hello
{"deviceId":"dev-9102b2","proto":1,"paired":false,"pairingCode":"942576",
 "config":{"imuSensitivity":1,"imuDeadzoneDeg":8,"tapLightG":2,"tapHardG":4,
           "idleToClockMin":5,"bgmDefaultSource":"wz","volume":60},
 "manifestRev":1,"manifestUrl":"/api/device/manifest?deviceId=dev-9102b2",
 "pollUrl":"/api/device/poll?deviceId=dev-9102b2",
 "serverTimeUtc":"2026-09-27T07:00:30.119408Z"}

GET /api/device/manifest?deviceId=dev-9102b2      # 注意：UUID 无效，必须用服务端 deviceId（UUID → 404）
assets count: 16  kinds: Counter({'LAYOUT': 11, 'FONT': 3, 'PARTS': 2})
"scale": "2"          ← ★ 字符串（设备链路缺陷）
"bottomMargin": "40"  ← ★ 字符串
entities: [{"id":"paperdoll:default","kind":"paperdoll","layouts":[11 hash],
            "partsHash":"abfa92dadac8183b","appearanceHash":"0|0|2000|humanEar|…",
            "defaultAction":"stand1"}]
clock_table: {"200000100":[240,178],"220000100":[240,240], …}

POST /api/admin/devices/dev-9102b2/command   # expression / bubble / brightness / bgm / reboot
{"ok":true,"seq":3,"type":"expression","value":"smile"}   (http=202)
… seq=4 bubble "审计测试气泡" / seq=5 brightness {"n":70} / seq=6 bgm "play" / seq=7 reboot
GET /api/device/poll?deviceId=dev-9102b2&since=0
lastSeq 7 waitedMs 0
  seq 3 expression "smile"   seq 4 bubble "审计测试气泡"   seq 5 brightness {"n": 70}
  seq 6 bgm "play"           seq 7 reboot {}

GET /api/device/asset/0ea2810909e88820   → http=200 bytes=1432
xxd: 4d50 414b 0100 0000 …   （MPAK magic + ver=1，与 Mpak.cs 一致）

POST /api/device/event {type:"battery",data:{a:15,s:"net_offline"},hashes:["abfa92dadac8183b"]}
{"ok":true,"manifestRev":3}
→ 下一轮 manifest：cached ones: [('abfa92dadac8183b','PARTS','纸娃娃装扮')]   # 服务端算 cached ✅

POST /api/device/log  → {"ok":true,"accepted":1,"skipped":0,"lastSeq":1}
GET /api/admin/device-logs/dev-9102b2?sinceSeq=0
{"lastSeq":1,"clockSynced":true,"total":1,"items":[{"seq":1,"lvl":"I","tag":"audit","msg":"hello"}]}

GET /api/admin/music/tracks?source=wz   → count 1167（首条 {"id":"Bgm00.img/SleepyWood",…}）
GET /api/device/bgm/stream?deviceId=dev-9102b2&source=wz&id=Bgm00.img/SleepyWood
  → http=200 bytes=1666351 type=audio/mpeg
  → file: MPEG ADTS, layer III, v2, 80 kbps, 22.05 kHz, JntStereo
POST /api/device/bgm/cmd  play/pause/next/prev/volume  → 全 http=200
  next → trackId "Bgm00.img/FloralLife"（未跨源）；prev → "Bgm_Picture.img/4350046"

GET /api/admin/devices
{"devices":[{"deviceId":"dev-9102b2","uuid":"AUDIT-UUID-0001","paired":false,"online":true,
 "profile":{"w":480,"h":480,"shape":"round","psram":8,"audio":true},
 "bgm":{"source":"wz","volume":40},
 "health":{"lastError":"net_offline","batteryPercent":15,"totalEvents":2}}]}

GET /api/admin/thumb?type=map&id=200000100  → http=200 image/png ；file: PNG image data, 64 x 64
GET /api/admin/materials?kind=map           → count 21422
POST /api/admin/settings/validate-path {"path":"/nope/wz"}
  → {"ok":false,"message":"WZ 路径不存在：/nope/wz"}

GET  /api/admin/presets  → http=404 bytes=0     ★ HEAD 快照路由被注释吞掉
POST /api/admin/presets  → http=201 {"preset":{"id":"p-3f6af259","name":"审计测试预设",…}}
git show HEAD:Server/MinipetServer/Api/AdminEndpoints.cs | sed -n '122p'
  → `        // ── 纸娃娃预设 CRUD（…）──        g.MapGet("/presets", (PresetStore presets) => …);`  ← 同一行
```

### 4.3 E13 争议点 6：设备上报 profile 真参与烘焙（第二实例，Debug 构建含 67ba3fd）

```bash
cd Server/tools/Exporter/bin/Debug/net9.0     # 15:01 构建，含 92a42af + 67ba3fd
MINIPET_DATA_DIR=/tmp/audit_data2 ASPNETCORE_URLS=http://127.0.0.1:38102 dotnet MinipetServer.dll &
POST /api/device/hello {profile:{w:320,h:240,shape:"square",…}} → deviceId dev-b8ad53
POST /api/admin/devices/dev-b8ad53/push {kind:"map",id:"200000100",switch:false}
# 解 BGMAP 包（payload offset 32 起 u16 vw/vh）
BGMAP mpak 450b73c19f21e7b2.mpak: map_id=200000100 vw=320 vh=240 payload=316912B
# 对照 480×480 设备同地图
480-device BGMAP a5a63b89e7edb61e.mpak map_id 200000100 vw 480 vh 480
# 同一 Debug 构建的 e2e manifest 仍是字符串 scale —— 证明是代码缺陷、不是旧产物
"scale": "2"
```
另：地图推送后 manifest 新增 `BGMAP{selector:map,map:200000100,thumb:"076c31a3228e3dcf"}` 与 `THUMB{缩略图 200000100}`；`file`：
```
215b84edf0531099.png: PNG image data, 96 x 96, 8-bit/color RGBA, non-interlaced
```

### 4.4 解包分析（E1 的 25 表情 / LAYOUT expression 维度）

```python
# PARTS abfa92dadac8183b.mpak → payload，u32 count + 20B 索引
PARTS part_count = 294
expr_group sizes (group->count), group 0 = non-face: {0: 269, 1: 25}
face families: 1 each count: [25]
  group 1: ids 100..124 count=25 consecutive=True
# LAYOUT 0ea2810909e88820.mpak（stand1）
LAYOUT action=stand1 frame_count=3 expression_count=25
expressions: ['blink','hit','smile','troubled','cry','angry','bewildered','stunned','vomit',
 'oops','cheers','chu','wink','pain','glitter','despair','love','shine','blaze','hum',
 'bowing','hot','dam','default','qBlue']
```

### 4.5 门禁与构建

```bash
grep -rn "Avalonia" Server/ --include=*.cs | wc -l                 # → 0
grep -rn "Dispatcher.UIThread" Server/ --include=*.cs | wc -l      # → 0
grep -rn "mdns|Mdns|MDNS|_minipet\._tcp|Zeroconf|Bonjour" Server --include=*.cs
                                                                   # → No matches（E14 mDNS 服务端未实现）
cd Web && npx vite build
  ✓ built in 3.73s   （DashboardView/MaterialsView/PaperdollView/MusicView/SettingsView 等 24 chunk）
```

### 4.6 临时服务端清理

```bash
job_kill bash-130 / bash-139     # 两个审计实例（:38101 / :38102）
lsof -nP -iTCP:38101 -sTCP:LISTEN   # → 空
lsof -nP -iTCP:38102 -sTCP:LISTEN   # → 空
```
未触碰 38090 / 38095 / 38097 / 38098 / 38099（均为其他 agent 的进程），未碰串口，未烧录。

---

## 5. 七个争议点的独立判断

### 争议 1 — E1「scale 2x 由导出器标注」（**部分实现，且有新缺陷**）
- 导出器 CLI 真跑产物**确实带** `"scale": 2` / `"bottomMargin": 40`，且类型是 number（commit 92a42af 的 ManifestBuilder 修复有效）。
- **但设备真正消费的 manifest 不是这份**：`GET /api/device/manifest` 由 `DeviceManifestService` 读设备目录下 `manifest-assets.json`，而该文件在服务端路径由 `PaperdollPackService.EntryOf`（PaperdollPackService.cs:142-167）/**`DeviceAssetService.EntryOf`（:174-193）** 写出——这两份是 `ManifestBuilder.EntryToJson` 的**未同步复制品**，都保留 `o[k] = v.ToString() ?? ""`（:160 / :192）。
- 实测：**Release（14:50）与 Debug（15:01）两个构建**的 `/api/device/manifest` 都返回 `"scale": "2"`。即 commit 92a42af 提交信息里点名的下游风险（「cJSON_IsNumber 会失配」）**在设备链路上仍然存在**。
- 另外固件根本不读该字段（编译期 `RC_SCALE`/`RC_ENT_MARGIN_B`），所以「标注」目前只是**随包下发的元数据**，不是设备行为的驱动源。
- **结论：部分实现** —— 标注已产出（CLI 侧正确 / 服务端侧类型错误）；消费端未接通。

### 争议 2 — E5 图层顺序（**已实现，与需求一致**）
`compose_region`（compositor.c:930 起）实测顺序：CLOCK_DOZE 特例 → ①static_back(:954-964) → ②视差条带(:966-968) → ③tile_layer(:970-988) → ④地图时钟(:990-991) → ⑤校准调试层 → ⑥宠物(:1059-1061) → ⑦气泡(:1063-1074) → ⑧未配网横幅(:1076) → ⑨BGM 控制条(:1116)。需求「static_back → 视差条带 → tile_layer → 宠物 → 气泡」**逐层吻合**：无 tile/strip 颠倒、宠物在 tile 之上、气泡在宠物之上。多出的 4 层（地图时钟在实体下、校准层仅调试、横幅在气泡上、BGM 条最顶）不违背需求。宠物内部 piece 的 `z` 字段不参与排序（导出端保证列表顺序，两端契约自洽）。

### 争议 3 — E7 设备菜单缩略图（**服务端有、设备端没有 → 部分实现**）
需求原文（requirements-analysis.md:110）把「**缩略图 64×64 服务端渲染**」写在设备选择器地图 tab 条目内；设计文档 `design-review.md:100` 更明确写「**设备选择器三个 tab 要显示条目名称与缩略图**，manifest 目前只有 hash+kind。需加 `label` 字段与缩略图 asset 引用」。据此：
- `label` ✅（已在 manifest）、缩略图 asset 引用 ✅（`extra["thumb"]` + THUMB 包，实测进 manifest）；
- **设备端呈现 ❌**：菜单行只有 `lv_button+lv_label`（lvgl_bridge.c:815-845），全固件无 `lv_image`；PNG 解码未开（sdkconfig:2667-2668）；THUMB 不下载（asset_dl.c:118-128 / :775-778）。
- 附带不一致：给设备的 THUMB 是 **96×96**（AssetExporter.cs:684），而需求/Web 链路是 **64×64**（ThumbService.cs:29 + 实测 `/api/admin/thumb?type=map` 返回 64×64）。
- **结论：部分实现**（服务端生成链路完整，设备端 UI/解码/下载三缺）。

### 争议 4 — E9 睡眠宠物（**已实现，判定为「有意的折衷」而非偏差**）
需求原文：「无人交互 N 分钟 → **宠物睡眠态** + WZ 数字时钟浮现…；AMOLED **纯黑背景只数字发光**（省电）」。
实际实现（compositor.c:946-952）：CLOCK_DOZE 分支 = 整脏区 `memset 0`（纯黑）→ 画时钟数字 → `ent_compose(..., RC_SLEEP_DARKEN)`（`RC_SLEEP_DARKEN 35`，compositor.h:83，RGB565 各通道 ×35/100，compositor.c:903-928）。
**判断（三条依据）**：
1. 需求同一句里既要求「宠物**睡眠态**」又要求「只数字发光」，二者字面互斥；旧实现（画完数字直接 `return`，宠物完全不画）才是与「睡眠态」冲突的那版——compositor.c:941-945 的注释也记录了「此前只画数字 → 被记为 E9 缺口（sleep-pet state）」。
2. 固件**没有睡眠动作素材**（`MP_ACTION_*` 只有 stand1/walk1/fly/alert/hit，app_core.h:87-92；全仓 grep sleep/doze 动作零命中；导出器也没有 sleep 动作）→「睡眠态」只能靠降亮表达。
3. CLOCK_DOZE 进态只发 DEFAULT 表情 + CLOCK 开关（state_machine.c:151-156），**实体动画仍在跑 stand1/blink**（未停 `rc_anim`）——即宠物是「亮着睡」而不是「静止睡」。
**结论：已实现**，代价是破了「只数字发光」的字面；省电影响限于实体区像素（屏其余纯黑）。建议在需求文档补一句「宠物睡眠态 = 宠物降亮 35% 保留可见（并在待机期继续 blink/stand1）」以消歧。

### 争议 5 — E11 离线事件名（**已实现，与需求修订文本逐字一致**）
固件上报：`MP_EVT_ERROR` + `s="net_offline"` / `s="net_online"`（state_machine.c:52-72 组包、:168-169 离线、:449-453 回网并按原始时刻补报）；events.c:42 映射 `MP_EVT_ERROR → "error"`，`s` 随 `data` 原样上报。
需求文档 E11 自己写明：「**暂复用**已映射的 `MP_EVT_ERROR` + `s="net_offline"/"net_online"`…待 events.c 增加专用 `net_offline`/`net_online` 事件名后切换」（requirements-analysis.md:167-171）。→ 代码 = 需求（修订版）原文。**未完成的是需求里也标注为「待切换」的专用事件名**（events.c 仍无 net_* 类型）。实测服务端 `health.lastError = "net_offline"`，链路真的通。

### 争议 6 — E13 导出是否用设备上报 profile（**已验证：是**）
- 代码：`DeviceAssetService.cs:70-73` 取 `_reg.Get(deviceId)?.Profile` → `ToExportProfile()`（w/h 缺失回落 480）→ `ExportMapAssets(mapId, warnings, devProfile)`；`AssetExporter.cs:798-807` 用该 profile；`ExportMap` 内 `profile.ViewportW/H`（:582-587）。
- 实测（Debug 15:01 构建，含 commit 67ba3fd）：profile=`320×240` 的设备推送地图后，BGMAP 包 `vw=320 vh=240`；profile=`480×480` 的设备同地图 `vw=480 vh=480`。
- 注意：**Release 产物（14:50:14）早于该提交 12 秒**，用旧产物验证会得到恒定 480×480 —— 复核此条必须用 ≥14:50:26 的构建（本次已分别验证）。
- **结论：已实现并有证据**（E13-7）。

### 争议 7 — E14 mDNS（**部分实现：设备有发现能力，服务端不广告**）
- 设备侧：`Firmware/main/net/mdns_discover.c:61-103`（`mdns_query_ptr` 查 `_minipet._tcp`；PTR 1.1s，响应无 A 记录时按 hostname 补 `mdns_query_a` 1.1s；用完 `mdns_free` 不留常驻任务）；`state_machine.c:205-211`/`:259-290` 仅在「srv_url 为空」或「hello 连不上」时兜底，手输优先；依赖 `main/idf_component.yml` `espressif/mdns ^1.2.2`。
- 服务端侧（五条独立证据，全部为空命中）：① `grep -rniI "mdns\|_minipet\._tcp\|zeroconf" Server --exclude-dir={bin,obj}` → 无输出；② `grep -rniI "avahi\|dnssd\|dns-sd\|bonjour\|makaretu\|Tmds" Server` → 无输出；③ `MinipetServer.csproj` 仅 SkiaSharp/SkiaSharp.NativeAssets.Linux/System.IO.Hashing；④ 部署件无 UDP 5353 放行、compose 无 `network_mode: host`（docker-compose.example.yml:16-18）；⑤ 仓库文档自证 `docs/ai/keys-touch-handoff.md:197-199`「固件已实现，服务端未实现」+ :220-236 四条实现要求。
- → **部分实现**，设备侧 mDNS 兜底在当前服务端下**实际不可用**（除非用户自建 avahi/Bonjour 应答者；即便补上，桥接网络默认收不到 5353 组播，还需 host 网络）。最小补法见 §3 第 1 条。

---

## 6. software-design.md 接口契约抽查（8 条）

| # | 设计文档契约 | 代码/实测 | 判定 |
|---|---|---|---|
| C1 | `/api/device` 端点面：hello / manifest / asset / poll / event / bgm/stream / bgm/cmd / firmware（software-design.md:68） | DeviceEndpoints.cs:25-39 注册 **9 条**（多 `/log`，E14 新增）；全部实测可达（firmware → 404 因无产物，路由存在）。固件侧 poller/http_client 与之一一对应 | **一致（超集）** |
| C2 | hello 请求/响应字段（profile w,h,shape,psram,audio + UUID + 固件版本 → deviceId + 配置） | DeviceEndpoints.cs:40-62（`HelloRequest{proto,uuid,firmware,profile{W,H,Shape,Psram,Audio}}`）；**实测响应**含 `deviceId/proto/paired/pairingCode/config{…}/manifestRev/manifestUrl/pollUrl/serverTimeUtc`；固件解析 http_client.c:414-431 | **一致** |
| C3 | manifest 字段（`proto/rev/assets{hash→{kind,bytes,url,label,selector,entity,action,bounds,scale,bottomMargin…}}` + `firmware` + `clock_table`）（software-design 4.4/2.3；algorithm-asset-format） | **实测**：`proto/rev/assets/entities/clock_table`（+地图后 `BGMAP/THUMB`）；固件读取 asset_dl.c:270-310（`rev`/`active_map`/`clock_table`/`assets`）+ 本地 manifest 用 `files` 数组（自洽）。**例外**：`scale`/`bottomMargin` 类型为字符串（§5 争议 1） | **基本一致**（数值类型字段除外） |
| C4 | MPAK 包格式（magic/version/kind/content_hash/payload_len/crc32c，40B 头）（algorithm-asset-format.md §二、Mpak.cs:44-63） | **实测**：asset 响应首 16 字节 `4d50 414b 0100 0000 …`（`MPAK`+ver=1）；固件 mpak.c 校验顺序与 Mpak.cs:155-188 一致；文档 §二:29 的「MAGIC→crc→长度→hash」描述顺序与实现（长度先于 crc）**不一致** | **实现一致，文档一句顺序需改** |
| C5 | 指令口径（poll 指令 type/payload ↔ 固件 handle_cmd；bgm 的 vol/source 走 `{t,v,n}` 旧口径）（AdminEndpoints.cs:629-641 注释） | **实测响应** `commands:[{seq,type,payload}]`；固件 poller.c:234-320 注释写明「服务端实际响应（DeviceEndpoints.cs）：{commands:[{seq,type,payload}], lastSeq, mrev}」并做 payload 摊平（含 `payload.v` 兼容分支 :259-262）；实测 expression/bubble/brightness/bgm/reboot 五类全通 | **一致** |
| C6 | 事件上报契约：单事件 `{deviceId,type,tsUtc,data,hashes}`（DeviceEndpoints.cs:54-62） | **实测**：battery/error/log 三种 POST 全 200；固件 events.c:109-125 注释记录「旧 `{events:[…]}` 形状会被 400 拒收，现改单事件 + `data.batch[]` 携批」 | **一致** |
| C7 | 配置模型键（`Wz.DataPath` / `QqMusic{Enabled,Cookie,GatewayPort}` / `Bgm.DefaultSource` / `Device{ImuDeadzoneDeg,TapLightG,TapHardG,IdleToClockMin}` / `Clock.MapOffsets`），**端口不入 appsettings**（software-design.md:74-86） | ConfigService.cs:14-110 与 `Server/MinipetServer/appsettings.json` 键名逐字对应；**实测** `/api/admin/settings` 无端口键、`/api/health` 的端口来自 ASPNETCORE_URLS/.env | **一致** |
| C8 | 任务架构与引脚（render/input 在 APP_CPU(1)、net/ota/bgm 在 PRO_CPU(0)；IMU 双中断 GPIO17/21；TF 布局 `/minipet/{manifest.json,parts,layout,bg,font,firmware}`；85% 水位淘汰）（software-design.md:112-124、4.4） | main.c:293（render core 1）、:304（input core 1）、poller.c:560-563（core 0）、events.c:192-194（core 0）、ota.c:151、asset_dl.c:849、bgm.c:804/850（core 0）；input_dispatch.c:1019（IMU 17/21）、:1003（菜单键 18）；app_core.h:39-42（`/sdcard/minipet/…`）；asset_dl.c:39-40（`WATERMARK_PCT 85` / 清到 80%） | **一致** |
| C9（附加） | 2.3「认证：局域网信任模型（v1 无鉴权，**预留 API key 字段**）」 | `grep -rn "ApiKey\|apiKey\|api_key\|X-Api-Key" Server/MinipetServer --include=*.cs` → 零命中 | **未实现（预留字段也没有）**，属设计里承诺的最小预留 |

---

## 7. 「跑过的」与「只读代码推断的」分界

**跑过（真实执行、输出已贴 §4）**
1. Exporter CLI 全量导出（WZ 真数据）→ 13 产物 + manifest-assets.json 的 `scale` 类型；
2. Release 服务端（:38101）+ Debug 服务端（:38102）各起一次 → health / hello / manifest / asset / poll / event / log / bgm stream / bgm cmd / admin devices / settings / validate-path / thumb / materials / music tracks / music sources / presets(404+201) / device-logs / logs / health / firmware(404)；
3. 地图推送（`POST /admin/devices/{id}/push`）两个 profile（480×480 / 320×240）→ BGMAP 包 vw/vh 对照；
4. MPAK 解包（PARTS 25 槽 / LAYOUT expression_count=25 / header magic）；
5. `grep` 门禁（Avalonia / Dispatcher.UIThread / Server 侧 mDNS）与 `npx vite build`；
6. `git show HEAD:...AdminEndpoints.cs` 复核 presets 路由被注释吞掉。

**只读代码推断（未在真机/构建上验证）**
- E5 全部运行时渲染行为（合成、时序、脏区带宽、帧率）、E6 全部交互行为（菜单冻结、触摸命中、IMU 分级、力度阈值）、E7 设备菜单行为与随机表情被吞、E8 固件解码/I2S 出声/**设备端选源入口不可达**/**QQ 无法起播**、E9 RTC 走时（含 **TZ 双重偏移**）、E11 淘汰与离线运行、E12 气泡渲染与断行、E13 配对码屏显、E14 配网/OTA/看门狗实机路径 —— 这些均无真机运行（未烧录、未碰串口），结论来自读码。
- E5-8 的 QSPI 带宽、E6-8 的误触率、E7-6 的随机表情被吞、E9-3 的 8 小时偏移、E8-10 的 QQ 起播失败，属**代码推断**，需真机确认。
- 仓库为移动靶：审计期间 `compositor.c`（1914→1933 行）、`state_machine.c`、`input_dispatch.c`、`bgm.c`、`poller.c`、`provision.c`、`DeviceEndpoints.cs`、`AdminEndpoints.cs`、`WzMusicSource.cs` 被其他 agent 修改；HEAD 从 67ba3fd 前进到 69fe230（其中 69fe230 修掉了本审计独立发现的 presets 路由缺陷）。行号可能已漂移，复核请按引文片段 grep。

### 7.1 本次审计的分工与证据强度

| 审计块 | 执行者 | 证据强度 |
|---|---|---|
| E1（除导出器真跑）/E3/E4 | 子审计（只读 grep + read + `git show`） | 代码级；E1-12 的「scale 是 number」结论被本审计的**运行时**证据推翻并修正（服务端链路实为字符串） |
| E2/E13/E14 | 子审计（只读；未起服务） | 代码级 + 文档自证；服务端端点连通性由本审计的 curl 实测补齐（两者结论一致） |
| E5/E6/E7 | 子审计（只读；未构建/未烧录） | 代码级；争议点 2（z 序）由本审计独立读码复核，结论一致 |
| E8/E9/E10/E12 | 子审计（只读；未构建/未烧录） | 代码级；E9-3 的 TZ 缺陷与 E8-2 的死代码入口由本审计独立复核确认 |
| **运行时验证（§4 全部命令）** | 本审计 | 真跑：Exporter + 两个服务端实例 + curl 全端点 + MPAK 解包 + vite build + 门禁 grep |

---

## 8. 一句话总览

- **E1**：迁移与导出器基本达标，但「scale 标注」在**设备下发链路上仍写成字符串**（两份 EntryOf 未同步修），且固件不消费 → 部分实现。
- **E2/E13**：协议与设备管理是本次审计**最扎实**的两块（端点全通、entities[]/proto/cached/profile 烘焙均实测通过）；缺口是 manifest 无 BGM 清单、asset 端点未按设备隔离、阈值下发非实时。
- **E3/E4**：部署与 Web 面基本完成；缺口集中在 **ImageSharp fallback（未做）**、容器内真实渲染验证、以及审计期发现的 **`GET /api/admin/presets` 被注释吞掉**（已在 69fe230 修复，需重建镜像）。
- **E5/E6/E7**：渲染主链（z 序、时序、blink、铁律）达标；缺口集中在**设备选择器**（缩略图不显示、无服务端预设/最近/收藏端点、NPC 只能缓存、收藏保护死代码、随机表情被菜单守卫吞）与**按键模型**（无键位表、无无键设备兜底按钮）。
- **E8**：服务端双源/短路规则/流式转发实测通过；**设备侧**有三处硬缺口——**选音源入口是死代码**、**QQ 源无法起播（网关脚本缺 + play 不选曲）**、**随机切歌未实现**。
- **E9/E10/E12**：低电口径（troubled）/25 表情/宋体字体链均落地；争议点 4（睡眠宠物）判定为**有意的折衷、满足需求**；新发现 **RTC 二次校时 TZ 双重偏移（推断，+8h）** 与固件多一个死表情宏。
- **E11/E14**：降级矩阵与支撑设施基本达标；**服务端 mDNS 广告是 E14 唯一完全未做的子项**（设备侧发现能力因此空转），另有配网页地址无真值预填、串口日志未关、真实私网 IP 残留在跟踪文件。
