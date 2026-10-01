# 真机联调遗留问题清单（2026-09-26 · 多 agent 修复波次后更新）

> ⚠️ 技术事实/根因台账/契约/操作手册已迁移至 **`technical-reference.md`**（唯一权威技术文档），本文档只保留联调动态。

> 状态：真机联调进行中。本文档记录全部未关闭问题、已尝试方案、证据与下一步。
> 每次修复必须附带真机证据（日志/照片），禁止"代码写完即报完成"。

---

## 〇B、2026-10-01 凌晨波次（预设推送/BGM/地图渲染 三大案闭环 + 滚筒菜单定稿）

> 主线：模拟 Web 页面点击（BGM 播放 + 预设推送）对照串口自测整条链路 → 连环牵出
> 三个真机大案，全部根因闭环并上板验证。**新取证手段：UDP 帧倾倒（见 2.6），本轮
> 诊断全程依赖，可远程看到屏幕真实像素，不用拔卡。**

### B1. 滚筒菜单上板定稿 ✅（用户口头验收）

- 羊皮纸木质主题 / 全中文（regen.sh 自动提取用字重烘焙）/ 触屏滚筒 / 短列表 NORMAL 模式 /
  锁屏幕滚动。用户验收口径："除了纸娃娃和BGM都可以了"。

### B2. BGM 播放链路自测全通 ✅（设备侧无责）

- 用 Web 同款请求 `POST /api/admin/devices/{id}/command {"type":"bgm","value":"play"}`
  → 202 seq → 设备 poll 消费 → 拿曲目 id → codec 启动 → hum 表情 → 曲目表 1167 首 →
  会话返回，**全程零错误**。
- 判读法：tprobe 的 bgm 消息/feeder 计数是 **10s 周期**打印，"看起来没日志"先等 tprobe；
  `since` 涨=命令被消费。

### B3. 预设推送后宠物消失/全黑屏 ✅ 根因闭环（本轮最大案）

- **复现**：推送休塔尔克预设（PUT petConfig）→ 服务端 rev 32→33 → 设备同步成功、
  字体全部换新 hash，但 parts/layout 解析出**已不在服务端清单的旧 hash**
  （本地 LRU 已淘汰）→ open errno=2 → 实体 0×0 → force_redraw 全黑。
- **根因**：`asset_dl.c` 的 s_files 只增量 upsert **从不剪除**——服务端重新登记后
  旧条目不被服务、文件被淘汰后也永不再下（下载循环只遍历清单），但
  `parts_path(NULL)` 按"第一个 paperdoll 匹配"永远选中排在前面的死 hash。
- **修法**：`prune_stale_locked()`——每次拿到完整清单都以服务端 assets 为准对账剪除；
  **rev 相等的 304 分支同样剪**（TF manifest.json 可能是旧固件写入的混入版本）；
  剪除后**不早退**，继续走完整路径（关键文件门+热切回+MANIFEST_SYNCED 重绑通知）。
- **验证**：烧录后 boot 日志 `parts 路径=…de4ed70d… rc=0`、`layout … rc=0`，宠物恢复。
- **连带修复**："后台下载补齐：已绑定 TF 素材"路径原不查 render rc（谎报成功），已补
  parts/layout rc 日志。

### B4. 地图只剩天空 + 两条黑线 ✅ 根因闭环（调试开关入库案）

- **现象**（UDP 帧实证）：上半屏浅蓝竖条纹天空、底部一条地形、中间两条黑横线。
- **根因**：`compositor.c` 的 `g_map_static_only=true`（花屏二分调试态）随 fbceb82
  提交入库——**tile 层和全部条带段不装载**。新分段导出格式下 static 本身就只是
  天空段（back 层序修正后 back 按条带切段，段0=static 天空，树冠/房子/丘陵在
  speed=0 条带段里），只剩 static = 只剩天空。
- **修法**：`=false`。烧录后帧实拍：蘑菇屋+红枫树冠+绿丘+宠物全部在。
- **判读法**：boot 日志只打开 2 个 parts（paperdoll+clock）、没有任何条带 opens
  = 此开关还开着；正常装载应有 4 个条带 parts 的 mpak open。

### B5. "背景变了"（红框浅蓝 vs 蓝框原景）✅ = 跨 session 工作区冲突案

- **真相**：另一 session（板B）把 compositor.c 的 `MP_GROUND_CAM_SHIFT_PX` 宏
  0→248（相机下移实验重开），板A构建被动带上 → 板A画面下移 124 世界像素、
  蘑菇屋/树冠移出画面（§14.5 已知代价），用户报"背景变了"。
- **终解（终结宏开关之争）**：删除全局宏，改 **profile 字段
  `ground_cam_shift_px`**（amoled216.h）：板A=0（定稿原景），板B(lcd185b)=248
  保留实验。compositor `render_init` 从 profile 装载，运行时判定。
- **同案连带**：板B session 在 input_dispatch.c 留下半成品（`s_k0_menu_latch`
  使用先于声明 → 板A编译失败）——声明已前置修复。**教训见 2.4B 铁律第 7 条。**

### B6. "000010000 是拉取失败了吗" ✅ = 服务端未登记案

- rev33+ 清单只有 200000100/100000000/104000000/200000000 四张 BGMAP
  （另一 session 用新 push 端点推的）——**000010000 没人推过，不是下载失败**。
- 修法：`POST /api/admin/devices/{id}/push {"kind":"map","id":"000010000","switch":true}`
  → 运行时登记（WZ 打包 237KB 新格式包）+ rev bump + 自动切图。设备装载 rc=0。
- **注意**：push 端点会**双发切图指令（立即 + 15s 后重发）**——串口里"连换 4 张
  地图"= 有人在连续推图 + 双发机制，是设计行为不是 bug。

### B7. 越屏 >50% 弹回边框 ✅（用户定稿新需求）

- 场景：动作/表情切换时画布绕 origin 重展（fly 368×258 ≫ stand 214×168），
  setter 时刻夹好的 drag 对新画布失效 → 宠物大半出屏。
- 修法：`ent_bounce_if_offscreen()` 每帧合成前检查——可见面积 <50% 即把 drag 按
  当前画布夹回"矩形完全在屏内"（与 drag_clamp 同数学，贴边弹回）；旧/新位置双
  `mark_ent()` 防瞬移残影。可见 ≥50% 不干预（全屏拖拽保留）。

### B8. UDP 帧倾倒取证通道 ✅（新基础设施，永久可用）

- 触发：bubble 文本魔数 `::shot`（服务端 command 白名单未放行 screenshot，
  bubble 是现成通道；帧内不含该气泡）。设备把 g_fb（最终合成帧 RGB565）经
  UDP 广播 :9999，同时 TF 截图照旧落 /sdcard/debug/shot_NNN.bmp。
- 操作手册/坑位 → **technical-reference.md §2.6**。
- 板A当前验收帧：蘑菇屋场景完整、宠物 fly 动作 16 pieces miss=0、
  站位 y=460、合成完整性失败 0、无残留告警、内部堆 54K。

### B9. 本波遗留（未关闭）

- [ ] 服务端 command 白名单未放行 `screenshot`（AdminEndpoints.cs type 校验）
      ——设备侧两种口径均已支持，放行后 Web 可直发。
- [ ] fly 态 compose 均 46-59ms/最大 103ms（大画布 480×448 脏区），30fps 边缘，
      待用户实测观感；如卡顿再议脏区细化。
- [ ] 幻触摸待观察：设备平放时出现 `触摸按下 raw(239,458)` 与 drag 变化
      （CST9217 有效性门已在，来源待查：掌压/桌面/静电）。
- [ ] 相机下移实验板B侧验收（ground_cam_shift_px=248）由板B session 负责。
- [ ] tprobe/取证探针在验收后统一降级（见 §五 清理清单）。

---

## 〇、方向定稿（已上板）

> "USB 口朝右，USB 口左边那个边当做底边，不准篡改"

- 面板级已应用：`swap_xy=true + mirror(F,F)`（display_co5300.c，两轮照片校准定稿）
- 所有渲染坐标/触摸映射以此方向为基准

---

## 一、本轮根因修复（代码已上板，等照片终验）

### 1. 拖动路径残留 ✅ 根因已修（待照片确认）

- **根因**：`compose_region()` 步骤 1（static_back 铺底）与步骤 3（tile 层）漏加 x 列偏移——
  脏区从第 x 列开始，背景却从第 0 列开始写，脏区 `[x, x+w)` 的 framebuffer 从未被重铺，
  blit 发出去的是陈旧像素（= 上一帧实体图像）。背景近纯色时错位拷贝肉眼看不出，
  实体移动后才暴露 → "三轮修不掉"的真相。
- **修法**：static_back 的 memcpy/memset 与 tile 层（含 mask 路径）全部补 `+x` 偏移。
- 验证：待真机拖动拍照（本轮日志侧无异常，flush 探针 120 次/75s 节奏正常）。

### 2. 拖动卡死 / NO_MEM 风暴 ✅ 根因已修（真机实证零风暴）

- **根因链**（三层，全部实证）：
  1. `blit_be` 行高取 25（奇数）→ `display_blit` 内 `even_round` 外扩 +1 行 →
     `aw==w && ah==h` 快速通道失效 → 走 PSRAM 暂存路径；
  2. esp_lcd/spi_master `setup_priv_desc` 对 PSRAM（非 DMA-capable）缓冲**每次排队强制
     malloc 一整块 MALLOC_CAP_DMA 内部堆拷贝**（~25KB/次）；
  3. 本板内部堆仅 ~143KB，WiFi 初始化后所剩无几 → 分配失败 → `ESP_ERR_NO_MEM`
     → 这就是"首次失败总在 WiFi GOT_IP 之后"与换队列深度/同步化/重试全无效的原因。
- **修法**：blit_be 行高恒取偶（快速通道恒命中，DMA 直推零分配）+ s_blit_stage 对齐 64B
  + 显示 IO 槽位信号量背压（在飞≤队列深度，完成回调归还，2s 泄漏探针）+
  stage 24KB→12KB（内部堆余量）。
- **验证**：修复前 80s 7404 条 "send color data failed" 且伴随 watchdog 重启；
  修复后 75s **0 条**、零重启、零任务创建失败、渲染探针全线正常（boot8 日志）。
- 顺带实证：内部堆 143KB 是系统性瓶颈，任务栈分配在启动挤压窗口期会随机失败
  （selftest 从未跑成、keyscan 首创失败）——keyscan 已加重试，selftest 已删除。

### 3. 人物不居中 ✅ 公式已证精确（剩余问题在素材侧或已修复）

- ENTPOS 探针真机读数：`sx=240 sy=440`（无素材时）→ 加载后 `sx=122 sy=198 cx0=-59 cy0=-121`
  反推 body 锚点 = (240, 440) = **水平正中 + 脚底距屏底 40px，与规格分毫不差**。
- 结论：固件公式正确。若照片仍偏，偏移来自素材的 body 锚点约定（导出端），
  用 ENTPOS 读数对照照片即可定位（探针保留，1 次/秒，grep `ENTPOS`）。

### 4. 触摸方向 / 三套坐标系 🟡 等白点实测（判读表已备）

- 现行映射 `(raw_y, 480−raw_x)` 在跑（真机日志实证 raw/map 成对输出）。
- **发现**：现行映射与 BSP 官方口径 `(480−raw_y, raw_x)`（mirror 先、swap 后）**恰好差 180°**，
  两者必有一错（固件内两处注释互相矛盾）。
- 白点回显探针在板（POKER 态触摸即显）。真机判读表（按四角+中心各点一次）：
  - 四角全对 → 定稿不动
  - 对角互换（180°）→ 改 `sx=480−ry, sy=rx`
  - 仅左右镜像 → `sx=480−ry, sy=480−rx`
  - 仅上下镜像 → `sx=ry, sy=rx`
- 附带发现：calib 红线/白点**没有关闭路径**（全仓无 `render_calib_set(false,…)`），
  首触后常驻——标定期是特性，定稿后需加关闭逻辑。

### 5. 中键（GPIO0）无日志 🟡 软件链路已排除，等按键实测

- 逐项排查：init 已调（input_dispatch_task 入口）、20ms 轮询在跑、消抖与已验证的
  菜单键同构、GPIO0 输入+上拉配置正确——软件无 bug，加了无条件 raw press 日志。
- 60s GPIO 扫描探针在板（候选 0/16/47/48，跳变即打日志，创建失败自动重试）。
- **最可能根因：中键是接 CHIP_PU 的硬复位键**（wiki："Key2 同接 GPIO0 与 CHIP_PU"）
  ——按下即整机重启，固件无从感知，与"完全无日志"严丝合缝。
- 真机判读：按中键 → 若出现重启横幅/日志即实锤；若 keyscan 报某脚跳变 → 改
  `KEY_GPIO0_PIN` 一处即可。

### 6. BGM 播放/切换 🟡 两处"必炸"缺陷已修，等上板验证

- **minimp3.h 是占位 stub**（不产 PCM，烧板必无声）→ 已 vendored 上游 lieff/minimp3 原文。
- **bgm 任务栈 8192**（mp3 解码 scratch ~16KB 在栈上，首帧即溢出）→ 24576。
- ES8311 上电序列与仓库内 esp_codec_dev 权威参考逐条比对重写（REG09 16bit 字长
  缺失、REG02/03/04/05 等整段缺失）。
- 其余：播放中选曲延后执行（原被静默丢弃）、PA=GPIO46 直控链路确认完整。
- 验证：菜单 BGM → 播放；无声则 dump es8311 寄存器对照（见 agent 汇报排序）。

### 7. 时间同步 🟡 冷启动种子已补，常态重校准待定夺

- **真缺口**：全工程无人 settimeofday——断网冷启动必 1970。已补 `seed_time_from_rtc_once()`
  （挂 wifi_init_once，读 PCF85063 → 有效则种子系统时钟；真机日志验证路径工作正常）。
- SNTP 校准只在配网流程跑（正常开机不跑）——设计取舍，长期不断电会漂移。
  如需周期重校准须异步化（现阻塞 15s），待定夺。

### 8. 配对输码闭环 🟡 板侧无责实锤，等查服务端

- 固件诊断日志上板：`mp_http_tx_fail ret=0x7002 (ESP_ERR_HTTP_CONNECT) errno=104
  (Connection reset by peer)` ——hello/manifest/event 全部在 **TCP 建连阶段被服务端
  主动 RST**（连续多轮启动复现，errno 稳定 104）。
- 结论：板子网络链路正常（WiFi/DNS/TCP 路由都通），**服务端 <NAS_IP>:38090
  在拒绝连接**——查服务端进程状态/端口监听/防火墙（用户行动）。
- 服务端可达后配对码气泡链路已在（hello 下发 → 字体绑定后重显）。

---

## 二、已解决问题（存档）

| # | 问题 | 根因 | 修法 |
|---|---|---|---|
| 1 | 黑屏（首版） | 全工程未调 lv_init | bridge_init 补 lv_init + tick |
| 2 | 菜单呼出后重启 | flush 未调 flush_ready 死循环 | 补 lv_display_flush_ready |
| 3 | 配网页打不开 | httpd 任务栈分配失败（内部堆碎片） | 三档栈降级重试 |
| 4 | manifest 读不到 | FATFS LFN 未开 | 修 CONFIG_FATFS_LFN_HEAP=y |
| 5 | 配网后无限重启 | esp_wifi_start 自动重连 + set_config 撞态 abort | RAM 存储 + 容错化 |
| 6 | WiFi 密码错 | 配网页手输 13≠12 | 用户重输正确密码 |
| 7 | IMU 读数恒 0 | CTRL7 写了自测位 + CTRL2/3 写反 + ODR 非法 | 寄存器全表修正 |
| 8 | 素材包解析全失败 | 信封头 32B vs 40B 等五处 | C# 写出器对齐固件解析 |
| 9 | 内部堆 182KB 占满 | LVGL 大缓冲走内部 RAM | ALWAYSINTERNAL=4096 |
| 10 | 拖动路径残留 | compose_region 列偏移缺失（见上 1） | 补 +x 偏移 |
| 11 | 拖动卡死/NO_MEM 风暴 | even_round 外扩→PSRAM 暂存→驱动强制内部 DMA 拷贝（见上 2） | 偶数行高+对齐+背压+12KB |

---

## 三、方向定稿（唯一标准）

> USB 口朝右握持时：**USB 口左边的那条竖边 = 屏幕底边**。人物脚底踩在这条边上、文字正立。

面板级 swap_xy=true + mirror(F,F) 已上板生效，游戏逻辑零改动。

---

## 四、验收流程（强制）

1. 改代码 → idf.py build 0 error
2. 烧录 → monitor 抓 30s 日志
3. **拍照发给主线程确认视觉**（不接受"应该好了"）
4. 主线程确认后 git commit（附真机照片描述）

## 五、本轮待清理项（照片验收后一次性处理）

- [ ] 诊断探针降级/移除：flush 探针（INFO 500ms）、ENTPOS（1/s）、keyscan 扫描
      （`MP_KEY_SCAN_PROBE`→0）、TWDT panic（sdkconfig `ESP_TASK_WDT_PANIC`→n）
- [ ] calib 红线/白点加关闭逻辑（现首触后常驻）
- [ ] 触摸映射按白点判读表定稿（如需改，同步清理 touch_cst9220.c 两处矛盾注释）
