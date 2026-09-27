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

## 7. 仍需人眼确认的一点

面板侧没有 TE（撕裂同步）引脚（BSP 里 `BSP_LCD_*` 无 TE、`tear_avoid_mode = NONE`），
所以"写入 GRAM 时面板正在扫描"这件事在硬件上无法消除；本次修复把**内容错帧/残影/越界裁切**
这四类软件根因全部消除后，剩余可能的观感差异只可能是这种瞬时撕裂（照片抓拍才会看到）。
需要用户拍一张**刚开机、未触摸**的照片做最终确认。
