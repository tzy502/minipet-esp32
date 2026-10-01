# BSP 驱动层组件：只依赖硬件外设驱动 + profiles 引脚表。
# 对外暴露 components/drivers/include/drivers.h（统一入口头）。
#
# 单板分支（board-216，仅 AMOLED-2.16）：显示驱动固定 display_co5300.c
# （480x480 AMOLED，CO5300，背压版，勿动）。
set(DRIVERS_DISPLAY_SRCS "display_co5300.c")

idf_component_register(
    SRCS "i2c_bus.c"
         ${DRIVERS_DISPLAY_SRCS}
         "touch_cst9220.c"
         "imu_qmi8658.c"
         "rtc_pcf85063.c"
         "pmu_axp2101.c"
         "codec_es8311.c"
         "sd_tf.c"
         "key_gpio18.c"
         "key_gpio0.c"
    INCLUDE_DIRS "." "include"
    REQUIRES profiles driver esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_i2s esp_driver_sdmmc
    PRIV_REQUIRES esp_timer sdmmc fatfs wear_levelling esp_partition esp_lcd
                  waveshare__esp32_s3_touch_amoled_2_16 esp_lcd_co5300
)
