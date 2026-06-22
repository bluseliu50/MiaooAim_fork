#include "device_identity.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "dev_id";

#define DEFAULT_AP_PASSWORD "12345678"

static char s_id6_lower[7];
static char s_id6_upper[7];
static char s_ap_ssid[33];
static char s_mdns_host[32];
static char s_instance[48];
static bool s_inited;

void device_identity_init(void)
{
    if (s_inited)
        return;

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGE(TAG, "esp_read_mac failed, using placeholder id");
        strcpy(s_id6_lower, "000000");
        strcpy(s_id6_upper, "000000");
    } else {
        snprintf(s_id6_lower, sizeof(s_id6_lower), "%02x%02x%02x", mac[3], mac[4], mac[5]);
        snprintf(s_id6_upper, sizeof(s_id6_upper), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    }

    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "ESP32_EPD_%s", s_id6_upper);
    snprintf(s_mdns_host, sizeof(s_mdns_host), "epd%s", s_id6_lower);
    snprintf(s_instance, sizeof(s_instance), "EPaper-%s", s_id6_upper);

    s_inited = true;
    ESP_LOGI(TAG, "Device id: AP SSID=%s, mDNS=%s.local, STA MAC ...%s",
             s_ap_ssid, s_mdns_host, s_id6_upper);
}

const char *device_identity_get_ap_ssid(void)
{
    return s_ap_ssid[0] ? s_ap_ssid : "ESP32_EPD";
}

const char *device_identity_get_ap_password(void)
{
    return DEFAULT_AP_PASSWORD;
}

bool device_identity_recovery_ap_mode(void)
{
    return false;
}

const char *device_identity_get_mdns_hostname(void)
{
    return s_mdns_host[0] ? s_mdns_host : "epd";
}

const char *device_identity_get_mdns_instance(void)
{
    return s_instance[0] ? s_instance : "ESP32 E-Paper Display";
}

const char *device_identity_get_id6_upper(void)
{
    return s_id6_upper[0] ? s_id6_upper : "------";
}
