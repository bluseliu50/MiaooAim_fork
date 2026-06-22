#pragma once

#include <stdbool.h>

#include "fb_render.h"

typedef enum {
    UI_FRAME_RED_ACCENT = 1 << 0,
    UI_FRAME_THIN       = 1 << 1,
} ui_frame_flags_t;

typedef enum {
    UI_LAYOUT_COMPACT = 0,  /* 2.9" class */
    UI_LAYOUT_42,           /* 400x300 4.2" class */
    UI_LAYOUT_583,          /* 648x480 5.83" class */
    UI_LAYOUT_LARGE,        /* 7.5" / 800x480 class */
} ui_layout_class_t;

typedef enum {
    UI_TEXT_SMALL = 0,
    UI_TEXT_BODY,
    UI_TEXT_LABEL,
    UI_TEXT_TITLE,
    UI_TEXT_VALUE,
} ui_text_role_t;

ui_layout_class_t ui_layout_for(const fb_t *fb);
bool ui_layout_is_42(const fb_t *fb);
bool ui_layout_is_583(const fb_t *fb);
bool ui_layout_is_wide(const fb_t *fb);
int  ui_scale_for(const fb_t *fb);
int  ui_text_width(const char *s, int scale);
int  ui_role_scale(const fb_t *fb, ui_text_role_t role);
int  ui_role_text_width(const fb_t *fb, const char *s, ui_text_role_t role);
int  ui_draw_text_role(fb_t *fb, int x, int y, const char *s,
                       fb_color_t color, ui_text_role_t role);
int  ui_draw_text_role_maxw(fb_t *fb, int x, int y, const char *s,
                            fb_color_t color, ui_text_role_t role, int max_w);
void ui_draw_dotted_hline(fb_t *fb, int x, int y, int w, fb_color_t color, int step);
void ui_draw_dotted_vline(fb_t *fb, int x, int y, int h, fb_color_t color, int step);
void ui_draw_page_frame(fb_t *fb, ui_frame_flags_t flags);
int  ui_draw_battery_badge(fb_t *fb, int right_x, int y);
void ui_draw_header(fb_t *fb, const char *title, const char *right, bool red_accent);
void ui_draw_footer(fb_t *fb, const char *left, const char *right);
void ui_draw_card(fb_t *fb, int x, int y, int w, int h, bool red_accent);
void ui_draw_section_label(fb_t *fb, int x, int y, const char *label, fb_color_t color, int scale);
void ui_draw_empty_state(fb_t *fb, const char *title, const char *hint);
void ui_draw_progress_bar(fb_t *fb, int x, int y, int w, int h, int percent, fb_color_t fill);
