#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define COUNTDOWN_MAX_ITEMS 3
#define COUNTDOWN_TITLE_LEN 48

typedef struct {
    char     title[COUNTDOWN_TITLE_LEN];
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    bool     active;
} countdown_item_t;

typedef struct {
    bool             enabled;
    uint8_t          count;
    countdown_item_t items[COUNTDOWN_MAX_ITEMS];
} countdown_config_t;

esp_err_t countdown_init(void);
esp_err_t countdown_show(void);

esp_err_t countdown_get_config(countdown_config_t *out);
esp_err_t countdown_set_config(const countdown_config_t *cfg);
