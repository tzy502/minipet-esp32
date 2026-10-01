# profiles 组件：设备板卡描述（引脚表 / 能力位 / 硬件参数）
# 由 Firmware/CMakeLists.txt 的 EXTRA_COMPONENT_DIRS 挂载，组件名 = 目录名 "profiles"。
# 185B 单板分支：只编译 profile_lcd185b.c。
idf_component_register(
    SRCS "profile_lcd185b.c"
    INCLUDE_DIRS "."
)
