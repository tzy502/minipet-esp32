# profiles 组件：设备板卡描述（引脚表 / 能力位 / 硬件参数）
# 由本工程根 CMakeLists.txt（Firmware/board216/CMakeLists.txt）的 EXTRA_COMPONENT_DIRS 挂载，组件名 = 目录名 "profiles"。
# board216 工程（仅 AMOLED-2.16 板）：只编译 amoled216 一个 profile。
idf_component_register(
    SRCS "profile_amoled216.c"
    INCLUDE_DIRS "."
)
