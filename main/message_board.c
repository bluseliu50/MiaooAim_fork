#include "message_board.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "fb_render.h"
#include "epd.h"
#include "scheduler.h"
#include "display_policy.h"
#include "ui_theme.h"

static const char *TAG    = "msgboard";
static const char *NVS_NS = "msgboard";

static msg_config_t s_cfg = {
    .text      = "",
    .font_size = 0,
    .align     = MSG_ALIGN_CENTER,
    .color     = 0,
    .x_offset  = 0,
    .y_offset  = 0,
};

static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── NVS ──────────────────────────────────────────────────────────── */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t len = sizeof(s_cfg.text);
    nvs_get_str(h, "text", s_cfg.text, &len);

    uint8_t v8;
    if (nvs_get_u8(h, "font", &v8) == ESP_OK) s_cfg.font_size = v8;
    if (nvs_get_u8(h, "align", &v8) == ESP_OK) s_cfg.align = (msg_align_t)v8;
    if (nvs_get_u8(h, "color", &v8) == ESP_OK) s_cfg.color = v8;

    int16_t v16;
    if (nvs_get_i16(h, "xoff", &v16) == ESP_OK) s_cfg.x_offset = v16;
    if (nvs_get_i16(h, "yoff", &v16) == ESP_OK) s_cfg.y_offset = v16;

    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "text",  s_cfg.text);
    nvs_set_u8(h,  "font",  s_cfg.font_size);
    nvs_set_u8(h,  "align", (uint8_t)s_cfg.align);
    nvs_set_u8(h,  "color", s_cfg.color);
    nvs_set_i16(h, "xoff",  s_cfg.x_offset);
    nvs_set_i16(h, "yoff",  s_cfg.y_offset);
    nvs_commit(h);
    nvs_close(h);
}

/* ── UTF-8 helpers ────────────────────────────────────────────────── */

static int utf8_decode(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    if (p[0] == 0)
        return -1;
    if (p[0] < 0x80) {
        *pp += 1;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0) {
        if ((p[1] & 0xC0) != 0x80) { *pp += 1; return -1; }
        *pp += 2;
        return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) { *pp += 1; return -1; }
        *pp += 3;
        return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((p[0] & 0xF8) == 0xF0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) { *pp += 1; return -1; }
        *pp += 4;
        return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *pp += 1;
    return -1;
}

static int utf8_encode_codepoint(int cp, char out[5])
{
    if (cp < 0)
        cp = '?';
    if (cp < 0x80) {
        out[0] = (char)cp;
        out[1] = '\0';
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = '\0';
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = '\0';
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    out[4] = '\0';
    return 4;
}

static int msg_font_px_for_scale(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    return 16 * scale;
}

static int utf8_char_width_px(int cp, int target_px)
{
    char one[5];
    utf8_encode_codepoint(cp, one);
    return fb_utf8_px_width(one, target_px);
}

/* ── word-wrap rendering ──────────────────────────────────────────── */

#define MARGIN_X  20
#define MARGIN_Y  16

static void render_message(unsigned epoch)
{
    fb_t *fb = fb_create();
    if (!fb) {
        ESP_LOGE(TAG, "fb_create failed");
        return;
    }

    msg_config_t cfg;
    portENTER_CRITICAL(&s_cfg_mux);
    cfg = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);

    if (cfg.text[0] == '\0') {
        fb_destroy(fb);
        ESP_LOGW(TAG, "No message to display");
        return;
    }

    int scale = cfg.font_size + 1;
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    int target_px = msg_font_px_for_scale(scale);
    int char_h = target_px;
    int line_h = char_h + 2 * scale;
    int max_w  = fb->width - 2 * MARGIN_X - 16;
    fb_color_t color = (cfg.color == 1) ? COLOR_RED : COLOR_BLACK;
    int page_s = ui_scale_for(fb);
    int content_top = 34 * page_s;
    int content_bottom = fb->height - 30 * page_s;
    if (content_bottom <= content_top + line_h)
        content_bottom = fb->height - 20 * page_s;

    ESP_LOGI(TAG, "Rendering: scale=%d px=%d, color=%d, align=%d, offset=(%d,%d), text_len=%d",
             scale, target_px, cfg.color, (int)cfg.align, cfg.x_offset, cfg.y_offset,
             (int)strlen(cfg.text));

    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
    ui_draw_header(fb, "\xe7\x95\x99\xe8\xa8\x80\xe6\x9d\xbf", "MESSAGE", true);

    /* first pass: calculate lines for vertical centering */
    typedef struct { const char *start; int len; int px_w; } line_t;
    line_t lines[32];
    int line_count = 0;

    const char *p = cfg.text;
    const char *line_start = p;
    int cur_w = 0;

    while (*p && line_count < 32) {
        if (*p == '\n') {
            lines[line_count].start = line_start;
            lines[line_count].len   = (int)(p - line_start);
            lines[line_count].px_w  = cur_w;
            line_count++;
            p++;
            line_start = p;
            cur_w = 0;
            continue;
        }

        const char *next = p;
        int cp = utf8_decode(&next);
        if (cp < 0) { p = next; continue; }
        int cw = utf8_char_width_px(cp, target_px);

        if (cur_w + cw > max_w && cur_w > 0) {
            lines[line_count].start = line_start;
            lines[line_count].len   = (int)(p - line_start);
            lines[line_count].px_w  = cur_w;
            line_count++;
            line_start = p;
            cur_w = 0;
            if (line_count >= 32) break;
        }

        cur_w += cw;
        p = next;
    }

    if (line_count < 32 && (cur_w > 0 || p > line_start)) {
        lines[line_count].start = line_start;
        lines[line_count].len   = (int)(p - line_start);
        lines[line_count].px_w  = cur_w;
        line_count++;
    }

    bool truncated = (*p != '\0' && line_count >= 32);

    int total_h = line_count * line_h;
    int content_h = content_bottom - content_top;
    int start_y = content_top + (content_h - total_h) / 2 + cfg.y_offset;
    if (start_y < content_top) start_y = content_top;

    for (int i = 0; i < line_count; i++) {
        int y = start_y + i * line_h;
        if (y + char_h > content_bottom) break;

        int x;
        if (cfg.align == MSG_ALIGN_CENTER) {
            x = (fb->width - lines[i].px_w) / 2 + cfg.x_offset;
        } else {
            x = MARGIN_X + 8 * page_s + cfg.x_offset;
        }
        if (x < MARGIN_X)
            x = MARGIN_X;

        char buf[MSG_MAX_LEN + 1];
        int copy_len = lines[i].len;
        if (copy_len > MSG_MAX_LEN) copy_len = MSG_MAX_LEN;
        memcpy(buf, lines[i].start, copy_len);
        buf[copy_len] = '\0';

        fb_utf8_px(fb, x, y, buf, color, target_px);
    }

    if (truncated) {
        int hint_y = start_y + line_count * line_h;
        if (hint_y + 16 <= fb->height)
            fb_utf8_px(fb, (fb->width - fb_utf8_px_width("...", 16)) / 2,
                       hint_y, "...", COLOR_BLACK, 16);
    }

    ui_draw_footer(fb, "MSG", cfg.align == MSG_ALIGN_CENTER ? "CENTER" : "LEFT");

    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        ESP_LOGI(TAG, "skip stale message render");
        return;
    }

    /* export and display */
    const char *raw_path = "/spiffs/image.bin";
    fb_raw_file_lock();
    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        fb_raw_file_unlock();
        ESP_LOGI(TAG, "skip stale message render before export");
        return;
    }
    esp_err_t err = fb_export(fb, raw_path);
    fb_destroy(fb);

    if (err == ESP_OK && epd_is_ready() && display_policy_epoch_is_current(epoch)) {
        ESP_LOGI(TAG, "FB exported, sending to EPD (%d lines, scale=%d px=%d)...",
                 line_count, scale, target_px);
        esp_err_t disp_err = epd_display_from_file(raw_path);
        if (disp_err == ESP_OK) {
            display_policy_set_manual_screen_active(true);
            scheduler_notify_manual_show();
            ESP_LOGI(TAG, "Message displayed on EPD");
        } else {
            ESP_LOGE(TAG, "display failed: %s", esp_err_to_name(disp_err));
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "fb_export failed: %s", esp_err_to_name(err));
    }
    fb_raw_file_unlock();
}

/* ── public API ───────────────────────────────────────────────────── */

esp_err_t message_board_get_config(msg_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_cfg_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
    return ESP_OK;
}

esp_err_t message_board_set_config(const msg_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg = *cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
    nvs_save();
    ESP_LOGI(TAG, "Config saved (text_len=%d)", (int)strlen(cfg->text));
    return ESP_OK;
}

static SemaphoreHandle_t s_render_mutex;
static atomic_bool       s_pending;
static atomic_uint       s_pending_epoch;

esp_err_t message_board_init(void)
{
    s_render_mutex = xSemaphoreCreateBinary();
    if (!s_render_mutex)
        return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_render_mutex);
    nvs_load();
    ESP_LOGI(TAG, "Message board init (text_len=%d, font=%d, align=%d, color=%d, xy=%d,%d)",
             (int)strlen(s_cfg.text), s_cfg.font_size, (int)s_cfg.align, s_cfg.color,
             s_cfg.x_offset, s_cfg.y_offset);
    return ESP_OK;
}

static void render_task(void *arg)
{
    unsigned epoch = (unsigned)(uintptr_t)arg;
    for (;;) {
        atomic_store(&s_pending, false);
        ESP_LOGI(TAG, "Render task started");
        render_message(epoch);
        ESP_LOGI(TAG, "Render task done");

        portENTER_CRITICAL(&s_cfg_mux);
        bool has_text = (s_cfg.text[0] != '\0');
        portEXIT_CRITICAL(&s_cfg_mux);
        if (!atomic_load(&s_pending) || !has_text) break;
        epoch = atomic_load(&s_pending_epoch);
    }

    xSemaphoreGive(s_render_mutex);
    vTaskDelete(NULL);
}

esp_err_t message_board_show_queued(unsigned *out_epoch)
{
    if (s_cfg.text[0] == '\0') {
        ESP_LOGW(TAG, "No message text set");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_render_mutex)
        return ESP_ERR_INVALID_STATE;

    unsigned epoch = display_policy_begin_manual_display();
    if (out_epoch)
        *out_epoch = epoch;

    if (xSemaphoreTake(s_render_mutex, 0) != pdTRUE) {
        atomic_store(&s_pending_epoch, epoch);
        atomic_store(&s_pending, true);
        ESP_LOGI(TAG, "Render queued (will re-render after current finishes)");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(render_task, "msg_render", 8192,
                                 (void *)(uintptr_t)epoch, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create render task");
        xSemaphoreGive(s_render_mutex);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t message_board_show(void)
{
    return message_board_show_queued(NULL);
}

void message_board_wait_idle(void)
{
    if (!s_render_mutex)
        return;
    xSemaphoreTake(s_render_mutex, portMAX_DELAY);
    xSemaphoreGive(s_render_mutex);
}
