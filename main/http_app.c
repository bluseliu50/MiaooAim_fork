#include "http_app.h"
#include "http_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "mbedtls/base64.h"
#include "nvs.h"

#include "battery_mon.h"
#include "button.h"
#include "cJSON.h"
#include "calendar_display.h"
#include "canvas_board.h"
#include "clock_display.h"
#include "codex_quota.h"
#include "countdown.h"
#include "device_identity.h"
#include "display_mode.h"
#include "display_policy.h"
#include "epd.h"
#include "fb_render.h"
#include "font_ext.h"
#include "image_convert.h"
#include "message_board.h"
#include "power_mgr.h"
#include "scheduler.h"
#include "spiffs_mount.h"
#include "time_sync.h"
#include "timetable.h"
#include "todo.h"
#include "weather.h"
#include "wifi_manager.h"

static const char *TAG = "http_app";

httpd_handle_t http_server_handle;
http_app_config_t http_app_cfg;
const char *http_images_dir = "/spiffs/images";
const int http_upload_max_bytes = 2 * 1024 * 1024;

/* backward compat aliases used within this file */
#define s_server http_server_handle
#define s_cfg http_app_cfg
#define IMAGES_DIR http_images_dir
#define UPLOAD_MAX_BYTES http_upload_max_bytes

/* ── Basic Auth ───────────────────────────────────────────────────── */

#define AUTH_NVS_NS "http_auth"
#define AUTH_NVS_USER "user"
#define AUTH_NVS_PASS "pass"
#define EPD_REPAIR_TASK_STACK 4096

static char s_auth_user[32];
static char s_auth_pass[64];
static TaskHandle_t s_epd_repair_task;

void http_auth_load_from_nvs(void) {
  s_auth_user[0] = '\0';
  s_auth_pass[0] = '\0';
  nvs_handle_t h;
  if (nvs_open(AUTH_NVS_NS, NVS_READONLY, &h) != ESP_OK)
    return;
  size_t lu = sizeof(s_auth_user), lp = sizeof(s_auth_pass);
  nvs_get_str(h, AUTH_NVS_USER, s_auth_user, &lu);
  nvs_get_str(h, AUTH_NVS_PASS, s_auth_pass, &lp);
  nvs_close(h);
}

bool http_auth_is_enabled(void) {
  return s_auth_user[0] != '\0' && s_auth_pass[0] != '\0';
}

bool http_check_basic_auth(httpd_req_t *req) {
  if (!http_auth_is_enabled())
    return true;

  char hdr[160] = {0};
  if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) !=
      ESP_OK) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"EPD\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
  }

  const char *prefix = "Basic ";
  if (strncmp(hdr, prefix, 6) != 0) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
  }

  char expected[128];
  snprintf(expected, sizeof(expected), "%s:%s", s_auth_user, s_auth_pass);
  unsigned char encoded[192];
  size_t olen = 0;
  if (mbedtls_base64_encode(encoded, sizeof(encoded) - 1, &olen,
                            (const unsigned char *)expected,
                            strlen(expected)) != 0) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "Auth internal error", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  encoded[olen] = '\0';

  if (strcmp(hdr + 6, (const char *)encoded) != 0) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"EPD\"");
    httpd_resp_send(req, "Wrong credentials", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  return true;
}

static esp_err_t auth_config_get_handler(httpd_req_t *req) {
  if (http_auth_is_enabled() && !http_check_basic_auth(req))
    return ESP_OK;

  cJSON *root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
    return ESP_OK;
  }
  cJSON_AddBoolToObject(root, "enabled", http_auth_is_enabled());
  cJSON_AddStringToObject(root, "user", s_auth_user[0] ? s_auth_user : "");
  char *str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!str) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
  free(str);
  return ESP_OK;
}

static esp_err_t auth_config_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  char buf[160] = {0};
  if (!http_read_request_body(req, buf, sizeof(buf), "请求体为空"))
    return ESP_OK;

  cJSON *root = cJSON_Parse(buf);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
    return ESP_OK;
  }

  cJSON *ju = cJSON_GetObjectItem(root, "user");
  cJSON *jp = cJSON_GetObjectItem(root, "pass");

  nvs_handle_t h;
  esp_err_t nerr = nvs_open(AUTH_NVS_NS, NVS_READWRITE, &h);
  if (nerr != ESP_OK) {
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"NVS 打开失败\"}");
    return ESP_OK;
  }

  esp_err_t op = ESP_OK;
  if (ju && cJSON_IsString(ju) && jp && cJSON_IsString(jp) &&
      strlen(ju->valuestring) > 0 && strlen(jp->valuestring) > 0) {
    char user_trunc[32], pass_trunc[64];
    strncpy(user_trunc, ju->valuestring, sizeof(user_trunc) - 1);
    user_trunc[sizeof(user_trunc) - 1] = '\0';
    strncpy(pass_trunc, jp->valuestring, sizeof(pass_trunc) - 1);
    pass_trunc[sizeof(pass_trunc) - 1] = '\0';
    op = nvs_set_str(h, AUTH_NVS_USER, user_trunc);
    if (op == ESP_OK)
      op = nvs_set_str(h, AUTH_NVS_PASS, pass_trunc);
  } else {
    (void)nvs_erase_key(h, AUTH_NVS_USER);
    (void)nvs_erase_key(h, AUTH_NVS_PASS);
  }
  if (op == ESP_OK)
    op = nvs_commit(h);
  nvs_close(h);
  cJSON_Delete(root);

  if (op != ESP_OK) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"凭据未保存\"}");
    return ESP_OK;
  }

  http_auth_load_from_nvs();

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* embedded HTML externs are in http_internal.h */

/* ── helpers ───────────────────────────────────────────────────────── */

int http_stat_size_or_neg(const char *path) {
  struct stat st;
  if (!path)
    return -1;
  if (stat(path, &st) == 0)
    return (int)st.st_size;
  return -1;
}

static void drain_request_body(httpd_req_t *req) {
  char tmp[64];
  int remain = req ? req->content_len : 0;
  while (remain > 0) {
    int to_recv = remain < (int)sizeof(tmp) ? remain : (int)sizeof(tmp);
    int r = httpd_req_recv(req, tmp, to_recv);
    if (r <= 0) {
      break;
    }
    remain -= r;
  }
}

/** EPD 未就绪时返回 JSON 503，避免 httpd 与 epd_init 并行时刷硬件 */
bool http_require_epd_ready(httpd_req_t *req) {
  if (epd_is_ready())
    return true;
  httpd_resp_set_status(req, "503 Service Unavailable");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(
      req, "{\"ok\":false,\"msg\":\"墨水屏正在初始化，请几秒后重试\"}");
  return false;
}

bool http_get_query_param(httpd_req_t *req, const char *key, char *out,
                          size_t out_len) {
  int qlen = httpd_req_get_url_query_len(req);
  if (qlen <= 0 || !out || out_len == 0)
    return false;
  char *q = (char *)malloc((size_t)qlen + 1);
  if (!q)
    return false;
  if (httpd_req_get_url_query_str(req, q, (size_t)qlen + 1) != ESP_OK) {
    free(q);
    return false;
  }
  bool ok = (httpd_query_key_value(q, key, out, out_len) == ESP_OK);
  free(q);
  return ok;
}

/* ── GET / ─────────────────────────────────────────────────────────── */

static esp_err_t index_get_handler(httpd_req_t *req) {
  return http_send_embedded_html(req, index_html_start, index_html_end);
}

/* ── GET /app.css ──────────────────────────────────────────────────── */
static esp_err_t app_css_get_handler(httpd_req_t *req) {
  return http_send_embedded_css(req);
}

static const char *http_panel_name(epd_panel_t panel) {
  switch (panel) {
  case EPD_PANEL_42_BWR:
    return "4.2 BWR SSD1619";
  case EPD_PANEL_583_BWR:
    return "5.83 BWR UC8179";
  case EPD_PANEL_WS_583_BWR:
    return "Waveshare 5.83 BWR B V2";
  case EPD_PANEL_ATC_SSD1619_16_BWR:
    return "ATC/Solum 1.6 BWR SSD1619 exp";
  case EPD_PANEL_ATC_SSD1619_22_BWR:
    return "ATC/Solum 2.2 BWR SSD1619 exp";
  case EPD_PANEL_ATC_SSD1619_26_BWR:
    return "ATC/Solum 2.6 BWR SSD1619 exp";
  case EPD_PANEL_HINK_SSD1619_29_BWR:
    return "HINK-E029A14-A1 2.9 BWR SSD1619";
  case EPD_PANEL_ATC_UC8151_29_BWR:
    return "ATC/Solum 2.9 compatible B / UC8151";
  case EPD_PANEL_ATC_UCVAR43_BWR:
    return "ATC/Solum 4.3 BWR UC variant exp";
  case EPD_PANEL_ATC_DUALSSD_585_BWR:
    return "ATC/Solum 5.85 BWR dual SSD exp";
  case EPD_PANEL_ATC_DUALSSD_585_BW:
    return "ATC/Solum 5.85 BW dual SSD exp";
  case EPD_PANEL_ATC_UC8159_60_BWR:
    return "ATC/Solum 6.0 BWR UC8159 exp";
  case EPD_PANEL_ATC_UC8179_74_BWR:
    return "ATC/Solum 7.4 BWR UC8179 exp";
  case EPD_PANEL_ATC_SSD_97_BWR:
    return "ATC/Solum 9.7 BWR SSD exp";
  case EPD_PANEL_ATC_PEGHOOK_13_BWR:
    return "ATC/Solum 1.3 peghook BWR SSD exp";
  case EPD_PANEL_REF_UC8176_42_BW:
    return "Ref UC8176 4.2 BW exp";
  case EPD_PANEL_REF_UC8176_42_BWR:
    return "Ref UC8176 4.2 BWR exp";
  case EPD_PANEL_REF_UC8179_75_BW:
    return "Ref UC8179 7.5 BW exp";
  case EPD_PANEL_REF_UC8179_75_BWR:
    return "Ref UC8179 7.5 BWR exp";
  case EPD_PANEL_REF_UC8159_583_BW:
    return "Ref UC8159 5.83 BW exp";
  case EPD_PANEL_REF_UC8159_75_LOW_BWR:
    return "Ref UC8159 7.5 low BWR exp";
  case EPD_PANEL_REF_SSD1677_75_HD_BW:
    return "Ref SSD1677 7.5 HD BW exp";
  case EPD_PANEL_REF_SSD1677_75_HD_BWR:
    return "Ref SSD1677 7.5 HD BWR exp";
  case EPD_PANEL_REF_JD79665_75_BWRY:
    return "Ref JD79665 7.5 BWRY exp";
  case EPD_PANEL_REF_SSD1619_42_BW:
    return "Ref SSD1619 4.2 BW exp";
  case EPD_PANEL_REF_SSD1619_42_BWR:
    return "Ref SSD1619 4.2 BWR exp";
  case EPD_PANEL_REF_JD79668_42_BWRY:
    return "Ref JD79668 4.2 BWRY exp";
  case EPD_PANEL_REF_SSD1683_42_BWR:
    return "Ref SSD1683 4.2 BWR exp";
  case EPD_PANEL_REF_SSD1683_42_LEGACY:
    return "Ref SSD1683 4.2 BWR exp";
  default:
    return "Unknown panel";
  }
}

/* ── GET /status ───────────────────────────────────────────────────── */

static esp_err_t status_get_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  const esp_app_desc_t *desc = esp_app_get_description();
  const esp_partition_t *running = esp_ota_get_running_partition();
  const int upload_sz = http_stat_size_or_neg(s_cfg.upload_path);
  const int raw_sz = http_stat_size_or_neg("/spiffs/image.bin");

  size_t total = 0, used = 0;
  bool spiffs_ok = spiffs_mount_is_mounted() &&
                   esp_spiffs_info("spiffs", &total, &used) == ESP_OK;

  char time_str[32] = "";
  time_sync_get_str(time_str, sizeof(time_str));

  battery_status_t bat;
  battery_mon_get_status(&bat);

  weather_config_t wx_cfg = {0};
  weather_summary_t wx_sum = {0};
  weather_get_config(&wx_cfg);
  weather_get_summary_copy(&wx_sum);

  int current_mode = button_get_current_mode();
  const char *mode_name = display_mode_name(current_mode);
  const char *mode_label = display_mode_label(current_mode);
  epd_panel_t panel = epd_get_panel();

  char esc_version[40];
  char esc_idf[64];
  char esc_running[24];
  char esc_mode_name[24];
  char esc_mode_label[32];
  char esc_panel[80];
  char esc_city[48];
  char esc_wx_text[32];
  char esc_wx_update[32];

  json_escape(esc_version, sizeof(esc_version), desc ? desc->version : "");
  json_escape(esc_idf, sizeof(esc_idf), desc ? desc->idf_ver : "");
  json_escape(esc_running, sizeof(esc_running),
              running ? running->label : "unknown");
  json_escape(esc_mode_name, sizeof(esc_mode_name), mode_name ? mode_name : "");
  json_escape(esc_mode_label, sizeof(esc_mode_label),
              mode_label ? mode_label : "");
  json_escape(esc_panel, sizeof(esc_panel), http_panel_name(panel));
  json_escape(esc_city, sizeof(esc_city),
              wx_cfg.city_name[0] ? wx_cfg.city_name : wx_cfg.location);
  json_escape(esc_wx_text, sizeof(esc_wx_text),
              wx_sum.valid ? wx_sum.now.text : "");
  json_escape(esc_wx_update, sizeof(esc_wx_update),
              wx_sum.valid ? wx_sum.update_time : "");

  bool wx_configured =
      wx_cfg.api_key[0] && wx_cfg.api_host[0] && wx_cfg.location[0];
  char json[2048];
  snprintf(
      json, sizeof(json),
      "{\"version\":\"%s\",\"idf\":\"%s\",\"build_date\":\"%s\",\"build_time\":"
      "\"%s\","
      "\"running\":\"%s\","
      "\"upload_path\":\"%s\",\"upload_bytes\":%d,"
      "\"raw_path\":\"/spiffs/image.bin\",\"raw_bytes\":%d,"
      "\"spiffs_ok\":%s,\"spiffs_total\":%u,\"spiffs_used\":%u,"
      "\"time\":\"%s\",\"time_synced\":%s,"
      "\"current_mode\":%d,\"current_mode_name\":\"%s\","
      "\"current_mode_label\":\"%s\",\"active_mode\":%d,"
      "\"panel\":%d,\"panel_name\":\"%s\",\"panel_width\":%d,"
      "\"panel_height\":%d,\"panel_has_red\":%s,\"panel_has_yellow\":%s,"
      "\"busy_idle\":%d,\"epd_ready\":%s,"
      "\"free_heap\":%u,\"min_free_heap\":%u,"
      "\"internal_free\":%u,\"psram_free\":%u,"
      "\"battery_pct\":%u,\"battery_valid\":%s,\"charging\":%s,"
      "\"battery_adc\":%d,\"battery_mv\":%d,"
      "\"weather_enabled\":%s,\"weather_configured\":%s,"
      "\"weather_city\":\"%s\",\"weather_refresh_min\":%lu,"
      "\"weather_valid\":%s,\"weather_temp\":%d,"
      "\"weather_text\":\"%s\",\"weather_update\":\"%s\"}",
      esc_version, esc_idf, desc ? desc->date : "", desc ? desc->time : "",
      esc_running, s_cfg.upload_path, upload_sz, raw_sz,
      spiffs_ok ? "true" : "false", (unsigned)total, (unsigned)used, time_str,
      time_sync_is_synced() ? "true" : "false", current_mode, esc_mode_name,
      esc_mode_label, display_mode_active(), (int)panel, esc_panel, epd_width(),
      epd_height(), epd_has_red() ? "true" : "false",
      epd_has_yellow() ? "true" : "false", epd_busy_idle_level(),
      epd_is_ready() ? "true" : "false", (unsigned)esp_get_free_heap_size(),
      (unsigned)esp_get_minimum_free_heap_size(),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
      (unsigned)bat.percent, bat.valid ? "true" : "false",
      bat.charging ? "true" : "false", bat.adc_raw, bat.voltage_mv,
      wx_cfg.enabled ? "true" : "false", wx_configured ? "true" : "false",
      esc_city, (unsigned long)wx_cfg.refresh_min,
      wx_sum.valid ? "true" : "false", wx_sum.valid ? wx_sum.now.temp : 0,
      esc_wx_text, esc_wx_update);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* gallery handlers moved to http_gallery.c */

/* ── GET /config ───────────────────────────────────────────────────── */

static esp_err_t config_get_handler(httpd_req_t *req) {
  return http_send_embedded_html(req, config_html_start, config_html_end);
}

static esp_err_t miaooaim_mark_png_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "image/png");
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
  httpd_resp_send(req, (const char *)miaooaim_mark_png_start,
                  (ssize_t)(miaooaim_mark_png_end - miaooaim_mark_png_start));
  return ESP_OK;
}

static esp_err_t gallery_page_get_handler(httpd_req_t *req) {
  return http_send_embedded_html(req, gallery_html_start, gallery_html_end);
}

static esp_err_t weather_page_get_handler(httpd_req_t *req) {
  return http_send_embedded_html(req, weather_html_start, weather_html_end);
}

static esp_err_t clock_page_get_handler(httpd_req_t *req) {
  return http_send_embedded_html(req, clock_html_start, clock_html_end);
}

static esp_err_t calendar_page_get_handler(httpd_req_t *req) {
  return http_send_embedded_html(req, calendar_html_start, calendar_html_end);
}

static esp_err_t message_page_get_handler(httpd_req_t *req) {
  return http_send_embedded_html(req, message_html_start, message_html_end);
}

static esp_err_t codex_page_get_handler(httpd_req_t *req) {
  return http_send_embedded_html(req, codex_html_start, codex_html_end);
}

/* timetable/todo/countdown UI handlers moved to http_features.c */

/* ── GET /wifi_status ──────────────────────────────────────────────── */

static esp_err_t wifi_status_get_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  cJSON *root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
    return ESP_OK;
  }

  cJSON_AddNumberToObject(root, "mode", (int)wifi_manager_get_mode());
  cJSON_AddBoolToObject(root, "sta_connected", wifi_manager_sta_connected());
  cJSON_AddStringToObject(root, "sta_ssid", wifi_manager_get_sta_ssid());
  cJSON_AddStringToObject(root, "sta_ip", wifi_manager_get_sta_ip());
  cJSON_AddStringToObject(root, "ap_ssid", wifi_manager_get_ap_ssid());
  cJSON_AddStringToObject(root, "mdns_host",
                          device_identity_get_mdns_hostname());
  cJSON_AddStringToObject(root, "id6", device_identity_get_id6_upper());
  cJSON_AddBoolToObject(root, "recovery_ap",
                        device_identity_recovery_ap_mode());

  cJSON *saved_arr = cJSON_AddArrayToObject(root, "saved_wifi");
  if (saved_arr) {
    wifi_mgr_saved_cred_t saved[WIFI_MGR_MAX_CREDENTIALS];
    int saved_count =
        wifi_manager_get_saved_credentials(saved, WIFI_MGR_MAX_CREDENTIALS);
    for (int i = 0; i < saved_count; i++) {
      cJSON_AddItemToArray(saved_arr, cJSON_CreateString(saved[i].ssid));
    }
  }

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  free(json);
  return ESP_OK;
}

/* ── GET /scan ─────────────────────────────────────────────────────── */

static esp_err_t scan_get_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  wifi_mgr_ap_record_t aps[20];
  int count = wifi_manager_scan(aps, 20);

  cJSON *root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
    return ESP_OK;
  }
  cJSON *arr = cJSON_AddArrayToObject(root, "aps");
  for (int i = 0; i < count; i++) {
    cJSON *item = cJSON_CreateObject();
    if (!item)
      continue;
    cJSON_AddStringToObject(item, "ssid", aps[i].ssid);
    cJSON_AddNumberToObject(item, "rssi", aps[i].rssi);
    cJSON_AddNumberToObject(item, "auth", aps[i].authmode);
    cJSON_AddItemToArray(arr, item);
  }
  char *str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!str) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
  free(str);
  return ESP_OK;
}

/* ── POST /wifi_connect ────────────────────────────────────────────── */

static esp_err_t wifi_connect_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  char buf[256] = {0};
  if (!http_read_request_body(req, buf, sizeof(buf), "invalid request body"))
    return ESP_OK;

  cJSON *root = cJSON_Parse(buf);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
    return ESP_OK;
  }
  const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid"));
  const char *pass =
      cJSON_GetStringValue(cJSON_GetObjectItem(root, "password"));
  if (!ssid || ssid[0] == '\0') {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 WiFi 名称");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "WiFi connect request: SSID=\"%s\"", ssid);
  esp_err_t err = wifi_manager_connect(ssid, pass ? pass : "");
  cJSON_Delete(root);

  cJSON *resp = cJSON_CreateObject();
  if (err == ESP_OK) {
    esp_err_t ts_err = time_sync_init();
    if (ts_err != ESP_OK)
      ESP_LOGW(TAG, "time sync start after WiFi connect failed: %s",
               esp_err_to_name(ts_err));
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "sta_ip", wifi_manager_get_sta_ip());
    cJSON_AddStringToObject(resp, "mdns_host",
                            device_identity_get_mdns_hostname());
    cJSON_AddStringToObject(resp, "ap_ssid", wifi_manager_get_ap_ssid());
  } else {
    cJSON_AddBoolToObject(resp, "ok", false);
    cJSON_AddStringToObject(resp, "msg", "Connection failed");
  }
  char *str = cJSON_PrintUnformatted(resp);
  cJSON_Delete(resp);
  if (!str) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
  free(str);
  return ESP_OK;
}

/* ── POST /wifi_forget ─────────────────────────────────────────────── */

static esp_err_t wifi_forget_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  /* 不直接修改 req->content_len（httpd_req_recv 内部会跟踪剩余字节）。
   * 用本地 remain 计数器消耗请求体即可，避免依赖未文档化的内部行为。 */
  char tmp[64];
  int remain = req->content_len;
  while (remain > 0) {
    int r = httpd_req_recv(
        req, tmp, remain < (int)sizeof(tmp) ? remain : (int)sizeof(tmp));
    if (r <= 0)
      break;
    remain -= r;
  }

  esp_err_t err = wifi_manager_forget();
  char json[64];
  snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* ── GET /slideshow ────────────────────────────────────────────────── */

static esp_err_t slideshow_get_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  slideshow_config_t cfg;
  scheduler_get_config(&cfg);
  const char *current = scheduler_get_current_image();
  char esc_img[80];
  json_escape(esc_img, sizeof(esc_img), current ? current : "");

  char json[320];
  snprintf(json, sizeof(json),
           "{\"enabled\":%s,\"interval_sec\":%lu,\"mode\":%d,\"clock_overlay\":"
           "%s,\"current_image\":\"%s\"}",
           cfg.enabled ? "true" : "false", (unsigned long)cfg.interval_sec,
           (int)cfg.mode, cfg.clock_overlay ? "true" : "false", esc_img);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* ── POST /slideshow ──────────────────────────────────────────────── */

static esp_err_t slideshow_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  char buf[128] = {0};
  if (!http_read_request_body(req, buf, sizeof(buf), "请求体错误"))
    return ESP_OK;

  cJSON *root = cJSON_Parse(buf);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
    return ESP_OK;
  }

  slideshow_config_t cfg;
  scheduler_get_config(&cfg);

  cJSON *j;
  if ((j = cJSON_GetObjectItem(root, "enabled")) && cJSON_IsBool(j))
    cfg.enabled = cJSON_IsTrue(j);
  if ((j = cJSON_GetObjectItem(root, "interval_sec")) && cJSON_IsNumber(j))
    cfg.interval_sec = (uint32_t)j->valuedouble;
  if ((j = cJSON_GetObjectItem(root, "mode")) && cJSON_IsNumber(j))
    cfg.mode = (slideshow_mode_t)j->valueint;
  if ((j = cJSON_GetObjectItem(root, "clock_overlay")) && cJSON_IsBool(j))
    cfg.clock_overlay = cJSON_IsTrue(j);

  cJSON_Delete(root);

  esp_err_t err = scheduler_set_config(&cfg);
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "配置无效");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

/* ── GET /version ──────────────────────────────────────────────────── */

static esp_err_t version_get_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  const esp_app_desc_t *desc = esp_app_get_description();
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

  char json[384];
  snprintf(json, sizeof(json),
           "{\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
           "\"idf\":\"%s\",\"running\":\"%s\",\"next_ota\":\"%s\"}",
           desc->version, desc->date, desc->time, desc->idf_ver,
           running ? running->label : "unknown", next ? next->label : "none");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* ── POST /ota ─────────────────────────────────────────────────────── */

static void reboot_task(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(2000));
  esp_restart();
}

static esp_err_t ota_post_handler(httpd_req_t *req) {
  if (!http_auth_is_enabled()) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_sendstr(req,
                       "{\"ok\":false,\"msg\":\"OTA requires authentication. "
                       "Set credentials in /auth_config first.\"}");
    return ESP_OK;
  }
  if (!http_check_basic_auth(req))
    return ESP_OK;
  const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
  if (!update) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"No OTA partition\"}");
    return ESP_OK;
  }

  if (req->content_len <= 0 || req->content_len > (int)update->size) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Invalid firmware size\"}");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "OTA start: %d bytes -> '%s' @ 0x%lx", req->content_len,
           update->label, (unsigned long)update->address);

  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"OTA begin failed\"}");
    return ESP_OK;
  }

  char *buf = malloc(4096);
  if (!buf) {
    esp_ota_abort(handle);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"malloc failed\"}");
    return ESP_OK;
  }

  int remaining = req->content_len;
  int received = 0;
  bool failed = false;

  while (remaining > 0) {
    int to_recv = remaining > 4096 ? 4096 : remaining;
    int r = httpd_req_recv(req, buf, to_recv);
    if (r == HTTPD_SOCK_ERR_TIMEOUT)
      continue;
    if (r <= 0) {
      ESP_LOGE(TAG, "OTA recv error at %d/%d bytes", received,
               req->content_len);
      failed = true;
      break;
    }

    err = esp_ota_write(handle, buf, r);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
      failed = true;
      break;
    }

    received += r;
    remaining -= r;
  }

  free(buf);

  if (failed) {
    esp_ota_abort(handle);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(
        req, "{\"ok\":false,\"msg\":\"Firmware upload interrupted\"}");
    return ESP_OK;
  }

  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"ok\":false,\"msg\":\"Firmware validation failed: %s\"}",
             esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  err = esp_ota_set_boot_partition(update);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req,
                       "{\"ok\":false,\"msg\":\"Set boot partition failed\"}");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "OTA success: %d bytes written to '%s', rebooting...", received,
           update->label);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req,
                     "{\"ok\":true,\"msg\":\"OTA success, rebooting...\"}");

  xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
  return ESP_OK;
}

/* ── GET /weather_config ───────────────────────────────────────────── */

static esp_err_t weather_config_get_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  weather_config_t cfg;
  weather_get_config(&cfg);
  weather_summary_t d;
  weather_get_summary_copy(&d);

  cJSON *root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
    return ESP_OK;
  }
  cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
  /* 不在 GET 中下发明文 Key，避免局域网嗅探；前端用 api_key_set +
   * 留空保存保留原密钥 */
  cJSON_AddStringToObject(root, "api_key", "");
  cJSON_AddBoolToObject(root, "api_key_set", cfg.api_key[0] != '\0');
  cJSON_AddStringToObject(root, "api_host", cfg.api_host);
  cJSON_AddStringToObject(root, "location", cfg.location);
  cJSON_AddStringToObject(root, "city_name", cfg.city_name);
  cJSON_AddNumberToObject(root, "refresh_min", cfg.refresh_min);
  cJSON_AddBoolToObject(root, "has_data", d.valid);
  if (d.valid) {
    cJSON_AddNumberToObject(root, "temp", d.now.temp);
    cJSON_AddStringToObject(root, "text", d.now.text);
    cJSON_AddStringToObject(root, "update_time", d.update_time);
  }
  char *str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!str) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
  free(str);
  return ESP_OK;
}

/* ── POST /weather_config ─────────────────────────────────────────── */

static esp_err_t weather_config_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  char buf[512] = {0};
  if (!http_read_request_body(req, buf, sizeof(buf), "请求体错误"))
    return ESP_OK;

  cJSON *root = cJSON_Parse(buf);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
    return ESP_OK;
  }

  weather_config_t cfg;
  weather_get_config(&cfg);

  cJSON *j;
  if ((j = cJSON_GetObjectItem(root, "enabled")) && cJSON_IsBool(j))
    cfg.enabled = cJSON_IsTrue(j);
  const char *s;
  cJSON *jk = cJSON_GetObjectItem(root, "api_key");
  if (jk && cJSON_IsString(jk) && jk->valuestring && jk->valuestring[0] != '\0')
    snprintf(cfg.api_key, sizeof(cfg.api_key), "%s", jk->valuestring);
  j = cJSON_GetObjectItem(root, "api_key_clear");
  if (j && cJSON_IsTrue(j))
    cfg.api_key[0] = '\0';
  if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "api_host"))))
    snprintf(cfg.api_host, sizeof(cfg.api_host), "%s", s);
  if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "location"))))
    snprintf(cfg.location, sizeof(cfg.location), "%s", s);
  if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "city_name"))))
    snprintf(cfg.city_name, sizeof(cfg.city_name), "%s", s);
  if ((j = cJSON_GetObjectItem(root, "refresh_min")) && cJSON_IsNumber(j))
    cfg.refresh_min = (uint32_t)j->valuedouble;
  cJSON_Delete(root);

  weather_set_config(&cfg);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

/* ── POST /weather_show ───────────────────────────────────────────── */

static esp_err_t weather_show_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  if (!http_require_epd_ready(req))
    return ESP_OK;

  drain_request_body(req);

  esp_err_t err = weather_fetch_and_display(true);
  if (err == ESP_OK) {
    button_set_current_mode(DISPLAY_MODE_WEATHER);
    power_mgr_save_mode(DISPLAY_MODE_WEATHER);
  }
  char json[96];
  snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* ── Codex quota config ───────────────────────────────────────────── */

static esp_err_t codex_quota_config_get_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  codex_quota_config_t cfg;
  codex_quota_data_t data;

  /* Optional ?provider=N: return that provider's saved credentials without
   * changing the active provider (lets the web UI load per-provider tokens). */
  bool use_provider = false;
  uint8_t qprovider = 0;
  char qbuf[32] = {0};
  if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
    char pval[8] = {0};
    if (httpd_query_key_value(qbuf, "provider", pval, sizeof(pval)) == ESP_OK) {
      int pv = atoi(pval);
      if (pv >= 0 && pv < QUOTA_PROVIDER_MAX) {
        use_provider = true;
        qprovider = (uint8_t)pv;
      }
    }
  }
  if (use_provider)
    codex_quota_get_provider_config(&cfg, qprovider);
  else
    codex_quota_get_config(&cfg);
  codex_quota_get_data_copy(&data);

  cJSON *root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
    return ESP_OK;
  }
  cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
  cJSON_AddNumberToObject(root, "provider", (double)cfg.provider);
  cJSON_AddStringToObject(
      root, "provider_name",
      codex_quota_provider_name((quota_provider_t)cfg.provider));
  cJSON_AddStringToObject(root, "api_url", cfg.api_url);
  cJSON_AddStringToObject(root, "api_key", "");
  cJSON_AddBoolToObject(root, "api_key_set", cfg.api_key[0] != '\0');
  cJSON_AddBoolToObject(root, "ak_id_set", cfg.access_key_id[0] != '\0');
  cJSON_AddBoolToObject(root, "ak_sk_set", cfg.secret_access_key[0] != '\0');
  cJSON_AddStringToObject(root, "unit", cfg.unit);
  cJSON_AddNumberToObject(root, "refresh_min", cfg.refresh_min);
  cJSON_AddBoolToObject(root, "has_data", data.valid);
  if (data.valid) {
    cJSON_AddNumberToObject(root, "kind", (double)data.kind);
    cJSON_AddStringToObject(root, "plan", data.plan);
    cJSON_AddStringToObject(root, "update_time", data.update_time);
    cJSON_AddStringToObject(root, "message", data.message);
    /* subscription quota: tiers[] */
    if (data.kind == 0) {
      cJSON *tiers = cJSON_AddArrayToObject(root, "tiers");
      for (int i = 0; i < data.tier_count && tiers; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", data.tiers[i].name);
        cJSON_AddNumberToObject(t, "utilization", data.tiers[i].utilization);
        cJSON_AddNumberToObject(t, "reset_ts", (double)data.tiers[i].reset_ts);
        if (data.tiers[i].have_abs) {
          cJSON_AddNumberToObject(t, "used_value", data.tiers[i].used_value);
          cJSON_AddNumberToObject(t, "max_value", data.tiers[i].max_value);
        }
        cJSON_AddItemToArray(tiers, t);
      }
    } else {
      /* balance */
      cJSON_AddNumberToObject(root, "remaining", data.remaining);
      cJSON_AddNumberToObject(root, "used", data.used);
      cJSON_AddNumberToObject(root, "total", data.total);
      cJSON_AddNumberToObject(root, "percent_used", data.percent_used);
    }
  }
  char *str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!str) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
  free(str);
  return ESP_OK;
}

static esp_err_t codex_quota_config_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  char buf[1024] = {0};
  if (!http_read_request_body(req, buf, sizeof(buf), "请求体错误"))
    return ESP_OK;

  cJSON *root = cJSON_Parse(buf);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
    return ESP_OK;
  }

  codex_quota_config_t cfg;

  /* Determine the provider the POST targets. Start from that provider's saved
   * credentials (not the active provider's) so a save for provider B doesn't
   * accidentally inherit provider A's key when the field is omitted. */
  cJSON *jp = cJSON_GetObjectItem(root, "provider");
  uint8_t post_provider;
  codex_quota_config_t cur;
  codex_quota_get_config(&cur);
  if (jp && cJSON_IsNumber(jp))
    post_provider = (uint8_t)jp->valuedouble;
  else
    post_provider = cur.provider;
  if (post_provider >= QUOTA_PROVIDER_MAX)
    post_provider = cur.provider;
  if (post_provider == cur.provider)
    cfg = cur; /* active provider: creds already loaded */
  else
    codex_quota_get_provider_config(&cfg, post_provider);

  cJSON *j;
  if ((j = cJSON_GetObjectItem(root, "enabled")) && cJSON_IsBool(j))
    cfg.enabled = cJSON_IsTrue(j);
  cfg.provider = post_provider;
  const char *s;
  if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "api_url"))))
    snprintf(cfg.api_url, sizeof(cfg.api_url), "%s", s);
  cJSON *jk = cJSON_GetObjectItem(root, "api_key");
  if (jk && cJSON_IsString(jk) && jk->valuestring && jk->valuestring[0])
    snprintf(cfg.api_key, sizeof(cfg.api_key), "%s", jk->valuestring);
  j = cJSON_GetObjectItem(root, "api_key_clear");
  if (j && cJSON_IsTrue(j))
    cfg.api_key[0] = '\0';
  /* Volcengine AK/SK */
  if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "access_key_id"))))
    snprintf(cfg.access_key_id, sizeof(cfg.access_key_id), "%s", s);
  j = cJSON_GetObjectItem(root, "ak_id_clear");
  if (j && cJSON_IsTrue(j))
    cfg.access_key_id[0] = '\0';
  if ((s = cJSON_GetStringValue(
           cJSON_GetObjectItem(root, "secret_access_key"))))
    snprintf(cfg.secret_access_key, sizeof(cfg.secret_access_key), "%s", s);
  j = cJSON_GetObjectItem(root, "ak_sk_clear");
  if (j && cJSON_IsTrue(j))
    cfg.secret_access_key[0] = '\0';
  if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "unit"))))
    snprintf(cfg.unit, sizeof(cfg.unit), "%s", s);
  if ((j = cJSON_GetObjectItem(root, "refresh_min")) && cJSON_IsNumber(j))
    cfg.refresh_min = (uint32_t)j->valuedouble;
  cJSON_Delete(root);

  codex_quota_set_config(&cfg);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

static esp_err_t codex_quota_show_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  if (!http_require_epd_ready(req))
    return ESP_OK;

  drain_request_body(req);

  esp_err_t err = codex_quota_show();
  if (err == ESP_OK) {
    button_set_current_mode(DISPLAY_MODE_CODEX_QUOTA);
    power_mgr_save_mode(DISPLAY_MODE_CODEX_QUOTA);
  }
  char json[96];
  snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* ── GET /clock_config ─────────────────────────────────────────────── */

static esp_err_t clock_config_get_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  clock_config_t cfg;
  clock_display_get_config(&cfg);

  char json[128];
  snprintf(json, sizeof(json),
           "{\"enabled\":%s,\"style\":%d,\"show_weather\":%s}",
           cfg.enabled ? "true" : "false", (int)cfg.style,
           cfg.show_weather ? "true" : "false");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* ── POST /clock_config ───────────────────────────────────────────── */

static esp_err_t clock_config_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  char buf[128] = {0};
  if (!http_read_request_body(req, buf, sizeof(buf), "请求体错误"))
    return ESP_OK;

  cJSON *root = cJSON_Parse(buf);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
    return ESP_OK;
  }

  clock_config_t cfg;
  clock_display_get_config(&cfg);

  cJSON *j;
  if ((j = cJSON_GetObjectItem(root, "enabled")) && cJSON_IsBool(j))
    cfg.enabled = cJSON_IsTrue(j);
  if ((j = cJSON_GetObjectItem(root, "style")) && cJSON_IsNumber(j))
    cfg.style = (uint8_t)j->valueint;
  if ((j = cJSON_GetObjectItem(root, "show_weather")) && cJSON_IsBool(j))
    cfg.show_weather = cJSON_IsTrue(j);
  cJSON_Delete(root);

  clock_display_set_config(&cfg);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

/* ── POST /clock_show ─────────────────────────────────────────────── */

static esp_err_t clock_show_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  if (!http_require_epd_ready(req))
    return ESP_OK;

  drain_request_body(req);

  display_policy_bump_display_epoch();
  esp_err_t err = clock_display_show();
  if (err == ESP_OK) {
    button_set_current_mode(DISPLAY_MODE_CLOCK);
    power_mgr_save_mode(DISPLAY_MODE_CLOCK);
  }
  char json[96];
  snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* ── GET /msg_config ───────────────────────────────────────────────── */

static esp_err_t msg_config_get_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  msg_config_t cfg;
  message_board_get_config(&cfg);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "text", cfg.text);
  cJSON_AddNumberToObject(root, "font_size", cfg.font_size);
  cJSON_AddNumberToObject(root, "align", (int)cfg.align);
  cJSON_AddNumberToObject(root, "color", cfg.color);
  cJSON_AddNumberToObject(root, "x_offset", cfg.x_offset);
  cJSON_AddNumberToObject(root, "y_offset", cfg.y_offset);
  char *str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!str) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
  free(str);
  return ESP_OK;
}

/* ── POST /msg_config ─────────────────────────────────────────────── */

static esp_err_t msg_config_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  char *buf = malloc(MSG_MAX_LEN + 128);
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存分配失败");
    return ESP_OK;
  }
  if (req->content_len <= 0 || req->content_len >= MSG_MAX_LEN + 128) {
    free(buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体错误");
    return ESP_OK;
  }
  if (!http_read_request_body(req, buf, MSG_MAX_LEN + 128, "请求体错误")) {
    free(buf);
    return ESP_OK;
  }

  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
    return ESP_OK;
  }

  msg_config_t cfg;
  message_board_get_config(&cfg);

  const char *s;
  if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text")))) {
    snprintf(cfg.text, sizeof(cfg.text), "%s", s);
  }
  cJSON *j;
  if ((j = cJSON_GetObjectItem(root, "font_size")) && cJSON_IsNumber(j))
    cfg.font_size = (uint8_t)j->valueint;
  if ((j = cJSON_GetObjectItem(root, "align")) && cJSON_IsNumber(j))
    cfg.align = (msg_align_t)j->valueint;
  if ((j = cJSON_GetObjectItem(root, "color")) && cJSON_IsNumber(j))
    cfg.color = (uint8_t)j->valueint;
  if ((j = cJSON_GetObjectItem(root, "x_offset")) && cJSON_IsNumber(j))
    cfg.x_offset = (int16_t)j->valueint;
  if ((j = cJSON_GetObjectItem(root, "y_offset")) && cJSON_IsNumber(j))
    cfg.y_offset = (int16_t)j->valueint;

  cJSON_Delete(root);

  message_board_set_config(&cfg);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

/* ── POST /msg_show ───────────────────────────────────────────────── */

static esp_err_t msg_show_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  if (!http_require_epd_ready(req))
    return ESP_OK;

  drain_request_body(req);

  unsigned epoch = display_policy_display_epoch();
  esp_err_t err = message_board_show_queued(&epoch);
  if (err == ESP_OK)
    message_board_wait_idle();
  if (err == ESP_OK && !display_policy_epoch_is_current(epoch))
    err = ESP_ERR_INVALID_STATE;
  char json[112];
  snprintf(json, sizeof(json), "{\"ok\":%s,\"canceled\":%s}",
           err == ESP_OK ? "true" : "false",
           err == ESP_ERR_INVALID_STATE ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* ── POST /msg_image_show ───────────────────────────────────────────
 * Receive a browser-rendered PNG/JPEG/BMP message image and display it without
 * adding it to the gallery. This keeps decorative/user fonts out of firmware
 * fontfs while preserving message-board semantics.
 */
static esp_err_t msg_image_show_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  if (req->content_len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty image");
    return ESP_OK;
  }
  if (req->content_len > http_upload_max_bytes) {
    httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "image too large");
    return ESP_OK;
  }
  if (!http_require_epd_ready(req))
    return ESP_OK;

  uint8_t *img = (uint8_t *)heap_caps_malloc(
      (size_t)req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!img)
    img = (uint8_t *)malloc((size_t)req->content_len);
  if (!img) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "image buffer alloc failed");
    return ESP_OK;
  }

  int remaining = req->content_len;
  int total = 0;
  bool ok = true;
  while (remaining > 0) {
    int got = httpd_req_recv(req, (char *)img + total, remaining);
    if (got == HTTPD_SOCK_ERR_TIMEOUT)
      continue;
    if (got <= 0) {
      ok = false;
      break;
    }
    total += got;
    remaining -= got;
  }
  if (!ok) {
    free(img);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "receive image failed");
    return ESP_OK;
  }

  bool was_manual = display_policy_manual_screen_active();
  unsigned epoch = display_policy_begin_manual_display();
  const char *raw_path = "/spiffs/image.bin";
  fb_raw_file_lock();
  esp_err_t cerr = image_convert_to_epd_raw(img, (size_t)total, raw_path);
  free(img);
  esp_err_t derr = ESP_OK;
  if (cerr == ESP_OK && display_policy_epoch_is_current(epoch)) {
    epd_request_full_refresh_next();
    fb_t *fb = fb_create();
    if (!fb) {
      derr = ESP_ERR_NO_MEM;
    } else {
      derr = fb_import(fb, raw_path);
      if (derr == ESP_OK)
        derr = epd_display_fb_free(fb);
      else
        fb_destroy(fb);
    }
  }
  fb_raw_file_unlock();

  if (cerr != ESP_OK) {
    if (!was_manual && display_policy_epoch_is_current(epoch))
      display_policy_set_manual_screen_active(false);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image convert failed");
    return ESP_OK;
  }
  if (derr != ESP_OK) {
    if (!was_manual && display_policy_epoch_is_current(epoch))
      display_policy_set_manual_screen_active(false);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "EPD display failed");
    return ESP_OK;
  }
  display_policy_set_manual_screen_active(true);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

/* calendar_show moved to http_features.c */

/* ── GET /panel_config ─────────────────────────────────────────────── */

static esp_err_t panel_config_get_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  epd_panel_t pending_panel = epd_get_panel();
  int pending_busy = epd_busy_idle_level();
  (void)epd_get_saved_panel_config(&pending_panel, &pending_busy);

  bool reboot_required = (pending_panel != epd_get_panel()) ||
                         (pending_busy != epd_busy_idle_level());
  char json[384];
  snprintf(json, sizeof(json),
           "{\"panel\":%d,\"active_panel\":%d,\"pending_panel\":%d,"
           "\"width\":%d,\"height\":%d,\"has_red\":%s,"
           "\"busy_idle\":%d,\"pending_busy_idle\":%d,"
           "\"reboot_required\":%s}",
           (int)pending_panel, (int)epd_get_panel(), (int)pending_panel,
           epd_width(), epd_height(), epd_has_red() ? "true" : "false",
           epd_busy_idle_level(), pending_busy,
           reboot_required ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* ── POST /panel_config ────────────────────────────────────────────── */

static esp_err_t panel_config_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  char buf[192] = {0};
  if (!http_read_request_body(req, buf, sizeof(buf), "invalid request body"))
    return ESP_OK;

  cJSON *root = cJSON_Parse(buf);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
    return ESP_OK;
  }

  epd_panel_t pending_panel = epd_get_panel();
  int pending_busy = epd_busy_idle_level();

  cJSON *jp = cJSON_GetObjectItem(root, "panel");
  esp_err_t cfg_err = ESP_OK;
  if (jp && cJSON_IsNumber(jp)) {
    pending_panel = (epd_panel_t)jp->valueint;
  }
  cJSON *jb = cJSON_GetObjectItem(root, "busy_idle");
  if (cfg_err == ESP_OK && jb && cJSON_IsNumber(jb)) {
    pending_busy = jb->valueint;
  } else if (jp && cJSON_IsNumber(jp)) {
    int default_busy = epd_panel_default_busy_idle(pending_panel);
    if (default_busy >= 0)
      pending_busy = default_busy;
  }
  cfg_err = epd_save_panel_config(pending_panel, pending_busy);
  cJSON_Delete(root);

  if (cfg_err != ESP_OK) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"invalid panel config\"}");
    return ESP_OK;
  }
  bool reboot_required = (pending_panel != epd_get_panel()) ||
                         (pending_busy != epd_busy_idle_level());
  char json[384];
  snprintf(
      json, sizeof(json),
      "{\"ok\":true,\"active_panel\":%d,\"pending_panel\":%d,"
      "\"width\":%d,\"height\":%d,\"busy_idle\":%d,\"pending_busy_idle\":%d,"
      "\"reboot_required\":%s,\"note\":\"panel config saved; reboot to "
      "apply\"}",
      (int)epd_get_panel(), (int)pending_panel, epd_width(), epd_height(),
      epd_busy_idle_level(), pending_busy, reboot_required ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* ── favicon (suppress 404 spam) ──────────────────────────────────── */

static esp_err_t favicon_get_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

/* ── power config handlers ─────────────────────────────────────────── */

static esp_err_t power_config_get_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  power_config_t pc;
  power_mgr_get_config(&pc);
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"enabled\":%s,\"interval\":%d,\"idle\":%d}",
           pc.enabled ? "true" : "false", pc.interval_min, pc.idle_timeout_s);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, buf);
  return ESP_OK;
}

static esp_err_t power_config_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  char body[256] = {0};
  if (!http_read_request_body(req, body, sizeof(body), "请求为空"))
    return ESP_OK;

  cJSON *root = cJSON_Parse(body);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
    return ESP_OK;
  }

  power_config_t pc;
  power_mgr_get_config(&pc);

  cJSON *j;
  j = cJSON_GetObjectItem(root, "enabled");
  if (j)
    pc.enabled = cJSON_IsTrue(j);
  j = cJSON_GetObjectItem(root, "interval");
  if (j && cJSON_IsNumber(j))
    pc.interval_min = j->valueint;
  j = cJSON_GetObjectItem(root, "idle");
  if (j && cJSON_IsNumber(j))
    pc.idle_timeout_s = j->valueint;

  cJSON_Delete(root);

  esp_err_t err = power_mgr_set_config(&pc);
  if (err != ESP_OK) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"low-power task failed\"}");
    return ESP_OK;
  }
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

/* ── display mode config handlers ─────────────────────────────────── */

static esp_err_t mode_config_get_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  int order[DISPLAY_MODE_MAX];
  int on = display_mode_get_order(order, DISPLAY_MODE_MAX);

  /* Build a compact JSON. With <=12 modes and short labels this fits well
   * within a 1KB stack buffer. */
  char buf[1024];
  int off = 0;
  off += snprintf(buf + off, sizeof(buf) - off, "{\"order\":[");
  for (int i = 0; i < on; i++)
    off +=
        snprintf(buf + off, sizeof(buf) - off, "%s%d", i ? "," : "", order[i]);
  off += snprintf(buf + off, sizeof(buf) - off, "],\"modes\":[");
  for (int i = 0; i < on; i++) {
    int idx = order[i];
    char esc[48];
    json_escape(esc, sizeof(esc), display_mode_label(idx));
    off += snprintf(buf + off, sizeof(buf) - off,
                    "%s{\"index\":%d,\"name\":\"%s\",\"label\":\"%s\","
                    "\"enabled\":%s}",
                    i ? "," : "", idx, display_mode_name(idx), esc,
                    display_mode_is_enabled(idx) ? "true" : "false");
  }
  off += snprintf(buf + off, sizeof(buf) - off, "]}");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, buf);
  return ESP_OK;
}

static esp_err_t mode_config_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  char body[512] = {0};
  if (!http_read_request_body(req, body, sizeof(body), "请求为空"))
    return ESP_OK;

  cJSON *root = cJSON_Parse(body);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
    return ESP_OK;
  }

  /* Parse the order array (optional). Each entry is a mode index. */
  int new_order[DISPLAY_MODE_MAX];
  int order_len = 0;
  cJSON *jorder = cJSON_GetObjectItem(root, "order");
  if (jorder && cJSON_IsArray(jorder)) {
    int n = cJSON_GetArraySize(jorder);
    for (int i = 0; i < n && order_len < DISPLAY_MODE_MAX; i++) {
      cJSON *e = cJSON_GetArrayItem(jorder, i);
      if (cJSON_IsNumber(e))
        new_order[order_len++] = e->valueint;
    }
  }

  /* Parse enabled map (optional). Accept either an object
   * {"0":true,...} keyed by index or an array aligned to the order array. */
  cJSON *jenabled = cJSON_GetObjectItem(root, "enabled");

  /* Apply order first. */
  if (order_len > 0)
    display_mode_set_order(new_order, order_len);

  /* Re-read the effective order so enabled flags map correctly. */
  int order[DISPLAY_MODE_MAX];
  int on = display_mode_get_order(order, DISPLAY_MODE_MAX);

  if (jenabled && cJSON_IsArray(jenabled)) {
    int n = cJSON_GetArraySize(jenabled);
    for (int i = 0; i < n && i < on; i++) {
      cJSON *e = cJSON_GetArrayItem(jenabled, i);
      if (cJSON_IsBool(e))
        display_mode_set_enabled(order[i], cJSON_IsTrue(e));
    }
  } else if (jenabled && cJSON_IsObject(jenabled)) {
    cJSON *e = NULL;
    cJSON_ArrayForEach(e, jenabled) {
      int idx = atoi(e->string);
      if (idx >= 0 && idx < on)
        display_mode_set_enabled(idx, cJSON_IsTrue(e));
    }
  }

  /* Ensure at least one mode stays enabled; otherwise re-enable the first. */
  int any_enabled = 0;
  for (int i = 0; i < on; i++)
    if (display_mode_is_enabled(order[i])) {
      any_enabled = 1;
      break;
    }
  if (!any_enabled && on > 0)
    display_mode_set_enabled(order[0], true);

  cJSON_Delete(root);

  display_mode_prefs_save();

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

/* countdown handlers moved to http_features.c */

/* ── session open hook (reset inactivity timer) ───────────────────── */

static esp_err_t http_session_open_hook(httpd_handle_t hd, int sockfd) {
  (void)hd;
  (void)sockfd;
  power_mgr_reset_activity();
  return ESP_OK;
}

/* ── server start ──────────────────────────────────────────────────── */

static esp_err_t epd_test_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  if (!http_require_epd_ready(req))
    return ESP_OK;

  display_policy_begin_manual_display();
  esp_err_t err = epd_display_test_pattern();
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "EPD test failed");
    return ESP_OK;
  }
  display_policy_set_manual_screen_active(true);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

static esp_err_t font_test_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  if (!http_require_epd_ready(req))
    return ESP_OK;

  font_ext_refresh();
  font_ext_info_t fonts[FONT_EXT_COUNT] = {0};
  font_ext_get_info(fonts);

  fb_t *fb = fb_create();
  if (!fb) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "framebuffer alloc failed");
    return ESP_OK;
  }
  fb_clear(fb);

  const int W = fb->width;
  const int H = fb->height;
  const int m = (W >= 360) ? 14 : 8;
  int y = (H >= 300) ? 12 : 6;

  fb_rect(fb, 0, 0, W, H, COLOR_BLACK);
  fb_utf8_px(fb, m, y, "字库测试", COLOR_BLACK, (W >= 360) ? 30 : 24);
  y += (W >= 360) ? 31 : 26;

  char line[128];
  snprintf(line, sizeof(line), "16:%s %luK 24:%s %luK 32:%s %luK",
           fonts[0].present ? "OK" : "--",
           fonts[0].present ? (unsigned long)(fonts[0].file_size / 1024) : 0UL,
           fonts[1].present ? "OK" : "--",
           fonts[1].present ? (unsigned long)(fonts[1].file_size / 1024) : 0UL,
           fonts[2].present ? "OK" : "--",
           fonts[2].present ? (unsigned long)(fonts[2].file_size / 1024) : 0UL);
  fb_utf8_px_maxw(fb, m, y, line, COLOR_BLACK, 16, W - 2 * m);
  y += 18;

  fb_hline(fb, m, y, W - 2 * m, COLOR_BLACK);
  y += 7;

  const int label_w = (W >= 360) ? 54 : 42;
  const int sample_x = m + label_w;
  const int sample_w = W - sample_x - m;

  fb_utf8_px(fb, m, y + 4, "24px", COLOR_BLACK, 16);
  fb_utf8_px_maxw(fb, sample_x, y, "日历 天气 待办", COLOR_BLACK, 24, sample_w);
  y += 28;
  fb_utf8_px(fb, m, y + 4, "24px", COLOR_BLACK, 16);
  fb_utf8_px_maxw(fb, sample_x, y, "0123456789 23:59", COLOR_BLACK, 24,
                  sample_w);
  y += 28;
  fb_utf8_px(fb, m, y + 4, "24px", COLOR_BLACK, 16);
  fb_utf8_px_maxw(fb, sample_x, y, "℃ ￥ ± % / () !?", COLOR_BLACK, 24,
                  sample_w);
  y += 32;

  fb_hline(fb, m, y - 5, W - 2 * m, COLOR_BLACK);
  fb_utf8_px(fb, m, y + 7, "32px", COLOR_BLACK, 16);
  fb_utf8_px_maxw(fb, sample_x, y, "2026-06-21", COLOR_BLACK, 32, sample_w);
  y += 38;
  fb_utf8_px(fb, m, y + 7, "32px", COLOR_BLACK, 16);
  fb_utf8_px_maxw(fb, sample_x, y, "25℃ 88% ￥9.9", COLOR_BLACK, 32, sample_w);
  y += 40;

  fb_hline(fb, m, y - 5, W - 2 * m, COLOR_BLACK);
  if (H - y > 34) {
    fb_utf8_px(fb, m, y + 7, "32px", COLOR_BLACK, 16);
    fb_utf8_px_maxw(fb, sample_x, y, "12:34 98%", COLOR_BLACK, 32, sample_w);
  }

  display_policy_begin_manual_display();
  esp_err_t err = epd_display_fb_free(fb);
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "font test failed");
    return ESP_OK;
  }
  display_policy_set_manual_screen_active(true);

  char json[192];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"font16\":%s,\"font24\":%s,\"font32\":%s}",
           fonts[0].present ? "true" : "false",
           fonts[1].present ? "true" : "false",
           fonts[2].present ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static void epd_repair_task(void *arg) {
  int packed = (int)(intptr_t)arg;
  int cycles = packed & 0xFF;
  int pattern = (packed >> 8) & 0xFF;
  esp_err_t err = epd_repair_ghosting(cycles, pattern);
  if (err == ESP_OK)
    display_policy_set_manual_screen_active(true);
  else
    ESP_LOGW(TAG, "EPD ghost repair failed: %s", esp_err_to_name(err));
  s_epd_repair_task = NULL;
  vTaskDelete(NULL);
}

static esp_err_t epd_repair_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  if (!http_require_epd_ready(req))
    return ESP_OK;

  int cycles = 1;
  int pattern = 0;
  if (req->content_len > 0) {
    char buf[128] = {0};
    if (!http_read_request_body(req, buf, sizeof(buf), "invalid request body"))
      return ESP_OK;
    cJSON *root = cJSON_Parse(buf);
    if (root) {
      cJSON *jc = cJSON_GetObjectItem(root, "cycles");
      if (jc && cJSON_IsNumber(jc))
        cycles = jc->valueint;
      cJSON *jp = cJSON_GetObjectItem(root, "pattern");
      if (jp && cJSON_IsNumber(jp))
        pattern = jp->valueint;
      cJSON_Delete(root);
    }
  }
  if (cycles < 1)
    cycles = 1;
  if (cycles > 8)
    cycles = 8;
  if (pattern < 0 || pattern > 2)
    pattern = 0;

  if (s_epd_repair_task) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(
        req, "{\"ok\":false,\"busy\":true,\"msg\":\"repair already running\"}");
    return ESP_OK;
  }

  display_policy_begin_manual_display();
  int packed = (cycles & 0xFF) | ((pattern & 0xFF) << 8);
  BaseType_t ok =
      xTaskCreate(epd_repair_task, "epd_repair", EPD_REPAIR_TASK_STACK,
                  (void *)(intptr_t)packed, 4, &s_epd_repair_task);
  if (ok != pdPASS) {
    s_epd_repair_task = NULL;
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(
        req, "{\"ok\":false,\"busy\":false,\"msg\":\"task create failed\"}");
    return ESP_OK;
  }

  char json[96];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"busy\":true,\"cycles\":%d,\"pattern\":%d}", cycles,
           pattern);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static void send_spiffs_action_result(httpd_req_t *req, esp_err_t err,
                                      bool reboot_required) {
  char json[160];
  snprintf(json, sizeof(json),
           "{\"ok\":%s,\"spiffs_ok\":%s,\"err\":\"%s\",\"reboot_required\":%s}",
           err == ESP_OK ? "true" : "false",
           spiffs_mount_is_mounted() ? "true" : "false",
           err == ESP_OK ? "ESP_OK" : esp_err_to_name(err),
           reboot_required ? "true" : "false");
  if (err != ESP_OK)
    httpd_resp_set_status(req, "500 Internal Server Error");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t spiffs_remount_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;
  drain_request_body(req);

  esp_err_t err = spiffs_mount_retry();
  send_spiffs_action_result(req, err, err == ESP_OK);
  return ESP_OK;
}

static esp_err_t spiffs_format_post_handler(httpd_req_t *req) {
  if (!http_check_basic_auth(req))
    return ESP_OK;

  char buf[128] = {0};
  if (!http_read_request_body(req, buf, sizeof(buf), "invalid request body"))
    return ESP_OK;

  cJSON *root = cJSON_Parse(buf);
  const char *confirm =
      root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "confirm")) : NULL;
  char confirm_norm[32] = {0};
  if (confirm) {
    size_t start = 0;
    size_t len = strlen(confirm);
    while (start < len && (unsigned char)confirm[start] <= ' ')
      start++;
    while (len > start && (unsigned char)confirm[len - 1] <= ' ')
      len--;
    size_t out_len = len - start;
    if (out_len >= sizeof(confirm_norm))
      out_len = sizeof(confirm_norm) - 1;
    for (size_t i = 0; i < out_len; i++) {
      char ch = confirm[start + i];
      confirm_norm[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    }
  }
  if (strcmp(confirm_norm, "FORMAT_SPIFFS") != 0) {
    if (root)
      cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing confirm");
    return ESP_OK;
  }
  cJSON_Delete(root);

  esp_err_t err = spiffs_mount_format_and_mount();
  send_spiffs_action_result(req, err, err == ESP_OK);
  if (err == ESP_OK)
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
  return ESP_OK;
}

esp_err_t http_app_start(const http_app_config_t *cfg) {
  if (!cfg || !cfg->mount_path || !cfg->upload_path)
    return ESP_ERR_INVALID_ARG;
  s_cfg = *cfg;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.recv_wait_timeout = 8;
  config.send_wait_timeout = 15;
  config.max_uri_handlers = 80;
  config.stack_size = 16384;
  config.lru_purge_enable = true;
  config.max_open_sockets = 7;
  config.open_fn = http_session_open_hook;

  http_auth_load_from_nvs();

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK)
    return err;

  const httpd_uri_t uris[] = {
      /* core pages */
      {"/", HTTP_GET, index_get_handler, NULL},
      {"/app.css", HTTP_GET, app_css_get_handler, NULL},
      {"/config", HTTP_GET, config_get_handler, NULL},
      {"/gallery", HTTP_GET, gallery_page_get_handler, NULL},
      {"/weather", HTTP_GET, weather_page_get_handler, NULL},
      {"/clock", HTTP_GET, clock_page_get_handler, NULL},
      {"/calendar", HTTP_GET, calendar_page_get_handler, NULL},
      {"/message", HTTP_GET, message_page_get_handler, NULL},
      {"/codex", HTTP_GET, codex_page_get_handler, NULL},
      {"/favicon.ico", HTTP_GET, favicon_get_handler, NULL},
      {"/miaooaim-mark.png", HTTP_GET, miaooaim_mark_png_get_handler, NULL},
      {"/status", HTTP_GET, status_get_handler, NULL},
      /* gallery (http_gallery.c) */
      {"/upload", HTTP_POST, gallery_upload_post_handler, NULL},
      {"/images", HTTP_GET, gallery_images_get_handler, NULL},
      {"/image", HTTP_GET, gallery_image_get_handler, NULL},
      {"/show", HTTP_POST, gallery_show_post_handler, NULL},
      {"/delete_image", HTTP_POST, gallery_delete_image_post_handler, NULL},
      {"/delete", HTTP_POST, gallery_delete_post_handler, NULL},
      /* wifi & panel config (http_app.c) */
      {"/wifi_status", HTTP_GET, wifi_status_get_handler, NULL},
      {"/scan", HTTP_GET, scan_get_handler, NULL},
      {"/wifi_connect", HTTP_POST, wifi_connect_post_handler, NULL},
      {"/wifi_forget", HTTP_POST, wifi_forget_post_handler, NULL},
      {"/panel_config", HTTP_GET, panel_config_get_handler, NULL},
      {"/panel_config", HTTP_POST, panel_config_post_handler, NULL},
      /* slideshow, weather, clock, msg config (http_app.c) */
      {"/slideshow", HTTP_GET, slideshow_get_handler, NULL},
      {"/slideshow", HTTP_POST, slideshow_post_handler, NULL},
      {"/weather_config", HTTP_GET, weather_config_get_handler, NULL},
      {"/weather_config", HTTP_POST, weather_config_post_handler, NULL},
      {"/weather_show", HTTP_POST, weather_show_post_handler, NULL},
      {"/codex_quota_config", HTTP_GET, codex_quota_config_get_handler, NULL},
      {"/codex_quota_config", HTTP_POST, codex_quota_config_post_handler, NULL},
      {"/codex_quota_show", HTTP_POST, codex_quota_show_post_handler, NULL},
      {"/clock_config", HTTP_GET, clock_config_get_handler, NULL},
      {"/clock_config", HTTP_POST, clock_config_post_handler, NULL},
      {"/clock_show", HTTP_POST, clock_show_post_handler, NULL},
      {"/msg_config", HTTP_GET, msg_config_get_handler, NULL},
      {"/msg_config", HTTP_POST, msg_config_post_handler, NULL},
      {"/msg_show", HTTP_POST, msg_show_post_handler, NULL},
      {"/msg_image_show", HTTP_POST, msg_image_show_post_handler, NULL},
      /* canvas board (http_canvas.c) */
      {"/board", HTTP_GET, canvas_board_ui_get, NULL},
      {"/canvas_layout", HTTP_GET, canvas_layout_get, NULL},
      {"/canvas_layout", HTTP_POST, canvas_layout_post, NULL},
      {"/canvas_show", HTTP_POST, canvas_show_post, NULL},
      {"/canvas_icons", HTTP_GET, canvas_icons_get, NULL},
      {"/canvas_icon_upload", HTTP_POST, canvas_icon_upload_post, NULL},
      {"/canvas_icon_delete", HTTP_POST, canvas_icon_delete_post, NULL},
      {"/canvas_image_list", HTTP_GET, canvas_image_list_get, NULL},
      {"/canvas_image", HTTP_GET, canvas_image_get, NULL},
      {"/canvas_image_upload", HTTP_POST, canvas_image_upload_post, NULL},
      {"/canvas_image_delete", HTTP_POST, canvas_image_delete_post, NULL},
      /* features (http_features.c) */
      {"/calendar_show", HTTP_POST, feat_calendar_show_post, NULL},
      {"/timetable", HTTP_GET, feat_timetable_ui_get, NULL},
      {"/timetable.json", HTTP_GET, feat_timetable_json_get, NULL},
      {"/timetable", HTTP_POST, feat_timetable_post, NULL},
      {"/timetable_show", HTTP_POST, feat_timetable_show_post, NULL},
      {"/todo", HTTP_GET, feat_todo_ui_get, NULL},
      {"/todo.json", HTTP_GET, feat_todo_json_get, NULL},
      {"/todo", HTTP_POST, feat_todo_post, NULL},
      {"/todo_show", HTTP_POST, feat_todo_show_post, NULL},
      {"/countdown", HTTP_GET, feat_countdown_ui_get, NULL},
      {"/countdown_config", HTTP_GET, feat_countdown_config_get, NULL},
      {"/countdown_config", HTTP_POST, feat_countdown_config_post, NULL},
      {"/countdown_show", HTTP_POST, feat_countdown_show_post, NULL},
      /* system */
      {"/version", HTTP_GET, version_get_handler, NULL},
      {"/epd_test", HTTP_POST, epd_test_post_handler, NULL},
      {"/font_test", HTTP_POST, font_test_post_handler, NULL},
      {"/epd_repair", HTTP_POST, epd_repair_post_handler, NULL},
      {"/spiffs_remount", HTTP_POST, spiffs_remount_post_handler, NULL},
      {"/spiffs_format", HTTP_POST, spiffs_format_post_handler, NULL},
      {"/ota", HTTP_POST, ota_post_handler, NULL},
      {"/auth_config", HTTP_GET, auth_config_get_handler, NULL},
      {"/auth_config", HTTP_POST, auth_config_post_handler, NULL},
      {"/power_config", HTTP_GET, power_config_get_handler, NULL},
      {"/power_config", HTTP_POST, power_config_post_handler, NULL},
      /* display mode enable/order config */
      {"/mode_config", HTTP_GET, mode_config_get_handler, NULL},
      {"/mode_config", HTTP_POST, mode_config_post_handler, NULL},
  };
  int registered = 0;
  for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
    esp_err_t rerr = httpd_register_uri_handler(s_server, &uris[i]);
    if (rerr == ESP_OK) {
      registered++;
    } else {
      ESP_LOGE(TAG, "Failed to register %s (%d): %s", uris[i].uri,
               uris[i].method, esp_err_to_name(rerr));
    }
  }

  ESP_LOGI(TAG, "HTTP server started (%d/%d routes)", registered,
           (int)(sizeof(uris) / sizeof(uris[0])));
  return ESP_OK;
}
