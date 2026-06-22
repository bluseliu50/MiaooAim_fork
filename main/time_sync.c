#include "time_sync.h"

#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"

#include "clock_display.h"

static const char *TAG = "time_sync";
static atomic_bool s_synced;
static atomic_bool s_ip_handler_registered;

/** 周期性校时：默认 15 分钟，减小两次 SNTP 之间的晶振漂移（原 1 小时易累计偏差） */
#define SNTP_RESYNC_INTERVAL_MS (15 * 60 * 1000)

static void on_time_sync(struct timeval *tv)
{
    s_synced = true;
    struct tm t;
    localtime_r(&tv->tv_sec, &t);
    ESP_LOGI(TAG, "Time synchronized: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    clock_display_notify_time_resync();
}

static void on_sta_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    if (!esp_sntp_enabled())
        return;
    esp_sntp_restart();
    ESP_LOGI(TAG, "SNTP restarted after STA got IP");
}

static esp_err_t register_sta_got_ip_handler_once(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_ip_handler_registered, &expected, true))
        return ESP_OK;

    esp_err_t err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_sta_got_ip, NULL);
    if (err != ESP_OK) {
        atomic_store(&s_ip_handler_registered, false);
        ESP_LOGW(TAG, "STA got IP handler register failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t time_sync_init(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_err_t handler_err = register_sta_got_ip_handler_once();
    if (esp_sntp_enabled()) {
        esp_sntp_restart();
        ESP_LOGI(TAG, "SNTP already initialized, restart requested");
        return handler_err;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    /* 收到 NTP 包后立即 settimeofday，避免 SMOOTH 模式在数分钟内缓慢追赶导致表盘滞后 */
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_set_sync_interval(SNTP_RESYNC_INTERVAL_MS);
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP: TZ=CST-8, mode=IMMED, resync=%ds, servers=ntp.aliyun.com, cn.pool.ntp.org",
             SNTP_RESYNC_INTERVAL_MS / 1000);
    return handler_err;
}

bool time_sync_is_synced(void)
{
    return s_synced;
}

bool time_sync_get_local(struct tm *out)
{
    if (!s_synced || !out) return false;
    time_t now = time(NULL);
    localtime_r(&now, out);
    return true;
}

bool time_sync_system_time_valid(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    time_t now = time(NULL);
    if (now < 1704067200) /* 2024-01-01 00:00:00 UTC */
        return false;

    struct tm t;
    localtime_r(&now, &t);
    int year = t.tm_year + 1900;
    return year >= 2024 && year <= 2100;
}

bool time_sync_get_local_relaxed(struct tm *out)
{
    if (!out)
        return false;
    if (time_sync_get_local(out))
        return true;
    if (!time_sync_system_time_valid())
        return false;
    time_t now = time(NULL);
    localtime_r(&now, out);
    return true;
}

bool time_sync_get_str(char *buf, size_t len)
{
    if (!s_synced || !buf || len == 0) return false;
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &t);
    return true;
}
