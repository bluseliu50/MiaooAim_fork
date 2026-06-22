#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

/**
 * Start SNTP time synchronization.
 * Configures timezone to CST-8 (China Standard Time) and
 * begins periodic sync with NTP servers.
 * Safe to call even if not connected — sync will happen
 * once network is available.
 */
esp_err_t time_sync_init(void);

/** true if time has been synchronized at least once. */
bool time_sync_is_synced(void);

/** Get current local time. Returns false if time not yet synced. */
bool time_sync_get_local(struct tm *out);

/** true if the system clock is plausible even if this boot has not SNTP-synced. */
bool time_sync_system_time_valid(void);

/**
 * Get local time when it is plausible.
 * This accepts RTC/system time kept across deep sleep so offline quick-refresh
 * paths do not start WiFi only because the in-RAM SNTP flag was cleared.
 */
bool time_sync_get_local_relaxed(struct tm *out);

/** Get formatted time string (e.g. "2026-03-15 01:43"). */
bool time_sync_get_str(char *buf, size_t len);
