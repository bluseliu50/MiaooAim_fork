#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool enabled;
    int  interval_min;     /* wake-up interval in minutes (1-1440) */
    int  idle_timeout_s;   /* inactivity timeout in seconds (30-600) */
} power_config_t;

/**
 * Load config from NVS and detect wake-up cause.
 * Must be called early in app_main(), after nvs_flash_init().
 */
esp_err_t power_mgr_init(void);

esp_err_t power_mgr_get_config(power_config_t *out);
esp_err_t power_mgr_set_config(const power_config_t *cfg);

/** Reset inactivity timer (call from button press, HTTP request, etc.) */
void power_mgr_reset_activity(void);

/** Mark that an EPD refresh completed; deep sleep will wait for panel settle. */
void power_mgr_note_epd_refresh_complete(void);

/** Start the inactivity-check timer (call after full boot completes) */
esp_err_t power_mgr_arm(void);

/** Configure wake sources and enter deep sleep immediately */
void power_mgr_enter_sleep(void);

/** True if current boot was caused by RTC timer (not button or power-on) */
bool power_mgr_is_timer_wake(void);

/** Save/load display mode index across deep sleep cycles */
void power_mgr_save_mode(int mode_index);
int  power_mgr_load_mode(void);
bool power_mgr_saved_mode_has_marker(void);
