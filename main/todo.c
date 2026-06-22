#include "todo.h"

#include <string.h>
#include <stdio.h>

#include "cJSON.h"
#include "display_policy.h"
#include "epd.h"
#include "esp_log.h"
#include "http_internal.h"
#include "fb_render.h"
#include "ui_theme.h"
#include "nvs.h"
#include "time_sync.h"
#include "button.h"
#include "display_mode.h"
#include "power_mgr.h"

#include "freertos/semphr.h"

static const char *TAG    = "todo";
static const char *NVS_NS = "todo";

static todo_config_t s_cfg;
static SemaphoreHandle_t s_cfg_mutex;

/* NVS persistence */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t v8;
    if (nvs_get_u8(h, "enabled", &v8) == ESP_OK)
        s_cfg.enabled = (v8 != 0);

    size_t sz = sizeof(s_cfg.items);
    if (nvs_get_blob(h, "items", s_cfg.items, &sz) == ESP_OK)
        s_cfg.count = sz / sizeof(todo_item_t);

    nvs_close(h);
}

static void nvs_save_locked_snapshot(const todo_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_u8(h, "enabled", cfg->enabled ? 1 : 0);
    nvs_set_blob(h, "items", cfg->items,
                 (size_t)cfg->count * sizeof(todo_item_t));
    nvs_commit(h);
    nvs_close(h);
}

/* EPD rendering */

static int count_pending_in(const todo_config_t *cfg)
{
    int n = 0;
    for (int i = 0; i < cfg->count; i++)
        if (!cfg->items[i].done) n++;
    return n;
}

static void draw_todo_stat_card(fb_t *fb, int x, int y, int w, int h,
                                const char *label, const char *value,
                                fb_color_t accent, int value_scale)
{
    if (value_scale > 1 && h < 58)
        value_scale = 1;
    ui_draw_card(fb, x, y, w, h, accent == COLOR_RED);
    int inner_x = x + 8;
    int inner_w = w - 16;
    if (inner_w < 1)
        inner_w = 1;

    int label_y = y + (h < 58 ? 5 : 8);
    int value_y = y + h - 16 * value_scale - (h < 58 ? 5 : 10);
    int min_value_y = label_y + 16 + 2;
    if (value_y < min_value_y)
        value_y = min_value_y;
    if (value_y + 16 * value_scale > y + h - 2)
        value_y = y + h - 2 - 16 * value_scale;
    if (value_y < label_y)
        value_y = label_y;

    fb_utf8_scaled_maxw(fb, inner_x, label_y, label, accent, 1, inner_w);
    fb_utf8_scaled_maxw(fb, inner_x, value_y, value,
                        COLOR_BLACK, value_scale, inner_w);
}

/* Select visible rows by usefulness when the screen cannot show every item. */
static int collect_todo_visible_indices(const todo_config_t *cfg,
                                        int *out, int max_out)
{
    int n = 0;
    for (int i = 0; i < cfg->count && n < max_out; i++) {
        if (!cfg->items[i].done && cfg->items[i].priority >= 2)
            out[n++] = i;
    }
    for (int i = 0; i < cfg->count && n < max_out; i++) {
        if (!cfg->items[i].done && cfg->items[i].priority == 1)
            out[n++] = i;
    }
    for (int i = 0; i < cfg->count && n < max_out; i++) {
        if (!cfg->items[i].done && cfg->items[i].priority == 0)
            out[n++] = i;
    }
    for (int i = 0; i < cfg->count && n < max_out; i++) {
        if (cfg->items[i].done)
            out[n++] = i;
    }
    return n;
}

static bool render_todo_snapshot(const todo_config_t *cfg, unsigned epoch)
{
    fb_t *fb = fb_create();
    if (!fb) return false;
    fb_clear(fb);

    const int W = fb->width;
    const int H = fb->height;
    const int s = (W >= 600) ? 2 : 1;
    const int pad = 14 * s;

    int done_n = 0;
    for (int i = 0; i < cfg->count; i++)
        if (cfg->items[i].done) done_n++;

    char right[16] = "";
    struct tm now;
    if (time_sync_get_local_relaxed(&now)) {
        snprintf(right, sizeof(right), "%02d:%02d", now.tm_hour, now.tm_min);
    }
    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
    ui_draw_header(fb, "\xe5\xbe\x85\xe5\x8a\x9e\xe4\xba\x8b\xe9\xa1\xb9", right, true);

    if (cfg->count == 0) {
        ui_draw_empty_state(fb, "\xe6\x9a\x82\xe6\x97\xa0\xe5\xbe\x85\xe5\x8a\x9e",
                            "\xe8\xaf\xb7\xe9\x80\x9a\xe8\xbf\x87\xe7\xbd\x91\xe9\xa1\xb5\xe6\xb7\xbb\xe5\x8a\xa0");
        if (!display_policy_epoch_is_current(epoch)) {
            fb_destroy(fb);
            return false;
        }
        return epd_display_fb_free(fb) == ESP_OK;
    }

    int pct = done_n * 100 / cfg->count;
    int pending_n = cfg->count - done_n;
    int urgent_n = 0;
    int important_n = 0;
    for (int i = 0; i < cfg->count; i++) {
        if (cfg->items[i].done)
            continue;
        if (cfg->items[i].priority >= 2)
            urgent_n++;
        else if (cfg->items[i].priority == 1)
            important_n++;
    }

    int summary_y = 32 * s;
    int summary_h = 42 * s;
    int gap = 6 * s;
    int stat_w = (W - 2 * pad - 2 * gap) / 3;

    char pending_str[16];
    char done_str[16];
    char focus_str[24];
    snprintf(pending_str, sizeof(pending_str), "%d", pending_n);
    snprintf(done_str, sizeof(done_str), "%d%%", pct);
    snprintf(focus_str, sizeof(focus_str), "%d/%d",
             urgent_n, important_n);

    draw_todo_stat_card(fb, pad, summary_y, stat_w, summary_h,
                        "\xe5\x89\xa9\xe4\xbd\x99", pending_str,
                        pending_n ? COLOR_RED : COLOR_BLACK, 2);
    draw_todo_stat_card(fb, pad + stat_w + gap, summary_y, stat_w, summary_h,
                        "\xe5\xae\x8c\xe6\x88\x90\xe7\x8e\x87", done_str,
                        pct >= 100 ? COLOR_BLACK : COLOR_RED, 2);
    draw_todo_stat_card(fb, pad + (stat_w + gap) * 2, summary_y, stat_w, summary_h,
                        "\xe7\xb4\xa7\xe6\x80\xa5/\xe9\x87\x8d\xe8\xa6\x81",
                        focus_str, urgent_n ? COLOR_RED : COLOR_BLACK, 2);

    int bar_y = summary_y + summary_h + 5 * s;
    ui_draw_progress_bar(fb, pad, bar_y, W - 2 * pad, 5 * s, pct,
                         (pct >= 100) ? COLOR_BLACK : COLOR_RED);

    int list_y = bar_y + 9 * s;
    int footer_h = 22 * s;
    int avail  = H - list_y - footer_h - 6 * s;
    int item_gap = 4 * s;
    const int min_item_h = 20 * s;
    const int max_item_h = 32 * s;
    if (avail < min_item_h)
        avail = min_item_h;

    int max_fit = (avail + item_gap) / (min_item_h + item_gap);
    if (max_fit < 1) max_fit = 1;
    if (max_fit > 6) max_fit = 6;
    if (W <= 400 && max_fit > 5) max_fit = 5;

    int n_show = cfg->count < max_fit ? (int)cfg->count : max_fit;
    int visible_idx[6];
    n_show = collect_todo_visible_indices(cfg, visible_idx, n_show);
    int item_h = (avail - item_gap * (n_show - 1)) / n_show;
    if (item_h > max_item_h) item_h = max_item_h;
    if (item_h < min_item_h) item_h = min_item_h;
    int sc   = (s > 1 && item_h >= 54) ? 2 : 1;
    int ch_h = 16 * sc;
    const int pri_sc = 1;

    for (int i = 0; i < n_show; i++) {
        const todo_item_t *it = &cfg->items[visible_idx[i]];
        int y      = list_y + i * (item_h + item_gap);
        int text_y = y + (item_h - ch_h) / 2;

        ui_draw_card(fb, pad, y, W - 2 * pad, item_h,
                     (!it->done && it->priority >= 2));

        if (it->priority >= 2)
            fb_fill_rect(fb, pad + 1, y + 5, 3 * s, item_h - 10, COLOR_RED);
        else if (it->priority == 1)
            fb_fill_rect(fb, pad + 1, y + 5, 3 * s, item_h - 10, COLOR_BLACK);

        int cb_x  = pad + 12 * s;
        int cb_sz = ch_h - 2;
        if (cb_sz > 14 * s) cb_sz = 14 * s;
        if (cb_sz < 10) cb_sz = 10;
        int cb_y  = text_y + (ch_h - cb_sz) / 2;

        if (it->done) {
            fb_fill_rect(fb, cb_x, cb_y, cb_sz, cb_sz, COLOR_BLACK);
            for (int t = 0; t < 2; t++) {
                for (int d = 0; d <= cb_sz / 3; d++)
                    fb_pixel(fb,
                             cb_x + cb_sz / 4 + d,
                             cb_y + cb_sz * 2 / 5 + d + t, COLOR_WHITE);
                for (int d = 0; d <= cb_sz / 2; d++)
                    fb_pixel(fb,
                             cb_x + cb_sz / 4 + cb_sz / 3 + d,
                             cb_y + cb_sz * 2 / 5 + cb_sz / 3 - d + t,
                             COLOR_WHITE);
            }
        } else {
            fb_rect(fb, cb_x, cb_y, cb_sz, cb_sz, COLOR_BLACK);
        }

        int tx    = cb_x + cb_sz + 8 * s;
        bool show_pri = (it->priority > 0);
        int pri_w = show_pri ? (2 * 16 * pri_sc + 12 * s) : 0;
        int max_w = W - tx - pri_w - 16 * s;
        if (max_w < 16 * sc) {
            show_pri = false;
            pri_w = 0;
            max_w = W - tx - 8 * s;
        }
        if (max_w < 1) max_w = 1;
        int drawn_w = fb_utf8_scaled_maxw(fb, tx, text_y, it->text,
                                          COLOR_BLACK, sc, max_w);

        if (it->done) {
            int ly = text_y + ch_h / 2;
            int strike_w = (drawn_w > 0 && drawn_w < max_w) ? drawn_w : max_w;
            fb_hline(fb, tx, ly, strike_w, COLOR_BLACK);
        }

        int pri_x = W - 2 * 16 * pri_sc - 13 * s;
        if (show_pri && it->priority >= 2)
            fb_utf8_scaled(fb, pri_x, text_y,
                           "\xe7\xb4\xa7\xe6\x80\xa5", COLOR_RED, pri_sc);
        else if (show_pri && it->priority == 1)
            fb_utf8_scaled(fb, pri_x, text_y,
                           "\xe9\x87\x8d\xe8\xa6\x81", COLOR_BLACK, pri_sc);
    }

    char foot[20];
    if (cfg->count > n_show)
        snprintf(foot, sizeof(foot), "%d/%d", n_show, (int)cfg->count);
    else
        snprintf(foot, sizeof(foot), "%d PENDING", pending_n);
    ui_draw_footer(fb, "TODO", foot);

    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        return false;
    }
    return epd_display_fb_free(fb) == ESP_OK;
}

/* Public API */

esp_err_t todo_init(void)
{
    s_cfg_mutex = xSemaphoreCreateMutex();
    if (!s_cfg_mutex)
        return ESP_ERR_NO_MEM;
    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_load();
    ESP_LOGI(TAG, "init ok (enabled=%d, items=%d)", s_cfg.enabled, s_cfg.count);
    return ESP_OK;
}

esp_err_t todo_get_config(todo_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t todo_set_config(const todo_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    todo_config_t snap;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    s_cfg = *cfg;
    if (s_cfg.count > TODO_MAX_ITEMS)
        s_cfg.count = TODO_MAX_ITEMS;
    snap = s_cfg;
    xSemaphoreGive(s_cfg_mutex);
    /* Release lock before NVS flash write to avoid blocking readers. */
    nvs_save_locked_snapshot(&snap);
    return ESP_OK;
}

esp_err_t todo_show(void)
{
    todo_config_t snap;
    if (todo_get_config(&snap) != ESP_OK)
        return ESP_FAIL;
    if (!snap.enabled)
        return ESP_ERR_INVALID_STATE;
    if (!epd_is_ready())
        return ESP_ERR_INVALID_STATE;

    unsigned epoch = display_policy_display_epoch();
    if (!render_todo_snapshot(&snap, epoch))
        return ESP_ERR_INVALID_STATE;
    display_policy_set_manual_screen_active(true);
    ESP_LOGI(TAG, "displayed %d items (%d pending)",
             snap.count, count_pending_in(&snap));
    return ESP_OK;
}

/* HTTP: GET /todo.json */

esp_err_t todo_http_get_handler(httpd_req_t *req)
{
    todo_config_t snap;
    if (todo_get_config(&snap) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "enabled", snap.enabled);
    cJSON *arr = cJSON_AddArrayToObject(root, "items");

    for (int i = 0; i < snap.count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "text", snap.items[i].text);
        cJSON_AddBoolToObject(obj, "done", snap.items[i].done);
        cJSON_AddNumberToObject(obj, "priority", snap.items[i].priority);
        cJSON_AddItemToArray(arr, obj);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

/* HTTP: POST /todo */

esp_err_t todo_http_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    int len = req->content_len;
    if (len <= 0 || len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = 0;
    while (received < len) {
        int got = httpd_req_recv(req, buf + received, (size_t)(len - received));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) { free(buf); return ESP_FAIL; }
        received += got;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);

    cJSON *j;
    j = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(j))
        s_cfg.enabled = cJSON_IsTrue(j);

    j = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(j)) {
        int n = cJSON_GetArraySize(j);
        if (n > TODO_MAX_ITEMS) n = TODO_MAX_ITEMS;
        s_cfg.count = (uint8_t)n;
        memset(s_cfg.items, 0, sizeof(s_cfg.items));

        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(j, i);
            if (!item) continue;

            cJSON *t = cJSON_GetObjectItem(item, "text");
            if (cJSON_IsString(t) && t->valuestring) {
                strncpy(s_cfg.items[i].text, t->valuestring, TODO_TEXT_LEN - 1);
                s_cfg.items[i].text[TODO_TEXT_LEN - 1] = '\0';
            }

            cJSON *d = cJSON_GetObjectItem(item, "done");
            if (cJSON_IsBool(d))
                s_cfg.items[i].done = cJSON_IsTrue(d);

            cJSON *p = cJSON_GetObjectItem(item, "priority");
            if (cJSON_IsNumber(p))
                s_cfg.items[i].priority = (uint8_t)p->valueint;
        }
    }

    todo_config_t snap = s_cfg;
    xSemaphoreGive(s_cfg_mutex);
    /* Release lock before NVS flash write to avoid blocking readers. */
    cJSON_Delete(root);

    nvs_save_locked_snapshot(&snap);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* HTTP: POST /todo_show */

esp_err_t todo_show_http_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (!epd_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"EPD busy\"}");
        return ESP_OK;
    }

    todo_config_t snap;
    if (todo_get_config(&snap) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* allow showing even if not "enabled" - treat as manual push */
    if (snap.count == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"no items\"}");
        return ESP_OK;
    }

    unsigned epoch = display_policy_begin_manual_display();
    if (!render_todo_snapshot(&snap, epoch)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"display canceled\"}");
        return ESP_OK;
    }
    display_policy_set_manual_screen_active(true);
    button_set_current_mode(DISPLAY_MODE_TODO);
    power_mgr_save_mode(DISPLAY_MODE_TODO);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
