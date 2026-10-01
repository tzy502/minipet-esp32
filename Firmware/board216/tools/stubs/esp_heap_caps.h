/*
 * tools/stubs/esp_heap_caps.h — host 侧编译 mpak.c 用的 heap_caps 替身。
 * 语义等价性：固件里瓦片缓存一律 MALLOC_CAP_SPIRAM（内部堆只剩几十 KB，抢内部堆
 * 会 newlib abort）；host 上没有 PSRAM 概念，直接映射到 malloc —— 单测校验的是
 * **读取算法与字节数**，内存来源与真机无关。
 */
#ifndef HOST_STUB_ESP_HEAP_CAPS_H
#define HOST_STUB_ESP_HEAP_CAPS_H

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT   0

static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void  heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(int caps) { (void)caps; return 0; }

#endif /* HOST_STUB_ESP_HEAP_CAPS_H */
