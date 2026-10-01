# Firmware — ESP32-S3 固件（LCD-1.85B 单板分支）

目标硬件：Waveshare ESP32-S3-Touch-LCD-1.85B（1.85" 圆 360×360 LCD，ST77916）
框架：ESP-IDF v5.5+ + LVGL v9

固件铁律：
- 零 WZ 解析（素材由服务端下发）
- 零 Hermes / 零 HA 代码（协议无此概念）
- 设备永远是 HTTP client
- 动画只播服务端下发布局表（WZ 真实 action，禁自创）

本分支能力位：has_audio/has_touch/has_pmu/has_key 全 false（音频/触摸/PMU/
独立菜单键链路已拆除，降级路径即正式行为）；菜单键 = BOOT(GPIO0)。
已知坑：GPIO5=LCD 背光（开/关两档）；GPIO10/11 共享 I2C。
RAMless 面板：靠持续全帧刷新任务维持画面（合成器 480×480 → 360 取样）。

## 目录结构

```
Firmware/
├── CMakeLists.txt                    # 工程根；SDKCONFIG_DEFAULTS + EXTRA_COMPONENT_DIRS
├── sdkconfig.defaults                # 通用默认（FREERTOS_HZ=1000 / FATFS LFN / 性能优先）
├── sdkconfig.defaults.amoled185b     # 185B 板级默认（含素材堆门 6KB）
├── sdkconfig                         # 活动配置（185B 实测值，原 sdkconfig.185b 更名）
├── profiles/                         # 设备板卡描述（组件）
│   ├── lcd185b.h                     #   minipet_profile_t：480x480 合成空间/形状/能力位/引脚表
│   ├── profile_lcd185b.c             #   MINIPET_PROFILE_LCD185B 实例（引脚唯一来源）
│   └── CMakeLists.txt
├── components/
│   └── drivers/                      # BSP 驱动层（本层，不含业务）
│       ├── i2c_bus.c/h               # 共享 I2C（SCL=10/SDA=11）+ 器件注册表 + 总线互斥
│       ├── display_st77916.c/h       # 圆 LCD QSPI（46/45/42/41/40/21/3，BL=5）持续刷新/blit/睡眠
│       ├── imu_qmi8658.c/h           # 六轴（INT 未引出，轮询）acc/gyro + DRDY 中断
│       ├── rtc_pcf85063.c/h          # RTC（INT=6）BCD 读写 + set_from_epoch
│       ├── sd_tf.c/h                 # microSD SDMMC 1-bit(15/14/16) + FATFS，sd_mount()→errno
│       ├── key_gpio0.c/h             # 菜单键 BOOT=GPIO0，30ms 轮询消抖（无 ISR）
│       └── include/drivers.h         # 统一对外头（app/render/audio 只 include 它）
└── main/                             # 应用层（app/render/net/audio，另建，驱动层勿碰）
```

## 初始化顺序（必须，详见 drivers.h 头注释）

```c
i2c_bus_init();              // 1. 共享 I2C（其余 I2C 器件的公共前置）
display_init();              // 2. ST77916 QSPI（SPI2_HOST，独占；render_init 内部调用）
imu_qmi8658_init();          // 3. IMU（缺失不阻断启动，app 层降级）
rtc_pcf85063_init();         // 4. RTC（未校时前 get_time 返回 INVALID_STATE）
sd_mount();                  // 5. TF 卡（SDMMC，失败返回 errno，app 降级）
key_gpio0_init();            // 6. 菜单键 BOOT=GPIO0（input 任务内）
```

## 驱动层契约（上层模块必读）

- **blit 字节序**：`display_blit(x,y,w,h,buf)` 的 buf 是**大端 RGB565**，
  LVGL/合成器输出小端时由渲染层预先字节交换；持续全帧刷新开启期间
  blit/fill_rect 为无操作（刷新流是唯一显示通路）。
- **中断回调上下文**：IMU 的回调在中断上下文执行，只允许
  FROM_ISR 动作，禁止 I2C/SPI/阻塞。
- **I2C 纪律**：所有 I2C 器件必须经 `i2c_bus_register_device()` 注册后使用，
  事务统一走总线互斥锁；禁止绕过 `i2c_bus_*` 直连。
- **总线分工**：显示=SPI2_HOST（QSPI）；TF=SDMMC 槽（1-bit）。互不相干。
- **引脚唯一来源**：`MINIPET_ACTIVE_PROFILE.pins`（profiles/lcd185b.h）。
- **与 main/app/hal_contract.h 的命名差异**：app 层使用的短名
  （display_off/display_set_backlight/imu_wait_event/codec_write/pa…）
  与本层实际导出名不同。对齐方式：app 侧用
  `display_set_sleep(true)`≈display_off、`display_brightness()`≈set_backlight、
  `display_fill_rect()`、`imu_qmi8658_wait_event()`≈imu_wait_event、
  `imu_qmi8658_read_acc()`、`rtc_pcf85063_get_time()/set_time()`、`sd_mount()`；
  音频入口（mp_codec_*/mp_pa_enable）为本板无操作直路（has_audio=false）。

## 构建

```bash
idf.py -B build build        # 需 main/ 组件就位（应用层）
```

sdkconfig 默认值链：`sdkconfig.defaults` → `sdkconfig.defaults.amoled185b`；
活动配置 `sdkconfig`（185B 实测值）。

## Bring-up 核对清单

代码中所有 `[核对]` 标记：

1. **ST77916 批次 init 表**（display_st77916.c + st77916_init_tables_esp.c）——
   官方 BSP v1/v2 序列按 RDDID 自动选择；花屏先查 INVOFF/COLMOD 取证日志。
2. **QMI8658 CTRL1/CTRL7/CTRL8 位定义与 ODR/量程档位**（imu_qmi8658.c）——
   对照 QMI8658A 手册寄存器表；WHO_AM_I=0x05 已做硬校验。
3. **屏幕形状**：profile 按**圆形**屏（合成空间 480x480）填写。
4. **素材绑定堆门**：CONFIG_MP_ASSET_HEAP_GATE_KB=6 为实测安全值
   （本板内部堆紧张），调高前须多轮绑定零 abort 验证。
