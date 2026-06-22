#include "http_internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

#include "clock_display.h"
#include "display_policy.h"
#include "epd.h"
#include "fb_render.h"
#include "image_convert.h"
#include "scheduler.h"
#include "spiffs_mount.h"
#include "weather.h"
#include "button.h"
#include "display_mode.h"
#include "power_mgr.h"

static const char *TAG = "http_gal";
static atomic_bool s_upload_active;

static esp_err_t gallery_send_empty(httpd_req_t *req, bool spiffs_ok)
{
    char json[48];
    snprintf(json, sizeof(json), "{\"items\":[],\"spiffs_ok\":%s}",
             spiffs_ok ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static atomic_uint s_upload_epoch;

static unsigned gallery_upload_begin(void)
{
    atomic_store(&s_upload_active, true);
    return atomic_fetch_add(&s_upload_epoch, 1) + 1;
}

static void gallery_upload_end(unsigned epoch)
{
    if (atomic_load(&s_upload_epoch) == epoch)
        atomic_store(&s_upload_active, false);
}

static bool gallery_upload_busy(void)
{
    return atomic_load(&s_upload_active);
}

static unsigned gallery_upload_epoch(void)
{
    return atomic_load(&s_upload_epoch);
}

static void gallery_restore_manual_if_current(bool was_manual, unsigned display_epoch)
{
    if (!was_manual && display_policy_epoch_is_current(display_epoch))
        display_policy_set_manual_screen_active(false);
}

/** 追加 JSON 片段；*len 为已用长度（不含 '\\0'），cap 为 buf 总容量 */
static bool gallery_json_append(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len >= cap) return true;
    size_t avail = cap - *len;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *len, avail, fmt, ap);
    va_end(ap);
    if (w < 0) return false;
    if ((size_t)w >= avail) {
        *len = cap - 1;
        buf[*len] = '\0';
        return true;
    }
    *len += (size_t)w;
    return true;
}

/* 白名单验证文件名：只允许字母、数字、点、下划线、连字符 */
static bool is_safe_filename(const char *name)
{
    if (!name || name[0] == '\0') return false;
    size_t len = strlen(name);
    if (len > 60 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

/** Image focus is transient. Display policy pauses background weather without rewriting NVS. */
static void weather_disable_if_enabled_for_image_focus(void)
{
    /* Kept as a call-site marker; do not disable the user's weather configuration here. */
}

static bool gallery_needs_precise_image_clear(void)
{
    return epd_get_panel() == EPD_PANEL_ATC_SSD1619_29_BWR;
}

static esp_err_t gallery_prepare_image_refresh(const char *reason)
{
    if (!gallery_needs_precise_image_clear()) {
        epd_request_full_refresh_next();
        return ESP_OK;
    }

    ESP_LOGI(TAG, "%s: clearing 2.9 SSD1619 before manual image refresh", reason);
    esp_err_t err = epd_clear_screen();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: pre-clear failed: %s", reason, esp_err_to_name(err));
        return err;
    }
    epd_request_full_refresh_next();
    return ESP_OK;
}

static esp_err_t gallery_display_raw_like_canvas(const char *raw_path)
{
    fb_t *fb = fb_create();
    if (!fb)
        return ESP_ERR_NO_MEM;

    esp_err_t err = fb_import(fb, raw_path);
    if (err != ESP_OK) {
        fb_destroy(fb);
        return err;
    }
    return epd_display_fb_free(fb);
}

esp_err_t gallery_images_get_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    if (!spiffs_mount_is_mounted()) {
        return gallery_send_empty(req, false);
    }

    DIR *dir = opendir(http_images_dir);
    if (!dir) {
        (void)mkdir(http_images_dir, 0755);
        dir = opendir(http_images_dir);
    }
    if (!dir) {
        return gallery_send_empty(req, true);
    }

    size_t cap = 2048;
    char *buf = malloc(cap);
    if (!buf) {
        closedir(dir);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存分配失败");
        return ESP_OK;
    }

    size_t len = 0;
    (void)gallery_json_append(buf, cap, &len, "{\"items\":[");

    struct dirent *ent;
    bool first = true;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
        char path[160];
        snprintf(path, sizeof(path), "%s/%.80s", http_images_dir, ent->d_name);
        int sz = http_stat_size_or_neg(path);
        if (sz < 0) continue;

        size_t need = strlen(ent->d_name) + 48;
        if (len + need >= cap) {
            size_t newcap = cap * 2;
            char *tmp = realloc(buf, newcap);
            if (!tmp) {
                ESP_LOGW(TAG, "/images: realloc %u failed, truncating list", (unsigned)newcap);
                break;
            }
            buf = tmp;
            cap = newcap;
        }

        if (!first) {
            if (len + 1 < cap)
                buf[len++] = ',';
            else
                break;
        }
        first = false;
        char esc_name[80];
        json_escape(esc_name, sizeof(esc_name), ent->d_name);
        if (!gallery_json_append(buf, cap, &len, "{\"name\":\"%s\",\"bytes\":%d}", esc_name, sz))
            break;
    }
    closedir(dir);

    (void)gallery_json_append(buf, cap, &len, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)len);
    free(buf);
    return ESP_OK;
}

esp_err_t gallery_image_get_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char name[64];
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少文件名");
        return ESP_OK;
    }
    if (!is_safe_filename(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "文件名无效");
        return ESP_OK;
    }

    char path[160];
    snprintf(path, sizeof(path), "%s/%s", http_images_dir, name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "文件未找到");
        return ESP_OK;
    }

    const char *mime = "image/jpeg";
    size_t nlen = strlen(name);
    if (nlen > 4 && strcasecmp(name + nlen - 4, ".png") == 0) mime = "image/png";
    if (nlen > 4 && strcasecmp(name + nlen - 4, ".bmp") == 0) mime = "image/bmp";
    httpd_resp_set_type(req, mime);

    char buf[1024];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t gallery_show_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    unsigned upload_epoch = gallery_upload_epoch();
    if (gallery_upload_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "图片正在上传，请稍后再显示图库图片");
        return ESP_OK;
    }
    if (!http_require_epd_ready(req))
        return ESP_OK;

    char name[64];
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少文件名");
        return ESP_OK;
    }
    if (!is_safe_filename(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "文件名无效");
        return ESP_OK;
    }

    char jpeg_path[160];
    snprintf(jpeg_path, sizeof(jpeg_path), "%s/%s", http_images_dir, name);
    int sz = http_stat_size_or_neg(jpeg_path);
    if (sz <= 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "文件未找到");
        return ESP_OK;
    }

    bool was_manual = display_policy_manual_screen_active();
    unsigned display_epoch = display_policy_begin_manual_display();

    const char *raw_path = "/spiffs/image.bin";
    fb_raw_file_lock();
    if (gallery_upload_busy() || gallery_upload_epoch() != upload_epoch ||
        !display_policy_epoch_is_current(display_epoch)) {
        fb_raw_file_unlock();
        ESP_LOGI(TAG, "Discard stale gallery show '%s' after upload request", name);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "图片上传已接管屏幕，本次旧显示请求已忽略");
        return ESP_OK;
    }
    esp_err_t cerr = image_convert_file_to_epd_raw(jpeg_path, raw_path);
    esp_err_t derr = ESP_OK;
    bool skipped_by_newer = false;
    if (cerr == ESP_OK && display_policy_epoch_is_current(display_epoch)) {
        derr = gallery_prepare_image_refresh("gallery show");
        if (derr == ESP_OK && gallery_upload_epoch() == upload_epoch &&
            !gallery_upload_busy() &&
            display_policy_epoch_is_current(display_epoch)) {
            derr = gallery_display_raw_like_canvas(raw_path);
        } else if (derr == ESP_OK) {
            skipped_by_newer = true;
        }
    }
    fb_raw_file_unlock();
    if (cerr != ESP_OK) {
            gallery_restore_manual_if_current(was_manual, display_epoch);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "图片转换失败");
        return ESP_OK;
    }
    if (skipped_by_newer) {
        ESP_LOGI(TAG, "Gallery show '%s' skipped after pre-clear: newer request owns display", name);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "stale gallery show skipped");
        return ESP_OK;
    }
    if (!display_policy_epoch_is_current(display_epoch)) {
        ESP_LOGI(TAG, "Discard stale gallery show '%s' before display", name);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "stale gallery show skipped");
        return ESP_OK;
    }
    if (derr != ESP_OK) {
            gallery_restore_manual_if_current(was_manual, display_epoch);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "墨水屏刷新失败");
        return ESP_OK;
    }
    if (!display_policy_epoch_is_current(display_epoch)) {
        ESP_LOGI(TAG, "Gallery show '%s' completed but was superseded", name);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "gallery show superseded");
        return ESP_OK;
    }
    scheduler_set_current_image_name(name);
    /* 手动画面由 display_policy 控制，避免后台天气/时钟继续写 image.bin 抢屏。 */
    weather_disable_if_enabled_for_image_focus();
    scheduler_notify_manual_show();
    button_set_current_mode(DISPLAY_MODE_SLIDESHOW);
    power_mgr_save_mode(DISPLAY_MODE_SLIDESHOW);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

esp_err_t gallery_delete_image_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    char name[64];
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少文件名");
        return ESP_OK;
    }
    if (!is_safe_filename(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "文件名无效");
        return ESP_OK;
    }

    char jpeg_path[160];
    snprintf(jpeg_path, sizeof(jpeg_path), "%s/%s", http_images_dir, name);
    int ok = (remove(jpeg_path) == 0) ? 1 : 0;

    char msg[96];
    snprintf(msg, sizeof(msg), "delete %.40s: %s", name, ok ? "ok" : "not found");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

esp_err_t gallery_delete_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    char tmp[64];
    int remain = req->content_len;
    while (remain > 0) {
        int to_recv = remain < (int)sizeof(tmp) ? remain : (int)sizeof(tmp);
        int r = httpd_req_recv(req, tmp, to_recv);
        if (r <= 0) break;
        remain -= r;
    }

    int ok1 = (remove(http_app_cfg.upload_path)  == 0) ? 1 : 0;
    fb_raw_file_lock();
    int ok2 = (remove("/spiffs/image.bin") == 0) ? 1 : 0;
    fb_raw_file_unlock();

    if (epd_is_ready())
        (void)epd_clear_screen();
    display_policy_set_manual_screen_active(false);
    clock_display_notify_home_changed();

    char msg[96];
    snprintf(msg, sizeof(msg), "已删除：upload.jpg=%s，image.bin=%s；并已清屏",
             ok1 ? "是" : "否", ok2 ? "是" : "否");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

esp_err_t gallery_upload_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体为空");
        return ESP_OK;
    }
    if (req->content_len > http_upload_max_bytes) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "文件过大");
        return ESP_OK;
    }

    unsigned upload_epoch = gallery_upload_begin();
    bool was_manual = display_policy_manual_screen_active();
    unsigned display_epoch = display_policy_begin_manual_display();

    FILE *f = fopen(http_app_cfg.upload_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d", http_app_cfg.upload_path, errno);
        gallery_upload_end(upload_epoch);
            gallery_restore_manual_if_current(was_manual, display_epoch);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "文件打开失败");
        return ESP_OK;
    }

    char buf[1024];
    int remaining = req->content_len;
    size_t total_written = 0;
    bool first_chunk = true;
    while (remaining > 0) {
        int to_recv = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int r = httpd_req_recv(req, buf, to_recv);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r < 0) {
            fclose(f);
            remove(http_app_cfg.upload_path);
            gallery_upload_end(upload_epoch);
        gallery_restore_manual_if_current(was_manual, display_epoch);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收数据失败");
            return ESP_OK;
        }
        if (r == 0) {
            fclose(f);
            remove(http_app_cfg.upload_path);
            gallery_upload_end(upload_epoch);
        gallery_restore_manual_if_current(was_manual, display_epoch);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "客户端断开连接");
            return ESP_OK;
        }
        if (first_chunk && r >= 4) {
            ESP_LOGI(TAG, "First chunk magic: %02X %02X %02X %02X (recv %d bytes)",
                     (uint8_t)buf[0], (uint8_t)buf[1], (uint8_t)buf[2], (uint8_t)buf[3], r);
            first_chunk = false;
        }
        size_t written = fwrite(buf, 1, (size_t)r, f);
        if (written != (size_t)r) {
            ESP_LOGE(TAG, "fwrite failed: wrote %u of %d", (unsigned)written, r);
            fclose(f);
            remove(http_app_cfg.upload_path);
            gallery_upload_end(upload_epoch);
        gallery_restore_manual_if_current(was_manual, display_epoch);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "写入失败");
            return ESP_OK;
        }
        total_written += written;
        remaining -= r;
    }
    fclose(f);
    ESP_LOGI(TAG, "Saved upload to %s (%u bytes written)", http_app_cfg.upload_path, (unsigned)total_written);

    /* verify the written file */
    {
        struct stat vst;
        if (stat(http_app_cfg.upload_path, &vst) == 0) {
            ESP_LOGI(TAG, "Verify: file size on disk = %ld", (long)vst.st_size);
        } else {
            ESP_LOGE(TAG, "Verify: stat failed errno=%d", errno);
        }
        FILE *vf = fopen(http_app_cfg.upload_path, "rb");
        if (vf) {
            uint8_t hd[4] = {0};
            fread(hd, 1, 4, vf);
            fclose(vf);
            ESP_LOGI(TAG, "Verify: file magic = %02X %02X %02X %02X", hd[0], hd[1], hd[2], hd[3]);
        }
    }

    char name[64];
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        const char *ext = "jpg";
        uint8_t hdr[4] = {0};
        FILE *hf = fopen(http_app_cfg.upload_path, "rb");
        if (hf) { fread(hdr, 1, 4, hf); fclose(hf); }
        if (hdr[0] == 0x89 && hdr[1] == 0x50) ext = "png";
        if (hdr[0] == 0x42 && hdr[1] == 0x4D) ext = "bmp";
        snprintf(name, sizeof(name), "img_%d.%s", (int)esp_random(), ext);
    }
    if (!is_safe_filename(name)) {
        snprintf(name, sizeof(name), "img_%d.jpg", (int)esp_random());
    }
    char final_path[160];
    snprintf(final_path, sizeof(final_path), "%s/%s", http_images_dir, name);
    FILE *src = fopen(http_app_cfg.upload_path, "rb");
    FILE *dst = src ? fopen(final_path, "wb") : NULL;
    bool copy_ok = false;
    if (src && dst) {
        char cbuf[512];
        size_t r;
        copy_ok = true;
        while ((r = fread(cbuf, 1, sizeof(cbuf), src)) > 0) {
            if (fwrite(cbuf, 1, r, dst) != r) {
                copy_ok = false;
                break;
            }
        }
        fclose(dst);
        if (copy_ok) {
            ESP_LOGI(TAG, "Stored gallery copy at %s", final_path);
        } else {
            ESP_LOGE(TAG, "Failed while copying gallery image to %s", final_path);
        }
    }
    if (src) fclose(src);
    if (!copy_ok) {
        remove(final_path);
        gallery_upload_end(upload_epoch);
        gallery_restore_manual_if_current(was_manual, display_epoch);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "图库保存失败");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Free heap before convert: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    if (!http_require_epd_ready(req)) {
        gallery_upload_end(upload_epoch);
        gallery_restore_manual_if_current(was_manual, display_epoch);
        return ESP_OK;
    }

    const char *raw_path = "/spiffs/image.bin";
    fb_raw_file_lock();
    if (!display_policy_epoch_is_current(display_epoch) ||
        gallery_upload_epoch() != upload_epoch) {
        fb_raw_file_unlock();
        gallery_upload_end(upload_epoch);
        ESP_LOGI(TAG, "Upload '%s' display skipped: newer request owns display", name);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "upload saved, display skipped by newer request");
        return ESP_OK;
    }
    esp_err_t cerr = image_convert_file_to_epd_raw(http_app_cfg.upload_path, raw_path);
    if (cerr != ESP_OK) {
        fb_raw_file_unlock();
        remove(final_path);
        gallery_upload_end(upload_epoch);
        gallery_restore_manual_if_current(was_manual, display_epoch);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "图片转换失败：请上传标准的 JPEG、PNG 或 BMP 图片");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Raw ready at %s, free heap: %lu",
             raw_path, (unsigned long)esp_get_free_heap_size());

    if (!display_policy_epoch_is_current(display_epoch) ||
        gallery_upload_epoch() != upload_epoch) {
        fb_raw_file_unlock();
        gallery_upload_end(upload_epoch);
        ESP_LOGI(TAG, "Upload '%s' display skipped after convert: newer request owns display", name);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "upload saved, display skipped by newer request");
        return ESP_OK;
    }

    esp_err_t derr = gallery_prepare_image_refresh("upload");
    bool upload_skipped_by_newer = false;
    if (derr == ESP_OK && display_policy_epoch_is_current(display_epoch) &&
        gallery_upload_epoch() == upload_epoch) {
        derr = gallery_display_raw_like_canvas(raw_path);
    } else if (derr == ESP_OK) {
        upload_skipped_by_newer = true;
    }
    fb_raw_file_unlock();
    if (upload_skipped_by_newer) {
        gallery_upload_end(upload_epoch);
        ESP_LOGI(TAG, "Upload '%s' display skipped after pre-clear: newer request owns display", name);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "upload saved, display skipped by newer request");
        return ESP_OK;
    }
    if (derr != ESP_OK) {
        ESP_LOGW(TAG, "EPD display failed: %s", esp_err_to_name(derr));
        gallery_upload_end(upload_epoch);
        gallery_restore_manual_if_current(was_manual, display_epoch);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "图片已转换但墨水屏刷新失败");
        return ESP_OK;
    }

    if (!display_policy_epoch_is_current(display_epoch) ||
        gallery_upload_epoch() != upload_epoch) {
        gallery_upload_end(upload_epoch);
        ESP_LOGI(TAG, "Upload '%s' display completed but was superseded", name);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "upload display superseded");
        return ESP_OK;
    }
    scheduler_set_current_image_name(name);
    weather_disable_if_enabled_for_image_focus();
    scheduler_notify_manual_show();
    button_set_current_mode(DISPLAY_MODE_SLIDESHOW);
    power_mgr_save_mode(DISPLAY_MODE_SLIDESHOW);
    gallery_upload_end(upload_epoch);
    httpd_resp_sendstr(req, "OK — 已转换并触发墨水屏刷新。");
    return ESP_OK;
}
