# BSP 驱动层组件（LCD-1.85B 单板分支）：只依赖硬件外设驱动 + profiles 引脚表。
# 对外暴露 components/drivers/include/drivers.h（统一入口头）。
#
# 显示驱动固定 display_st77916.c（360x360 圆 LCD，ST77916 QSPI，含 BL=GPIO5 背光；
# API 契约头 = display_st77916.h）。
idf_component_register(
    SRCS "i2c_bus.c"
         "display_st77916.c"
         "imu_qmi8658.c"
         "rtc_pcf85063.c"
         "sd_tf.c"
         "key_gpio0.c"
    INCLUDE_DIRS "." "include"
    REQUIRES profiles driver esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_i2s esp_driver_sdmmc
    PRIV_REQUIRES esp_timer sdmmc fatfs wear_levelling esp_partition esp_lcd
                  esp_lcd_st77916
)
