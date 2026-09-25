# MiniPet 固件 main 组件（software-design 四.固件 / 4.1 双核任务三队列）
#
# 应用层（本组件）：app/ 状态机·看门狗·交互·配网；net/ http·poll·event·素材·OTA；
#                  audio/ minimp3·环形缓冲·BGM。
# 并行组件：drivers（BSP）、lvgl（菜单 UI）、profiles/amoled216.h（设备画像）。
#
# 注意：
#  - esp_ota_ops 头文件由组件 app_update 提供（任务书口径 esp_ota_ops → 此处映射）
#  - ../profiles 以相对包含暴露给本组件（顶层 CMake 不动）
idf_component_register(
    SRCS
        "main.c"
        "app/state_machine.c"
        "app/watchdog.c"
        "app/input_dispatch.c"
        "app/provision.c"
        "render/mpak.c"
        "render/compositor.c"
        "render/entity_anim.c"
        "render/lvgl_bridge.c"
        "render/font_lazy.c"
        "render/clock_digits.c"
        "net/http_client.c"
        "net/poller.c"
        "net/events.c"
        "net/asset_dl.c"
        "net/ota.c"
        "audio/bgm.c"
        "audio/pcm_ring.c"
        "audio/minimp3/minimp3.c"
    INCLUDE_DIRS
        "."
        "app"
        "render"
        "net"
        "audio"
    PRIV_REQUIRES
        drivers            # components/drivers（hal_contract.h 消费面）
        lvgl               # 渲染层（main/render 由渲染 agent 提供）
        fatfs              # esp_vfs_fat_info（TF 水位）
        nvs_flash          # 配置/看门狗计数/偏好
        esp_http_client    # E2 全端点
        esp_http_server    # provision captive portal
        esp_wifi           # SoftAP/STA
        esp_netif          # IP 事件/SNTP
        esp_event          # WiFi 事件循环
        lwip               # sockets（DNS 劫持）/SNTP
        app_update         # esp_ota_ops（E11 双分区）
        json               # cJSON（协议/E2 全 JSON 端点）
        esp_timer
        esp_system
        esp_common
)

# profiles/amoled216.h：profiles 组件经 drivers 传递暴露（drivers.h 里 #include "amoled216.h"）；
# render/render.h 里是 #include "profiles/amoled216.h" —— 需要 Firmware 根目录在包含路径上。
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/.."
    "${CMAKE_CURRENT_LIST_DIR}/../profiles"
)
