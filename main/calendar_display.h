#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t calendar_display_init(void);
esp_err_t calendar_display_show(int year, int month);
esp_err_t calendar_display_show_current(void);
esp_err_t calendar_display_toggle_style(void);
bool calendar_display_style_uses_weather(void);
bool calendar_display_needs_midnight_refresh(void);

/* Block until the async calendar render task has finished. */
void calendar_display_wait_render_idle(void);
