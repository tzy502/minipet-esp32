/**
 * pcm_ring.c — PSRAM 环形缓冲实现（单写者 bgm / 单读者 i2s feeder）
 */
#include "pcm_ring.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

struct pcm_ring_t {
    int16_t         *buf;
    size_t          cap;            /* int16 计 */
    size_t          rd, wr;         /* int16 计，单调递增取模 */
    SemaphoreHandle_t lock;
};

pcm_ring_t *pcm_ring_create(size_t size_bytes)
{
    size_t cap = (size_bytes / 2) & ~(size_t)2;   /* 4 字节对齐 */
    if (cap < 1024) return NULL;

    pcm_ring_t *r = malloc(sizeof(*r));
    if (!r) return NULL;

    r->buf = heap_caps_malloc(cap * 2, MALLOC_CAP_SPIRAM);
    if (!r->buf) {
        free(r);
        return NULL;
    }
    r->cap = cap;
    r->rd = r->wr = 0;
    r->lock = xSemaphoreCreateMutex();
    return r;
}

size_t pcm_ring_write(pcm_ring_t *r, const int16_t *data, size_t samples)
{
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t used = r->wr - r->rd;
    size_t space = r->cap - used;
    size_t n = (samples < space) ? samples : space;   /* 满则短写（背压） */

    for (size_t i = 0; i < n; i++) {
        r->buf[r->wr % r->cap] = data[i];
        r->wr++;
    }
    xSemaphoreGive(r->lock);
    return n;
}

size_t pcm_ring_read(pcm_ring_t *r, int16_t *out, size_t samples, uint32_t timeout_ms)
{
    int64_t deadline = -1;
    if (timeout_ms > 0) {
        deadline = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS + timeout_ms;
    }

    for (;;) {
        xSemaphoreTake(r->lock, portMAX_DELAY);
        size_t avail = r->wr - r->rd;
        if (avail > 0) {
            size_t n = (samples < avail) ? samples : avail;
            for (size_t i = 0; i < n; i++) {
                out[i] = r->buf[r->rd % r->cap];
                r->rd++;
            }
            xSemaphoreGive(r->lock);
            return n;
        }
        xSemaphoreGive(r->lock);

        if (deadline < 0) return 0;
        int64_t now = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now >= deadline) return 0;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

size_t pcm_ring_count(pcm_ring_t *r)
{
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t n = r->wr - r->rd;
    xSemaphoreGive(r->lock);
    return n;
}

size_t pcm_ring_capacity(pcm_ring_t *r)
{
    return r->cap;
}

void pcm_ring_flush(pcm_ring_t *r)
{
    xSemaphoreTake(r->lock, portMAX_DELAY);
    r->rd = r->wr;
    xSemaphoreGive(r->lock);
}

void pcm_ring_destroy(pcm_ring_t *r)
{
    if (!r) return;
    vSemaphoreDelete(r->lock);
    heap_caps_free(r->buf);
    free(r);
}
