#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t spiffs_mount_init(void);
esp_err_t spiffs_mount_retry(void);
esp_err_t spiffs_mount_format_and_mount(void);
bool spiffs_mount_is_mounted(void);

