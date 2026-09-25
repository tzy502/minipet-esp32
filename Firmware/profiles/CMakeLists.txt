# profiles 组件：设备板卡描述（引脚表 / 能力位 / 硬件参数）
# 由 Firmware/CMakeLists.txt 的 EXTRA_COMPONENT_DIRS 挂载，组件名 = 目录名 "profiles"。
idf_component_register(
    SRCS "profile_amoled216.c"
    INCLUDE_DIRS "."
)
