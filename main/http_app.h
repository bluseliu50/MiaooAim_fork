#pragma once

#include "esp_err.h"

typedef struct {
    const char *mount_path;  // "/spiffs"
    const char *upload_path; // "/spiffs/image.bin"
} http_app_config_t;

esp_err_t http_app_start(const http_app_config_t *cfg);

