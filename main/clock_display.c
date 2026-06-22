#include "clock_display.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "fb_render.h"
#include "epd.h"
#include "scheduler.h"
#include "time_sync.h"
#include "weather.h"
#include "display_policy.h"
#include "display_mode.h"
#include "ui_theme.h"
#include "calendar_display.h"
#include "battery_mon.h"

/* 摘要区占位（无有效天气数据时） */
static const char k_wx_placeholder_l1[] =
    "\xe5\xa4\xa9\xe6\xb0\x94\xe6\x95\xb0\xe6\x8d\xae\xe6\x9c\xaa\xe5\xb0\xb1\xe7\xbb\xaa"; /* 天气数据未就绪 */
static const char k_wx_placeholder_l2[] =
    "\xe8\xaf\xb7\xe5\x9c\xa8\xe3\x80\x8c\xe5\xa4\xa9\xe6\xb0\x94\xe3\x80\x8d\xe9\xa1\xb5\xe5\xa1\xab API \xe5\x90\x8e\xe7\x82\xb9\xe5\x88\xb7\xe6\x96\xb0"; /* 请在「天气」页填 API 后点刷新 */

static const char *TAG    = "clock";
static const char *NVS_NS = "clock";

#define BIT_CFG_CHANGED   BIT0
#define BIT_WEATHER_DATA  BIT1
/* 纯唤醒位：用于让 clock_task 从 disabled 长睡里出来重新评估
 * （比如外部 clock_display_show 把模式切回时钟），但不强制渲染。 */
#define BIT_WAKE          BIT2

/* 时钟自动刷新的分钟粒度。
 * 1 = 每分钟全刷（时间始终准确，4.2" 约 10s / 5.83" 约 18s 刷新一次）；
 * 5 = 每 5 分钟全刷（23:00/05/10/15...）；
 * 整点/跨日/配置/天气变更仍立即全刷，与此值无关。 */
#define CLOCK_FULL_REFRESH_STEP_MIN   1

static clock_config_t s_cfg = {
    .enabled      = false,
    .style        = 0,   /* TODO: style > 0 not yet implemented in render_clock() */
    .show_weather = true,
};

static EventGroupHandle_t s_event;
static SemaphoreHandle_t  s_render_mutex;
static portMUX_TYPE       s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── 渲染状态镜像 ──────────────────────────────────────────────────
 *
 * 任务循环、外部 clock_display_show()、天气通知三条路径共用此状态。
 * 任何成功完成的 render_clock() 都会更新它，保证 task 不会再多刷一次
 * （之前 task 与 show 不互通，导致一次配置保存 + 一次 weather 通知能在
 * 同一次循环里把同一分钟刷 2 次）。
 *
 * 写入：render_clock 末尾、disabled→idle 复位、weather 通知去重。
 * 读取：clock_task 顶部决策。
 */
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    int  last_minute;        /* tm.tm_min of last successful render, -1 = never */
    int  last_hour;
    int  last_mday;
    int  last_weather_sig;   /* 见 compute_weather_sig()，-1 = 无数据 */
    bool last_show_weather;  /* 上次渲染时 show_weather 的实际值 */
    bool force_full_next;    /* 下一次渲染是否强制全刷 */
} s_state = {
    .last_minute = -1,
    .last_hour = -1,
    .last_mday = -1,
    .last_weather_sig = -1,
    .last_show_weather = false,
    .force_full_next = true,
};

/* 计算天气数据签名：temp/icon/humidity/daily_count 拼成 32 位整型。
 * 只在显示天气时用来判断"是否真正变了"，避免每次 BIT_WEATHER_DATA 通知都强制全刷。 */
static int compute_weather_sig(void)
{
    return weather_data_signature();
}

static void state_reset(void)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state.last_minute = -1;
    s_state.last_hour = -1;
    s_state.last_mday = -1;
    s_state.last_weather_sig = -1;
    s_state.last_show_weather = false;
    s_state.force_full_next = true;
    portEXIT_CRITICAL(&s_state_mux);
}

static bool clock_get_local_or_system(struct tm *out)
{
    if (!out)
        return false;
    if (time_sync_get_local_relaxed(out))
        return true;

    time_t now = time(NULL);
    localtime_r(&now, out);
    return false;
}

/* ── time headline rendering ─────────────────────────────────────── */

/*
 * Original Gitee clock face: seven-segment headline digits.
 * Keep this isolated from the generic styled font path so other pages can use
 * normal large text without changing the clock identity.
 */
#define SEG_A 0x01
#define SEG_B 0x02
#define SEG_C 0x04
#define SEG_D 0x08
#define SEG_E 0x10
#define SEG_F 0x20
#define SEG_G 0x40

static const uint8_t digit_segs[10] = {
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F,        /* 0 */
    SEG_B|SEG_C,                                /* 1 */
    SEG_A|SEG_B|SEG_D|SEG_E|SEG_G,              /* 2 */
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_G,              /* 3 */
    SEG_B|SEG_C|SEG_F|SEG_G,                    /* 4 */
    SEG_A|SEG_C|SEG_D|SEG_F|SEG_G,              /* 5 */
    SEG_A|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,        /* 6 */
    SEG_A|SEG_B|SEG_C,                          /* 7 */
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,  /* 8 */
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_F|SEG_G,        /* 9 */
};

static const int seg_base[7][4] = {
    { 10,  0, 36,  8 },   /* A */
    { 48, 10,  8, 31 },   /* B */
    { 48, 49,  8, 31 },   /* C */
    { 10, 80, 36,  8 },   /* D */
    {  0, 49,  8, 31 },   /* E */
    {  0, 10,  8, 31 },   /* F */
    { 10, 40, 36,  8 },   /* G */
};

#define BASE_DW   56
#define BASE_DH   88
#define BASE_COL  18

static void clock_fill_beveled_rect(fb_t *fb, int x, int y, int w, int h,
                                    fb_color_t color)
{
    if (!fb || w <= 0 || h <= 0)
        return;
    fb_fill_rect(fb, x, y, w, h, color);

    int bevel = (w < h ? w : h) / 2;
    if (bevel < 2)
        return;
    for (int i = 0; i < bevel; i++) {
        int len = bevel - i;
        fb_hline(fb, x, y + i, len, COLOR_WHITE);
        fb_hline(fb, x + w - len, y + i, len, COLOR_WHITE);
        fb_hline(fb, x, y + h - 1 - i, len, COLOR_WHITE);
        fb_hline(fb, x + w - len, y + h - 1 - i, len, COLOR_WHITE);
    }
}

static void draw_digit_s(fb_t *fb, int x, int y, int digit,
                         fb_color_t color, int num, int den)
{
    if (digit < 0 || digit > 9)
        return;
    uint8_t segs = digit_segs[digit];
    for (int s = 0; s < 7; s++) {
        if (segs & (1 << s)) {
            clock_fill_beveled_rect(fb,
                                    x + seg_base[s][0] * num / den,
                                    y + seg_base[s][1] * num / den,
                                    seg_base[s][2] * num / den,
                                    seg_base[s][3] * num / den,
                                    color);
        }
    }
}

static void draw_colon_s(fb_t *fb, int x, int y, fb_color_t color,
                         int num, int den)
{
    int dh = BASE_DH * num / den;
    int dot = 8 * num / den;
    if (dot < 4)
        dot = 4;
    int cx = x + 4 * num / den;
    fb_fill_rect(fb, cx, y + dh * 3 / 10, dot, dot, color);
    fb_fill_rect(fb, cx, y + dh * 7 / 10, dot, dot, color);
}

static int clock_text_px(const char *s, int scale)
{
    if (scale < 1)
        scale = 1;
    return ui_text_width(s, scale);
}

static int clock_draw_text(fb_t *fb, int x, int y, const char *s,
                           fb_color_t color, int scale)
{
    if (scale < 1)
        scale = 1;
    return fb_utf8_scaled_builtin(fb, x, y, s, color, scale);
}

static int clock_draw_text_maxw(fb_t *fb, int x, int y, const char *s,
                                fb_color_t color, int scale, int max_w)
{
    if (scale < 1)
        scale = 1;
    return fb_utf8_scaled_builtin_maxw(fb, x, y, s, color, scale, max_w);
}

static void clock_draw_footer(fb_t *fb, const char *left, const char *right)
{
    if (!fb)
        return;

    const int W = fb->width;
    const int H = fb->height;
    const int s = ui_scale_for(fb);
    const int y = H - 20 * s;

    ui_draw_dotted_hline(fb, 12 * s, y - 4 * s, W - 24 * s,
                         COLOR_BLACK, 6);

    int left_max_w = W - 28 * s;
    if (right && right[0]) {
        int rw = clock_text_px(right, 1);
        left_max_w = W - 34 * s - rw;
    }
    if (left && left[0] && left_max_w > 0)
        clock_draw_text_maxw(fb, 14 * s, y, left, COLOR_BLACK, 1,
                             left_max_w);

    if (right && right[0]) {
        int rw = clock_text_px(right, 1);
        clock_draw_text(fb, W - 14 * s - rw, y, right, COLOR_BLACK, 1);
    }
}

/* ── weekday strings ──────────────────────────────────────────────── */

static const char *weekday_zh[] = {
    "\xe6\x97\xa5",  /* 日 */
    "\xe4\xb8\x80",  /* 一 */
    "\xe4\xba\x8c",  /* 二 */
    "\xe4\xb8\x89",  /* 三 */
    "\xe5\x9b\x9b",  /* 四 */
    "\xe4\xba\x94",  /* 五 */
    "\xe5\x85\xad",  /* 六 */
};

/* ── NVS ──────────────────────────────────────────────────────────── */

static void clock_plot_circle_points(fb_t *fb, int cx, int cy, int x, int y,
                                     fb_color_t color)
{
    fb_pixel(fb, cx + x, cy + y, color);
    fb_pixel(fb, cx - x, cy + y, color);
    fb_pixel(fb, cx + x, cy - y, color);
    fb_pixel(fb, cx - x, cy - y, color);
    fb_pixel(fb, cx + y, cy + x, color);
    fb_pixel(fb, cx - y, cy + x, color);
    fb_pixel(fb, cx + y, cy - x, color);
    fb_pixel(fb, cx - y, cy - x, color);
}

static void clock_draw_circle(fb_t *fb, int cx, int cy, int r, int thickness,
                              fb_color_t color)
{
    if (!fb || r <= 0)
        return;
    if (thickness < 1)
        thickness = 1;
    for (int rr = r; rr > r - thickness && rr > 0; rr--) {
        int x = 0;
        int y = rr;
        int d = 1 - rr;
        while (x <= y) {
            clock_plot_circle_points(fb, cx, cy, x, y, color);
            if (d < 0) {
                d += 2 * x + 3;
            } else {
                d += 2 * (x - y) + 5;
                y--;
            }
            x++;
        }
    }
}

static void clock_draw_calendar_icon(fb_t *fb, int x, int y, fb_color_t color)
{
    fb_rect(fb, x, y + 4, 18, 18, color);
    fb_hline(fb, x + 2, y + 9, 14, color);
    fb_fill_rect(fb, x + 4, y, 2, 7, color);
    fb_fill_rect(fb, x + 12, y, 2, 7, color);
    fb_fill_rect(fb, x + 4, y + 13, 3, 3, color);
    fb_fill_rect(fb, x + 10, y + 13, 3, 3, color);
    fb_fill_rect(fb, x + 4, y + 18, 3, 2, color);
    fb_fill_rect(fb, x + 10, y + 18, 3, 2, color);
}

static void clock_draw_status_clock_icon(fb_t *fb, int cx, int cy)
{
    clock_draw_circle(fb, cx, cy, 20, 3, COLOR_RED);
    fb_fill_rect(fb, cx - 2, cy - 13, 4, 15, COLOR_RED);
    for (int i = 0; i < 11; i++)
        fb_fill_rect(fb, cx + i, cy + i / 2, 3, 3, COLOR_RED);
}

static void clock_draw_ref_card(fb_t *fb, int x, int y, int w, int h)
{
    const int r = 5;
    fb_hline(fb, x + r, y, w - 2 * r, COLOR_BLACK);
    fb_hline(fb, x + r, y + h - 1, w - 2 * r, COLOR_BLACK);
    fb_vline(fb, x, y + r, h - 2 * r, COLOR_BLACK);
    fb_vline(fb, x + w - 1, y + r, h - 2 * r, COLOR_BLACK);

    fb_pixel(fb, x + 2, y + 1, COLOR_BLACK);
    fb_pixel(fb, x + 1, y + 2, COLOR_BLACK);
    fb_pixel(fb, x + 1, y + 3, COLOR_BLACK);
    fb_pixel(fb, x + 2, y + h - 2, COLOR_BLACK);
    fb_pixel(fb, x + 1, y + h - 3, COLOR_BLACK);
    fb_pixel(fb, x + 1, y + h - 4, COLOR_BLACK);

    fb_pixel(fb, x + w - 3, y + 1, COLOR_BLACK);
    fb_pixel(fb, x + w - 2, y + 2, COLOR_BLACK);
    fb_pixel(fb, x + w - 2, y + 3, COLOR_BLACK);
    fb_pixel(fb, x + w - 3, y + h - 2, COLOR_BLACK);
    fb_pixel(fb, x + w - 2, y + h - 3, COLOR_BLACK);
    fb_pixel(fb, x + w - 2, y + h - 4, COLOR_BLACK);
}

static void clock_draw_ref_battery(fb_t *fb, int right_x, int y)
{
    battery_status_t st = {0};
    battery_mon_get_status(&st);

    char label[8];
    if (!st.valid)
        snprintf(label, sizeof(label), "--");
    else
        snprintf(label, sizeof(label), "%u%%", (unsigned)st.percent);

    const int icon_w = 22;
    const int icon_h = 10;
    const int cap_w = 3;
    const int gap = 4;
    const int text_w = clock_text_px(label, 1);
    const int x = right_x - icon_w - cap_w - gap - text_w;
    const int icon_x = x;
    const int icon_y = y + 3;

    fb_rect(fb, icon_x, icon_y, icon_w, icon_h, COLOR_BLACK);
    fb_fill_rect(fb, icon_x + icon_w, icon_y + 3, cap_w, icon_h - 6, COLOR_BLACK);
    if (st.valid) {
        const int inner_w = icon_w - 4;
        fb_fill_rect(fb, icon_x + 2, icon_y + 2, inner_w, icon_h - 4, COLOR_BLACK);
        int remain = inner_w * st.percent / 100;
        if (st.percent > 0 && remain < 1)
            remain = 1;
        if (remain > inner_w)
            remain = inner_w;
        if (remain > 0)
            fb_fill_rect(fb, icon_x + 2, icon_y + 2, remain, icon_h - 4, COLOR_RED);
    } else {
        fb_hline(fb, icon_x + 5, icon_y + 4, 10, COLOR_RED);
        fb_hline(fb, icon_x + 7, icon_y + 6, 10, COLOR_RED);
    }
    clock_draw_text(fb, icon_x + icon_w + cap_w + gap, y + 1,
                    label, COLOR_BLACK, 1);
}

static void render_clock_42_reference(fb_t *fb, const struct tm *tm)
{
    const int W = fb->width;
    const int ds_num = 2;
    const int ds_den = 2;
    const int dw = BASE_DW * ds_num / ds_den;
    const int dh = BASE_DH * ds_num / ds_den;
    const int d_gap = dw + 6 * ds_num / ds_den;
    const int col_w_px = BASE_COL * ds_num / ds_den;
    const int time_w = 3 * d_gap + col_w_px + dw;

    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
    clock_draw_ref_battery(fb, W - 11, 15);

    int hour = tm->tm_hour;
    int min = tm->tm_min;
    int time_y = 50;

    char date_str[64];
    snprintf(date_str, sizeof(date_str),
             "%04d\xe5\xb9\xb4%02d\xe6\x9c\x88%02d\xe6\x97\xa5",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);

    int wday = tm->tm_wday;
    if (wday < 0 || wday > 6)
        wday = 0;
    char week_str[32];
    snprintf(week_str, sizeof(week_str),
             "\xe6\x98\x9f\xe6\x9c\x9f%s", weekday_zh[wday]);

    const int date_y = time_y + dh + 22;
    const int date_w = clock_text_px(date_str, 1);
    const int week_w = clock_text_px(week_str, 1);
    const int icon_w = 22;
    const int gap1 = 12;
    const int gap2 = 16;
    const int row_w = icon_w + gap1 + date_w + gap2 + 1 + gap2 + week_w;
    int row_x = (W - row_w) / 2;
    if (row_x < 20)
        row_x = 20;

    int time_x = row_x + row_w / 2 - time_w / 2;
    if (time_x < 18)
        time_x = 18;
    int cx = time_x;
    draw_digit_s(fb, cx, time_y, hour / 10, COLOR_BLACK, ds_num, ds_den);
    cx += d_gap;
    draw_digit_s(fb, cx, time_y, hour % 10, COLOR_BLACK, ds_num, ds_den);
    cx += d_gap;
    draw_colon_s(fb, cx, time_y, COLOR_RED, ds_num, ds_den);
    cx += col_w_px;
    draw_digit_s(fb, cx, time_y, min / 10, COLOR_BLACK, ds_num, ds_den);
    cx += d_gap;
    draw_digit_s(fb, cx, time_y, min % 10, COLOR_BLACK, ds_num, ds_den);

    clock_draw_calendar_icon(fb, row_x, date_y - 3, COLOR_RED);
    int text_x = row_x + icon_w + gap1;
    clock_draw_text(fb, text_x, date_y, date_str, COLOR_BLACK, 1);
    int bar_x = text_x + date_w + gap2;
    fb_vline(fb, bar_x, date_y - 1, 19, COLOR_BLACK);
    clock_draw_text(fb, bar_x + gap2, date_y, week_str, COLOR_RED, 1);

    const int line_y = date_y + 31;
    fb_fill_rect(fb, 20, line_y, W - 40, 2, COLOR_RED);

    const int card_x = 22;
    const int card_y = line_y + 15;
    const int card_w = W - 2 * card_x;
    const int card_h = 78;
    clock_draw_ref_card(fb, card_x, card_y, card_w, card_h);

    int red_x = card_x + card_w - 5;
    int red_y = card_y + 27;
    int red_h = card_h - 27;
    fb_fill_rect(fb, red_x, red_y + 3, 5, red_h - 6, COLOR_RED);
    fb_hline(fb, red_x + 1, red_y + 1, 4, COLOR_RED);
    fb_hline(fb, red_x, red_y + 2, 5, COLOR_RED);
    fb_hline(fb, red_x, red_y + red_h - 3, 5, COLOR_RED);
    fb_hline(fb, red_x + 1, red_y + red_h - 2, 4, COLOR_RED);

    int icon_cx = card_x + 38;
    int icon_cy = card_y + card_h / 2;
    clock_draw_status_clock_icon(fb, icon_cx, icon_cy);

    int split_x = card_x + 76;
    fb_vline(fb, split_x, card_y + 13, card_h - 24, COLOR_BLACK);

    char ampm[16];
    if (hour < 6)       snprintf(ampm, sizeof(ampm), "\xe5\x87\x8c\xe6\x99\xa8");
    else if (hour < 12) snprintf(ampm, sizeof(ampm), "\xe4\xb8\x8a\xe5\x8d\x88");
    else if (hour < 18) snprintf(ampm, sizeof(ampm), "\xe4\xb8\x8b\xe5\x8d\x88");
    else                snprintf(ampm, sizeof(ampm), "\xe6\x99\x9a\xe4\xb8\x8a");

    const char *tag = time_sync_is_synced()
                          ? "\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe5\x90\x8c\xe6\xad\xa5"
                          : "\xe7\xad\x89\xe5\xbe\x85\xe7\xbd\x91\xe7\xbb\x9c\xe6\xa0\xa1\xe6\x97\xb6";
    int info_x = split_x + 20;
    int info_w = card_x + card_w - 20 - info_x;
    clock_draw_text_maxw(fb, info_x, card_y + 12, ampm, COLOR_BLACK, 2, info_w);
    clock_draw_text_maxw(fb, info_x, card_y + 50, tag,
                         time_sync_is_synced() ? COLOR_BLACK : COLOR_RED,
                         1, info_w);
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t v8;
    if (nvs_get_u8(h, "enabled", &v8) == ESP_OK) s_cfg.enabled = (v8 != 0);
    if (nvs_get_u8(h, "style", &v8) == ESP_OK) s_cfg.style = v8;
    if (nvs_get_u8(h, "show_w", &v8) == ESP_OK) s_cfg.show_weather = (v8 != 0);

    nvs_close(h);
}

static void nvs_save_snapshot(const clock_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "enabled", cfg->enabled ? 1 : 0);
    nvs_set_u8(h, "style",   cfg->style);
    nvs_set_u8(h, "show_w",  cfg->show_weather ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

/* ── render clock ─────────────────────────────────────────────────── */

/**
 * 渲染时钟到帧缓冲并送屏。
 *
 * 三色屏不同控制器波形差异较大，本模块统一走全屏刷新，画面质量优先。
 */
static esp_err_t render_clock(unsigned epoch, bool notify_scheduler)
{
    if (s_render_mutex && xSemaphoreTake(s_render_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "render_clock: another render in progress, skip");
        return ESP_ERR_TIMEOUT;
    }

    /* 在 spinlock 保护下取一份配置快照，整次渲染使用 cfg_local 而非 s_cfg，
     * 避免与 HTTP POST 同时改 s_cfg 时撕裂读取（show_weather 等字段）。 */
    clock_config_t cfg_local;
    portENTER_CRITICAL(&s_cfg_mux);
    cfg_local = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);

    fb_t *fb = fb_create();
    if (!fb) {
        ESP_LOGE(TAG, "fb_create failed");
        if (s_render_mutex)
            xSemaphoreGive(s_render_mutex);
        return ESP_ERR_NO_MEM;
    }

    struct tm tm;
    if (!clock_get_local_or_system(&tm)) {
        ESP_LOGW(TAG, "SNTP not synced, using system time");
    }

    int hour = tm.tm_hour;
    int min  = tm.tm_min;

    const int W = fb->width;
    const int H = fb->height;
    const int MX = 20;
    const bool clock42_reference = (W == 400 && H == 300 && !cfg_local.show_weather);
    if (clock42_reference) {
        render_clock_42_reference(fb, &tm);
        goto clock_render_done;
    }

    const bool big = (W >= 600);
    const bool compact_clock = (!big && W >= 360 && H >= 260);
    const bool plain_compact_clock = compact_clock && !cfg_local.show_weather;
    const int sc = big ? 2 : 1;
    const int ds_num = big ? 3 : 2;
    const int ds_den = big ? 2 : 2;
    int dw = BASE_DW * ds_num / ds_den;
    int dh = BASE_DH * ds_num / ds_den;
    int d_gap = dw + 6 * ds_num / ds_den;
    int col_w_px = BASE_COL * ds_num / ds_den;

    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);

    int total_w = 4 * d_gap + col_w_px - 6 * ds_num / ds_den;
    int time_x = (W - total_w) / 2;
    int time_y = H * 34 / 480;
    if (compact_clock) {
        if (!plain_compact_clock)
            time_x -= 12;
        if (time_x < 16)
            time_x = 16;
        time_y = plain_compact_clock ? 22 : 16;
    }

    int cx = time_x;
    draw_digit_s(fb, cx, time_y, hour / 10, COLOR_BLACK, ds_num, ds_den);
    cx += d_gap;
    draw_digit_s(fb, cx, time_y, hour % 10, COLOR_BLACK, ds_num, ds_den);
    cx += d_gap;
    draw_colon_s(fb, cx, time_y, COLOR_BLACK, ds_num, ds_den);
    cx += col_w_px;
    draw_digit_s(fb, cx, time_y, min / 10, COLOR_BLACK, ds_num, ds_den);
    cx += d_gap;
    draw_digit_s(fb, cx, time_y, min % 10, COLOR_BLACK, ds_num, ds_den);

    char date_str[64];
    snprintf(date_str, sizeof(date_str),
             "%04d\xe5\xb9\xb4%02d\xe6\x9c\x88%02d\xe6\x97\xa5",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    char week_str[32];
    int wday = tm.tm_wday;
    if (wday < 0 || wday > 6) wday = 0;
    snprintf(week_str, sizeof(week_str),
             "\xe6\x98\x9f\xe6\x9c\x9f%s",
             weekday_zh[wday]);

    int date_y = time_y + dh + (compact_clock ? 10 : 18);
    int date_w = clock_text_px(date_str, sc);
    int week_w = clock_text_px(week_str, sc);
    int date_gap = big ? 22 * sc : 18;
    if (compact_clock)
        date_gap = 16;
    int date_x = (W - date_w - week_w - date_gap) / 2;
    if (compact_clock && cfg_local.show_weather)
        date_x = MX + 32;
    if (date_x < 16)
        date_x = 16;
    clock_draw_text(fb, date_x, date_y, date_str, COLOR_BLACK, sc);
    clock_draw_text(fb, date_x + date_w + date_gap, date_y, week_str, COLOR_RED, sc);
    if (big)
        ui_draw_battery_badge(fb, date_x - 10, date_y + (sc > 1 ? 3 : 0));
    else {
        int battery_y = compact_clock ? 18
                                      : (cfg_local.show_weather ? (date_y - 18) : (H - 34));
        ui_draw_battery_badge(fb, compact_clock ? (W - 18) : (W - 28), battery_y);
    }

    weather_summary_t wd_local = {0};
    weather_config_t wcfg = {0};
    bool has_weather = false;

    if (cfg_local.show_weather) {
        weather_get_summary_copy(&wd_local);
        if (wd_local.valid) {
            weather_get_config(&wcfg);
            has_weather = true;
        }
    }
    const weather_summary_t *wd = &wd_local;

    int sep_y = date_y + 16 * sc + (compact_clock ? 9 : 12);
    if (big || has_weather || cfg_local.show_weather) {
        ui_draw_dotted_hline(fb, MX + 12, sep_y, W - 2 * MX - 24, COLOR_BLACK, 6);
    } else {
        fb_fill_rect(fb, W / 2 - 34, sep_y, 68, 2, COLOR_RED);
    }

    int bot_y = sep_y + (compact_clock ? 12 : 14);
    const int wx_head_sc = big ? 2 : 1;
    const int wx_body_sc = 1;
    const int wx_head_gap = 16 * wx_head_sc + 8;

    if (has_weather) {
        if (compact_clock) {
            const int current_y = bot_y;
            const int icon_x = MX + 18;
            const int icon_y = current_y + 13;
            const int left_x = icon_x + WEATHER_ICON_W + 10;
            const int split_x = W / 2 + 8;
            const int right_x = split_x + 16;

            fb_bitmap(fb, icon_x, icon_y, WEATHER_ICON_W, WEATHER_ICON_H,
                      weather_icon_bitmap(wd->now.icon), COLOR_BLACK);
            ui_draw_dotted_vline(fb, split_x, current_y + 3, 55, COLOR_BLACK, 6);

            char place[80];
            if (wcfg.city_name[0]) {
                snprintf(place, sizeof(place), "%.31s", wcfg.city_name);
            } else {
                snprintf(place, sizeof(place), "%s",
                         "\xe5\xbd\x93\xe5\x89\x8d\xe5\xa4\xa9\xe6\xb0\x94");
            }

            char now_line[80];
            snprintf(now_line, sizeof(now_line), "%.23s  %d\xc2\xb0""C",
                     wd->now.text, wd->now.temp);

            fb_utf8_scaled_maxw(fb, left_x, current_y + 3, place,
                                COLOR_BLACK, 1, split_x - left_x - 10);
            fb_utf8_scaled_maxw(fb, left_x, current_y + 27, now_line,
                                COLOR_RED, 1, split_x - left_x - 10);

            char feels[64];
            char humid[64];
            char wind[80];
            snprintf(feels, sizeof(feels),
                     "\xe4\xbd\x93\xe6\x84\x9f %d\xc2\xb0""C",
                     wd->now.feels_like);
            snprintf(humid, sizeof(humid),
                     "\xe6\xb9\xbf\xe5\xba\xa6 %d%%",
                     wd->now.humidity);
            snprintf(wind, sizeof(wind), "%.15s %d\xe7\xba\xa7",
                     wd->now.wind_dir, wd->now.wind_scale);

            fb_utf8_scaled_maxw(fb, right_x, current_y + 0, feels,
                                COLOR_BLACK, 1, W - right_x - MX);
            fb_utf8_scaled_maxw(fb, right_x, current_y + 20, humid,
                                COLOR_BLACK, 1, W - right_x - MX);
            fb_utf8_scaled_maxw(fb, right_x, current_y + 40, wind,
                                COLOR_BLACK, 1, W - right_x - MX);

            int dot_y = current_y + 66;
            ui_draw_dotted_hline(fb, MX + 10, dot_y, W - 2 * MX - 20,
                                 COLOR_BLACK, 6);

            const int gap = 8;
            const int fcol = (W - 2 * MX - 2 * gap) / 3;
            const int row_y = dot_y + 9;
            for (int i = 0; i < wd->daily_count && i < 3; i++) {
                int fx = MX + i * (fcol + gap);
                if (i > 0) {
                    ui_draw_dotted_vline(fb, fx - gap / 2, row_y + 2, 38,
                                         COLOR_BLACK, 6);
                }

                char label[24];
                if (i == 0) {
                    snprintf(label, sizeof(label), "%s", "\xe4\xbb\x8a\xe5\xa4\xa9");
                } else {
                    const char *d = wd->daily[i].date;
                    if (!d) {
                        label[0] = '\0';
                    } else if (strlen(d) >= 10) {
                        snprintf(label, sizeof(label), "%s", d + 5);
                    } else {
                        snprintf(label, sizeof(label), "%s", d);
                    }
                }

                char detail[80];
                snprintf(detail, sizeof(detail), "%s %d/%d",
                         wd->daily[i].text_day,
                         wd->daily[i].temp_max, wd->daily[i].temp_min);

                int icon_fx = fx + fcol - WEATHER_ICON_W - 6;
                fb_bitmap(fb, icon_fx, row_y + 4, WEATHER_ICON_W, WEATHER_ICON_H,
                          weather_icon_bitmap(wd->daily[i].icon_day), COLOR_BLACK);

                int text_w = icon_fx - fx - 5;
                if (text_w < 32)
                    text_w = fcol - 8;
                fb_utf8_scaled_maxw(fb, fx + 4, row_y, label,
                                    (i == 0) ? COLOR_RED : COLOR_BLACK,
                                    1, text_w);
                fb_utf8_scaled_maxw(fb, fx + 4, row_y + 21, detail,
                                    COLOR_BLACK, 1, text_w);
            }
        } else {
        char line1[280];
        if (wcfg.city_name[0]) {
            snprintf(line1, sizeof(line1), "%.31s  %.23s  %d\xc2\xb0""C",
                     wcfg.city_name, wd->now.text, wd->now.temp);
        } else {
            snprintf(line1, sizeof(line1), "%.23s  %d\xc2\xb0""C",
                     wd->now.text, wd->now.temp);
        }
        {
            int icon_x = W * 66 / 648;
            int icon_y = bot_y + (16 * wx_head_sc - WEATHER_ICON_H) / 2;
            if (icon_y < bot_y)
                icon_y = bot_y;
            fb_bitmap(fb, icon_x, icon_y, WEATHER_ICON_W, WEATHER_ICON_H,
                      weather_icon_bitmap(wd->now.icon), COLOR_BLACK);

            int lx = icon_x + WEATHER_ICON_W + 10;
            int lw = W - lx - MX - 8;
            if (lw < 24) lw = 24;
            fb_utf8_scaled_maxw(fb, lx, bot_y, line1, COLOR_BLACK, wx_head_sc, lw);
        }

        char line2[280];
        snprintf(line2, sizeof(line2),
                 "\xe4\xbd\x93\xe6\x84\x9f%d\xc2\xb0""C  "
                 "\xe6\xb9\xbf\xe5\xba\xa6%d%%  "
                 "%.15s%d\xe7\xba\xa7",
                 wd->now.feels_like,
                 wd->now.humidity,
                 wd->now.wind_dir, wd->now.wind_scale);
        {
            int lx2 = W * 80 / 648;
            int lw2 = W - lx2 - MX - 8;
            if (lw2 < 24) lw2 = 24;
            fb_utf8_scaled_maxw(fb, lx2, bot_y + wx_head_gap, line2, COLOR_BLACK, wx_head_sc, lw2);
        }

        int dot_y = bot_y + 2 * wx_head_gap + 2;
        ui_draw_dotted_hline(fb, MX + 10, dot_y, W - 2 * MX - 20, COLOR_BLACK, 6);

        int fcol = (W - 2 * MX - 40) / 3;
        if (fcol < 32) fcol = 32;
        const int fpad = 6;
        const int max_fc = fcol - fpad;
        int row_y = dot_y + 10;
        const int line_gap = 16 * wx_body_sc + 4;

        for (int i = 0; i < wd->daily_count && i < 3; i++) {
            int fx = MX + 20 + i * fcol;
            char line_a[64];
            char line_b[48];

            if (i == 0) {
                snprintf(line_a, sizeof(line_a),
                         "\xe4\xbb\x8a\xe5\xa4\xa9 %d/%d\xc2\xb0""C",
                         wd->daily[i].temp_max, wd->daily[i].temp_min);
            } else {
                const char *d = wd->daily[i].date;
                char md[16];
                if (!d) {
                    md[0] = '\0';
                } else if (strlen(d) >= 10) {
                    snprintf(md, sizeof(md), "%s", d + 5);
                } else {
                    snprintf(md, sizeof(md), "%s", d);
                }
                snprintf(line_a, sizeof(line_a), "%.15s %d/%d\xc2\xb0""C",
                         md, wd->daily[i].temp_max, wd->daily[i].temp_min);
            }
            snprintf(line_b, sizeof(line_b), "%.23s", wd->daily[i].text_day);

            int icon_x = fx + max_fc - WEATHER_ICON_W;
            if (icon_x < fx)
                icon_x = fx;
            fb_bitmap(fb, icon_x, row_y + 1, WEATHER_ICON_W, WEATHER_ICON_H,
                      weather_icon_bitmap(wd->daily[i].icon_day), COLOR_BLACK);

            int text_max = max_fc - WEATHER_ICON_W - 6;
            if (text_max < 32)
                text_max = max_fc;
            fb_utf8_scaled_maxw(fb, fx, row_y, line_a,
                                (i == 0) ? COLOR_RED : COLOR_BLACK,
                                wx_body_sc, text_max);
            fb_utf8_scaled_maxw(fb, fx, row_y + line_gap, line_b,
                                COLOR_BLACK, wx_body_sc, text_max);
        }
        }
    } else if (cfg_local.show_weather) {
        /* 已勾选「显示天气摘要」但尚未拉到 API 数据：提示原因，避免误以为开关无效 */
        int lx = MX + 16;
        int lw = W - 2 * MX - 32;
        if (lw < 24) lw = 24;
        fb_utf8_scaled_maxw(fb, lx, bot_y, k_wx_placeholder_l1,
                            COLOR_BLACK, wx_body_sc, lw);
        fb_utf8_scaled_maxw(fb, lx, bot_y + 16 * wx_body_sc + 6,
                            k_wx_placeholder_l2, COLOR_BLACK, wx_body_sc, lw);
    } else {
        char ampm[16];
        if (hour < 6)       snprintf(ampm, sizeof(ampm), "\xe5\x87\x8c\xe6\x99\xa8");
        else if (hour < 12) snprintf(ampm, sizeof(ampm), "\xe4\xb8\x8a\xe5\x8d\x88");
        else if (hour < 18) snprintf(ampm, sizeof(ampm), "\xe4\xb8\x8b\xe5\x8d\x88");
        else                snprintf(ampm, sizeof(ampm), "\xe6\x99\x9a\xe4\xb8\x8a");
        if (!big) {
            const int card_x = 24;
            const int card_y = bot_y + (plain_compact_clock ? 16 : 6);
            const int card_w = W - 2 * card_x;
            const int card_h = 72;
            ui_draw_card(fb, card_x, card_y, card_w, card_h, true);

            const char *tag = time_sync_is_synced()
                                  ? "\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe5\x90\x8c\xe6\xad\xa5"
                                  : "\xe7\xad\x89\xe5\xbe\x85\xe7\xbd\x91\xe7\xbb\x9c\xe6\xa0\xa1\xe6\x97\xb6";
            int aw = clock_text_px(ampm, 2);
            int tw = clock_text_px(tag, 1);
            clock_draw_text(fb, (W - aw) / 2, card_y + 14, ampm, COLOR_BLACK, 2);
            clock_draw_text(fb, (W - tw) / 2, card_y + 48, tag,
                            time_sync_is_synced() ? COLOR_BLACK : COLOR_RED, 1);
        } else {
            ui_draw_dotted_hline(fb, W / 2 - 70, bot_y + 30, 140, COLOR_BLACK, 6);
            int aw = clock_text_px(ampm, sc);
            clock_draw_text(fb, (W - aw) / 2, bot_y + 50, ampm, COLOR_BLACK, sc);
        }
    }

    if (big || has_weather || cfg_local.show_weather)
        clock_draw_footer(fb, "CLOCK", cfg_local.show_weather ? "WEATHER" : "TIME");

clock_render_done:
    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        if (s_render_mutex)
            xSemaphoreGive(s_render_mutex);
        ESP_LOGI(TAG, "Clock display skipped before EPD refresh: stale request");
        return ESP_ERR_INVALID_STATE;
    }

    /* Display directly from RAM; clock refreshes should not rewrite /spiffs/image.bin. */
    esp_err_t err = epd_display_fb_free(fb);
    if (err != ESP_OK) {
        if (s_render_mutex)
            xSemaphoreGive(s_render_mutex);
        return err;
    }

    if (notify_scheduler)
        scheduler_notify_manual_show();
    ESP_LOGI(TAG, "Clock displayed: %02d:%02d", hour, min);

    /* 同步全局渲染状态：任何成功的 render_clock() 都更新它，
     * 这样 task 与外部 show 共用一份"最近渲染了什么"，不会再各自重复刷屏。 */
    int sig_now = cfg_local.show_weather ? compute_weather_sig() : -1;
    portENTER_CRITICAL(&s_state_mux);
    s_state.last_minute = tm.tm_min;
    s_state.last_hour = tm.tm_hour;
    s_state.last_mday = tm.tm_mday;
    s_state.last_show_weather = cfg_local.show_weather;
    if (sig_now != -1)
        s_state.last_weather_sig = sig_now;
    s_state.force_full_next = false;
    portEXIT_CRITICAL(&s_state_mux);

    if (s_render_mutex)
        xSemaphoreGive(s_render_mutex);
    return ESP_OK;
}

/* ── auto-refresh task ────────────────────────────────────────────── */

/*
 * 重写说明（2026-04-26）：
 *
 * 旧实现把 minute-check 和 bits-handler 写成两段独立路径，会在同一次循环里
 * 触发两次渲染（典型场景：配置保存→BIT_CFG_CHANGED 唤醒任务→minute path 全刷
 * 22:19→等 bits→BIT_WEATHER_DATA 已 pending→bit handler 又全刷 22:19），
 * 浪费 18.8s × N 次。
 *
 * 新实现：每轮顶部读完所有信号 + 时间，做 ONE 次决策，最多 ONE 次渲染。
 *
 *   - 等 bits（带 30~60s 超时，对齐到下一分钟边界）；
 *   - 唤醒后核对：分钟变化 / 整点 / 日期 / 配置 / 天气可见性 / 天气数据签名 /
 *     force_full_next（时间重同步等）；
 *   - 时钟一律全刷（BWR 局刷在 SSD1619/UC8179 上质量不可靠，已移除）；
 *   - 收到 BIT_WEATHER_DATA 但天气数据签名没变 → 不刷。
 *
 * 状态镜像 s_state 由 render_clock() 在每次成功渲染时同步，使外部
 * clock_display_show()（HTTP / 按键路径）也参与状态去重，不会再让任务多
 * 走一次"伪 cfg_changed → 重复全刷"。
 */
static void clock_task(void *arg)
{
    ESP_LOGI(TAG, "Clock task running");

    for (;;) {
        if (!display_policy_clock_may_auto_refresh() &&
            !display_policy_calendar_may_midnight_refresh()) {
            state_reset();
            xEventGroupWaitBits(s_event,
                                BIT_CFG_CHANGED | BIT_WEATHER_DATA | BIT_WAKE,
                                pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }

        /* 等待时长对齐到下一分钟边界（最长 60s），让分钟跳变能尽快响应。 */
        TickType_t timeout = pdMS_TO_TICKS(30000);
        struct tm tm_pre;
        if (clock_get_local_or_system(&tm_pre)) {
            int wait_sec = 60 - tm_pre.tm_sec;
            if (wait_sec < 1) wait_sec = 1;
            if (wait_sec > 60) wait_sec = 60;
            timeout = pdMS_TO_TICKS(wait_sec * 1000);
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_event,
            BIT_CFG_CHANGED | BIT_WEATHER_DATA | BIT_WAKE,
            pdTRUE, pdFALSE, timeout);

        /* 唤醒后再核一次活跃条件（wait 期间可能被切到其他模式） */
        bool clock_may_refresh = display_policy_clock_may_auto_refresh();
        bool calendar_may_refresh = display_policy_calendar_may_midnight_refresh();
        if (!clock_may_refresh && !calendar_may_refresh) {
            continue;
        }

        struct tm tm;
        if (!clock_get_local_or_system(&tm)) {
            ESP_LOGW(TAG, "SNTP not synced, auto-refresh uses system time");
        }

        bool show_weather;
        portENTER_CRITICAL(&s_cfg_mux);
        show_weather = s_cfg.show_weather;
        portEXIT_CRITICAL(&s_cfg_mux);

        /* 取一份当前状态镜像，避免与外部 show 比较时撕裂。 */
        int  s_last_min, s_last_hr, s_last_md, s_last_sig;
        bool s_last_sw, s_force_full;
        portENTER_CRITICAL(&s_state_mux);
        s_last_min   = s_state.last_minute;
        s_last_hr    = s_state.last_hour;
        s_last_md    = s_state.last_mday;
        s_last_sig   = s_state.last_weather_sig;
        s_last_sw    = s_state.last_show_weather;
        s_force_full = s_state.force_full_next;
        portEXIT_CRITICAL(&s_state_mux);

        bool minute_changed = (tm.tm_min != s_last_min);
        bool hour_changed   = (s_last_hr >= 0 && tm.tm_hour != s_last_hr);
        bool day_changed    = (s_last_md >= 0 && tm.tm_mday != s_last_md);
        bool cfg_changed    = (bits & BIT_CFG_CHANGED) != 0;
        bool weather_evt    = (bits & BIT_WEATHER_DATA) != 0;
        bool sw_visibility_changed = (s_last_sw != show_weather);

        /* 仅当前正在显示天气时才比对签名；不显示时签名变化无关紧要 */
        bool weather_data_changed = false;
        if (weather_evt && show_weather) {
            int sig = compute_weather_sig();
            weather_data_changed = (sig != s_last_sig);
        }

        bool full_step_due = minute_changed &&
                             (CLOCK_FULL_REFRESH_STEP_MIN <= 1 ||
                              (tm.tm_min % CLOCK_FULL_REFRESH_STEP_MIN) == 0);

        if (calendar_may_refresh && minute_changed &&
            calendar_display_needs_midnight_refresh()) {
            ESP_LOGI(TAG, "calendar date changed, refreshing calendar page");
            unsigned cal_epoch = display_policy_display_epoch();
            if (display_mode_show_request(DISPLAY_MODE_CALENDAR, &cal_epoch) == ESP_OK)
                calendar_display_wait_render_idle();
            portENTER_CRITICAL(&s_state_mux);
            s_state.last_minute = tm.tm_min;
            s_state.last_hour = tm.tm_hour;
            s_state.last_mday = tm.tm_mday;
            portEXIT_CRITICAL(&s_state_mux);
            continue;
        }

        if (!clock_may_refresh) {
            if (minute_changed) {
                portENTER_CRITICAL(&s_state_mux);
                s_state.last_minute = tm.tm_min;
                s_state.last_hour = tm.tm_hour;
                s_state.last_mday = tm.tm_mday;
                portEXIT_CRITICAL(&s_state_mux);
            }
            continue;
        }

        bool need_render = false;

        if (cfg_changed)                          need_render = true;
        if (sw_visibility_changed)                need_render = true;
        if (hour_changed || day_changed)          need_render = true;
        if (show_weather && weather_data_changed) need_render = true;
        if (full_step_due)                        need_render = true;
        if (s_force_full)                         need_render = true;

        if (!need_render) {
            /* 比如：BIT_WEATHER_DATA 触发但数据没变；或者非 step 边界的分钟跳变。
             * 仍然更新时间镜像，避免下一轮反复把同一分钟当作 minute_changed。 */
            if (minute_changed) {
                portENTER_CRITICAL(&s_state_mux);
                s_state.last_minute = tm.tm_min;
                s_state.last_hour = tm.tm_hour;
                s_state.last_mday = tm.tm_mday;
                portEXIT_CRITICAL(&s_state_mux);
            }
            continue;
        }

        unsigned epoch = display_policy_display_epoch();
        ESP_LOGI(TAG,
                 "tick %02d:%02d -> render clock refresh=full min=%d full_step=%d "
                 "hr=%d day=%d cfg=%d sw_vis=%d wx=%d ff=%d",
                 tm.tm_hour, tm.tm_min,
                 minute_changed, full_step_due,
                 hour_changed, day_changed, cfg_changed,
                 sw_visibility_changed, weather_data_changed, s_force_full);
        render_clock(epoch, false);
    }
}

/* ── public API ───────────────────────────────────────────────────── */

esp_err_t clock_display_init(void)
{
    s_event = xEventGroupCreate();
    if (!s_event) return ESP_ERR_NO_MEM;

    s_render_mutex = xSemaphoreCreateMutex();
    if (!s_render_mutex) {
        vEventGroupDelete(s_event);
        s_event = NULL;
        return ESP_ERR_NO_MEM;
    }

    nvs_load();

    BaseType_t ok = xTaskCreate(clock_task, "clock_disp", 8192, NULL, 3, NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_render_mutex);
        s_render_mutex = NULL;
        vEventGroupDelete(s_event);
        s_event = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Clock init (enabled=%d, style=%d, weather=%d)",
             s_cfg.enabled, s_cfg.style, s_cfg.show_weather);
    return ESP_OK;
}

esp_err_t clock_display_get_config(clock_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_cfg_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
    return ESP_OK;
}

esp_err_t clock_display_set_config(const clock_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* 与旧值比较：如果只是写了同样的内容（前端表单重复提交场景），无需打扰 task。 */
    bool actually_changed;
    portENTER_CRITICAL(&s_cfg_mux);
    actually_changed = (s_cfg.enabled != cfg->enabled) ||
                       (s_cfg.style != cfg->style) ||
                       (s_cfg.show_weather != cfg->show_weather);
    s_cfg = *cfg;
    portEXIT_CRITICAL(&s_cfg_mux);

    /* 使用入参打日志，避免再次读 s_cfg 造成不必要的并发暴露 */
    nvs_save_snapshot(cfg);
    ESP_LOGI(TAG, "Config saved: enabled=%d, style=%d, weather=%d (changed=%d)",
             cfg->enabled, cfg->style, cfg->show_weather, actually_changed);

    if (s_event && actually_changed && display_policy_clock_may_auto_refresh())
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
    if (cfg->show_weather && display_policy_clock_may_auto_refresh())
        weather_request_embedded_refresh();
    return ESP_OK;
}

esp_err_t clock_display_show(void)
{
    if (!time_sync_is_synced()) {
        ESP_LOGW(TAG, "Time not synced, using system time for clock");
    }

    bool show_weather;
    portENTER_CRITICAL(&s_cfg_mux);
    show_weather = s_cfg.show_weather;
    portEXIT_CRITICAL(&s_cfg_mux);

    display_policy_set_manual_screen_active(false);

    /* 手动 show（按键/网页/启动）→ 强制下一次全刷。
     * render_clock 内部成功后会把 force_full_next 清回 false，
     * 这里只是保证从空闲/异常态恢复时第一次绝对是全刷。 */
    portENTER_CRITICAL(&s_state_mux);
    s_state.force_full_next = true;
    portEXIT_CRITICAL(&s_state_mux);

    /* 同步渲染（耗时 ~18.8s 全刷）。render_clock 末尾会更新 s_state，
     * 任务循环看到 s_state.last_minute 已是当前分钟 → 不会立即重复刷。 */
    unsigned epoch = display_policy_display_epoch();
    esp_err_t err = render_clock(epoch, true);
    if (err != ESP_OK)
        return err;

    display_mode_set_active(DISPLAY_MODE_CLOCK);

    if (show_weather)
        weather_request_embedded_refresh();

    /* 用 BIT_WAKE 而不是 BIT_CFG_CHANGED：唤醒任务从 disabled 等待里出来
     * 接管后续分钟刷新，但不会被任务当作"配置变了"再多刷一次。
     * 任务的渲染决策完全基于 s_state 与当前时间的差异，而 s_state 已被
     * 当前 render_clock 更新到最新值。 */
    if (s_event)
        xEventGroupSetBits(s_event, BIT_WAKE);
    return ESP_OK;
}

void clock_display_notify_weather_data(void)
{
    if (!s_event)
        return;
    if (!display_policy_clock_may_auto_refresh())
        return;

    /* 入口去重：如果天气数据签名与上次成功渲染时一致，根本不需要唤醒任务。
     * 这是观察到的"同分钟连刷两次 22:19"问题的源头之一。 */
    int sig = compute_weather_sig();
    int last_sig;
    portENTER_CRITICAL(&s_state_mux);
    last_sig = s_state.last_weather_sig;
    portEXIT_CRITICAL(&s_state_mux);
    if (sig != -1 && sig == last_sig) {
        return;
    }
    xEventGroupSetBits(s_event, BIT_WEATHER_DATA);
}

void clock_display_notify_home_changed(void)
{
    if (s_event)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
}

void clock_display_notify_time_resync(void)
{
    /* 时间重同步：不一定意味着内容要变，让任务正常按分钟流程刷即可。
     * 但 last_minute 可能因时区切换而对不上当前分钟，强制全刷一次更稳妥。 */
    portENTER_CRITICAL(&s_state_mux);
    s_state.force_full_next = true;
    portEXIT_CRITICAL(&s_state_mux);
    if (s_event)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
}
