# 渲染「分割线 / 头部缺一块」取证与修复报告（2026-09-27）

> 本文记录用户报障「是有一条明显的分割线 / 头明显锁了一块 / 人物抖动」这一串**显示类**
> 问题的完整取证链、根因与修复。方法上刻意避开"猜算法"，全部用**可对拍的事实**收敛。

## 0. 结论速览

| # | 现象 | 根因 | 状态 |
|---|------|------|------|
| 1 | 屏幕上一块内容错帧 / 上下两条不同时刻的画面（"分割线"） | **合成器跨任务竞态**：`compositor.c` 全静态状态（`g_fb`/`g_ent_px`/`g_mark`/`s_blit_stage`）无锁，而 `render_set_map/clock/exit_menu/force_redraw/parts/layout/expression` 会被**输入任务、状态机任务、asset_dl 任务**调用（`render.h` 的"必须与 render_tick 同任务"约定在应用层被违反）→ 渲染任务的 `compose_region + blit_be` 被从中间截断；`pc_flush()` 还会在渲染任务读位图时释放 PSRAM | ✅ 已修（递归互斥，见 §3） |
| 2 | 人物被屏幕边缘"切出一个直角块"（头缺一块） | **拖拽偏移越界且松手后保留**：真机取证 `drag=(61,172)`、`落点=(301,412 179x68)` —— 214×168 的显示矩形只有 179×68 在屏内 | ✅ 已修（整只夹在屏内，四边仍可到达，见 §4） |
| 3 | 永远纯黑背景（"没有 TF 卡就渲染默认地图 000010000"未落地） | `asset_dl_map_path()` 只比对**内容 hash**，而 SET_MAP 下发的是**地图 id** → 按 id 查询恒失败且静默返回 | ✅ 已修（两种键都认 + 失败记 ERROR + 清单就绪后重投，见 §5） |
| 4 | 服务器页面没有点歌入口 | 曲库表只有展示列；固件也没有"点播指定曲目"指令 | ✅ 已补（Web 行内播放 + 服务端 trackId 折算 + 固件 `v="track"`，见 §6） |

## 1. 取证方法（可复现）

1. **主机端离线解码**：`/tmp/mpkview.py` 直接解 PARTS/LAYOUT 的 wire 格式，
   按固件同一算法（1bit 掩码 tight bitpack、2× nearest、帧内列表序、动作级联合画布）
   合成 PNG；据此确认**素材本身没问题**（stand1 三帧、fly 两帧均与参考图一致）。
2. **设备端逐件对拍**：`compositor.c` 里 `RC_FORENSIC` 探针打印
   `PARTS/LAYOUT content_hash`、每件 `lay_id/expr/x/y/w/h/alpha/maskbits/px_hash/mask_hash`
   与实体缓冲 ASCII 图；主机端对同一 mpk 算同名字段 → 全部一致。
   ```
   取证#1：PARTS hash=d2dac1ab278dea8b LAYOUT hash=69cb6a45b94f1161 画布=(0,0 107x84) 落点=(240,240 214x168)
   取证件[13] lay_id=34 expr=255 xy=(39,3) flip=0 z=1 | img_id=34 50x36 alpha=1 maskbits=1155 ...
   ```
   → **g_ent_px 与主机端逐件一致**（发顶那一件也在，说明"头缺一块"不是素材/解码问题）。
3. **上屏字节对拍**：`blit_be()` 对每个分块先算 FNV 再 `display_blit`，主机端按同一
   合成结果算分块指纹；`/tmp/check_flush_consistency.py` 自动对拍：
   ```
   总计 flush=79  单帧全等=79  混合/不符=0  blit 非 0 返回=0
   ```
   → 交给面板的字节**逐块全等**，`display_blit` 全部返回 0。
4. **残留自检**：无地图时屏幕底色应为纯黑，`flush_dirty()` 每秒扫一次"实体矩形之外"
   的非黑像素（横幅/控制条/气泡除外）并打 WARN。真机 230 笔 flush 运行期间**零告警**
   → 脏区机制没有留下残影。

> 结论：`g_ent_px` 正确 + 上屏字节正确 + 无残留 ⇒ 照片里的"错帧块/分割线"只剩
> **合成过程被并发打断**这一条路径（§0 第 1 项），以及**人物被拖出屏外裁切**（第 2 项）。

## 2. 为什么"两个任务同时画"会画出照片里那种东西

* `full_recompose()`（整屏 compose + 20~40 笔窗口写，真机实测约 10ms）与
  `flush_dirty()`（增量 bbox）可以交错执行 → 同一屏上出现**两个不同时刻**的内容；
* `recompose_entity()` 会被 `render_set_parts/layout/expression` 从别的任务调用，
  它 `memset + 逐件 blit` 写 `g_ent_px`/`g_ent_cov`，正好在渲染任务 `ent_compose()`
  读它的时候被截断 → 半张脸、某件错帧（"头缺一块"）；
* `render_set_parts()` 的 `pc_flush()` 会**释放全部部件位图 PSRAM**，渲染任务此刻
  正在 `blit_ent_2x()` 读同一块内存 → 读到被复用的垃圾（画面上就是漂移的横条/色块）。

## 3. 修复：合成器递归互斥

`Firmware/main/render/compositor.c`

* 新增 `s_rlock`（`xSemaphoreCreateRecursiveMutex`，`render_init` 创建）+ `rc_lock/rc_unlock`；
* 原语级加锁：`flush_dirty()`（标脏清零→compose→分块 blit 整段原子）、
  `full_recompose()`、`recompose_entity()`、`mark_rect()`（跨任务标脏入口）、
  MENU 路径的整屏 `memcpy + blit_be`；
* 公共写 API 全部改为 `*_nolock` 实现 + 加锁包装：
  `render_set_parts / render_set_layout / render_set_expression / render_set_map /
   render_set_clock / render_force_redraw / render_enter_menu / render_exit_menu`；
* 递归锁 → 同任务嵌套（`flush_dirty` 里再调 `mark_rect` 等）安全，不会自锁；
  跨任务调用最坏只等一笔 flush（≈10ms），远小于各任务超时预算。

回归判据：`check_flush_consistency.py` 要求**每一笔 flush 的全部分块来自同一帧**且与主机端逐块全等。

## 4. 修复：拖拽范围 = 整只宠物留在屏内

`render_set_drag_off()/render_set_drag_off_y()` 旧口径 ±屏宽/±屏高，允许把人物拖到
几乎完全出屏并**原地保留**（用户照片里"头被切一块"的直接来源）。现改为
`drag_clamp()`：按当前画布尺寸 ×2 与 tilt 视差基准夹取，保证显示矩形四边都在屏内。

* 107×84 的 stand1：横向可移动 266px、纵向 312px（屏幕四边都能贴到）→ "可以全屏拖动"成立；
* 换动作/换装改变画布尺寸、以及 IMU 倾斜（±8px）时都会重新夹取；
* 启动默认 `drag=(0,0)` → **`origin` 恒在屏幕中点**（探针：`落点=(240,240 214x168)`）。

## 5. 修复：无 TF 卡时默认地图真的被渲染

* `asset_dl_map_path()` 现在同时接受**内容 hash** 与**地图 id**；
* `dispatch_map()` 查询失败不再静默：记 ERROR + 请求素材同步；
* `dispatch_manifest_synced()` 末尾：出厂素材模式且尚未装载地图时**重投**
  `SET_MAP 000010000`（启动早期 1.3s 的首次投递必然早于 ~5s 的清单解析）。

## 6. 补齐：Web 点歌（用户："页面 bgm 没有选择歌曲的地方合理吗"）

* **Web**（`Web/src/views/MusicView.vue`、`Web/src/api/client.js`）：曲库表新增行内
  「▶ 播放」列 + `playTrack()`，body `{type:"bgm",value:"track",trackId,trackTitle,source}`；
  设备卡新增「当前曲目」标签（读 `GET /admin/devices/{id}` 的 `bgm.trackId/trackTitle`）。
* **服务端**（`AdminEndpoints.cs`）：`bgm` + `value=track` 分支：按 `TrackIdForKey(key)`
  （XxHash32，与设备曲目表同口径）折算 u32，走旧口径 `{t:"bgm",v:"track",n:<u32>}`
  入队；曲目所属音源与设备当前音源不同时先补发 `source` 指令；曲目落
  `DeviceBgmPrefs.TrackId/TrackTitle`。
  `CommandQueue.LegacyCommand.N` 由 `int?` 放宽为 `long?`（u32 曲目 id 近半数 ≥2^31）。
* **固件**（`net/poller.c`）：两条解析路径都认 `v="track"` → `MP_AUDIO_PLAY`
  （`a` = 曲目 id，`bgm.c` 原本就按 u32 位型处理）；顺带把 `n` 的读取从
  `(int32_t)double` 改成**先在 double 域判范围再折算**，避免 u32 越界时的未定义行为。

## 7. 修复过程中连带挖出的三处「静默失败」（都在同一类：读错偏移/资源上限）

| # | 现象 | 根因 | 真机证据（修前 → 修后） |
|---|------|------|------------------------|
| 1 | 三档字体从未绑定 → 气泡/菜单/曲库文本全退化 | `read_font_px()` 读**文件偏移 32**（信封里 payload_len 的低字节），而 payload 首字节在 **40**；且服务端清单 FONT 条目**不带 px** → `font_px` 恒为垃圾 | 修前日志无 `set_font` 行；修后 `set_font px=16/24/32 开始/完成` 六行齐 |
| 2 | `font_px` 一旦修好反而整机 FATAL：`vfs_fat: open: no free file descriptors` → `mpak: open /sdcard/minipet/parts/… failed` → `本地素材加载失败` → 人物整只消失 + 渲染任务 TWDT 每 5s 连发 | `SD_MAX_FILES = 4`（早期"manifest+1 包并发"假设，只对下载成立）；实际常驻打开 = 时钟 PARTS(1)+三档 FONT(3)+纸娃娃 PARTS(1)+LAYOUT(≤2)+瞬时(1~2) | 修前 TWDT 触发 37 次、`miss=15`；`max_files 4→12` 后 TWDT **0 次**、`miss=0`、内部堆 @素材全绑后 空闲=18107/最大块=8180 |
| 3 | `cJSON_GetNumberValue(NULL)` 返回 **NAN**，`(uint8_t)NAN == 255` → 坏值被 `save_local_manifest()` 持久化回清单，只判 `==0` 的兜底救不回来 | 缺字段没先判 `cJSON_IsNumber` | 统一 `jnum()/jnum_at()`；FONT 档位**以包内 size_px 为准**（清单值只作参考） |

> 顺带把常态日志收敛：`RC_FORENSIC` 默认 0（需要时改 1 复跑取证），周期探针 3s → 30s，
> flush 日志仅在取证窗口打印。最终固件 60s 只输出 **307 行**日志、**0 次 TWDT**、0 条 ERROR。

## 8. 追加：把「Web 指令全部不生效 / BGM 永远没声」也挖到底了（同日深夜）

用户报障「服务器页面也没有播放 bgm 的按钮（已补）」「时钟好像也不能用」，
在真机取证中发现一条**更严重的链路断裂**：Web 下发的任何指令其实**从未送达设备**。
四个独立根因，逐个实测确认：

| # | 根因 | 真机证据 | 修复 |
|---|------|---------|------|
| 1 | **poller 卡在"WiFi 重连"这一跳，永不轮询** | `ping <PET_IP>` 4/4 通（5~25ms），但串口阶段计数 `poller 阶段=[7 7 7 0 …]`：**7 轮循环全部卡在 WiFi 连接门**，`g_pol_stage[3]` 恒 0 → 永远进不到 poll；服务端只看到开机那次 hello，之后 Web 指令全躺在队列里 | ① `provision_wifi_connect_sta()` 补"实测已连"判据（驱动已关联 + netif 有 IP 即复位 `s_sta_connected`；GOT_IP 事件在 netif 保留 IP 时**不会**再触发，这是旗标永久为假的机理）；② poller 在该门失败时**再用裸 TCP 探针实测**，探得通就照常 poll（链路真断才退避） |
| 2 | **曲库表恒空（0 首）** | `bgm: audio pack 8d618b5d337f2818.mpk unusable, skipped` → `曲目表构建：src=0 命中 0 首` | `MPAK_AUDIO_MAX_TRACKS` 1024 < 实际 **1167 首** → `parse_audio` 直接 `MPAK_ERR_FMT`。放宽到 4096（表在 PSRAM）→ 真机 `命中 1167 首`、`track table: 1167 tracks` |
| 3 | **BGM 任务起播即崩（重启循环）** | 6144 栈：`***ERROR*** A stack overflow in task bgm` → 重启；10240 栈：`Guru Meditation (LoadProhibited, EXCVADDR=0)`，`addr2line` 反解 = `bgm_task→handle_audio_msg→bgm_play_session→play_track→stream_chunk→decode_pending→mp3dec_decode_frame→L3_huffman` | 根因写在本仓库 `minimp3.h` 文件头：`mp3dec_scratch_t ≈16KB 在调用栈上 → 任务栈须 ≥20KB`。改回 **24576 内部栈**并把**建栈提前到 `mp_codec_init`/渲染任务之前**（此刻最大连续块 ≈34KB，一次成功）。PSRAM 栈试过不行：本任务读 Flash，PSRAM 栈在关 cache 临界区触发 `assert esp_task_stack_is_sane_cache_disabled()` → 1.5s 重启 |
| 4 | **`play` 永远不选曲** | 服务端事件只有 `BGM：play（设备现场控制）`，设备侧 `bgm 步骤` 恒 =1（连 track table 都没打） | 固件 PLAY 分支 `if (id==0) { bgm_cmd("play",0,&id); if (id==0) return; }`，而服务端 `HandleBgmCmd` 对 `play` **不选曲**（只有 next/prev 调 `NextTrackAsync`）→ 回 `id:null` → 静默返回。服务端已改为 play/resume 未指定曲目时 `NextTrackAsync(source, 当前曲目, 0)`；固件该分支补 WARN 日志（不再静默） |

**修复后的端到端实测**（旧服务端 + 新固件，用 next 绕开服务端选曲）：

```
曲目表构建：src=0 命中 1167 首（audio 目录存在）
track table: 1167 tracks (source=wz)
（无 stack overflow / 无 panic）
feeder 计数 20s 内 +2161（=108 次/s I2S 写入 = 正在出声）
→ 下发 bgm=pause 后 feeder 回落到 20 次/s（空闲节流）
服务端事件：指令下发：bgm=source/play/next → BGM：play（设备现场控制）
```

> ⚠️ 服务端侧改动（play 选曲 / `bgm=track` 点播 / Web 曲库行内播放 + 当前曲目）
> **需要重建并重新部署 NAS 镜像**才生效；固件侧改动已烧进设备。

## 9. 仍需人眼确认的一点

面板侧没有 TE（撕裂同步）引脚（BSP 里 `BSP_LCD_*` 无 TE、`tear_avoid_mode = NONE`），
所以"写入 GRAM 时面板正在扫描"这件事在硬件上无法消除；本次修复把**内容错帧/残影/越界裁切**
这四类软件根因全部消除后，剩余可能的观感差异只可能是这种瞬时撕裂（照片抓拍才会看到）。
需要用户拍一张**刚开机、未触摸**的照片做最终确认。
