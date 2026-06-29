#include "countdown.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "epd.h"
#include "fb_render.h"
#include "ui_theme.h"
#include "time_sync.h"
#include "display_policy.h"
#include "scheduler.h"

static const char *TAG = "countdown";

#define NVS_NS   "countdown"
#define NVS_BLOB "cfg"

static countdown_config_t s_cfg;
static SemaphoreHandle_t  s_cfg_mutex;
static uint8_t s_page_idx;

/* NVS persistence */

static void nvs_load(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_cfg);
    if (nvs_get_blob(h, NVS_BLOB, &s_cfg, &len) != ESP_OK || len != sizeof(s_cfg))
        memset(&s_cfg, 0, sizeof(s_cfg));
    if (s_cfg.count > COUNTDOWN_MAX_ITEMS)
        s_cfg.count = 0;
    nvs_close(h);
}

static void nvs_save_snapshot(const countdown_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_BLOB, cfg, sizeof(*cfg));
    nvs_commit(h);
    nvs_close(h);
}

/* Helpers */

static int days_remaining(const countdown_item_t *item)
{
    struct tm now_tm;
    if (!time_sync_get_local_relaxed(&now_tm)) return -1;

    struct tm target = {0};
    target.tm_year = item->year - 1900;
    target.tm_mon  = item->month - 1;
    target.tm_mday = item->day;
    target.tm_hour = 0;

    time_t t_now    = mktime(&now_tm);
    time_t t_target = mktime(&target);

    now_tm.tm_hour = 0;
    now_tm.tm_min  = 0;
    now_tm.tm_sec  = 0;
    t_now = mktime(&now_tm);

    double diff = difftime(t_target, t_now);
    return (int)(diff / 86400.0);
}

/* EPD render */

/** YYYY-MM-DD into buf[11]; clamps fields so no truncation warnings. */
static void format_date_ymd(char buf[11], int y, int mo, int da)
{
    if (y < 0) y = 0;
    else if (y > 9999) y = 9999;
    if (mo < 1) mo = 1;
    else if (mo > 12) mo = 12;
    if (da < 1) da = 1;
    else if (da > 31) da = 31;
    buf[0] = (char)('0' + (y / 1000) % 10);
    buf[1] = (char)('0' + (y / 100) % 10);
    buf[2] = (char)('0' + (y / 10) % 10);
    buf[3] = (char)('0' + y % 10);
    buf[4] = '-';
    buf[5] = (char)('0' + mo / 10);
    buf[6] = (char)('0' + mo % 10);
    buf[7] = '-';
    buf[8] = (char)('0' + da / 10);
    buf[9] = (char)('0' + da % 10);
    buf[10] = '\0';
}

static void draw_header(fb_t *fb, int W)
{
    char today[16] = "";
    struct tm now;
    if (time_sync_get_local_relaxed(&now)) {
        format_date_ymd(today, now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    }
    (void)W;
    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
    ui_draw_header(fb, "\xe5\x80\x92\xe6\x95\xb0\xe6\x97\xa5", today, true);
}

static const char *status_text_for_days(int days)
{
    if (days > 0)
        return "\xe8\xbf\x98\xe6\x9c\x89";
    if (days == 0)
        return "\xe4\xbb\x8a\xe5\xa4\xa9";
    return "\xe5\xb7\xb2\xe8\xbf\x87";
}

static const char *suffix_text_for_days(int days)
{
    if (days > 0)
        return "\xe5\xa4\xa9\xe5\x90\x8e";
    if (days == 0)
        return "\xe5\xb0\xb1\xe6\x98\xaf\xe4\xbb\x8a\xe5\xa4\xa9";
    return "\xe5\xa4\xa9\xe5\x89\x8d";
}

static int countdown_sort_score(int days)
{
    if (days >= 0)
        return days;
    return 10000 - days;
}

static void sort_active_indices(int *idx, int n, const countdown_config_t *cfg)
{
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            int di = days_remaining(&cfg->items[idx[i]]);
            int dj = days_remaining(&cfg->items[idx[j]]);
            if (countdown_sort_score(dj) < countdown_sort_score(di)) {
                int tmp = idx[i];
                idx[i] = idx[j];
                idx[j] = tmp;
            }
        }
    }
}

static void draw_day_metric(fb_t *fb, int right_x, int center_y,
                            int days, int card_h)
{
    int abs_days = days < 0 ? -days : days;
    fb_color_t c = (days >= 0) ? COLOR_RED : COLOR_BLACK;

    if (days == 0) {
        const char *today = "\xe4\xbb\x8a\xe5\xa4\xa9";
        int scale = (card_h >= 74) ? 2 : 1;
        int w = ui_text_width(today, scale);
        fb_utf8_scaled(fb, right_x - w, center_y - 8 * scale,
                       today, COLOR_RED, scale);
        return;
    }

    char days_str[16];
    snprintf(days_str, sizeof(days_str), "%d", abs_days);
    const char *unit = suffix_text_for_days(days);
    int d_scale = (card_h >= 70 && abs_days < 1000) ? 3 : 2;
    int num_w = (int)strlen(days_str) * 8 * d_scale;
    int unit_w = ui_text_width(unit, 1);
    int gap = 6;
    int total_w = num_w + gap + unit_w;
    int x = right_x - total_w;
    int num_y = center_y - 8 * d_scale;
    int unit_y = center_y - 8;

    fb_utf8_scaled(fb, x, num_y, days_str, c, d_scale);
    fb_utf8_scaled(fb, x + num_w + gap, unit_y, unit, c, 1);
}

static void draw_hero(fb_t *fb, int W, int H, const countdown_item_t *item, int days)
{
    const int s = ui_scale_for(fb);
    const int margin = 14 * s;
    const int card_x = margin;
    const int card_y = 38 * s;
    const int card_w = W - 2 * margin;
    int card_h = H - card_y - 34 * s;
    if (card_h < 160)
        card_h = H - card_y - 24 * s;

    char days_str[16];
    int abs_days = days < 0 ? -days : days;
    snprintf(days_str, sizeof(days_str), "%d", abs_days);

    bool hot = (days >= 0 && days <= 7);
    fb_color_t accent = hot ? COLOR_RED : COLOR_BLACK;
    ui_draw_card(fb, card_x, card_y, card_w, card_h, hot);
    ui_draw_section_label(fb, card_x + 12 * s, card_y + 10 * s,
                          "\xe7\x84\xa6\xe7\x82\xb9\xe4\xba\x8b\xe4\xbb\xb6",
                          accent, 1);
    fb_utf8_scaled(fb, card_x + card_w - 12 * s - ui_text_width(status_text_for_days(days), 1),
                   card_y + 10 * s, status_text_for_days(days), accent, 1);

    int title_scale = (W >= 600) ? 2 : 1;
    int title_max = card_w - 28 * s;
    if (ui_text_width(item->title, title_scale) > title_max)
        title_scale = 1;
    int title_y = card_y + 36 * s;
    int title_w = ui_text_width(item->title, title_scale);
    if (title_w <= title_max) {
        fb_utf8_scaled(fb, card_x + (card_w - title_w) / 2, title_y,
                       item->title, COLOR_BLACK, title_scale);
    } else {
        fb_utf8_scaled_maxw(fb, card_x + 14 * s, title_y, item->title,
                            COLOR_BLACK, title_scale, title_max);
    }

    char date_buf[11];
    format_date_ymd(date_buf, item->year, item->month, item->day);
    int date_y = card_y + card_h - 26 * s;
    ui_draw_dotted_hline(fb, card_x + 12 * s, date_y - 8 * s,
                         card_w - 24 * s, accent, 6);
    fb_utf8_scaled(fb, card_x + 14 * s, date_y,
                   "\xe7\x9b\xae\xe6\xa0\x87\xe6\x97\xa5", COLOR_BLACK, 1);
    fb_utf8_scaled(fb, card_x + card_w - 14 * s - 10 * 8, date_y,
                   date_buf, COLOR_BLACK, 1);

    int d_scale = 6;
    if (W >= 700 && abs_days < 1000) d_scale = 7;
    if (abs_days >= 1000) d_scale = 4;
    else if (abs_days >= 100) d_scale = 5;
    if (H < 400) d_scale--;
    if (H < 320) d_scale--;
    if (d_scale < 2) d_scale = 2;

    const char *suf = suffix_text_for_days(days);
    fb_color_t suf_c = (days == 0) ? COLOR_RED : COLOR_BLACK;
    const int suf_scale = 1;
    const char *primary = (days == 0) ? "\xe4\xbb\x8a\xe5\xa4\xa9" : days_str;
    int primary_w = (days == 0) ? ui_text_width(primary, d_scale)
                                : (int)strlen(primary) * 8 * d_scale;

    int num_top = title_y + 16 * title_scale + 12 * s;
    int num_bottom = date_y - 12 * s;
    for (;;) {
        int block_h = 16 * d_scale + 6 * s + 16 * suf_scale;
        primary_w = (days == 0) ? ui_text_width(primary, d_scale)
                                : (int)strlen(primary) * 8 * d_scale;
        if ((primary_w <= card_w - 32 * s && block_h <= num_bottom - num_top) ||
            d_scale <= 2)
            break;
        d_scale--;
    }

    int num_h = 16 * d_scale;
    int block_h = num_h + 6 * s + 16 * suf_scale;
    int block_y = num_top + ((num_bottom - num_top) - block_h) / 2;
    if (block_y < num_top)
        block_y = num_top;

    primary_w = (days == 0) ? ui_text_width(primary, d_scale)
                            : (int)strlen(primary) * 8 * d_scale;
    fb_utf8_scaled(fb, card_x + (card_w - primary_w) / 2, block_y,
                   primary, COLOR_RED, d_scale);

    int suf_y = block_y + num_h + 6 * s;
    int sw = ui_text_width(suf, suf_scale);
    fb_utf8_scaled(fb, card_x + (card_w - sw) / 2, suf_y,
                   suf, suf_c, suf_scale);
}

static void draw_cards(fb_t *fb, int W, int H,
                       const countdown_config_t *cfg, int active_count)
{
    (void)active_count;
    const int s = ui_scale_for(fb);
    const int body_y = 38 * s;
    const int margin = 14 * s;
    const int gap = 7 * s;

    int active_idx[COUNTDOWN_MAX_ITEMS];
    int n = 0;
    for (int i = 0; i < cfg->count && i < COUNTDOWN_MAX_ITEMS; i++) {
        if (cfg->items[i].active) active_idx[n++] = i;
    }
    if (n <= 0) return;
    sort_active_indices(active_idx, n, cfg);

    int pages = (n + 3) / 4;
    if (pages < 1) pages = 1;
    int page = (int)(s_page_idx % pages);
    int start = page * 4;
    int show_n = n - start;
    if (show_n > 4) show_n = 4;
    if (show_n < 1) show_n = 1;

    ui_draw_section_label(fb, margin, body_y,
                          "\xe8\xbf\x91\xe6\x9c\x9f\xe4\xba\x8b\xe4\xbb\xb6",
                          COLOR_BLACK, 1);

    int list_y = body_y + 22 * s;
    int body_h = H - list_y - 34 * s;
    int card_h = (body_h - gap * (show_n - 1)) / show_n;
    if (card_h > 88) card_h = 88;
    if (card_h < 42) card_h = 42;

    int right_col = (W >= 600) ? 172 : 116;
    int left_maxw = W - margin * 2 - right_col - 12 * s;
    if (left_maxw < 80) left_maxw = 80;

    int y = list_y;
    for (int k = 0; k < show_n; k++) {
        int i = active_idx[start + k];
        int days = days_remaining(&cfg->items[i]);
        bool hot = (days >= 0 && days <= 7);
        fb_color_t accent = hot ? COLOR_RED : COLOR_BLACK;

        ui_draw_card(fb, margin, y, W - margin * 2, card_h, hot);
        fb_fill_rect(fb, margin + 1, y + 8, 3 * s, card_h - 16, accent);

        int title_scale = (W >= 600 && card_h >= 74) ? 2 : 1;
        if (ui_text_width(cfg->items[i].title, title_scale) > left_maxw)
            title_scale = 1;
        int title_y = y + 8 * s;
        int text_x = margin + 10 * s;
        fb_utf8_scaled_maxw(fb, text_x, title_y, cfg->items[i].title,
                            COLOR_BLACK, title_scale, left_maxw);

        char date_buf[11];
        format_date_ymd(date_buf, cfg->items[i].year,
                        cfg->items[i].month, cfg->items[i].day);
        int meta_y = title_y + 16 * title_scale + 5 * s;
        if (meta_y + 16 > y + card_h - 6)
            meta_y = y + card_h - 20;

        const char *status = status_text_for_days(days);
        fb_utf8_scaled(fb, text_x, meta_y, status, accent, 1);
        int status_w = ui_text_width(status, 1);
        fb_utf8_scaled(fb, text_x + status_w + 8 * s, meta_y,
                       date_buf, COLOR_BLACK, 1);

        int sep_x = W - margin - right_col + 4 * s;
        ui_draw_dotted_vline(fb, sep_x, y + 10, card_h - 20,
                             COLOR_BLACK, 6);
        draw_day_metric(fb, W - margin - 12 * s,
                        y + card_h / 2, days, card_h);

        y += card_h + gap;
    }

    char page_str[16];
    if (pages > 1)
        snprintf(page_str, sizeof(page_str), "%d/%d", page + 1, pages);
    else
        snprintf(page_str, sizeof(page_str), "%d EVENTS", n);
    ui_draw_footer(fb, "COUNTDOWN", page_str);
}

esp_err_t countdown_show(void)
{
    countdown_config_t cfg;
    esp_err_t cfg_err = countdown_get_config(&cfg);
    if (cfg_err != ESP_OK)
        return cfg_err;

    if (!cfg.enabled) {
        ESP_LOGW(TAG, "Countdown disabled");
        return ESP_ERR_INVALID_STATE;
    }

    unsigned epoch = display_policy_display_epoch();

    fb_t *fb = fb_create();
    if (!fb) return ESP_ERR_NO_MEM;
    fb_clear(fb);

    int W = epd_width(), H = epd_height();

    int active_count = 0;
    int first_active = -1;
    for (int i = 0; i < cfg.count && i < COUNTDOWN_MAX_ITEMS; i++) {
        if (cfg.items[i].active) {
            if (first_active < 0) first_active = i;
            active_count++;
        }
    }

    draw_header(fb, W);

    if (active_count == 0) {
        ui_draw_empty_state(fb,
                            "\xe6\x9a\x82\xe6\x97\xa0\xe5\x80\x92\xe8\xae\xa1\xe6\x97\xb6",
                            "\xe8\xaf\xb7\xe9\x80\x9a\xe8\xbf\x87\xe7\xbd\x91\xe9\xa1\xb5\xe6\xb7\xbb\xe5\x8a\xa0");
        ui_draw_footer(fb, "COUNTDOWN", "EMPTY");
    } else if (active_count == 1) {
        int days = days_remaining(&cfg.items[first_active]);
        draw_hero(fb, W, H, &cfg.items[first_active], days);
        ui_draw_footer(fb, "COUNTDOWN", days == 0 ? "TODAY" : "FOCUS");
    } else {
        draw_cards(fb, W, H, &cfg, active_count);
        if (active_count > 4) {
            int pages = (active_count + 3) / 4;
            s_page_idx = (uint8_t)((s_page_idx + 1) % pages);
        } else {
            s_page_idx = 0;
        }
    }

    fb_raw_file_lock();
    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        fb_raw_file_unlock();
        ESP_LOGI(TAG, "Countdown display skipped: stale request");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t exp_err = fb_export(fb, "/spiffs/image.bin");
    fb_destroy(fb);
    if (exp_err != ESP_OK) {
        fb_raw_file_unlock();
        ESP_LOGE(TAG, "fb_export failed: %s", esp_err_to_name(exp_err));
        return exp_err;
    }
    if (!display_policy_epoch_is_current(epoch)) {
        fb_raw_file_unlock();
        ESP_LOGI(TAG, "Countdown display skipped before EPD refresh");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t disp_err = epd_display_from_file("/spiffs/image.bin");
    fb_raw_file_unlock();
    if (disp_err != ESP_OK) {
        ESP_LOGE(TAG, "display failed: %s", esp_err_to_name(disp_err));
        return disp_err;
    }

    display_policy_set_manual_screen_active(true);
    scheduler_notify_manual_show();

    ESP_LOGI(TAG, "Countdown displayed (%d active items)", active_count);
    return ESP_OK;
}

/* Public API */

esp_err_t countdown_init(void)
{
    s_cfg_mutex = xSemaphoreCreateMutex();
    if (!s_cfg_mutex)
        return ESP_ERR_NO_MEM;
    nvs_load();
    ESP_LOGI(TAG, "init ok (enabled=%d, items=%d)", s_cfg.enabled, s_cfg.count);
    return ESP_OK;
}

esp_err_t countdown_get_config(countdown_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_cfg_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t countdown_set_config(const countdown_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!s_cfg_mutex) return ESP_ERR_INVALID_STATE;
    countdown_config_t snap;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    if (s_cfg.count > COUNTDOWN_MAX_ITEMS)
        s_cfg.count = COUNTDOWN_MAX_ITEMS;
    snap = s_cfg;
    xSemaphoreGive(s_cfg_mutex);

    nvs_save_snapshot(&snap);
    ESP_LOGI(TAG, "Config updated (enabled=%d, items=%d)", snap.enabled, snap.count);
    return ESP_OK;
}
