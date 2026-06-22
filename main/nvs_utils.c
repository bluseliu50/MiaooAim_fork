#include "nvs_utils.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "nvs_util";

#define MAX_TRACKED 12

typedef struct {
    nvs_handle_t handle;
    int64_t      last_commit_us;
    bool         pending;
} tracked_ns_t;

static tracked_ns_t s_tracked[MAX_TRACKED];
static int          s_count;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_timer;

static tracked_ns_t *find_or_add(nvs_handle_t h)
{
    for (int i = 0; i < s_count; i++) {
        if (s_tracked[i].handle == h)
            return &s_tracked[i];
    }
    if (s_count < MAX_TRACKED) {
        tracked_ns_t *t = &s_tracked[s_count++];
        t->handle = h;
        t->last_commit_us = 0;
        t->pending = false;
        return t;
    }
    return NULL;
}

static void flush_pending(void)
{
    for (int i = 0; i < s_count; i++) {
        if (s_tracked[i].pending) {
            nvs_commit(s_tracked[i].handle);
            s_tracked[i].pending = false;
            s_tracked[i].last_commit_us = esp_timer_get_time();
        }
    }
}

static void timer_cb(void *arg)
{
    (void)arg;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        flush_pending();
        xSemaphoreGive(s_lock);
    }
}

esp_err_t nvs_utils_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    esp_timer_create_args_t args = {
        .callback = timer_cb,
        .name = "nvs_flush",
    };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "NVS throttle ready (interval=%d ms)", NVS_THROTTLE_MS);
    return ESP_OK;
}

esp_err_t nvs_throttled_commit(nvs_handle_t handle)
{
    if (!s_lock) return nvs_commit(handle);

    xSemaphoreTake(s_lock, portMAX_DELAY);

    tracked_ns_t *t = find_or_add(handle);
    if (!t) {
        xSemaphoreGive(s_lock);
        return nvs_commit(handle);
    }

    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - t->last_commit_us;

    if (elapsed >= (int64_t)NVS_THROTTLE_MS * 1000) {
        esp_err_t err = nvs_commit(handle);
        t->last_commit_us = now;
        t->pending = false;
        xSemaphoreGive(s_lock);
        return err;
    }

    t->pending = true;
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, (uint64_t)NVS_THROTTLE_MS * 1000);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void nvs_flush_all(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    flush_pending();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Flushed all pending NVS writes");
}
