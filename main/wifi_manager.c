#include "wifi_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "wifi_mgr";

#define NVS_NAMESPACE   "wifi_cfg"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"
#define NVS_KEY_COUNT   "count"

#define STA_CONNECT_TIMEOUT_MS  15000
#define STA_MAX_RETRY           3

#define BIT_CONNECTED    BIT0
#define BIT_FAIL         BIT1
#define BIT_STA_STARTED  BIT2
#define BIT_RECONNECT    BIT3

static EventGroupHandle_t s_event_group;
static wifi_mgr_mode_t    s_mode = WIFI_MGR_MODE_NONE;
static bool               s_sta_connected = false;
static char               s_sta_ip[20];
static char               s_sta_ssid[33];
static int                s_retry_count;
static bool               s_sta_connecting = false;
static esp_netif_t       *s_ap_netif;
static esp_netif_t       *s_sta_netif;
static char               s_ap_ssid[33];
static char               s_ap_pass[65];
static portMUX_TYPE       s_sta_mux = portMUX_INITIALIZER_UNLOCKED;

static TimerHandle_t      s_reconnect_timer;
static TaskHandle_t       s_reconnect_task;
static int                s_reconnect_backoff_s = 10;
#define RECONNECT_MIN_S   10
#define RECONNECT_MAX_S   300
#define RECONNECT_TASK_STACK 4096
#define RECONNECT_TASK_PRIO  4

/* ── NVS helpers ──────────────────────────────────────────────── */

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_cred_t;

static void cred_key(char *dst, size_t dst_len, const char *prefix, int idx)
{
    snprintf(dst, dst_len, "%s%d", prefix, idx);
}

static bool nvs_load_legacy_credentials(nvs_handle_t h, char *ssid, size_t ssid_len,
                                        char *pass, size_t pass_len)
{
    return (nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK &&
            nvs_get_str(h, NVS_KEY_PASS, pass, &pass_len) == ESP_OK &&
            ssid[0] != '\0');
}

static int nvs_load_credentials_list(wifi_cred_t *creds, int max_records)
{
    if (!creds || max_records <= 0)
        return 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return 0;

    uint8_t stored_count = 0;
    (void)nvs_get_u8(h, NVS_KEY_COUNT, &stored_count);
    if (stored_count > WIFI_MGR_MAX_CREDENTIALS)
        stored_count = WIFI_MGR_MAX_CREDENTIALS;

    int count = 0;
    for (int i = 0; i < stored_count && count < max_records; i++) {
        char ssid_key[8];
        char pass_key[8];
        cred_key(ssid_key, sizeof(ssid_key), "ssid", i);
        cred_key(pass_key, sizeof(pass_key), "pass", i);

        size_t ssid_len = sizeof(creds[count].ssid);
        size_t pass_len = sizeof(creds[count].pass);
        creds[count].ssid[0] = '\0';
        creds[count].pass[0] = '\0';

        if (nvs_get_str(h, ssid_key, creds[count].ssid, &ssid_len) == ESP_OK &&
            nvs_get_str(h, pass_key, creds[count].pass, &pass_len) == ESP_OK &&
            creds[count].ssid[0] != '\0') {
            count++;
        }
    }

    if (count == 0) {
        size_t ssid_len = sizeof(creds[0].ssid);
        size_t pass_len = sizeof(creds[0].pass);
        creds[0].ssid[0] = '\0';
        creds[0].pass[0] = '\0';
        if (nvs_load_legacy_credentials(h, creds[0].ssid, ssid_len,
                                        creds[0].pass, pass_len)) {
            count = 1;
        }
    }

    nvs_close(h);
    return count;
}

static esp_err_t nvs_write_credentials_list(const wifi_cred_t *creds, int count)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    if (count < 0)
        count = 0;
    if (count > WIFI_MGR_MAX_CREDENTIALS)
        count = WIFI_MGR_MAX_CREDENTIALS;

    for (int i = 0; i < WIFI_MGR_MAX_CREDENTIALS; i++) {
        char ssid_key[8];
        char pass_key[8];
        cred_key(ssid_key, sizeof(ssid_key), "ssid", i);
        cred_key(pass_key, sizeof(pass_key), "pass", i);
        nvs_erase_key(h, ssid_key);
        nvs_erase_key(h, pass_key);
    }

    for (int i = 0; err == ESP_OK && i < count; i++) {
        char ssid_key[8];
        char pass_key[8];
        cred_key(ssid_key, sizeof(ssid_key), "ssid", i);
        cred_key(pass_key, sizeof(pass_key), "pass", i);
        err = nvs_set_str(h, ssid_key, creds[i].ssid);
        if (err == ESP_OK)
            err = nvs_set_str(h, pass_key, creds[i].pass);
    }

    if (err == ESP_OK)
        err = nvs_set_u8(h, NVS_KEY_COUNT, (uint8_t)count);

    /* Keep old keys current so downgrade firmware can still connect. */
    if (err == ESP_OK && count > 0) {
        err = nvs_set_str(h, NVS_KEY_SSID, creds[0].ssid);
        if (err == ESP_OK)
            err = nvs_set_str(h, NVS_KEY_PASS, creds[0].pass);
    } else if (err == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_SSID);
        nvs_erase_key(h, NVS_KEY_PASS);
    }

    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_credentials(const char *ssid, const char *pass)
{
    wifi_cred_t old[WIFI_MGR_MAX_CREDENTIALS] = {0};
    wifi_cred_t next[WIFI_MGR_MAX_CREDENTIALS] = {0};
    int old_count = nvs_load_credentials_list(old, WIFI_MGR_MAX_CREDENTIALS);
    int next_count = 0;

    strlcpy(next[next_count].ssid, ssid, sizeof(next[next_count].ssid));
    strlcpy(next[next_count].pass, pass ? pass : "", sizeof(next[next_count].pass));
    next_count++;

    for (int i = 0; i < old_count && next_count < WIFI_MGR_MAX_CREDENTIALS; i++) {
        if (strcmp(old[i].ssid, ssid) == 0)
            continue;
        strlcpy(next[next_count].ssid, old[i].ssid, sizeof(next[next_count].ssid));
        strlcpy(next[next_count].pass, old[i].pass, sizeof(next[next_count].pass));
        next_count++;
    }

    return nvs_write_credentials_list(next, next_count);
}

static void nvs_erase_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_SSID);
        nvs_erase_key(h, NVS_KEY_PASS);
        nvs_erase_key(h, NVS_KEY_COUNT);
        for (int i = 0; i < WIFI_MGR_MAX_CREDENTIALS; i++) {
            char ssid_key[8];
            char pass_key[8];
            cred_key(ssid_key, sizeof(ssid_key), "ssid", i);
            cred_key(pass_key, sizeof(pass_key), "pass", i);
            nvs_erase_key(h, ssid_key);
            nvs_erase_key(h, pass_key);
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ── Background reconnect timer ───────────────────────────────── */

static void reconnect_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (s_event_group)
        xEventGroupSetBits(s_event_group, BIT_RECONNECT);
}

static void start_reconnect_timer(void);
static bool try_saved_credentials(const char *reason, bool sta_only_mode);

static void reconnect_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_event_group, BIT_RECONNECT,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        if (s_sta_connected) {
            s_reconnect_backoff_s = RECONNECT_MIN_S;
            continue;
        }

        ESP_LOGI(TAG, "Background reconnect attempt (backoff %ds)",
                 s_reconnect_backoff_s);
        (void)try_saved_credentials("background reconnect", s_ap_netif == NULL);

        int next = s_reconnect_backoff_s * 2;
        if (next > RECONNECT_MAX_S)
            next = RECONNECT_MAX_S;
        s_reconnect_backoff_s = next;
    }
}

static esp_err_t ensure_reconnect_task(void)
{
    if (s_reconnect_task)
        return ESP_OK;

    BaseType_t ok = xTaskCreate(reconnect_task, "wifi_rc_task",
                                RECONNECT_TASK_STACK, NULL,
                                RECONNECT_TASK_PRIO, &s_reconnect_task);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

static void start_reconnect_timer(void)
{
    if (!s_reconnect_timer) return;
    xTimerChangePeriod(s_reconnect_timer,
                       pdMS_TO_TICKS(s_reconnect_backoff_s * 1000),
                       0);
    xTimerStart(s_reconnect_timer, 0);
}

static void stop_reconnect_timer(void)
{
    if (s_reconnect_timer)
        xTimerStop(s_reconnect_timer, 0);
    s_reconnect_backoff_s = RECONNECT_MIN_S;
}

/* ── Event handler ────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA interface ready (PHY initialized)");
            xEventGroupSetBits(s_event_group, BIT_STA_STARTED);
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            portENTER_CRITICAL(&s_sta_mux);
            s_sta_connected = false;
            s_sta_ip[0] = '\0';
            portEXIT_CRITICAL(&s_sta_mux);
            if (!s_sta_connecting) {
                start_reconnect_timer();
                break;
            }
            if (s_retry_count < STA_MAX_RETRY) {
                s_retry_count++;
                ESP_LOGI(TAG, "STA disconnected, retry %d/%d",
                         s_retry_count, STA_MAX_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "STA connection failed after %d retries", STA_MAX_RETRY);
                s_sta_connecting = false;
                xEventGroupSetBits(s_event_group, BIT_FAIL);
                start_reconnect_timer();
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "A station joined the AP");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "A station left the AP");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        portENTER_CRITICAL(&s_sta_mux);
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_sta_connected = true;
        portEXIT_CRITICAL(&s_sta_mux);
        ESP_LOGI(TAG, "STA got IP: %s", s_sta_ip);
        s_retry_count = 0;
        s_sta_connecting = false;
        stop_reconnect_timer();
        xEventGroupSetBits(s_event_group, BIT_CONNECTED);
    }
}

/* ── Internal: start AP ───────────────────────────────────────── */

static void configure_ap(void)
{
    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    strlcpy((char *)ap_cfg.ap.password, s_ap_pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = s_ap_pass[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    ap_cfg.ap.beacon_interval = 100;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
}

/* ── Internal: attempt STA connection ─────────────────────────── */

static esp_err_t validate_sta_credentials(const char *ssid, const char *pass)
{
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    size_t pass_len = pass ? strlen(pass) : 0;
    if (ssid_len == 0 || ssid_len > 32)
        return ESP_ERR_INVALID_ARG;
    if (pass_len > 0 && pass_len < 8)
        return ESP_ERR_INVALID_ARG;
    if (pass_len > 64)
        return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static bool try_sta_connect(const char *ssid, const char *pass)
{
    if (validate_sta_credentials(ssid, pass) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid STA credentials (ssid_len=%u, pass_len=%u)",
                 (unsigned)(ssid ? strlen(ssid) : 0),
                 (unsigned)(pass ? strlen(pass) : 0));
        return false;
    }

    s_retry_count = 0;
    portENTER_CRITICAL(&s_sta_mux);
    s_sta_connected = false;
    s_sta_ip[0] = '\0';
    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
    portEXIT_CRITICAL(&s_sta_mux);
    xEventGroupClearBits(s_event_group, BIT_CONNECTED | BIT_FAIL);

    /* WiFi is always running in APSTA mode at this point. */
    esp_wifi_disconnect();

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(err));
        s_sta_connecting = false;
        s_mode = s_ap_netif ? WIFI_MGR_MODE_AP : WIFI_MGR_MODE_NONE;
        return false;
    }

    s_sta_connecting = true;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        s_sta_connecting = false;
        s_mode = s_ap_netif ? WIFI_MGR_MODE_AP : WIFI_MGR_MODE_NONE;
        return false;
    }

    ESP_LOGI(TAG, "Connecting to \"%s\" ...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           BIT_CONNECTED | BIT_FAIL,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));

    if (bits & BIT_CONNECTED) {
        s_sta_connecting = false;
        s_mode = WIFI_MGR_MODE_APSTA;
        ESP_LOGI(TAG, "STA connected to \"%s\", IP=%s (AP still active)", ssid, s_sta_ip);
        return true;
    }

    ESP_LOGW(TAG, "STA failed to connect to \"%s\", staying in AP mode", ssid);
    s_sta_connecting = false;
    esp_wifi_disconnect();
    s_mode = s_ap_netif ? WIFI_MGR_MODE_AP : WIFI_MGR_MODE_NONE;
    return false;
}

/* ── Public API ───────────────────────────────────────────────── */

static bool try_saved_credentials(const char *reason, bool sta_only_mode)
{
    wifi_cred_t creds[WIFI_MGR_MAX_CREDENTIALS] = {0};
    int count = nvs_load_credentials_list(creds, WIFI_MGR_MAX_CREDENTIALS);
    if (count <= 0) {
        ESP_LOGI(TAG, "No stored WiFi credentials, %s", reason);
        return false;
    }

    ESP_LOGI(TAG, "Found %d stored WiFi credential(s), %s", count, reason);
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "Trying stored WiFi %d/%d: \"%s\"", i + 1, count, creds[i].ssid);
        if (try_sta_connect(creds[i].ssid, creds[i].pass)) {
            if (sta_only_mode)
                s_mode = WIFI_MGR_MODE_STA;
            if (i > 0) {
                esp_err_t save_err = nvs_save_credentials(creds[i].ssid, creds[i].pass);
                if (save_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to promote working WiFi credential: %s",
                             esp_err_to_name(save_err));
                }
            }
            return true;
        }
    }

    ESP_LOGW(TAG, "All stored WiFi credentials failed");
    return false;
}

esp_err_t wifi_manager_init(const char *ap_ssid, const char *ap_pass)
{
    s_event_group = xEventGroupCreate();
    if (!s_event_group)
        return ESP_ERR_NO_MEM;

    s_reconnect_timer = xTimerCreate("wifi_rc", pdMS_TO_TICKS(RECONNECT_MIN_S * 1000),
                                     pdFALSE, NULL, reconnect_timer_cb);
    if (!s_reconnect_timer || ensure_reconnect_task() != ESP_OK)
        return ESP_ERR_NO_MEM;

    strlcpy(s_ap_ssid, ap_ssid ? ap_ssid : "ESP32_EPD", sizeof(s_ap_ssid));
    strlcpy(s_ap_pass, ap_pass ? ap_pass : "", sizeof(s_ap_pass));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    (void)s_ap_netif;
    (void)s_sta_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    /* Always start WiFi in APSTA mode first. This triggers PHY init
       (which can take 10-15 seconds on first boot). We wait for
       WIFI_EVENT_STA_START to confirm the radio is ready. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    configure_ap();
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi starting, waiting for PHY init...");

    EventBits_t start_bits = xEventGroupWaitBits(
        s_event_group, BIT_STA_STARTED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));
    if (!(start_bits & BIT_STA_STARTED)) {
        ESP_LOGW(TAG, "WiFi STA interface start timed out");
    }

    s_mode = WIFI_MGR_MODE_AP;
    ESP_LOGI(TAG, "AP mode started: SSID=%s (scan-ready)", s_ap_ssid);

    /* Now WiFi is fully running. Try all saved STA credentials if present. */
    if (try_saved_credentials("attempting STA", false)) {
        return ESP_OK;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_init_sta_only(void)
{
    s_event_group = xEventGroupCreate();
    if (!s_event_group)
        return ESP_ERR_NO_MEM;

    s_reconnect_timer = xTimerCreate("wifi_rc", pdMS_TO_TICKS(RECONNECT_MIN_S * 1000),
                                     pdFALSE, NULL, reconnect_timer_cb);
    if (!s_reconnect_timer || ensure_reconnect_task() != ESP_OK)
        return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = NULL;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    (void)s_sta_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA-only WiFi starting, waiting for PHY init...");

    EventBits_t start_bits = xEventGroupWaitBits(
        s_event_group, BIT_STA_STARTED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));
    if (!(start_bits & BIT_STA_STARTED))
        ESP_LOGW(TAG, "WiFi STA interface start timed out");

    s_mode = WIFI_MGR_MODE_STA;
    wifi_cred_t creds[WIFI_MGR_MAX_CREDENTIALS] = {0};
    int saved_count = nvs_load_credentials_list(creds, WIFI_MGR_MAX_CREDENTIALS);
    if (saved_count <= 0) {
        ESP_LOGW(TAG, "STA-only requested but no stored WiFi credentials");
        return ESP_ERR_NOT_FOUND;
    }

    if (!try_saved_credentials("STA-only connect", true))
        return ESP_FAIL;
    s_mode = WIFI_MGR_MODE_STA;
    return ESP_OK;
}

const char *wifi_manager_get_ap_ssid(void)
{
    return s_ap_ssid[0] ? s_ap_ssid : "ESP32_EPD";
}

void wifi_manager_set_sta_hostname(const char *hostname)
{
    if (!hostname || !hostname[0] || !s_sta_netif)
        return;
    esp_err_t err = esp_netif_set_hostname(s_sta_netif, hostname);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "esp_netif_set_hostname: %s", esp_err_to_name(err));
}

wifi_mgr_mode_t wifi_manager_get_mode(void)
{
    return s_mode;
}

bool wifi_manager_sta_connected(void)
{
    portENTER_CRITICAL(&s_sta_mux);
    bool v = s_sta_connected;
    portEXIT_CRITICAL(&s_sta_mux);
    return v;
}

const char *wifi_manager_get_sta_ip(void)
{
    return s_sta_ip;  /* 字符串由调用方在短暂的原子窗口内使用，snprintf 到局部 buf 即可 */
}

const char *wifi_manager_get_sta_ssid(void)
{
    return s_sta_ssid;
}

int wifi_manager_get_saved_credentials(wifi_mgr_saved_cred_t *out, int max_records)
{
    if (!out || max_records <= 0)
        return 0;

    wifi_cred_t creds[WIFI_MGR_MAX_CREDENTIALS] = {0};
    int count = nvs_load_credentials_list(creds, WIFI_MGR_MAX_CREDENTIALS);
    if (count > max_records)
        count = max_records;

    for (int i = 0; i < count; i++) {
        strlcpy(out[i].ssid, creds[i].ssid, sizeof(out[i].ssid));
    }
    return count;
}

int wifi_manager_scan(wifi_mgr_ap_record_t *out, int max_records)
{
    if (!out || max_records <= 0) return 0;

    /* We always run in APSTA mode, so STA interface is available for
       scanning without any mode switch (which would drop AP clients). */
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan start failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        esp_wifi_scan_get_ap_records(&ap_count, NULL);
        return 0;
    }

    uint16_t fetch = (ap_count > (uint16_t)max_records) ? (uint16_t)max_records : ap_count;
    wifi_ap_record_t *records = malloc(fetch * sizeof(wifi_ap_record_t));
    if (!records) {
        esp_wifi_scan_get_ap_records(&fetch, NULL);
        return 0;
    }

    esp_wifi_scan_get_ap_records(&fetch, records);

    int count = 0;
    for (int i = 0; i < fetch; i++) {
        if (records[i].ssid[0] == '\0') continue;
        bool dup = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(out[j].ssid, (char *)records[i].ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        strlcpy(out[count].ssid, (char *)records[i].ssid, sizeof(out[count].ssid));
        out[count].rssi = records[i].rssi;
        out[count].authmode = (uint8_t)records[i].authmode;
        count++;
        if (count >= max_records) break;
    }
    free(records);

    ESP_LOGI(TAG, "Scan complete: %d unique APs found", count);
    return count;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    const char *pass = password ? password : "";

    esp_err_t err = validate_sta_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Rejected invalid WiFi credentials");
        return err;
    }

    bool ok = try_sta_connect(ssid, pass);
    if (!ok)
        return ESP_FAIL;

    err = nvs_save_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Connected but failed to save credentials: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t wifi_manager_forget(void)
{
    nvs_erase_credentials();

    portENTER_CRITICAL(&s_sta_mux);
    s_sta_ssid[0] = '\0';
    s_sta_ip[0] = '\0';
    s_sta_connected = false;
    portEXIT_CRITICAL(&s_sta_mux);

    if (s_mode == WIFI_MGR_MODE_APSTA) {
        esp_wifi_disconnect();
        s_mode = WIFI_MGR_MODE_AP;
    }

    ESP_LOGI(TAG, "Credentials erased, AP mode active");
    return ESP_OK;
}
