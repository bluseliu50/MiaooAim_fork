#pragma once

#include "esp_err.h"

/**
 * Three physical buttons (TS35CA, active-HIGH with 10k external pull-down):
 *   SW3 = GPIO 9   (left / prev)
 *   SW4 = GPIO 46  (middle / refresh)
 *   SW5 = GPIO 3   (right / next)
 *
 * Display mode cycle:  Clock -> Calendar -> Timetable -> Weather -> Clock ...
 *   Left   (SW3) : previous mode
 *   Middle (SW4) : refresh current mode
 *   Right  (SW5) : next mode
 */

esp_err_t button_init(void);

int  button_get_current_mode(void);
void button_set_current_mode(int mode);
