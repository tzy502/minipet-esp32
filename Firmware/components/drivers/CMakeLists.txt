# BSP 驱动层组件：只依赖硬件外设驱动 + profiles 引脚表。
# 对外暴露 components/drivers/include/drivers.h（统一入口头）。
#
# 显示驱动按 Kconfig MP_BOARD 二选一（display_co5300.h 是两块板共用的 API 契约头）：
#   AMOLED-2.16 → display_co5300.c（480x480 AMOLED，背压版，勿动）
#   LCD-1.85B   → display_st77916.c（360x360 圆 LCD，含 BL=GPIO5 背光）
set(DRIVERS_DISPLAY_SRCS "display_co5300.c")
if(CONFIG_MP_BOARD_LCD185B)
    set(DRIVERS_DISPLAY_SRCS "display_st77916.c")
endif()

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
                  waveshare__esp32_s3_touch_amoled_2_16 esp_lcd_co5300 esp_lcd_st77916
)
