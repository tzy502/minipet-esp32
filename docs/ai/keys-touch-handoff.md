# 按键定位与触摸修复交接（2026-09-27）

> 本文档记录本轮真机联调中**按键硬件归属**的实测结论与**触摸链路**的修复。
> 规则：只有真机日志/实测支撑的才写入；推测一律标注「待验证」。

---

## 一、板上三键（wiki 权威 + 实测）

板型：微雪 ESP32-S3-Touch-AMOLED-2.16（480×480 / CO5300 / 8MB Octal PSRAM）

| 物理位置 | wiki 名称 | 硬件连接 | 固件当前处理 | 实测状态 |
|---|---|---|---|---|
| 上（照片标注"菜单"） | Key3 | **GPIO18**（10K 上拉，按下接地） | 短按=呼出菜单/确认；长按=待机时钟 | ✅ 工作正常（日志 `菜单键：POKER -> MENU`） |
| 中（照片标注"理论向上"） | Key2 | wiki：`GPIO0` / `CHIP_PU` 同接 | 菜单内=光标上移 | ❌ **不在 GPIO0**（见下） |
| 下（照片标注"理论向下"） | Key1 | `PWRON`（PMU 电源键输入） | 菜单内=光标下移 | ❌ AXP2101 IRQ 状态位从未变化 |

### 1.1 中键：不在 GPIO0

**实测证据（多轮采样）**：

```
I (30832) key0: 取证 GPIO0=1 按下沿累计=0
I (33838) key0: 取证 GPIO0=1 按下沿累计=0
...（持续数十条，用户按键期间完全不变）
```

- GPIO0 恒为 **1（高/松开）**，`key_gpio0_tick()` 按下沿累计恒 **0**
- keyscan 常驻探针（候选 0/16/47/48）在用户按键期间**零跳变**
- 结论：**固件一直在监听一个空脚**，中键与 GPIO0 无关

**关键新证据**：用户按住中键时，`/dev/cu.usbmodem21201` **整个从 USB 消失**
（`OSError: [Errno 6] Device not configured`，随后 `ls` 无此设备）——
说明按中键会导致**整机断电/复位**（USB 重新枚举），固件不可能感知该按键。

> ⚠️ 这与用户最初的描述一致（"中键完全没反应也不重启"→ 实为整机掉电，
> 观感上"没反应"）。**待用户重新上电后复测确认**：中键是否为纯硬件复位/电源键。

### 1.2 下键（PWRON）：AXP2101 状态位不锁存

**实测证据（43 次连续采样，I2C 无错 e=0/0/0）**：

```
I (1037) axp2101: AXP2101 原始值 status0=0x20 status1=0x15 batt%=0
                  irq0..3=[0x00 0x00 0x00 0x00] reg27=0x14 reg28=0x00
I (13704) axp2101: 取证 irq[44..47]=[00 00 00 00] reg27=14 reg28=00
...（43 条完全一致，用户按键期间不变）
```

- 0x44..0x47（IRQ 状态 0..3）恒 0x00；reg27=0x14（PWRON 配置）恒定
- 结论：**PWRON 引脚状态位从未锁存** → 下键也不是走 AXP2101 IRQ 这条路
- 对照（已排除的假设）：IRQ 使能寄存器（0x40..0x43）只门控中断引脚输出，
  不影响状态锁存，因此"没开中断"不能解释状态位恒零

### 1.3 下一步定位方法（用户重新上电后）

1. 重新插电/上电，确认 `/dev/cu.usbmodem21201` 回来
2. 用常驻 keyscan（当前候选 0/16/47/48）逐一按三键，记录哪个脚跳变：
   - 若某脚跳变 → 即该键的真实引脚，改 `KEY_GPIO0_PIN` 指向它
   - 若全无跳变且按键导致 USB 消失 → 该键是硬件电源/复位键，**固件无法使用**
3. 若确认中键/下键都是硬件键 → 侧键分工改为：**仅用 Key3(GPIO18) 单键操作**
   （短按=移动光标，长按=确认），触摸负责点击（见第二节）

---

## 二、触摸链路（本轮已修，需复测）

### 2.1 "点哪都是第一个 map"——根因已定位并修复

**根因**：`input_dispatch.c` 的 `touch_tick()` 分支顺序写反：

```c
if (state_machine_menu_open()) { ...; return; }   // ← 先跑，什么都不喂
...
if (tst != MP_ST_POKER && tst != MP_ST_OFFLINE) {
    if (read_ok) lv_bridge_touch_feed(f.x, f.y, f.touched);   // ← 永远到不了
    return;
}
```

菜单态是 `MENU`，第一道门就把触摸吞掉 → LVGL indev 永远收不到按下 →
点任何按钮都不触发 `CLICKED` → 只剩侧键确认，而确认作用于 `sel`（初始 0）
→ 现象"无论怎么点都是第一个 map"。

**修法**：菜单分支内先 `touch_read_frame` → `lv_bridge_touch_feed(f.x, f.y, f.touched)`
（读失败时补发一次"抬起"，防 LVGL 侧残留 PRESSED），再复位宠物交互状态。

### 2.2 光标不动——根因与修法

- 曾把 `render_menu_nav` 改成"到边界停住"，而中键=上移、初始 `sel=0`
  → 每次按键都命中 `nav(0) 已到首行 0/6：停住`，光标被焊死
- 已恢复**回绕**（首行再上移 → 末行 Exit）
- 另加**文字光标**（选中行前缀 `> `），不依赖样式渲染，确保光标移动可见

### 2.3 触摸映射（已定稿）

- 显示已固化为面板原生方向（`swap=false` + 无镜像）
- 触摸映射**恒等**（候选 0）：`raw → screen` 1:1，与显示同向
- 已关闭"点屏自动轮播"（`MP_TOUCH_CALIB=0`）——它会每次触摸切候选，导致方向漂移
- NVS 加 v3 强制迁移，旧的转置映射自动纠正

---

## 三、本轮修掉的其他真机问题（均有日志证据）

| 问题 | 根因 | 状态 |
|---|---|---|
| 配对成功但无心跳 | poller 任务先于 `state_machine_boot()` 启动，`mp_http_init()` 未跑 → `mp_http_server_url()` 恒 NULL → `http_txn` 静默 return -1，**从未轮询过** | ✅ 修复（poller 自行 init） |
| 心跳忽上忽下 | poll 超时走 60s 指数退避，越过服务端 90s 在线窗口 | ✅ 超时改 5s 快速重试 |
| 事件上报恒空 | 报文形状与服务端单事件 DTO 不符 → 400 | ✅ 改单请求批量（顶层 type + data.batch[]） |
| 网络栈瘫痪 | 每事件一个 POST + keep-alive 常驻 → `errno=105 No buffer space` | ✅ 批量 + POST 关 keep-alive |
| 素材下不到 | manifest/poll/bgm-stream 缺 `deviceId` → 400 | ✅ 三处补参 |
| 启动崩溃循环 | main 任务 3.5KB 栈跑 WiFi+TLS+cJSON → 栈溢出 | ✅ 起 16KB `mp_main` 任务 |
| **扫描探针致重启循环（本轮自伤）** | 把 **GPIO33/34**（Octal PSRAM 数据脚）配成 GPIO 输入 → 打断 PSRAM 总线 → WDT 复位 | ✅ 撤销，排除项写进注释 |
| 待机时钟恒 `--:--` | SNTP 只在配网流程跑；已配网设备 RTC 未校准时永远等不到校时 | ✅ 新增 `provision_rtc_resync_start()`；实测 `SNTP synced: ... (Sun Sep 27 10:24:44 2026)` + `RTC set OK` |

---

## 四、当前待办

1. **用户重新上电**，复测中键/下键归属（第 1.3 节方法）
2. 触摸点击复测：菜单里点第 2/3 行 → 日志应出现 `menu: activate idx=N ... hash=...`
3. 刷新裂纹目视确认（SPI 队列深度 3→1 已烧录）
4. 服务端容器重启（NAS 上 38090 曾无响应）
5. 探针清理（验收后一次性）：`MP_KEY_SCAN_PROBE`、菜单帧耗时、高亮已贴、
   `key0 取证`、`axp2101 取证`、`probe: flush/ENTPOS`
6. E7 缩略图（THUMB 是裸 PNG 无 MPAK 信封 + 无 PNG 解码器）
7. NAS 部署新服务端代码（字体自动补链/首启 provisioning/指令端点）

---

## 五、提交记录（本轮）

```
0c63445 fix(fw): 撤销把 GPIO33/34（Octal PSRAM 数据脚）配成 GPIO 输入的重启循环 + 中键取证探针
2d2dfa4 fix(fw): 菜单态触摸被吞（点哪都第一项）+ 恢复光标回绕（中键"不动"）+ 菜单帧探针
754ca1b fix(fw): 事件上报改单请求批量 + POST 关 keep-alive（修网络栈瘫痪）
55e73ce fix(fw+server): 心跳双根因 / 菜单光标回绕 / 人物消失 / IMU灵敏度 / 常态化校时
bd4d3e8 fix(fw+server+web): 真机主链路打通 + 菜单/触摸/裂纹修复 + E4/E7/E12 缺口补全
```

---

## 六、2026-09-27 第二轮：网络层根因链（环境阻塞）

### 6.1 结论：设备被 AP 拒绝在**认证阶段**（非固件逻辑）

```
I (1612) wifi:state: init -> auth (0xb0)
I (2613) wifi:state: auth -> init (0x200)      ← 认证失败回退
W (2614) provision: WiFi 断开 reason=2
```

- `reason=2` = AP 未响应/拒绝认证帧；**密码错会是 15/202**，所以不是凭据问题
- Mac 侧同一 AP 完全正常（`ping <GATEWAY_IP>` 0% 丢包），AP 本身在线
- 设备历史（2026-09-26T18:04Z）曾成功 poll，说明硬件与凭据曾经可用

**已排除**：
1. 服务端地址配置 —— 设备日志实证 `hello 目标 http=http://<NAS_IP>:38090`（正确）
2. 服务端监听/防火墙 —— 0.0.0.0:38090 监听中，Mac curl 22ms 通
3. lwip 参数 —— `MAX_SOCKETS=16 / TCP_MSL=3000 / RECVMBOX=12` 已在 build header 生效
4. GPIO 冲突 —— 误配 GPIO33/34（Octal PSRAM 脚）已撤销
5. WiFi 省电 —— 已挪到 GOT_IP 之后调用（`WIFI_PS_NONE`）
6. PMF/认证阈值 —— 两边一致，threshold 已放宽到 `WIFI_AUTH_WPA_WPA2_PSK`

**建议用户侧动作（任一即可）**：
- 重启路由器/AP（清理该客户端的异常状态）
- 或对设备**重新配网**：`idf.py erase-flash`（会清 NVS → 设备进 SoftAP `MiniPet-XXXX`，手机连上填 WiFi + 服务器地址）

### 6.2 内部堆挤压（第二阻塞）

```
W (1075) provision: RTC 未校准（首次上电或读数无效），系统时钟暂为 1970，等 SNTP 校准
W (2601) provision: rtcsync 任务创建失败（内部堆挤压）→ 稍后重试
W (2607) bgm: bgm 任务首建失败（内部堆挤压），转 10s 周期自愈重试
E (1486) es8311: I2S 初始化失败: ESP_ERR_NO_MEM
E (2707) bridge: font 1 not loaded (render_set_font first)
```

`wifi_init_once` 时实测 **内部堆空闲 108KB / 最大块 34KB**。表现：
- **待机时钟恒 `--:--`**（校时任务起不来，已改为可重试）
- BGM 任务起不来 → 无声
- I2S 初始化失败 → 无声
- 气泡字体加载失败 → 气泡走 5x7 兜底

**已做的缓解**：清掉全部常驻诊断探针（keyscan/key0/axp2101/菜单帧/高亮/flush/ENTPOS）、
校时任务栈 4096→3072 且失败可重试。

**待办（下一轮）**：
- 内部堆水位探针（`wifi_init_once` 已打）逐项定位占用方
- 考虑：SDL/日志缓冲瘦身、LVGL 缓冲部分移 PSRAM、`CONFIG_LOG` 等级降级

---

## 七、【E14】设备发现 + 设备日志上报：服务端侧接口需求

> 本节由固件侧改动（Firmware/，2026-09-28）引出。**固件已实现，服务端未实现**；
> 下述端点/服务名是接口契约，需服务端补齐后「Web 可拉取设备日志」「设备自动
> 发现服务端」才真正闭环。固件侧代码位置已逐条标注，便于对照。

### 7.1 mDNS：服务端必须广告 `_minipet._tcp`

**需求原文（E14）**：「mDNS：设备可发现服务端（配网页手输地址的补充，可选启用）」

**设备侧行为（已实现）**：`Firmware/main/net/mdns_discover.c`
（`mp_mdns_discover_server()`，由 `app/state_machine.c` 自检期调用）

- 只在两种情况下查询：①NVS `srv_url` 为空（用户在配网页留空服务器地址）；
  ②`srv_url` 有值但 `hello` 连不上（服务端换 IP / 地址写错）
- **手输地址优先**：`srv_url` 有效且 hello 成功时一次 mDNS 都不发
- 服务名：**`_minipet._tcp`**（= `_minipet._tcp.local`，PTR/SRV/TXT）
- 超时：PTR 查询 1.1s；若该响应里没带 A 记录，再按 hostname 补一次 A 查询
  1.1s（合计 ~2.2s 上限）。失败**静默**（不刷日志、不重试、不拖慢启动）
- 命中后地址**不写 NVS**（避免顶掉手输配置 / DHCP 换 IP 后永远用旧值），
  只作本次运行的兜底
- 命中会打一条 INFO：`I (xxxx) mdns: 发现服务端: http://<ip>:<port> instance=... host=...`

**服务端需实现（.NET 9）**：

1. 广告服务类型 `_minipet._tcp`，端口 = 服务端 HTTP 监听端口（默认 **38090**）
2. **SRV 的 target/hostname 必须能被 A 记录解析**（设备在 PTR 响应未带 A 记录时
   会用 hostname 补查 A）。实践要点：广告时直接带上主机 A 记录，成功率最高
3. 建议 TXT（当前固件不读，预留给服务端版本协商）：
   - `ver=1`（协议版本，对应固件 `MP_PROTO_VER`）
   - `path=/api/device`（端点前缀）
   - `name=<服务端展示名>`（Web 里给用户看）
4. 实现库选型（三选一，按维护性排序）：
   - **`Makaretu.Dns`**（NuGet，最常用）：`new ServiceProfile(...)` +
     `ServiceDiscovery.Advertise(profile)`，跨平台、纯托管、无外部依赖；
     注意它同时做 responder 与 resolver，只需 Advertise 即可
   - **`Tmds.MDns`**：轻量，但只做浏览器（browse）+ 有限广告能力，
     若它不支持「自定义 SRV target + A 记录」则要自己补一条 A 记录
   - 系统级 Zeroconf（Linux `avahi-daemon` + `/etc/avahi/services/minipet.service`
     或 Windows Bonjour/`dnssd`）：不用改 C# 代码，但要求部署环境装 daemon，
     容器里还需 `--net=host` 或放行 UDP 5353 组播 —— **推荐作为兜底方案**
5. 网络前提（无论哪种实现）：服务端主机与设备**同一二层广播域**，UDP **5353**
   组播（224.0.0.251 / ff02::fb）双向可达；Docker 桥接网络默认收不到组播，
   部署侧需 `network_mode: host`（`docker-compose.example.yml` 可参考）
6. **不需要**服务端主动发现设备；设备不做 responder（不广告自己）

**验证方式（服务端自测，不依赖设备）**：
- Linux/Mac：`avahi-browse -rt _minipet._tcp` 或 `dns-sd -B _minipet._tcp`
- 期望看到实例名与端口；`dns-sd -L <instance> _minipet._tcp` 能看到
  hostname + port + TXT

### 7.2 设备日志：需新增两个端点

**需求原文（E14）**：「日志：设备环形日志缓冲，Web 可拉取（排障不用插线）；
串口日志仅开发期」

**固件侧行为（已实现）**：
- `Firmware/main/app/logbuf.c`：**定长槽位环**，152 槽 × 216B ≈ **32.8KB，全在 PSRAM**
  （`heap_caps_malloc(MALLOC_CAP_SPIRAM)`，内部动态堆常驻占用 0），经
  `esp_log_set_vprintf()` 挂接，每槽存（`seq` 递增序号 + `t` 设备毫秒 +
  `ts` epoch 毫秒 + `lvl` + `tag` + 文本 ≤168B）；满则覆盖最旧（最旧序号 =
  最新 − 151）；写路径不阻塞、不递归、不新建任务、ISR 内自动跳过
- 上报：`Firmware/main/net/http_client.c` 的 `mp_http_device_log_step()`，
  由**已在跑的 poller 任务**周期调用（`net/poller.c`，不新建任务）：
  20s 心跳；出现 E 级日志或首次上报立即触发；失败退避 60s 重传
  （失败**不推进游标** → 不丢日志）；单次 4s 超时，绝不拖慢长轮询
- 单批上限约 4.6KB（≈20 条），一条塞不进就留到下一批（游标滚动推进）
- 环容量语义：设备最多保留**最近 152 条**日志；服务端若不及时收，被覆盖的
  那部分就永久缺失（这是「环形缓冲」的固有取舍，排障场景够用）

**服务端需实现（两个端点）**：

#### (1) `POST /api/device/log` —— 设备增量上报（设备 → 服务端）

请求体（`Content-Type: application/json`）：

```json
{
  "proto": 1,
  "deviceId": "44BD8D60DAC0",
  "since": 128,             // 设备上次成功送达的最大 seq；本次上报 seq > since
  "count": 3,
  "logs": [
    { "seq": 129, "ts": 1769999999123, "t": 45678, "lvl": "I",
      "tag": "poller", "msgHex": "68656c6c6f" },
    { "seq": 130, "ts": 1769999999500, "t": 46055, "lvl": "W",
      "tag": "provision", "msgHex": "..." }
  ]
}
```

字段说明（**字段名大小写必须完全一致**，System.Text.Json web 默认 camelCase 绑定）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `proto` | int | 协议版本（当前恒 1） |
| `deviceId` | string | hello 返回的 deviceId（与 poll/event 同口径） |
| `since` | uint32 | 设备侧上次成功送达的 seq；服务端可用于去重校验 |
| `count` | int | `logs` 数组长度 |
| `logs[].seq` | uint32 | **设备内单调递增**序号（重启从 0 重新开始，见「遗留」7.3） |
| `logs[].ts` | uint64 | epoch 毫秒；**系统时间未校准时是开机毫秒**（值 << 1.7e12 即为未校时） |
| `logs[].t` | uint32 | 设备开机毫秒（本地时基，恒单调） |
| `logs[].lvl` | string(1) | `E`/`W`/`I`/`D`/`V` |
| `logs[].tag` | string | ESP-IDF 日志 TAG（≤15 字符） |
| `logs[].msgHex` | string | **日志正文的 hex（UTF-8 字节的十六进制小写）**，非明文 |

**为什么 `msgHex` 而不是明文**：固件侧要零堆分配、零转义地序列化任意日志文本
（日志里含引号/换行/中文），hex 编码是最省事且不丢字节的做法。
**服务端处理要点**：`Convert.FromHexString(msgHex)` → UTF-8 解码 → 存文本。
（若嫌 hex 占空间，可协商改成明文 + 服务端 `JsonElement` 原样收；固件侧改一行。）

**响应约定**：

| 状态码 | 含义 | 设备行为 |
|---|---|---|
| `200` | 已接收（body 可为 `{"ok":true}` 或空） | 推进游标 `since = logs[^1].seq` |
| 其他 | 拒收/未实现 | **不推进游标**，60s 后整批重传（幂等：seq 去重即可） |

**幂等要求**：设备会重传（网络抖动/服务端 404），服务端应按
`(deviceId, seq)` 去重后追加，或按 seq 覆盖写；重复批次不得产生重复行。

#### (2) `GET /api/admin/device-logs/{id}` —— Web 拉取（Web → 服务端）

需求：Web 端「排障不用插线」看设备日志。

- `{id}` = 服务端设备记录 id（Web 现有设备列表的主键口径），**不是** 设备 UUID；
  也可额外提供 `GET /api/admin/device-logs/by-uuid/{uuid}`
- 建议查询参数：`?sinceSeq=<n>&limit=<n>&level=<E|W|I>&tag=<tag>`
  （`sinceSeq` 与设备侧 `seq` 同口径，便于 Web 增量轮询）
- 建议响应：

```json
{
  "deviceId": "44BD8D60DAC0",
  "lastSeq": 131,
  "clockSynced": true,
  "items": [
    { "seq": 129, "tsUtc": "2026-09-28T10:12:03.123Z", "t": 45678,
      "lvl": "I", "tag": "poller", "msg": "poll ok" }
  ]
}
```

- 服务端需**每设备保留有界环缓**（建议 ≥512 行或 ≥64KB，超出丢最旧）——
  需求原文即「设备环形日志缓冲」，服务端是设备环缓的持久化副本；
  若直接落库，建议加索引 `(deviceId, seq)` 并定期裁剪

### 7.3 遗留与待真机验证（固件侧已知项）

1. **`seq` 在设备重启后从 0 重新开始**（环缓在 RAM）。服务端若按 seq 去重，
   需按「连接会话/启动周期」分段，或允许同 seq 覆盖写。若要求跨重启单调，
   固件侧可改为把 seq 存 NVS（有写放大，需权衡）——**待服务端口径确定后再定**
2. **`ts` 在校时前是开机毫秒**（RTC 未校准场景，见本文 §6.2）。Web 展示需按
   `ts < 1.7e12` 判未校时，回退用「服务端接收时间」
3. **mDNS 发现只在开机期做（1 次）+ hello 失败后兜底 1 次**，不在 poller 里
   周期重试。若「设备先开机、服务端后启动」且 `srv_url` 为空，需重启设备才会
   再发现（设计取舍：不给心跳路径加 mDNS 延迟）。若要覆盖该场景，服务端补上
   `_minipet._tcp` 广告后，可在 poller 的离线分支加「每 60s 一次发现」——
   注意 mDNS 查询最坏阻塞 2.2s，必须与心跳解耦。
4. **`msgHex` 使 body 体积翻倍**（每条正文 ×2）：单批因此约 20 条。
   若服务端愿意收明文（`JsonElement` 原样存），把 `http_client.c` 的
   `log_msg_to_hex()` 换成转义即可，单批条数可翻倍。
5. 上述两个端点与 mDNS 广告**均未在真机验证**（本轮仅 `idf.py build` 通过 +
   代码路径自检 + 主机 harness 逻辑验证；真机验证受网络环境阻塞）。真机首验
   建议看两条证据：① 串口/环日志出现 `I (xxx) mdns: 发现服务端: http://...`；
   ② 服务端出现 `POST /api/device/log` 且 200。

---

## 8. 设备联网卡点交接（2026-09-27 真机实测，未闭环）

### 8.1 已经修好并有证据的部分

| 项 | 证据 |
|---|---|
| Reset WiFi 后无限重启（`rst:0xc` / LoadProhibited `EXCVADDR=0x2c`） | 修复后连续多轮启动 `crashes: 0` |
| 配网页 192.168.4.1 打不开 | `curl http://192.168.4.1/` → 200 / 5276B / `<title>MiniPet 配网</title>` |
| 热点连上但拿不到 IP（只剩 169.254） | Mac 连上 3s 内拿到 `192.168.4.2` |
| 配网页只有响应头没 body | 由 15s 超时 size=0 → 1.7s / 5276B |
| 配网提交 | `POST /save` → 200 → 重启后 `WiFi GOT_IP: <PET_IP>` |
| 时钟 6h 漂移校时从未执行 | 已改为到点真校一次并回写 RTC |

关键改动：httpd 改到开机最早窗口启动且必须确认 80 端口真在监听（listen 失败是任务内
异步发生，只看 ESP32_OK 会得到"哑巴 httpd"）；dns53 bind 失败改重试（原来直接自杀
且句柄残留 → 手机不弹配网页）；mp_main/bgm/render/httpd 栈下调；HTTP 上报缓冲挪 PSRAM；
WiFi **RX** 缓冲回默认（缩容会引发 `reason=2` 认证失败，**不要再动 RX**），仅 TX 16→8。

### 8.2 未闭环：设备拿到 IP 后 TCP 一律失败

真机现状（本文件写作时）：

```
I (2749) provision: WiFi GOT_IP: <PET_IP>          ← 关联 + DHCP 正常
W (2751) http: hello 目标 http=http://<NAS_IP>:38090 deviceId=44BD8D60DAC0
W (10758) http: mp_http_tx_fail ret=0x7002 (ESP_ERR_HTTP_CONNECT) errno=0 phase=open
W (33004) probe: [网关] <LAN_IP>:80 connect=-1 errno=128 (Socket is not connected)
```

同时刻对照事实：
- Mac 侧 `curl http://<NAS_IP>:38090/api/health` → **200**；`nc -z <NAS_IP> 38090` 通
- Mac 侧 `ping <PET_IP>` → **0% 丢包 6~30ms**（设备整机在线、链路层活着）
- 设备侧连**自己的默认网关**的 TCP 都不通（errno 113 → 128 两次不同）
- 内部堆诊断：总 196363B / 空闲 2344B / **最大连续块 2036B**（100KB 段 0 空闲、179 个分配块）
- 设备侧同时出现 `W:m f null`（WiFi 管理帧分配失败）

已排除：NAS 防火墙/端口（Mac 同 URL 200）、路由（网关 ping 通）、DNS（用的是点分 IP）、
凭据（NVS 里 ssid/srv_url 正确）、RX 缓冲缩容（已回退）。

**下一步方向（按优先级）**：
1. 内部堆碎片是唯一还站得住的嫌疑：`connect()` 需要 lwIP 从内部堆取 TCP PCB/发送缓冲。
   建议做一次**启动期内部堆分配清单**（heap_caps 分段 + 各任务栈 + 组件 .bss 排序），
   找出 100KB 段被 179 个小块吃光的具体来源，而不是继续零敲碎打地调栈。
2. 若确认是 WiFi 驱动收包路径（`m f null` + TCP 全灭但 ICMP 通），可在拿到 IP 后
   增加 `esp_wifi_stop()+start()` 的"重启净空"实验（本板内存紧张，需实测）。
3. 验证手段：`Firmware/main/net/http_client.c` 的 `raw_tcp_probe_once()` 已内置
   网关+服务端双点裸 socket 探针（含 2 次重试），下次直接看它打印的 errno 即可。

### 8.3 服务端/Web 本轮补齐（E14 日志链路，已自测）

- `POST /api/device/log` + `GET /api/admin/device-logs/{id}`|`/by-uuid/{uuid}`：
  见 §7.2 契约；实现含 **幂等**（重传同批 `accepted=0`）与会话判定（用设备上报的
  `t` 开机毫秒判断重启，**不能只看 seq 回退**——否则重传被误判成重启并重建库）。
- Web 设备详情页新增「设备日志」卡（级别/TAG 过滤、5s 自动刷新、E/W 着色、
  端点缺失时给可读提示）。CI smoke 增加 GET/PUT 与本次幂等的断言。

### 8.4 分阶段内部堆实测（2026-09-27 追加）

```
@wifi_preinit 后        空闲=82355  最大块=31732
@render_init 后         空闲=77699  最大块=31732
@state_machine_boot 前  空闲=15787  最大块=7924
（boot 后）             空闲≈2300   最大块≈2000
```

即 66KB 内部 RAM 在「任务创建 → 自检连接」窗口被切成 ~179 个小块，
之后 WiFi 驱动自己都分不到管理帧：`W:m f null` / `W:m f probe req l=0`（每 10s 一次）。
已做的回收：mp_main 12K→10K、bgm 16K→12K、render 12K→8K、httpd 8K→3.5K、
portal 6K→4K、上报缓冲 5.6KB 挪 PSRAM、TX 池 16→8、STA 连上后关掉 SoftAP。
仍未让 TCP 建连成功 —— 说明只靠"省"不够，需要一次**启动期常驻 RAM 清单**
（任务栈 + 组件 .bss + 每段分配者）来定结构性问题。

### 8.5 三次连开实测（同一固件，环境未变）

| 次数 | 结果 |
|---|---|
| 1 | `WiFi 断开 reason=2`（路由器拒绝认证）→ OFFLINE |
| 2 | 拿到 IP 但 TCP 失败：`probe: [服务端] <NAS_IP>:38090 connect=-1 errno=118 (Host is unreachable)` |
| 3 | 同样 TCP 失败（manifest 拉取 phase=open 超时） |

关键对照：**Mac 用同一网线/同一 URL `curl` 得 200**，`ping 设备` 0% 丢包。
所以「NAS 防火墙 / 端口 / 凭据 / 路由」都已排除；现象是设备侧 TCP 一律建不起来，
且伴随管理帧分配失败。下一步优先级：
1. 启动期常驻 RAM 清单（结构性定位，不再零敲碎打）
2. 换一个已知良好的 AP/SSID（2.4G 独立）复测，排除路由器特有的客户端限制
3. 若换 AP 后仍失败，再回到 lwIP/WiFi 驱动的收包路径（本次 errno 从 113→128→118 漂移，
   指向分配失败而非固定拦截）
