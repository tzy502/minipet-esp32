# profiles 组件：设备板卡描述（引脚表 / 能力位 / 硬件参数）
# 由 Firmware/CMakeLists.txt 的 EXTRA_COMPONENT_DIRS 挂载，组件名 = 目录名 "profiles"。
set(PROFILE_SRCS "profile_amoled216.c")
if(CONFIG_MP_BOARD_LCD185B)
    list(APPEND PROFILE_SRCS "profile_lcd185b.c")
endif()
idf_component_register(
    SRCS ${PROFILE_SRCS}
    INCLUDE_DIRS "."
)
