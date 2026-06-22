/**
 * @file timetable.c
 *
 * Period-based weekly timetable with semester week tracking.
 * Grid model: grid[day_mon0][period] -> {name, room, weeks_bitmask}.
 * EPD rendering: today's course card list with RED highlight for current period.
 */

#include "timetable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "display_policy.h"
#include "epd.h"
#include "http_internal.h"
#include "esp_log.h"
#include "fb_render.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "time_sync.h"
#include "button.h"
#include "display_mode.h"
#include "power_mgr.h"
#include "ui_theme.h"

#include "freertos/semphr.h"

static const char *TAG    = "timetable";
static const char *NVS_NS = "timetable";
static const char *NVS_KEY = "tt_cfg";

static timetable_config_t s_cfg;
static bool s_inited;
static SemaphoreHandle_t s_cfg_mutex;


/* ── NVS ─────────────────────────────────────────────────────────── */

static esp_err_t tt_nvs_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;

    size_t len = sizeof(s_cfg);
    err = nvs_get_blob(h, NVS_KEY, &s_cfg, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(s_cfg)) {
        memset(&s_cfg, 0, sizeof(s_cfg));
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t tt_nvs_save_snapshot(const timetable_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    err = nvs_set_blob(h, NVS_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ── Time helpers ────────────────────────────────────────────────── */

static int tt_minutes(uint8_t h, uint8_t m)
{
    return (int)h * 60 + (int)m;
}

/** 0=Mon ... 6=Sun, -1 if time not synced */
static int tt_weekday_mon0(const struct tm *tm)
{
    int w = tm->tm_wday;
    return (w == 0) ? 6 : (w - 1);
}

static int tt_now_minutes(const struct tm *tm)
{
    return tm->tm_hour * 60 + tm->tm_min;
}

/* ── Week calculation ────────────────────────────────────────────── */

/** Same as timetable_current_week(), but uses the passed snapshot without locking. */
static int tt_current_week_in(const timetable_config_t *cfg)
{
    if (cfg->semester_start == 0)
        return 0;

    struct tm tm;
    if (!time_sync_get_local_relaxed(&tm))
        return 0;

    time_t now = mktime(&tm);
    if (now == (time_t)-1)
        return 0;

    int diff_sec = (int)(now - (time_t)cfg->semester_start);
    if (diff_sec < 0)
        return 0;

    int week = diff_sec / (7 * 24 * 3600) + 1;
    return (week >= 1 && week <= TT_MAX_WEEKS) ? week : 0;
}

int timetable_current_week(void)
{
    /* Only semester_start is needed here; avoid copying the large table onto small stacks. */
    int32_t semester_start;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    semester_start = s_cfg.semester_start;
    xSemaphoreGive(s_cfg_mutex);

    if (semester_start == 0)
        return 0;

    struct tm tm;
    if (!time_sync_get_local_relaxed(&tm))
        return 0;

    time_t now = mktime(&tm);
    if (now == (time_t)-1)
        return 0;

    int diff_sec = (int)(now - (time_t)semester_start);
    if (diff_sec < 0)
        return 0;

    int week = diff_sec / (7 * 24 * 3600) + 1;
    return (week >= 1 && week <= TT_MAX_WEEKS) ? week : 0;
}

static bool tt_cell_active(const tt_cell_t *c, int week)
{
    if (c->name[0] == '\0')
        return false;
    if (week <= 0)
        return true;
    return (c->weeks & (1u << (week - 1))) != 0;
}

/* -- Grid query (current / next course), all based on the passed snapshot -- */

static const tt_cell_t *tt_find_current_in(const timetable_config_t *cfg,
                                           int wd, int now_min, int week)
{
    if (wd < 0 || wd >= TT_DAYS)
        return NULL;

    for (int p = 0; p < cfg->period_count; p++) {
        const tt_period_def_t *pd = &cfg->periods[p];
        int a = tt_minutes(pd->start_hour, pd->start_minute);
        int b = tt_minutes(pd->end_hour, pd->end_minute);
        if (now_min >= a && now_min < b) {
            const tt_cell_t *c = &cfg->grid[wd][p];
            if (tt_cell_active(c, week))
                return c;
        }
    }
    return NULL;
}

/** Find current period index in snapshot, -1 if none */
static int tt_current_period_idx_in(const timetable_config_t *cfg,
                                    int wd, int now_min)
{
    (void)wd;
    for (int p = 0; p < cfg->period_count; p++) {
        const tt_period_def_t *pd = &cfg->periods[p];
        int a = tt_minutes(pd->start_hour, pd->start_minute);
        int b = tt_minutes(pd->end_hour, pd->end_minute);
        if (now_min >= a && now_min < b)
            return p;
    }
    return -1;
}

static const tt_cell_t *tt_find_next_in(const timetable_config_t *cfg,
                                        int wd, int now_min, int week,
                                        int *out_wait)
{
    if (wd < 0 || wd >= TT_DAYS)
        return NULL;

    for (int p = 0; p < cfg->period_count; p++) {
        const tt_period_def_t *pd = &cfg->periods[p];
        int a = tt_minutes(pd->start_hour, pd->start_minute);
        if (a <= now_min)
            continue;
        const tt_cell_t *c = &cfg->grid[wd][p];
        if (tt_cell_active(c, week)) {
            if (out_wait)
                *out_wait = a - now_min;
            return c;
        }
    }
    return NULL;
}

/* ── Public: config / query ──────────────────────────────────────── */

esp_err_t timetable_init(void)
{
    if (s_inited)
        return ESP_OK;
    s_cfg_mutex = xSemaphoreCreateMutex();
    if (!s_cfg_mutex)
        return ESP_ERR_NO_MEM;
    tt_nvs_load();
    s_inited = true;
    ESP_LOGI(TAG, "init ok (enabled=%d, periods=%d)", s_cfg.enabled, s_cfg.period_count);
    return ESP_OK;
}

esp_err_t timetable_get_config(timetable_config_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t timetable_set_config(const timetable_config_t *cfg)
{
    if (!cfg)
        return ESP_ERR_INVALID_ARG;
    timetable_config_t snap;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    s_cfg = *cfg;
    if (s_cfg.period_count > TT_MAX_PERIODS)
        s_cfg.period_count = TT_MAX_PERIODS;
    if (s_cfg.show_days != 7)
        s_cfg.show_days = 5;
    snap = s_cfg;
    xSemaphoreGive(s_cfg_mutex);
    /* Save NVS after releasing the lock to avoid blocking readers. */
    return tt_nvs_save_snapshot(&snap);
}

bool timetable_get_current_course(char *name_buf, size_t name_len,
                                  char *room_buf, size_t room_len,
                                  int *minutes_remaining)
{
    /* Allocate timetable_config_t on heap to avoid stack pressure. */
    timetable_config_t *snap = malloc(sizeof(*snap));
    if (!snap) return false;
    bool ok = false;

    do {
        if (timetable_get_config(snap) != ESP_OK) break;
        if (!snap->enabled) break;

        struct tm tm;
        if (!time_sync_get_local_relaxed(&tm)) break;

        int wd = tt_weekday_mon0(&tm);
        int now = tt_now_minutes(&tm);
        int week = tt_current_week_in(snap);

        const tt_cell_t *c = tt_find_current_in(snap, wd, now, week);
        if (!c) break;

        int cur_p = tt_current_period_idx_in(snap, wd, now);
        if (cur_p < 0) break;

        if (name_buf && name_len > 0) {
            strncpy(name_buf, c->name, name_len - 1);
            name_buf[name_len - 1] = '\0';
        }
        if (room_buf && room_len > 0) {
            strncpy(room_buf, c->room, room_len - 1);
            room_buf[room_len - 1] = '\0';
        }
        if (minutes_remaining) {
            int end = tt_minutes(snap->periods[cur_p].end_hour,
                                 snap->periods[cur_p].end_minute);
            *minutes_remaining = end - now;
        }
        ok = true;
    } while (0);

    free(snap);
    return ok;
}

bool timetable_get_next_course(char *name_buf, size_t name_len,
                               char *room_buf, size_t room_len,
                               int *minutes_until_start)
{
    timetable_config_t *snap = malloc(sizeof(*snap));
    if (!snap) return false;
    bool ok = false;

    do {
        if (timetable_get_config(snap) != ESP_OK) break;
        if (!snap->enabled) break;

        struct tm tm;
        if (!time_sync_get_local_relaxed(&tm)) break;

        int wd = tt_weekday_mon0(&tm);
        int now = tt_now_minutes(&tm);
        int week = tt_current_week_in(snap);

        int wait = 0;
        const tt_cell_t *c = tt_find_next_in(snap, wd, now, week, &wait);

        if (!c) {
            int nd = (wd + 1) % TT_DAYS;
            c = tt_find_next_in(snap, nd, -1, week, &wait);
            if (c) {
                int mins_left_today = 24 * 60 - now;
                wait += mins_left_today;
            }
        }

        if (!c) break;

        if (name_buf && name_len > 0) {
            strncpy(name_buf, c->name, name_len - 1);
            name_buf[name_len - 1] = '\0';
        }
        if (room_buf && room_len > 0) {
            strncpy(room_buf, c->room, room_len - 1);
            room_buf[room_len - 1] = '\0';
        }
        if (minutes_until_start)
            *minutes_until_start = wait;
        ok = true;
    } while (0);

    free(snap);
    return ok;
}

/* ── EPD: today's course card view ──────────────────────────────── */

static const char *const TT_DAY_FULL[] = {
    "\xe5\x91\xa8\xe4\xb8\x80", "\xe5\x91\xa8\xe4\xba\x8c",
    "\xe5\x91\xa8\xe4\xb8\x89", "\xe5\x91\xa8\xe5\x9b\x9b",
    "\xe5\x91\xa8\xe4\xba\x94", "\xe5\x91\xa8\xe5\x85\xad",
    "\xe5\x91\xa8\xe6\x97\xa5",
};

static int tt_collect_today_in(const timetable_config_t *cfg,
                               int wd, int week, int *out, int max)
{
    int n = 0;
    for (int p = 0; p < cfg->period_count && n < max; p++) {
        if (tt_cell_active(&cfg->grid[wd][p], week))
            out[n++] = p;
    }
    return n;
}

static int tt_text_px(const char *s, int scale)
{
    if (scale < 1)
        scale = 1;
    return fb_utf8_px_width(s, 16 * scale);
}

static int tt_draw_text(fb_t *fb, int x, int y, const char *s,
                        fb_color_t color, int scale)
{
    if (scale < 1)
        scale = 1;
    return fb_utf8_px(fb, x, y, s, color, 16 * scale);
}

static int tt_draw_text_maxw(fb_t *fb, int x, int y, const char *s,
                             fb_color_t color, int scale, int max_w)
{
    if (scale < 1)
        scale = 1;
    return fb_utf8_px_maxw(fb, x, y, s, color, 16 * scale, max_w);
}

static void tt_draw_header(fb_t *fb, const char *title, const char *right,
                           bool red_accent)
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
        title_w = tt_text_px(title, 1);
        if (title_w > title_max_w)
            title_w = title_max_w;
        tt_draw_text_maxw(fb, x, y, title, COLOR_BLACK, 1, title_max_w);
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

        int rw = tt_text_px(right, 1);
        int rx = right_limit - rw;
        if (rx < min_right_x)
            rx = min_right_x;
        if (right_limit > rx)
            tt_draw_text_maxw(fb, rx, y + 1 * s, right, COLOR_BLACK, 1,
                              right_limit - rx);
    }

    ui_draw_dotted_hline(fb, 12 * s, header_h + 6 * s, W - 24 * s,
                         COLOR_BLACK, 6);
}

static void tt_draw_footer(fb_t *fb, const char *left, const char *right)
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
        int rw = tt_text_px(right, 1);
        left_max_w = W - 34 * s - rw;
    }
    if (left && left[0] && left_max_w > 0)
        tt_draw_text_maxw(fb, 14 * s, y, left, COLOR_BLACK, 1, left_max_w);

    if (right && right[0]) {
        int rw = tt_text_px(right, 1);
        tt_draw_text(fb, W - 14 * s - rw, y, right, COLOR_BLACK, 1);
    }
}

static void tt_draw_empty_state(fb_t *fb, const char *title, const char *hint)
{
    if (!fb)
        return;

    int s = ui_scale_for(fb);
    int y = fb->height / 2 - 28 * s;
    if (title) {
        int w = tt_text_px(title, s + 1);
        tt_draw_text(fb, (fb->width - w) / 2, y, title, COLOR_BLACK, s + 1);
    }
    if (hint) {
        int w = tt_text_px(hint, s);
        tt_draw_text(fb, (fb->width - w) / 2, y + 28 * s, hint, COLOR_RED, s);
    }
}

/* ── EPD: full-week grid view (for 5.83" 648x480) ────────────────── */

static esp_err_t timetable_show_week_grid(const timetable_config_t *cfg,
                                          int today, int now_min, int week,
                                          const struct tm *tm,
                                          unsigned epoch)
{
    fb_t *fb = fb_create();
    if (!fb)
        return ESP_ERR_NO_MEM;
    fb_clear(fb);

    int W = fb->width;
    int H = fb->height;
    int cols = cfg->show_days;
    int s = ui_scale_for(fb);

    /* ── title ── */
    {
        char title[64];
        if (week > 0)
            snprintf(title, sizeof(title),
                     "\xe8\xaf\xbe\xe7\xa8\x8b\xe8\xa1\xa8 \xc2\xb7 \xe7\xac\xac%d\xe5\x91\xa8",
                     week);
        else
            snprintf(title, sizeof(title),
                     "\xe8\xaf\xbe\xe7\xa8\x8b\xe8\xa1\xa8");

        char ts[64];
        snprintf(ts, sizeof(ts), "%d/%02d/%02d %02d:%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min);
        ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
        tt_draw_header(fb, title, ts, true);
    }

    int pc = cfg->period_count;
    if (pc == 0) {
        tt_draw_empty_state(fb, "\xe6\x97\xa0\xe8\xaf\xbe\xe7\xa8\x8b", "\xe8\xaf\xb7\xe9\x80\x9a\xe8\xbf\x87\xe7\xbd\x91\xe9\xa1\xb5\xe6\xb7\xbb\xe5\x8a\xa0");
        if (!display_policy_epoch_is_current(epoch)) {
            fb_destroy(fb);
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = epd_display_fb_free(fb);
        return err;
    }

    /* ── layout dimensions ── */
    int margin = 14 * s;
    int hdr_h = 20 * s;
    int pcol_w = 56;
    int footer_h = 26 * s;
    int y_start = 34 * s;
    int grid_w = W - 2 * margin;
    if (grid_w < pcol_w + cols)
        grid_w = pcol_w + cols;
    int cell_w = (grid_w - pcol_w) / cols;
    int body_y = y_start + hdr_h;
    int grid_h = H - body_y - footer_h;
    int cell_h = grid_h / pc;
    if (cell_h < 20) cell_h = 20;
    int grid_right = margin + pcol_w + cols * cell_w;

    /* ── day header row ── */
    {
        const char *jie = "\xe8\x8a\x82\xe6\xac\xa1";
        int jw = tt_text_px(jie, 1);
        tt_draw_text(fb, margin + (pcol_w - jw) / 2,
                     y_start + (hdr_h - 16) / 2, jie, COLOR_BLACK, 1);
    }

    static const char *const DAY_SHORT[] = {
        "\xe4\xb8\x80", "\xe4\xba\x8c", "\xe4\xb8\x89",
        "\xe5\x9b\x9b", "\xe4\xba\x94", "\xe5\x85\xad", "\xe6\x97\xa5",
    };
    for (int d = 0; d < cols; d++) {
        int cx = margin + pcol_w + d * cell_w;

        char label[16];
        snprintf(label, sizeof(label), "\xe5\x91\xa8%s", DAY_SHORT[d]);
        int lw = tt_text_px(label, 1);
        fb_color_t lc = (d == today) ? COLOR_RED : COLOR_BLACK;
        tt_draw_text(fb, cx + (cell_w - lw) / 2,
                     y_start + (hdr_h - 16) / 2, label, lc, 1);
        if (d == today)
            fb_fill_rect(fb, cx + 12, y_start + hdr_h - 4, cell_w - 24, 2, COLOR_RED);
    }
    ui_draw_dotted_hline(fb, margin, body_y - 1, grid_right - margin,
                         COLOR_BLACK, 6);

    /* ── grid body ── */
    for (int p = 0; p < pc; p++) {
        int ry = body_y + p * cell_h;
        if (ry + cell_h > H - footer_h) break;

        if (p > 0)
            ui_draw_dotted_hline(fb, margin, ry, grid_right - margin,
                                 COLOR_BLACK, 6);

        /* period column: number + time range */
        const tt_period_def_t *pd = &cfg->periods[p];
        char pnum[4];
        snprintf(pnum, sizeof(pnum), "%d", p + 1);

        char t_start[8], t_end[8];
        snprintf(t_start, sizeof(t_start), "%02d:%02d",
                 pd->start_hour, pd->start_minute);
        snprintf(t_end, sizeof(t_end), "%02d:%02d",
                 pd->end_hour, pd->end_minute);

        if (cell_h >= 64) {
            int pnw = tt_text_px(pnum, 2);
            int t1w = tt_text_px(t_start, 1);
            int t2w = tt_text_px(t_end, 1);
            int vy = ry + (cell_h - 64) / 2;
            tt_draw_text(fb, margin + (pcol_w - pnw) / 2, vy, pnum, COLOR_BLACK, 2);
            tt_draw_text(fb, margin + (pcol_w - t1w) / 2, vy + 32, t_start, COLOR_BLACK, 1);
            tt_draw_text(fb, margin + (pcol_w - t2w) / 2, vy + 48, t_end, COLOR_BLACK, 1);
        } else if (cell_h >= 48) {
            int pnw1 = tt_text_px(pnum, 1);
            int t1w = tt_text_px(t_start, 1);
            int t2w = tt_text_px(t_end, 1);
            int vy = ry + (cell_h - 48) / 2;
            tt_draw_text(fb, margin + (pcol_w - pnw1) / 2, vy, pnum, COLOR_BLACK, 1);
            tt_draw_text(fb, margin + (pcol_w - t1w) / 2, vy + 16, t_start, COLOR_BLACK, 1);
            tt_draw_text(fb, margin + (pcol_w - t2w) / 2, vy + 32, t_end, COLOR_BLACK, 1);
        } else {
            int pnw1 = tt_text_px(pnum, 1);
            int t1w = tt_text_px(t_start, 1);
            int vy = ry + (cell_h - 32) / 2;
            tt_draw_text(fb, margin + (pcol_w - pnw1) / 2, vy, pnum, COLOR_BLACK, 1);
            tt_draw_text(fb, margin + (pcol_w - t1w) / 2, vy + 16, t_start, COLOR_BLACK, 1);
        }

        /* each day's cell */
        for (int d = 0; d < cols; d++) {
            int cx = margin + pcol_w + d * cell_w;
            const tt_cell_t *cell = &cfg->grid[d][p];

            if (!tt_cell_active(cell, week))
                continue;

            bool is_cur = (d == today &&
                           p == tt_current_period_idx_in(cfg, today, now_min));

            if (is_cur)
                fb_fill_rect(fb, cx + 2, ry + 5, 3, cell_h - 10, COLOR_RED);

            fb_color_t nc = is_cur ? COLOR_RED : COLOR_BLACK;
            fb_color_t rc = COLOR_BLACK;

            int pad = 8;
            int content_w = cell_w - pad * 2;

            int nw2 = tt_text_px(cell->name, 2);
            int name_sc = (cell_h >= 58 && nw2 <= content_w) ? 2 : 1;
            int name_h = 16 * name_sc;
            int room_h = (cell->room[0] && cell_h >= 34) ? 16 : 0;
            int gap = room_h ? 2 : 0;
            int total_h = name_h + gap + room_h;
            int ny = ry + (cell_h - total_h) / 2;
            if (ny < ry + 1) ny = ry + 1;

            tt_draw_text_maxw(fb, cx + pad, ny,
                              cell->name, nc, name_sc, content_w);
            if (room_h)
                tt_draw_text_maxw(fb, cx + pad, ny + name_h + gap,
                                  cell->room, rc, 1, content_w);
        }
    }

    int last_y = body_y + pc * cell_h;
    if (last_y < H - footer_h)
        ui_draw_dotted_hline(fb, margin, last_y, grid_right - margin,
                             COLOR_BLACK, 6);

    /* column separators */
    ui_draw_dotted_vline(fb, margin + pcol_w, y_start, hdr_h + pc * cell_h,
                         COLOR_BLACK, 6);
    for (int d = 1; d < cols; d++)
        ui_draw_dotted_vline(fb, margin + pcol_w + d * cell_w,
                             y_start, hdr_h + pc * cell_h,
                             COLOR_BLACK, 6);

    /* ── footer ── */
    {
        int today_courses = 0;
        if (today >= 0 && today < TT_DAYS)
            for (int p = 0; p < pc; p++)
                if (tt_cell_active(&cfg->grid[today][p], week))
                    today_courses++;

        int week_courses = 0;
        for (int d = 0; d < cols; d++)
            for (int p = 0; p < pc; p++)
                if (tt_cell_active(&cfg->grid[d][p], week))
                    week_courses++;

        char foot[96];
        snprintf(foot, sizeof(foot),
                 "%s %d\xe8\x8a\x82 / \xe6\x9c\xac\xe5\x91\xa8%d\xe8\x8a\x82",
                 TT_DAY_FULL[today], today_courses, week_courses);
        tt_draw_footer(fb, "TIMETABLE", foot);
    }

    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = epd_display_fb_free(fb);

    if (err == ESP_OK)
        ESP_LOGI(TAG, "show week grid: %d periods, %d days (week=%d)", pc, cols, week);
    return err;
}

/* ── EPD: single-day agenda view ─────────────────────────────────── */

esp_err_t timetable_show_day(int day)
{
    if (!epd_is_ready())
        return ESP_ERR_INVALID_STATE;
    if (day < 0 || day >= TT_DAYS)
        return ESP_ERR_INVALID_ARG;

    unsigned epoch = display_policy_display_epoch();

    /* Take one heap snapshot first; all rendering below uses this stable copy. */
    timetable_config_t *snap = malloc(sizeof(*snap));
    if (!snap)
        return ESP_ERR_NO_MEM;
    if (timetable_get_config(snap) != ESP_OK) {
        free(snap);
        return ESP_FAIL;
    }
    if (!snap->enabled) {
        free(snap);
        return ESP_ERR_INVALID_STATE;
    }

    struct tm tm;
    bool have_time = time_sync_get_local_relaxed(&tm);
    if (!have_time) {
        ESP_LOGW(TAG, "time not synced");
        free(snap);
        return ESP_ERR_INVALID_STATE;
    }

    int W = epd_width();
    int H = epd_height();

    /* 5.83" and wider panels: use full-week grid view. */
    if (W >= 600) {
        int today   = tt_weekday_mon0(&tm);
        int now_min = tt_now_minutes(&tm);
        int week    = tt_current_week_in(snap);
        esp_err_t err = timetable_show_week_grid(snap, today, now_min, week, &tm, epoch);
        free(snap);
        return err;
    }

    /* 4.2" (400x300): keep single-day agenda view */
    fb_t *fb = fb_create();
    if (!fb) {
        free(snap);
        return ESP_ERR_NO_MEM;
    }
    fb_clear(fb);

    W = fb->width;
    H = fb->height;
    int s = 1;

    int today   = tt_weekday_mon0(&tm);
    int now_min = tt_now_minutes(&tm);
    int week    = tt_current_week_in(snap);
    bool is_today = (day == today);
    int cur_p   = is_today ? tt_current_period_idx_in(snap, day, now_min) : -1;

    char title[64];
    if (week > 0)
        snprintf(title, sizeof(title), "%s \xc2\xb7 \xe7\xac\xac%d\xe5\x91\xa8",
                 TT_DAY_FULL[day], week);
    else
        snprintf(title, sizeof(title), "%s", TT_DAY_FULL[day]);

    char right[16] = "";
    if (is_today) {
        snprintf(right, sizeof(right), "%02d:%02d", tm.tm_hour, tm.tm_min);
    }

    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
    tt_draw_header(fb, title, right, true);

    /* ── collect courses for the requested day ── */
    int order[TT_MAX_PERIODS];
    int count = tt_collect_today_in(snap, day, week, order, TT_MAX_PERIODS);

    if (count == 0) {
        /* "X无课" */
        char msg[32];
        snprintf(msg, sizeof(msg), "%s\xe6\x97\xa0\xe8\xaf\xbe", TT_DAY_FULL[day]);
        int mw = tt_text_px(msg, s + 1);
        int mx = (W - mw) / 2;
        (void)mw;
        (void)mx;
        tt_draw_empty_state(fb, msg, "\xe4\xbb\x8a\xe5\xa4\xa9\xe6\xb2\xa1\xe6\x9c\x89\xe8\xaf\xbe\xe7\xa8\x8b");
        tt_draw_footer(fb, "TIMETABLE", "EMPTY");
        if (!display_policy_epoch_is_current(epoch)) {
            fb_destroy(fb);
            free(snap);
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = epd_display_fb_free(fb);
        free(snap);
        return err;
    }

    /* ── agenda layout ── */
    int lm       = 14 * s;
    int rm       = 10 * s;
    int bar_w    = 4 * s;
    int bar_gap  = 3 * s;
    int footer_h = 22 * s;
    int y_top    = 34 * s;
    int avail    = H - y_top - footer_h - 2;
    int block_h  = avail / count;

    bool two_line;
    int name_sc;
    if (block_h >= 56 * s) {
        block_h = 56 * s;
        name_sc = s + 1;
        two_line = true;
    } else if (block_h >= 36 * s) {
        name_sc = s;
        two_line = true;
    } else {
        name_sc = s;
        two_line = false;
    }

    for (int i = 0; i < count; i++) {
        int p = order[i];
        const tt_cell_t *cell = &snap->grid[day][p];
        const tt_period_def_t *pd = &snap->periods[p];
        bool is_now = (p == cur_p);

        int by = y_top + i * block_h;
        if (by + 16 * s > H - footer_h)
            break;

        if (two_line) {
            int text_h = 16 * s + 2 * s + 16 * name_sc;
            int ty = by + (block_h - text_h) / 2;

            if (is_now)
                fb_fill_rect(fb, lm - bar_w - bar_gap, ty,
                             bar_w, text_h, COLOR_RED);

            char info[48];
            snprintf(info, sizeof(info),
                     "\xe7\xac\xac%d\xe8\x8a\x82  %02d:%02d-%02d:%02d",
                     p + 1,
                     pd->start_hour, pd->start_minute,
                     pd->end_hour, pd->end_minute);
            tt_draw_text(fb, lm, ty, info, COLOR_BLACK, s);

            if (is_now) {
                const char *tag = "\xe8\xbf\x9b\xe8\xa1\x8c\xe4\xb8\xad";
                int tw = tt_text_px(tag, s);
                tt_draw_text(fb, W - rm - tw, ty, tag, COLOR_RED, s);
            }

            int ny = ty + 16 * s + 2 * s;
            int content_w = W - lm - rm;

            int room_w = 0;
            if (cell->room[0])
                room_w = tt_text_px(cell->room, s);

            int name_max = content_w;
            if (room_w > 0) {
                name_max = content_w - room_w - 8 * s;
                int room_y = ny + (16 * name_sc - 16 * s) / 2;
                tt_draw_text(fb, W - rm - room_w, room_y,
                             cell->room, COLOR_BLACK, s);
            }
            tt_draw_text_maxw(fb, lm, ny, cell->name,
                              COLOR_BLACK, name_sc, name_max);

            if (i < count - 1) {
                int sep_y = by + block_h - 1;
                ui_draw_dotted_hline(fb, lm, sep_y, W - lm - rm,
                                     COLOR_BLACK, 6);
            }
        } else {
            int ly = by + (block_h - 16 * s) / 2;

            if (is_now)
                fb_fill_rect(fb, lm - bar_w - bar_gap, ly,
                             bar_w, 16 * s, COLOR_RED);

            char prefix[24];
            snprintf(prefix, sizeof(prefix), "%d %02d:%02d",
                     p + 1, pd->start_hour, pd->start_minute);
            int pw = tt_draw_text(fb, lm, ly, prefix, COLOR_BLACK, s);

            int nx = lm + pw + 8 * s;
            int nmax = W - nx - rm;

            if (cell->room[0]) {
                int rw = tt_text_px(cell->room, s);
                nmax -= rw + 8 * s;
                tt_draw_text(fb, W - rm - rw, ly,
                             cell->room, COLOR_BLACK, s);
            }
            if (nmax < 16 * s) nmax = 16 * s;
            tt_draw_text_maxw(fb, nx, ly, cell->name,
                              COLOR_BLACK, s, nmax);

            if (i < count - 1) {
                int sep_y = by + block_h - 1;
                ui_draw_dotted_hline(fb, lm, sep_y, W - lm - rm,
                                     COLOR_BLACK, 6);
            }
        }
    }

    /* ── footer ── */
    {
        char foot[48];
        snprintf(foot, sizeof(foot),
                 "%s %d \xe8\x8a\x82\xe8\xaf\xbe",
                 TT_DAY_FULL[day], count);
        tt_draw_footer(fb, "TIMETABLE", foot);
    }

    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        free(snap);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = epd_display_fb_free(fb);
    free(snap);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "epd: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "show day=%d: %d courses (week=%d)", day, count, week);
    return ESP_OK;
}

esp_err_t timetable_show(void)
{
    struct tm tm;
    if (!time_sync_get_local_relaxed(&tm))
        return ESP_ERR_INVALID_STATE;
    return timetable_show_day(tt_weekday_mon0(&tm));
}

esp_err_t timetable_show_current(void)
{
    return timetable_show();
}

/* ── Weeks bitmask <-> string ────────────────────────────────────── */

static uint32_t tt_parse_weeks(const char *s)
{
    if (!s || !s[0])
        return 0x01FFFFFFu; /* all 25 weeks */

    uint32_t mask = 0;
    const char *p = s;

    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        int a = (int)strtol(p, (char **)&p, 10);
        if (a < 1 || a > TT_MAX_WEEKS) {
            while (*p && *p != ',') p++;
            continue;
        }

        if (*p == '-') {
            p++;
            int b = (int)strtol(p, (char **)&p, 10);
            if (b < a) b = a;
            if (b > TT_MAX_WEEKS) b = TT_MAX_WEEKS;
            for (int i = a; i <= b; i++)
                mask |= (1u << (i - 1));
        } else {
            mask |= (1u << (a - 1));
        }
    }
    return mask ? mask : 0x01FFFFFFu;
}

static void tt_weeks_to_str(uint32_t mask, char *buf, int buflen)
{
    if (mask == 0 || mask == 0x01FFFFFFu) {
        snprintf(buf, buflen, "1-%d", TT_MAX_WEEKS);
        return;
    }

    buf[0] = '\0';
    int pos = 0;
    int i = 1;
    while (i <= TT_MAX_WEEKS) {
        if (!(mask & (1u << (i - 1)))) { i++; continue; }
        int start = i;
        while (i <= TT_MAX_WEEKS && (mask & (1u << (i - 1)))) i++;
        int end = i - 1;

        if (pos > 0 && pos < buflen - 1)
            pos += snprintf(buf + pos, buflen - pos, ",");

        if (start == end)
            pos += snprintf(buf + pos, buflen - pos, "%d", start);
        else
            pos += snprintf(buf + pos, buflen - pos, "%d-%d", start, end);
    }
}

/* ── HTTP: GET /timetable.json ───────────────────────────────────── */

esp_err_t timetable_http_get_handler(httpd_req_t *req)
{
    timetable_config_t snap;
    if (timetable_get_config(&snap) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config unavailable");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json oom");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "enabled", snap.enabled);
    cJSON_AddNumberToObject(root, "period_count", snap.period_count);
    cJSON_AddNumberToObject(root, "show_days", snap.show_days);
    cJSON_AddNumberToObject(root, "semester_start", snap.semester_start);
    cJSON_AddNumberToObject(root, "current_week", tt_current_week_in(&snap));

    /* periods */
    cJSON *periods = cJSON_AddArrayToObject(root, "periods");
    for (int p = 0; p < snap.period_count; p++) {
        cJSON *po = cJSON_CreateObject();
        if (!po) continue;
        cJSON_AddNumberToObject(po, "start_hour", snap.periods[p].start_hour);
        cJSON_AddNumberToObject(po, "start_minute", snap.periods[p].start_minute);
        cJSON_AddNumberToObject(po, "end_hour", snap.periods[p].end_hour);
        cJSON_AddNumberToObject(po, "end_minute", snap.periods[p].end_minute);
        cJSON_AddItemToArray(periods, po);
    }

    /* grid[7][period_count] */
    cJSON *grid = cJSON_AddArrayToObject(root, "grid");
    for (int d = 0; d < TT_DAYS; d++) {
        cJSON *day_arr = cJSON_CreateArray();
        for (int p = 0; p < snap.period_count; p++) {
            const tt_cell_t *c = &snap.grid[d][p];
            if (c->name[0] == '\0') {
                cJSON_AddItemToArray(day_arr, cJSON_CreateNull());
                continue;
            }
            cJSON *co = cJSON_CreateObject();
            if (!co) { cJSON_AddItemToArray(day_arr, cJSON_CreateNull()); continue; }
            cJSON_AddStringToObject(co, "name", c->name);
            cJSON_AddStringToObject(co, "room", c->room);
            char wbuf[64];
            tt_weeks_to_str(c->weeks, wbuf, sizeof(wbuf));
            cJSON_AddStringToObject(co, "weeks", wbuf);
            cJSON_AddItemToArray(day_arr, co);
        }
        cJSON_AddItemToArray(grid, day_arr);
    }

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json oom");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    free(str);
    return ESP_OK;
}

/* ── HTTP: POST /timetable ───────────────────────────────────────── */

esp_err_t timetable_http_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    int total = req->content_len;
    if (total <= 0 || total > 16384) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad content length");
        return ESP_OK;
    }

    char *buf = malloc(total + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv fail"); return ESP_OK; }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    timetable_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cJSON *j;
    j = cJSON_GetObjectItem(root, "enabled");
    cfg.enabled = (j && cJSON_IsTrue(j));

    j = cJSON_GetObjectItem(root, "show_days");
    cfg.show_days = (j && cJSON_IsNumber(j) && j->valueint == 7) ? 7 : 5;

    j = cJSON_GetObjectItem(root, "semester_start");
    if (j && cJSON_IsNumber(j))
        cfg.semester_start = (int32_t)j->valuedouble;

    j = cJSON_GetObjectItem(root, "period_count");
    if (j && cJSON_IsNumber(j)) {
        cfg.period_count = (uint8_t)j->valueint;
        if (cfg.period_count > TT_MAX_PERIODS)
            cfg.period_count = TT_MAX_PERIODS;
    }

    cJSON *periods = cJSON_GetObjectItem(root, "periods");
    if (periods && cJSON_IsArray(periods)) {
        int np = cJSON_GetArraySize(periods);
        if (np > TT_MAX_PERIODS) np = TT_MAX_PERIODS;
        if ((int)cfg.period_count < np)
            cfg.period_count = (uint8_t)np;

        for (int p = 0; p < np; p++) {
            cJSON *po = cJSON_GetArrayItem(periods, p);
            if (!po) continue;
            tt_period_def_t *pd = &cfg.periods[p];

            j = cJSON_GetObjectItem(po, "start_hour");
            if (j && cJSON_IsNumber(j)) pd->start_hour = (uint8_t)j->valueint;
            j = cJSON_GetObjectItem(po, "start_minute");
            if (j && cJSON_IsNumber(j)) pd->start_minute = (uint8_t)j->valueint;
            j = cJSON_GetObjectItem(po, "end_hour");
            if (j && cJSON_IsNumber(j)) pd->end_hour = (uint8_t)j->valueint;
            j = cJSON_GetObjectItem(po, "end_minute");
            if (j && cJSON_IsNumber(j)) pd->end_minute = (uint8_t)j->valueint;
        }
    }

    cJSON *grid = cJSON_GetObjectItem(root, "grid");
    if (grid && cJSON_IsArray(grid)) {
        int nd = cJSON_GetArraySize(grid);
        if (nd > TT_DAYS) nd = TT_DAYS;

        for (int d = 0; d < nd; d++) {
            cJSON *day_arr = cJSON_GetArrayItem(grid, d);
            if (!day_arr || !cJSON_IsArray(day_arr)) continue;

            int ns = cJSON_GetArraySize(day_arr);
            if (ns > TT_MAX_PERIODS) ns = TT_MAX_PERIODS;

            for (int p = 0; p < ns; p++) {
                cJSON *co = cJSON_GetArrayItem(day_arr, p);
                if (!co || cJSON_IsNull(co)) continue;

                tt_cell_t *cell = &cfg.grid[d][p];

                j = cJSON_GetObjectItem(co, "name");
                if (j && cJSON_IsString(j))
                    strncpy(cell->name, j->valuestring, TT_NAME_LEN - 1);

                j = cJSON_GetObjectItem(co, "room");
                if (j && cJSON_IsString(j))
                    strncpy(cell->room, j->valuestring, TT_ROOM_LEN - 1);

                j = cJSON_GetObjectItem(co, "weeks");
                if (j && cJSON_IsString(j))
                    cell->weeks = tt_parse_weeks(j->valuestring);
                else
                    cell->weeks = 0x01FFFFFFu;
            }
        }
    }

    cJSON_Delete(root);

    esp_err_t err = timetable_set_config(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── HTTP: POST /timetable_show ──────────────────────────────────── */

esp_err_t timetable_show_http_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (!epd_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"EPD not ready\"}");
        return ESP_OK;
    }

    /* parse optional {"day":0..6} from body */
    int show_day = -1;
    int body_remaining = req->content_len;
    if (body_remaining > 0 && body_remaining < 128) {
        char buf[128];
        int r = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (r > 0) {
            body_remaining -= r;
            buf[r] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *jd = cJSON_GetObjectItem(root, "day");
                if (cJSON_IsNumber(jd))
                    show_day = jd->valueint;
                cJSON_Delete(root);
            }
        }
    }

    /* drain remaining body */
    char tmp[64];
    while (body_remaining > 0) {
        int to_recv = body_remaining < (int)sizeof(tmp) ? body_remaining : (int)sizeof(tmp);
        int r = httpd_req_recv(req, tmp, to_recv);
        if (r <= 0) break;
        body_remaining -= r;
    }

    display_policy_begin_manual_display();

    esp_err_t err;
    if (show_day >= 0 && show_day < TT_DAYS)
        err = timetable_show_day(show_day);
    else
        err = timetable_show();

    if (err == ESP_OK) {
        button_set_current_mode(DISPLAY_MODE_TIMETABLE);
        power_mgr_save_mode(DISPLAY_MODE_TIMETABLE);
    }

    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
