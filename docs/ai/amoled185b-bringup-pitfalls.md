# 1.85B 显示 bring-up —— 停工归档（2026-10-01 额度耗尽）

> **当前目标：185B(360×360 圆屏 ST77916) 正确渲染 000010000 地图场景 + 纸娃娃**
> 验收标准：娃娃站地面线上、背景完整、颜色正确、触摸可操作（菜单+宠物）
> **当前状态：下半部分正确，上半部分地形不对，颜色疑似仍有偏差**

---

## 一、当前已验证可用（不要动）

| 层 | 状态 | 证据 |
|---|---|---|
| 硬件初始化（RDDID→v2 序列→INVOFF→SLPOUT→DISPON） | ✅ | RDDID=00 02 7F 7F, INVOFF 扫频实证 |
| 面板 1:1 通路（480 合成器→0.75 驱动采样→360 面板） | ✅ | 三色自测通过、娃娃下半正确 |
| 地图装载 BGMAP rc=0 | ✅ | 多轮一致 |
| 素材绑定 rc=0 | ✅ | 素材全绑后 空闲 11703 |
| 刷新流 480→360 0.75x 采样 | ✅ | 450 帧 0 失败 |
| 横幅黑底白字 | ✅ | bswap 修正后 |
| BOOT(GPIO0)=菜单键 + 长按时钟 | ✅ | 代码已落（subagent 审查修正 2 bug） |
| 216 板 | ✅ 未碰 | 只读 MAC+烧素材确认污染后已回滚 |

## 二、核心未解问题

### 2.1 上半部分地形不对（当前卡点）
- **现象**：下半部分（草地/小路/娃娃/花）渲染正确；上半部分（山丘/远景/天）显示为不正确的重复图案/绿色虚线
- **参考正确画面**：用户 photo——上半=绿色山丘+橙棕悬崖+蓝天+白云
- **已排除**：面板通路（三色自测通过）、地图装载（rc=0）、缩放公式（数学验证正确）
- **最可能原因**：**条带层（scroll strips）在整图视口下坐标/内容不对**。条带是"滚动树丛/远景层"画在特定 Y 范围——FitWholeMap 导出后条带 Y 坐标计算公式与 185B 驱动的绘制比例不匹配。
- **我的错误**：把条带层从硬编码 2x 改成 Q16 比例泛化时，公式推导有误（步进/周期计算在 1.5x 下不正确），且没有先在 480 素材上验证就上了。

### 2.2 颜色 R 通道微抬升
- COLMOD 读回 00 58（非标准 0x55），强制 0x55 无效（低字节写不落地）
- 可疑：ST77916 的像素格式可能不是标准 RGB565（可能需要不同的 COLMOD 设置或字节序）
- 下一步：读帧数据逐字节与导出文件比对（偏差模式可定位是位偏移还是字节序）

### 2.3 纸娃娃被 24KB 门拦截
- 185B 绑定时堆空闲 ~9-11KB，24KB 门永久拦截
- 两个 agent 的代码**已落工作区但未构建烧录**（堆门 Kconfig 化 + FONT 笔误修复 + 刷新暂停）
- **需用户与另一会话协调**：门值改板级自适应（另一会话不允许我改他们的 24KB）

### 2.4 触摸 CST816S 未移植
- 驱动文件已写好（board185b/components/drivers/touch_cst816.c/h），未接线（main.c 调 init）
- 坐标方向待真机校准

## 三、已验证可用的代码基线（下次从这里继续）

**路径**：`Firmware/board185b/`（独立构建树）
**构建**：`cd Firmware/board185b && idf.py build`
**烧录**：`cd build && esptool --port /dev/cu.usbmodem21101 write_flash @flash_args`
**素材**：`/tmp/minipet-assets.vbCYqT/assets.bin` → 0x620000（240 裁剪相机视角）

**当前固件状态（板上运行的版本）**：
- profiles/profile_lcd185b.c：width=480, height=480（合成器空间=480 与 216 同管线）
- components/drivers/display_st77916.c：
  - 官方 BSP 流程 + INVOFF 补偿 + v2 序列
  - refresh_task：480→360 (0.75x) 取样 + 帧锁（ABBA 死锁修复版）
  - **无 bswap**（撤了——上一版 bswap 用于修负片，但最终 INVOFF 后不需要了，撤了以匹配 CO5300 路径）
  - blit/fill 在刷新运行时跳过（唯一显示通路=刷新任务）
- 地面表：480 坐标 × 0.75 缩放（ksc_q16 方式）+ 屏底-20 夹取
- 门：24KB（另一会话原值，被 185B 恢复了）

**⚠️ 注意：板上固件和工作区代码有版本差**——工作区有两 agent 的改动（BOOT 菜单键+堆门 CONFIG 化+暂停 API），板上是停工前最后一版。下次构建前先 review 这些改动。

## 四、已排除的死路（不要重试）

| 方案 | 为什么不行 |
|---|---|
| FitWholeMap 整图缩放（zoom=0.1586） | 整图缩成 360 后每世界像素只剩 0.16 屏幕像素→内容不可辨认 |
| 180 档素材（2x 密度统一） | 240 裁剪不可用+LAYOUT 语义不匹配→娃娃缩成碎片 |
| tx_param 手动路径写色 | 在本板不显示（队列路径才能显示）|
| INVOFF（反转补偿） | 屏幕全黑 |
| 刷新流 bswap | INVOFF 后不需要，撤了以匹配 blit 路径 |
| 低字节写 MADCTL/COLMOD | 写不落地（中间字节才是正确写法，但当前不需要）|
| 480 表 y480×0.0793+210 | 基于错误相机推导的变换，地面线位置不对 |

## 五、正确渲染的关键参数（如果重导素材）

**如果换回 480 相机裁剪素材（vbCYqT）**：
- 合成器 480×480（RC_SCALE=2）
- 静态层 240×240→480（2x 展开，layer_rgb_load 原有逻辑）
- 驱动 480→360（0.75x 最近邻）
- 地面表 480 坐标×0.75（ksc_q16=360<<16/480）+ 屏底-20
- 条带 y = 世界 y × 0.75

**如果用 360 原生素材**（需要导出器正确渲染上半部分）：
- 合成器 360×360
- 静态层 360×360 直接 1:1
- 驱动 1:1
- 地面表坐标=360 视口坐标（无变换）
- 需先修条带 Y 坐标/内容在 FitWholeMap 导出下的问题

## 六、工具/环境备忘

- **串口**：185B=21101, 216=21201（**端口会互换！每次烧录前必须读 MAC 确认**）
- **构建 185B**：`cd Firmware/board185b && idf.py build`（项目标准 build 目录）
- **构建 216**：`cd Firmware/board216 && idf.py build`（另一会话的工作树，**不要动**）
- **素材烧录**：`esptool write_flash 0x620000 assets.bin`（仅 assets）或 `write_flash @flash_args`（全量）
- **抓日志**：`/Users/a502/mp-work/capture_boot.py PORT SECONDS OUTFILE`
- **gdb**：openocd -f board/esp32s3-builtin.cfg + xtensa-esp32s3-elf-gdb（PYTHONHOME 必须 set）
- **另一会话正在工作**：board216/ 目录、sdkconfig(216) 是他们的——不要动

## 七、两个 subagent 的成果（在树里，未构建）

### 7.1 BOOT=菜单键（input_dispatch.c）
- POKER 短按=菜单开关、长按≥700ms=时钟（s_k0_menu_latch）
- 修正 2 bug：长按双触发（按下沿→释放沿触发）、DOZE 唤醒沿误开菜单（st_before 快照）
- 216 零影响（menu≥0 门控）
- **待验证**：需构建烧录后按 BOOT 测试

### 7.2 堆门 CONFIG 化+刷新暂停
- MP_ASSET_HEAP_GATE_KB（默认 24）Kconfig + state_machine.c/asset_dl.c 引用
- 185B sdkconfig.185b 设 6KB
- display_refresh_suspend/resume（st77916 实现，co5300 空操作）
- state_machine dispatch_manifest_synced 5 退出路径全配对 resume
- **待验证**：需构建烧录后确认绑定成功+娃娃渲染

## 八、涉及的两会话协调事项

1. **堆门阈值**：另一会话的 24KB 保护是为 216 设计的，185B 需要 6KB——Kconfig 化方案已实现但未验证
2. **共享文件**：compositor.c 的 frame_lock/frame_source 钩子、provision.c 的 AP 幂等防御、mpak.c 的 tile 公式——这些是 185B 需要的修复，需要另一会话确认对 216 无副作用
3. **素材统一**：两板目前用不同素材版本（216=旧版，185B=新版），需要决定是否统一

---

# 九、【续工 2026-10-01 下午】见随板详细记录

本轮（用户指令：读坑档继续开工 + 把 216 有而 185 没有的功能全量迁移，多 agent 并行）
的完整记录写在**随板副本**：
`Firmware/board185b/docs/amoled185b-bringup-pitfalls.md` §八。

**摘要**：
1. **修了两个回归根因**：① `profile_lcd185b.c` 合成空间被改成 360（应为 480）→ static 1.5× 与
   条带/实体 2× 层间比例不一致 = 本文档 §2.1「上半地形不对/重复图案」的机制；
   ② active `sdkconfig` 的 LVGL 内置 64KB 池回归（`sdkconfig.defaults` 里一行注释被 `\ n` 转义吞掉
   + `# ... is not set` 写法对 kconfig 无效）→ `_heap_start` 前移 61.9KB。
2. **216 → 185B 全量迁移**：mpak R2 整图（64MB/8192/跳 CRC/分块读）、compositor 整图相机
   + 窗口缓存 + 地面表、asset_dl（per-map 键/隐藏标识/中文 label）、poller 解除隐藏、
   state_machine（相机 NVS/字体堆门控/16 条带槽）、lvgl_bridge（地图功能子页/相机调参态）、
   input_dispatch（触摸手势链 + **CST816S 帧格式与 ×4/3 上采样**）、音频（ES8311 + BGM 全链，
   `has_audio=true`）、**新写 BQ27220 电量计驱动**（216 是 AXP2101）、字体子集 220→2181 字。
3. **顺手修掉 11 处颜色常量截断**（24bit RGB 赋给 uint16 帧缓冲 → 错色；216 同样存在）。
4. **编译 0 error**；bin 2,244,432 B（3MB 分区余 29%）。
5. **⚠️ 未烧录**：本 session 串口被沙箱禁止（`/dev/cu.usbmodem*` EPERM）→ §八.4 的
   V1–V14 验收表需用户/另一 session 执行；整图包（16.9MB）必须走 TF（assets 分区仅 6MB）。
