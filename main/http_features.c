#include "http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "cJSON.h"
#include "timetable.h"
#include "todo.h"
#include "countdown.h"
#include "calendar_display.h"
#include "button.h"
#include "display_mode.h"
#include "display_policy.h"
#include "power_mgr.h"

__attribute__((unused)) static const char *TAG = "http_feat";

/* ── GET /timetable (HTML UI) ─────────────────────────────────────── */

esp_err_t feat_timetable_ui_get(httpd_req_t *req)
{
    return http_send_embedded_html(req, timetable_html_start, timetable_html_end);
}

/* ── GET /timetable.json / POST /timetable / POST /timetable_show ─────
 *  实现位于 timetable.c，此处提供 feat_* 入口供路由注册。 */

esp_err_t feat_timetable_json_get(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    return timetable_http_get_handler(req);
}

esp_err_t feat_timetable_post(httpd_req_t *req)
{
    return timetable_http_post_handler(req);
}

esp_err_t feat_timetable_show_post(httpd_req_t *req)
{
    return timetable_show_http_handler(req);
}

/* ── GET /todo (HTML UI) ──────────────────────────────────────────── */

esp_err_t feat_todo_ui_get(httpd_req_t *req)
{
    return http_send_embedded_html(req, todo_html_start, todo_html_end);
}

/* ── GET /todo.json / POST /todo / POST /todo_show ────────────────────
 *  实现位于 todo.c */

esp_err_t feat_todo_json_get(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    return todo_http_get_handler(req);
}

esp_err_t feat_todo_post(httpd_req_t *req)
{
    return todo_http_post_handler(req);
}

esp_err_t feat_todo_show_post(httpd_req_t *req)
{
    return todo_show_http_handler(req);
}

/* ── POST /calendar_show ──────────────────────────────────────────── */

esp_err_t feat_calendar_show_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (!http_require_epd_ready(req))
        return ESP_OK;

    int year = 0, month = 0;
    if (req->content_len > 0) {
        char buf[128];
        if (http_read_request_body(req, buf, sizeof(buf), "请求体错误")) {
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *jy = cJSON_GetObjectItem(root, "year");
                cJSON *jm = cJSON_GetObjectItem(root, "month");
                if (jy && cJSON_IsNumber(jy)) year  = jy->valueint;
                if (jm && cJSON_IsNumber(jm)) month = jm->valueint;
                cJSON_Delete(root);
            }
        } else {
            return ESP_OK;
        }
    }

    unsigned epoch = display_policy_begin_manual_display();
    esp_err_t err = calendar_display_show(year, month);
    if (err == ESP_OK)
        calendar_display_wait_render_idle();
    if (err == ESP_OK) {
        if (display_policy_epoch_is_current(epoch)) {
            button_set_current_mode(DISPLAY_MODE_CALENDAR);
            power_mgr_save_mode(DISPLAY_MODE_CALENDAR);
        } else {
            err = ESP_ERR_INVALID_STATE;
        }
    }
    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":%s,\"canceled\":%s}",
             err == ESP_OK ? "true" : "false",
             err == ESP_ERR_INVALID_STATE ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── countdown ────────────────────────────────────────────────────── */

esp_err_t feat_countdown_ui_get(httpd_req_t *req)
{
    return http_send_embedded_html(req, countdown_html_start, countdown_html_end);
}

esp_err_t feat_countdown_config_get(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    countdown_config_t cfg;
    if (countdown_get_config(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config unavailable");
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
    cJSON_AddNumberToObject(root, "count", cfg.count);
    cJSON *arr = cJSON_AddArrayToObject(root, "items");
    if (arr) {
        for (int i = 0; i < cfg.count && i < COUNTDOWN_MAX_ITEMS; i++) {
            cJSON *item = cJSON_CreateObject();
            if (!item) continue;
            cJSON_AddStringToObject(item, "title", cfg.items[i].title);
            cJSON_AddNumberToObject(item, "year", cfg.items[i].year);
            cJSON_AddNumberToObject(item, "month", cfg.items[i].month);
            cJSON_AddNumberToObject(item, "day", cfg.items[i].day);
            cJSON_AddBoolToObject(item, "active", cfg.items[i].active);
            cJSON_AddItemToArray(arr, item);
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

esp_err_t feat_countdown_config_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求长度错误");
        return ESP_OK;
    }
    char *buf = malloc((size_t)total + 1);
    if (!buf) { httpd_resp_send_500(req); return ESP_OK; }

    int received = 0;
    while (received < total) {
        int got = httpd_req_recv(req, buf + received, (size_t)(total - received));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) { free(buf); return ESP_FAIL; }
        received += got;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误"); return ESP_OK; }

    countdown_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cJSON *j = cJSON_GetObjectItem(root, "enabled");
    cfg.enabled = j && cJSON_IsTrue(j);

    cJSON *arr = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > COUNTDOWN_MAX_ITEMS) n = COUNTDOWN_MAX_ITEMS;
        cfg.count = (uint8_t)n;
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(arr, i);
            if (!item) continue;
            cJSON *t = cJSON_GetObjectItem(item, "title");
            if (cJSON_IsString(t) && t->valuestring) {
                strncpy(cfg.items[i].title, t->valuestring, COUNTDOWN_TITLE_LEN - 1);
                cfg.items[i].title[COUNTDOWN_TITLE_LEN - 1] = '\0';
            }
            cJSON *y = cJSON_GetObjectItem(item, "year");
            if (cJSON_IsNumber(y)) cfg.items[i].year = (uint16_t)y->valueint;
            cJSON *m = cJSON_GetObjectItem(item, "month");
            if (cJSON_IsNumber(m)) cfg.items[i].month = (uint8_t)m->valueint;
            cJSON *d = cJSON_GetObjectItem(item, "day");
            if (cJSON_IsNumber(d)) cfg.items[i].day = (uint8_t)d->valueint;
            cJSON *a = cJSON_GetObjectItem(item, "active");
            cfg.items[i].active = a && cJSON_IsTrue(a);
        }
    }
    cJSON_Delete(root);

    countdown_set_config(&cfg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t feat_countdown_show_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (!http_require_epd_ready(req)) return ESP_OK;
    display_policy_begin_manual_display();
    esp_err_t err = countdown_show();
    if (err == ESP_OK) {
        button_set_current_mode(DISPLAY_MODE_COUNTDOWN);
        power_mgr_save_mode(DISPLAY_MODE_COUNTDOWN);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}
