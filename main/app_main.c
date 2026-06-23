#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery_mon.h"
#include "button.h"
#include "calendar_display.h"
#include "canvas_board.h"
#include "clock_display.h"
#include "codex_quota.h"
#include "countdown.h"
#include "device_identity.h"
#include "display_mode.h"
#include "display_policy.h"
#include "epd.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "fb_render.h"
#include "font_ext.h"
#include "http_app.h"
#include "mdns.h"
#include "message_board.h"
#include "nvs_utils.h"
#include "power_mgr.h"
#include "scheduler.h"
#include "spiffs_mount.h"
#include "time_sync.h"
#include "timetable.h"
#include "todo.h"
#include "ui_theme.h"
#include "weather.h"
#include "wifi_manager.h"

static const char *TAG = "app";

static int normalize_saved_mode(int saved_mode, int mode_count,
                                bool *write_back) {
  if (write_back)
    *write_back = false;

  if (saved_mode < 0 || saved_mode >= mode_count) {
    if (write_back)
      *write_back = true;
    return DISPLAY_MODE_CLOCK;
  }

  if (saved_mode == DISPLAY_MODE_SLIDESHOW &&
      !power_mgr_saved_mode_has_marker()) {
    ESP_LOGW(TAG, "Saved slideshow mode has no v2 marker; treating it as "
                  "legacy stale state");
    if (write_back)
      *write_back = true;
    return DISPLAY_MODE_CLOCK;
  }

  return saved_mode;
}

static void render_welcome_screen(void) {
  fb_t *fb = fb_create();
  if (!fb) {
    ESP_LOGE(TAG, "FB alloc failed");
    return;
  }
  fb_clear(fb);

  int W = fb->width;
  int H = fb->height;
  int sc = ui_scale_for(fb);
  int body_sc = 1;
  int ln = 16 * body_sc;
  int gap = 4 * sc;
  int pad = 18 * sc;

  ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
  ui_draw_header(fb,
                 "\xe7\x94\xb5\xe5\xad\x90\xe5\xa2\xa8\xe6\xb0\xb4\xe5\xb1\x8f",
                 "", true);

  int card_gap = 10 * sc;
  int card_w = (W - 2 * pad - card_gap) / 2;
  int top_y = 50 * sc;
  int card_h = H - top_y - 54 * sc;

  ui_draw_card(fb, pad, top_y, card_w, card_h, true);
  ui_draw_card(fb, pad + card_w + card_gap, top_y, card_w, card_h, false);

  int y = top_y + 12 * sc;
  int left_x = pad + 10 * sc;
  int right_x = pad + card_w + card_gap + 10 * sc;
  int text_w = card_w - 20 * sc;

  ui_draw_section_label(fb, left_x, y,
                        "\xe7\xbd\x91\xe7\xbb\x9c\xe4\xbf\xa1\xe6\x81\xaf",
                        COLOR_RED, body_sc);
  y += ln + gap;

  bool sta = wifi_manager_sta_connected();
  if (sta) {
    char buf[80];
    snprintf(buf, sizeof(buf), "STA:%s", wifi_manager_get_sta_ssid());
    fb_utf8_scaled_maxw(fb, left_x, y, buf, COLOR_BLACK, body_sc, text_w);
    y += ln + gap;
    snprintf(buf, sizeof(buf), "IP:%s", wifi_manager_get_sta_ip());
    fb_utf8_scaled_maxw(fb, left_x, y, buf, COLOR_BLACK, body_sc, text_w);
    y += ln + gap;
  } else {
    fb_utf8_scaled(fb, left_x, y, "STA:\xe7\xa6\xbb\xe7\xba\xbf", COLOR_BLACK,
                   body_sc);
    y += ln + gap;
  }
  {
    char line[80];
    snprintf(line, sizeof(line), "AP:%s", device_identity_get_ap_ssid());
    fb_utf8_scaled_maxw(fb, left_x, y, line, COLOR_BLACK, body_sc, text_w);
    y += ln + gap;
  }
  {
    char line[80];
    snprintf(line, sizeof(line), "PW:%s", device_identity_get_ap_password());
    fb_utf8_scaled_maxw(fb, left_x, y, line, COLOR_BLACK, body_sc, text_w);
    y += ln + gap;
  }
  fb_utf8_scaled(fb, left_x, y, "WEB:192.168.4.1", COLOR_BLACK, body_sc);
  y += ln + gap;

  {
    char line[80];
    snprintf(line, sizeof(line), "MD:%s.local",
             device_identity_get_mdns_hostname());
    fb_utf8_scaled_maxw(fb, left_x, y, line, COLOR_BLACK, body_sc, text_w);
    y += ln + gap;
  }

  battery_mon_draw_on_fb(fb, left_x, y, COLOR_BLACK, body_sc);

  y = top_y + 12 * sc;
  ui_draw_section_label(fb, right_x, y,
                        "\xe4\xbd\xbf\xe7\x94\xa8\xe6\xad\xa5\xe9\xaa\xa4",
                        COLOR_RED, body_sc);
  y += ln + gap;

  {
    char line[88];
    snprintf(line, sizeof(line),
             "1.\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xb7\xa6\xe4\xbe\xa7 AP");
    fb_utf8_scaled_maxw(fb, right_x, y, line, COLOR_BLACK, body_sc, text_w);
  }
  y += ln + gap;
  fb_utf8_scaled(fb, right_x, y, "2.\xe6\x89\x93\xe5\xbc\x80 WEB", COLOR_BLACK,
                 body_sc);
  y += ln + gap;
  fb_utf8_scaled(fb, right_x, y,
                 "3.\xe9\x85\x8d\xe7\xbd\xae\xe5\xae\xb6\xe5\xba\xad WiFi",
                 COLOR_BLACK, body_sc);
  y += ln + gap;
  fb_utf8_scaled(fb, right_x, y, "4.\xe8\xbf\x94\xe5\x9b\x9e MD.local",
                 COLOR_BLACK, body_sc);
  y += ln + gap;
  y += gap;
  ui_draw_dotted_hline(fb, right_x, y, text_w, COLOR_BLACK, 6);
  y += gap * 2;
  fb_utf8_scaled_maxw(
      fb, right_x, y,
      "\xe9\x94\xae: \xe6\xa8\xa1\xe5\xbc\x8f / \xe4\xb8\x8a / \xe4\xb8\x8b",
      COLOR_RED, body_sc, text_w);

  {
    const esp_app_desc_t *app = esp_app_get_description();
    char info[80];
    snprintf(info, sizeof(info), "v%s | %dx%d | %s", app->version, W, H,
             epd_has_red() ? "BW+Red" : "BW");
    ui_draw_footer(fb, "SETUP", info);
  }

  epd_display_fb_free(fb);
  ESP_LOGI(TAG, "Welcome screen displayed");
}

static esp_err_t render_spiffs_recovery_screen(esp_err_t spiffs_err) {
  fb_t *fb = fb_create();
  if (!fb) {
    ESP_LOGE(TAG, "SPIFFS recovery FB alloc failed");
    return ESP_ERR_NO_MEM;
  }
  fb_clear(fb);

  int W = fb->width;
  int H = fb->height;
  const int body_sc = 1;
  const int ln = 16 * body_sc;
  const int gap = 3;
  const int pad = ui_layout_is_wide(fb) ? 24 : 12;
  const int header_y = (H >= 430) ? 16 : 14;
  const int divider_y = header_y + 21;
  const int card_y = divider_y + 7;
  const int card_h = H - card_y - ((H >= 430) ? 30 : 14);

  ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
  fb_fill_rect(fb, 12, header_y + 1, 2, 12, COLOR_RED);
  fb_utf8_scaled_maxw(fb, 20, header_y, "文件系统恢复", COLOR_BLACK, 1,
                      W / 2 - 28);
  {
    int battery_w = ui_draw_battery_badge(fb, W - 14, header_y);
    int tag_w = 6 * 8;
    int tag_x = W - 14 - battery_w - 8 - tag_w;
    if (tag_x < W / 2)
      tag_x = W / 2;
    fb_utf8_scaled_maxw(fb, tag_x, header_y + 1, "SPIFFS", COLOR_BLACK, 1,
                        W - 14 - battery_w - 8 - tag_x);
  }
  ui_draw_dotted_hline(fb, 12, divider_y, W - 24, COLOR_BLACK, 6);

  ui_draw_card(fb, pad, card_y, W - 2 * pad, card_h, true);
  int tx = pad + 12;
  int text_w = W - tx - pad - 8;
  int y = card_y + 12;

  fb_utf8_scaled(fb, tx, y, "文件系统挂载失败", COLOR_BLACK, body_sc);
  y += ln + gap;
  fb_utf8_scaled(fb, tx, y, "用户数据未被格式化", COLOR_RED, body_sc);
  y += ln + gap;

  {
    char line[96];
    snprintf(line, sizeof(line), "热点: %s", device_identity_get_ap_ssid());
    fb_utf8_scaled_maxw(fb, tx, y, line, COLOR_BLACK, body_sc, text_w);
    y += ln + gap;
    snprintf(line, sizeof(line), "密码: %s", device_identity_get_ap_password());
    fb_utf8_scaled_maxw(fb, tx, y, line, COLOR_BLACK, body_sc, text_w);
    y += ln + gap;
  }

  fb_utf8_scaled(fb, tx, y, "打开网页: 192.168.4.1/config", COLOR_BLACK,
                 body_sc);
  y += ln + gap;
  fb_utf8_scaled(fb, tx, y, "1.先点重试文件系统", COLOR_BLACK, body_sc);
  y += ln + gap;
  fb_utf8_scaled(fb, tx, y, "2.确认不要图片后再格式化", COLOR_BLACK, body_sc);
  y += ln + gap;
  fb_utf8_scaled(fb, tx, y, "确认词: FORMAT_SPIFFS", COLOR_RED, body_sc);
  y += ln + gap;

  {
    char info[96];
    ui_draw_dotted_hline(fb, tx, y, text_w, COLOR_BLACK, 6);
    y += gap * 2;
    snprintf(info, sizeof(info), "\xe9\x94\x99\xe8\xaf\xaf:%s",
             esp_err_to_name(spiffs_err));
    fb_utf8_scaled_maxw(fb, tx, y, info, COLOR_BLACK, body_sc, text_w);
    y += ln + gap;
    fb_utf8_scaled_maxw(fb, tx, y, "状态: 等待网页恢复", COLOR_BLACK, body_sc,
                        text_w);
  }

  return epd_display_fb_free(fb);
}

static void try_show_spiffs_recovery_screen(esp_err_t spiffs_err) {
  ESP_LOGW(TAG, "SPIFFS diagnostic: trying to show recovery screen");

  fb_reserve_planes_early();
  esp_err_t epd_err = epd_init();
  if (epd_err != ESP_OK) {
    ESP_LOGW(TAG, "SPIFFS diagnostic display skipped: EPD init failed (%s)",
             esp_err_to_name(epd_err));
    fb_release_reserved_planes();
    return;
  }

  (void)display_policy_begin_manual_display();
  esp_err_t disp_err = render_spiffs_recovery_screen(spiffs_err);
  if (disp_err == ESP_OK) {
    ESP_LOGI(TAG, "SPIFFS recovery screen displayed");
  } else {
    ESP_LOGW(TAG, "SPIFFS recovery screen failed: %s",
             esp_err_to_name(disp_err));
  }
  fb_release_reserved_planes();
}

static void boot_display_route(void) {
  ESP_LOGI(TAG, "boot_display_route: starting");
  display_policy_set_manual_screen_active(true);
  unsigned boot_epoch = display_policy_display_epoch();

  ESP_LOGI(TAG, "boot_display_route: showing welcome screen");
  render_welcome_screen();
  ESP_LOGI(TAG, "boot_display_route: welcome screen done, delaying 10s");
  vTaskDelay(pdMS_TO_TICKS(10000));

  if (display_policy_display_epoch() != boot_epoch) {
    ESP_LOGI(TAG, "boot_display_route: canceled by newer display request");
    return;
  }

  int saved_mode = power_mgr_load_mode();
  int n = display_mode_count();
  bool write_mode_back = false;
  int mode = normalize_saved_mode(saved_mode, n, &write_mode_back);
  /* If the saved mode was disabled by the user, fall back to the first
   * enabled mode (still keeping the NVS preference untouched). */
  if (!display_mode_is_enabled(mode)) {
    int first = -1;
    int order[DISPLAY_MODE_MAX];
    int on = display_mode_get_order(order, DISPLAY_MODE_MAX);
    for (int i = 0; i < on; i++)
      if (display_mode_is_enabled(order[i])) {
        first = order[i];
        break;
      }
    if (first >= 0) {
      ESP_LOGI(TAG, "boot_display_route: saved mode %d disabled, using %d",
               mode, first);
      mode = first;
      write_mode_back = true;
    }
  }
  ESP_LOGI(TAG, "boot_display_route: switching to mode %d (%s)", mode,
           display_mode_name(mode));
  boot_epoch = 0;
  esp_err_t err = display_mode_show_request(mode, &boot_epoch);
  if (err == ESP_OK && mode == DISPLAY_MODE_CALENDAR)
    calendar_display_wait_render_idle();
  if (display_policy_display_epoch() != boot_epoch) {
    ESP_LOGI(
        TAG,
        "boot_display_route: mode restore canceled by newer display request");
    return;
  }
  /* 数据源不可用时（典型：轮播图但 SPIFFS 无图）回落到时钟，
   * 保留 NVS 中用户原偏好，等数据补齐后下次启动自动恢复。 */
  if (err != ESP_OK && mode != DISPLAY_MODE_CLOCK) {
    ESP_LOGW(
        TAG,
        "boot_display_route: mode %d unavailable (%s), falling back to clock",
        mode, esp_err_to_name(err));
    esp_err_t fallback_err =
        display_mode_show_request(DISPLAY_MODE_CLOCK, &boot_epoch);
    if (fallback_err == ESP_OK) {
      mode = DISPLAY_MODE_CLOCK;
      err = fallback_err;
    } else {
      ESP_LOGW(TAG, "boot_display_route: clock fallback failed (%s)",
               esp_err_to_name(fallback_err));
      err = fallback_err;
    }
  }
  if (err == ESP_OK)
    button_set_current_mode(mode);
  /* 仅当原 NVS 值非法被纠正时才回写，避免把临时回落覆盖掉用户偏好 */
  if (write_mode_back && err == ESP_OK) {
    power_mgr_save_mode(mode);
  }

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "boot_display_route: done, mode %d active%s", mode,
             (mode != saved_mode) ? " (fallback)" : "");
  } else {
    ESP_LOGW(TAG, "boot_display_route: no active mode (%s)",
             esp_err_to_name(err));
  }
}

static void start_mdns(void) {
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
    return;
  }
  mdns_hostname_set(device_identity_get_mdns_hostname());
  mdns_instance_name_set(device_identity_get_mdns_instance());
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
  ESP_LOGI(TAG, "mDNS: http://%s.local/", device_identity_get_mdns_hostname());
}

/* ── Quick-refresh path: minimal init, refresh content, back to sleep ── */

static bool app_weather_config_ready(void) {
  weather_config_t cfg;
  if (weather_get_config(&cfg) != ESP_OK)
    return false;
  return cfg.enabled && cfg.api_key[0] && cfg.api_host[0] && cfg.location[0];
}

static bool app_codex_config_ready(void) {
  codex_quota_config_t cfg;
  if (codex_quota_get_config(&cfg) != ESP_OK)
    return false;
  if (!cfg.enabled)
    return false;
  /* Volcengine uses AK/SK; all others use api_key (+ URL for custom relay). */
  if (cfg.provider == QUOTA_PROVIDER_VOLCENGINE)
    return cfg.access_key_id[0] && cfg.secret_access_key[0];
  if (cfg.provider == QUOTA_PROVIDER_CUSTOM)
    return cfg.api_url[0] && cfg.api_key[0];
  return cfg.api_key[0] != '\0';
}

static void register_display_modes(void) {
  display_mode_register(
      &(display_mode_entry_t){"clock", "时钟", clock_display_show});
  display_mode_register(&(display_mode_entry_t){"calendar", "日历",
                                                calendar_display_show_current});
  display_mode_register(
      &(display_mode_entry_t){"timetable", "课程表", timetable_show});
  display_mode_register(
      &(display_mode_entry_t){"weather", "天气", weather_display_cached});
  display_mode_register(&(display_mode_entry_t){"slideshow", "轮播图",
                                                scheduler_show_next_image});
  display_mode_register(&(display_mode_entry_t){"todo", "待办事项", todo_show});
  display_mode_register(
      &(display_mode_entry_t){"countdown", "倒计时", countdown_show});
  display_mode_register(
      &(display_mode_entry_t){"codex", "Coding额度", codex_quota_show});
}

static bool quick_mode_needs_wifi(int mode, bool time_valid,
                                  bool *needs_weather) {
  bool weather_needed = false;
  bool wifi_needed = false;

  switch (mode) {
  case DISPLAY_MODE_WEATHER:
    weather_needed = app_weather_config_ready();
    wifi_needed = weather_needed;
    break;
  case DISPLAY_MODE_CODEX_QUOTA:
    wifi_needed = app_codex_config_ready();
    break;
  case DISPLAY_MODE_CLOCK: {
    clock_config_t cc;
    if (!time_valid)
      wifi_needed = true;
    if (clock_display_get_config(&cc) == ESP_OK && cc.show_weather) {
      weather_needed = app_weather_config_ready();
      wifi_needed = wifi_needed || weather_needed;
    }
    break;
  }
  case DISPLAY_MODE_CALENDAR:
    if (!time_valid)
      wifi_needed = true;
    if (calendar_display_style_uses_weather()) {
      weather_needed = app_weather_config_ready();
      wifi_needed = wifi_needed || weather_needed;
    }
    break;
  case DISPLAY_MODE_TIMETABLE:
  case DISPLAY_MODE_TODO:
  case DISPLAY_MODE_COUNTDOWN:
    wifi_needed = !time_valid;
    break;
  case DISPLAY_MODE_SLIDESHOW: {
    slideshow_config_t sc;
    if (scheduler_get_config(&sc) == ESP_OK && sc.clock_overlay && !time_valid)
      wifi_needed = true;
    break;
  }
  default:
    wifi_needed = !time_valid;
    break;
  }

  if (needs_weather)
    *needs_weather = weather_needed;
  return wifi_needed;
}

static void quick_refresh_and_sleep(void) {
  ESP_LOGI(TAG, "Quick-refresh path: timer wake-up");
  int mode = power_mgr_load_mode();

  esp_err_t spiffs_err = spiffs_mount_init();
  if (spiffs_err != ESP_OK) {
    ESP_LOGE(
        TAG,
        "Quick-refresh: SPIFFS unavailable (%s), sleeping instead of rebooting",
        esp_err_to_name(spiffs_err));
    power_mgr_enter_sleep();
    return;
  }
  display_policy_init();
  font_ext_init();
  display_policy_set_quick_refresh_active(true);
  device_identity_init();

  epd_load_panel_from_nvs();
  fb_reserve_planes_early();
  esp_err_t epd_err = epd_init();
  if (epd_err != ESP_OK) {
    ESP_LOGE(
        TAG,
        "Quick-refresh: EPD init failed (%s), sleeping instead of rebooting",
        esp_err_to_name(epd_err));
    power_mgr_enter_sleep();
    return;
  }

  /* Init content modules (they load NVS config) */
  ESP_ERROR_CHECK(scheduler_init());
  /* NOTE: do NOT call scheduler_boot_complete() here.
   * The slideshow_task must stay blocked — we only need
   * scheduler_show_next_image() which works without it. */
  weather_skip_initial_task_fetch_once();
  weather_set_quick_refresh_network_allowed(false);
  ESP_ERROR_CHECK(weather_init());
  ESP_ERROR_CHECK(clock_display_init());
  /* 与 full_boot 一致：日历依赖 s_mutex，未 init 则 calendar_display_show
   * 直接失败并触发回退到时钟 */
  ESP_ERROR_CHECK(calendar_display_init());
  ESP_ERROR_CHECK(timetable_init());
  ESP_ERROR_CHECK(todo_init());
  ESP_ERROR_CHECK(countdown_init());
  codex_quota_set_auto_network_allowed(false);
  ESP_ERROR_CHECK(codex_quota_init());

  register_display_modes();
  display_mode_prefs_load();

  int saved_mode = mode;
  int n = display_mode_count();
  bool write_mode_back = false;
  mode = normalize_saved_mode(saved_mode, n, &write_mode_back);
  /* Skip modes disabled by the user; keep NVS preference untouched. */
  if (!display_mode_is_enabled(mode)) {
    int order[DISPLAY_MODE_MAX];
    int on = display_mode_get_order(order, DISPLAY_MODE_MAX);
    for (int i = 0; i < on; i++)
      if (display_mode_is_enabled(order[i])) {
        mode = order[i];
        write_mode_back = true;
        break;
      }
  }

  bool time_valid = time_sync_get_local_relaxed(&(struct tm){0});

  bool prefetch_weather = false;
  bool need_wifi = quick_mode_needs_wifi(mode, time_valid, &prefetch_weather);
  weather_set_quick_refresh_network_allowed(need_wifi);
  codex_quota_set_auto_network_allowed(need_wifi);

  display_policy_set_manual_screen_active(true);

  ESP_LOGI(TAG, "Quick-refresh: restoring mode %d (%s)", mode,
           display_mode_name(mode));

  if (need_wifi) {
    ESP_LOGI(TAG, "Quick-refresh: WiFi enabled for mode %s (time_valid=%d)",
             display_mode_name(mode), time_valid ? 1 : 0);
    esp_err_t wifi_err = wifi_manager_init_sta_only();
    if (wifi_err != ESP_OK) {
      ESP_LOGW(TAG, "Quick-refresh: STA-only WiFi unavailable: %s",
               esp_err_to_name(wifi_err));
    }

    if (wifi_err == ESP_OK && wifi_manager_sta_connected()) {
      time_sync_init();
      for (int i = 0; i < 80; i++) {
        struct tm tm;
        if (time_sync_get_local(&tm))
          break;
        vTaskDelay(pdMS_TO_TICKS(100));
      }
    }
  } else {
    ESP_LOGI(TAG, "Quick-refresh: WiFi skipped for mode %s (time_valid=%d)",
             display_mode_name(mode), time_valid ? 1 : 0);
  }

  if (mode == DISPLAY_MODE_CALENDAR) {
    prefetch_weather = prefetch_weather && need_wifi;
  } else if (mode == DISPLAY_MODE_CLOCK) {
    prefetch_weather = prefetch_weather && need_wifi;
  }
  if (prefetch_weather) {
    esp_err_t wx_err = weather_request_cache_fetch_wait(120000);
    if (wx_err != ESP_OK)
      ESP_LOGW(TAG, "Quick-refresh: weather prefetch failed: %s",
               esp_err_to_name(wx_err));
  }

  esp_err_t err;
  if (mode == DISPLAY_MODE_WEATHER) {
    /* Deep sleep clears the in-RAM weather cache; timer wake must fetch
     * on the weather task stack, then sleep after the EPD update finishes. */
    (void)display_policy_begin_manual_display();
    display_mode_set_active(mode);
    err = weather_request_fullscreen_fetch_wait(180000);
  } else {
    err = display_mode_show_request(mode, NULL);
  }
  bool used_fallback = false;

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Quick-refresh: mode %d failed, trying clock", mode);
    err = display_mode_show_request(0, NULL);
    used_fallback = true;
  }

  bool displayed = false;
  if (err == ESP_OK) {
    displayed = true;
    if (write_mode_back)
      power_mgr_save_mode(mode);
    if (!used_fallback) {
      /* Original mode succeeded — no need to update NVS */
      ESP_LOGI(TAG, "Quick-refresh: mode %d displayed", mode);
    } else {
      /* Fell back to clock for *this* wake-up only.
       * Keep the user's preferred mode in NVS so next wake-up
       * retries it (e.g. SNTP may succeed by then). */
      ESP_LOGW(TAG,
               "Quick-refresh: showing clock as fallback, "
               "keeping saved mode %d for next wake",
               mode);
    }
    if (strcmp(display_mode_name(used_fallback ? 0 : mode), "calendar") == 0)
      calendar_display_wait_render_idle();
  } else {
    ESP_LOGW(TAG, "Quick-refresh: clock fallback also failed");
  }

  if (displayed) {
    ESP_LOGI(TAG, "Quick-refresh: display complete, sleep guard will wait for "
                  "EPD settle");
  }

  ESP_LOGI(TAG, "Quick-refresh done, going back to sleep");
  power_mgr_enter_sleep();
}

/* ── Full boot path (normal operation) ─────────────────────────────── */

static void full_boot(void) {
  esp_err_t spiffs_err = spiffs_mount_init();
  const bool spiffs_ok = (spiffs_err == ESP_OK);
  if (!spiffs_ok) {
    ESP_LOGE(TAG,
             "SPIFFS unavailable (%s); booting AP/Web diagnostics without "
             "formatting",
             esp_err_to_name(spiffs_err));
  }
  display_policy_init();
  font_ext_init();
  device_identity_init();

  ESP_ERROR_CHECK(wifi_manager_init(device_identity_get_ap_ssid(),
                                    device_identity_get_ap_password()));
  wifi_manager_set_sta_hostname(device_identity_get_mdns_hostname());
  start_mdns();

  if (wifi_manager_sta_connected()) {
    ESP_LOGI(TAG, "Connected to \"%s\", STA IP: %s",
             wifi_manager_get_sta_ssid(), wifi_manager_get_sta_ip());
    ESP_LOGI(TAG, "Access http://%s.local/ or http://%s/",
             device_identity_get_mdns_hostname(), wifi_manager_get_sta_ip());
    time_sync_init();
  } else {
    ESP_LOGI(TAG, "AP-only: connect to SSID \"%s\" (default AP password)",
             wifi_manager_get_ap_ssid());
  }

  /* 先读 NVS 再起 HTTP：换屏后 NVS 未改时 epd_init 可能长时间等 BUSY，httpd
   * 仍可响应 */
  epd_load_panel_from_nvs();
  ESP_LOGI(
      TAG,
      "NVS panel preset: %d (%dx%d) — open /config.html if hardware differs",
      (int)epd_get_panel(), epd_width(), epd_height());
  /* Reserve display planes only when the filesystem is usable; diagnostic
   * mode keeps RAM available for HTTP and recovery actions. */
  if (spiffs_ok) {
    fb_reserve_planes_early();
  }

  http_app_config_t cfg = {
      .mount_path = "/spiffs",
      .upload_path = "/spiffs/upload.jpg",
  };
  ESP_ERROR_CHECK(http_app_start(&cfg));

  if (!spiffs_ok) {
    display_policy_set_manual_screen_active(true);
    try_show_spiffs_recovery_screen(spiffs_err);
    if (scheduler_init() != ESP_OK)
      ESP_LOGW(TAG, "Diagnostic init: scheduler unavailable");
    weather_set_quick_refresh_network_allowed(true);
    if (weather_init() != ESP_OK)
      ESP_LOGW(TAG, "Diagnostic init: weather unavailable");
    if (clock_display_init() != ESP_OK)
      ESP_LOGW(TAG, "Diagnostic init: clock unavailable");
    if (message_board_init() != ESP_OK)
      ESP_LOGW(TAG, "Diagnostic init: message board unavailable");
    if (canvas_board_init() != ESP_OK)
      ESP_LOGW(TAG, "Diagnostic init: canvas unavailable");
    if (calendar_display_init() != ESP_OK)
      ESP_LOGW(TAG, "Diagnostic init: calendar unavailable");
    if (timetable_init() != ESP_OK)
      ESP_LOGW(TAG, "Diagnostic init: timetable unavailable");
    if (todo_init() != ESP_OK)
      ESP_LOGW(TAG, "Diagnostic init: todo unavailable");
    if (countdown_init() != ESP_OK)
      ESP_LOGW(TAG, "Diagnostic init: countdown unavailable");
    ESP_LOGW(TAG, "SPIFFS diagnostic mode: content display and sleep arming "
                  "are disabled");
    ESP_LOGI(TAG, "Ready. http://%s.local/ or http://192.168.4.1/",
             device_identity_get_mdns_hostname());
    return;
  }

  esp_err_t epd_err = epd_init();
  if (epd_err != ESP_OK) {
    ESP_LOGE(TAG, "EPD init failed (%s); keeping AP/Web online for diagnostics",
             esp_err_to_name(epd_err));
  }

  display_policy_set_manual_screen_active(true);

  ESP_ERROR_CHECK(scheduler_init());
  {
    bool write_mode_back = false;
    int boot_mode = normalize_saved_mode(
        power_mgr_load_mode(), DISPLAY_MODE_CODEX_QUOTA + 1, &write_mode_back);
    if (boot_mode == DISPLAY_MODE_WEATHER) {
      ESP_LOGI(TAG, "Boot target is weather; deferring initial weather fetch");
      weather_skip_initial_task_fetch_once();
    }
  }
  weather_set_quick_refresh_network_allowed(true);
  ESP_ERROR_CHECK(weather_init());
  ESP_ERROR_CHECK(clock_display_init());
  ESP_ERROR_CHECK(message_board_init());
  ESP_ERROR_CHECK(canvas_board_init());
  ESP_ERROR_CHECK(calendar_display_init());
  ESP_ERROR_CHECK(timetable_init());
  ESP_ERROR_CHECK(todo_init());
  ESP_ERROR_CHECK(countdown_init());
  codex_quota_set_auto_network_allowed(true);
  ESP_ERROR_CHECK(codex_quota_init());

  register_display_modes();
  display_mode_prefs_load();

  if (epd_err == ESP_OK) {
    boot_display_route();
  } else {
    ESP_LOGW(TAG, "Skipping boot display because EPD is not ready");
  }
  scheduler_boot_complete();

  ESP_ERROR_CHECK(button_init());

  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running) {
    if (running->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY) {
      esp_ota_mark_app_valid_cancel_rollback();
      ESP_LOGI(TAG, "OTA firmware validated (rollback cancelled)");
    }
    ESP_LOGI(TAG, "Running from partition '%s' @ 0x%lx", running->label,
             (unsigned long)running->address);
  }
  ESP_LOGI(TAG, "Ready. http://%s.local/ or http://192.168.4.1/",
           device_identity_get_mdns_hostname());

  esp_err_t arm_err = power_mgr_arm();
  if (arm_err != ESP_OK)
    ESP_LOGW(TAG, "Power manager arm failed: %s", esp_err_to_name(arm_err));
}

/* ── Entry point ───────────────────────────────────────────────────── */

void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  } else {
    ESP_ERROR_CHECK(err);
  }
  ESP_ERROR_CHECK(nvs_utils_init());

  ESP_ERROR_CHECK(power_mgr_init());

  if (battery_mon_init() != ESP_OK)
    ESP_LOGW(TAG, "Battery monitor unavailable (check BAT_DET / ADC pin)");

  if (power_mgr_is_timer_wake()) {
    quick_refresh_and_sleep();
  } else {
    full_boot();
  }
}
