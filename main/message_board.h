#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define MSG_MAX_LEN  500

typedef enum {
    MSG_ALIGN_LEFT   = 0,
    MSG_ALIGN_CENTER = 1,
} msg_align_t;

typedef struct {
    char        text[MSG_MAX_LEN + 1];
    uint8_t     font_size;     /* 0=1x(16px), 1=2x(32px), 2=3x(48px), 3=4x(64px), 4=5x(80px) */
    msg_align_t align;
    uint8_t     color;         /* 0 = black, 1 = red */
    int16_t     x_offset;     /* pixel offset from computed position */
    int16_t     y_offset;
} msg_config_t;

esp_err_t message_board_init(void);
esp_err_t message_board_get_config(msg_config_t *out);
esp_err_t message_board_set_config(const msg_config_t *cfg);
esp_err_t message_board_show(void);
esp_err_t message_board_show_queued(unsigned *out_epoch);
void message_board_wait_idle(void);
