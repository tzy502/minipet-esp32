# 1.85B 显示 bring-up —— 坑与教训实录(2026-10-01)

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
