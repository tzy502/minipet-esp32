# 服务端缺口收口报告（T1–T4）· 2026-09-27

> 执行者：子 agent（只改 `Server/`）。dotnet = `"/Volumes/SSD/C#/dotnet"`。
> 编译门禁：`"/Volumes/SSD/C#/dotnet" build Server/MinipetServer/MinipetServer.csproj -c Debug --no-incremental` → **66 Warning(s) / 0 Error(s)**（警告全为既有 CS86xx/CS0649，与本次改动无关）。
> 未做 git commit（本仓库有其它 agent 在提交）。

## 0. 生效范围速查（本机 vs 线上）

| 项 | 本机实例 `http://127.0.0.1:5059`（`MINIPET_DATA_DIR=/tmp/minipet-t1`，测试设备 `dev-cf3c4c`） | NAS 线上 `http://<NAS_IP>:38090`（容器，设备 `dev-693ea4`） |
| --- | --- | --- |
| T1 代码（bgm 指令） | **已生效并全部自测通过** | **未生效**（镜像烘死）。实测同一请求：线上 `400 {"error":"type 非法：bgm（可用：expression/action/bubble/brightness/reboot）"}`，本机 `202 {"ok":true,"seq":15,"type":"bgm","value":"next"}` |
| T2/T3 资产（数据写入） | 不涉及 | **已生效**：manifest rev 17→23，assets 23→54，BGMAP 1→4，NPC 条目 0→8 |
| T4 | 只读排查 + 源码核对 | 只读排查（未改线上任何文件） |

> 代码改动要上 NAS：重建/重拉 `ghcr.io/<WIFI_SSID>/minipet-esp32:latest` 并 `docker compose up -d`（NAS 上 docker CLI 对 ssh 用户无权限，本轮未执行）。

## 1. T1【P0】admin 指令端点支持 bgm

### 1.1 固件真实形状（`Firmware/main/net/poller.c`，poller.c 未被本轮任何 agent 改动）

`do_poll_once()` 对 `commands[]` **有两条解析路径**：

- **现代口径** `{seq,type,payload}`（服务端 `CommandQueue` 一直产出这个形状）→ poller.c:155-224 的专用解析：
  - `payload` 是字符串 → `vitem = payload`；对象则取 `id`/`ver`/`url`/`n`；
  - bgm 分支 **poller.c:208-217**：只认 **6 个字符串值** `play/pause/resume/stop/next/prev` → `MP_AUDIO_*`；
  - **没有 vol / source 分支**；`{"payload":{"n":50}}` 会被 poller.c:171-173 折算成 `vitem=NULL` → 分支不匹配 → **静默丢弃**。
- **旧口径** `{t,v,n}`（命令对象**不带 `type` 键**）→ poller.c:158 `if (!t) { handle_cmd(jc); continue; }` 直通 **handle_cmd（poller.c:77-88）**：
  - 那里才有 `bgm` 的 `vol`（`MP_AUDIO_VOL`，`a=n` 绝对音量）与 `source`（`MP_AUDIO_SOURCE`，`a=0/1`）。

### 1.2 服务端改动（3 个文件，均只在本机生效）

| 文件 | 改动 |
| --- | --- |
| `Server/MinipetServer/Api/AdminEndpoints.cs` | `/devices/{id}/command` 放行 `type=bgm`：6 个动词 → 裸字符串 payload；`vol`/`volume`(+`n∈[0,100]`)、`source`(+`n∈{0,1}`) → 旧口径；同步落 `dev.Bgm.Volume/Source` 偏好；兜底 400 文案加入 bgm |
| `Server/MinipetServer/Api/DeviceEndpoints.cs` | ① poll 响应 `commands` 经 `DeviceCommand.ToWire()` 投影（现代指令形状**逐字节不变**，仅 Legacy 指令变 `{seq,t,v,n}`）；② `POST /api/device/bgm/cmd` 回传补事件日志（`Web/docs/interfaces-needed-from-server.md §T6` 实现要点 4 的建议项） |
| `Server/MinipetServer/Device/CommandQueue.cs` | 新增 `DeviceCommand.Legacy`（`{t,v,n}`，`JsonIgnore(WhenWritingNull)` 保证现代指令线上形状不变）+ `ToWire()` + `Queue.EnqueueLegacy()` |

> 注：`CommandQueue.cs` 与 `DeviceEndpoints.cs` 的这部分改动已被**其它 agent 的提交 `8b4ba1e`（12:33:13）**连带提交；`AdminEndpoints.cs` 与 `DeviceEndpoints.cs` 的事件日志改动仍是工作区未提交状态。

### 1.3 自测（本机实例，真实 curl）

```
--- 1) bgm next ---      HTTP 202 {"ok":true,"seq":1,"type":"bgm","value":"next"}
--- 2) bgm vol n=45 ---  HTTP 202 {"ok":true,"seq":2,"type":"bgm","value":"vol","n":45}
--- 3) bgm source n=1 ---HTTP 202 {"ok":true,"seq":3,"type":"bgm","value":"source","n":1}
--- 4) bgm play ---      HTTP 202 {"ok":true,"seq":4,"type":"bgm","value":"play"}
--- 5) bgm vol 无 n ---  HTTP 400 {"error":"bgm=vol 需要 n∈[0,100]（固件 vol_apply 夹取 0..100）"}
--- 6) bgm value=bogus --HTTP 400 {"error":"bgm 的 value 非法：bogus（可用：play/pause/resume/stop/next/prev；音量 vol + n；音源 source + n）"}
--- 7) expression 回归 --HTTP 202 {"ok":true,"seq":5,"type":"expression","value":"smile"}
--- 8) brightness 回归 --HTTP 202 {"ok":true,"seq":6,"type":"brightness","n":70}
--- 9) 设备现场控制回传 --POST /api/device/bgm/cmd {"deviceId":..,"cmd":"volume","volume":33}
                        → {"ok":true,"cmd":"volume","source":"qq","volume":33}，事件日志新增
                          「BGM：volume（设备现场控制）· source=qq · volume=33」
```

`GET /api/device/poll?deviceId=dev-cf3c4c&since=0` 实际响应（线上形状）：

```json
[ {"seq":12,"type":"bgm","payload":"play","enqueuedAtUtc":"…"},
  {"seq":13,"t":"bgm","v":"vol","n":33},
  {"seq":14,"t":"bgm","v":"source","n":0},
  {"seq":6,"type":"brightness","payload":{"n":70}, …} ]
```

按 `poller.c` 逐行镜像解析（`/tmp/fw-mirror.py`，仅镜像 155-224 与 44-94 的判定逻辑，非真机运行）：

```
✅ {"seq":12,"type":"bgm","payload":"play"} → 现代分支: MP_AUDIO_PLAY (mp_post_audio)
✅ {"seq":13,"t":"bgm","v":"vol","n":33}   → 旧口径 handle_cmd (poller.c:158): MP_AUDIO_VOL a=33
✅ {"seq":14,"t":"bgm","v":"source","n":0} → 旧口径 handle_cmd (poller.c:158): MP_AUDIO_SOURCE a=0
✅ {"seq":6,"type":"brightness","payload":{"n":70}} → 现代分支: MP_CMD_BRIGHTNESS a=70
```

### 1.4 与 Web 侧（`Web/src/api/client.js` / `MusicView.vue`）的接口契合

- Web 发的 body 正是 `{"type":"bgm","value":"…"}` + 可选 `n` → 与本端点完全一致，**Web 无需改动**。
- Web 探针哨兵 `{"type":"bgm","value":"__probe__"}` → 本实现回 `400 bgm 的 value 非法：__probe__…`；
  用 Node 跑 Web 自己的正则复核：该文案**不会**被误判成「端点不支持 bgm」（只有旧文案 `type 非法：bgm（可用：…）` 才判不支持）→ 前端会正确点亮 7 个按钮。
- **重要更正**：`Web/docs/interfaces-needed-from-server.md §T6` 写「音量要真生效，固件需在 poller.c:208 的 bgm 分支补 `pn` 支持」——**不需要**。音量走 poller.c 本就支持的旧口径 `{t:"bgm",v:"vol",n:N}`（handle_cmd），当前固件即可生效，**固件零改动**。Web 侧照发 `{"type":"bgm","value":"vol","n":…}` 即可。

## 2. T2【P0】第 2/3/4 张地图（写线上 NAS 实例）

逐条记录（每次 push 后轮询 `GET /api/device/manifest`）：

| # | mapId | label | BGMAP hash | bytes | manifest rev | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 起始 | 200000100 | 神秘岛：天空之城售票处 | `c472cc60aad29688` | 950560 | 17 | 原有 |
| 1 | 100000000 | 射手村：射手村 | `48e6c351a1512356` | 950532 | 18 | 202 → 4s 后条目出现 |
| 2 | 104000000 | 明珠港：明珠港 | `1a7bbb1abe7760fa` | 950644 | 19 | 202 → 4s 后条目出现 |
| 3 | 200000000 | 神秘岛：天空之城 | `d4874120c55d6f90` | 950630 | 20 | 202 → 4s 后条目出现 |
| 复位 | 200000100 | （幂等，不新增条目） | – | – | 23 | 见下「switch 语义」 |

- mapId 取自 `GET /api/admin/materials?kind=map`（total 21422，真实枚举，非编造）；推送前用 `GET /api/admin/thumb?type=map&id=…&size=64` 预检 6 张候选图均 `HTTP 200`（PNG 有画面）。
- **最终 manifest（线上实测）**：`rev=23`、`assets=54`、**BGMAP 条目 4 个**（要求 ≥2）：
  `48e6c351a1512356`(100000000 射手村)、`1a7bbb1abe7760fa`(104000000 明珠港)、`d4874120c55d6f90`(200000000 天空之城)、`c472cc60aad29688`(200000100 售票处，`cached=true`)。
- 事件日志（NAS）逐条：`[12:34:35] 推送地图 100000000（资产已登记）`、`[12:34:42] 104000000`、`[12:34:46] 200000000`、`[12:36:40] 200000100`。
- **switch 语义说明**：端点默认 `switch=true`，每次 push 会额外入队一条 `SET_MAP`（payload = 该图 BGMAP hash，含 15s 重发）。设备 `dev-693ea4` 当前 **offline**，指令未被消费；为避免设备重连后被动换图，最后用 `200000100` 复推一次（资产幂等跳过），使队列里**最后一条** `SET_MAP` 指向设备原本所在的地图 → 重连后画面不变，菜单新增 3 行可自选。若确实想让设备直接落到新图，一条 curl 即可：`POST /api/admin/devices/dev-693ea4/push {"kind":"map","id":"200000000"}`。

## 3. T3【P1】NPC 资产（写线上 NAS 实例）

npcId 取自 `GET /api/admin/materials?kind=npc`（total 15789，名字可解析）；推送前 `thumb?type=npc` 预检 `2101`/`10200`/`2000`/`10201` 均 200。

| NPC | label | 主条目（PARTS，entity=npc:*） | LAYOUT | rev |
| --- | --- | --- | --- | --- |
| 2101 | 希娜（2101） | `bdd8dce9fd1fb001`（1048008 B） | 5 个：stand/smile0/smile1/cry/bewildered | 21 |
| 10200 | 赫丽娜（10200） | `3a65369831f38c7e`（13332 B） | 1 个：stand | 22 |

- 最终 manifest 里 `selector=npc` 条目 **8 个**（2 个 PARTS + 6 个 LAYOUT），全部 `entity=npc:{npcId}`，`cached` 未标记（设备 offline 尚未上报本地 hash 集，属预期）。
- 事件日志：`[12:35:00] 推送 NPC 2101（资产已登记）`、`[12:35:06] 推送 NPC 10200（资产已登记）`。

## 4. T4【P1】`manifest-assets.json` 的 `generated` 与 mtime 不一致（只读排查）

**证据（NAS `/volume2/docker/minipet/data/cache/export/dev-693ea4/manifest-assets.json`，改动前）：**

```
generated = 2026-09-26T17:39:45Z
mtime     = 2026-09-27 09:43:42 +0800 = 2026-09-27T01:43:42Z   ← 相差 8h03m57s
索引键     = ["assets","generated"]（没有 deviceId / proto）
```

扣除 +08:00 的「UTC 存 / 本地显示」差后仍多出 **3 分 57 秒** → 该文件确实被**重写却没有刷新 `generated`**。

**根因（文件形态比对，确定到 writer）：**

- 同一秒（09:43:42）内先写 3 个 FONT 包（`.176`→16px、`.333`→24px、`.453`→32px，按字号升序），最后 `.726` 写索引 —— 这正是 `FontPackService.EnsureFonts` 的写入顺序（`foreach size` 写 mpak → 再写索引）。
- 该索引**没有 `deviceId` 键**，而本机 HEAD 的 `FontPackService`（`Services/FontPackService.cs:103-104`）在写索引时同时写 `deviceId` 与 `generated`；二者都是提交 **`bd4d3e8`（2026-09-27 09:50:33 +0800）**加进去的，**比那次写入晚 7 分钟**。
- 结论：**NAS 运行的镜像版本早于 `bd4d3e8`**，其 FontPackService 会重写索引而不更新 `generated` → 这就是 stale 的来源。**本机 HEAD 的 4 个 writer 全部刷新 `generated`**（grep 证据：`ManifestBuilder.cs:63`、`PaperdollPackService.cs:76`、`DeviceAssetService.cs:97`、`FontPackService.cs:104`）→ **代码侧无需再改**。
- **反证（在线上实例上做的对照实验）**：本轮 T2/T3 push 走的是同一实例的 `DeviceAssetService.MergeAndWrite`（另一个 writer），推完后：
  `generated = 2026-09-27T04:35:06Z`，`mtime = 2026-09-27T04:35:06Z`，**两者差 0.13 秒** → 该 writer 在线上是正常刷新的，stale 只发生在字体写入路径。
- 残留疑点（不影响结论）：09:37:19 有一次**单文件**重写（AUDIO_META `8d618b5d337f2818`，内容/哈希未变）。HEAD 中没有任何代码路径会单独写一个 AUDIO_META 包，故可能是 NAS 侧同步/备份工具触碰或更老代码的一次性导出；无法在只读权限内进一步定位。
- 自愈路径：重新部署含 `bd4d3e8`+ 的镜像后，下一次字体/资产写入即会把 `generated` 刷到当下。

## 5. 未跑 / 无法验证

- **真机端到端**：`dev-693ea4` 全程 `online=false`（lastSeen 2026-09-26T18:04:30Z），无法验证固件真的出声、切图、菜单行数、NPC 页显示；T1 只有「形状级」证据（形状逐字对齐 poller.c），T2/T3 只有 manifest/索引级证据。
- **NAS 容器层信息**：ssh 用户对 `/var/run/docker.sock` 无权限（`permission denied`），未能取镜像版本/启动时间；镜像版本是用「索引缺 deviceId + 字体写入顺序 + 提交时间线」推断的。
- **Web UI 实测**：`Web/` 禁改，未点击验证；只做了「Web 现有正则/请求体 vs 本端点实际响应」的静态比对（Node 复跑正则 + body 形状比对）。
