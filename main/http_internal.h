#pragma once

/**
 * Internal shared declarations for the HTTP subsystem.
 * Used by http_app.c, http_gallery.c, http_config.c, http_features.c,
 * http_system.c.
 */

#include "esp_http_server.h"
#include "http_app.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/** Escape a string for safe embedding in a JSON value.
 *  Writes at most dst_len-1 chars + NUL.  Returns dst. */
static inline char *json_escape(char *dst, size_t dst_len, const char *src) {
  if (!dst || dst_len == 0)
    return dst;
  size_t o = 0;
  if (!src)
    src = "";
  for (const char *s = src; *s && o + 2 < dst_len; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x20) {
      if (o + 6 >= dst_len)
        break;
      static const char hex[] = "0123456789abcdef";
      dst[o++] = '\\';
      dst[o++] = 'u';
      dst[o++] = '0';
      dst[o++] = '0';
      dst[o++] = hex[c >> 4];
      dst[o++] = hex[c & 0x0f];
      continue;
    }
    if (*s == '"' || *s == '\\') {
      dst[o++] = '\\';
    }
    dst[o++] = *s;
  }
  dst[o] = '\0';
  return dst;
}

#define HTTP_HTML_CACHE_CONTROL "private, max-age=300"

static inline esp_err_t http_send_embedded_html(httpd_req_t *req,
                                                const uint8_t *start,
                                                const uint8_t *end) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", HTTP_HTML_CACHE_CONTROL);
  return httpd_resp_send(req, (const char *)start, (ssize_t)(end - start));
}

static inline bool http_read_request_body(httpd_req_t *req, char *buf,
                                          size_t buf_len, const char *bad_msg) {
  if (!req || !buf || buf_len == 0)
    return false;

  int len = req->content_len;
  if (len <= 0 || len >= (int)buf_len) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        bad_msg ? bad_msg : "invalid request body");
    return false;
  }

  int total = 0;
  while (total < len) {
    int r = httpd_req_recv(req, buf + total, len - total);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT)
        continue;
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败");
      return false;
    }
    total += r;
  }
  buf[total] = '\0';
  return true;
}

extern httpd_handle_t http_server_handle;
extern http_app_config_t http_app_cfg;
extern const char *http_images_dir;
extern const int http_upload_max_bytes;

bool http_check_basic_auth(httpd_req_t *req);
bool http_auth_is_enabled(void);
void http_auth_load_from_nvs(void);
bool http_require_epd_ready(httpd_req_t *req);
int http_stat_size_or_neg(const char *path);
bool http_get_query_param(httpd_req_t *req, const char *key, char *out,
                          size_t out_len);

/* embedded HTML blobs */
/* shared app shell stylesheet */
extern const uint8_t app_css_start[] asm("_binary_app_css_start");
extern const uint8_t app_css_end[] asm("_binary_app_css_end");

static inline esp_err_t http_send_embedded_css(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/css; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", HTTP_HTML_CACHE_CONTROL);
  return httpd_resp_send(req, (const char *)app_css_start,
                         (ssize_t)(app_css_end - app_css_start));
}

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t config_html_start[] asm("_binary_config_html_start");
extern const uint8_t config_html_end[] asm("_binary_config_html_end");
extern const uint8_t gallery_html_start[] asm("_binary_gallery_html_start");
extern const uint8_t gallery_html_end[] asm("_binary_gallery_html_end");
extern const uint8_t weather_html_start[] asm("_binary_weather_html_start");
extern const uint8_t weather_html_end[] asm("_binary_weather_html_end");
extern const uint8_t clock_html_start[] asm("_binary_clock_html_start");
extern const uint8_t clock_html_end[] asm("_binary_clock_html_end");
extern const uint8_t calendar_html_start[] asm("_binary_calendar_html_start");
extern const uint8_t calendar_html_end[] asm("_binary_calendar_html_end");
extern const uint8_t message_html_start[] asm("_binary_message_html_start");
extern const uint8_t message_html_end[] asm("_binary_message_html_end");
extern const uint8_t codex_html_start[] asm("_binary_codex_html_start");
extern const uint8_t codex_html_end[] asm("_binary_codex_html_end");
extern const uint8_t timetable_html_start[] asm("_binary_timetable_html_start");
extern const uint8_t timetable_html_end[] asm("_binary_timetable_html_end");
extern const uint8_t todo_html_start[] asm("_binary_todo_html_start");
extern const uint8_t todo_html_end[] asm("_binary_todo_html_end");
extern const uint8_t countdown_html_start[] asm("_binary_countdown_html_start");
extern const uint8_t countdown_html_end[] asm("_binary_countdown_html_end");
extern const uint8_t
    miaooaim_mark_png_start[] asm("_binary_miaooaim_mark_png_start");
extern const uint8_t
    miaooaim_mark_png_end[] asm("_binary_miaooaim_mark_png_end");

/* ── gallery handlers (http_gallery.c) ── */
esp_err_t gallery_upload_post_handler(httpd_req_t *req);
esp_err_t gallery_show_post_handler(httpd_req_t *req);
esp_err_t gallery_delete_image_post_handler(httpd_req_t *req);
esp_err_t gallery_delete_post_handler(httpd_req_t *req);
esp_err_t gallery_images_get_handler(httpd_req_t *req);
esp_err_t gallery_image_get_handler(httpd_req_t *req);

/* ── canvas board HTML blob ── */
extern const uint8_t board_html_start[] asm("_binary_board_html_start");
extern const uint8_t board_html_end[] asm("_binary_board_html_end");

/* ── canvas board handlers (http_canvas.c) ── */
esp_err_t canvas_board_ui_get(httpd_req_t *req);
esp_err_t canvas_layout_get(httpd_req_t *req);
esp_err_t canvas_layout_post(httpd_req_t *req);
esp_err_t canvas_show_post(httpd_req_t *req);
esp_err_t canvas_icons_get(httpd_req_t *req);
esp_err_t canvas_icon_upload_post(httpd_req_t *req);
esp_err_t canvas_icon_delete_post(httpd_req_t *req);
esp_err_t canvas_image_list_get(httpd_req_t *req);
esp_err_t canvas_image_get(httpd_req_t *req);
esp_err_t canvas_image_upload_post(httpd_req_t *req);
esp_err_t canvas_image_delete_post(httpd_req_t *req);

/* ── feature handlers (http_features.c) ── */
esp_err_t feat_timetable_ui_get(httpd_req_t *req);
esp_err_t feat_timetable_json_get(httpd_req_t *req);
esp_err_t feat_timetable_post(httpd_req_t *req);
esp_err_t feat_timetable_show_post(httpd_req_t *req);
esp_err_t feat_todo_ui_get(httpd_req_t *req);
esp_err_t feat_todo_json_get(httpd_req_t *req);
esp_err_t feat_todo_post(httpd_req_t *req);
esp_err_t feat_todo_show_post(httpd_req_t *req);
esp_err_t feat_countdown_ui_get(httpd_req_t *req);
esp_err_t feat_countdown_config_get(httpd_req_t *req);
esp_err_t feat_countdown_config_post(httpd_req_t *req);
esp_err_t feat_countdown_show_post(httpd_req_t *req);
esp_err_t feat_calendar_show_post(httpd_req_t *req);
