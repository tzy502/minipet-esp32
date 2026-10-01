# BSP 驱动层组件（LCD-1.85B 单板分支）：只依赖硬件外设驱动 + profiles 引脚表。
# 对外暴露 components/drivers/include/drivers.h（统一入口头）。
#
# 显示驱动固定 display_st77916.c（360x360 圆 LCD，ST77916 QSPI，含 BL=GPIO5 背光；
# API 契约头 = display_st77916.h）。
#
# 触摸 touch_cst816.c（CST816S@0x15，纯轮询）：MP_TOUCH_CST816 条件编译——
# 关闭=整文件剔除（bring-up 隔离开关，main/Kconfig.projbuild default y）。
#
# 2026-10-01 从 216 移植补齐的三件（本板等价物，接口同名同签名）：
#   · codec_es8311.c   ES8311 + I2S TX（音频链路，本板引脚走 profile.pins.audio）
#   · pmu_bq27220.c    电量计（216 是 AXP2101；本板是 BQ27220@0x55，只读标准命令）
#   · touch_cst816.c   触摸（216 是 CST9220@0x5A；本板 CST816S@0x15，帧格式不同）
# 板级隔离说明：本目录是 board185b 单板构建树的组件，board216 构建树
# （board216/components/drivers）不含本文件，216 构建零变化（结构性隔离，
# 无需 #if CONFIG_MP_BOARD_* 之类的跨板开关）。
set(TOUCH_SRCS "")
if(CONFIG_MP_TOUCH_CST816)
    list(APPEND TOUCH_SRCS "touch_cst816.c")
endif()

idf_component_register(
    SRCS "i2c_bus.c"
         "display_st77916.c"
         "imu_qmi8658.c"
         "rtc_pcf85063.c"
         "pmu_bq27220.c"
         "codec_es8311.c"
         "sd_tf.c"
         "key_gpio0.c"
         ${TOUCH_SRCS}
    INCLUDE_DIRS "." "include"
    REQUIRES profiles driver esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_i2s esp_driver_sdmmc
    PRIV_REQUIRES esp_timer sdmmc fatfs wear_levelling esp_partition esp_lcd
                  esp_lcd_st77916
)
