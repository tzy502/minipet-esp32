# MiniPet 固件根 CMakeLists（ESP-IDF 5.5）
#
# 说明：
# - main/ 由应用层 agent 维护（app/render/net/audio），本文件只声明工程与组件目录。
# - 板级差异集中在 profiles/ 组件 + sdkconfig.defaults.amoled216。
# - SDKCONFIG_DEFAULTS / EXTRA_COMPONENT_DIRS 必须在 include project.cmake 之前设置。

cmake_minimum_required(VERSION 3.16)

# sdkconfig 默认值：先通用默认，再 amoled216 板级覆盖（后写的优先级更高）
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.amoled216")

# profiles/（设备引脚表/能力位）不在默认 components/ 搜索路径内，显式挂载为组件
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/profiles")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(minipet)
