# MiniPet ESP32 技术总档（唯一权威技术文档）

> **本档为 board216 目录自持副本，仅服务 AMOLED-2.16 板。**

> **【拆分注记】原 board-216 单板分支已目录化为独立工程 `Firmware/board216/`（即本档所在目录）。**
> **禁止在本工程构建/烧录 1.85B 板——1.85B 的一切工作请在 `Firmware/board185b/` 工程进行，两板零共享文件。**

> 2026-09-27 汇总，2026-10-01 增补（预设推送/地图分段/UDP 取证/双板冲突规约）。
> 本文档 = 硬件事实 + 环境操作 + 架构 + 全部根因台账 + 契约 + 标定状态 + 遗留清单。
> 新会话/新人接手：先读本文档，再读 `hardware-bringup-issues.md`（联调动态）。
> 规则：每次修复必须有真机证据（日志/照片）；禁止"代码写完即报完成"。

---

## 一、板卡硬件事实

| 项 | 值 |
|---|---|
| 板型 | 微雪 ESP32-S3 Touch AMOLED 2.16"（480×480 QSPI 圆屏方屏） |
| 芯片 | ESP32-S3，Octal PSRAM 8MB（占用 35-37 脚），16MB Flash |
| 面板 | CO5300（QSPI，BSP=waveshare__esp32_s3_touch_amoled_2_16） |
| 触摸 | CST9220（I2C，INT=11 RST=40；帧格式见 §六） |
| IMU | QMI8658（I2C，INT1=17，DRDY 中断+20ms 轮询兜底） |
| RTC | PCF85063（I2C，寄存器存 UTC；OS 标志=首次上电判据） |
| PMU | AXP2101（PWRON 短按=底键，IRQ 脚未接线→寄存器轮询） |
| Codec | ES8311（I2S，44100Hz；**PA 使能=GPIO46 高有效**，纯 GPIO 直控） |
| SD | SDMMC（1/2/3/41）；内部 Flash assets 分区 0x620000/6M 为兜底 |
| 按键 | 顶键=菜单 GPIO18；中键=GPIO0（BOOT，strapping 只准输入+上拉）；底键=AXP2101 PWRON |
| USB | 19/20 = 原生 USB（烧录/日志口，绝不可挪用）；串口设备名 `/dev/cu.usbmodem21201` |
| **内部 RAM** | **动态堆仅 ~143KB（90+21+32KiB），启动挤压窗口期可低至空闲 6KB/最大块 3KB** —— 本板一切"随机任务创建失败/分配失败"的总根源 |
| 出厂复位 | NVS 擦除 = `0x9000-0xF000` erase_region（不影响 assets FAT） |

### 引脚占用总表（扫描探针排除依据）
SD 1/2/3/41 · LCD 4/5/6/7/12/38/39 · 音频 8/9/10/42/45/46 · 触摸 11/40 · RTC 13 · I2C 14/15 · IMU 17/21 · 菜单键 18 · PA 46 · 系统 26-32(Flash)/35-37(PSRAM)/43-44(UART0)/19-20(USB)；空闲候选=GPIO0/16/47/48。

---

## 二、环境与操作手册

### 2.1 环境（每次新 shell 必做）
```bash
source /Users/<USER>/esp/esp-idf/export.sh        # IDF v5.5
cd /Users/<USER>/IdeaProjects/minipet-esp32/Firmware/board216   # ⚠️ 必须 cd 进板级工程根，仓库根/Firmware 层都没有 CMakeLists
```
- IDF 自带 python（含 pyserial）：`/Users/<USER>/.espressif/python_env/idf5.5_py3.9_env/bin/python`——系统 python3 **没有** serial 模块，抓日志必须用这个。

### 2.2 标准烧录流程（build → flash → 复位 → 抓日志）
```bash
idf.py build                                      # 验收线：0 error 0 warning
idf.py -p /dev/cu.usbmodem21201 flash             # 烧录，末尾自动硬复位启动新固件
```
烧录完成后**新固件已在跑**，抓日志三种姿势：

**(a) 烧完立刻抓（推荐）**：flash 完成后 1 秒内打开串口读：
```bash
PY=/Users/<USER>/.espressif/python_env/idf5.5_py3.9_env/bin/python
$PY - <<'EOF'
import serial, time
s = serial.Serial('/dev/cu.usbmodem21201', 115200, timeout=1)
f = open('/tmp/boot.log', 'wb'); t = time.time()
while time.time() - t < 20:            # 时长按需：开机诊断 15-25s，含 keyscan 期 75s+
    d = s.read(4096)
    if d: f.write(d); f.flush()
f.close(); s.close()
EOF
```
**(b) 干净复位重抓（要完整 boot 日志时）**：先借 esptool 复位，立刻开读（同上读取脚本）：
```bash
$PY -m esptool --chip esp32s3 -p /dev/cu.usbmodem21201 \
    --before default_reset --after hard_reset chip_id >/dev/null 2>&1
```
**(c) 运行态软复位**（板子正常跑着触发重启）：pyserial 打开后拨 DTR/RTS：
```python
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)   # RTS 脉冲复位
```

### 2.3 日志判读要点（踩过的坑）
- **脏日志识别**：时间戳乱序（如 1322ms 行后出现 6970ms 再回 1323ms）= USJ CDC 缓冲把上一 boot 残留混进来了，**整份作废重抓**，不要基于它下结论。
- **抓不到开机头**：复位到开读之间会丢 bootloader 段日志；关键 app 日志（1.4s 之后）一般来得及。
- **关键日志锚点**（grep tag）：`key0`（中键计数/按下时状态）、`tmap`（触摸映射）、`orient`（方向）、`probe`（flush/ENTPOS 渲染探针）、`rc`（渲染层，含气泡兜底）、`http`（hello/tx_fail/RAW 探针）、`co5300`（显示驱动/NO_MEM）、`menu`（导航诊断）、`bgm`、`keyscan`（GPIO 扫描）。
- **TWDT panic**：`CONFIG_ESP_TASK_WDT_PANIC=y`（当前开着）卡死即 panic 打回溯；定位完关掉。
- 中键取证：NVS `calib/key0`=累计次数、`calib/k0st`=最后一次按下时的状态机状态（开机日志自动回读，4=MENU）。

### 2.4 常见故障
| 症状 | 处置 |
|---|---|
| `idf.py` 报 CMakeLists not found | 当前目录不对，cd 进 Firmware/board216 |
| 端口打不开/烧录失败 | 残留 monitor 占口：`lsof | grep usbmodem` 找 PID kill，或 pkill -f idf_monitor |
| ModuleNotFoundError: serial | 用 IDF 环境的 python（见 2.1） |
| 恢复出厂（重配网） | esptool `erase_region 0x9000 0xF000`（只擦 NVS 不动 assets）；配网页 WiFi 密码 12 位 <WIFI_PASSWORD> |
| 烧录后行为诡异 | 先确认 flash 真的成功（完整输出见 "Done"）；再排除脏日志误判 |
| **黑屏 + 串口刷 `boot: No bootable app partitions`（复位循环）** | 烧录中途被取消 → app 分区残缺镜像（`invalid segment length 0xffffffff`），ota_1 又为空 → 处置=重烧完整镜像即愈，与代码无关。**烧录一旦开始不要中断** |

### 2.4B 双板并行开发隔离规约（2026-09-29 起两块板在线）

| 板 | 串口（本次枚举） | 身份特征 | 归属 |
|---|---|---|---|
| A（原 2.16） | /dev/cu.usbmodem**21101** | hello 日志 `dev-693ea4`；MAC `44:bd:8d:...` | 主联调板 |
| B（新入） | /dev/cu.usbmodem**21201** | deviceId 不同；IMU I2C NACK 刷屏为当前特征 | 另一 agent |

**铁律（避免互相影响）**：
1. **端口名会换**（重插 USB 后 21101/21201 可能互换）——任何 flash/reset 前先静听 5 秒核对身份特征，**禁止裸 `idf.py flash`**（必须 `-p` 显式指定）。
2. **构建目录天然分板**：两板已拆为独立工程——板A=`Firmware/board216`、板B=`Firmware/board185b`，各自目录内 `idf.py build`（build/ 互不相干）；禁止共用一个构建目录并行 build（ninja 锁/半成品会互相踩）。
3. 串口互斥：monitor/脚本用完立刻关，不然对方烧录报端口占用。
4. 服务端天然隔离：deviceId 由 MAC 派生，两板各自注册/配对/manifest，互不覆盖；Web 换装注意选对 deviceId。
5. WiFi 两板各自 DHCP，无冲突。
6. 日志文件分板存（/tmp/boardA_*、/tmp/boardB_*），混读会误诊。
7. **共享源文件必须参数化，禁止宏开关**（2026-10-01 血案）：一个 session 把
   `MP_GROUND_CAM_SHIFT_PX` 宏 0→248，另一板构建被动带上=用户"背景变了"；另一
   session 在 input_dispatch.c 留半成品（变量用先于声明）→ 对方编译失败。
   **板级差异一律进 profile 字段**（先例：`ground_cam_shift_px`），跨 session
   改共享文件前先确认对方没有在飞编辑。（2026-10-01 双板目录化拆分后两板工程
   已零共享文件，本条为历史案例存档；板级差异走 profile 字段的纪律不变。）

### 2.5 服务端/网络环境
- 本地服务端（Mac）：`dotnet run`=5059；用户常驻实例抢 5000 勿动；测试显式 `ASPNETCORE_URLS=5059`。dotnet/WZ 数据在 `/Volumes/SSD`（见记忆 dev-env-ssd）。
- 生产服务端：NAS `http://<NAS_IP>:38090`（**禁改后端**；Mac IP=<MAC_IP>，板子 DHCP≈<DEVICE_IP>）。
- 家 WiFi：<SSID> / <WIFI_PASSWORD>（12 位，别输成 13 位）。SoftAP 配网：`<AP_SSID>` → portal 192.168.4.1。
- 服务端健康自检：`curl -m 5 http://<NAS_IP>:38090/api/device/hello`（GET 应 200）。
- **地图/NPC 运行时登记**：`POST /api/admin/devices/{id}/push {"kind":"map|npc","id":...,"switch":true}`
  → WZ 打包+登记+rev bump+自动切图。**会双发切图指令（立即+15s）**——串口连换两次
  同图=设计行为。资产未被 push 过就不在清单里（"拉取失败"先查清单有没有该条目）。

### 2.6 UDP 帧倾倒（远程看屏取证，2026-10-01 上线）

- **触发**：bubble 文本魔数 `::shot`——`curl -X POST http://<NAS_IP>:38090/api/admin/devices/{id}/command -H "Content-Type: application/json" -d '{"type":"bubble","value":"::shot"}'`。
  设备把 g_fb（最终合成帧 RGB565）UDP 广播 :9999；TF 截图照旧落 /sdcard/debug/shot_NNN.bmp。
- **Mac 收帧**（nc 收不全——UDP 到了内核但不进 nc，用 python）：
  ```python
  # /tmp/udprx.py：bind 0.0.0.0:9999，SO_RCVBUF 1<<22，收 "MPFB"+w+h+n 头与
  # seq+1400B 数据包，25s 超时，重组写 /tmp/fb.raw
  ```
  然后 RGB565→PNG（PIL）：`Image.frombytes('RGB',(w,h), RGB565le 展开字节)`。
- **丢包判读链**：设备日志 `📡 UDP 帧倾倒：1+330 包（成 N）`（N<330=设备侧 lwip pbuf
  枯竭，已带每 2 包歇 1 tick+单包 3 试退避）；`netstat -s -p udp | grep received`
  触发前后对照（涨了=包到内核，收不到=应用层/nc 语义问题）。
- **用例**：宠物/地图渲染对不对、脏区残影、弹回/拖拽行为——串口探针看不见的视觉
  真相，一条命令拿到精确像素。服务端 screenshot 白名单放行后可换正式通道。

---

## 三、固件架构速览

```
app_main（main.c）
├─ NVS → 事件循环 → 三队列(cmd/event/audio) → watchdog → 驱动 init
├─ render_init（内含 display_init）→ 【render/input 任务必须先于 bgm(24K栈) 创建！】
├─ poller/events/asset_dl/ota/bgm_start
├─ state_machine_boot（阻塞：WiFi/服务端探测；OFFLINE 态=服务端不可达也正常跑）
└─ render_task(33ms/30fps): 排空 cmd_q → render_tick → watchdog_kick
   input_task(20ms): IMU→touch_tick→key_tick(菜单键18)→key0_tick(中键GPIO0)→pwron_tick(底键PMU)
```

### 渲染管线（compositor.c，非 LVGL；LVGL 仅菜单态）
1. 脏区 16×16 块网格 → flush_dirty 合并包围盒
2. compose_region 逐层重算：clock_doze → **static_back(须 +x 偏移)** → strip → tile(**须 +x**) → clock → calib线 → 实体(1bit覆盖掩码) → 气泡 → 横幅
3. blit_be：小端 FB → 大端字节交换（12KB 64B 对齐 stage，**行高恒偶**）→ display_blit
4. display_co5300.c：esp_lcd QSPI（队列深度 3）+ **槽位信号量背压**（在飞≤3，完成回调归还，2s 泄漏探针）
5. 面板方向：**swap=false + 无镜像（组合 0，原生方向）**——菜单照片终审：组合1(mirror_x)文字左右镜像、组合2(mirror_y)垂直颠倒，原生即正立
6. **相机下移实验=profile 字段 `ground_cam_shift_px`**（amoled216=0 定稿原景 /
   lcd185b=248 实验）：装载地图后 static/tile/掩码整体上移 N 行、底部用内嵌
   地面带（ground_band_000010000.bin）补齐、条带 y -= N/2 世界像素。**禁止恢复全局
   宏**（跨 session 冲突血案，见 2.4B 铁律 7）。
7. **地图分段格式**（运行时登记的 237KB 新包）：static=天空段（back 切段的段0），
   树冠/房子/丘陵在 speed=0 条带段（y=0 全视口层）、地形在 tile 层——
   **`g_map_static_only` 必须 false**（true=只装 static=只剩天空，调试遗留入库案）。
8. **越屏弹回**：`ent_bounce_if_offscreen()` 每帧合成前——宠物出屏面积>50% →
   drag 按当前画布夹回全可见（贴边），双 mark_ent 防残影（动作切换画布重展是
   主要出屏来源）。

### 音频管线（bgm.c）
触发(audio_q) → 曲目表(SD `minipet/audio/*.mpk` AUDIO_META) → mp_http 流式 GET → minimp3(vendored 真解码) → pcm_ring → feeder 任务 → ES8311 I2S；PA=GPIO46 有数据才开。任务栈 24576（mp3dec scratch ~16KB 在栈上）。

---

## 四、根因台账（全部真机实证，新异常先查此单）

### 启动/系统
| 症状 | 根因 | 修法/铁律 |
|---|---|---|
| NO_MEM 风暴 7404条/80s + 卡死 | **三层链**：blit 行高奇数→even_round 外扩+1 行→快速通道失效走 PSRAM 暂存→esp_lcd `setup_priv_desc` 对非 DMA-capable/未对齐缓冲**每次排队强制 malloc 整块内部 DMA 拷贝**→内部堆见底 | 行高恒偶+stage 64B 对齐+12KB+槽位背压。**铁律：SPI 推送缓冲必须 DMA-capable 且地址+长度对齐** |
| 任务创建随机失败（假"在跑"） | 内部堆 143KB，启动窗口期最大块<所需栈；SELFTEST 曾从未跑成 | 关键任务先建（render 12K 先于 bgm 24K）+失败重试/自愈定时器+日志带堆水位 |
| 菜单呼出重启 | flush 未调 flush_ready 死循环 | 入口无条件 flush_ready |
| 配网后无限重启 | WIFI_STORAGE_FLASH 自动重连撞 set_config | RAM 存储+容错化 |
| 断网冷启动时间=1970 | 无人用 RTC 种子系统时钟 | seed_time_from_rtc_once（wifi_init_once 末尾）；⚠️ SNTP 仅配网流程跑（常态化待定夺） |
| TWDT already initialized 噪音 | 系统已自动 init，watchdog 再 init | 已 reconfigure 兜底；panic 跟随 CONFIG_ESP_TASK_WDT_PANIC（诊断期=y） |

### 渲染/显示
| 症状 | 根因 | 修法 |
|---|---|---|
| 拖动路径永久残留 | **compose_region 步骤1/3 漏加 x 列偏移**——脏区 framebuffer 从未重铺，blit 发陈旧像素；背景近纯色肉眼不可见 | 两层补 +x；待照片终验 |
| 显示颠倒/镜像（方向三改） | 组合2=垂直颠倒、组合1=水平镜像（菜单文字照片实证） | **组合 0 = swap=false + 无镜像**（原生即正立；教训：判读必须用有手性的标记，白点/位置分不清镜像与颠倒） |
| 触摸/拖动方向不对 | 方向固化后触摸映射未跟转 | **8 候选自校准**（NVS calib/tmap，点屏切换，白点落指尖=正确）；定稿后 MP_TOUCH_CALIB=0 |
| 气泡不显示 | `font 1 not loaded`（服务端字体链断 E12）→ LVGL 渲染静默失败 | **内置 5x7 兜底** bubble_render_fallback（配对码=纯数字不依赖字体链） |
| 黑屏（历史） | 手写 QSPI 时序不可靠 | 换官方 waveshare BSP |
| 右缘 480 残影 | mark/compose/blit 三路矩形分歧 | ent_screen_rect_at 权威矩形（先 clamp 再取偶） |
| **预设推送后宠物消失/全黑** | **s_files 只增量 upsert 不剪除**：服务端重新登记后旧 hash 不被服务+本地 LRU 淘汰，但 parts_path 按"第一个 paperdoll 匹配"仍选中死 hash → open errno=2 → 实体 0×0 | `prune_stale_locked()` 每次完整清单对账剪除；**rev 相等 304 分支也剪**（TF manifest.json 可能是旧固件混入版），剪后不早退走完整路径触发重绑 |
| **地图只剩天空+黑线** | `g_map_static_only=true` 花屏二分调试态随提交入库：tile+条带段全不装载；新分段格式下 static 本身=天空段 | =false。判读：boot 无条带 parts 的 mpak open = 还开着 |
| 宠物被拖/换动作后大半出屏 | 动作切换画布重展（fly 368×258≫stand）后旧 drag 失效 | `ent_bounce_if_offscreen()` 每帧：出屏>50% → drag_clamp 夹回贴边 |

### 输入
| 症状 | 根因 | 结论/修法 |
|---|---|---|
| 中键"没反应" | **按键是好的**（NVS 计数实证 7 次）；反馈横幅画在颠倒画面里看不见+没放歌音量变化无声 | 方向修正后横幅可见；menu 态中键=选择器上移；⚠️ VOL 横幅在 MENU 态被 LVGL 整屏覆盖（设计如此） |
| 触摸乱跳/坐标垃圾 | CST9220 帧缺有效性门 | ACK=0xAB 且 d[0]&0xF==0x06 才有效 |
| 倾斜互切风暴 | 竖握 roll≈±80° 永超阈值 | 45° 内才算倾斜；SET_ACTION 同名跳过 |

### 网络
| 症状 | 根因 | 状态 |
|---|---|---|
| 板子 TCP 被服务端 RST（errno=104） | **服务端与请求内容均无责**（Mac 同网段逐字复刻全 200+连发 200；板子 TCP 握手能过、open 阶段被掐） | 已自然恢复（hello ok 多轮）；裸 socket 探针 `raw_tcp_probe_once` 常驻失败路径，复发时自动裁决"NAS 按源拦截 vs 客户端层" |
| WiFi 每 2-5s 被掐 | poller 循环无条件 connect_sta 掐断活连接 | s_sta_connected 门 + s_conn_busy 闩（set_config 失败须清闩） |
| URL 双坑 | 手输无 scheme / 浏览器自动 https | NVS 读入时 scheme 归一化（https 强制降 http） |
| **串口连换多张地图** | push 端点**双发切图指令（立即+15s）**+ 有人连续推图——设计行为不是 bug | 判读：地图装载日志 hash 各不相同=收到的指令就是不同 hash（载荷=BGMAP hash 非 id） |
| **manifest 拉到了但素材还是旧的** | 设备 s_files 与服务端清单不对账（历史遗留混入） | 已修：每轮 sync 对账剪除；复发先查 boot 日志"清单对账：剪除 N 个" |
| **同步 40B 大清单静默失败** | 响应缓冲 16K < rev30+ 清单 18.6KB，collect 中断 | MANIFEST_RESP_CAP 16K→24K |

### 音频（BGM）—— 2026-10-01 全程无声定案
| 症状 | 根因 | 修法/铁律 |
|---|---|---|
| **BGM 点了播放毫无反应/全程无声**（命令被消费、曲目表建成 1167 首、PA 也开了，就是没声音，串口零报错） | **`codec_es8311_init()` 全仓零调用**：`hal_contract.h` 的 `mp_codec_init()` 只有定义没人调，`bgm_start()` 里也没有——git 取证 `051ff84` 的 bgm_start 有 `mp_codec_init(44100);`，`feb1360` 重排建栈顺序时删掉后再没恢复（main.c 注释"codec_init 在内"成了过期承诺）。后果：`s_tx/s_dev` 恒 NULL → `codec_es8311_set_sample_rate()` 返回 `ESP_ERR_INVALID_STATE`、`codec_es8311_write()` 返回 `ESP_ERR_INVALID_ARG`，而 feeder 两处返回值原先把丢弃 → 解码/环形缓冲/PA 全在正常跑，没有一字节进 I2S | `bgm_start()` 在建栈**之后**补 `mp_codec_init(44100)`（顺序铁律：mp_codec_init 会吃内部堆连续块，先建 codec 会让 8KB bgm 栈再也建不起来）；feeder 的 `codec_write` 返回值必须计数+报错（`g_bgm_wr_err`），**禁止再出现"返回值丢弃"的静默失败** |
| 起播/拖动期"卡顿"（可听断音） | 三处叠加：① 起播时"ring 里有一帧就往 I2S 送"，首包未到就抽干 DMA；② I2S DMA 默认只有 6×240=1440 帧（22.05kHz ≈65ms），而 feeder 每次写 1152 帧（52ms）→ 抗抖动余量仅 ~13ms；③ feeder 优先级 4，与 bgm/poller/events/asset_dl（同为 prio 3）挤在 PRO 核，素材同步/长轮询一次调度抖动就抽干 DMA | ① 起播预缓冲 `PRIME_MS=400`（带 2.5s 超时兜底）再开声；② I2S DMA 抬到 8×512=4096 帧（22.05kHz ≈186ms）；③ feeder 优先级 4→6（仍低于 WiFi23/TCP-IP18）。取证口径：tprobe 的「bgm 音频：断供/最低水位/I2S 写最长」 |
| "设了 BGM 没声音"判读顺序（排障用） | —— | ① `bgm 任务起步/ codec 初始化完成` 有没有（没有=启动期就断了）② `曲目表构建：命中 N 首`（0 首=TF audio 包缺失）③ tprobe 的「丢弃[无解码器/断网/置灰]」是否在涨 ④ `codec写失败` 是否在涨 ⑤ 断供/水位（ring 侧）⑥ I2S 写最长（DMA 侧） |

---

## 五、跨端契约（C# 导出 ↔ C 解析，全部真机炸过后定稿）
- MPAK：信封头 **40B**（magic 16B 含 8B 零填充）；PARTS 索引 **20B**（尾 2B pad）；LAYOUT 帧头 **12B** 无 pad、piece **12B**（尾 1B pad）；FONT 头 **8B**、glyph **12B**；AUDIO 轨 **108B**；**offset=位图区相对**。
- **BGMAP 分段格式**（运行时登记的 237KB 新包，~240×240 1x）：static_back=**天空段**
  （back 切段的段0），场景中间层=**speed=0 条带段**（wire strip y=0、全视口高、
  blend bit0=1 带 1bit 掩码，PARTS 小包经 part_ref 引用），地形在 tile 层+掩码。
  旧 950KB 包=全部 back 合成一张 static——两种格式固件同一路径都能吃，
  **勿按 static 内容假设"static=完整场景"**（花屏二分案教训）。
- LAYOUT x/y 已含帧位移（导出契约），move 字段勿重复叠加；piece 顺序=权威绘制序。
- 协议：poll 回 `{commands:[{seq,type,payload}],lastSeq,mrev}`；hello 返回 `pairingCode`/`deviceId`/`manifestRev`；manifest kind 全大写（固件已 strcasecmp 兼容）。
- 通用规律：**两端各自实现时必须拿真实响应样本对表**；格式文档字段和≠标注步长时以固件读法为准。

---

## 五B、显示撕裂（“无垂直同步”现象）方案研究（2026-09-27，未动代码）

### 现象定性
AMOLED 内置 GRAM 以固定帧率自扫；QSPI 重写窗口期间扫描线恰好越过该区域 → 同屏出现半新半旧两帧画面 = 撕裂（与 PC 关垂直同步同机理）。

### 业界标准解法 = TE（Tearing Effect）脚同步
面板在每帧消隐期输出 TE 脉冲 → MCU 等 TE 沿再开始写 GRAM，且传输须在一帧内完成。
- **CO5300 支持 TE**：驱动 init 序列已发 `0x35`（TEON，esp_lcd_co5300_spi.c:216）——面板侧已开启。
- **本板没有引出 TE**：wiki GPIO 表完整（LCD 仅 CS/QSPI×5/RST）；keyscan 探针（20ms 轮询 60s+）在仅剩空闲脚 16/47/48 上零脉冲——TE 未接任何 GPIO。**硬件垂直同步在本板不可行**（除非飞线）。
- 佐证：Waveshare Watch RS（同 CO5300）引 TE 到 GPIO13 做同步——控制器支持，只是本板没走线。

### 本板可做的软件缓解（按效果排序，均未实施）
1. **传输窗口压缩**：脏区 368×288×2≈212KB @QSPI40MHz≈10.6ms < 一帧(16.7~33ms)——部分更新本身能塞进帧内，撕裂只在扫描线恰好在传输中途越过窗口时发生；进一步缩小脏区/分条带写可降低概率。pclk 40MHz 是宏定值（BSP 同款），提 80MHz 需查 CO5300 QSPI 上限，有风险。
2. **软件帧锁定**：测出面板实际刷新周期后，把 blit 起点锁定到固定相位（无 TE 只能统计对齐，不能精确）。
3. **菜单态全屏拷贝必撕裂**：menu_buf 整屏 460KB≈23ms > 一帧——菜单重绘改 LVGL 脏区增量上屏（放弃每帧全屏 memcpy+blit）是菜单侧最大的一刀。
4. 现状已有的缓解：30fps tick、拖动 66ms 节流、脏区增量重绘。
- 同类板（LilyGo AMOLED 无 TE）同样撕裂（社区未解）——这是无 TE GRAM 屏的固有产物。

### 结论
真解需硬件（TE 走线，改板一版一行即可，面板/驱动均已就绪）；本板只能软件缓解，天花板=“明显减少、不能根除”。

参考：Espressif 官方撕裂文档、Waveshare 显示基础、LilyGo issue #21、LVGL/Aduino 社区讨论（链接见会话记录）。

## 五C、换板调研（圆形需求，2026-09-27，官方文档逐一核实）

| 板 | 屏 | 驱动 | TE 脚 | 撕裂 | 淘宝价 |
|---|---|---|---|---|---|
| **AMOLED-1.75**（电子吧唧形态） | **1.75" 圆 466×466 AMOLED** | CO5300 QSPI | **✅ GPIO13**（HARDWARE_REFERENCE.md 实锤） | **可根治** | ¥228-248 |
| AMOLED-2.06（手表形态） | 2.06" 圆 410×502 | CO5300 QSPI @80MHz | ✅ GPIO13 | 可根治 | ¥185 |
| AMOLED-1.43 | 1.43" 466×466 | SH8601/CO5300 | ❌ 无 | 不可 | — |
| LCD-1.85B | 1.85" 圆 360×360 **LCD** | ST77916 | ❌ 无（GPIO 表只有 CS/PCLK/D0-3/RST/BL） | 不可 | — |
| AMOLED-2.16（现用板） | 2.16" 方 480×480 | CO5300 | ❌ 无 | 不可 | — |

**1.75 迁移成本极低的证据**（官方 HARDWARE_REFERENCE.md 引脚表 vs 我们 profile）：
LCD_DATA0-3=GPIO4/5/6/7、PCLK=38、CS=12、RST=39——**与现板逐脚相同**；TP_INT=11/TP_RST=40（CST9217，与 CST9220 同族）、I2C=14/15、PA_CTRL=46 全同。差异仅：+TE(GPIO13)、分辨率 480→466、RTC_INT 位置待查（13 被 TE 占）。

**"官方支持视频"的真相**：1.85B wiki 实无视频 demo（仅设备测试+MP3+语音识别）；"支持视频"= 能解码能放的能力话术。视频帧帧不同天然掩盖撕裂（UI 拖动撕裂最刺眼、视频最不显眼）；Tasmota 社区有该板横向撕裂实录（每秒数次）。

## 六、CST9220 触摸帧格式（消费侧 input_dispatch 直读）
- 8 字节帧 @reg 0x00：d[0]低半字节==0x06 且 d[6]==0xAB 才有效；触点数=d[5]&0x7F
- 12 位重组：`rx=(d[1]<<4)|(d[3]>>4)`；`ry=(d[2]<<4)|(d[3]&0x0F)`
- 映射：8 候选自校准（`tmap_apply`，NVS calib/tmap，当前=用户点屏选定）；抬起帧回填最后有效坐标。

---

## 七、标定与取证状态（NVS namespace="calib"）
| key | 含义 | 当前值 |
|---|---|---|
| k | 方向组合（已弃用：固化进 display_init） | 2（无效） |
| tmap | 触摸映射候选 0-7 | 用户点屏选定中（默认 6=旧口径） |
| key0 | 中键累计按下次数 | **7（GPIO0 通路实证良好）** |

### 临时件清理清单（验收后一次性处理）
- [ ] `MP_TOUCH_CALIB`→0（input_dispatch.c）
- [ ] 探针降级：flush(500ms INFO)/ENTPOS(1s INFO)/keyscan(常驻，`MP_KEY_SCAN_PROBE`→0)/drag 探针
- [ ] `CONFIG_ESP_TASK_WDT_PANIC`→n（sdkconfig+defaults）
- [ ] calib 红蓝线/白点加关闭路径（现首触常驻；无 render_calib_set(false) 调用者）
- [ ] 气泡/横幅在 MENU 态不可见（LVGL 整屏覆盖，设计如此，待产品定夺）
- [ ] touch_cst9220.c 文件头与 input_dispatch 两处映射注释矛盾未清
- [ ] UDP 帧倾倒触发通道定稿：服务端 command 白名单放行 `screenshot` 后，
      bubble `::shot` 魔数可退役（设备侧两种口径均已支持，见 §2.6）

---

## 八、遗留问题（服务端/导出端，固件侧无责）
- **E12 字体链断**：FontSizes{16}、seed/fonts 孤儿、无 CJK charset → 气泡中文/字号不可用（配对码已用内置字体绕过）
- E7 NPC 导出器、E8 QQ 音源、设备日志/mDNS、Web 表情入口、entities[] 未上线
- minimp3 VBR gapless 未处理（首尾 ~529 样本过渡，影响极小）
- 常态 SNTP 重校准未做（现仅配网流程校时，长期不断电会漂移）
- **command 白名单未含 `screenshot`**（AdminEndpoints.cs）：设备 poller 两种口径
  （legacy `{"t":"screenshot"}` / 现代 `{"type":"screenshot"}`）均已支持，放行一行的事
- 相机下移实验板B侧验收（ground_cam_shift_px=248）归板B session；真 foothold 彻底
  入镜需服务端按 §14.1 结论重定导出相机（暂缓，用户定稿板A用原景）

---

## 八B、整图地图性能账（2026-10-01 实测，改包格式前必读）

### 硬数据（真机 board216 / dev-693ea4）

| 项 | 实测值 | 说明 |
|---|---|---|
| SD **顺序**读（32KB 块 POSIX pread） | **1336 KB/s** | 1-bit SDMMC@20MHz 理论 2560 ⇒ 卡与总线都没问题 |
| 整图**窗口**填充（跨行距逐行 / 64KB 预读） | **~130 KB/s** | static 层行距 4540B 而每行只需 672B ⇒ 读放大 6.7× |
| 8 条带图一次装载 | 8.5 s（`窗口读 4700 行 / 1712 KB / 8534 ms`） | |
| 14 条带图（200000000）一次装载 | 17.2 s（`5774 行 / 1935 KB / 17164 ms`） | |
| 单帧满层合成 | **compose 128 ms + blit 32 ms** | 23 万像素逐点取缓存 + 掩码判定 |
| **时间滚动条带周期重填** | **每 ~2.2s 一次、每次 1775~2440 ms** | 见下，最隐蔽的一条 |

### 已实施的优化（本轮，累计效果）
1. **装载期直落相机**：原先"装载按置中填一遍缓存（10.7s）→ 应用 NVS 相机跨度过大 → RELOAD 整窗重填（14.2s）"= 开机 25s；改成装载前把该图 NVS 相机交给渲染层，**只填一次**（`整图装载直落相机 (x,y)`）。
2. **按带读**（`wc_fill_cache` 非 wrap 层，band=32 行）：原来每行一次 h=1 请求，预读块跨不了行；改后一次请求覆盖多行。
3. **条带预读单例**（像素 64KB / 掩码 8KB，同 fd 且区间命中即零 syscall）替代"每行一次裸 pread"。
4. **缓存 336→288**（可见 240 + 两侧各 24）：填充行数 -14%，PSRAM 常驻 1552KB→1190KB。
5. **不在可见范围的条带不读源**（按"带世界 y ∩ 相机可见区间"判定；14 条带图实测只开 9 个条带文件）。
6. **调参期不降级**（用户定稿）：背景必须正常全层渲染，性能只能来自"只加载用到的块"。

### 仍然存在的两条硬伤（未解决前"体验差"无法根治）
1. **跨行距读放大**：窗口读只有 ~130KB/s。唯一治本 = **服务端分块(tile)导出**，
   固件按块连续读（契约 `docs/ai/map-tiled-format-contract.md`，目标整窗 ≤1s、补边 ≤0.2s）。
2. **时间滚动条带每 ~2.2s 触发一次 ~2s 的缓存重填**（真机日志同位置反复出现
   `相机 → (1900,878) 窗口读 1654 行 / 补 ~280 列 / ~90 KB（1775~2440 ms）开条带文件 5 次`）：
   条带相位按 4Hz 量化推进（`strip_window`），越过 24px 余量就要补整条新露出的列
   —— 因为逐行读慢，补一次就要近 2 秒，等于**设备常态有 ~90% 时间在补条带**。
   缓解：交互期已冻结视差刷新（`render_note_activity()` → `g_ui_active_until_ms`，触摸/按键都会调），
   所以拖动时不会撞上它；但闲置期它仍在周期性吃掉渲染预算。
   **根治同样依赖分块包**（补边从 ~1.8s 降到 ~24ms）。

### 判读入口（排障时先看这三行）
```
rc: 相机 → (x,y) 窗口读 N 行 / 补 N 列 / N KB（N ms）开条带文件 N 次   ← 填充成本
调参合成 N ms（compose A + blit B）level L 区域 WxH                    ← 合成成本
flush 哨兵（N 笔/30s）：compose 均 A 最大 B ms | blit 均 C 最大 D ms    ← 30s 汇总
```

---

## 九、工作纪律（血泪教训）
1. **无真机实证不得宣称修复**（多次"修好了"被照片证伪——必须 build 0 告警 → 烧录 → 抓日志/拍照）
2. 旋转/镜像类改动**一轮一变量**，必须照片确认；白点测映射分不清 180° 颠倒（点无手性）——判读要用文字/三角等有手性的标记
3. 多 agent 纪律：前台 ≤5/波 + 文件边界 + 派前探针定契约 + 波后主线程接缝对读；agent 汇报不算自测
4. 用户红线：不留占位/假功能；改 defaults 必须合法符号名并 grep sdkconfig.h 验证；python 批量 patch 必须断言
5. 每次验收流程：build → 烧录 → monitor 30s → 拍照给用户确认 → 才许 git commit（当前全部改动未 commit）
