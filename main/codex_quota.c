#include "codex_quota.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

#include "battery_mon.h"
#include "display_mode.h"
#include "display_policy.h"
#include "epd.h"
#include "fb_render.h"
#include "time_sync.h"
#include "ui_theme.h"

static const char *TAG = "codex_quota";
static const char *NVS_NS = "codex_quota";

#define QUOTA_HTTP_TIMEOUT_MS 30000
#define QUOTA_HTTP_RETRIES    2
#define QUOTA_MAX_RESP_LEN    8192
#define QUOTA_SHOW_STACK      16384
#define QUOTA_AUTO_STACK      4096
#define QUOTA_AUTO_POLL_MS    5000
#define QUOTA_MIN_REFRESH_MIN 5
#define QUOTA_MAX_REFRESH_MIN 1440
#define ONE_API_QUOTA_PER_USD 500000.0

static codex_quota_config_t s_cfg = {
    .enabled = false,
    .api_url = "",
    .api_key = "",
    .unit = "USD",
    .refresh_min = QUOTA_MIN_REFRESH_MIN,
};

static codex_quota_data_t s_data;
static SemaphoreHandle_t  s_mutex;
static portMUX_TYPE       s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t       s_auto_task;
static volatile TickType_t s_last_fetch_tick;
static bool               s_auto_network_allowed = true;

typedef struct {
    char *buf;
    int   len;
    int   cap;
} resp_buf_t;

static void config_snapshot(codex_quota_config_t *out)
{
    portENTER_CRITICAL(&s_cfg_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
}

static bool config_has_api(const codex_quota_config_t *cfg)
{
    return cfg && cfg->enabled && cfg->api_url[0] && cfg->api_key[0];
}

static uint32_t normalize_refresh_min(uint32_t refresh_min)
{
    if (refresh_min == 0)
        return 0;
    if (refresh_min < QUOTA_MIN_REFRESH_MIN)
        return QUOTA_MIN_REFRESH_MIN;
    if (refresh_min > QUOTA_MAX_REFRESH_MIN)
        return QUOTA_MAX_REFRESH_MIN;
    return refresh_min;
}

static void build_quota_url(const codex_quota_config_t *cfg, char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return;
    if (!cfg || !cfg->api_url[0]) {
        out[0] = '\0';
        return;
    }

    snprintf(out, out_len, "%s", cfg->api_url);
    if (strstr(out, "/v1/usage"))
        return;
    char *old_usage = strstr(out, "/api/usage/token");
    if (!old_usage)
        old_usage = strstr(out, "/api/user/self");
    if (old_usage)
        *old_usage = '\0';

    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '/') {
        out[n - 1] = '\0';
        n--;
    }
    if (n >= 3 && strcmp(out + n - 3, "/v1") == 0)
        snprintf(out + n, out_len > n ? out_len - n : 0, "/usage");
    else
        snprintf(out + n, out_len > n ? out_len - n : 0, "/v1/usage");
}

static void normalize_config(codex_quota_config_t *cfg)
{
    if (!cfg)
        return;
    if (cfg->unit[0] == '\0')
        snprintf(cfg->unit, sizeof(cfg->unit), "USD");
    cfg->refresh_min = normalize_refresh_min(cfg->refresh_min);
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return;

    uint8_t en = 0;
    if (nvs_get_u8(h, "enabled", &en) == ESP_OK)
        s_cfg.enabled = en != 0;

    size_t len = sizeof(s_cfg.api_url);
    nvs_get_str(h, "api_url", s_cfg.api_url, &len);
    len = sizeof(s_cfg.api_key);
    nvs_get_str(h, "api_key", s_cfg.api_key, &len);
    len = sizeof(s_cfg.unit);
    nvs_get_str(h, "unit", s_cfg.unit, &len);
    uint32_t refresh = s_cfg.refresh_min;
    if (nvs_get_u32(h, "refresh", &refresh) == ESP_OK)
        s_cfg.refresh_min = refresh;
    normalize_config(&s_cfg);
    nvs_close(h);
}

static void nvs_save_snapshot(const codex_quota_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, "enabled", cfg->enabled ? 1 : 0);
    nvs_set_str(h, "api_url", cfg->api_url);
    nvs_set_str(h, "api_key", cfg->api_key);
    nvs_set_str(h, "unit", cfg->unit);
    nvs_set_u32(h, "refresh", cfg->refresh_min);
    nvs_commit(h);
    nvs_close(h);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb && evt->data && evt->data_len > 0) {
        int avail = rb->cap - 1 - rb->len;
        if (avail > 0) {
            int n = evt->data_len;
            if (n > avail)
                n = avail;
            memcpy(rb->buf + rb->len, evt->data, (size_t)n);
            rb->len += n;
            rb->buf[rb->len] = '\0';
        }
    }
    return ESP_OK;
}

static void log_tls_heap_state(void)
{
    ESP_LOGI(TAG, "before quota HTTPS heap: free=%lu largest=%lu internal=%lu/%lu psram=%lu/%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static char *http_get_quota(const codex_quota_config_t *cfg)
{
    char url[224];
    build_quota_url(cfg, url, sizeof(url));

    resp_buf_t rb = {
        .buf = malloc(QUOTA_MAX_RESP_LEN),
        .len = 0,
        .cap = QUOTA_MAX_RESP_LEN,
    };
    if (!rb.buf)
        return NULL;

    esp_err_t err = ESP_FAIL;
    int status = 0;
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);

    for (int attempt = 0; attempt < QUOTA_HTTP_RETRIES; attempt++) {
        rb.len = 0;
        rb.buf[0] = '\0';
        if (attempt > 0) {
            ESP_LOGW(TAG, "quota HTTP retry %d/%d", attempt + 1, QUOTA_HTTP_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        log_tls_heap_state();
        esp_http_client_config_t config = {
            .url = url,
            .event_handler = http_event_handler,
            .user_data = &rb,
            .timeout_ms = QUOTA_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            free(rb.buf);
            return NULL;
        }
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_header(client, "Accept", "application/json");
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err == ESP_OK && status >= 200 && status < 300)
            break;
        ESP_LOGW(TAG, "quota GET attempt %d failed: err=%s status=%d",
                 attempt + 1, esp_err_to_name(err), status);
    }

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "quota GET failed after retries: err=%s status=%d",
                 esp_err_to_name(err), status);
        free(rb.buf);
        return NULL;
    }

    ESP_LOGI(TAG, "quota GET OK (%d bytes)", rb.len);
    return rb.buf;
}

static cJSON *json_get_path(cJSON *root, const char *path)
{
    if (!root || !path || !path[0])
        return NULL;
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s", path);
    cJSON *cur = root;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ".", &save); tok; tok = strtok_r(NULL, ".", &save)) {
        cur = cJSON_GetObjectItem(cur, tok);
        if (!cur)
            return NULL;
    }
    return cur;
}

static bool json_number_any(cJSON *root, const char *const *paths, int n, double *out)
{
    for (int i = 0; i < n; i++) {
        cJSON *j = json_get_path(root, paths[i]);
        if (cJSON_IsNumber(j)) {
            *out = j->valuedouble;
            return true;
        }
        if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) {
            char *end = NULL;
            double v = strtod(j->valuestring, &end);
            if (end && end != j->valuestring) {
                *out = v;
                return true;
            }
        }
    }
    return false;
}

static bool json_string_any(cJSON *root, const char *const *paths, int n,
                            char *out, size_t out_len)
{
    for (int i = 0; i < n; i++) {
        cJSON *j = json_get_path(root, paths[i]);
        const char *s = cJSON_GetStringValue(j);
        if (s && s[0]) {
            snprintf(out, out_len, "%s", s);
            return true;
        }
    }
    return false;
}

static bool parse_quota_json(const char *json, codex_quota_data_t *out,
                             const codex_quota_config_t *cfg)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return false;

    memset(out, 0, sizeof(*out));
    snprintf(out->unit, sizeof(out->unit), "%s", cfg->unit[0] ? cfg->unit : "USD");
    json_string_any(root, (const char *const[]){"unit", "data.unit"}, 2,
                    out->unit, sizeof(out->unit));

    static const char *const remain_paths[] = {
        "total_available", "available", "balance", "remain", "remaining",
        "data.total_available", "data.available", "data.balance",
        "data.remain", "data.remaining", "data.quota", "quota"
    };
    static const char *const used_paths[] = {
        "total_used", "used", "used_quota", "usage",
        "actual_cost", "cost",
        "usage.total.actual_cost", "usage.total.cost",
        "data.total_used", "data.used", "data.used_quota", "data.usage",
        "data.usage.total.actual_cost", "data.usage.total.cost"
    };
    static const char *const total_paths[] = {
        "total_granted", "total", "credit", "credits", "hard_limit_usd",
        "data.total_granted", "data.total", "data.credit", "data.credits",
        "subscription.hard_limit_usd", "data.subscription.hard_limit_usd"
    };
    static const char *const request_paths[] = {
        "request_count", "requests", "usage.total.requests", "usage.today.requests",
        "data.request_count", "data.requests", "data.usage.total.requests",
        "data.usage.today.requests"
    };
    static const char *const today_cost_paths[] = {
        "today_actual_cost", "today_cost", "usage.today.actual_cost",
        "usage.today.cost", "data.today_actual_cost", "data.today_cost",
        "data.usage.today.actual_cost", "data.usage.today.cost"
    };
    static const char *const token_paths[] = {
        "total_tokens", "tokens", "usage.total.total_tokens",
        "usage.today.total_tokens", "data.total_tokens", "data.tokens",
        "data.usage.total.total_tokens", "data.usage.today.total_tokens"
    };
    static const char *const account_paths[] = {
        "username", "name", "email", "group", "planName", "mode",
        "data.username", "data.name", "data.email", "data.group",
        "data.planName", "data.mode"
    };
    static const char *const msg_paths[] = {
        "message", "msg", "error.message", "data.message", "data.msg"
    };

    out->have_remaining = json_number_any(root, remain_paths,
                                          (int)(sizeof(remain_paths) / sizeof(remain_paths[0])),
                                          &out->remaining);
    out->have_used = json_number_any(root, used_paths,
                                     (int)(sizeof(used_paths) / sizeof(used_paths[0])),
                                     &out->used);
    out->have_total = json_number_any(root, total_paths,
                                      (int)(sizeof(total_paths) / sizeof(total_paths[0])),
                                      &out->total);

    double req = 0;
    if (json_number_any(root, request_paths,
                        (int)(sizeof(request_paths) / sizeof(request_paths[0])), &req)) {
        out->request_count = (int)req;
    }
    out->have_today_cost = json_number_any(root, today_cost_paths,
                                           (int)(sizeof(today_cost_paths) / sizeof(today_cost_paths[0])),
                                           &out->today_cost);
    out->have_total_tokens = json_number_any(root, token_paths,
                                             (int)(sizeof(token_paths) / sizeof(token_paths[0])),
                                             &out->total_tokens);
    json_string_any(root, account_paths,
                    (int)(sizeof(account_paths) / sizeof(account_paths[0])),
                    out->account, sizeof(out->account));
    if (!out->account[0])
        snprintf(out->account, sizeof(out->account), "Codex");

    if (!out->have_total && out->have_remaining && out->have_used) {
        out->total = out->remaining + out->used;
        out->have_total = true;
    }
    if (!out->have_remaining && out->have_total && out->have_used) {
        out->remaining = out->total - out->used;
        out->have_remaining = true;
    }
    if (!out->have_used && out->have_total && out->have_remaining) {
        out->used = out->total - out->remaining;
        out->have_used = true;
    }

    double max_abs = 0;
    if (out->have_total && fabs(out->total) > max_abs) max_abs = fabs(out->total);
    if (out->have_used && fabs(out->used) > max_abs) max_abs = fabs(out->used);
    if (out->have_remaining && fabs(out->remaining) > max_abs) max_abs = fabs(out->remaining);
    if (max_abs > 10000.0) {
        if (out->have_total) out->total /= ONE_API_QUOTA_PER_USD;
        if (out->have_used) out->used /= ONE_API_QUOTA_PER_USD;
        if (out->have_remaining) out->remaining /= ONE_API_QUOTA_PER_USD;
        snprintf(out->message, sizeof(out->message), "已按 One API 额度比例换算");
    }

    if (out->have_total && out->total > 0 && out->have_used) {
        int pct = (int)((out->used * 100.0 / out->total) + 0.5);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        out->percent_used = pct;
    }

    time_sync_get_str(out->update_time, sizeof(out->update_time));
    if (!out->update_time[0])
        snprintf(out->update_time, sizeof(out->update_time), "--:--");

    out->valid = out->have_remaining || out->have_used || out->have_total;
    if (!out->valid) {
        if (!json_string_any(root, msg_paths,
                             (int)(sizeof(msg_paths) / sizeof(msg_paths[0])),
                             out->message, sizeof(out->message))) {
            snprintf(out->message, sizeof(out->message), "\xe6\x9c\xaa\xe8\xaf\x86\xe5\x88\xab\xe9\xa2\x9d\xe5\xba\xa6\xe5\xad\x97\xe6\xae\xb5");
        }
    }

    cJSON_Delete(root);
    return out->valid;
}

static void format_amount(char *buf, size_t len, bool have, double value,
                          const char *unit)
{
    if (!have) {
        snprintf(buf, len, "--");
        return;
    }
    if (fabs(value) >= 1000.0)
        snprintf(buf, len, "%.0f %s", value, unit ? unit : "");
    else
        snprintf(buf, len, "%.2f %s", value, unit ? unit : "");
}

static void quota_date_header(char *buf, size_t len)
{
    static const char *const wd[] = {
        "周日", "周一", "周二", "周三", "周四", "周五", "周六"
    };
    struct tm tm;
    if (time_sync_get_local_relaxed(&tm)) {
        snprintf(buf, len, "%d\xe6\x9c\x88%d\xe6\x97\xa5 %s\xe6\x99\x9a\xe4\xb8\x8a", tm.tm_mon + 1, tm.tm_mday,
                 wd[tm.tm_wday]);
    } else {
        snprintf(buf, len, "AI用量看板");
    }
}

static void quota_reset_label(char *buf, size_t len)
{
    struct tm tm;
    if (time_sync_get_local_relaxed(&tm)) {
        snprintf(buf, len, "%02d-%02d", tm.tm_mon + 1, tm.tm_mday);
    } else {
        snprintf(buf, len, "--");
    }
}

static void draw_header_like_reference(fb_t *fb, const codex_quota_data_t *data)
{
    const int W = fb->width;
    const int header_h = 34;
    fb_fill_rect(fb, 6, 6, W - 12, header_h, COLOR_RED);
    fb_utf8_scaled(fb, 18, 14, "Codex额度", COLOR_WHITE, 1);

    char center[40];
    quota_date_header(center, sizeof(center));
    int cw = ui_text_width(center, 1);
    fb_utf8_scaled_maxw(fb, (W - cw) / 2, 16, center, COLOR_WHITE, 1, 140);

    battery_status_t bat = {0};
    battery_mon_get_status(&bat);
    char right[12];
    if (bat.valid)
        snprintf(right, sizeof(right), "%u%%", (unsigned)bat.percent);
    else if (data && data->valid && data->percent_used > 0)
        snprintf(right, sizeof(right), "%d%%", data->percent_used);
    else
        snprintf(right, sizeof(right), "--");
    fb_utf8_scaled(fb, W - 38, 16, right, COLOR_WHITE, 1);
}

static void draw_quota_bar(fb_t *fb, int x, int y, int w, int h, int percent,
                           fb_color_t fill)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    fb_rect(fb, x, y, w, h, COLOR_BLACK);
    int inner = (w - 4) * percent / 100;
    if (percent > 0 && inner < 2)
        inner = 2;
    if (inner > 0)
        fb_fill_rect(fb, x + 2, y + 2, inner, h - 4, fill);
}

static void draw_quota_row(fb_t *fb, int y, const char *label, int percent,
                           fb_color_t color, const char *right)
{
    const int label_x = 18;
    const int bar_x = 96;
    const int pct_x = 248;
    const int date_x = 306;
    fb_utf8_scaled_maxw(fb, label_x, y - 2, label, COLOR_BLACK, 1, 72);
    draw_quota_bar(fb, bar_x, y, 142, 12, percent, color);

    char pct[10];
    snprintf(pct, sizeof(pct), "%d%%", percent);
    fb_utf8_scaled(fb, pct_x, y - 2, pct, color == COLOR_RED ? COLOR_RED : COLOR_BLACK, 1);
    fb_utf8_scaled_maxw(fb, date_x, y - 2, right ? right : "--", COLOR_BLACK, 1, 84);
}

static void draw_quota_value_row(fb_t *fb, int y, const char *label,
                                 const char *value, fb_color_t color,
                                 const char *right)
{
    const int label_x = 18;
    const int value_x = 96;
    const int date_x = 306;
    fb_utf8_scaled_maxw(fb, label_x, y - 2, label, COLOR_BLACK, 1, 72);
    fb_utf8_scaled_maxw(fb, value_x, y - 2, value ? value : "--", color, 1, 200);
    fb_utf8_scaled_maxw(fb, date_x, y - 2, right ? right : "--", COLOR_BLACK, 1, 84);
}

static void draw_section_title(fb_t *fb, int x, int y, const char *title)
{
    fb_utf8_scaled(fb, x, y, title, COLOR_BLACK, 2);
}

static int quota_percent_from_data(const codex_quota_data_t *data)
{
    if (!data || !data->valid)
        return 0;
    if (data->have_total && data->have_used)
        return data->percent_used;
    if (data->have_remaining)
        return data->remaining > 0 ? 100 : 0;
    return 0;
}

static int percent_from_ratio(double part, double total)
{
    if (total <= 0)
        return 0;
    int pct = (int)((part * 100.0 / total) + 0.5);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

static void format_request_count(char *buf, size_t len, int count)
{
    if (count < 0)
        count = 0;
    if (count >= 100000000)
        snprintf(buf, len, "%.1f亿次", count / 100000000.0);
    else if (count >= 10000)
        snprintf(buf, len, "%.1f万次", count / 10000.0);
    else
        snprintf(buf, len, "%d\xe6\xac\xa1", count);
}

static void format_token_count(char *buf, size_t len, bool have, double tokens)
{
    if (!have || tokens <= 0) {
        snprintf(buf, len, "--");
    } else if (tokens >= 100000000.0) {
        snprintf(buf, len, "%.1f\xe4\xba\xbf", tokens / 100000000.0);
    } else if (tokens >= 10000.0) {
        snprintf(buf, len, "%.1f\xe4\xb8\x87", tokens / 10000.0);
    } else {
        snprintf(buf, len, "%.0f", tokens);
    }
}

static void quota_refresh_label(const codex_quota_config_t *cfg, char *buf, size_t len)
{
    uint32_t refresh_min = cfg ? cfg->refresh_min : 0;
    if (refresh_min > 0)
        snprintf(buf, len, "自动%lu分钟刷新", (unsigned long)refresh_min);
    else
        snprintf(buf, len, "\xe6\x98\xbe\xe7\xa4\xba\xe6\x97\xb6\xe6\x9b\xb4\xe6\x96\xb0");
}

static void render_quota_page(const codex_quota_config_t *cfg,
                              const codex_quota_data_t *data,
                              const char *error)
{
    fb_t *fb = fb_create();
    if (!fb)
        return;

    const int H = fb->height;
    const int W = fb->width;
    char refresh_label[32];
    quota_refresh_label(cfg, refresh_label, sizeof(refresh_label));

    fb_rect(fb, 6, 6, W - 12, H - 12, COLOR_BLACK);
    draw_header_like_reference(fb, data);

    if (error) {
        ui_draw_empty_state(fb, "\xe9\xa2\x9d\xe5\xba\xa6\xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8", error);
        fb_hline(fb, 14, H - 28, W - 28, COLOR_BLACK);
        fb_utf8_scaled(fb, 20, H - 20, refresh_label, COLOR_BLACK, 1);
        fb_utf8_scaled(fb, W - 44, H - 20, "1/1", COLOR_BLACK, 1);
        epd_display_fb_free(fb);
        return;
    }

    char reset[16];
    quota_reset_label(reset, sizeof(reset));
    int used_pct = quota_percent_from_data(data);
    int remain_pct = 0;
    if (data->have_remaining && data->have_total && data->total > 0) {
        remain_pct = percent_from_ratio(data->remaining, data->total);
    }
    int today_pct = 0;
    if (data->have_today_cost)
        today_pct = percent_from_ratio(data->today_cost,
                                       data->today_cost + (data->have_remaining ? data->remaining : 0));

    char remain[40], used[40], today[40], req[24], tokens[24];
    format_amount(remain, sizeof(remain), data->have_remaining, data->remaining, data->unit);
    format_amount(used, sizeof(used), data->have_used, data->used, data->unit);
    format_amount(today, sizeof(today), data->have_today_cost, data->today_cost, data->unit);
    format_request_count(req, sizeof(req), data->request_count);
    format_token_count(tokens, sizeof(tokens), data->have_total_tokens, data->total_tokens);

    int y = 56;
    draw_section_title(fb, 18, y, "额度");
    y += 42;
    draw_quota_row(fb, y, "余额", remain_pct,
                   remain_pct <= 30 ? COLOR_RED : COLOR_BLACK, remain);
    y += 26;
    draw_quota_row(fb, y, "已用", used_pct,
                   used_pct >= 90 ? COLOR_RED : COLOR_BLACK, used);
    y += 26;
    draw_quota_row(fb, y, "今日", today_pct,
                   today_pct >= 80 ? COLOR_RED : COLOR_BLACK, today);
    y += 26;
    draw_quota_row(fb, y, "总计", used_pct,
                   used_pct >= 90 ? COLOR_RED : COLOR_BLACK, used);
    y += 30;
    draw_quota_value_row(fb, y, "请求", req, COLOR_BLACK, reset);
    y += 24;
    draw_quota_value_row(fb, y, "令牌", tokens, COLOR_BLACK, reset);

    ui_draw_dotted_hline(fb, 14, H - 32, W - 28, COLOR_BLACK, 4);
    fb_utf8_scaled(fb, 20, H - 22, refresh_label, COLOR_BLACK, 1);
    fb_utf8_scaled(fb, W / 2 - 16, H - 22, "[Zz]", COLOR_BLACK, 1);
    fb_utf8_scaled(fb, W - 44, H - 22, cfg && cfg->enabled ? "1/1" : "0/1",
                   COLOR_BLACK, 1);
    epd_display_fb_free(fb);
}

static bool quota_auto_refresh_due(uint32_t refresh_min)
{
    TickType_t last = s_last_fetch_tick;
    if (last == 0)
        return true;
    TickType_t interval = pdMS_TO_TICKS(refresh_min * 60UL * 1000UL);
    return (xTaskGetTickCount() - last) >= interval;
}

static void codex_quota_auto_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(QUOTA_AUTO_POLL_MS));

        codex_quota_config_t cfg;
        config_snapshot(&cfg);
        if (!s_auto_network_allowed)
            continue;
        if (!config_has_api(&cfg) || cfg.refresh_min == 0)
            continue;
        if (display_mode_active() != DISPLAY_MODE_CODEX_QUOTA)
            continue;
        if (!quota_auto_refresh_due(cfg.refresh_min))
            continue;

        ESP_LOGI(TAG, "auto refresh quota page every %lum",
                 (unsigned long)cfg.refresh_min);
        esp_err_t err = codex_quota_show();
        if (err != ESP_OK)
            ESP_LOGW(TAG, "auto refresh failed: %s", esp_err_to_name(err));
    }
}

void codex_quota_set_auto_network_allowed(bool allowed)
{
    s_auto_network_allowed = allowed;
}

esp_err_t codex_quota_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
        return ESP_ERR_NO_MEM;
    memset(&s_data, 0, sizeof(s_data));
    nvs_load();
    ESP_LOGI(TAG, "init enabled=%d url_set=%d key_set=%d refresh=%lum",
             s_cfg.enabled, s_cfg.api_url[0] != '\0', s_cfg.api_key[0] != '\0',
             (unsigned long)s_cfg.refresh_min);
    if (!s_auto_task) {
        BaseType_t ok = xTaskCreate(codex_quota_auto_task, "quota_auto",
                                    QUOTA_AUTO_STACK, NULL, 3, &s_auto_task);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "auto task create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t codex_quota_get_config(codex_quota_config_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    config_snapshot(out);
    return ESP_OK;
}

esp_err_t codex_quota_set_config(const codex_quota_config_t *cfg)
{
    if (!cfg)
        return ESP_ERR_INVALID_ARG;
    codex_quota_config_t snap = *cfg;
    normalize_config(&snap);
    bool changed;
    portENTER_CRITICAL(&s_cfg_mux);
    changed = memcmp(&s_cfg, &snap, sizeof(s_cfg)) != 0;
    s_cfg = snap;
    portEXIT_CRITICAL(&s_cfg_mux);
    if (changed) {
        nvs_save_snapshot(&snap);
        ESP_LOGI(TAG, "config saved enabled=%d url_set=%d key_set=%d refresh=%lum",
                 snap.enabled, snap.api_url[0] != '\0', snap.api_key[0] != '\0',
                 (unsigned long)snap.refresh_min);
    }
    return ESP_OK;
}

void codex_quota_get_data_copy(codex_quota_data_t *out)
{
    if (!out)
        return;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = s_data;
        xSemaphoreGive(s_mutex);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

static esp_err_t codex_quota_show_inner(void)
{
    if (!epd_is_ready())
        return ESP_ERR_INVALID_STATE;

    codex_quota_config_t cfg;
    config_snapshot(&cfg);
    if (!config_has_api(&cfg)) {
        render_quota_page(&cfg, NULL, "\xe8\xaf\xb7\xe5\x85\x88\xe5\x9c\xa8\xe7\xbd\x91\xe9\xa1\xb5\xe9\x85\x8d\xe7\xbd\xae API URL \xe5\x92\x8c Key");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(90000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    s_last_fetch_tick = xTaskGetTickCount();
    char *resp = http_get_quota(&cfg);
    esp_err_t ret = ESP_OK;
    codex_quota_data_t parsed;
    if (!resp) {
        render_quota_page(&cfg, NULL, "\xe6\x8e\xa5\xe5\x8f\xa3\xe8\xaf\xb7\xe6\xb1\x82\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x8c\xe8\xaf\xb7\xe6\xa3\x80\xe6\x9f\xa5\xe7\xbd\x91\xe7\xbb\x9c/API Key");
        ret = ESP_FAIL;
        goto out;
    }

    if (!parse_quota_json(resp, &parsed, &cfg)) {
        ESP_LOGW(TAG, "quota parse failed: %.160s", resp);
        render_quota_page(&cfg, &parsed, parsed.message[0] ? parsed.message : "\xe9\xa2\x9d\xe5\xba\xa6 JSON \xe6\x9c\xaa\xe8\xaf\x86\xe5\x88\xab");
        free(resp);
        ret = ESP_FAIL;
        goto out;
    }
    free(resp);

    s_data = parsed;
    render_quota_page(&cfg, &s_data, NULL);

out:
    xSemaphoreGive(s_mutex);
    return ret;
}

typedef struct {
    TaskHandle_t waiter;
    esp_err_t    result;
} quota_show_job_t;

static void codex_quota_show_task(void *arg)
{
    quota_show_job_t *job = (quota_show_job_t *)arg;
    job->result = codex_quota_show_inner();
    xTaskNotifyGive(job->waiter);
    vTaskDelete(NULL);
}

esp_err_t codex_quota_show(void)
{
    quota_show_job_t job = {
        .waiter = xTaskGetCurrentTaskHandle(),
        .result = ESP_FAIL,
    };
    BaseType_t ok = xTaskCreatePinnedToCore(codex_quota_show_task, "quota_show",
                                            QUOTA_SHOW_STACK, &job, 4, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "quota_show task create failed; running inline");
        return codex_quota_show_inner();
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return job.result;
}
