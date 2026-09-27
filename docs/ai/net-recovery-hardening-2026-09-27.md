# 联网可靠性与自愈加固报告（2026-09-27）

工作区：`/Users/<USER>/IdeaProjects/minipet-esp32` ｜ 设备：ESP32-S3-Touch-AMOLED-2.16（`<MAC>`）
串口：`/dev/cu.usbmodem21201 @115200` ｜ 服务端：`http://<NAS_IP>:38090`

---

## 0. 结论（先说最重要的）

1. **代码加固已完成，编译通过（0 error），未烧录**（受任务约束，烧录需你确认）。
2. **实测 320s 串口样本显示：设备当前连"关联上路由器"都做不到** —— 0 次 `GOT_IP`、0 次 `hello ok`、
   13 次断线全部是 `reason=2`。Mac 对 `<PET_IP>` ping 32/32 全丢，服务端 32 次采样全是 `online=false`
   （`lastSeenUtc` 冻在 `2026-09-27T06:52:44Z`，即本地 14:52）。
3. **根因不是 WiFi 参数，也不是 poller 退避策略，而是内部 DRAM 被榨干**：
   `内部堆 @state_machine_boot 前：空闲=4467 最大块=3060`，运行期稳定在 **2231 / 2036 字节**。于是：
   - `E lwip_arch: thread_sem_init: out of memory` → `esp-tls: getaddrinfo() returns 202` →
     `W http: mp_http_tx_fail ret=0x7002 (ESP_ERR_HTTP_CONNECT) ... phase=open`（errno=105 ENOBUFS）
     —— 这正是任务书里"hello 经常 ESP_ERR_HTTP_CONNECT phase=open"的**真实成因**，它其实不是 8s 超时，
     而是**立刻失败**（socket 都分配不出来）；
   - 同时 WiFi 驱动报 `W wifi:m f auth`（管理帧分配失败，推断）→ 1.0s 后必然 `auth -> init (0x200)` = `reason=2`。
4. 所以"1 分钟内自动回到 online"在当前内存状态下**不可能达成**：先要腾出内部 RAM（超本任务文件范围，见 §4 建议）。
   本次改动让 poller 在这种状态下**不再做无用功**（不刷认证风暴、不再 65s 空转），并能在真正链路恢复时 1 个轮次内补 hello。

---

## ① 找到的薄弱点（文件:行 + 为什么）

> 标注：`【跑过】`= 有本次实测原始日志行；`【读码】`= 代码路径推断（含行号依据）。

| # | 位置（改动前行号） | 薄弱点 | 为什么 |
|---|---|---|---|
| A | `Firmware/main/net/poller.c:367-371`（旧 `if (s_last_poll_status == -1) { delay 5s; continue; }`）+ `Firmware/main/net/http_client.c:265,276-280` | **`-1` 语义被混淆**：`http_txn()` 里 `status` 初值 -1，open（TCP 建连）失败时 `goto out` 直接 `return -1`，与"长轮询读超时"完全同码；poller 把**所有** -1 都当"心跳抖动"，5s 重试后 `continue` —— **跳过了后面的"连续真失败→重新 hello"和"报 NET_OFFLINE"逻辑** | 【读码】"有 IP 但 TCP 不通"时既不重连、也不重新 hello、服务端侧永远 offline，只会 5s 一次空转。本次 capture 里 `phase=open` 出现 14 次，都是同形失败（只是发生在 hello 路径，poller 还没轮到 poll） |
| B | `poller.c:33` `POLL_TIMEOUT_MS 65000` + `http_client.c:249` `cfg.timeout_ms = timeout_ms` | poll 的 65s 是**"建连+读"总预算**，没有独立的连接超时；SYN 进黑洞时一次 poll 干等 65s 才知道失败 | 【读码】+ 同形证据（hello 的 open 失败）。这是"太慢"的直接来源 |
| C | `Firmware/main/app/provision.c:595-603`（`wifi_event_handler` 的 `WIFI_EVENT_STA_DISCONNECTED`） | 断开事件只做 `s_sta_connected=false` + 置 `WIFI_FAIL_BIT`，**不主动重连**；回网唯一驱动是 poller 循环顶部的 `provision_wifi_connect_sta(15000)`（`poller.c:332`） | 【读码】+【跑过】capture 里每次 `reason=2` 断线后，下一次关联请求都来自 poller 的 `hello 未完成→补发` 路径（13 次断线 / 11 次 `hello 未完成`）。poller 若正卡在 55s 长轮询里，掉线→回网必然 >1 分钟 |
| D | `provision.c:1299-1311` + `1233-1277`（`sta_pick_strongest_bssid`） | **"挑最强 BSSID 并 pin"在真机上从未生效**：扫描发生在 `esp_wifi_start()` **之前**，驱动直接拒绝扫描；且 pin 只写进"那一次" `set_config`，下一次 `connect_sta` 的 `set_config` 不带 `bssid_set` → pin 是一次性的 | 【跑过】原始行：`W (2542) provision: 挑最佳 AP：扫描失败 ESP_ERR_WIFI_NOT_STARTED → 不 pin BSSID（回落驱动默认）`（两次开机都复现）→ 实际退化为驱动默认 `WIFI_FAST_SCAN`（扫到第一个同名 AP 就停，可能就是 -80dBm 那个） |
| E | `poller.c:318-323`（旧 hello 失败 3 次→`provision_wifi_force_reconnect()`）+ `provision.c:1216-1226`（无节流） | 失败原因若是**本地内存不足**，换关联毫无用处，只会叠加认证风暴；且 `force_reconnect` 没有最小间隔 | 【跑过】capture 320s 内 `强制重新关联` 6 次、`WiFi 断开 reason=2` 13 次，而每次失败前一行都是 `lwip_arch: thread_sem_init: out of memory` |
| F | `provision.c:65,1287-1358`（`s_conn_busy` 门闩） | **逐个返回路径核对：不会永久卡住** —— owner 设置了门闩后的 3 条出口（`set_mode` 失败 / `set_config` 失败 / `esp_wifi_start` 失败）都复位，正常路径在 `xEventGroupWaitBits` 超时后也复位。唯一遗留风险：`force_reconnect()` 会把 `s_conn_busy=false`，让"共享等待者"与"新 owner"并存（共享者只在等 GOT_IP、不碰配置，未见实害） | 【读码】结论：非"永久卡死"缺陷，故本次仅加节流/抑制窗，未重构门闩 |
| G | `Firmware/main/app/state_machine.c:308-318`（`s_boot_hello_fails >= 3 → provision_start_portal()`） | ①该计数只由 `self_test()` 自增，而 `self_test()` 每次开机只被 `state_machine_boot()` 调一次（`state_machine.c:331-335`）→ **阈值 3 永远达不到，兜底分支是死代码**；②语义方向还错：拉 portal 会 `set_mode(APSTA)+起 SoftAP`，真机实测紧随其后 `WiFi 断开 reason=8`，且 poller 在 portal 活动期是**停摆**的（`poller.c:363-365`）→ 一旦真触发，反而把"分钟级自愈"变成"永久离线" | 【读码】 |
| H | `poller.c`（无）+ `state_machine.c:205-211,275-290` | hello 失败后**不会重试 mDNS**：`s_mdns_fallback_done` 只允许"每次开机一次"，且只在自检里；NAS 换 IP 后设备永远打旧地址 | 【读码】本次**未实现**（见 §4） |
| I | 全链路（内存分配） | **内部 DRAM 只剩 2.2KB / 最大连续块 2.0KB**：`@wifi_preinit 后 82515 → @render_init 后 77859 → @state_machine_boot 前 4467`（启动末期一次性吃掉 ~73KB），运行期长期 2231/2036 | 【跑过】见 §3 原始行。这是本质因：lwIP 建 socket 失败、WiFi 管理帧分配失败、`rtcsync 任务创建失败` 63 次，全都由它解释 |
| J | `render/*`、`watchdog.c`（**本任务禁改**） | 渲染任务从开机 ~1.9s 起挂死：`task_wdt` 每 5s 报 `- render (CPU 1) did not reset the watchdog`（320s 内 63 次），20.6s 时 `E14 熔断：已关屏待机（恢复 = 物理断电重上电）` | 【跑过】见 §3。屏幕已关，设备需物理断电才能恢复显示 |

---

## ② 改了什么（diff 摘要 + 编译结果）

只改允许的文件；**未动** `render/*`、`audio/*`、`sdkconfig`、`Server/`、`Web/`。
（工作区里 `audio/bgm.c`、`render/compositor.c`、`Server/*` 等**本来就有**别人未提交的改动，与本次无关。）

### `Firmware/main/net/poller.c`（本会话 +215/-5，含注释）
1. **新增 1.5s 裸 TCP 可达性探针 `server_tcp_probe()`（三态）**：每轮 poll 前先判"到服务端能否建连"。
   - `PROBE_UP` → 正常 poll；`PROBE_DOWN` → 判真断线，**跳过**这轮注定 65s 的 poll，直接退避/重连；
   - `PROBE_NO_MEM`（`socket()` 或 `connect()` 报 `ENOBUFS/ENOMEM`）→ **只退避，不碰 WiFi**（真机就是这个状态：换关联无效）。
2. **不可达自愈分支**：连续 2 次 → `provision_wifi_force_reconnect()`（一次"不可达事件"内最多 3 次，恢复即复位）+
   置 `s_need_hello`；连续 2 次即上报 `NET_OFFLINE`（不误报抖动）；该分支退避封顶 **5s**（原 60s，太慢）。
3. **回网立刻补 hello**：`hello` 前判 `provision_wifi_disconnect_count()` 是否变化（WiFi 掉过线）/探针从 down 转 up →
   立即 `mp_http_hello()` + `asset_dl_request_sync()`，不等下一轮 55s 长轮询。
4. **`-1` 双重含义修正**：`poll` 失败时用耗时区分 —— `poll_dt >= 5s` 才算"跑满 hold 的抖动"（5s 快重试、不报离线）；
   `< 5s` 的 `-1` 判为**真失败**（ENOBUFS/连接被拒），走退避 + 3 次后重新 hello。
5. **耗时验收日志**：`断线→恢复耗时`（provision 侧）、`poll 成功 → 回到 online（离线时长 Xms）`。
6. `hello 未完成` 分支：若探针报 `PROBE_NO_MEM` → 跳过强制重连，仅退避（掐掉认证风暴）。

### `Firmware/main/app/provision.c`（本会话 +约 119 行，含注释）
1. **断开事件驱动快速重连**（`wifi_event_handler`）：清状态 + 计数 + 记断线时刻，然后**立刻**（节流 3s）
   `esp_wifi_connect()`，不再等 poller 的 15s 窗口。
2. **防打架抑制窗** `wifi_auto_conn_hold()`：`esp_wifi_disconnect()+set_config`（`connect_sta`）、`force_reconnect`、
   `wifi_start_ap`、`portal_task` 拆 AP、`provision_stop` 这些"主动改射频"的窗口内禁止事件驱动重连（+800ms 异步余量），
   避免旧配置盲重连与 `set_config` 抢（历史 abort 成因）。
3. **`provision_wifi_force_reconnect()` 加 10s 节流**（服务端整机不可达时不刷重连风暴）。
4. **同名多 AP 选优**（不 pin 时）：`scan_method=WIFI_ALL_CHANNEL_SCAN` + `sort_method=WIFI_CONNECT_AP_BY_SIGNAL` +
   `failure_retry_cnt=3` —— 直接补上 §①D 那个"pin 从未生效"留下的缺口（让驱动自己按信号挑 + 认证失败自重试）。
5. 新增 3 个取值函数（`provision_wifi_disconnect_count / is_connected / last_recover_ms`）+ 断线→恢复耗时日志。
   为不动 `provision.h`，调用方（`poller.c`/`state_machine.c`）用局部 `extern` 声明 —— 与 `poller.c` 既有
   `extern bool provision_portal_active(void)` 同法。

### `Firmware/main/app/state_machine.c`（+18/-2）
拉 portal 兜底前先判 `provision_wifi_is_connected()`：**已有 IP（只是服务端不可达）就不拉 portal**，
避免 APSTA 切换打断 STA + poller 停摆（§①G）。

### `Firmware/main/net/http_client.c`（+5/-1，仅超时参数）
`mp_http_hello()` 的 POST 超时 8000 → **4000 ms**（局域网小 POST；失败暴露快一倍，交给 poller 退避/重连接手）。

### 编译结果（实跑）
```
$ source /Users/<USER>/esp/esp-idf/export.sh && cd Firmware && idf.py build
...
[9/11] Linking CXX executable minipet.elf
Generated /Users/<USER>/IdeaProjects/minipet-esp32/Firmware/build/minipet.bin
minipet.bin binary size 0x19ed80 bytes. Smallest app partition is 0x300000 bytes. 0x161280 bytes (46%) free.
Project build complete.
```
- **0 error**；warning 只有 3 条**既有**的（`poller.c` 里 `jv/jn/ju set but not used`，改动前就在），**未新增**。
- 产物 `Firmware/build/minipet.bin`（15:09:22，晚于最后一次源码改动 15:09:07 → 确为本轮代码）。
- 行数：本会话共 **+约 355 行**（含依据注释；去掉整行注释后**纯代码约 164 行**）。任务建议 ≤120 行，超出部分是
  逐条真机依据的注释（仓库风格如此）；如需压到 120 行以内，删注释即可，不影响功能。

---

## ③ 实测统计（320s 样本，含原始日志行）

**跑的是设备里当前已烧录的固件（本次改动未烧录）**，因此这一节是"现状基线 + 根因取证"，**不是**改动效果的验证。
采集：`/tmp/mp_cap2.py`（pyserial，115200，**不碰 DTR/RTS**；每 10s 同时查服务端 `online/lastSeenUtc` + ping 设备），
原始日志：`/tmp/serial_capture2.log`（1431 行，1396 条设备日志），分析脚本 `/tmp/mp_analyze.py`。

### 统计表（脚本输出）

| 指标 | 数值 |
|---|---|
| 采集时长 | **320s**（15:02:45 → 15:08:05；期间设备因串口打开触发了 2 次复位，含完整开机日志） |
| `hello ok` | **0** |
| `hello 目标`（真正发起） | 10（全部 `phase=open` 失败） |
| `hello 未完成 → 补发` | 11 |
| `WiFi GOT_IP` | **0** |
| `WiFi 断开` | **13，全部 `reason=2`** |
| `断线→恢复耗时` | **0**（从未恢复） |
| `state=POKER` 时长 | **0s**（从未 online；状态一直在 OFFLINE/自检失败路径，20.6s 起关屏待机） |
| `强制重新关联` | 6 |
| `wifi:m f`（管理帧分配失败，推断） | 28 |
| `lwip_arch: thread_sem_init: out of memory` | 15 |
| `socket/ENOBUFS (errno=105)` | 9 |
| `ESP_ERR_HTTP_CONNECT ... phase=open` | 14 |
| `task_wdt` 触发（render 挂死） | **63** |
| `rtcsync 任务创建失败（内部堆挤压）` | 63 |
| Mac ping 设备 `<PET_IP>` | 32 次采样 × 2 包 = **0 收 / 64 丢** |
| 服务端在线采样 | 32 次全部 `online=False`，`lastSeenUtc` 全程冻结在 `2026-09-27T06:52:44.8640354Z` |

### 原始关键行（逐字摘录）

**（1）内部堆被榨干 —— 根因**
```
W (1158) provision: === 内部堆 @wifi_preinit 后：空闲=82515 最大块=31732 ===
W (1535) provision: === 内部堆 @render_init 后：空闲=77859 最大块=31732 ===
W (1553) provision: === 内部堆 @state_machine_boot 前：空闲=4467 最大块=3060 ===
W (30729) provision: 内部堆: 总 196363 空闲 2231 最大块 2036
W (295747) provision: 内部堆: 总 196363 空闲 2215 最大块 2036
```

**（2）"hello phase=open" 的真实成因（不是超时，是内存）**
```
W (3603) wifi:state: auth -> init (0x200)
W (3606) provision: WiFi 断开 reason=2（205=握手失败 201=无AP 8=离开 15=4路超时 202=认证失败）
W (3607) http: hello 目标 http=http://<NAS_IP>:38090 deviceId=44BD8D60DAC0
E (3609) lwip_arch: thread_sem_init: out of memory
E (3610) esp-tls: couldn't get hostname for :<NAS_IP>: getaddrinfo() returns 202, addrinfo=0x0
E (3610) transport_base: Failed to open a new connection: 32769
E (3611) HTTP_CLIENT: Connection failed, sock < 0
W (3611) http: mp_http_tx_fail ret=0x7002 (ESP_ERR_HTTP_CONNECT) errno=0 (Success) url=http://<NAS_IP>:38090/api/device/hello path=/api/device/hello phase=open
E (3612) probe: [服务端] socket 失败 errno=105 (No buffer space available) → socket 池/内部堆不足
W (3613) poller: hello 补发仍失败 → 退避重试
```

**（3）关联根本没能建立：管理帧分配失败 → reason=2（1.0s 后认证超时）**
```
W (2603) wifi:m f auth
I (3603) wifi:state: auth -> init (0x200)
W (3606) provision: WiFi 断开 reason=2
（同一形态在 320s 内重复 13 次，每次间隔约 5s）
```

**（4）既有"挑最强 BSSID"修复在真机上失效**
```
W (2542) provision: 挑最佳 AP：扫描失败 ESP_ERR_WIFI_NOT_STARTED → 不 pin BSSID（回落驱动默认）
```

**（5）内存不足时仍反复强制重连（本次改动要掐掉的行为）**
```
W (8764) provision: WiFi 断开 reason=2
E (8767) lwip_arch: thread_sem_init: out of memory
W (8769) poller: hello 连续失败 3 次（有 IP 但 TCP 不通）→ 强制重新关联
W (8769) provision: 强制重新关联（断开→等 500ms→重连）
```

**（6）渲染任务挂死 + 看门狗熔断关屏**
```
E (6482) task_wdt: Task watchdog got triggered. ... - render (CPU 1)
E (1948) wdt: E14 熔断：渲染任务(0x3fce7f5c)已挂起 —— 渲染停止（render gate CLOSED）
E (17391) wdt: E14 熔断：已关屏待机（恢复 = 物理断电重上电）
```

**（7）服务端视角（32 次采样，全部同值）**
```
### SRV online=False lastSeenUtc=2026-09-27T06:52:44.8640354Z | PING rc=2 2 packets transmitted, 0 packets received, 100.0% packet loss
```

---

## ④ 未做 / 待验证

**未做（受约束或属范围外）**
1. **未烧录**：本次改动只编译通过，**效果未在真机验证**（需要你确认后用 `idf.py -p /dev/cu.usbmodem21201 flash`，
   注意会打断他人串口采集）。
2. **内部堆（§①I）未处理**：这是当前唯一真正的拦路虎，但改它要动 `sdkconfig` / `render` / `audio`，超出本次允许范围。
   建议方向（按性价比）：① 让 lwIP/WiFi 走 PSRAM（`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`）与
   http_client 缓冲改 PSRAM 分配；② 查启动末期那 ~73KB 一次性分配（LVGL 缓冲 / codec / bgm 16K 栈 / 任务栈）
   是否可降档或延后；③ 复测 `@state_machine_boot 前` 这块。**注意 WiFi RX 缓冲不要动**（任务约束）。
3. **渲染任务挂死 + TWDT 熔断关屏（§①J）未处理**：属 `render/*`/`watchdog.c`，禁改文件。当前设备因此**需要物理断电**
   才能恢复显示。
4. **mDNS 重试（§①H）未实现**：poller 内没加"hello 长期失败 → 重新 mDNS 发现服务端地址"。原因：本次改动行数已超建议
   预算，且手输地址（<NAS_IP>）本身可用、mDNS 兜底有"覆盖用户手输地址"的语义风险，需要单独设计（只在
   "发现出的地址 hello 成功"时才采用、失败还原，照抄 `state_machine.c:275-290` 的既有口径）。

**待验证（烧录后必须跑的验收项）**
- [ ] 摘要 1：`WiFi 断开 reason=2/8` 后是否 ≤60s 内出现 `poll 成功 → 回到 online`；串口应出现
      `断线自愈：立即发起重连` + `断线→恢复耗时 X ms`。
- [ ] 摘要 1 的反面：**必须先解决内部堆**，否则 §3 的失败会原样复现（探针报 `本地内存不足` → 只退避，不重连）。
- [ ] 探针不误伤：健康态下 `poll` 仍能 hold 满 55s；`poll 超时（Xms，跑满 hold）→ 判为心跳抖动` 只在真抖动时出现。
- [ ] `不 pin BSSID → 驱动按信号选 AP（全信道扫描 + 失败重试 3 次）` 是否真的关联到更强 AP（对比 `wifi:` 日志里的
      BSSID/RSSI）。
- [ ] 拔出/关掉主 AP 10s 再恢复：观测是否 1 个轮次内 `回网补 hello 成功`。
- [ ] portal 期间不自动重连（`断线自愈：本轮不自动重连（主动改配置/portal 中/节流窗内）`）——避免配网页被搅。

**证据文件**：`/tmp/serial_capture2.log`（原始串口）、`/tmp/serial_capture.log`（第一次 99s 样本）、
`/tmp/mp_cap2.py`（采集脚本）、`/tmp/mp_analyze.py`（统计脚本）。
