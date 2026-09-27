# Web 侧待服务端支持的接口清单（T2 / T4 / T5 / T6）

> 生成时间：2026-09-27 · 提出方：Web（`Web/`，本轮只改 Web，未动 `Server/`）
> 现状核对基线：`Server/MinipetServer` 工作区当前代码（`AdminEndpoints.cs` / `ConfigService.cs` /
> `DeviceEndpoints.cs` / `Services/PaperdollService.cs`）+ 线上实例 `http://<NAS_IP>:38090` 实测。
> 前端已按本文约定**预留调用与 UI**，并在运行时探测服务端是否已支持：
> 字段/端点一旦上线，前端**无需改动**即自动启用（见每节的「前端现状与探测行为」）。
>
> **状态复核（2026-09-27 晚，曲库 BGM 控制轮实测）**：T2 / T4 / T5 已被服务端实现，
> 本文对应章节的「缺什么」已过期（保留作历史记录）：
> ```
> $ curl -s -X POST -H 'Content-Type: application/json' -d '{"type":"expression","value":"default"}' \
>     http://<NAS_IP>:38090/api/admin/devices/dev-693ea4/command
> {"ok":true,"seq":30,"type":"expression","value":"default"}          # HTTP 202（T2 已上线）
> $ curl -s http://<NAS_IP>:38090/api/admin/settings | jq '.config.device.imuSensitivity, .config.speech'
> 1          # T4 字段在位（未验证固件是否消费）
> {"enabled":false,"idleSec":300,"lines":[]}    # T5 配置段在位
> ```
> 影响：`Web/scripts/ui-smoke.mjs` 中「真实服务端尚无 T2/T4/T5」的 4 条断言因此转 FAIL
> （它们断言的是「服务端缺」，不是「前端错」）——**本轮未改这 4 条断言**，留待服务端任务收口时同步。
> **T6（bgm）仍未实现**（同一端点，但 type 白名单不含 bgm，见下）。

---

## T2：设备指令入队端点（动作 / 表情 / 气泡，E4「25 表情手动指定」）

### 缺什么（实测证据）

- 固件侧**已完备**：`Firmware/main/net/poller.c:141-210` 把 `commands[]` 摊平成
  `{t,v,n,u}` 后，`action` / `expression` / `bubble` / `map` / `brightness` / `reboot` / `bgm` / `ota`
  八类指令全部有落地分支；`expression` → `MP_CMD_SET_EXPRESSION` → `render_set_expression(name)`
  （`Firmware/main/app/state_machine.c:546`）。
- 服务端**已具备能力但无入口**：`CommandQueue.Enqueue(deviceId, type, payload)`
  （`Server/MinipetServer/Device/CommandQueue.cs:70`）存在，但唯一调用方只有
  `DeviceManifestService`（manifest 唤醒）、`AdminEndpoints` 的 `ota`（:242）与 `map`（:297/301）。
- 实测（2026-09-27，线上实例）：
  ```
  $ curl -s -o /dev/null -w "%{http_code}" -X POST -H 'Content-Type: application/json' \
      -d '{"type":"expression","value":"default"}' \
      http://<NAS_IP>:38090/api/admin/devices/dev-693ea4/command
  405        # 路由不存在（SPA fallback 对非 GET 回 405）
  ```
  对照：`POST /api/admin/devices/dev-693ea4/push {}` → `400 {"error":"kind 必须是 map 或 npc"}`
  （证明探测方法能区分「路由存在但入参错」与「路由不存在」）。

### 需要新增

| 项 | 值 |
| --- | --- |
| 方法 | `POST` |
| 路径 | `/api/admin/devices/{id}/command` |
| 请求体 | `{ "type": "expression" \| "action" \| "bubble", "value": "<表情名 / 动作名 / 气泡文本>", "durationMs"?: number }` |
| 成功响应 | `202` `{ "ok": true, "seq": <long>, "type": "<type>", "value": "<value>" }` |
| 失败响应 | `404` `{ "error": "设备不存在：<id>" }`；`400` `{ "error": "type 必须是 expression/action/bubble" }`、`{ "error": "value 必填" }`、`{ "error": "expression 必须是 25 表情之一" }`、`{ "error": "气泡文本超长（≤95 字节 UTF-8）" }` |

**实现要点（必须遵守，否则固件静默忽略指令）**

1. 入队 payload 必须是**裸 JSON 字符串**：`queue.Enqueue(id, "expression", "smile")`。
   固件读 `commands[].type` + `commands[].payload`（`poller.c:147-168`），
   `payload` 是字符串 → 直接当 `v`；也可用对象键 `id`（`{"id":"smile"}` 会被识别）。
   **`{"value":"smile"}` 这种形态固件不认**（只找 `id` / `ver` / `url` / `n`）→ 设备毫无反应。
2. `expression` 名必须取自 25 表情清单（服务端自己的 `PaperdollService.KnownExpressions`，
   `Services/PaperdollService.cs:63-74`）；不在清单内 → 固件 `rc_anim_set_expression` 打 warn 并保持原表情。
3. 气泡文本长度：固件 `mp_cmd_t.s` 是 `char[96]`（`Firmware/main/app/app_core.h:150`）
   → UTF-8 ≤ 95 字节（≈31 汉字）；超长应在服务端 400 拦掉（而不是截断）。
4. `durationMs`：固件 `MP_CMD_SET_EXPRESSION` 只消费 `s`（表情名），暂无时长通道；
   服务端可接受并忽略该字段（或后续走 `n` 通道扩展）。**前端目前只发 `type`+`value`。**
5. 建议同时支持 `action`（WZ 真实动作名，如 `walk1`/`fly`/`hit`）与 `bubble`，三者共用同一端点，
   便于 E12 手测与后续 agent 驱动。

### 前端现状与探测行为

- `Web/src/api/client.js`：`sendDeviceCommand()` / `probeDeviceCommand()` / `deviceCommandPath()` 已按上表实现。
- `Web/src/views/DeviceDetailView.vue`「表情 / 气泡调试（E4 25 表情手动指定）」卡片：
  - 开卡探测（`POST` 一条 `expression=default`，视觉无变化的空操作）：
    404/405/501 → 卡片状态标「端点缺失」，**25 个表情按钮全部 disabled** + 卡片内贴出本条需求；
    2xx → 标「端点在位」，按钮可点，点击后 toast 显示 `seq`；
    400/422/503 → 标「端点入参不符」（说明路由在、入参或依赖有问题）。
  - 25 表情清单逐字取自 `PaperdollService.KnownExpressions`，落在
    `Web/src/utils/expressions.js`（`key`/`cn`/`friendly` 三列）。
- **实测**：真实服务端（无端点）→ 25/25 按钮 disabled + 提示（`Web/scripts/ui-smoke.mjs` 断言）；
  用 `Web/scripts/preview-with-live-api.mjs` 的 `MINIPET_MOCK=1` 模拟端点上线 → 状态转「端点在位」、
  按钮全可点、点击「微笑」toast `指令已入队（expression=smile，seq 42）`。

---

## T4：`DeviceThresholdsConfig.ImuSensitivity`（E4「IMU 灵敏度」）

### 缺什么（实测证据）

```
$ curl -s http://<NAS_IP>:38090/api/admin/settings | jq '.config.device | keys'
["_comment","idleToClockMin","imuDeadzoneDeg","tapHardG","tapLightG"]
```
`Server/MinipetServer/Config/ConfigService.cs:38-47` 的 `DeviceThresholdsConfig` 只有
`ImuDeadzoneDeg` / `TapLightG` / `TapHardG` / `IdleToClockMin` 四个业务字段；
`DeviceEndpoints.cs:120-127`（hello → `config`）也只下发这四个 + `bgmDefaultSource` / `volume`。
固件侧 `g_mp_cfg`（`Firmware/main/app/app_core.h:189`）同样只有死区/轻拍/重拍/待机四项。

### 需要新增

| 项 | 值 |
| --- | --- |
| 配置字段 | `MinipetConfig.device.imuSensitivity`（`double`，默认 `1.0`，建议范围 `0.2–3.0`） |
| 语义（建议） | 灵敏度倍率：有效阈值 = 原阈值 ÷ 灵敏度（>1 更灵敏）。若采用其它口径（如档位 1–10），请把口径写进 `_comment` 并在实现后同步本文 |
| 下发通道 | `POST /api/device/hello` 响应的 `config.imuSensitivity`（与现有四字段同层） |
| 设备级覆盖 | 复用 `PUT /api/admin/devices/{id}` 的 `thresholds` 对象（`DeviceThresholdsConfig` 加字段即自动生效） |
| 固件消费 | `Firmware/main/net/http_client.c:334-343` 按 `json_num2(cfg, "imuSensitivity", NULL, &d)` 增加一行即可（可选：不消费则服务端字段对设备无影响，Web 仍可配置） |

### 前端现状与探测行为

- `Web/src/views/SettingsView.vue`「设备阈值」卡片与 `Web/src/views/DeviceDetailView.vue`「阈值」卡片
  各有一个「IMU 灵敏度（倍率，越大越灵敏）」输入框。
- 运行时探测：`config.device.imuSensitivity != null`（设置页）/ `device.thresholds.imuSensitivity != null`（详情页）。
  - **未探测到（当前真实态）**：输入框 disabled + 卡片内 `n-alert` 说明缺口，且
    **PUT /admin/settings、PUT /admin/devices/{id} 都不会带 `imuSensitivity` 字段**
    （不硬塞发不出去的字段）。
  - 探测到：自动启用，随 PUT 一并下发。
- **实测**：真实服务端 → 打开「覆盖全局阈值」后 5 个阈值输入中 4 个可编辑、灵敏度仍 disabled + 说明；
  `MINIPET_MOCK=1`（模拟字段上线）→ 5/5 可编辑、占位说明消失。

---

## T5：随机台词气泡（E12「静置久了冒预设台词气泡，文本 Web 配置」）

### 缺什么（实测证据）

- 配置模型无台词段：`MinipetConfig`（`ConfigService.cs:61-68`）只有
  `Wz` / `QqMusic` / `Bgm` / `Device` / `Clock`；线上 `GET /api/admin/settings` 返回的
  `config` 同样只有这五段（`speech` 字段为 `null`）。
  且 `ConfigService.Replace`（`:196-204`）**只拷贝这五段** → 前端即便塞 `speech`，PUT 后也会被静默丢弃。
- 无调度：服务端没有任何「静置计时 → 下发 bubble」的逻辑；`AdminEndpoints` 也无 bubble 入队入口。
- 固件侧**已就绪**：`poller.c:186` 的 `bubble` 分支 → `MP_CMD_BUBBLE` → 气泡渲染。

### 需要新增

**① 配置段 `speech`（随 `PUT /api/admin/settings` 读写）**

```json
"speech": {
  "_comment": "静置台词气泡（E12）：enabled=开关；idleSec=无交互多少秒后冒一句；lines=台词库（单条 ≤95 字节 UTF-8，≤50 条）",
  "enabled": true,
  "idleSec": 60,
  "lines": ["今天也要加油哦", "摸摸头～"]
}
```
（`ConfigService.cs`：`MinipetConfig` 加 `SpeechConfig Speech`，`Normalize` 补 `??= new()`，
`Replace` 加一行 `c.Speech = incoming.Speech;`——否则前端保存会被丢弃。）

**② 静置调度（服务端）**

- 触发口径建议：设备最近一次 poll/心跳/事件（`DeviceRegistry.Touch`）距今 ≥ `speech.idleSec`
  **且** 期间无 action/expression/bubble 指令下发 → 随机挑一条 `lines`，
  `queue.Enqueue(deviceId, "bubble", line)`（payload 必须是**裸字符串**，见 T2 要点 1）。
- 频控建议：同一设备两次自动气泡间隔 ≥ `idleSec`，避免刷屏；设备离线不排。

**③ 手动测试入口**：直接复用 T2 的 `POST /api/admin/devices/{id}/command`
（`{"type":"bubble","value":"…"}`）——前端「表情 / 气泡调试」卡片已经有气泡输入框与字节计数。

### 前端现状与探测行为

- `Web/src/views/SettingsView.vue`「随机台词气泡（E12…）」卡片：开关 / 静置秒数 / 台词库 textarea
  （逐行解析，单条 UTF-8 ≤95 字节、≤50 条，**前端做真实 `TextEncoder` 字节校验**）。
- 运行时探测 `config.speech != null`：
  - **未探测到（当前真实态）**：整卡 disabled + 醒目说明（含上述①②③的需求摘要）；
  - 探测到：表单启用，随 PUT /admin/settings 全量同构回传（`speech` 段参与往返）。
- **实测**：真实服务端 → 卡片 tag「待服务端支持」+ 需求说明可见、输入禁用；
  `MINIPET_MOCK=1`（模拟 speech 段）→ tag「配置段在位」、开关/秒数/台词 textarea 启用且回填两条台词。

---

## T6：BGM 播放控制 admin 下发（E8 附加：曲库页「设备播放控制」卡）

> 背景：E8 定稿「控制入口在设备触摸屏（菜单内 BGM 入口 → 半屏控制条），Web 只管曲库/歌单/cookie」。
> 用户实测反馈「服务器页面也没有播放 bgm 的按钮」→ 本项是**超出需求的附加能力**，
> Web 侧按「只读展示 + 可选远程下发」实现，并**探测到才启用**。

### 缺什么（实测证据）

- 固件侧**播放/暂停/切歌已完备**，`Firmware/main/net/poller.c:208-217`（`commands[].type + payload`
  通道）的 `bgm` 分支认 6 个**字符串**值：`play` / `pause` / `resume` / `stop` / `next` / `prev`
  → `MP_AUDIO_*`（`Firmware/main/app/app_core.h:165`；处理见 `Firmware/main/audio/bgm.c:568-660`）。
- 服务端 `POST /api/admin/devices/{id}/command` **端点已在位**（T2 已上线），但 type 白名单**不含 bgm**：
  ```
  $ curl -s -w ' [HTTP %{http_code}]' -X POST -H 'Content-Type: application/json' \
      -d '{"type":"bgm","value":"play"}' \
      http://<NAS_IP>:38090/api/admin/devices/dev-693ea4/command
  {"error":"type 非法：bgm（可用：expression/action/bubble/brightness/reboot）"} [HTTP 400]
  ```
  （`Server/MinipetServer/Api/AdminEndpoints.cs:281-284` 的兜底分支）。
- **音量缺两端**（重要，勿只改服务端）：
  1. 服务端：白名单不含 bgm，且 `vol` 需要数值通道 `n`；
  2. 固件：`poller.c:208` 的 bgm 分支条件是 `cJSON_IsString(vitem)`，而 `poller.c:171-173` 对
     `{"n":50}` 形态会把 `vitem` 置 NULL → bgm 分支不匹配 → **静默丢弃**（既不出声也不报错）。
     `vol` / `source` 目前只在 `poller.c:77-88` 的**旧扁平通道**（`{t,v,n}`）有分支，而
     `CommandQueue` 只会产出 `{seq,type,payload}` 形态，永远走不到那条路。
     → 音量要真生效，固件需在 `poller.c:208` 的 bgm 分支补 `pn` 支持，例如
     `else if (strcmp(tbuf,"bgm")==0 && (!cJSON_IsString(vitem) && pn && strcmp(pid?...,"vol")...))`
     的等价判断（按 §请求体约定：`{"type":"bgm","payload":{"n":50}}` → `MP_AUDIO_VOL, m.a = n`）。
- 只读展示侧的缺口（可选，不阻塞）：设备现场控制的回传 `POST /api/device/bgm/cmd`
  （`DeviceEndpoints.cs:242-273`）只更新 `dev.Bgm` 偏好，**不写设备事件日志**
  （`HandleBgmCmd` 没注入 `DeviceEventLog`）→ Web「设备事件」里看不到「设备上按了播放/调了音量」。

### 需要新增

| 项 | 值 |
| --- | --- |
| 方法 | `POST`（复用 T2 已上线的端点，只扩 type 白名单） |
| 路径 | `/api/admin/devices/{id}/command` |
| 请求体（播放控制） | `{ "type": "bgm", "value": "play" \| "pause" \| "resume" \| "stop" \| "next" \| "prev" }` |
| 请求体（音量，需固件同补） | `{ "type": "bgm", "value": "vol", "n": 0-100 }` |
| 成功响应 | `202 { "ok": true, "seq": <long>, "type": "bgm", "value": "<value>" }`（vol 建议回显 `n`） |
| 失败响应 | `400 { "error": "bgm 的 value 非法：<v>（可用：play/pause/resume/stop/next/prev/vol）" }`；`400 { "error": "bgm vol 需要 n∈[0,100]" }`；`404 { "error": "设备不存在：<id>" }` |

**实现要点**

1. `play/pause/resume/stop/next/prev` 的 payload 必须是**裸 JSON 字符串**：
   `queue.Enqueue(id, "bgm", "play")`；写成 `{"value":"play"}` 固件不认（与 T2 要点 1 同）。
2. 音量 payload 用对象 `{ n = ... }`（`queue.Enqueue(id, "bgm", new { n = body.N })`）——
   与 `brightness` 同款数值通道；**但需固件补 `pn` 分支才会生效**（见上「缺什么」）。
3. 探测约定（前端已按此实现，请勿改语义）：前端开卡发
   `{"type":"bgm","value":"__probe__"}` 哨兵值——
   - 服务端**未放行**时按现状回 `400 type 非法：bgm…` → 前端判「端点不支持 bgm」并禁用按钮；
   - 服务端**已放行**时，哨兵值会被 value 白名单拒 → `400 bgm 的 value 非法：__probe__…`
     → 前端判「端点在位」（`probeRejected`，这是**预期**响应，不是失败）；
   - 2xx 也判「端点在位」。哨兵值固件不认（`m.type` 保持 `MP_AUDIO_NONE` → 不 `mp_post_audio`），
     故探测对设备**零副作用**（不出声、不改音量）。
4. 建议顺带（可选项）：`HandleBgmCmd` 增加一行
   `eventLog.Append(deviceId, $"BGM：{cmd}（设备现场控制）{(trackId>0?$"，曲目 {trackId}":"")}")`，
   这样 Web 的只读展示能反映「设备触摸屏上做了什么」。

### 前端现状与探测行为

- `Web/src/api/client.js`：`sendBgmCommand()` / `probeBgmCommand()` / `setDeviceBgmPrefs()` /
  `getDeviceLogs()` / `BGM_COMMAND` / `BGM_PROBE_VALUE`；`sendDeviceCommand()` 扩了 `opts.n`（数值通道）。
- `Web/src/views/MusicView.vue` 顶部「设备播放控制（E8 附加：只读展示 + 可选远程下发）」卡片：
  - 只读：设备在线态 + BGM 偏好（`GET /admin/devices/{id}` 的 `bgm.source/volume`）+
    设备事件（`GET /admin/logs/{id}` 最近 5 条）；
  - 下发：`▶播放 / ⏸暂停 / ⏯续播 / ⏹停止 / ⏮上一首 / ⏭下一首 / 下发音量`（7 个按钮）——
    `bgmSupport === 'missing'` 时**全部禁用** + 卡内贴出本节需求；`ok` 时全部启用；
  - 音量另有**已在位**通道：`存为设备偏好` → `PUT /api/admin/devices/{id}` body
    `{"bgm":{"volume":50}}`（服务端 `AdminEndpoints.cs:44-48`，HTTP 200 实测）——
    只改服务端偏好，**非即时下发**（固件 hello 只消费阈值四件套，不读 `config.volume`，
    见 `Firmware/main/net/http_client.c:363-383`）。
- **实测**（2026-09-27）：
  - 真实实例：卡片探测 → 标「端点不支持 bgm」，7 键 disabled、卡内贴出本节需求；
    「存为设备偏好」可点，真实 `PUT` 回 200（`ui-smoke.mjs` T6 三条断言 PASS）。
  - mock 实例（`PORT=5198 MINIPET_MOCK=1`，按本节形态模拟）→ 探针把「400 bgm 的 value 非法」
    判为端点在位、7 键全启用、点「播放」真实发出
    `POST /api/admin/devices/dev-693ea4/command body {"type":"bgm","value":"play"}` → 202 seq 42。

---

## 附：可选的性能优化请求（不阻塞任何验收）

**设备列表带 `petConfig`（T1 的 N+1 优化）**

- 现状：`GET /api/admin/devices`（`AdminEndpoints.cs:435-452` 的 `DeviceCard`）只回
  `hasPetConfig` 布尔，不含 `petConfig` 本体；前端为了让卡片缩略图反映真实装扮，
  必须对每台设备再发一次 `GET /api/admin/devices/{id}`（结果按 deviceId 缓存，换装后失效重取）。
- 请求（可选）：`DeviceCard` 增加 `petConfig` 字段（或提供 `?withPetConfig=true`），
  设备数多时可省掉 N 次详情请求。**前端当前 N+1 方案在 1–20 台设备规模下已实测可用**（见下）。

**push 支持 `kind=mob`（可选）**

- 现状：`POST /api/admin/devices/{id}/push` 仅接受 `kind=map|npc`（`AdminEndpoints.cs:262-264`），
  素材页「怪物」tab 与「纸娃娃部件」tab 的 📤 按钮因此为**禁用态**（tooltip 说明）。
  若 E7 需要怪物上机，需扩 `DeviceAssetService.EnsureMobAsync` + 端点 kind 白名单。

---

## 附：本文档涉及的自测入口（可复跑）

```bash
cd Web
npm run build                                             # 产物 dist/
node scripts/preview-with-live-api.mjs                    # 真实服务端 API → http://127.0.0.1:5199
PORT=5198 MINIPET_MOCK=1 node scripts/preview-with-live-api.mjs          # 模拟 T2/T4/T5 已补齐
PORT=5197 MINIPET_MOCK=1 MOCK_PUSH_FAIL=503 node scripts/preview-with-live-api.mjs  # 推送错误分支注入
node scripts/ui-smoke.mjs --base http://127.0.0.1:5199 \
     --mock-base http://127.0.0.1:5198 --push-fail-base http://127.0.0.1:5197 --push
```

> T6（bgm 播放控制）自测点已并入 `ui-smoke.mjs`：真实实例 3 条（端点不支持 → 7 键禁用 + 需求文案 /
> 「存为设备偏好」可用 / 真实 PUT 200）、mock 实例 3 条（端点在位 + 7 键启用 / 播放 202 / 音量 202）。
