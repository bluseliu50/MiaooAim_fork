#include "display_mode.h"
#include "display_policy.h"
#include "epd.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_utils.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "dmode";

#define PREFS_NVS_NS "dmode"
#define PREFS_NVS_KEY "prefs_v1"

static display_mode_entry_t s_modes[DISPLAY_MODE_MAX];
static int s_count;
static atomic_int s_active_mode = ATOMIC_VAR_INIT(-1);

/* Per-mode prefs: enabled flags + custom cycle order.
 * s_order[i] holds the display mode index at cycle position i.
 * By default every mode is enabled and ordered by registration. */
static bool s_enabled[DISPLAY_MODE_MAX];
static int s_order[DISPLAY_MODE_MAX];
static bool s_prefs_loaded;

int display_mode_register(const display_mode_entry_t *entry) {
  if (!entry || !entry->name || !entry->show)
    return -1;
  if (s_count >= DISPLAY_MODE_MAX) {
    ESP_LOGE(TAG, "mode registry full (%d)", DISPLAY_MODE_MAX);
    return -1;
  }
  s_modes[s_count] = *entry;
  ESP_LOGI(TAG, "[%d] %s (%s)", s_count, entry->name, entry->label_cn);
  return s_count++;
}

int display_mode_count(void) { return s_count; }

const display_mode_entry_t *display_mode_get(int idx) {
  if (idx < 0 || idx >= s_count)
    return NULL;
  return &s_modes[idx];
}

static esp_err_t display_mode_show_internal(int idx, bool new_request,
                                            unsigned *epoch_out) {
  const display_mode_entry_t *m = display_mode_get(idx);
  if (!m)
    return ESP_ERR_INVALID_ARG;
  int prev = display_mode_active();
  unsigned epoch;
  if (new_request) {
    epoch = display_policy_begin_manual_display();
  } else {
    epd_request_full_refresh_next();
    epoch = display_policy_display_epoch();
  }
  if (epoch_out)
    *epoch_out = epoch;
  display_mode_set_active(idx);
  esp_err_t err = m->show();
  if (err != ESP_OK)
    display_mode_set_active(prev);
  return err;
}

esp_err_t display_mode_show(int idx) {
  return display_mode_show_internal(idx, false, NULL);
}

esp_err_t display_mode_show_request(int idx, unsigned *epoch_out) {
  return display_mode_show_internal(idx, true, epoch_out);
}

const char *display_mode_name(int idx) {
  const display_mode_entry_t *m = display_mode_get(idx);
  return m ? m->name : "?";
}

const char *display_mode_label(int idx) {
  const display_mode_entry_t *m = display_mode_get(idx);
  return m ? m->label_cn : "?";
}

int display_mode_active(void) { return atomic_load(&s_active_mode); }

void display_mode_set_active(int idx) {
  if (idx < 0 || idx >= s_count) {
    atomic_store(&s_active_mode, -1);
    return;
  }
  atomic_store(&s_active_mode, idx);
}

/* ── Mode preferences (enable + cycle order) ─────────────────────── */

/* Layout persisted in NVS as a blob of int8:
 *   byte 0:        version (=1)
 *   byte 1:        count (= s_count)
 *   byte 2..1+n:   order[i] (display mode index at cycle position i)
 *   byte 2+n..1+2n: enabled[i] (0/1)
 * Only registered modes are stored; extra bytes are ignored on load. */

static void prefs_apply_defaults(void) {
  for (int i = 0; i < s_count; i++) {
    s_enabled[i] = true;
    s_order[i] = i;
  }
  for (int i = s_count; i < DISPLAY_MODE_MAX; i++) {
    s_enabled[i] = false;
    s_order[i] = 0;
  }
}

void display_mode_prefs_load(void) {
  prefs_apply_defaults();

  nvs_handle_t h;
  if (nvs_open(PREFS_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
    s_prefs_loaded = true;
    return;
  }

  size_t len = 0;
  if (nvs_get_blob(h, PREFS_NVS_KEY, NULL, &len) == ESP_OK && len >= 2) {
    uint8_t buf[1 + 2 * DISPLAY_MODE_MAX];
    if (len > sizeof(buf))
      len = sizeof(buf);
    if (nvs_get_blob(h, PREFS_NVS_KEY, buf, &len) == ESP_OK && len >= 2 &&
        buf[0] == 1) {
      int n = buf[1];
      if (n > s_count)
        n = s_count; /* modes may have been added since */
      if (2 + 2 * n <= (int)len) {
        bool seen[DISPLAY_MODE_MAX] = {false};
        int valid = 0;
        for (int i = 0; i < n; i++) {
          int m = buf[2 + i];
          if (m >= 0 && m < s_count && !seen[m]) {
            seen[m] = true;
            s_order[valid++] = m;
            s_enabled[m] = (buf[2 + n + i] != 0);
          }
        }
        /* append any modes missing from the stored order (new modes) */
        for (int m = 0; m < s_count; m++)
          if (!seen[m]) {
            s_order[valid++] = m;
            s_enabled[m] = true;
          }
      }
    }
  }
  nvs_close(h);
  s_prefs_loaded = true;
  ESP_LOGI(TAG, "prefs loaded: %d modes", s_count);
}

void display_mode_prefs_save(void) {
  if (!s_prefs_loaded || s_count <= 0)
    return;
  uint8_t buf[1 + 2 * DISPLAY_MODE_MAX];
  buf[0] = 1; /* version */
  buf[1] = (uint8_t)s_count;
  for (int i = 0; i < s_count; i++)
    buf[2 + i] = (uint8_t)s_order[i];
  for (int i = 0; i < s_count; i++)
    buf[2 + s_count + i] = s_enabled[i] ? 1 : 0;

  nvs_handle_t h;
  if (nvs_open(PREFS_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
    return;
  nvs_set_blob(h, PREFS_NVS_KEY, buf, (size_t)(2 + 2 * s_count));
  nvs_throttled_commit(h);
  nvs_close(h);
}

bool display_mode_is_enabled(int index) {
  if (index < 0 || index >= s_count)
    return false;
  return s_enabled[index];
}

void display_mode_set_enabled(int index, bool enabled) {
  if (index < 0 || index >= s_count)
    return;
  s_enabled[index] = enabled;
}

int display_mode_get_order(int *out_order, int max) {
  if (!out_order || max < s_count)
    return 0;
  for (int i = 0; i < s_count; i++)
    out_order[i] = s_order[i];
  return s_count;
}

void display_mode_set_order(const int *order, int order_len) {
  if (!order || order_len <= 0) {
    prefs_apply_defaults();
    return;
  }
  bool seen[DISPLAY_MODE_MAX] = {false};
  int valid = 0;
  for (int i = 0; i < order_len && valid < s_count; i++) {
    int m = order[i];
    if (m >= 0 && m < s_count && !seen[m]) {
      seen[m] = true;
      s_order[valid++] = m;
    }
  }
  /* append any modes not in the provided order */
  for (int m = 0; m < s_count; m++)
    if (!seen[m])
      s_order[valid++] = m;
}

int display_mode_next_enabled(int from) {
  if (s_count <= 0 || from < 0 || from >= s_count)
    return from;
  int pos = -1;
  for (int i = 0; i < s_count; i++)
    if (s_order[i] == from) {
      pos = i;
      break;
    }
  if (pos < 0)
    pos = 0;
  for (int step = 1; step <= s_count; step++) {
    int p = (pos + step) % s_count;
    int m = s_order[p];
    if (s_enabled[m])
      return m;
  }
  return from; /* nothing else enabled */
}

int display_mode_prev_enabled(int from) {
  if (s_count <= 0 || from < 0 || from >= s_count)
    return from;
  int pos = -1;
  for (int i = 0; i < s_count; i++)
    if (s_order[i] == from) {
      pos = i;
      break;
    }
  if (pos < 0)
    pos = 0;
  for (int step = 1; step <= s_count; step++) {
    int p = (pos - step + s_count) % s_count;
    int m = s_order[p];
    if (s_enabled[m])
      return m;
  }
  return from;
}
