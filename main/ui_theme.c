#include "ui_theme.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "battery_mon.h"

ui_layout_class_t ui_layout_for(const fb_t *fb)
{
    if (!fb)
        return UI_LAYOUT_COMPACT;
    if (fb->width >= 760 && fb->height >= 430)
        return UI_LAYOUT_LARGE;
    if (fb->width >= 600 && fb->height >= 430)
        return UI_LAYOUT_583;
    if (fb->width == 400 && fb->height == 300)
        return UI_LAYOUT_42;
    return UI_LAYOUT_COMPACT;
}

bool ui_layout_is_42(const fb_t *fb)
{
    return ui_layout_for(fb) == UI_LAYOUT_42;
}

bool ui_layout_is_583(const fb_t *fb)
{
    return ui_layout_for(fb) == UI_LAYOUT_583;
}

bool ui_layout_is_wide(const fb_t *fb)
{
    ui_layout_class_t cls = ui_layout_for(fb);
    return cls == UI_LAYOUT_583 || cls == UI_LAYOUT_LARGE;
}

int ui_scale_for(const fb_t *fb)
{
    return (fb && fb->width >= 600) ? 2 : 1;
}

int ui_text_width(const char *s, int scale)
{
    if (!s)
        return 0;
    if (scale < 1)
        scale = 1;

    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp = 0;
        if (*p < 0x80) {
            cp = *p;
            p++;
        } else if ((*p & 0xE0) == 0xC0 && p[1] && (p[1] & 0xC0) == 0x80) {
            cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 &&
                   p[1] && (p[1] & 0xC0) == 0x80 &&
                   p[2] && (p[2] & 0xC0) == 0x80) {
            cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 &&
                   p[1] && (p[1] & 0xC0) == 0x80 &&
                   p[2] && (p[2] & 0xC0) == 0x80 &&
                   p[3] && (p[3] & 0xC0) == 0x80) {
            cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
                 ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        } else {
            cp = '?';
            p++;
        }

        if (cp == 0x3000 || (cp >= 0xFF01 && cp <= 0xFF5E))
            cp = (cp == 0x3000) ? ' ' : cp - 0xFEE0;

        if (cp < 0x80)
            w += 8 * scale;
        else
            w += 16 * scale;
    }
    return w;
}

int ui_role_scale(const fb_t *fb, ui_text_role_t role)
{
    switch (role) {
    case UI_TEXT_VALUE:
    case UI_TEXT_TITLE:
        return 2;
    case UI_TEXT_LABEL:
    case UI_TEXT_SMALL:
    case UI_TEXT_BODY:
    default:
        return 1;
    }
}

static bool ui_role_prefers_ext(const fb_t *fb, ui_text_role_t role, int scale)
{
    (void)fb;
    (void)role;
    (void)scale;
    return false;
}

int ui_role_text_width(const fb_t *fb, const char *s, ui_text_role_t role)
{
    int scale = ui_role_scale(fb, role);
    if (ui_role_prefers_ext(fb, role, scale))
        return fb_utf8_scaled_styled_width(s, scale);
    return fb_utf8_scaled_width(s, scale);
}

int ui_draw_text_role(fb_t *fb, int x, int y, const char *s,
                      fb_color_t color, ui_text_role_t role)
{
    int scale = ui_role_scale(fb, role);
    if (ui_role_prefers_ext(fb, role, scale))
        return fb_utf8_scaled_styled(fb, x, y, s, color, scale);
    return fb_utf8_scaled(fb, x, y, s, color, scale);
}

int ui_draw_text_role_maxw(fb_t *fb, int x, int y, const char *s,
                           fb_color_t color, ui_text_role_t role, int max_w)
{
    int scale = ui_role_scale(fb, role);
    if (ui_role_prefers_ext(fb, role, scale))
        return fb_utf8_scaled_styled_maxw(fb, x, y, s, color, scale, max_w);
    return fb_utf8_scaled_maxw(fb, x, y, s, color, scale, max_w);
}

void ui_draw_dotted_hline(fb_t *fb, int x, int y, int w, fb_color_t color, int step)
{
    if (!fb || w <= 0)
        return;
    if (step < 3)
        step = 3;
    int dash = step / 2;
    if (dash < 1)
        dash = 1;
    for (int dx = 0; dx < w; dx += step) {
        int len = dash;
        if (dx + len > w)
            len = w - dx;
        fb_hline(fb, x + dx, y, len, color);
    }
}

void ui_draw_dotted_vline(fb_t *fb, int x, int y, int h, fb_color_t color, int step)
{
    if (!fb || h <= 0)
        return;
    if (step < 3)
        step = 3;
    int dash = step / 2;
    if (dash < 1)
        dash = 1;
    for (int dy = 0; dy < h; dy += step) {
        int len = dash;
        if (dy + len > h)
            len = h - dy;
        fb_vline(fb, x, y + dy, len, color);
    }
}

void ui_draw_page_frame(fb_t *fb, ui_frame_flags_t flags)
{
    if (!fb)
        return;

    const int W = fb->width;
    const int H = fb->height;
    int m = (flags & UI_FRAME_THIN) ? 4 : 8;
    fb_rect(fb, m, m, W - 2 * m, H - 2 * m, COLOR_BLACK);

    if (flags & UI_FRAME_RED_ACCENT) {
        int stroke = (flags & UI_FRAME_THIN) ? 2 : 3;
        int accent_w = W / 10;
        int accent_h = H / 10;
        if (accent_w < 42)
            accent_w = 42;
        if (accent_h < 42)
            accent_h = 42;
        fb_fill_rect(fb, m, m, accent_w, stroke, COLOR_RED);
        fb_fill_rect(fb, m, m, stroke, accent_h, COLOR_RED);
    }
}

int ui_draw_battery_badge(fb_t *fb, int right_x, int y)
{
    if (!fb)
        return 0;

    battery_status_t st = {0};
    battery_mon_get_status(&st);

    char label[8];
    if (!st.valid)
        snprintf(label, sizeof(label), "--");
    else if (st.charging)
        snprintf(label, sizeof(label), "+%u%%", (unsigned)st.percent);
    else
        snprintf(label, sizeof(label), "%u%%", (unsigned)st.percent);

    const int icon_w = 22;
    const int icon_h = 10;
    const int cap_w = 3;
    const int gap = 4;
    const int text_w = ui_text_width(label, 1);
    const int total_w = icon_w + cap_w + gap + text_w;
    const int x = right_x - total_w;
    const int icon_x = x;
    const int icon_y = y + 3;

    fb_rect(fb, icon_x, icon_y, icon_w, icon_h, COLOR_BLACK);
    fb_fill_rect(fb, icon_x + icon_w, icon_y + 3, cap_w, icon_h - 6, COLOR_BLACK);

    fb_color_t accent = COLOR_BLACK;
    if (!st.valid || st.charging || st.percent <= 20)
        accent = COLOR_RED;

    if (st.valid) {
        int fill = (icon_w - 4) * st.percent / 100;
        if (st.percent > 0 && fill < 1)
            fill = 1;
        if (fill > 0)
            fb_fill_rect(fb, icon_x + 2, icon_y + 2, fill, icon_h - 4, accent);
    } else {
        fb_hline(fb, icon_x + 5, icon_y + 4, 10, COLOR_RED);
        fb_hline(fb, icon_x + 7, icon_y + 6, 10, COLOR_RED);
    }

    fb_utf8_scaled(fb, icon_x + icon_w + cap_w + gap, y + 1, label, accent, 1);
    return total_w;
}

void ui_draw_header(fb_t *fb, const char *title, const char *right, bool red_accent)
{
    if (!fb)
        return;

    const int W = fb->width;
    const int s = ui_scale_for(fb);
    const int x = 14 * s;
    const int y = 8 * s;
    const int header_h = 18 * s;

    if (red_accent)
        fb_fill_rect(fb, x - 6 * s, y + 1 * s, 2 * s, 12 * s, COLOR_RED);

    int title_w = 0;
    if (title && title[0]) {
        int title_max_w = W / 2 - 28 * s;
        if (title_max_w < 0)
            title_max_w = 0;
        title_w = ui_text_width(title, 1);
        if (title_w > title_max_w)
            title_w = title_max_w;
        fb_utf8_scaled_maxw(fb, x, y, title, COLOR_BLACK, 1, title_max_w);
    }

    int battery_right = W - 14 * s;
    int battery_w = ui_draw_battery_badge(fb, battery_right, y);
    int right_limit = battery_right - battery_w - 8 * s;

    if (right && right[0]) {
        int min_right_x = x + title_w + 12 * s;
        if (min_right_x < x)
            min_right_x = x;
        if (min_right_x > right_limit)
            min_right_x = right_limit;

        int rw = ui_text_width(right, 1);
        int rx = right_limit - rw;
        if (rx < min_right_x)
            rx = min_right_x;
        if (right_limit > rx)
            fb_utf8_scaled_maxw(fb, rx, y + 1 * s, right, COLOR_BLACK, 1,
                                right_limit - rx);
    }

    ui_draw_dotted_hline(fb, 12 * s, header_h + 6 * s, W - 24 * s,
                         COLOR_BLACK, 6);
}

void ui_draw_footer(fb_t *fb, const char *left, const char *right)
{
    if (!fb)
        return;

    const int W = fb->width;
    const int H = fb->height;
    const int s = ui_scale_for(fb);
    const int y = H - 24 * s;

    ui_draw_dotted_hline(fb, 12 * s, y - 4 * s, W - 24 * s, COLOR_BLACK, 6);

    int left_max_w = W - 28 * s;
    if (right && right[0]) {
        int rw = ui_text_width(right, 1);
        left_max_w = W - 34 * s - rw;
    }
    if (left && left[0] && left_max_w > 0)
        fb_utf8_scaled_maxw(fb, 14 * s, y, left, COLOR_BLACK, 1, left_max_w);

    if (right && right[0]) {
        int rw = ui_text_width(right, 1);
        fb_utf8_scaled(fb, W - 14 * s - rw, y, right, COLOR_BLACK, 1);
    }
}

void ui_draw_card(fb_t *fb, int x, int y, int w, int h, bool red_accent)
{
    if (!fb || w <= 1 || h <= 1)
        return;
    fb_rect(fb, x, y, w, h, COLOR_BLACK);
    if (red_accent) {
        int accent_w = w / 5;
        if (accent_w < 28)
            accent_w = 28;
        if (accent_w > 96)
            accent_w = 96;
        fb_fill_rect(fb, x, y, accent_w, 2, COLOR_RED);
        fb_fill_rect(fb, x, y, 2, h > 34 ? 34 : h, COLOR_RED);
    }
}

void ui_draw_section_label(fb_t *fb, int x, int y, const char *label,
                           fb_color_t color, int scale)
{
    if (!fb || !label)
        return;
    if (scale < 1)
        scale = 1;
    fb_fill_rect(fb, x, y + 4 * scale, 4 * scale, 10 * scale, color);
    fb_utf8_scaled(fb, x + 8 * scale, y, label, color, scale);
}

void ui_draw_empty_state(fb_t *fb, const char *title, const char *hint)
{
    if (!fb)
        return;
    int s = ui_scale_for(fb);
    int y = fb->height / 2 - 28 * s;
    if (title) {
        int w = ui_text_width(title, s + 1);
        fb_utf8_scaled(fb, (fb->width - w) / 2, y, title, COLOR_BLACK, s + 1);
    }
    if (hint) {
        int w = ui_text_width(hint, s);
        fb_utf8_scaled(fb, (fb->width - w) / 2, y + 28 * s, hint, COLOR_RED, s);
    }
}

void ui_draw_progress_bar(fb_t *fb, int x, int y, int w, int h, int percent, fb_color_t fill)
{
    if (!fb || w <= 2 || h <= 2)
        return;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    fb_rect(fb, x, y, w, h, COLOR_BLACK);
    int inner = (w - 2) * percent / 100;
    if (inner > 0)
        fb_fill_rect(fb, x + 1, y + 1, inner, h - 2, fill);
}
