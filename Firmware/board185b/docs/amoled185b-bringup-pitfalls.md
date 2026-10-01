# 1.85B 显示 bring-up —— 坑与教训实录(2026-10-01)

> 本档随板存放于 Firmware/board185b/docs/（2026-10-01 双板目录化）。
> 本文档由主会话按用户要求落地。配套代码状态见 git 工作区(未提交清单在文末)。
> 记忆卡同步:`~/.zcode/cli/memories/.../amoled185b-bringup.md`。

## 一、硬件/板型事实(已实证)

- 板:微雪 ESP32-S3-Touch-LCD-1.85B,360×360 圆 LCD,驱动 IC ST77916(QSPI)
- 引脚:CS=21 PCLK=40 D0-3=46/45/42/41 RST=3 BL=5 TE=18;I2C SCL=10 SDA=11(≠216 板的 14/15)
- 器件:QMI8658@0x6B(在,INT 未引出)、PCF85063@0x51(在)、CST816S 触摸@0x15(未移植)、BQ27220@0x55(非 AXP2101)
- 按键(官方 wiki 实锤):仅两颗——**PWR 电源键=硬件电源通路不走 GPIO 固件读不到(官方明说"不支持自定义")**;**BOOT=GPIO0**(strapping 脚,只输入安全)。
- **BOOT 键=菜单键绑定定稿(2026-10-01,subagent 审查修正版)**:input_dispatch.c——`pins.key.menu<0`(185B)时 POKER/OFFLINE 态短按=菜单开关(释放沿,复用 key_fire_menu_toggle)、长按≥700ms=待机时钟(s_k0_menu_latch 状态机);CLOCK_DOZE 唤醒沿只唤醒不开菜单(st_before 快照修正 DOZE 不可达 bug);MENU 态仍走原 hold_tick(短按下移/长按 800ms 退出);BGM 控制条同构。216 板(menu=18)行为一字不变(POKER 态仍音量减)。
- **subagent 审查抓出的 2 个真 bug**(引以为戒):①按下沿直接 toggle → 长按"先开菜单 700ms 后转时钟"双触发(GPIO18 版是释放沿+clock_fired 互斥,必须同构);②note_interaction() 在状态快照之前执行 → DOZE 分支不可达,时钟态按键会"唤醒+开菜单"连招。
- 面板批次:RDDID(0x04) 读法 = `(reg<<8)|(0x0B<<24)` 寄存器在中间字节;本板 = v2(00 02 7F 7F),序列 185 条(官方 BSP 两批次表)
- 串口:新板 usbmodem21201,旧 216 板 21101(未提交的烧录全部显式 21201)

## 二、显示通路的坑(全部真机实证,按踩的顺序)

1. **esp_lcd_st77916 组件默认序列不可用** → 白花带。必须按 RDDID 选批次序列注入(vendor_config.init_cmds)。
2. **INVON 极性**:v2 表尾自带 0x21(INVON),本批次必须再补 0x20(INVOFF)——扫频实证(4 配置×三色竖条,走 draw_bitmap 队列路径才能显示;tx_param 手动路径在本板不显示,别用它做判读!)。
3. **字节序**:g_fb 是小端,面板要大端——刷新流必须逐像素 bswap,否则负片色;blit 路径的数据是合成器预交换的,两路不一致 = "闪烁+偶发一帧对"。
4. **零长度命令**(SLPOUT/DISPON)在 spi_master 半双工下不能带 USE_TXDATA/length,否则 INVALID_ARG 静默失败。
5. **RAMless 特性**:面板不锁存,必须持续全帧刷新(refresh_task ~15fps,stage 降级链 2/1 行,内部 DMA 堆)。单帧写入"成功"但屏不显示。
6. **ABBA 死锁**:刷新任务(持显示锁等帧锁)↔合成器(持帧锁等显示锁)→ 全系统冻结 15s。锁序必须统一:先帧锁后显示锁。
7. **堆门**:另一会话的 24KB 素材绑定门在 185B(空闲 ~9KB)上永久拦截娃娃绑定 → 娃娃不显示。**这是当前娃娃缺失的根因,等用户协调两会话门值**(4KB 门实测绑定成功零崩溃)。
8. **COLMOD 读回 0x58**(非标准),强制 0x55 低字节写不落地;R 通道轻微抬升的色偏未根除,疑似像素格式,待中间字节写实验。

## 三、素材/地图的坑(核心教训)

### 3.1 素材 profile 必须匹配屏
- 480 档素材(世界裁剪 240×240)在 360 屏:背景泛化缩放(240→360)可行;360 档素材(180×180)的 LAYOUT 语义与合成器不匹配(娃娃缩成碎片)——**决定:用 480 档素材+固件泛化缩放**。
- mpak.c BGMAP tile 层长度公式必须兼容导出端 4 字节对齐 pad(180 宽时 68850→68852,严格相等会 MPAK_ERR_FMT 拒载)。

### 3.2 【最大坑】"先裁剪再移镜头"是假动作
- 现象:我在导出器里"渲染完再改相机",或者在固件里对裁剪结果做平移/缩放——全是错的,坐标系统一性被破坏,画面位置永远对不上 216 板。
- **正确顺序:先定镜头 → RenderViewport(以该相机渲染)→ 再谈其他**。
- **镜头基准(用户定稿,2026-10-01):以 foothold 地面线为构图锚——地面线放在屏底往上 20px(或 30px)的水平位置**,地图整体相对这条线展开;不是"地图中心"也不是"clock 锚点"。
- 相机数据(000010000,480 视口 zoom=1):cam=(-175, 11.5),地图世界范围 X[-1310,960] Y[-892,915]。185B 整图 fit=360/2270≈0.1586(半分辨率图 1135 宽→视口 360)。
- foothold 世界 y≈245.5(480 设备坐标 708)。按地面锚定:相机 y = foothold_y − (vh/2 − 20/zoom)。

### 3.3 两个会话共享代码的纪律(血泪)
- compositor.c/mpak.c/render.h/provision.c/main.c 是两板共用文件——**185B 的适配不得直接改共享常量**(MP_GROUND_CAM_SHIFT_PX 改成含运行时变量的宏 → 480 板白屏事故,另一会话被迫回滚)。
- 正确姿势:板级差异进 profile(Kconfig/profile json/驱动内 if CONFIG_MP_BOARD_xxx),共享文件只做泛化(数学上对旧板退化为原行为,并跑 216 回归验证)。
- 工作区是两会话共用的:未提交的半成品改动会泄漏进对方的构建。改完尽快提交或明确标注。

## 四、当前状态(2026-10-01 本文档落地时)

- 板上固件:build-185b(1:1 满屏、INVOFF、bswap、帧锁、变换版地面表 kGround000010000_185b——但该表基于"错误的整图相机"推导,**地面锚定重构后需重推**)
- 素材:gh4w2M 目录的 FitWholeMap 版(相机=地图中心上移30px——与"foothold 锚定"不符,需重导)
- 娃娃:24KB 门拦截绑定(间歇出现)
- 触摸:未移植(CST816S@0x15,TP_RST=1 INT=4)
- 横幅:黑底白字 ✓(但左端裁切,480 布局常数)
- 颜色:接近正确,R 通道微抬升(COLMOD 0x58 之谜)

## 五、立即要做(按用户指令)

1. **导出器:foothold 锚定相机**——读地图 foothold y(已有 kGround 数据或从 WZ 读),camY = fooY − (vh/2 − 20/zoom);camX = 地图中心;FitWholeMap 分支改用此相机;重导素材烧录。
2. 固件:ground 表按新相机重推(或直接用通用线=屏底-20,因为导出已保证 foothold 在 20px 处——**推荐后者,一劳永逸**)。
3. 娃娃层 RC_SCALE 2x 的 360 适配(等门值协调后验证)。
4. 触摸 CST816S 移植。
5. 探针清理(~25 处 [取证] 日志)→ 216 回归 → 分批 commit。

## 六、未提交文件清单(git status 实测)

sd_tf.c/h、display_st77916.c、asset_dl.c、sdkconfig.185b、state_machine.c、clock_digits.c、compositor.c、mpak.c、render.h、watchdog.c、main.c、provision.c、AssetExporter.cs、DeviceProfile.cs、lcd185b.json、rebuild-flash-assets.sh、sdkconfig.defaults —— **两会话改动混在工作区,commit 时需分开归类**。

---

## 七、【停工快照 2026-10-01】用户勒令停工时的完整现场

**停工原因**:185B 调试过程中共享文件被波及(216 板相关文件出现在未提交改动里),用户勒令停工、全部落文档。**停工后不再有任何代码修改/构建/烧录**,本节为交接现场。

### 7.1 工作区未提交改动清单(git status 实测,20 项)

| 文件 | 改动来源 | 内容 | 216 影响 |
|---|---|---|---|
| components/drivers/display_st77916.c | 本会话 | 185B 专用驱动全量(官方 BSP 流程/INVOFF/bswap 刷新/暂停 API) | 无(不参与 216 构建) |
| components/drivers/sd_tf.c/.h | 用户(会话开始前) | 用户自己的改动 | ⚠️ 共享,需用户确认 |
| components/drivers/display_co5300.c/.h | 纸娃娃 agent | suspend/resume 空实现+声明 | **空操作,但属共享文件,216 下次构建前建议过目** |
| main/Kconfig.projbuild | 纸娃娃 agent | MP_ASSET_HEAP_GATE_KB(默认 24,216 等价不变) | 等价零变化 |
| main/app/input_dispatch.c | BOOT 键 agent | BOOT=菜单键(pins.key.menu<0 门控) | 216 零变化(menu=18 走原路径) |
| main/app/state_machine.c | 纸娃娃 agent+另一会话 | 门值 CONFIG 化 + 5 路径 resume | 门默认 24KB 等价 |
| main/net/asset_dl.c | 纸娃娃 agent+另一会话 | 门 CONFIG 化(+修复 8*108 历史笔误) | 等价 |
| main/render/compositor.c | 本会话(已回滚大部)+HEAD 钩子 | 仅剩帧锁/帧源注册钩子 | 数学等价/空操作 |
| main/render/render.h | 本会话 | 帧锁声明 | 仅声明 |
| profiles/amoled216.h/.c + profile_lcd185b.c | 多来源(见 7.2) | ground_cam_shift_px 字段等 | ⚠️ **216 profile 文件被改,需用户/另一会话确认归属** |
| sdkconfig(216 的!) | ⚠️ | 有改动(WiFi RX 等) | **⚠️ 216 构建配置被动过——必须与另一会话对齐后再构建 216** |
| sdkconfig.185b | 本会话 | 185B 专用(PCLK80/门6KB/板型) | 无 |
| Server:AssetExporter.cs/DeviceProfile.cs/lcd185b.json | 本会话(已部分回滚) | 剩余为最小残留 | 服务器侧,不影响固件 |
| docs/ai/hardware-bringup-issues.md、technical-reference.md | 另一会话 | 他们提交前的工作 | — |

### 7.2 ⚠️ 需要用户/另一会话裁决的三件事
1. **sdkconfig(216 构建配置)有未提交改动**——来源待确认(可能是两会话任一),216 下次构建前先核对;
2. **profiles/amoled216.h/.c 的 ground_cam_shift_px 字段**——另一会话方向的改动混入(与本会话无关),归属待确认;
3. **两 agent 的改动已完成但未构建未烧录未验证**——BOOT 菜单键+纸娃娃堆门,代码在树,下次开工先构建验证。

### 7.3 两 agent 成果(代码已落树,**未构建未烧录**)
- **BOOT(GPIO0)=菜单键**(input_dispatch.c,subagent 审查修正 2 bug):POKER 短按=菜单开关/长按 700ms=时钟;DOZE 唤醒沿只唤醒;MENU 内短按下移/长按 800ms 退出。216 零变化。
- **纸娃娃堆门板级化**(Kconfig MP_ASSET_HEAP_GATE_KB,默认 24,185B=6KB)+FONT 门历史笔误修复(8*108→CONFIG)+绑定期间刷新暂停(5 退出路径全配对 resume,泄漏清点见 agent 报告)。216 等价零变化。

### 7.4 下次开工顺序(建议)
1. 与另一会话对齐 7.2 三件事(尤其 sdkconfig/amoled216 profile 归属);
2. 216 构建:确认共享文件编译+行为零变化;
3. 185B 构建+烧录(0x20000):验证 BOOT 菜单键(NVS key0 计数+状态字节)、娃娃绑定(门 6KB 生效、rc=0、渲染帧);
4. 触摸 CST816S@0x15 移植;COLMOD 中间字节强制实验(R 抬升);
5. 探针清理 → 分批 commit(两会话改动分开)→ 强推前过脱敏映射表。

### 7.5 板上现状(未变)
- 185B(21201):停工前最后烧录版 = "480 管线复用"构建(render 480×480+驱动 3/4 取样刷新,vbCYqT 素材=216 同相机)——**不含**两 agent 成果;娃娃间歇出现(24KB 门);BOOT 键=音量减(旧语义);INVOFF/字节交换/整图渲染正常。
- 216(21101):未动(本会话全程只读过一次 MAC)。

---

# 八、【续工 2026-10-01 下午】216 → 185B 功能全量迁移记录

> 触发：用户指令「读文档继续开工 + 把 216 上有 185 上没有的功能全部迁移过来，多 agent 并行」。
> 手段：4 个分析 agent（渲染/UI 输入/音频/电源按键）并行读码 + 主线程外科式移植。
> **结果：编译 0 error（bin 2,244,432 B，3MB 分区余 29%）；⚠️ 未烧录真机**——
> 本 session 的串口被沙箱禁止（`/dev/cu.usbmodem*` open → EPERM），烧录与看图必须由用户执行。

## 8.1 先修的两个「回归」根因（都是未提交的工作区改动造成的）

### R1 上半地形不对/重复图案 ← profile 合成空间被从 480 改成 360
- **证据**：`git show HEAD:profiles/profile_lcd185b.c` = `.width 480/.height 480`；工作区 = 360。
  且分析 agent 用**编译产物字节扫描**复核：objs 里只有 `0x01e0`(480) 应出现的位置出现 `0x0168`(360)。
- **机理**：`RC_SCALE=2` 是全局契约（实体/条带/时钟/娃娃一律 `<<RC_SCALE_SHIFT`=2×），
  而 static/tile 走 `layer_rgb_load` 通用路径 → 240→**360 = 1.5×**；两者不同比例 ⇒
  条带 `band_y = s->y<<1` 与 static 铺法 Y 相位差 ~1/3 = 用户看到的「地形错位/重复图案」。
  同时驱动里 480→360 的 0.75 取样变成死代码（`src_w == SW`）。
- **修**：profile 回 **480×480**。**教训**：`width/height` 是**合成器空间**不是面板尺寸；
  面板适配只允许收敛在显示驱动唯一的缩放点（pitfalls §一/§五 的定稿）。

### R2 内部堆只有 ~120KB / 最大块 3KB ← LVGL 64KB 静态池回归
- **证据**：`build/minipet.map` 有 `lv_mem_core_builtin.c.obj @0x10000`（216 的 map 无此 obj）；
  `.dram0.bss` 185B=197,440 vs 216=130,168（差 67,272，其中 65,536 = 这一个池）；
  `_heap_start` 185B=0x3fccedc0 vs 216=0x3fcbe3b8（差 66.5KB）；真机日志同点
  216 `空闲=186,759` vs 185B `空闲=120,715`。
- **根因**：`sdkconfig.defaults` 里一行**转义写坏**的注释把 `CONFIG_LV_USE_CLIB_MALLOC=y`
  吞进了注释（`...启动崩溃\ nCONFIG_LV_USE_CLIB_MALLOC=y`），且
  `# CONFIG_LV_USE_BUILTIN_MALLOC is not set` 这种写法对 kconfig **无效**（"is not set" 只在
  已启用项上生效）⇒ active sdkconfig 落成 `BUILTIN=y + LV_MEM_SIZE=65536`。
- **修**：`sdkconfig` + `sdkconfig.defaults` 显式三连（`STDLIB_MALLOC=1 / CLIB_MALLOC=y /
  BUILTIN_MALLOC=n` + `LV_MEM_SIZE=0`）。**实测收益**：`_heap_start` 0x3fccedc0 → **0x3fcbfc80（+61.9KB）**，
  与 216 只差 6.3KB（本板自有 st77916 驱动 + st77916 init 表等 BSS）。

## 8.2 迁移清单（文件级）

| 文件 | 来源 | 适配点 |
|---|---|---|
| `main/render/mpak.{c,h}` | 216 整文件 | 无（纯格式层） |
| `main/render/compositor.{c,h}` | 216 整文件 | 无（`g_sw/g_sh` 全走 profile；`display_set_frame_locks/frame_source` 两板同名） |
| `main/render/render.h` | 216 主体 | include 回 `lcd185b.h`，板名注释 |
| `main/net/asset_dl.{c,h}` | 216 整文件 | 无 |
| `main/net/poller.c` | 216 整文件 | 无 |
| `main/app/state_machine.c` | 216 整文件 | 无 |
| `main/render/lvgl_bridge.c` | 216 整文件 | 无（引用 compositor.h/asset_dl/state_machine 的新符号，均已就位） |
| `main/app/input_dispatch.c` | 216 主体 + **触摸/按键按板改写** | ①帧格式 CST816S（d0 触点数 / d3 XH / d4 XY / d5 YL，**无 0xAB 校验字节**）②`i2c_bus_find("cst816")` ③重组后 ×4/3 上采样（面板 360 → UI 480）④`key_tick()`（GPIO18）整段删除，BOOT 一键合并语义 ⑤`touch_cst816_init` 仍在 input 任务内（本板历史口径） |
| `main/audio/bgm.{c,h}` | 216 整文件 | 无（栈 8192 非 24KB——旧注释过期） |
| `components/drivers/codec_es8311.{c,h}` | 216 整文件 | include 改 `lcd185b.h`（引脚全走 profile.pins.audio） |
| `components/drivers/pmu_bq27220.{c,h}` | **新写** | 本板电量计 BQ27220@0x55（216 是 AXP2101@0x34）：只读标准命令 SOC 0x2C/电压 0x08/电流 0x0C/状态 0x0A/温度 0x06；无 PWRON 键、无 IRQ、无 VBUS 检测位（如实降级） |
| `components/drivers/CMakeLists.txt`、`include/drivers.h` | 手工 | 加入 codec/pmu，更新初始化顺序注释 |
| `main/app/hal_contract.h` | 手工 | `mp_touch_read`（CST816S）、`mp_codec_init/start/set_rate/write`（真实实现）、`mp_pmu_*`（BQ27220） |
| `main/main.c` | 手工 | `pmu_bq27220_init()`（log-and-continue）、`mp_orient_calib_tap` 补桩（8 组合标定表，默认关）、tprobe BGM 取证计数扩展、`MP_TASK_PROBE=1` |
| `main/render/font_cn/{menu_font_cn.c,regen.sh,extra_chars.txt}` | 216 工具 + 本地重烘 | 220 字 → **2181 字**（源码用字 ∪ WZ 地图名用字 2125）；regen.sh 路径注释改本板 |
| `profiles/profile_lcd185b.c` | 手工 | 480×480 + has_audio/has_touch/has_pmu = true |
| `sdkconfig` / `sdkconfig.defaults` | 手工 | R2 修复（LVGL 三连 + `LV_MEM_SIZE=0`） |
| `Server/seed/profiles/lcd185b.json` | 手工 | audio/touch=true，shape 改 round（服务端导出 profile；运行时实际以 hello 上报为准） |

## 8.3 顺手修掉的真·颜色缺陷（**216 同样存在**）

`compositor.c` 里 11 个颜色常量按 **24bit RGB**（`0xRRGGBB`）写，却直接赋给 `uint16_t`
帧缓冲 → 编译期按值截断成 `0xRRGG` 的**错色**（构建期一直有 `-Woverflow`，本轮清零）：

| 原值 | 截断成 | 正确 RGB565 | 位置 |
|---|---|---|---|
| 0x101018 | 0x1018（偏紫黑） | **0x1083** | BGM 控制条面板底 |
| 0xE6E6F0 | 0xE6F0（脏黄） | **0xE73E** | 控制条标题 |
| 0x9AA0B4 | 0xA0B4 | **0x9D16** | 控制条信息行 |
| 0x3C4658 | 0x4658 | **0x3A2B** | 控制条分隔线 |
| 0x1E2433 | 0x2433 | **0x1926** | 控制条按钮底 |
| 0x2E7D6B | 0x7D6B | **0x2BED** | 控制条选中项 |
| 0xD8DEE9 | 0xDEE9 | **0xDCFD** | 控制条按钮字 |
| 0x6E7686 | 0x7686 | **0x6BB0** | 控制条提示字 |
| 0x303030 | 0x3030（青绿） | **0x3186** | 气泡边框 |
| 0xF7F7F2 | 0xF7F2（浅黄） | **0xF7BE** | 气泡底 |
| 0x101010 | 0x1010（偏蓝） | **0x1082** | 气泡文字 |

## 8.4 待真机验收清单（逐条可 grep，烧录后照单核对）

| # | 项 | 期望证据 |
|---|---|---|
| V1 | 合成空间 480 | `rc: render_init ok 480x480` |
| V2 | LVGL 池修复 | `@render_init 后 空闲=` 从 ~52,215 涨到 **~118,000** |
| V3 | 触摸在线 | `cst816: CST816S 就绪 INT=4 RST=1`；点屏 `触摸按下 raw(..)->屏幕(..)` + `四向判定: 按角→落点象限【…】` |
| V4 | 触摸坐标 | 面板中心 → `map=(240,240)`；四角 → (0,0)/(479,0)/(0,479)/(479,479) |
| V5 | **上半地形正确** | 绿山丘+橙棕悬崖+蓝天白云，无重复图案/绿虚线（**关键回归点**） |
| V6 | 颜色 | 气泡/控制条不再发紫发黄 |
| V7 | 电量计 | `bq27220: BQ27220 就绪 SOC=?% 电压=?mV 电流=?mA 状态=0x????`（SOC 0-100、电压 3000-4300mV） |
| V8 | 电量上报 | Web 设备详情「电量 N%」 |
| V9 | 音频 | `codec 初始化完成（ES8311 + I2S TX 就绪，PA 默认关）` + 真机出声 |
| V10 | BGM 取证 | `tprobe … bgm 音频：断供=… I2S 写最长=…`（MP_TASK_PROBE=1 已开） |
| V11 | 中文地图名 | 菜单地图列表显示中文原名（2181 字子集兜住） |
| V12 | 地图功能子页 | 点列表条目 → ①设为背景 ②修改当前地图的摄像头 ③删除此地图 ④返回地图列表 |
| V13 | 整图相机 | `mpak: bgmap 000010000 vw=2270 vh=1807 full_map=1` + `rc: 相机支持=1 范围 dx[…] dy[…]`；拖动跟手；`sm: 地图 … 应用相机 (x,y)` |
| V14 | 旧包零回归 | 窗口包（240 口径）视觉与停工前一致；相机入口提示"此图不支持相机" |

### 8.4.1 整图包前置条件（必读）
- 服务端整图导出**默认关**，只有 push 带 `fullMap=true`（或 CLI `--full-map`）才出；000010000 = **16.9MB**。
- 本板 **assets 分区只有 6MB**（`partitions.csv`）→ 整图包**只能落 TF**（`/sdcard/minipet/bg/<hash>.mpk`）。
- 窗口包路径逐字节不变（`full_map=0`）。

## 8.5 遗留 / 需用户决策

1. **真机验收未做**（串口沙箱禁止）→ §8.4 全表待用户执行；烧录前**先读 MAC 确认端口**。
2. **216 侧同步**：§8.3 颜色截断 + `sdkconfig.defaults` 转义写坏那行，216 同样存在；
   本 session **未碰 216 任何文件**（`git status` 里 216 的改动都是此前另一 session 的）。
3. **GPIO18 / PWRON 键**：1.85B 硬件不存在（官方 BSP 按键表只有 GPIO0），不做。
4. **BQ27220 寄存器口径**：来自 espressif 组件 v0.1.2 源码 + TI 标准命令表，**未真机实测**
   （首次上电看 V7 的 SOC/电压合理性；`vbus` 是 charging 近似，无 VBUS 检测位）。
5. **音频真机出声未测**；`has_audio=true` 后 hello 上报 `"audio":true`（服务端不消费该字段，
   不影响导出）。
6. **TF 卡**：整图包与音频曲目表都依赖 TF 在位。
7. **`printf` 级取证探针**：树里仍有 ~25 处 `[取证]` 日志（两板同源既有），定稿后清理。
