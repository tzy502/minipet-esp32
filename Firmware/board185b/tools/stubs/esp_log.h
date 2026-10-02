/*
 * tools/stubs/esp_log.h — host 侧编译 mpak.c 用的 ESP_LOG 替身
 * 只给 host 单测（tools/test_tiled_bgmap.c）用；固件构建走真正的 ESP-IDF 头。
 */
#ifndef HOST_STUB_ESP_LOG_H
#define HOST_STUB_ESP_LOG_H

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I %s: " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); } while (0)

#endif /* HOST_STUB_ESP_LOG_H */
