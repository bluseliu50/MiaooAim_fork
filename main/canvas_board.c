/**
 * @file canvas_board.c
 *
 * WYSIWYG 画布留言板渲染器。
 * 解析来自 Web 编辑器的 JSON 布局数组，逐元素绘制到 fb_t，再推送墨水屏。
 *
 * 支持的元素类型：
 *   text    — UTF-8 文本（含汉字），scale=1..6
 *   rect    — 矩形（fill/stroke，可圆角：radius 字段）
 *   ellipse — 椭圆/圆（fill/stroke，rx/ry）
 *   line    — Bresenham 直线
 *   icon    — 内置或用户上传的 16×16 1-bit 图标，scale=1..4
 */

#include "canvas_board.h"
#include "canvas_icons.h"
#include "fb_render.h"
#include "epd.h"
#include "display_policy.h"
#include "scheduler.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

static const char *TAG = "canvas";
static SemaphoreHandle_t s_layout_file_mutex;

#define CANVAS_RENDER_YIELD_ROWS 8

static SemaphoreHandle_t layout_file_mutex(void)
{
    if (!s_layout_file_mutex) {
        s_layout_file_mutex = xSemaphoreCreateMutex();
    }
    return s_layout_file_mutex;
}

static bool layout_file_lock(TickType_t timeout)
{
    SemaphoreHandle_t m = layout_file_mutex();
    return m && xSemaphoreTake(m, timeout) == pdTRUE;
}

static void layout_file_unlock(void)
{
    if (s_layout_file_mutex) {
        xSemaphoreGive(s_layout_file_mutex);
    }
}

static esp_err_t write_layout_bytes(const char *path, const char *buf, size_t len)
{
    FILE *out = fopen(path, "w");
    if (!out) {
        ESP_LOGE(TAG, "commit: fopen(%s) failed errno=%d", path, errno);
        return ESP_FAIL;
    }
    size_t nw = fwrite(buf, 1, len, out);
    int close_rc = fclose(out);
    if (nw != len || close_rc != 0) {
        ESP_LOGE(TAG, "commit: write %s failed %zu/%zu close=%d errno=%d",
                 path, nw, len, close_rc, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool is_valid_name(const char *name)
{
    if (!name || name[0] == '\0') return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

#define LAYOUT_PATH      "/spiffs/canvas_layout.json"
#define LAYOUT_BAK_PATH  "/spiffs/canvas_layout.bak"

/* ── 渲染互斥 ───────────────────────────────────────────────────────── */

static SemaphoreHandle_t s_mutex;

/* ── Bresenham 直线（支持线宽：垂直方向扩展半径） ─────────────────── */

static void draw_line_core(fb_t *fb, int x0, int y0, int x1, int y1, fb_color_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int steps = 0;
    for (;;) {
        fb_pixel(fb, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        if ((++steps & 0x3F) == 0)
            vTaskDelay(1);
    }
}

static void draw_line(fb_t *fb, int x0, int y0, int x1, int y1,
                      fb_color_t c, int sw)
{
    if (sw < 1) sw = 1;
    if (sw == 1) { draw_line_core(fb, x0, y0, x1, y1, c); return; }
    int half = sw / 2;
    /* 根据直线方向决定垂直扩展方向 */
    int ddx = abs(x1 - x0), ddy = abs(y1 - y0);
    for (int t = -half; t <= half; t++) {
        if (ddx >= ddy) /* 偏水平，垂直方向偏移 */
            draw_line_core(fb, x0, y0 + t, x1, y1 + t, c);
        else            /* 偏垂直，水平方向偏移 */
            draw_line_core(fb, x0 + t, y0, x1 + t, y1, c);
    }
}

/* ── 加粗矩形描边（按线宽向内收缩逐圈绘制） ────────────────────────── */

static void draw_rect_stroked(fb_t *fb, int x, int y, int w, int h,
                               fb_color_t c, int sw)
{
    if (sw < 1) sw = 1;
    for (int i = 0; i < sw && w - 2*i > 0 && h - 2*i > 0; i++)
        fb_rect(fb, x + i, y + i, w - 2*i, h - 2*i, c);
}

/* ── 圆角矩形（Bresenham 四分之一圆弧 + hline 填充/描边） ──────────── */

static void draw_rounded_rect(fb_t *fb, int x, int y, int w, int h,
                               int r, fb_color_t c, bool fill, int sw)
{
    if (r <= 0) {
        if (fill) fb_fill_rect(fb, x, y, w, h, c);
        else draw_rect_stroked(fb, x, y, w, h, c, sw);
        return;
    }
    r = r < w/2 ? r : w/2;
    r = r < h/2 ? r : h/2;

    /* 四个圆弧中心 */
    int cx0 = x + r,         cy0 = y + r;
    int cx1 = x + w - 1 - r;
    int cx2 = x + r,         cy2 = y + h - 1 - r;
    int cx3 = x + w - 1 - r;

    if (fill) {
        /* 水平填充行 */
        for (int dy = 0; dy <= r; dy++) {
            /* Bresenham: dx = sqrt(r^2 - dy^2) */
            int dx = (int)sqrtf((float)(r * r - dy * dy));
            fb_hline(fb, cx0 - dx, cy0 - dy, cx1 - cx0 + 2 * dx + 1, c);
            fb_hline(fb, cx2 - dx, cy2 + dy, cx3 - cx2 + 2 * dx + 1, c);
        }
        /* 中间实心区域 */
        if (cy0 + 1 <= cy2 - 1)
            for (int row = cy0 + 1; row <= cy2 - 1; row++)
                fb_hline(fb, x, row, w, c);
    } else {
        /* 描边：逐层收缩 */
        if (sw < 1) sw = 1;
        for (int s = 0; s < sw; s++) {
            int xs = x + s, ys = y + s, ws = w - 2*s, hs = h - 2*s;
            int rs = r - s; if (rs < 0) rs = 0;
            int cxa = xs + rs, cya = ys + rs;
            int cxb = xs + ws - 1 - rs, cyb = ys + hs - 1 - rs;
            /* 水平/垂直直线段 */
            fb_hline(fb, cxa, ys,     ws - 2*rs, c);
            fb_hline(fb, xs+rs, ys+hs-1, ws - 2*rs, c);
            fb_vline(fb, xs,     cya, hs - 2*rs, c);
            fb_vline(fb, xs+ws-1, cya, hs - 2*rs, c);
            /* 四段圆弧（Midpoint） */
            int px = 0, py = rs, p2 = 1 - rs;
            while (px <= py) {
                fb_pixel(fb, cxa - px, cya - py, c);
                fb_pixel(fb, cxa - py, cya - px, c);
                fb_pixel(fb, cxb + px, cya - py, c);
                fb_pixel(fb, cxb + py, cya - px, c);
                fb_pixel(fb, cxa - px, cyb + py, c);
                fb_pixel(fb, cxa - py, cyb + px, c);
                fb_pixel(fb, cxb + px, cyb + py, c);
                fb_pixel(fb, cxb + py, cyb + px, c);
                px++;
                if (p2 < 0) p2 += 2*px + 1;
                else { py--; p2 += 2*(px - py) + 1; }
            }
        }
    }
}

/* ── 椭圆（Midpoint 算法，支持线宽：逐层收缩） ─────────────────────── */

static void draw_ellipse_pts(fb_t *fb, int cx, int cy, int x, int y,
                              fb_color_t c, bool fill)
{
    if (fill) {
        fb_hline(fb, cx - x, cy + y, 2 * x + 1, c);
        if (y != 0)
            fb_hline(fb, cx - x, cy - y, 2 * x + 1, c);
    } else {
        fb_pixel(fb, cx + x, cy + y, c);
        fb_pixel(fb, cx - x, cy + y, c);
        fb_pixel(fb, cx + x, cy - y, c);
        fb_pixel(fb, cx - x, cy - y, c);
    }
}

static void draw_ellipse_one(fb_t *fb, int cx, int cy, int rx, int ry,
                              fb_color_t c, bool fill)
{
    if (rx <= 0 || ry <= 0) return;
    long rx2 = (long)rx * rx;
    long ry2 = (long)ry * ry;
    long x = 0, y = ry;
    long p = ry2 - rx2 * ry + rx2 / 4;
    while (2 * ry2 * x < 2 * rx2 * y) {
        draw_ellipse_pts(fb, cx, cy, (int)x, (int)y, c, fill);
        x++;
        if (p < 0) {
            p += 2 * ry2 * x + ry2;
        } else {
            y--;
            p += 2 * ry2 * x - 2 * rx2 * y + ry2;
        }
    }
    p = (long)(x + 0.5) * (long)(x + 0.5) * ry2
      + (y - 1) * (y - 1) * rx2 - rx2 * ry2;
    while (y >= 0) {
        draw_ellipse_pts(fb, cx, cy, (int)x, (int)y, c, fill);
        y--;
        if (p > 0) {
            p -= 2 * rx2 * y + rx2;
        } else {
            x++;
            p += 2 * ry2 * x - 2 * rx2 * y + rx2;
        }
    }
}

static void draw_ellipse(fb_t *fb, int cx, int cy, int rx, int ry,
                          fb_color_t c, bool fill, int sw)
{
    if (fill) { draw_ellipse_one(fb, cx, cy, rx, ry, c, true); return; }
    if (sw < 1) sw = 1;
    for (int i = 0; i < sw; i++) {
        int r2x = rx - i, r2y = ry - i;
        if (r2x <= 0 || r2y <= 0) break;
        draw_ellipse_one(fb, cx, cy, r2x, r2y, c, false);
    }
}

/* ── 颜色映射（JSON color: 0=黑 1=红 2=白） ──────────────────────── */

static fb_color_t json_color(int v)
{
    switch (v) {
    case 1:  return COLOR_RED;
    case 2:  return COLOR_WHITE;
    default: return COLOR_BLACK;
    }
}

/* ── 加载用户图标（16×16, 32 字节 1-bit） ───────────────────────── */

static bool load_user_icon(const char *name, uint8_t bmp[32])
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.bin", CANVAS_ICONS_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = (fread(bmp, 1, 32, f) == 32);
    fclose(f);
    return ok;
}

/* ── 元素渲染 ───────────────────────────────────────────────────────── */

#define CANVAS_COORD_MIN  (-2000)
#define CANVAS_COORD_MAX  (2000)
#define CANVAS_DIM_MAX    (2000)
#define CANVAS_STROKE_MAX 64

static inline int clamp_stroke(int sw)
{
    if (sw < 1) return 1;
    if (sw > CANVAS_STROKE_MAX) return CANVAS_STROKE_MAX;
    return sw;
}

static inline int clamp_coord(int v) {
    if (v < CANVAS_COORD_MIN) return CANVAS_COORD_MIN;
    if (v > CANVAS_COORD_MAX) return CANVAS_COORD_MAX;
    return v;
}
static inline int clamp_dim(int v) {
    if (v < 0) return 0;
    if (v > CANVAS_DIM_MAX) return CANVAS_DIM_MAX;
    return v;
}

static int canvas_text_px_for_scale(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    return 16 * scale;
}

static void render_element(fb_t *fb, cJSON *el)
{
    const char *type_str = cJSON_GetStringValue(cJSON_GetObjectItem(el, "type"));
    if (!type_str) return;

    cJSON *jc = cJSON_GetObjectItem(el, "color");
    fb_color_t color = json_color(jc && cJSON_IsNumber(jc) ? jc->valueint : 0);

    /* ── text ──────────────────────────────────────────────────────── */
    if (strcmp(type_str, "text") == 0) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(el, "text"));
        if (!text || text[0] == '\0') return;
        cJSON *jx = cJSON_GetObjectItem(el, "x");
        cJSON *jy = cJSON_GetObjectItem(el, "y");
        cJSON *js = cJSON_GetObjectItem(el, "scale");
        int x = clamp_coord(jx && cJSON_IsNumber(jx) ? jx->valueint : 0);
        int y = clamp_coord(jy && cJSON_IsNumber(jy) ? jy->valueint : 0);
        int scale = js && cJSON_IsNumber(js) ? js->valueint : 1;
        if (scale < 1) scale = 1;
        if (scale > 6) scale = 6;
        fb_utf8_px(fb, x, y, text, color, canvas_text_px_for_scale(scale));
        return;
    }

    /* ── rect ──────────────────────────────────────────────────────── */
    if (strcmp(type_str, "rect") == 0) {
        cJSON *jx  = cJSON_GetObjectItem(el, "x");
        cJSON *jy  = cJSON_GetObjectItem(el, "y");
        cJSON *jw  = cJSON_GetObjectItem(el, "w");
        cJSON *jh  = cJSON_GetObjectItem(el, "h");
        cJSON *jf  = cJSON_GetObjectItem(el, "fill");
        cJSON *jsw = cJSON_GetObjectItem(el, "sw");
        cJSON *jr  = cJSON_GetObjectItem(el, "radius");
        int x  = clamp_coord(jx  && cJSON_IsNumber(jx)  ? jx->valueint  : 0);
        int y  = clamp_coord(jy  && cJSON_IsNumber(jy)  ? jy->valueint  : 0);
        int w  = clamp_dim(jw  && cJSON_IsNumber(jw)  ? jw->valueint  : 0);
        int h  = clamp_dim(jh  && cJSON_IsNumber(jh)  ? jh->valueint  : 0);
        int sw = jsw && cJSON_IsNumber(jsw) ? jsw->valueint : 1;
        int r  = jr  && cJSON_IsNumber(jr)  ? jr->valueint  : 0;
        bool fill = jf && cJSON_IsTrue(jf);
        if (w <= 0 || h <= 0) return;
        draw_rounded_rect(fb, x, y, w, h, r, color, fill, sw);
        return;
    }

    /* ── ellipse ───────────────────────────────────────────────────── */
    if (strcmp(type_str, "ellipse") == 0) {
        cJSON *jx  = cJSON_GetObjectItem(el, "x");
        cJSON *jy  = cJSON_GetObjectItem(el, "y");
        cJSON *jrx = cJSON_GetObjectItem(el, "rx");
        cJSON *jry = cJSON_GetObjectItem(el, "ry");
        cJSON *jf  = cJSON_GetObjectItem(el, "fill");
        cJSON *jsw = cJSON_GetObjectItem(el, "sw");
        int cx = clamp_coord(jx  && cJSON_IsNumber(jx)  ? jx->valueint  : 0);
        int cy = clamp_coord(jy  && cJSON_IsNumber(jy)  ? jy->valueint  : 0);
        int rx = clamp_dim(jrx && cJSON_IsNumber(jrx) ? jrx->valueint : 0);
        int ry = clamp_dim(jry && cJSON_IsNumber(jry) ? jry->valueint : 0);
        int sw = clamp_stroke(jsw && cJSON_IsNumber(jsw) ? jsw->valueint : 1);
        bool fill = jf && cJSON_IsTrue(jf);
        draw_ellipse(fb, cx, cy, rx, ry, color, fill, sw);
        return;
    }

    /* ── line ──────────────────────────────────────────────────────── */
    if (strcmp(type_str, "line") == 0) {
        cJSON *jx1 = cJSON_GetObjectItem(el, "x1");
        cJSON *jy1 = cJSON_GetObjectItem(el, "y1");
        cJSON *jx2 = cJSON_GetObjectItem(el, "x2");
        cJSON *jy2 = cJSON_GetObjectItem(el, "y2");
        cJSON *jsw = cJSON_GetObjectItem(el, "sw");
        int x1 = clamp_coord(jx1 && cJSON_IsNumber(jx1) ? jx1->valueint : 0);
        int y1 = clamp_coord(jy1 && cJSON_IsNumber(jy1) ? jy1->valueint : 0);
        int x2 = clamp_coord(jx2 && cJSON_IsNumber(jx2) ? jx2->valueint : 0);
        int y2 = clamp_coord(jy2 && cJSON_IsNumber(jy2) ? jy2->valueint : 0);
        int sw = clamp_stroke(jsw && cJSON_IsNumber(jsw) ? jsw->valueint : 1);
        draw_line(fb, x1, y1, x2, y2, color, sw);
        return;
    }

    /* ── icon ──────────────────────────────────────────────────────── */
    if (strcmp(type_str, "icon") == 0) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(el, "name"));
        if (!name || name[0] == '\0') return;
        cJSON *jx = cJSON_GetObjectItem(el, "x");
        cJSON *jy = cJSON_GetObjectItem(el, "y");
        cJSON *js = cJSON_GetObjectItem(el, "scale");
        int x = clamp_coord(jx && cJSON_IsNumber(jx) ? jx->valueint : 0);
        int y = clamp_coord(jy && cJSON_IsNumber(jy) ? jy->valueint : 0);
        int scale = js && cJSON_IsNumber(js) ? js->valueint : 1;
        if (scale < 1) scale = 1;
        if (scale > 4) scale = 4;

        const uint8_t *bmp = canvas_find_builtin_icon(name);
        uint8_t user_bmp[32];
        if (!bmp) {
            if (is_valid_name(name) && load_user_icon(name, user_bmp))
                bmp = user_bmp;
        }
        if (!bmp) return;

        /* 用 fb_bitmap 绘制，scale>1 时逐像素放大 */
        if (scale == 1) {
            fb_bitmap(fb, x, y, 16, 16, bmp, color);
        } else {
            /* 手动放大：每个源像素 → scale×scale 个目标像素 */
            for (int row = 0; row < 16; row++) {
                uint16_t word = ((uint16_t)bmp[row * 2] << 8) | bmp[row * 2 + 1];
                for (int col = 0; col < 16; col++) {
                    if (word & (0x8000 >> col)) {
                        for (int dy = 0; dy < scale; dy++)
                            for (int dx = 0; dx < scale; dx++)
                                fb_pixel(fb, x + col * scale + dx,
                                             y + row * scale + dy, color);
                    }
                }
            }
        }
        return;
    }

    /* ── image ─────────────────────────────────────────────────────── */
    if (strcmp(type_str, "image") == 0) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(el, "name"));
        if (!is_valid_name(name)) return;
        cJSON *jx  = cJSON_GetObjectItem(el, "x");
        cJSON *jy  = cJSON_GetObjectItem(el, "y");
        cJSON *jdw = cJSON_GetObjectItem(el, "dw");
        cJSON *jdh = cJSON_GetObjectItem(el, "dh");
        int x  = clamp_coord(jx  && cJSON_IsNumber(jx)  ? jx->valueint  : 0);
        int y  = clamp_coord(jy  && cJSON_IsNumber(jy)  ? jy->valueint  : 0);
        int dw = jdw && cJSON_IsNumber(jdw) ? jdw->valueint : 0;
        int dh = jdh && cJSON_IsNumber(jdh) ? jdh->valueint : 0;

        char path[128];
        snprintf(path, sizeof(path), "%s/%s.bin", CANVAS_IMAGES_DIR, name);
        FILE *f = fopen(path, "rb");
        if (!f) return;

        uint16_t img_w = 0, img_h = 0;
        if (fread(&img_w, 2, 1, f) != 1 || fread(&img_h, 2, 1, f) != 1 ||
            img_w == 0 || img_h == 0 || img_w > 1024 || img_h > 1024) {
            fclose(f); return;
        }
        if (dw <= 0) dw = img_w;
        if (dh <= 0) dh = img_h;
        dw = clamp_dim(dw);
        dh = clamp_dim(dh);
        if (dw <= 0 || dh <= 0) { fclose(f); return; }
        int stride = (img_w + 7) / 8;
        size_t bitmap_len = (size_t)stride * img_h;
        bool has_red_plane = false;
        long data_start = 4;
        long file_size = 0;
        if (fseek(f, 0, SEEK_END) == 0) {
            file_size = ftell(f);
        }
        fseek(f, 4, SEEK_SET);
        uint8_t hdr[4];
        if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)) {
            if (hdr[0] == 'B' && hdr[1] == 'W' && (hdr[2] & 0x01) &&
                file_size >= 8 + (long)bitmap_len * 2) {
                has_red_plane = true;
                data_start = 8;
            } else {
                fseek(f, 4, SEEK_SET);
            }
        } else {
            fseek(f, 4, SEEK_SET);
        }
        size_t planes_len = bitmap_len * (has_red_plane ? 2 : 1);
        uint8_t *bitmap = heap_caps_malloc(planes_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!bitmap)
            bitmap = heap_caps_malloc(planes_len, MALLOC_CAP_8BIT);

        if (bitmap) {
            if (fread(bitmap, 1, planes_len, f) == planes_len) {
                const uint8_t *red_plane = has_red_plane ? bitmap + bitmap_len : NULL;
                for (int ty = 0; ty < dh; ty++) {
                    int sy = ty * img_h / dh;
                    const uint8_t *row = bitmap + (size_t)sy * stride;
                    const uint8_t *red_row = red_plane ? red_plane + (size_t)sy * stride : NULL;
                    for (int tx = 0; tx < dw; tx++) {
                        int sx = tx * img_w / dw;
                        int bit = (row[sx / 8] >> (7 - sx % 8)) & 1;
                        int red_bit = red_row ? ((red_row[sx / 8] >> (7 - sx % 8)) & 1) : 0;
                        if (red_bit) fb_pixel(fb, x + tx, y + ty, COLOR_RED);
                        else if (bit) fb_pixel(fb, x + tx, y + ty,
                                               has_red_plane ? COLOR_BLACK : color);
                    }
                    if ((ty & (CANVAS_RENDER_YIELD_ROWS - 1)) == 0)
                        vTaskDelay(1);
                }
            }
            free(bitmap);
        } else {
            fseek(f, data_start, SEEK_SET);
            uint8_t *row_buf = malloc((size_t)stride);
            if (!row_buf) { fclose(f); return; }

            int prev_sy = -1;
            bool stream_ok = true;
            for (int ty = 0; ty < dh && stream_ok; ty++) {
                int sy = ty * img_h / dh;
                while (prev_sy < sy) {
                    if (fread(row_buf, 1, (size_t)stride, f) < (size_t)stride) {
                        stream_ok = false;
                        break;
                    }
                    prev_sy++;
                }
                if (!stream_ok) break;
                for (int tx = 0; tx < dw; tx++) {
                    int sx = tx * img_w / dw;
                    int bit = (row_buf[sx / 8] >> (7 - sx % 8)) & 1;
                    if (bit) fb_pixel(fb, x + tx, y + ty,
                                      has_red_plane ? COLOR_BLACK : color);
                }
                if ((ty & (CANVAS_RENDER_YIELD_ROWS - 1)) == 0)
                    vTaskDelay(1);
            }
            free(row_buf);
        }
        fclose(f);
        return;
    }

    if (strcmp(type_str, "path") == 0) {
        /* pts 是 [[x0,y0],[x1,y1],...] 数组 */
        cJSON *jpts = cJSON_GetObjectItem(el, "pts");
        cJSON *jsw  = cJSON_GetObjectItem(el, "sw");
        int sw = clamp_stroke(jsw && cJSON_IsNumber(jsw) ? jsw->valueint : 1);
        if (!cJSON_IsArray(jpts)) return;
        int n = cJSON_GetArraySize(jpts);
        if (n < 2) return;
        for (int k = 0; k < n - 1; k++) {
            cJSON *p0 = cJSON_GetArrayItem(jpts, k);
            cJSON *p1 = cJSON_GetArrayItem(jpts, k + 1);
            if (!cJSON_IsArray(p0) || !cJSON_IsArray(p1)) continue;
            cJSON *x0j = cJSON_GetArrayItem(p0, 0), *y0j = cJSON_GetArrayItem(p0, 1);
            cJSON *x1j = cJSON_GetArrayItem(p1, 0), *y1j = cJSON_GetArrayItem(p1, 1);
            if (!x0j || !y0j || !x1j || !y1j) continue;
            if (!cJSON_IsNumber(x0j) || !cJSON_IsNumber(y0j) ||
                !cJSON_IsNumber(x1j) || !cJSON_IsNumber(y1j)) continue;
            int xa = clamp_coord(x0j->valueint);
            int ya = clamp_coord(y0j->valueint);
            int xb = clamp_coord(x1j->valueint);
            int yb = clamp_coord(y1j->valueint);
            draw_line(fb, xa, ya, xb, yb, color, sw);
        }
        return;
    }

    /* 未知类型忽略 */
    ESP_LOGW(TAG, "Unknown element type: %s", type_str);
}

/* ── 渲染全帧 ───────────────────────────────────────────────────────── */

static void do_render(unsigned epoch)
{
    /* 先确认布局文件存在并读取大小：避免无布局时白白占用 78KB 帧缓冲。 */
    if (!layout_file_lock(pdMS_TO_TICKS(5000))) {
        ESP_LOGW(TAG, "Layout file busy, skip render");
        return;
    }
    FILE *f = fopen(LAYOUT_PATH, "r");
    if (!f) {
        layout_file_unlock();
        ESP_LOGW(TAG, "No layout file, nothing to render");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > CANVAS_LAYOUT_MAX_BYTES) {
        fclose(f);
        layout_file_unlock();
        ESP_LOGE(TAG, "Layout file size invalid: %ld", sz);
        return;
    }

    /* 关键：先分配最大的那块（帧缓冲，约 78KB），再去解析 JSON。
     * 这样 fb_create 拿到尽可能大的连续块，避免被 cJSON 节点先撑碎堆。 */
    fb_t *fb = fb_create();
    if (!fb) {
        ESP_LOGE(TAG, "fb_create failed (free=%lu, largest=%lu)",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        fclose(f);
        layout_file_unlock();
        return;
    }

    char *json_str = malloc((size_t)sz + 1);
    if (!json_str) {
        fclose(f);
        layout_file_unlock();
        fb_destroy(fb);
        ESP_LOGE(TAG, "malloc failed for layout (%ld bytes)", sz);
        return;
    }
    size_t nr = fread(json_str, 1, (size_t)sz, f);
    fclose(f);
    layout_file_unlock();
    json_str[nr] = '\0';
    if ((long)nr != sz) {
        ESP_LOGE(TAG, "Layout read incomplete: %zu/%ld", nr, sz);
        free(json_str);
        fb_destroy(fb);
        return;
    }

    /* 解析前后采样堆状态，便于区分「JSON 损坏」和「内存不足」 */
    size_t free_before = esp_get_free_heap_size();
    size_t lblk_before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        const char *errp = cJSON_GetErrorPtr();
        long offset = (errp && errp >= json_str) ? (long)(errp - json_str) : -1;
        size_t free_after = esp_get_free_heap_size();
        size_t lblk_after = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        ESP_LOGE(TAG, "JSON parse failed at offset %ld of %ld bytes; "
                      "heap free: %u→%u, largest: %u→%u",
                 offset, sz,
                 (unsigned)free_before, (unsigned)free_after,
                 (unsigned)lblk_before, (unsigned)lblk_after);
        /* 打印失败位置附近的内容（最多 64 字节），帮助判断文件损坏 */
        if (offset >= 0 && offset < sz) {
            long start = offset > 16 ? offset - 16 : 0;
            long end   = offset + 48 < sz ? offset + 48 : sz;
            char snippet[80];
            long m = end - start;
            if (m >= (long)sizeof(snippet)) m = sizeof(snippet) - 1;
            memcpy(snippet, json_str + start, (size_t)m);
            snippet[m] = '\0';
            for (long i = 0; i < m; i++) {
                if ((unsigned char)snippet[i] < 0x20) snippet[i] = '?';
            }
            ESP_LOGE(TAG, "  context: \"%s\"", snippet);
        }
        free(json_str);
        fb_destroy(fb);
        return;
    }
    free(json_str);
    if (!cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Layout root is not an array");
        cJSON_Delete(root);
        fb_destroy(fb);
        return;
    }

    int n = cJSON_GetArraySize(root);
    ESP_LOGI(TAG, "Rendering %d elements", n);
    for (int i = 0; i < n; i++) {
        cJSON *el = cJSON_GetArrayItem(root, i);
        if (el) render_element(fb, el);
        vTaskDelay(1);
    }
    cJSON_Delete(root);

    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        ESP_LOGI(TAG, "skip stale canvas render");
        return;
    }

    esp_err_t err = epd_display_fb_free(fb);
    if (err == ESP_OK) {
        display_policy_set_manual_screen_active(true);
        scheduler_notify_manual_show();
        ESP_LOGI(TAG, "Canvas rendered (%d elements)", n);
    } else {
        ESP_LOGE(TAG, "epd_display_fb_free: %s", esp_err_to_name(err));
    }
}

/* ── 渲染任务 ───────────────────────────────────────────────────────── */

static void render_task(void *arg)
{
    unsigned epoch = (unsigned)(uintptr_t)arg;
    do_render(epoch);
    xSemaphoreGive(s_mutex);
    vTaskDelete(NULL);
}

/* ── 图片目录确保存在 ───────────────────────────────────────────────── */

static void ensure_images_dir(void)
{
    struct stat st;
    if (stat(CANVAS_IMAGES_DIR, &st) != 0)
        mkdir(CANVAS_IMAGES_DIR, 0755);
}

static void restore_layout_backup_if_needed(void)
{
    struct stat st;
    if (!layout_file_lock(pdMS_TO_TICKS(5000))) {
        ESP_LOGW(TAG, "Layout file busy, skip backup restore");
        return;
    }
    if (stat(LAYOUT_PATH, &st) == 0) {
        layout_file_unlock();
        return;
    }
    if (stat(LAYOUT_BAK_PATH, &st) != 0) {
        layout_file_unlock();
        return;
    }

    if (rename(LAYOUT_BAK_PATH, LAYOUT_PATH) == 0) {
        ESP_LOGW(TAG, "Restored canvas layout from backup");
    } else {
        ESP_LOGE(TAG, "Failed to restore canvas layout backup: errno=%d", errno);
    }
    layout_file_unlock();
}

/* ── 图片管理 ───────────────────────────────────────────────────────── */

esp_err_t canvas_board_list_images(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 4) return ESP_ERR_INVALID_ARG;
    size_t pos = 0;
    buf[pos++] = '[';
    DIR *d = opendir(CANVAS_IMAGES_DIR);
    if (d) {
        bool first = true;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && pos < buf_len - 64) {
            size_t nl = strlen(ent->d_name);
            if (nl < 5 || strcmp(ent->d_name + nl - 4, ".bin") != 0) continue;
            if (!first) buf[pos++] = ',';
            first = false;
            char name[64];
            size_t nlen = nl - 4;
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            memcpy(name, ent->d_name, nlen);
            name[nlen] = '\0';
            pos += snprintf(buf + pos, buf_len - pos - 2, "\"%s\"", name);
        }
        closedir(d);
    }
    buf[pos++] = ']';
    buf[pos] = '\0';
    return ESP_OK;
}

esp_err_t canvas_board_save_image(const char *name, const uint8_t *data, size_t len)
{
    if (!name || !data) return ESP_ERR_INVALID_ARG;
    if (!is_valid_name(name)) return ESP_ERR_INVALID_ARG;
    if (len < 5 || len > CANVAS_IMAGE_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    ensure_images_dir();
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.bin", CANVAS_IMAGES_DIR, name);
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return (w == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t canvas_board_get_image_data(const char *name,
                                       uint8_t *buf, size_t buf_len,
                                       size_t *out_len)
{
    if (!name || !buf || !out_len) return ESP_ERR_INVALID_ARG;
    if (!is_valid_name(name)) return ESP_ERR_INVALID_ARG;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.bin", CANVAS_IMAGES_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    *out_len = fread(buf, 1, buf_len, f);
    fclose(f);
    return ESP_OK;
}

esp_err_t canvas_board_delete_image(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    if (!is_valid_name(name)) return ESP_ERR_INVALID_ARG;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.bin", CANVAS_IMAGES_DIR, name);
    return (remove(path) == 0) ? ESP_OK : ESP_FAIL;
}


static void ensure_icons_dir(void)
{
    struct stat st;
    if (stat(CANVAS_ICONS_DIR, &st) != 0) {
        mkdir(CANVAS_ICONS_DIR, 0755);
    }
}

/* ── 公开 API ───────────────────────────────────────────────────────── */

esp_err_t canvas_board_init(void)
{
    s_mutex = xSemaphoreCreateBinary();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_mutex);
    ensure_icons_dir();
    ensure_images_dir();
    restore_layout_backup_if_needed();
    return ESP_OK;
}

esp_err_t canvas_board_get_layout(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return ESP_ERR_INVALID_ARG;
    if (!layout_file_lock(pdMS_TO_TICKS(5000))) return ESP_ERR_TIMEOUT;
    FILE *f = fopen(LAYOUT_PATH, "r");
    if (!f) {
        strncpy(buf, "[]", buf_len - 1);
        buf[buf_len - 1] = '\0';
        layout_file_unlock();
        return ESP_OK;
    }
    size_t r = fread(buf, 1, buf_len - 1, f);
    fclose(f);
    layout_file_unlock();
    buf[r] = '\0';
    return ESP_OK;
}

esp_err_t canvas_board_set_layout(const char *json, size_t len)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    if (len == 0) len = strlen(json);
    if (len > CANVAS_LAYOUT_MAX_BYTES) return ESP_ERR_INVALID_SIZE;

    /* 写入前先校验是 JSON 数组，防止坏数据污染文件，让后续渲染始终失败 */
    cJSON *probe = cJSON_ParseWithLength(json, len);
    if (!probe) {
        const char *errp = cJSON_GetErrorPtr();
        long off = (errp && errp >= json) ? (long)(errp - json) : -1;
        ESP_LOGE(TAG, "set_layout: invalid JSON at offset %ld of %zu bytes",
                 off, len);
        return ESP_ERR_INVALID_ARG;
    }
    bool is_arr = cJSON_IsArray(probe);
    cJSON_Delete(probe);
    if (!is_arr) {
        ESP_LOGE(TAG, "set_layout: root must be a JSON array");
        return ESP_ERR_INVALID_ARG;
    }

    if (!layout_file_lock(pdMS_TO_TICKS(5000))) return ESP_ERR_TIMEOUT;
    esp_err_t err = write_layout_bytes(LAYOUT_PATH, json, len);
    layout_file_unlock();
    return err;
}

esp_err_t canvas_board_commit_layout_from_file(const char *tmp_path)
{
    if (!tmp_path) return ESP_ERR_INVALID_ARG;

    /* 1) 取临时文件大小，做大小阈值检查 */
    FILE *f = fopen(tmp_path, "r");
    if (!f) {
        ESP_LOGE(TAG, "commit: tmp file %s missing", tmp_path);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > CANVAS_LAYOUT_MAX_BYTES) {
        fclose(f);
        unlink(tmp_path);
        ESP_LOGE(TAG, "commit: tmp size invalid: %ld", sz);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 2) 读回内存做 cJSON 校验。这里的 (sz+1) 单次 malloc 不可避免，
     *    但 cJSON 树本身就 ~5x 这一字符串大小，碎片化风险与 tree 一致。 */
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        unlink(tmp_path);
        return ESP_ERR_NO_MEM;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';
    if ((long)nr != sz) {
        free(buf);
        unlink(tmp_path);
        ESP_LOGE(TAG, "commit: read incomplete %zu/%ld", nr, sz);
        return ESP_FAIL;
    }

    cJSON *probe = cJSON_ParseWithLength(buf, (size_t)nr);
    if (!probe) {
        free(buf);
        unlink(tmp_path);
        ESP_LOGE(TAG, "commit: invalid JSON, tmp discarded");
        return ESP_ERR_INVALID_ARG;
    }
    bool is_arr = cJSON_IsArray(probe);
    cJSON_Delete(probe);
    if (!is_arr) {
        free(buf);
        unlink(tmp_path);
        ESP_LOGE(TAG, "commit: root not array, tmp discarded");
        return ESP_ERR_INVALID_ARG;
    }

    /* 3) 替换正式文件。当前板上 SPIFFS 对 tmp->real rename 稳定返回 EIO，
     *    所以这里直接写入已校验的 JSON；如已有旧文件，先备份便于失败恢复。 */
    if (!layout_file_lock(pdMS_TO_TICKS(5000))) {
        free(buf);
        return ESP_ERR_TIMEOUT;
    }
    bool had_old = (access(LAYOUT_PATH, F_OK) == 0);
    bool backup_ok = false;
    if (had_old) {
        unlink(LAYOUT_BAK_PATH);
        if (rename(LAYOUT_PATH, LAYOUT_BAK_PATH) != 0) {
            ESP_LOGE(TAG, "commit: backup rename(%s -> %s) failed errno=%d",
                     LAYOUT_PATH, LAYOUT_BAK_PATH, errno);
        } else {
            backup_ok = true;
        }
    }

    esp_err_t write_err = write_layout_bytes(LAYOUT_PATH, buf, (size_t)nr);
    if (write_err != ESP_OK) {
        unlink(LAYOUT_PATH);
        if (backup_ok && rename(LAYOUT_BAK_PATH, LAYOUT_PATH) != 0) {
            ESP_LOGE(TAG, "commit: restore layout backup failed errno=%d", errno);
        }
        unlink(tmp_path);
        layout_file_unlock();
        free(buf);
        return ESP_FAIL;
    }

    if (backup_ok)
        unlink(LAYOUT_BAK_PATH);
    unlink(tmp_path);
    layout_file_unlock();
    free(buf);
    ESP_LOGI(TAG, "Layout committed (%ld bytes)", sz);
    return ESP_OK;
}

esp_err_t canvas_board_show_queued(unsigned *out_epoch)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    if (!epd_is_ready()) {
        ESP_LOGW(TAG, "EPD not ready");
        return ESP_ERR_INVALID_STATE;
    }
    unsigned epoch = display_policy_begin_manual_display();
    if (out_epoch)
        *out_epoch = epoch;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "render already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(render_task, "canvas_render", 8192,
                    (void *)(uintptr_t)epoch, 5, NULL) != pdPASS) {
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t canvas_board_show(void)
{
    return canvas_board_show_queued(NULL);
}

void canvas_board_wait_idle(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    xSemaphoreGive(s_mutex);
}

/* ── 图标管理 ───────────────────────────────────────────────────────── */

esp_err_t canvas_board_list_builtin_icons(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 4) return ESP_ERR_INVALID_ARG;
    size_t pos = 0;
    buf[pos++] = '[';
    for (int i = 0; i < CANVAS_BUILTIN_ICON_COUNT && pos < buf_len - 4; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, buf_len - pos - 2,
                        "\"%s\"", CANVAS_BUILTIN_ICONS[i].name);
    }
    if (pos < buf_len - 2) {
        buf[pos++] = ']';
        buf[pos] = '\0';
    }
    return ESP_OK;
}

esp_err_t canvas_board_list_user_icons(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 4) return ESP_ERR_INVALID_ARG;
    size_t pos = 0;
    buf[pos++] = '[';
    DIR *d = opendir(CANVAS_ICONS_DIR);
    if (d) {
        bool first = true;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && pos < buf_len - 64) {
            /* 仅接受 *.bin */
            size_t nl = strlen(ent->d_name);
            if (nl < 5 || strcmp(ent->d_name + nl - 4, ".bin") != 0) continue;
            if (!first) buf[pos++] = ',';
            first = false;
            /* 去掉 .bin 扩展名 */
            char name[64];
            size_t nlen = nl - 4;
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            memcpy(name, ent->d_name, nlen);
            name[nlen] = '\0';
            pos += snprintf(buf + pos, buf_len - pos - 2, "\"%s\"", name);
        }
        closedir(d);
    }
    if (pos < buf_len - 2) {
        buf[pos++] = ']';
        buf[pos] = '\0';
    }
    return ESP_OK;
}

esp_err_t canvas_board_save_user_icon(const char *name,
                                       const uint8_t *data, size_t len)
{
    if (!name || !data) return ESP_ERR_INVALID_ARG;
    if (len != 32) return ESP_ERR_INVALID_SIZE;
    if (!is_valid_name(name)) return ESP_ERR_INVALID_ARG;

    ensure_icons_dir();
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.bin", CANVAS_ICONS_DIR, name);
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t w = fwrite(data, 1, 32, f);
    fclose(f);
    return (w == 32) ? ESP_OK : ESP_FAIL;
}

esp_err_t canvas_board_delete_user_icon(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    if (!is_valid_name(name)) return ESP_ERR_INVALID_ARG;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.bin", CANVAS_ICONS_DIR, name);
    return (remove(path) == 0) ? ESP_OK : ESP_FAIL;
}
