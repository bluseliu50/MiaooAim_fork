#include "power_mgr.h"

#include <string.h>
#include <time.h>

#include "display_mode.h"
#include "epd.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_utils.h"
#include "time_sync.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "power";

#define NVS_NS          "power"
#define NVS_KEY_EN      "enabled"
#define NVS_KEY_INTV    "interval"
#define NVS_KEY_IDLE    "idle"
#define NVS_KEY_MODE    "last_mode"
#define NVS_KEY_MODE_V2 "last_mode_v2"

#define DEFAULT_INTERVAL_MIN   60
#define DEFAULT_IDLE_TIMEOUT_S 120
#define CHECK_PERIOD_MS        10000
#define IDLE_TASK_STACK        4096
#define EPD_SLEEP_SETTLE_MS    25000
#define CALENDAR_MIDNIGHT_WAKE_GRACE_S 5

/*
 * 按键引脚见 button.c（SW3=9 / SW4=46 / SW5=3）。
 * EXT1 深睡唤醒：ESP32-S3 仅允许 RTC GPIO 0–21（见 ESP-IDF sleep_modes 文档），
 * GPIO46 不能参与 EXT1，掩码里若含 46 会导致 esp_sleep_enable_ext1_wakeup 失败、按键全不能唤醒。
 * SW4 仍可在运行时使用；深睡后请用 SW3 或 SW5 唤醒，或等定时唤醒。
 */
#define WAKE_GPIO_SW3  9
#define WAKE_GPIO_SW5  3

static power_config_t s_cfg = {
    .enabled        = false,
    .interval_min   = DEFAULT_INTERVAL_MIN,
    .idle_timeout_s = DEFAULT_IDLE_TIMEOUT_S,
};

static bool              s_timer_wake;
static int64_t           s_last_activity_us;
static int64_t           s_last_epd_refresh_us;
static portMUX_TYPE      s_activity_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t      s_idle_task;

static void enter_sleep_internal(bool from_idle, int64_t idle_snapshot_us);

/* ── NVS helpers ──────────────────────────────────────────────────── */

static void load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t v8;
    if (nvs_get_u8(h, NVS_KEY_EN, &v8) == ESP_OK)
        s_cfg.enabled = (v8 != 0);

    int32_t v32;
    if (nvs_get_i32(h, NVS_KEY_INTV, &v32) == ESP_OK && v32 >= 1 && v32 <= 1440)
        s_cfg.interval_min = (int)v32;

    if (nvs_get_i32(h, NVS_KEY_IDLE, &v32) == ESP_OK && v32 >= 30 && v32 <= 600)
        s_cfg.idle_timeout_s = (int)v32;

    nvs_close(h);
}

static void save_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_EN, s_cfg.enabled ? 1 : 0);
    nvs_set_i32(h, NVS_KEY_INTV, (int32_t)s_cfg.interval_min);
    nvs_set_i32(h, NVS_KEY_IDLE, (int32_t)s_cfg.idle_timeout_s);
    nvs_commit(h);
    nvs_close(h);
}

/* ── idle monitor task (dedicated stack, no Tmr Svc dependency) ──── */

static void idle_monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_PERIOD_MS));

        bool enabled;
        int timeout_s;
        portENTER_CRITICAL(&s_activity_mux);
        enabled   = s_cfg.enabled;
        timeout_s = s_cfg.idle_timeout_s;
        int64_t last = s_last_activity_us;
        portEXIT_CRITICAL(&s_activity_mux);

        if (!enabled) continue;

        int64_t idle_us = esp_timer_get_time() - last;
        int64_t threshold_us = (int64_t)timeout_s * 1000000LL;

        if (idle_us >= threshold_us) {
            ESP_LOGI(TAG, "Idle %d s >= %d s, entering deep sleep",
                     (int)(idle_us / 1000000LL), timeout_s);
            enter_sleep_internal(true, last);
        }
    }
}

/* ── public API ───────────────────────────────────────────────────── */

esp_err_t power_mgr_init(void)
{
    load_config();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    s_timer_wake = (cause == ESP_SLEEP_WAKEUP_TIMER);

    portENTER_CRITICAL(&s_activity_mux);
    s_last_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_activity_mux);

    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Woke from deep sleep (timer)");
    } else if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG, "Woke from deep sleep (button)");
    } else {
        ESP_LOGI(TAG, "Normal power-on (cause=%d)", (int)cause);
    }

    ESP_LOGI(TAG, "Config: enabled=%d, interval=%d min, idle=%d s",
             s_cfg.enabled, s_cfg.interval_min, s_cfg.idle_timeout_s);

    return ESP_OK;
}

bool power_mgr_is_timer_wake(void)
{
    return s_timer_wake && s_cfg.enabled;
}

esp_err_t power_mgr_get_config(power_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_activity_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_activity_mux);
    return ESP_OK;
}

esp_err_t power_mgr_set_config(const power_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    power_config_t validated = *cfg;
    if (validated.interval_min < 1)    validated.interval_min = 1;
    if (validated.interval_min > 1440) validated.interval_min = 1440;
    if (validated.idle_timeout_s < 30)  validated.idle_timeout_s = 30;
    if (validated.idle_timeout_s > 600) validated.idle_timeout_s = 600;
    portENTER_CRITICAL(&s_activity_mux);
    s_cfg = validated;
    portEXIT_CRITICAL(&s_activity_mux);
    ESP_LOGI(TAG, "Config updated: enabled=%d, interval=%d min, idle=%d s",
             s_cfg.enabled, s_cfg.interval_min, s_cfg.idle_timeout_s);

    /* 开机时若低功耗为关，app_main 里的 power_mgr_arm() 会直接返回且不会创建
     * pwr_idle；之后仅在网页里打开低功耗时，必须在此再次 arm，否则永远不会进深睡。 */
    if (s_cfg.enabled) {
        esp_err_t err = power_mgr_arm();
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_activity_mux);
            s_cfg.enabled = false;
            portEXIT_CRITICAL(&s_activity_mux);
            save_config();
            return err;
        }
    }

    save_config();  /* NVS write outside critical section */
    return ESP_OK;
}

void power_mgr_reset_activity(void)
{
    portENTER_CRITICAL(&s_activity_mux);
    s_last_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_activity_mux);
}

void power_mgr_note_epd_refresh_complete(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_activity_mux);
    s_last_activity_us = now;
    s_last_epd_refresh_us = now;
    portEXIT_CRITICAL(&s_activity_mux);
}

static bool activity_after(int64_t timestamp_us)
{
    portENTER_CRITICAL(&s_activity_mux);
    bool changed = s_last_activity_us > timestamp_us;
    portEXIT_CRITICAL(&s_activity_mux);
    return changed;
}

static bool calendar_midnight_sleep_us(uint64_t *out_sleep_us)
{
    if (!out_sleep_us || power_mgr_load_mode() != DISPLAY_MODE_CALENDAR)
        return false;

    struct tm now_tm;
    if (!time_sync_get_local_relaxed(&now_tm))
        return false;

    time_t now = time(NULL);
    struct tm target = now_tm;
    target.tm_sec = CALENDAR_MIDNIGHT_WAKE_GRACE_S;
    target.tm_min = 0;
    target.tm_hour = 0;
    target.tm_mday += 1;
    target.tm_isdst = -1;

    time_t wake = mktime(&target);
    if (wake <= now)
        return false;

    uint64_t sleep_s = (uint64_t)(wake - now);
    *out_sleep_us = sleep_s * 1000000ULL;
    ESP_LOGI(TAG, "Calendar mode: next timer wake at local 00:00:%02d in %llu s",
             CALENDAR_MIDNIGHT_WAKE_GRACE_S, (unsigned long long)sleep_s);
    return true;
}

static uint64_t next_timer_wakeup_us(const power_config_t *cfg)
{
    uint64_t sleep_us = 0;
    if (calendar_midnight_sleep_us(&sleep_us))
        return sleep_us;
    return (uint64_t)cfg->interval_min * 60ULL * 1000000ULL;
}

esp_err_t power_mgr_arm(void)
{
    if (!s_cfg.enabled) {
        ESP_LOGI(TAG, "Sleep mode disabled, not arming");
        return ESP_OK;
    }

    if (!s_idle_task) {
        TaskHandle_t task = NULL;
        BaseType_t ok = xTaskCreate(idle_monitor_task, "pwr_idle", IDLE_TASK_STACK,
                                    NULL, 2, &task);
        if (ok != pdPASS) {
            s_idle_task = NULL;
            ESP_LOGE(TAG, "Failed to create idle monitor task");
            return ESP_ERR_NO_MEM;
        }
        s_idle_task = task;
    }

    portENTER_CRITICAL(&s_activity_mux);
    s_last_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_activity_mux);
    ESP_LOGI(TAG, "Armed: will sleep after %d s of inactivity", s_cfg.idle_timeout_s);
    return ESP_OK;
}

static void enter_sleep_internal(bool from_idle, int64_t idle_snapshot_us)
{
    power_config_t cfg;
    power_mgr_get_config(&cfg);

    if (from_idle && activity_after(idle_snapshot_us)) {
        ESP_LOGI(TAG, "Sleep postponed: activity changed before sleep entry");
        return;
    }

    int64_t sleep_request_us = esp_timer_get_time();

    /* Do not sleep while an EPD transfer/refresh still owns the display mutex. */
    if (epd_is_ready() && !epd_wait_idle(70000)) {
        ESP_LOGW(TAG, "EPD still busy, postponing deep sleep");
        power_mgr_reset_activity();
        return;
    }

    if (from_idle && activity_after(sleep_request_us)) {
        ESP_LOGI(TAG, "Sleep postponed: display/user activity while waiting for EPD");
        return;
    }

    int64_t last_epd_us;
    portENTER_CRITICAL(&s_activity_mux);
    last_epd_us = s_last_epd_refresh_us;
    portEXIT_CRITICAL(&s_activity_mux);
    if (last_epd_us > 0) {
        int64_t elapsed_ms = (esp_timer_get_time() - last_epd_us) / 1000LL;
        if (elapsed_ms < EPD_SLEEP_SETTLE_MS) {
            int wait_ms = EPD_SLEEP_SETTLE_MS - (int)elapsed_ms;
            ESP_LOGI(TAG, "Waiting %d ms after EPD refresh before deep sleep", wait_ms);
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
    }

    uint64_t sleep_us = next_timer_wakeup_us(&cfg);
    esp_err_t terr = esp_sleep_enable_timer_wakeup(sleep_us);
    if (terr != ESP_OK) {
        ESP_LOGE(TAG, "timer wakeup config failed: %s", esp_err_to_name(terr));
        power_mgr_reset_activity();
        return;
    }

    uint64_t gpio_mask =
        (1ULL << WAKE_GPIO_SW3) | (1ULL << WAKE_GPIO_SW5);
    esp_err_t werr =
        esp_sleep_enable_ext1_wakeup(gpio_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
    if (werr != ESP_OK)
        ESP_LOGE(TAG, "ext1 wakeup config failed: %s (check RTC GPIO mask)",
                 esp_err_to_name(werr));

    ESP_LOGI(TAG,
             "Entering deep sleep for %llu s (configured interval=%d min, EXT1: GPIO %d/%d only; SW4 not on RTC)",
             (unsigned long long)(sleep_us / 1000000ULL), cfg.interval_min,
             WAKE_GPIO_SW5, WAKE_GPIO_SW3);

    nvs_flush_all();
    usb_serial_jtag_ll_phy_enable_pad(false);  // 关闭 USB PHY，避免深睡眠漏电
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}

void power_mgr_enter_sleep(void)
{
    enter_sleep_internal(false, 0);
}

void power_mgr_save_mode(int mode_index)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_KEY_MODE, (int32_t)mode_index);
    nvs_set_u8(h, NVS_KEY_MODE_V2, 1);
    nvs_commit(h);
    nvs_close(h);
}

int power_mgr_load_mode(void)
{
    nvs_handle_t h;
    int32_t mode = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    nvs_get_i32(h, NVS_KEY_MODE, &mode);
    nvs_close(h);
    return (int)mode;
}

bool power_mgr_saved_mode_has_marker(void)
{
    nvs_handle_t h;
    uint8_t marker = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_MODE_V2, &marker);
    nvs_close(h);
    return err == ESP_OK && marker == 1;
}
