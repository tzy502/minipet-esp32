# BSP 驱动层组件：只依赖硬件外设驱动 + profiles 引脚表。
# 对外暴露 components/drivers/include/drivers.h（统一入口头）。
idf_component_register(
    SRCS "i2c_bus.c"
         "display_co5300.c"
         "touch_cst9220.c"
         "imu_qmi8658.c"
         "rtc_pcf85063.c"
         "pmu_axp2101.c"
         "codec_es8311.c"
         "sd_tf.c"
         "key_gpio18.c"
    INCLUDE_DIRS "." "include"
    REQUIRES profiles driver esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_i2s
    PRIV_REQUIRES esp_timer sdmmc fatfs wear_levelling esp_partition
)
