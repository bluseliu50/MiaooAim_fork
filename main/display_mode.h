#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define DISPLAY_MODE_MAX 12

/* Keep in sync with app_main.c registration order. */
typedef enum {
  DISPLAY_MODE_CLOCK = 0,
  DISPLAY_MODE_CALENDAR,
  DISPLAY_MODE_TIMETABLE,
  DISPLAY_MODE_WEATHER,
  DISPLAY_MODE_SLIDESHOW,
  DISPLAY_MODE_TODO,
  DISPLAY_MODE_COUNTDOWN,
  DISPLAY_MODE_CODEX_QUOTA,
} display_mode_index_t;

typedef esp_err_t (*display_mode_show_fn)(void);

typedef struct {
  const char *name;     /* "clock", "calendar", etc. */
  const char *label_cn; /* "时钟", "日历", etc. */
  display_mode_show_fn show;
} display_mode_entry_t;

/**
 * Register a display mode. Call in each module's init or from app_main.
 * Returns the index (0-based) or -1 on error.
 */
int display_mode_register(const display_mode_entry_t *entry);

int display_mode_count(void);
const display_mode_entry_t *display_mode_get(int index);
esp_err_t display_mode_show(int index);
esp_err_t display_mode_show_request(int index, unsigned *epoch_out);
const char *display_mode_name(int index);
const char *display_mode_label(int index);

/** Last successfully displayed mode, or -1 before any mode takes ownership. */
int display_mode_active(void);
void display_mode_set_active(int index);

/**
 * Mode preferences: per-mode enable + a custom cycle order.
 *
 * After registration is complete, call display_mode_prefs_load() once to
 * hydrate from NVS. The web UI then reorders/toggles via
 * display_mode_prefs_get()/set(). Button cycling uses next/prev_enabled to
 * skip disabled modes.
 */

/** Returns true if the mode is enabled for button cycling. */
bool display_mode_is_enabled(int index);

/** Toggle a mode on/off for cycling (disabled modes are skipped by buttons). */
void display_mode_set_enabled(int index, bool enabled);

/**
 * Custom cycle order. `order[i]` is the display mode index at position i.
 * Returns the number of entries written into `out_order` (<= DISPLAY_MODE_MAX),
 * or 0 if the array is too small.
 */
int display_mode_get_order(int *out_order, int max);

/**
 * Set a new cycle order. `order_len` entries from `order` are copied; entries
 * may be omitted (disabled modes) and are appended in registration order.
 */
void display_mode_set_order(const int *order, int order_len);

/** Persist current prefs to NVS (throttled). */
void display_mode_prefs_save(void);

/** Load prefs from NVS into RAM (call after all modes are registered). */
void display_mode_prefs_load(void);

/**
 * Return the next enabled mode index after `from` in cycle order.
 * Returns `from` unchanged if no other enabled mode exists.
 */
int display_mode_next_enabled(int from);

/** Return the previous enabled mode index before `from` in cycle order. */
int display_mode_prev_enabled(int from);
