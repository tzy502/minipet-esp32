# 【通知】板 B 必做：出厂快照重烧（2026-09-29）

> 给正在调试板 B（/dev/cu.usbmodem21201）的 session：这块板的**内部 Flash assets 出厂分区是空壳**
> （manifest.json 仅 30 字节 `{"proto":1,"rev":0,"files":[]}`，无素材文件清单），
> 任何"TF 缺失/TF 素材不全 → 回落出厂分区"的路径都会拿到 0 个文件 → FATAL 黑屏文字。
> 板 A（21101）已踩完全程并修复，照做即可。

## 必做操作（烧进你那块板，注意端口是 21201）

```bash
cd /Users/a502/IdeaProjects/minipet-esp32
ESPPORT=/dev/cu.usbmodem21201 \
FACTORY_MAPS="000010000,100000000" \
Server/tools/rebuild-flash-assets.sh \
  --server http://192.168.3.46:38090 --device <你那块板的deviceId>
```

- `--device` 用板 B 自己的 deviceId（别用 dev-693ea4，那是板 A 的）
- `FACTORY_MAPS` 带哪些地图按需求定；6MB 分区约装：纸娃娃全套 + 布局 + 字体 + **2 张地图**（背景+条带），再多会打包失败
- 脚本会导出素材 → 生成符合固件 schema 的 manifest.json（**固件解析的 files 表就来自它**）→ wl_fatfsgen 打镜像 → esptool 写 0x620000

## 为什么必须做（板 A 血泪史，30 秒版本）

1. 出厂分区 manifest 是空壳 → 固件降级/无 TF 时查不到任何 PARTS → `ASSET LOAD FAILED` 文字屏
2. 固件侧降级链已就绪（dispatch 失败 → 自动切出厂分区重试；喂狗修复；同步失败不误通知渲染）
3. 分区数据补上后：TF 空卡/拉取失败 → 自动渲染出厂神子 + 横幅提示，**不再黑屏**

## 双板规矩提醒（technical-reference.md §2.4B）

- 端口名重插会互换：flash 前静听串口核对身份，禁止裸 `idf.py flash`
- **build 目录共享，构建必须串行**；长期解法各自 `-B build-a/build-b`
- 串口用完即关

## 固件侧新增的降级行为（两边固件都应包含）

- `sd_tf_switch_to_factory()`：TF 卸载→工厂分区接管 /sdcard（幂等）
- `asset_dl_reload_local()`：切换后重建内存文件表
- `dispatch_manifest_synced` 资产加载失败 → 自动走上述降级重试一次，仍失败才 FATAL
- sync_once：下载失败重试 ×3；纸娃娃 PARTS/stand1 缺失**不通知渲染**（防 dispatch 空转 FATAL）
- watchdog_kick 在 FATAL 锁存下仍喂硬件 TWDT（否则 TWDT_PANIC=y 时 FATAL=重启循环）
