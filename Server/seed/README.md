# Server/seed/ — 设备种子数据

服务端出厂自带的数据种子，供首次启动 / 重置 / CI 使用。

| 条目 | 说明 |
|---|---|
| `fonts/` | 三档宋体位图字体源（font-16/24/32.json，4bpp 中间格式，由 `scripts/fontpack/build-fonts.sh` 生成）——服务端 FontPackWriter 打包 kind=4 FONT 下发 |
| `clock_table.json` | 26 张地图的时钟显示坐标初始建议值（E9 魔法值表，视口屏幕坐标，Web 可改；来源 `~/Desktop/wz-clock-maps.md` 2026-09-24 全量实测，`scripts/clock_table_gen.py` 生成） |
| `profiles/` | 设备硬件档案（如 `amoled216.json`：480×480 AMOLED 圆屏） |
| `default-appearance.json` | 默认纸娃娃装扮配置（未配对 / 匿名设备的出厂宠物） |
