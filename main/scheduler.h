#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SLIDESHOW_SEQ    = 0,
    SLIDESHOW_RANDOM = 1,
} slideshow_mode_t;

typedef struct {
    bool              enabled;
    uint32_t          interval_sec;   /* 300 ~ 86400 */
    slideshow_mode_t  mode;
    bool              clock_overlay;  /* show time in corner */
} slideshow_config_t;

esp_err_t   scheduler_init(void);
void        scheduler_boot_complete(void);
esp_err_t   scheduler_get_config(slideshow_config_t *out);
esp_err_t   scheduler_set_config(const slideshow_config_t *cfg);
void        scheduler_notify_manual_show(void);
/** 记录刚显示过的图片，并把顺序轮播的下一张同步到它后面。 */
void        scheduler_set_current_image_name(const char *basename);
const char *scheduler_get_current_image(void);

/** Show next gallery image on EPD (for button-triggered display). */
esp_err_t   scheduler_show_next_image(void);
