/**
 * @file calendar_display.c
 *
 * 月历视图：参考常见电子纸日历（如 InkyDash / Inkycal 类项目）的极简层次 —
 * 高对比标题条、星期行、规整网格、当日高亮与农历/节日辅信息。
 *
 * 布局模型：固定 6×7 可见格，由格索引推导公历日（当月空白格不绘制）。
 */

#include "calendar_display.h"
#include "lunar.h"
#include "fb_render.h"
#include "ui_theme.h"
#include "epd.h"
#include "display_policy.h"
#include "time_sync.h"
#include "weather.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

/* -------------------------------------------------------------------------- */
/* 配置                                                                       */
/* -------------------------------------------------------------------------- */

static const char *TAG = "calendar";
static const char *NVS_NS = "calendar";
static const char *NVS_KEY_STYLE = "style";

typedef enum {
    CAL_STYLE_CLASSIC = 0,
    CAL_STYLE_OVERVIEW = 1,
    CAL_STYLE_COUNT,
} calendar_style_t;

static calendar_style_t s_style = CAL_STYLE_CLASSIC;
static int s_last_year;
static int s_last_month;
static int s_last_day;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

enum {
    CAL_GRID_ROWS = 6,
    CAL_GRID_COLS = 7,
    CAL_GRID_CELLS = CAL_GRID_ROWS * CAL_GRID_COLS,
    CAL_YEAR_MIN = 1900,
    CAL_YEAR_MAX = 2100,
};

/* -------------------------------------------------------------------------- */
/* 并发：单月历渲染互斥（HTTP 可早于 init 调 show，故惰性建 mutex）              */
/* -------------------------------------------------------------------------- */

static SemaphoreHandle_t s_mutex;

static void cal_style_load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return;

    uint8_t style = 0;
    if (nvs_get_u8(h, NVS_KEY_STYLE, &style) == ESP_OK && style < CAL_STYLE_COUNT) {
        portENTER_CRITICAL(&s_state_mux);
        s_style = (calendar_style_t)style;
        portEXIT_CRITICAL(&s_state_mux);
        ESP_LOGI(TAG, "NVS style loaded: %u", (unsigned)style);
    }
    nvs_close(h);
}

static void cal_style_save_nvs(calendar_style_t style)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed while saving style: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(h, NVS_KEY_STYLE, (uint8_t)style);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK)
        ESP_LOGI(TAG, "NVS style saved: %d", (int)style);
    else
        ESP_LOGW(TAG, "NVS style save failed: %s", esp_err_to_name(err));
}

esp_err_t calendar_display_init(void)
{
    s_mutex = xSemaphoreCreateBinary();
    if (!s_mutex)
        return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_mutex);
    cal_style_load_nvs();
    return ESP_OK;
}

/** 与 clock/timetable 一致：优先 SNTP 后的本地时区；未同步则退回系统 localtime */
static void cal_today_ymd(int *out_y, int *out_m, int *out_d)
{
    struct tm loc;
    if (time_sync_get_local_relaxed(&loc)) {
        *out_y = loc.tm_year + 1900;
        *out_m = loc.tm_mon + 1;
        *out_d = loc.tm_mday;
        return;
    }
    *out_y = 0;
    *out_m = 0;
    *out_d = 0;
}

/* -------------------------------------------------------------------------- */
/* 公历工具                                                                   */
/* -------------------------------------------------------------------------- */

static int cal_days_in_month(int y, int m)
{
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
        return 29;
    return dm[m - 1];
}

/** 0=周日 … 6=周六；mktime 为主，正午规避时区/DST；失败则 Sakamoto */
static int cal_solar_weekday_sun0(int year, int month, int day)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    time_t tt = mktime(&t);
    if (tt != (time_t)-1) {
        localtime_r(&tt, &t);
        return t.tm_wday;
    }
    static const int s[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int yy = year;
    if (month < 3)
        yy--;
    return (yy + yy / 4 - yy / 100 + yy / 400 + s[month - 1] + day) % 7;
}

static inline int cal_day_for_cell(int cell, int dow1, int dim)
{
    int d = cell - dow1 + 1;
    return (d >= 1 && d <= dim) ? d : 0;
}

/* -------------------------------------------------------------------------- */
/* 布局（由屏宽一次性算出，避免魔法数散落）                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    int fb_w, fb_h;
    int sc;
    int title_h;
    int wday_h;
    int footer_h;
    int grid_x;
    int grid_w;
    int grid_top;
    int grid_h;
    int row_h;
    int col_w;
} cal_layout_t;

static void cal_layout_build(cal_layout_t *L, int w, int h)
{
    L->fb_w = w;
    L->fb_h = h;
    L->sc = (w >= 600) ? 2 : 1;
    int s = L->sc;
    int mx = 12 * s;

    /* Month view is dense by nature. Keep the title readable, but render the
     * grid text at 1x so the page does not become a wall of heavy pixels. */
    L->title_h = 28 * s;
    L->wday_h  = 16 * s;
    L->footer_h = 30 * s;
    L->grid_x = mx;
    L->grid_w = w - 2 * mx;
    if (L->grid_w < CAL_GRID_COLS)
        L->grid_w = CAL_GRID_COLS;
    L->grid_top = L->title_h + L->wday_h;
    L->grid_h = h - L->grid_top - L->footer_h;
    if (L->grid_h < 1)
        L->grid_h = 1;
    L->row_h = L->grid_h / CAL_GRID_ROWS;
    L->col_w = L->grid_w / CAL_GRID_COLS;
    if (L->row_h < 1)
        L->row_h = 1;
    if (L->col_w < 1)
        L->col_w = 1;
}

/* -------------------------------------------------------------------------- */
/* 文本 / 图形                                                                */
/* -------------------------------------------------------------------------- */

static int cal_utf8_width_px(const char *str, int ascii_w, int cjk_w)
{
    int w = 0;
    for (const char *p = str; *p;) {
        if ((*p & 0x80) == 0) {
            w += ascii_w;
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            w += cjk_w;
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            w += cjk_w;
            p += 3;
        } else {
            w += cjk_w;
            p += 4;
        }
    }
    return w;
}

static void cal_utf8_copy_prefix(char *dst, size_t dst_sz,
                                 const char *src, int max_chars)
{
    size_t di = 0;
    int copied = 0;

    if (!dst || dst_sz == 0)
        return;
    dst[0] = '\0';
    if (!src || max_chars <= 0)
        return;

    const unsigned char *p = (const unsigned char *)src;
    while (*p && copied < max_chars) {
        size_t len = 1;
        if ((*p & 0xE0) == 0xC0)
            len = 2;
        else if ((*p & 0xF0) == 0xE0)
            len = 3;
        else if ((*p & 0xF8) == 0xF0)
            len = 4;

        if (di + len >= dst_sz)
            break;
        memcpy(dst + di, p, len);
        di += len;
        p += len;
        copied++;
    }
    dst[di] = '\0';
}

static int cal_day_of_year(int year, int month, int day)
{
    int doy = day;
    for (int m = 1; m < month; m++)
        doy += cal_days_in_month(year, m);
    return doy;
}

static int cal_iso_weeks_in_year(int year)
{
    int jan1 = cal_solar_weekday_sun0(year, 1, 1);
    int jan1_mon0 = (jan1 + 6) % 7;
    bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    return (jan1_mon0 == 3 || (jan1_mon0 == 2 && leap)) ? 53 : 52;
}

static int cal_iso_week_number(int year, int month, int day)
{
    int sun0 = cal_solar_weekday_sun0(year, month, day);
    int iso_wday = ((sun0 + 6) % 7) + 1; /* Monday=1 ... Sunday=7 */
    int week = (cal_day_of_year(year, month, day) - iso_wday + 10) / 7;
    if (week < 1)
        return cal_iso_weeks_in_year(year - 1);
    if (week > cal_iso_weeks_in_year(year))
        return 1;
    return week;
}

static int cal_isqrt_u32(uint32_t x)
{
    if (x == 0)
        return 0;
    uint32_t r = x;
    while (r * r > x)
        r = (r + x / r) >> 1;
    return (int)r;
}

static void cal_draw_circle_outline(fb_t *fb, int cx, int cy, int radius, fb_color_t c)
{
    if (radius <= 0)
        return;
    uint32_t r2 = (uint32_t)radius * (uint32_t)radius;
    uint32_t inner_r2 = (uint32_t)(radius - 1) * (uint32_t)(radius - 1);
    for (int dy = -radius; dy <= radius; dy++) {
        uint32_t outer = r2 - (uint32_t)(dy * dy);
        int outer_half = cal_isqrt_u32(outer);
        int inner_half = -1;
        if ((uint32_t)(dy * dy) <= inner_r2)
            inner_half = cal_isqrt_u32(inner_r2 - (uint32_t)(dy * dy));

        if (inner_half < 0) {
            fb_hline(fb, cx - outer_half, cy + dy, 2 * outer_half + 1, c);
        } else {
            int left_w = outer_half - inner_half;
            int right_x = cx + inner_half + 1;
            if (left_w > 0) {
                fb_hline(fb, cx - outer_half, cy + dy, left_w, c);
                fb_hline(fb, right_x, cy + dy, left_w, c);
            }
        }
    }
}

static void cal_draw_light_hline(fb_t *fb, int x, int y, int w, fb_color_t c)
{
    for (int dx = 0; dx < w; dx += 10) {
        int len = 1;
        if (dx + len > w)
            len = w - dx;
        fb_hline(fb, x + dx, y, len, c);
    }
}

static void cal_draw_light_vline(fb_t *fb, int x, int y, int h, fb_color_t c)
{
    for (int dy = 0; dy < h; dy += 10) {
        int len = 1;
        if (dy + len > h)
            len = h - dy;
        fb_vline(fb, x, y + dy, len, c);
    }
}

static void cal_draw_degree_mark(fb_t *fb, int x, int y, fb_color_t c)
{
    fb_pixel(fb, x + 1, y, c);
    fb_hline(fb, x, y + 1, 3, c);
    fb_pixel(fb, x + 1, y + 2, c);
}

static int cal_draw_compact_date(fb_t *fb, int x, int y,
                                 int year, int month, int day)
{
    char date[32];
    snprintf(date, sizeof(date), "%04d年%d月%d日", year, month, day);

    return fb_utf8_px(fb, x, y + 3, date, COLOR_RED, 24);
}

/* -------------------------------------------------------------------------- */
/* 绘制：标题 / 星期 / 网格线 / 日格                                          */
/* -------------------------------------------------------------------------- */

static void cal_draw_title(fb_t *fb, const cal_layout_t *L, int year, int month)
{
    int W = L->fb_w;
    int s = L->sc;

    char title[40];
    snprintf(title, sizeof(title), "%d年%d月", year, month);
    char meta[48] = "";

    lunar_date_t ld;
    int mid = cal_days_in_month(year, month) / 2;
    if (lunar_from_solar(year, month, mid, &ld)) {
        snprintf(meta, sizeof(meta), "%s年[%s]",
                 lunar_year_gz(ld.year), lunar_year_sx(ld.year));
    }
    fb_fill_rect(fb, L->grid_x, 8 * s, 2 * s, 12 * s, COLOR_RED);
    fb_utf8_scaled(fb, L->grid_x + 8 * s, 6 * s, title, COLOR_BLACK, s);
    if (meta[0]) {
        int tw = cal_utf8_width_px(meta, 8, 16);
        fb_utf8_scaled(fb, W - tw - L->grid_x, 10 * s, meta, COLOR_BLACK, 1);
    }
    ui_draw_dotted_hline(fb, L->grid_x, L->title_h - 4 * s,
                         L->col_w * CAL_GRID_COLS,
                         COLOR_BLACK, 6);
}

static void cal_draw_weekday_row(fb_t *fb, const cal_layout_t *L)
{
    static const char *WD[] = {"日", "一", "二", "三", "四", "五", "六"};
    int y = L->title_h + (L->wday_h - 16) / 2;

    for (int c = 0; c < CAL_GRID_COLS; c++) {
        int x = L->grid_x + c * L->col_w + (L->col_w - 16) / 2;
        fb_color_t col = (c == 0 || c == CAL_GRID_COLS - 1) ? COLOR_RED : COLOR_BLACK;
        fb_utf8_scaled(fb, x, y, WD[c], col, 1);
    }
}

/** 先画线再画数字，避免格线与文字争同一像素（常见月历 widget 顺序） */
static void cal_draw_grid_lines(fb_t *fb, const cal_layout_t *L)
{
    int H   = L->fb_h;
    int top = L->grid_top;
    int x0  = L->grid_x;
    int w   = L->col_w * CAL_GRID_COLS;

    /* 横线：每行上边界。1px dotted keeps the page closer to a paper planner. */
    for (int r = 0; r <= CAL_GRID_ROWS; r++) {
        int y = top + r * L->row_h;
        if (y < H)
            ui_draw_dotted_hline(fb, x0, y, w, COLOR_BLACK, 6);
    }
    /* 竖线：仅画内部分隔，不再形成厚重外框 */
    int actual_gh = CAL_GRID_ROWS * L->row_h + 1;
    if (top + actual_gh > H) actual_gh = H - top;
    for (int c = 1; c < CAL_GRID_COLS; c++) {
        int x = x0 + c * L->col_w;
        if (x < L->fb_w)
            ui_draw_dotted_vline(fb, x, top, actual_gh, COLOR_BLACK, 6);
    }
}

static void cal_draw_day_cell(fb_t *fb, const cal_layout_t *L,
                              int year, int month, int day, int col,
                              int cell_x, int cell_y,
                              int today_y, int today_m, int today_d)
{
    int cw = L->col_w;
    int text_sc = 1;

    bool is_today = (year == today_y && month == today_m && day == today_d);
    bool weekend  = (col == 0 || col == CAL_GRID_COLS - 1);

    int sub_gap = 3;
    int total_h = 16 * text_sc + sub_gap + 16 * text_sc;
    int top_pad = (L->row_h - total_h) / 2;
    if (top_pad < 3)
        top_pad = 3;

    if (is_today) {
        int ccx    = cell_x + cw / 2;
        int ccy    = cell_y + top_pad + 8 * text_sc;
        int radius = 10;
        if (radius > cw / 2 - 1)      radius = cw / 2 - 1;
        if (radius > ccy - cell_y)     radius = ccy - cell_y;  /* 不超出格顶 */
        if (radius < 4)                radius = 4;
        cal_draw_circle_outline(fb, ccx, ccy, radius, COLOR_RED);
    }

    char ds[12];
    snprintf(ds, sizeof(ds), "%d", day);
    int dw = (int)strlen(ds) * 8 * text_sc;
    int dx = cell_x + (cw - dw) / 2;
    int dy = cell_y + top_pad;
    fb_color_t num_c = weekend ? COLOR_RED : COLOR_BLACK;
    fb_utf8_scaled(fb, dx, dy, ds, num_c, text_sc);

    lunar_date_t ld;
    const char *label  = NULL;
    bool        special = false;

    if (lunar_from_solar(year, month, day, &ld)) {
        const char *fest = lunar_festival(year, month, day, &ld);
        const char *term = lunar_solar_term(year, month, day);
        if (fest) {
            label   = fest;
            special = true;
        } else if (term) {
            label   = term;
            special = true;
        } else if (ld.day == 1) {
            label   = lunar_month_str(ld.month);
            special = true;
        } else {
            label = lunar_day_str(ld.day);
        }
    }

    if (!label)
        return;

    int ly = cell_y + top_pad + 16 * text_sc + sub_gap;
    fb_color_t sub_c = (special || weekend) ? COLOR_RED : COLOR_BLACK;

    int max_w = cw - 2;
    if (max_w < 8) max_w = 8;
    int lw = cal_utf8_width_px(label, 8 * text_sc, 16 * text_sc);
    int lx = (lw <= max_w) ? cell_x + (cw - lw) / 2 : cell_x + (cw - max_w) / 2;
    if (lx < cell_x + 1)
        lx = cell_x + 1;
    fb_utf8_scaled_maxw(fb, lx, ly, label, sub_c, text_sc, max_w);
}

/* -------------------------------------------------------------------------- */
/* 合成一帧                                                                   */
/* -------------------------------------------------------------------------- */

static void cal_draw_overview_weather(fb_t *fb, int x, int y, int max_w, bool big)
{
    weather_summary_t wd;
    memset(&wd, 0, sizeof(wd));
    weather_get_summary_copy(&wd);

    const int box_h = big ? 42 : 48;
    if (max_w < WEATHER_ICON_W + 36)
        return;

    if (big)
        fb_rect(fb, x, y, max_w, box_h, COLOR_BLACK);

    if (wd.valid) {
        int icon_x = x + (big ? 5 : 4);
        int icon_y = y + (box_h - WEATHER_ICON_H) / 2;
        fb_bitmap(fb, icon_x, icon_y, WEATHER_ICON_W, WEATHER_ICON_H,
                  weather_icon_bitmap(wd.now.icon), COLOR_BLACK);

        char l1[48];
        char temp_num[8];
        char hum_part[16];
        char l2[48];
        char l3[48];
        snprintf(l1, sizeof(l1), "%s", wd.now.text);
        if (big) {
            snprintf(l2, sizeof(l2), "%d\xc2\xb0""C %d%%",
                     wd.now.temp, wd.now.humidity);
        } else {
            snprintf(temp_num, sizeof(temp_num), "%d", wd.now.temp);
            snprintf(hum_part, sizeof(hum_part), "%d%%", wd.now.humidity);
            snprintf(l2, sizeof(l2), "%dC | %d%%",
                     wd.now.temp, wd.now.humidity);
        }
        snprintf(l3, sizeof(l3), "%s %d级",
                 wd.now.wind_dir[0] ? wd.now.wind_dir : "风",
                 wd.now.wind_scale);

        int tx = icon_x + WEATHER_ICON_W + (big ? 5 : 3);
        int tw = max_w - (tx - x) - (big ? 4 : 3);
        if (tw < 32)
            tw = max_w;
        if (!big && cal_utf8_width_px(l2, 8, 16) > tw)
            snprintf(l2, sizeof(l2), "%dC", wd.now.temp);
        if (!big && cal_utf8_width_px(l3, 8, 16) > tw) {
            char wind_short[16];
            cal_utf8_copy_prefix(wind_short, sizeof(wind_short),
                                 wd.now.wind_dir, 2);
            snprintf(l3, sizeof(l3), "%s %d级",
                     wind_short[0] ? wind_short : "风",
                     wd.now.wind_scale);
            if (cal_utf8_width_px(l3, 8, 16) > tw)
                snprintf(l3, sizeof(l3), "%d级", wd.now.wind_scale);
        }
        if (big) {
            fb_utf8_scaled_maxw(fb, tx, y + 4, l1, COLOR_BLACK, 1, tw);
            fb_utf8_scaled_maxw(fb, tx, y + 22, l2, COLOR_BLACK, 1, tw);
        } else {
            fb_utf8_scaled_maxw(fb, tx, y, l1, COLOR_BLACK, 1, tw);
            int temp_w = fb_utf8_scaled_maxw(fb, tx, y + 16, temp_num,
                                             COLOR_BLACK, 1, 24);
            cal_draw_degree_mark(fb, tx + temp_w + 1, y + 18, COLOR_BLACK);
            fb_utf8_scaled(fb, tx + temp_w + 6, y + 16, "C", COLOR_BLACK, 1);
            fb_utf8_scaled(fb, tx + 40, y + 16, "|", COLOR_BLACK, 1);
            fb_utf8_scaled_maxw(fb, tx + 50, y + 16, hum_part, COLOR_BLACK, 1,
                                tw - 50);
            fb_utf8_scaled_maxw(fb, tx, y + 32, l3, COLOR_BLACK, 1, tw);
        }
    } else {
        fb_utf8_scaled_maxw(fb, x + 8, y + (big ? 13 : 9),
                            "\xe6\x97\xa0\xe5\xa4\xa9\xe6\xb0\x94",
                            COLOR_BLACK, 1, max_w - 16);
    }
}

static void cal_draw_overview_cell(fb_t *fb, int x, int y, int w, int h,
                                   int year, int month, int day, int col,
                                   int today_y, int today_m, int today_d)
{
    bool is_today = (year == today_y && month == today_m && day == today_d);
    bool weekend = (col == 0 || col == CAL_GRID_COLS - 1);
    bool big = (fb->width >= 600);
    int num_y = big ? y + 3 : y + 2;
    int lunar_y = big ? y + h - 17 : y + 18;
    int max_w = big ? w - 4 : w - 2;

    if (is_today) {
        if (big) {
            fb_rect(fb, x + 4, y + 4, w - 8, h - 7, COLOR_RED);
        } else {
            const int today_w = 38;
            fb_rect(fb, x + (w - today_w) / 2, y + 0, today_w, 35, COLOR_RED);
        }
    }

    char ds[8];
    snprintf(ds, sizeof(ds), "%d", day);
    int day_w = cal_utf8_width_px(ds, 8, 16);
    int num_x = x + (w - day_w) / 2;
    if (num_x < x + 1)
        num_x = x + 1;
    fb_utf8_scaled(fb, num_x, num_y, ds,
                   weekend ? COLOR_RED : COLOR_BLACK, 1);

    lunar_date_t ld;
    const char *label = NULL;
    bool special = false;
    if (lunar_from_solar(year, month, day, &ld)) {
        const char *fest = lunar_festival(year, month, day, &ld);
        const char *term = lunar_solar_term(year, month, day);
        if (fest) {
            label = fest;
            special = true;
        } else if (term) {
            label = term;
            special = true;
        } else if (ld.day == 1) {
            label = lunar_month_str(ld.month);
            special = true;
        } else {
            label = lunar_day_str(ld.day);
        }
    }

    if (!label)
        return;

    if (max_w < 8)
        max_w = 8;
    int lw = cal_utf8_width_px(label, 8, 16);
    int lx = (lw <= max_w) ? x + (w - lw) / 2 : x + (w - max_w) / 2;
    if (lx < x + 1)
        lx = x + 1;
    fb_utf8_scaled_maxw(fb, lx, lunar_y, label,
                        (special || weekend) ? COLOR_RED : COLOR_BLACK,
                        1, max_w);
}

static void cal_render_overview(fb_t *fb, int year, int month,
                                int today_y, int today_m, int today_d)
{
    const int W = fb->width;
    const int H = fb->height;
    const bool big = (W >= 600);
    const int mx = big ? 24 : 14;
    const int top = big ? 16 : 10;
    const int header_h = big ? 84 : 56;
    const int week_h = big ? 28 : 18;
    const int bottom_gap = big ? 10 : 10;
    const int grid_x = mx;
    const int grid_w = W - 2 * mx;
    const int col_w = grid_w / CAL_GRID_COLS;
    const int week_y = top + header_h;
    const int grid_top = week_y + week_h;
    const int grid_h = H - grid_top - bottom_gap;
    const int row_h = grid_h / CAL_GRID_ROWS;
    const int grid_actual_w = col_w * CAL_GRID_COLS;
    const int grid_actual_h = row_h * CAL_GRID_ROWS;

    fb_clear(fb);
    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
    if (!big)
        fb_rect(fb, 9, 9, W - 18, H - 18, COLOR_BLACK);

    const int date_x = big ? mx + 8 : mx + 1;
    int date_w = 0;
    if (big) {
        char date_big[32];
        snprintf(date_big, sizeof(date_big), "%04d.%02d.%02d",
                 today_y, today_m, today_d);
        date_w = fb_utf8_scaled(fb, date_x, top + 4, date_big, COLOR_RED, 3);
    } else {
        date_w = cal_draw_compact_date(fb, date_x, top + 3,
                                       today_y, today_m, today_d);
        char week_line[16];
        snprintf(week_line, sizeof(week_line), "第%d周",
                 cal_iso_week_number(today_y, today_m, today_d));
        fb_utf8_scaled_maxw(fb, date_x, top + 35, week_line, COLOR_RED, 1, 80);
    }

    char lunar_line[80] = "";
    lunar_date_t today_ld;
    if (lunar_from_solar(today_y, today_m, today_d, &today_ld)) {
        snprintf(lunar_line, sizeof(lunar_line), "%s\xe5\xb9\xb4  %s%s",
                 lunar_year_gz(today_ld.year),
                 lunar_month_str(today_ld.month),
                 lunar_day_str(today_ld.day));
    }

    static const char *WD_LONG[] = {
        "\xe6\x98\x9f\xe6\x9c\x9f\xe6\x97\xa5",
        "\xe6\x98\x9f\xe6\x9c\x9f\xe4\xb8\x80",
        "\xe6\x98\x9f\xe6\x9c\x9f\xe4\xba\x8c",
        "\xe6\x98\x9f\xe6\x9c\x9f\xe4\xb8\x89",
        "\xe6\x98\x9f\xe6\x9c\x9f\xe5\x9b\x9b",
        "\xe6\x98\x9f\xe6\x9c\x9f\xe4\xba\x94",
        "\xe6\x98\x9f\xe6\x9c\x9f\xe5\x85\xad",
    };
    int wday = cal_solar_weekday_sun0(today_y, today_m, today_d);
    if (wday < 0 || wday > 6)
        wday = 0;

    const int weather_w = big ? 190 : 116;
    const int weather_x = big ? W - mx - weather_w : W - mx - weather_w - 2;
    const int weather_y = top + (big ? 9 : 6);
    cal_draw_overview_weather(fb, weather_x, weather_y, weather_w, big);

    if (big) {
        const int sub_y = top + 58;
        int sub_max = weather_x - date_x - 10;
        if (sub_max < 80)
            sub_max = W / 2;
        fb_utf8_scaled_maxw(fb, date_x, sub_y, lunar_line, COLOR_BLACK, 1,
                            sub_max);
        int lunar_w = cal_utf8_width_px(lunar_line, 8, 16);
        if (lunar_w > sub_max - 64)
            lunar_w = sub_max - 64;
        if (lunar_w < 0)
            lunar_w = 0;
        fb_utf8_scaled_maxw(fb, date_x + 8 + lunar_w, sub_y, WD_LONG[wday],
                            COLOR_RED, 1, sub_max - lunar_w);
    } else {
        char meta_top[48] = "";
        char meta_bot[48] = "";
        if (today_ld.year) {
            snprintf(meta_top, sizeof(meta_top), "%s年 %s",
                     lunar_year_gz(today_ld.year), lunar_year_sx(today_ld.year));
            snprintf(meta_bot, sizeof(meta_bot), "%s%s",
                     lunar_month_str(today_ld.month), lunar_day_str(today_ld.day));
        }
        int meta_x = date_x + date_w + 8;
        int meta_w = weather_x - meta_x - 4;
        if (meta_w > 24) {
            fb_utf8_scaled_maxw(fb, meta_x, top + 6, meta_top,
                                COLOR_BLACK, 1, meta_w);
            fb_utf8_scaled_maxw(fb, meta_x, top + 24, meta_bot,
                                COLOR_BLACK, 1, meta_w);
        }
    }
    if (big) {
        cal_draw_light_hline(fb, grid_x, week_y - 7,
                             grid_actual_w, COLOR_BLACK);
    }

    fb_fill_rect(fb, grid_x, week_y, grid_actual_w, week_h, COLOR_BLACK);
    static const char *WD_SHORT[] = {
        "\xe6\x97\xa5", "\xe4\xb8\x80", "\xe4\xba\x8c", "\xe4\xb8\x89",
        "\xe5\x9b\x9b", "\xe4\xba\x94", "\xe5\x85\xad",
    };
    for (int c = 0; c < CAL_GRID_COLS; c++) {
        int x = grid_x + c * col_w;
        if (c == 0)
            fb_fill_rect(fb, x, week_y, col_w, week_h, COLOR_RED);
        fb_utf8_scaled(fb, x + (col_w - 16) / 2,
                       week_y + (week_h - 16) / 2,
                       WD_SHORT[c], COLOR_WHITE, 1);
    }

    fb_rect(fb, grid_x, grid_top, grid_actual_w, grid_actual_h, COLOR_BLACK);
    if (big) {
        for (int r = 1; r < CAL_GRID_ROWS; r++)
            cal_draw_light_hline(fb, grid_x, grid_top + r * row_h,
                                 grid_actual_w, COLOR_BLACK);
        for (int c = 1; c < CAL_GRID_COLS; c++)
            cal_draw_light_vline(fb, grid_x + c * col_w, grid_top,
                                 grid_actual_h, COLOR_BLACK);
    }

    int dim = cal_days_in_month(year, month);
    int dow1 = cal_solar_weekday_sun0(year, month, 1);
    for (int cell = 0; cell < CAL_GRID_CELLS; cell++) {
        int d = cal_day_for_cell(cell, dow1, dim);
        if (d == 0)
            continue;

        int col = cell % CAL_GRID_COLS;
        int row = cell / CAL_GRID_COLS;
        cal_draw_overview_cell(fb, grid_x + col * col_w,
                               grid_top + row * row_h,
                               col_w, row_h, year, month, d, col,
                               today_y, today_m, today_d);
    }
}

static void cal_render_month(fb_t *fb, int year, int month,
                             int today_y, int today_m, int today_d)
{
    cal_layout_t L;
    cal_layout_build(&L, fb->width, fb->height);

    fb_clear(fb);
    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);

    cal_draw_title(fb, &L, year, month);
    cal_draw_weekday_row(fb, &L);
    cal_draw_grid_lines(fb, &L);

    int dim = cal_days_in_month(year, month);
    int dow1 = cal_solar_weekday_sun0(year, month, 1);

    for (int cell = 0; cell < CAL_GRID_CELLS; cell++) {
        int d = cal_day_for_cell(cell, dow1, dim);
        if (d == 0)
            continue;

        int col = cell % CAL_GRID_COLS;
        int row = cell / CAL_GRID_COLS;
        int cx = L.grid_x + col * L.col_w;
        int cy = L.grid_top + row * L.row_h;

        cal_draw_day_cell(fb, &L, year, month, d, col, cx, cy,
                          today_y, today_m, today_d);
    }

    char today[24];
    snprintf(today, sizeof(today), "%04d-%02d-%02d", today_y, today_m, today_d);
    ui_draw_footer(fb, "CALENDAR", today);
}

static void cal_state_note_shown(int year, int month, int day)
{
    portENTER_CRITICAL(&s_state_mux);
    s_last_year = year;
    s_last_month = month;
    s_last_day = day;
    portEXIT_CRITICAL(&s_state_mux);
}

static calendar_style_t cal_style_snapshot(void)
{
    calendar_style_t style;
    portENTER_CRITICAL(&s_state_mux);
    style = s_style;
    portEXIT_CRITICAL(&s_state_mux);
    return style;
}

static void cal_run_render(int year, int month, calendar_style_t style,
                           unsigned epoch)
{
    fb_t *fb = fb_create();
    if (!fb) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return;
    }

    int ty, tm, td;
    cal_today_ymd(&ty, &tm, &td);
    if (ty == 0 || tm == 0 || td == 0) {
        ESP_LOGW(TAG, "calendar render aborted: local time is not valid");
        fb_destroy(fb);
        return;
    }

    if (style == CAL_STYLE_OVERVIEW)
        cal_render_overview(fb, year, month, ty, tm, td);
    else
        cal_render_month(fb, year, month, ty, tm, td);

    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        ESP_LOGI(TAG, "skip stale calendar render %d-%02d", year, month);
        return;
    }

    esp_err_t err = epd_display_fb_free(fb);

    if (err == ESP_OK) {
        display_policy_set_manual_screen_active(true);
        ESP_LOGI(TAG, "shown %d-%02d style=%d", year, month, (int)style);
    } else {
        ESP_LOGE(TAG, "epd_display_from_buffer: %s", esp_err_to_name(err));
    }
}

/* -------------------------------------------------------------------------- */
/* 任务入口（避免在 HTTP 栈上大块分配 / 长时间刷屏）                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    int year;
    int month;
    calendar_style_t style;
    unsigned epoch;
} cal_task_arg_t;

static void calendar_task(void *arg)
{
    cal_task_arg_t *a = (cal_task_arg_t *)arg;
    cal_run_render(a->year, a->month, a->style, a->epoch);
    free(a);
    xSemaphoreGive(s_mutex);
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* 公开 API                                                                   */
/* -------------------------------------------------------------------------- */

esp_err_t calendar_display_show(int year, int month)
{
    if (!s_mutex)
        return ESP_ERR_INVALID_STATE;

    if (year == 0 || month == 0) {
        struct tm t;
        if (time_sync_get_local_relaxed(&t)) {
            if (year == 0)
                year = t.tm_year + 1900;
            if (month == 0)
                month = t.tm_mon + 1;
        } else {
            ESP_LOGW(TAG, "calendar show rejected: local time is not valid");
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (month < 1 || month > 12 || year < CAL_YEAR_MIN || year > CAL_YEAR_MAX)
        return ESP_ERR_INVALID_ARG;

    if (!epd_is_ready()) {
        ESP_LOGW(TAG, "EPD not ready");
        return ESP_ERR_INVALID_STATE;
    }

    unsigned epoch = display_policy_display_epoch();

    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "render already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    cal_task_arg_t *a = malloc(sizeof(*a));
    if (!a) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    a->year = year;
    a->month = month;
    a->style = cal_style_snapshot();
    a->epoch = epoch;

    if (xTaskCreate(calendar_task, "cal_render", 8192, a, 5, NULL) != pdPASS) {
        free(a);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }
    {
        int ty, tm, td;
        cal_today_ymd(&ty, &tm, &td);
        cal_state_note_shown(year, month, (ty == year && tm == month) ? td : 0);
    }
    return ESP_OK;
}

esp_err_t calendar_display_show_current(void)
{
    struct tm tm;
    if (!time_sync_get_local_relaxed(&tm)) {
        /* SNTP not yet synced — fall back to system localtime
         * (may be epoch 1970 on first boot, but better than refusing to render) */
        ESP_LOGW(TAG, "calendar show rejected: local time is not valid");
        return ESP_ERR_INVALID_STATE;
    }
    return calendar_display_show(tm.tm_year + 1900, tm.tm_mon + 1);
}

bool calendar_display_style_uses_weather(void)
{
    return cal_style_snapshot() == CAL_STYLE_OVERVIEW;
}

bool calendar_display_needs_midnight_refresh(void)
{
    struct tm tm;
    if (!time_sync_get_local_relaxed(&tm))
        return false;

    portENTER_CRITICAL(&s_state_mux);
    int ly = s_last_year;
    int lm = s_last_month;
    int ld = s_last_day;
    portEXIT_CRITICAL(&s_state_mux);

    int y = tm.tm_year + 1900;
    int m = tm.tm_mon + 1;
    int d = tm.tm_mday;
    return ly != y || lm != m || ld != d;
}

esp_err_t calendar_display_toggle_style(void)
{
    int year;
    int month;
    calendar_style_t style;

    portENTER_CRITICAL(&s_state_mux);
    s_style = (calendar_style_t)(((int)s_style + 1) % CAL_STYLE_COUNT);
    style = s_style;
    year = s_last_year;
    month = s_last_month;
    portEXIT_CRITICAL(&s_state_mux);

    cal_style_save_nvs(style);

    if (year == 0 || month == 0) {
        int day;
        cal_today_ymd(&year, &month, &day);
        if (year == 0 || month == 0)
            return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "toggle style -> %d", (int)style);
    return calendar_display_show(year, month);
}

void calendar_display_wait_render_idle(void)
{
    if (!s_mutex)
        return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    xSemaphoreGive(s_mutex);
}
