# MiniPet ESP32 修复自测报告（2026-09-27）

> 范围：本轮按胶水指令修复的三类真机问题 + E 系列需求缺口补全。
> 规则：本报告只写**实际跑过**的验证；未跑过的明确标注"未验证"，禁止"代码写完即报完成"。
> 设备：`dev-693ea4`（NAS 服务端 `http://<NAS_IP>:38090`）/ 端口 `/dev/cu.usbmodem21201`

---

## 一、结论速览

| # | 问题 | 状态 | 验收证据 |
|---|---|---|---|
| 1 | 配对成功但**无心跳** | ✅ 已修复（真机实测） | 服务端 `online=true` + poll 心跳持续刷新 |
| 2 | 事件上报**恒空** | ✅ 已修复（真机实测） | events 表 0 → 20+ 条（boot/asset_error 真实落库） |
| 3 | 素材包下不到设备 | ✅ 已修复（真机实测） | PARTS 936KB + LAYOUT 已落 TF 并成功加载 |
| 4 | 触摸方向与显示不同向 | ✅ 已修复（真机实测） | raw 与屏幕坐标 1:1，胶水确认"拖动没问题" |
| 5 | 每次操作方向不一致 | ✅ 已修复（真机实测） | 关闭点屏自动轮播（根因）+ NVS v3 强制迁移 |
| 6 | 启动时菜单呼不出 | ✅ 已修复（待真机复验） | SELF_TEST/WIFI_PROVISION 允许进菜单 |
| 7 | 屏幕刷新有裂纹 | ✅ 已修复（待真机复验） | SPI 队列深度 3→1，消除同屏多笔交叠 |
| 8 | BGM 端到端不通 | 🟡 部分（契约已修，NAS 未部署新代码） | 4 处契约错位已修；QQ 网关本体仍缺 |
| 9 | E7 设备选择器 | 🟡 大幅补全（T5 缩略图未做，有明确障碍） | 真实数据源/NPC tab/cached/离线置灰/拉包再切换 |
| 10 | E4/E12 Web 缺口 | 🟡 Web 已就绪，卡服务端字段 | 本轮已补 `POST /devices/{id}/command` 端点 |

---

## 二、三条真机主链路修复（设备实测）

### 2.1 心跳断点：三处 URL 缺 `deviceId`

**根因**（服务端实测）：`/api/device/{manifest,poll,bgm/stream}` 的处理器签名要求必填 `deviceId`，固件三处都不带 → **400**。

```
GET /api/device/poll?since=0                      → 400   ← 固件原写法
GET /api/device/poll?deviceId=dev-693ea4&since=0  → 200   ← 修复后
GET /api/device/manifest（无参）                   → 400
GET /api/device/manifest?deviceId=dev-693ea4      → 200（rev 12 / 19 资产）
```

**改动**：`asset_dl.c:551`（manifest）、`poller.c:118`（poll）、`bgm.c:355`（BGM 流）三处补 `?deviceId=%s`（`mp_http_device_id()`）。

**验收**：服务端 `lastSeen` 从"停在 01:26:04" → 恢复周期性刷新；`online` 由 false → true。

### 2.2 事件上报：报文形状与服务端 DTO 不符

**根因**：固件发 `{proto,deviceId,events:[{type,ts,a,b,s}],cache:[...]}`，服务端 `DeviceEventRequest` 只认 `{deviceId,type,tsUtc,data,hashes}` 的**单事件**形状 → `type` 为空 → **400**。

**实测对照**（同一端点，两种形状）：
```
固件形状 {"events":[{"type":"touch_pet",...}],"cache":[...]}   → 400 {"error":"deviceId 与 type 必填"}
服务端形状 {"type":"probe_test","tsUtc":"...","data":{...}}     → 200 {"ok":true,"manifestRev":10}
```

**改动**：`events.c` 改为批内**逐条**按服务端 DTO 发送（`tsUtc` 由 uptime 反推 UTC；`a/b/s` 折进 `data`；`hashes` 随首条捎带）。

**验收**：`/api/admin/logs/dev-693ea4` 的 `events` 由 **0 条 → 20 条**，含真实 `boot`、`asset_error` 记录；`cached=true` 标记同时被点亮。

### 2.3 触摸方向 + 每次方向不一致

**根因（两层）**：
1. 显示已固化为面板原生方向（`swap=false` + 无镜像），而触摸默认候选 4/6 = `(ry, rx)` **交换 X/Y 轴**（转置）→ 与显示差 90°。
2. `tmap_tap_advance()` 绑在 POKER 态的**每一次触摸按下沿**（`input_dispatch.c:636`）→ 每点一下映射轮换一格 → **方向每次都变**（用户原话："我每次操作的方向都不一致"，是直接证据）。

**改动**：`MP_TOUCH_CALIB 1→0`（关闭自动轮播）；`s_tmap_k` 恒等映射；`tmap_init` 加 **v3 强制迁移**（旧的转置映射一律纠正为恒等）。

**验收（真机日志 + 胶水确认）**：
```
W (1425) tmap: 触摸映射组合 0/7（0=恒等，与显示同向）
I (14942) touch: down raw=(42,84)  map=(42,84)   → 象限【左上】   ← 点左上角
I (16973) touch: down raw=(459,440) map=(459,440) → 象限【右下】  ← 点右下角
```
胶水确认：**"拖动没问题"**。

---

## 三、崩溃修复（阻断级）

**症状**：刷入协议修复后设备**启动即崩溃重启**（12s 内 4 次）。

**定位**（符号化回溯 + IDF 源码）：
```
wl_flash: read(610): result = 0x00000101  →  ESP_ERR_NO_MEM
abort() at lock_init_generic (newlib locks.c:77)
回溯：render_task → dispatch_manifest_synced → render_set_layout → mpak_open → fopen → __sfp → abort
```

**根因**：SD 卡不在 → 回退挂内部 Flash `assets` 分区时，WL 层在内部堆被 WiFi/LVGL 挤压的情况下分配失败；`render_set_layout` 打开布局文件时 newlib stdio 锁分配失败 → abort。

**改动**：`sd_tf.c` 的 Flash 回退挂载失败改为**优雅降级**（记堆水位、返回 ENODEV，不阻断启动）。

**验收**：刷入后设备持续运行（一次采样 85 行日志、115s 无异常），PARTS/LAYOUT 正常加载。

---

## 四、本轮另外两条修复（待真机复验）

### 4.1 启动时菜单呼不出
**根因**：`MP_SM_EV_MENU_KEY` 只认 POKER/OFFLINE/MENU/CLOCK_DOZE，而开机自检期状态是 `MP_ST_SELF_TEST`（WiFi 探测最长 20s）→ 按键被 `default: break` 丢弃。
**改动**：`state_machine.c:335` 允许 `SELF_TEST`/`WIFI_PROVISION` 进菜单（退出仍回 POKER）。

### 4.2 屏幕刷新裂纹
**根因**：SPI 队列深度 3，一帧被拆成约 18 笔连续入队，面板在上一笔未扫出时就收到下一笔的窗口切换 → 同屏两笔交叠。
**改动**：`display_co5300.c` `TX_QUEUE_DEPTH 3→1`（每笔完成才发下一笔）。
**未验证**：需真机目视确认裂纹是否消失（本轮无照片）。

---

## 五、需求缺口补全（agent 并行交付）

### 5.1 服务端（T1–T5 ✅，NAS 未部署新代码）

| 项 | 结果 | 证据 |
|---|---|---|
| AUDIO_META kind 串 | ✅ | `MpakKindNames.DirName()` 特判；NAS manifest 出现 `"kind":"AUDIO_META"`（126088B / 1167 曲） |
| FONT 字体包 | ✅ | 3 档（16/24/32px）进 manifest 并可下载；本机实例新设备 hello 后自动补齐 |
| 首启 provisioning | ✅ | 新 UUID hello → PARTS×2 + LAYOUT×11 + FONT×3，二次 hello 幂等 |
| QQ 网关 | 🟡 | 进程管理/健康/重启 1 次/取链校验/流式转发已实现；**网关本体不在仓库** → Degraded + 明确原因 |
| cookie 有效期告警 | ✅ | `cookieSavedAtUtc` + 7 天阈值 + `cookieStale`；PUT 回传不丢时间 |

### 5.2 Web（T1–T3 ✅ / T4–T5 🟡 卡服务端）
- 设备卡片缩略图按真实装扮（实测图片字节数 23124→30495 变化）；素材推送按钮接通真实 push（202 + 服务端留痕）；25 表情/灵敏度/台词为占位+探测（服务端字段缺失时禁用且不下发）。
- 构建：`npm run build` ✅；CDP 真实浏览器冒烟 13/13 PASS。

### 5.3 固件选择器 E7（T1–T4 ✅ / T5 未做）
- 真实数据源（删掉写死 demo）、NPC tab、cached 三态标记、离线置灰、单包下载后再切换。
- **T5 缩略图未做**（三条硬障碍有源码证据）：THUMB 是**裸 PNG 无 MPAK 信封**（现有校验会误判损坏并污染事件流）；`LV_USE_LODEPNG` 未开；菜单行无 image 控件。

### 5.4 本轮我补的服务端端点（Web 已就绪，一通即自动启用）
新增 `POST /api/admin/devices/{id}/command`，**payload 形状与固件 `poller.c:182-197` 逐字对齐**：
```
expression/action/bubble → 裸 JSON 字符串（写成 {"value":…} 会被固件静默忽略）
brightness              → {"n":0..100}
reboot                  → {}
```
本机实例实测：`expression:"smile"` 202 → poll 取回 `payload='smile'`；`brightness n=70` → `payload={'n':70}`；缺 value 400 / 非法 type 400 / bubble 超 95B 400。

### 5.5 看门狗与口径
- `CONFIG_ESP_TASK_WDT_PANIC` 关闭（与设计一致，诊断期回溯会消失）；熔断后**真正停渲染**（`watchdog_render_allowed()` + 挂起渲染任务）；启动宽限 20s 防慢启动误判。
- E10 低电表情口径定稿为 `troubled`（文档已改）；离线/回网状态迁移补事件上报。

---

## 六、未验证 / 遗留（不编造）

1. **菜单按钮**：胶水反馈"菜单按钮不正常"仍需复验——本轮修了启动呼出与触摸映射，但**菜单行点击**的真机确认未拿到（需要点一下菜单行并回读 `row_click`/`menu_rebuild` 日志）。
2. **裂纹**：队列深度修复**未目视确认**。
3. **NAS 部署**：服务端新代码（字体自动补链/首启 provisioning/指令端点/cookie 告警）**未在 NAS 生效**——NAS 跑的是烘死的镜像，本机无 docker、NAS docker 需密码。设备当前能拉到 FONT/AUDIO_META 是本轮**手工投放产物**的结果。
4. **QQ 网关本体**不在仓库，线上 qq 源为 Degraded。
5. **E9 SNTP 常态化**：设备日志实测 `clock: 时间未同步：系统时间无效且 RTC 未校准，时钟显示 --:--` —— 配网后 RTC 未回填，常态重校准仍未做。
6. **E7 T5 缩略图**、**E7 切换后随机表情**（受菜单期表情冻结限制）未做。
7. 中文 label 在菜单仍渲染为空白（菜单字体仅拉丁字形），需绑定 TF FONT 包 + 行高重排。

---

## 七、复现命令（验收用）

```bash
# 环境
source /Users/<USER>/esp/esp-idf/export.sh
cd /Users/<USER>/IdeaProjects/minipet-esp32/Firmware
PY=/Users/<USER>/.espressif/python_env/idf5.5_py3.9_env/bin/python

# 构建 + 烧录
idf.py build && idf.py -p /dev/cu.usbmodem21201 flash

# 抓日志（含触摸/菜单/tmap 锚点）
$PY -c "
import serial,time
s=serial.Serial('/dev/cu.usbmodem21201',115200,timeout=0.5); t=time.time(); b=b''
while time.time()-t<20:
    d=s.read(4096)
    if d: b+=d
s.close()
[print(l) for l in b.decode('utf-8','replace').splitlines()
 if any(k in l for k in ('tmap','touch','menu','boot done','FONT','AUDIO','http:'))]"

# 服务端验收
curl -s "http://<NAS_IP>:38090/api/admin/devices"          # online=true
curl -s "http://<NAS_IP>:38090/api/admin/logs/dev-693ea4"  # events 非空
curl -s "http://<NAS_IP>:38090/api/device/manifest?deviceId=dev-693ea4"  # rev + 23 资产
```
