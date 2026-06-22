#pragma once

#include "nvs.h"

/**
 * Rate-limited nvs_commit: if the same NVS handle's namespace was committed
 * less than NVS_THROTTLE_MS ago, the commit is deferred to a background
 * timer. This protects Flash from excessive writes when the web UI
 * auto-saves frequently.
 *
 * Call nvs_utils_init() once at startup.
 * Use nvs_throttled_commit(h) as a drop-in for nvs_commit(h).
 */

#define NVS_THROTTLE_MS 5000

esp_err_t nvs_utils_init(void);

/**
 * Drop-in replacement for nvs_commit(). Defers the actual Flash write
 * if the same namespace was written recently.
 */
esp_err_t nvs_throttled_commit(nvs_handle_t handle);

/**
 * Force-flush all pending deferred commits (call before deep sleep).
 */
void nvs_flush_all(void);
