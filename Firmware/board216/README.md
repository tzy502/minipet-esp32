# Firmware — ESP32-S3 固件

目标硬件：Waveshare ESP32-S3-Touch-AMOLED-2.16（SKU 33969）
框架：ESP-IDF v5.5+ + LVGL v9

固件铁律：
- 零 WZ 解析（素材由服务端下发）
- 零 Hermes / 零 HA 代码（协议无此概念）
- 设备永远是 HTTP client
- 动画只播服务端下发布局表（WZ 真实 action，禁自创）

引脚表见 ../docs/ai/waveshare-wiki-ESP32-S3-Touch-AMOLED-2.16.md
已知坑：GPIO46=PA_CTRL 功放使能（不开无声）；GPIO14/15 共享 I2C 五器件。

## 目录结构

```
Firmware/
├── CMakeLists.txt               # 工程根；SDKCONFIG_DEFAULTS + EXTRA_COMPONENT_DIRS
├── sdkconfig.defaults           # 通用默认（FREERTOS_HZ=1000 / FATFS LFN / 性能优先）
├── sdkconfig.defaults.amoled216 # 板级：S3 / 240MHz / 8MB Octal PSRAM / 16MB Flash QIO
├── profiles/                    # 设备板卡描述（组件）
│   ├── amoled216.h              #   minipet_profile_t：480x480/形状/能力位/引脚表
│   ├── profile_amoled216.c      #   MINIPET_PROFILE_AMOLED216 实例（引脚唯一来源）
│   └── CMakeLists.txt
├── components/
│   └── drivers/                 # BSP 驱动层（本层，不含业务）
│       ├── i2c_bus.c/h          # 共享 I2C（SCL=14/SDA=15）+ 器件注册表 + 总线互斥
│       ├── display_co5300.c/h   # AMOLED QSPI（4/5/6/7/38/12/39）blit/亮度/睡眠
│       ├── touch_cst9220.c/h    # 触摸（INT=11/RST=40）读首点 + 中断通知
│       ├── imu_qmi8658.c/h      # 六轴（INT1=17/INT2=21）acc/gyro + DRDY 中断
│       ├── rtc_pcf85063.c/h     # RTC（INT=13）BCD 读写 + set_from_epoch
│       ├── pmu_axp2101.c/h      # 电量%/充电态 + 低电轮询（IRQ 引脚未确认）
│       ├── codec_es8311.c/h     # Codec I2C 配置 + I2S(MCLK=42/BCLK=9/LRCK=45/DSDIN=8)
│       │                        #   + pa_ctrl_enable(GPIO46，默认关)
│       ├── sd_tf.c/h            # microSD SPI(1/2/3/41) + FATFS，sd_mount()→errno
│       ├── key_gpio18.c/h       # 菜单键 GPIO18，20ms 消抖 + 回调
│       └── include/drivers.h    # 统一对外头（app/render/audio 只 include 它）
└── main/                        # 应用层（app/render/net/audio，另建，驱动层勿碰）
```

## 初始化顺序（必须，详见 drivers.h 头注释）

```c
i2c_bus_init();              // 1. 共享 I2C（其余 I2C 器件的公共前置）
display_init();              // 2. CO5300 QSPI（SPI2_HOST，独占）
touch_cst9220_init();        // 3. 触摸
imu_qmi8658_init();          // 4. IMU
rtc_pcf85063_init();         // 5. RTC（未校时前 get_time 返回 INVALID_STATE）
pmu_axp2101_init();          // 6. PMU（低电默认轮询）
codec_es8311_init(44100);    // 7. 音频（PA 默认关，播放前 pa_ctrl_enable(true)）
sd_mount();                  // 8. TF 卡（SPI3_HOST，失败返回 errno，app 降级）
```

## 驱动层契约（上层模块必读）

- **blit 字节序**：`display_blit(x,y,w,h,buf)` 的 buf 是**大端 RGB565**（CO5300
  显存序），LVGL/合成器输出小端时由渲染层预先字节交换。
- **中断回调上下文**：触摸/IMU/按键/PMU 的回调在中断上下文执行，只允许
  FROM_ISR 动作，禁止 I2C/SPI/阻塞。
- **I2C 纪律**：所有 I2C 器件必须经 `i2c_bus_register_device()` 注册后使用，
  事务统一走总线互斥锁；禁止绕过 `i2c_bus_*` 直连。
- **SPI 分工**：显示=SPI2_HOST（quad，40MHz）；SD=SPI3_HOST（20MHz）。互不相干。
- **引脚唯一来源**：`MINIPET_PROFILE_AMOLED216.pins`（profiles/amoled216.h）。
- **与 main/app/hal_contract.h 的命名差异**：app 层草拟的 hal_contract.h
  使用的短名（display_off/display_set_backlight/touch_read/imu_wait_event/
  codec_write/pa…）与本层实际导出名不同。对齐方式：app 侧用
  `display_set_sleep(true)`≈display_off、`display_brightness()`≈set_backlight、
  `display_fill_rect()`、`touch_cst9220_read()`≈touch_read、
  `imu_qmi8658_wait_event()`≈imu_wait_event、`imu_qmi8658_read_acc_gyro()`、
  `rtc_pcf85063_get_time()/set_time()`、`pmu_axp2101_get_battery_pct()/
  get_power()`、`codec_es8311_write()/pa_ctrl_enable()`、`sd_mount()`。
  `pmu_axp2101_get_temperature_c()` v1 返回 NOT_SUPPORTED（寄存器未核实）。

## 构建

```bash
idf.py set-target esp32s3   # 首次
idf.py build flash monitor  # 需 main/ 组件就位（应用层）
```

sdkconfig 默认值链：`sdkconfig.defaults` → `sdkconfig.defaults.amoled216`。

## Bring-up 核对清单（无法本机编译/实测，上机首日过一遍）

代码中所有 `[核对]` 标记：

1. **CO5300 初始化表**（display_co5300.c `s_init_cmds`）——对照微雪官方 demo；
   屏不响应先试 `CO5300_SPI_MODE 0→3`。
2. **CST9220 数据帧偏移/地址 0x5A**（touch_cst9220.c）——i2cdetect 扫总线确认。
3. **QMI8658 CTRL1/CTRL7/CTRL8 位定义与 ODR/量程档位**（imu_qmi8658.c）——
   对照 QMI8658A 手册寄存器表；WHO_AM_I=0x05 已做硬校验。
4. **ES8311 上电序列**（codec_es8311.c `s_es8311_init`）——对照 espressif
   esp_codec_dev 参考；无声时优先查 0x01/0x02/0x12。
5. **AXP2101 状态位/电量寄存器**（pmu_axp2101.c）——init 已打印原始值，
   用插/拔 USB、充电/满电状态对照修正掩码。
6. **屏幕形状**：profile 按**圆形**屏（480x480）填写，实测方形改
   `MINIPET_SHAPE_SQUARE`。
7. **PMU IRQ 引脚**：原理图确认 GPIO16(SYS_OUT) 是否为 AXP2101 中断输出；
   确认后填 `pins.pmu.pmu_irq`，`pmu_axp2101_set_isr_callback()` 即生效。
