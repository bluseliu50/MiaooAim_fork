#pragma once

#include <stdbool.h>

/* Build stable device names from the WiFi STA MAC suffix.
 * Call after nvs_flash_init() and before wifi_manager_init(). */
void device_identity_init(void);

const char *device_identity_get_ap_ssid(void);
const char *device_identity_get_ap_password(void);
bool device_identity_recovery_ap_mode(void);

/* mDNS host name without the .local suffix, for example epd2e5c78. */
const char *device_identity_get_mdns_hostname(void);
/* mDNS service instance display name. */
const char *device_identity_get_mdns_instance(void);
/* Uppercase six-character device suffix, for example 2E5C78. */
const char *device_identity_get_id6_upper(void);
