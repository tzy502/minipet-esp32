/**
 * ota.c — WiFi OTA（E11）双分区 + 回滚
 */
#include "ota.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_ota_ops.h"

#include "app_core.h"
#include "http_client.h"
#include "state_machine.h"

static const char *TAG = "ota";

#define OTA_BUF_LEN     4096
#define OTA_TIMEOUT_MS  30000

typedef struct {
    char ver[16];
    char url[160];
} ota_req_t;

static QueueHandle_t s_req_q;

/* ------------------------------------------------------------------ */
/* 版本比较："a.b.c" 语义化比较；返回 >0/=0/<0                              */
/* ------------------------------------------------------------------ */
static int ver_parse(const char *v, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    return sscanf(v, "%d.%d.%d", &out[0], &out[1], &out[2]) > 0 ? 0 : -1;
}

static int ver_cmp(const char *a, const char *b)
{
    int va[3], vb[3];
    if (ver_parse(a, va) != 0 || ver_parse(b, vb) != 0) return 0;
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return va[i] - vb[i];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 流式写入                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    esp_ota_handle_t handle;
    uint64_t written;
    bool ok;
} ota_wr_ctx_t;

static bool ota_write_chunk(void *ctx_, const char *data, size_t len)
{
    ota_wr_ctx_t *c = ctx_;
    esp_err_t err = esp_ota_write(c->handle, data, len);
    if (err != ESP_OK) {
        c->ok = false;
        return false;
    }
    c->written += len;
    return true;
}

static void do_ota(const char *ver, const char *url)
{
    /* 版本闸门：不升不降（同版重刷需服务端 bump 版本号） */
    if (ver_cmp(ver, MP_FIRMWARE_VERSION) <= 0) {
        ESP_LOGI(TAG, "ota ignored: %s <= current %s", ver, MP_FIRMWARE_VERSION);
        return;
    }

    state_machine_handle(MP_SM_EV_OTA_START);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "no ota partition");
        goto fail;
    }

    ota_wr_ctx_t ctx = { .handle = 0, .ok = true };
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ctx.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        goto fail;
    }

    int status = mp_http_get(url, OTA_TIMEOUT_MS, ota_write_chunk, &ctx);
    if (status != 200 || !ctx.ok || ctx.written < 1024) {
        ESP_LOGE(TAG, "ota download failed: status=%d ok=%d bytes=%llu",
                 status, (int)ctx.ok, (unsigned long long)ctx.written);
        esp_ota_abort(ctx.handle);
        goto fail;
    }

    err = esp_ota_end(ctx.handle);           /* 含镜像头/长度校验 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition failed");
        goto fail;
    }

    ESP_LOGI(TAG, "ota ok: %s -> %s (%llu bytes), rebooting",
             MP_FIRMWARE_VERSION, ver, (unsigned long long)ctx.written);
    {
        mp_cmd_t c = { .type = MP_CMD_OTA_DONE };
        mp_post_cmd(&c);
    }
    vTaskDelay(pdMS_TO_TICKS(3000));          /* 等气泡画完 */
    esp_restart();
    return;                                   /* not reached */

fail:
    {
        /* 失败：不切 boot 分区 = 下次仍启动旧分区（等效回滚） */
        mp_cmd_t c = { .type = MP_CMD_OTA_FAIL };
        mp_post_cmd(&c);
    }
    state_machine_handle(MP_SM_EV_OTA_END);
}

static void ota_task(void *arg)
{
    (void)arg;
    ota_req_t req;
    for (;;) {
        if (xQueueReceive(s_req_q, &req, portMAX_DELAY) == pdTRUE) {
            do_ota(req.ver, req.url);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                             */
/* ------------------------------------------------------------------ */
/* 【按需拉起 2026-09-27】OTA 任务原本开机常驻 6144B 栈，而它只在收到 OTA
 * 指令时才需要跑（设备可能几个月不升级一次）。本板内部 DRAM 运行期只剩
 * ~1.6KB 最大连续块 → 长轮询事务（需 4~8KB）因此失败 → 服务端一直显示 offline。
 * 改为：ota_start() 只建队列（几百字节），任务在**首次收到 OTA 指令**时才创建。
 * 代价：升级开始时的任务创建若因碎片失败，会重试一次（见下），但常规运行不再白占。 */
static bool ota_task_up;

static bool ota_task_try(void)
{
    if (ota_task_up) return true;
    if (xTaskCreatePinnedToCore(ota_task, "ota", 6144, NULL,
                                1 /* 最低优先级——4.1 */, NULL, 0 /* PRO */) == pdPASS) {
        ota_task_up = true;
        ESP_LOGW(TAG, "ota 任务按需创建成功（收到升级指令）");
        return true;
    }
    ESP_LOGE(TAG, "ota 任务创建失败（内部堆 空闲=%u 最大块=%u）→ 本次升级放弃",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return false;
}

void ota_start(void)
{
    s_req_q = xQueueCreate(2, sizeof(ota_req_t));
    /* 不在这里建任务：见上方注释（按需拉起省 6KB 常驻） */
}

void mp_ota_offer(const char *ver, const char *url)
{
    if (!s_req_q || !ver || !url || url[0] == 0) return;
    if (!ota_task_try()) return;      /* 按需拉起（失败即放弃本次升级） */
    ota_req_t req;
    strlcpy(req.ver, ver, sizeof(req.ver));
    strlcpy(req.url, url, sizeof(req.url));
    xQueueSend(s_req_q, &req, 0);             /* 满则丢（poller 会再推） */
}

void mp_ota_confirm_valid(void)
{
    /* 仅当处于 pending-verify 状态时生效；否则返回错误码，静默忽略 */
    esp_ota_mark_app_valid_cancel_rollback();
}
