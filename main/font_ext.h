#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fb_render.h"

#define FONT_EXT_COUNT 3
#define FONT_EXT_MAX_GLYPH_BYTES 288
#define FONT_EXT_BASE_PATH "/fontfs/fonts"

typedef struct {
    bool present;
    uint16_t px;
    uint16_t glyph_bytes;
    uint8_t format;
    uint16_t record_bytes;
    uint32_t glyph_count;
    uint32_t file_size;
    char path[40];
} font_ext_info_t;

void font_ext_init(void);
void font_ext_refresh(void);
void font_ext_get_info(font_ext_info_t out[FONT_EXT_COUNT]);

bool font_ext_validate_file(const char *path, int expected_px, font_ext_info_t *out);
const char *font_ext_path_for_px(int px);
bool font_ext_supported_px(int px);

bool font_ext_probe_glyph(uint32_t cp, int scale, int *advance);
bool font_ext_draw_glyph(fb_t *fb, int x, int y, uint32_t cp,
                         fb_color_t c, int scale, int *advance);
bool font_ext_probe_glyph_px(uint32_t cp, int target_px, int *advance);
bool font_ext_draw_glyph_px(fb_t *fb, int x, int y, uint32_t cp,
                            fb_color_t c, int target_px, int *advance);
