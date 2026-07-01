#include "fb_render.h"
#include "font_ext.h"
#include "font_data.h"
#include "epd.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "fb";

/* 启动早期预留：避免 httpd/WiFi/LwIP 切碎堆后单块 < plane_bytes */
static uint8_t *s_reserved_dual;
static int      s_reserved_pb;
static bool     s_reserved_in_use;
static portMUX_TYPE s_reserved_mux = portMUX_INITIALIZER_UNLOCKED;

static StaticSemaphore_t s_raw_file_mutex_buf;
static SemaphoreHandle_t s_raw_file_mutex;
static portMUX_TYPE      s_raw_file_mux = portMUX_INITIALIZER_UNLOCKED;

#define FB_FILE_WRITE_CHUNK 4096u

static uint8_t *fb_alloc_plane_storage(size_t size, const char *label)
{
    uint8_t *buf = NULL;

#ifdef CONFIG_SPIRAM
    if (size >= 4096) {
        buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf)
            return buf;
        ESP_LOGW(TAG, "%s PSRAM alloc(%u) failed", label, (unsigned)size);
    }
#endif

    buf = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (!buf)
        ESP_LOGE(TAG, "%s alloc(%u) failed: free=%lu largest=%lu internal=%lu/%lu",
                 label, (unsigned)size,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return buf;
}

static SemaphoreHandle_t raw_file_mutex_get(void)
{
    SemaphoreHandle_t m;
    portENTER_CRITICAL(&s_raw_file_mux);
    if (!s_raw_file_mutex)
        s_raw_file_mutex = xSemaphoreCreateMutexStatic(&s_raw_file_mutex_buf);
    m = s_raw_file_mutex;
    portEXIT_CRITICAL(&s_raw_file_mux);

    if (!m)
        ESP_LOGE(TAG, "raw file mutex init failed");
    return m;
}

void fb_raw_file_lock(void)
{
    SemaphoreHandle_t m = raw_file_mutex_get();
    if (m)
        xSemaphoreTake(m, portMAX_DELAY);
}

void fb_raw_file_unlock(void)
{
    SemaphoreHandle_t m = s_raw_file_mutex;
    if (m)
        xSemaphoreGive(m);
}

void fb_reserve_planes_early(void)
{
    int pb = epd_plane_bytes();
    if (pb <= 0) return;

    uint8_t *old = NULL;
    portENTER_CRITICAL(&s_reserved_mux);
    if (s_reserved_dual && s_reserved_pb == pb) {
        portEXIT_CRITICAL(&s_reserved_mux);
        return;
    }
    if (s_reserved_dual && s_reserved_in_use) {
        portEXIT_CRITICAL(&s_reserved_mux);
        ESP_LOGW(TAG, "early reserve skipped: reserved planes are in use");
        return;
    }
    old = s_reserved_dual;
    s_reserved_dual = NULL;
    s_reserved_pb   = 0;
    s_reserved_in_use = false;
    portEXIT_CRITICAL(&s_reserved_mux);
    free(old);

    uint8_t *buf = fb_alloc_plane_storage((size_t)pb * 2, "early reserve");
    if (!buf) {
        ESP_LOGW(TAG, "early reserve 2x%d failed (heap will use ad hoc alloc)", pb);
        return;
    }

    bool installed = false;
    portENTER_CRITICAL(&s_reserved_mux);
    if (!s_reserved_dual && !s_reserved_in_use) {
        s_reserved_dual = buf;
        s_reserved_pb   = pb;
        installed = true;
        buf = NULL;
    }
    portEXIT_CRITICAL(&s_reserved_mux);

    free(buf);
    if (installed) {
        ESP_LOGI(TAG, "early reserve OK 2x%d bytes (panel %dx%d)", pb,
                 epd_width(), epd_height());
    }
}

void fb_release_reserved_planes(void)
{
    uint8_t *old = NULL;
    portENTER_CRITICAL(&s_reserved_mux);
    if (s_reserved_in_use) {
        portEXIT_CRITICAL(&s_reserved_mux);
        ESP_LOGW(TAG, "release reserved while in use, keeping buffer alive");
        return;
    }
    old = s_reserved_dual;
    s_reserved_dual = NULL;
    s_reserved_pb   = 0;
    s_reserved_in_use = false;
    portEXIT_CRITICAL(&s_reserved_mux);
    free(old);
}

/* ── lifecycle ────────────────────────────────────────────────────── */

fb_t *fb_create(void)
{
    int W  = epd_width();
    int H  = epd_height();
    int pb = epd_plane_bytes();

    fb_t *fb = calloc(1, sizeof(fb_t));
    if (!fb) return NULL;

    fb->width        = W;
    fb->height       = H;
    fb->plane_bytes  = pb;
    fb->row_bytes    = W / 8;
    fb->planes_split = false;
    fb->uses_reserved = false;

    /* 优先：启动预留整块（仅单实例在用，避免双 fb 叠写） */
    uint8_t *reserved = NULL;
    portENTER_CRITICAL(&s_reserved_mux);
    if (s_reserved_dual && pb == s_reserved_pb && !s_reserved_in_use) {
        reserved = s_reserved_dual;
        s_reserved_in_use = true;
    }
    portEXIT_CRITICAL(&s_reserved_mux);
    if (reserved) {
        fb->black         = reserved;
        fb->red           = reserved + pb;
        fb->uses_reserved = true;
        fb_clear(fb);
        return fb;
    }

    /* 优先：单次大块（DMA-capable，最低碎片）。
     * 回退：单次普通堆。
     * 再回退：分别分配两块（碎片化下的最后一根稻草，
     *          epd_bulk 已经能透明处理非 DMA-capable 内存）。 */
    uint8_t *buf = fb_alloc_plane_storage((size_t)pb * 2, "fb dual plane");
    if (buf) {
        fb->black = buf;
        fb->red   = buf + pb;
    } else {
        /* 单块 2*pb 不行 → 分别申请两个 pb 大小的块，对碎片更友好 */
        uint8_t *b = fb_alloc_plane_storage((size_t)pb, "fb black plane");
        uint8_t *r = b ? fb_alloc_plane_storage((size_t)pb, "fb red plane") : NULL;
        if (!b || !r) {
            free(b);
            free(r);
            free(fb);
            ESP_LOGE(TAG, "Failed to allocate frame buffer (2x%d bytes), "
                     "free heap: %lu, largest block: %lu",
                     pb,
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            return NULL;
        }
        ESP_LOGW(TAG, "fb split planes (2x%d), free=%lu largest=%lu",
                 pb,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        fb->black        = b;
        fb->red          = r;
        fb->planes_split = true;
    }

    fb_clear(fb);
    return fb;
}

void fb_destroy(fb_t *fb)
{
    if (!fb) return;
    if (fb->uses_reserved) {
        int current_pb = epd_plane_bytes();
        uint8_t *old = NULL;
        portENTER_CRITICAL(&s_reserved_mux);
        s_reserved_in_use = false;
        if (s_reserved_dual == fb->black && s_reserved_pb != current_pb) {
            old = s_reserved_dual;
            s_reserved_dual = NULL;
            s_reserved_pb = 0;
        }
        portEXIT_CRITICAL(&s_reserved_mux);
        free(old);
        free(fb);
        return;
    }
    free(fb->black);
    if (fb->planes_split)
        free(fb->red);
    free(fb);
}

void fb_clear(fb_t *fb)
{
    memset(fb->black, 0xFF, fb->plane_bytes);
    memset(fb->red,   0x00, fb->plane_bytes);
}

/* ── primitives ───────────────────────────────────────────────────── */

void fb_pixel(fb_t *fb, int x, int y, fb_color_t c)
{
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return;
    int byte_idx = y * fb->row_bytes + x / 8;
    uint8_t mask = 0x80 >> (x % 8);

    switch (c) {
    case COLOR_RED:
        if (epd_has_red()) {
            fb->red[byte_idx] |= mask;
            fb->black[byte_idx] |= mask;
            return;
        }
        /* BW panels: red maps to black */
        __attribute__((fallthrough));
    case COLOR_BLACK:
        fb->black[byte_idx] &= ~mask;
        /* Black/red planes are exclusive: black clears any stale red bit. */
        if (epd_has_red()) fb->red[byte_idx] &= ~mask;
        break;
    default:
        fb->black[byte_idx] |= mask;
        fb->red[byte_idx] &= ~mask;
        break;
    }
}

void fb_hline(fb_t *fb, int x, int y, int w, fb_color_t c)
{
    if (y < 0 || y >= fb->height || w <= 0) return;
    int x0 = x, x1 = x + w - 1;
    if (x0 < 0) x0 = 0;
    if (x1 >= fb->width) x1 = fb->width - 1;
    if (x0 > x1) return;

    int row_off = y * fb->row_bytes;
    int sb = x0 / 8, eb = x1 / 8;

    bool has_red = epd_has_red();
    bool is_red = (c == COLOR_RED && has_red);
    bool is_black = (c == COLOR_BLACK) || (c == COLOR_RED && !has_red);

    if (sb == eb) {
        uint8_t mask = (uint8_t)((0xFF >> (x0 % 8)) & (0xFF << (7 - x1 % 8)));
        if (is_black) {
            fb->black[row_off + sb] &= ~mask;
            /* 同 fb_pixel：清掉可能残留的红位，保证黑色能盖在红色上 */
            if (has_red) fb->red[row_off + sb] &= ~mask;
        } else if (is_red) {
            fb->red[row_off + sb] |= mask;
            fb->black[row_off + sb] |= mask;
        } else {
            fb->black[row_off + sb] |= mask;
            fb->red[row_off + sb] &= ~mask;
        }
        return;
    }

    uint8_t lm = 0xFF >> (x0 % 8);
    uint8_t rm = 0xFF << (7 - x1 % 8);

    if (is_black) {
        fb->black[row_off + sb] &= ~lm;
        if (eb - sb > 1) memset(&fb->black[row_off + sb + 1], 0x00, (size_t)(eb - sb - 1));
        fb->black[row_off + eb] &= ~rm;
        /* 同上：连段红位也要清，避免黑色被红色"穿透" */
        if (has_red) {
            fb->red[row_off + sb] &= ~lm;
            if (eb - sb > 1) memset(&fb->red[row_off + sb + 1], 0x00, (size_t)(eb - sb - 1));
            fb->red[row_off + eb] &= ~rm;
        }
    } else if (is_red) {
        fb->red[row_off + sb] |= lm;   fb->black[row_off + sb] |= lm;
        if (eb - sb > 1) {
            memset(&fb->red[row_off + sb + 1],   0xFF, (size_t)(eb - sb - 1));
            memset(&fb->black[row_off + sb + 1], 0xFF, (size_t)(eb - sb - 1));
        }
        fb->red[row_off + eb] |= rm;   fb->black[row_off + eb] |= rm;
    } else {
        fb->black[row_off + sb] |= lm; fb->red[row_off + sb] &= ~lm;
        if (eb - sb > 1) {
            memset(&fb->black[row_off + sb + 1], 0xFF, (size_t)(eb - sb - 1));
            memset(&fb->red[row_off + sb + 1],   0x00, (size_t)(eb - sb - 1));
        }
        fb->black[row_off + eb] |= rm; fb->red[row_off + eb] &= ~rm;
    }
}

void fb_vline(fb_t *fb, int x, int y, int h, fb_color_t c)
{
    for (int i = 0; i < h; i++) fb_pixel(fb, x, y + i, c);
}

void fb_rect(fb_t *fb, int x, int y, int w, int h, fb_color_t c)
{
    fb_hline(fb, x, y, w, c);
    fb_hline(fb, x, y + h - 1, w, c);
    fb_vline(fb, x, y, h, c);
    fb_vline(fb, x + w - 1, y, h, c);
}

void fb_fill_rect(fb_t *fb, int x, int y, int w, int h, fb_color_t c)
{
    for (int j = 0; j < h; j++)
        fb_hline(fb, x, y + j, w, c);
}

void fb_bitmap(fb_t *fb, int x, int y, int w, int h,
               const uint8_t *data, fb_color_t c)
{
    int stride = (w + 7) / 8;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (data[j * stride + i / 8] & (0x80 >> (i % 8)))
                fb_pixel(fb, x + i, y + j, c);
        }
    }
}

/* Large numeric rendering.
 * Scaling the 8x16 bitmap font to 4x+ makes counters look blocky on e-paper.
 * These helpers draw numbers from filled geometry without adding a big font. */
enum {
    NUM7_A = 1 << 0,
    NUM7_B = 1 << 1,
    NUM7_C = 1 << 2,
    NUM7_D = 1 << 3,
    NUM7_E = 1 << 4,
    NUM7_F = 1 << 5,
    NUM7_G = 1 << 6,
};

static const uint8_t s_num7_map[10] = {
    NUM7_A | NUM7_B | NUM7_C | NUM7_D | NUM7_E | NUM7_F,
    NUM7_B | NUM7_C,
    NUM7_A | NUM7_B | NUM7_D | NUM7_E | NUM7_G,
    NUM7_A | NUM7_B | NUM7_C | NUM7_D | NUM7_G,
    NUM7_B | NUM7_C | NUM7_F | NUM7_G,
    NUM7_A | NUM7_C | NUM7_D | NUM7_F | NUM7_G,
    NUM7_A | NUM7_C | NUM7_D | NUM7_E | NUM7_F | NUM7_G,
    NUM7_A | NUM7_B | NUM7_C,
    NUM7_A | NUM7_B | NUM7_C | NUM7_D | NUM7_E | NUM7_F | NUM7_G,
    NUM7_A | NUM7_B | NUM7_C | NUM7_D | NUM7_F | NUM7_G,
};

static int num7_scale(int scale)
{
    return scale < 1 ? 1 : scale;
}

int fb_digit7_width(int scale)
{
    scale = num7_scale(scale);
    return 10 * scale;
}

int fb_number7_height(int scale)
{
    scale = num7_scale(scale);
    return 18 * scale;
}

static int num7_char_width(char ch, int scale)
{
    scale = num7_scale(scale);
    if (ch >= '0' && ch <= '9')
        return fb_digit7_width(scale);
    switch (ch) {
    case '-': return 7 * scale;
    case '.': return 3 * scale;
    case ':': return 4 * scale;
    case '/': return 8 * scale;
    case '%': return 14 * scale;
    case ' ': return 5 * scale;
    default:  return 8 * scale;
    }
}

int fb_number7_width(const char *s, int scale)
{
    if (!s)
        return 0;
    scale = num7_scale(scale);
    int w = 0;
    for (const char *p = s; *p; p++) {
        if (p != s)
            w += scale;
        w += num7_char_width(*p, scale);
    }
    return w;
}

static void num7_hseg(fb_t *fb, int x, int y, int w, int t, fb_color_t c)
{
    fb_fill_rect(fb, x, y, w, t, c);
    if (t > 2) {
        fb_pixel(fb, x - 1, y + t / 2, c);
        fb_pixel(fb, x + w, y + t / 2, c);
    }
}

static void num7_vseg(fb_t *fb, int x, int y, int t, int h, fb_color_t c)
{
    fb_fill_rect(fb, x, y, t, h, c);
    if (t > 2) {
        fb_pixel(fb, x + t / 2, y - 1, c);
        fb_pixel(fb, x + t / 2, y + h, c);
    }
}

static void draw_num7_digit(fb_t *fb, int x, int y, int digit,
                            fb_color_t c, int scale)
{
    if (!fb || digit < 0 || digit > 9)
        return;
    scale = num7_scale(scale);
    int t = scale;
    if (scale >= 5)
        t = scale + 1;

    int sx = scale;
    int sy = scale;
    int w = fb_digit7_width(scale);
    int h = fb_number7_height(scale);
    int right = x + w - t;
    int mid_y = y + h / 2 - t / 2;
    int bot_y = y + h - t;
    int v_top_y = y + 2 * sy;
    int v_bot_y = mid_y + 2 * sy;
    int v_h = mid_y - v_top_y - sy;
    int v_h2 = bot_y - v_bot_y - sy;
    if (v_h < t)
        v_h = t;
    if (v_h2 < t)
        v_h2 = t;

    uint8_t segs = s_num7_map[digit];
    if (segs & NUM7_A) num7_hseg(fb, x + 2 * sx, y,     w - 4 * sx, t, c);
    if (segs & NUM7_G) num7_hseg(fb, x + 2 * sx, mid_y, w - 4 * sx, t, c);
    if (segs & NUM7_D) num7_hseg(fb, x + 2 * sx, bot_y, w - 4 * sx, t, c);
    if (segs & NUM7_F) num7_vseg(fb, x,     v_top_y, t, v_h,  c);
    if (segs & NUM7_B) num7_vseg(fb, right, v_top_y, t, v_h,  c);
    if (segs & NUM7_E) num7_vseg(fb, x,     v_bot_y, t, v_h2, c);
    if (segs & NUM7_C) num7_vseg(fb, right, v_bot_y, t, v_h2, c);
}

static void draw_num7_symbol(fb_t *fb, int x, int y, char ch,
                             fb_color_t c, int scale)
{
    scale = num7_scale(scale);
    int h = fb_number7_height(scale);
    int t = scale;
    if (scale >= 5)
        t = scale + 1;

    switch (ch) {
    case '-':
        num7_hseg(fb, x + scale, y + h / 2 - t / 2, 5 * scale, t, c);
        break;
    case '.':
        fb_fill_rect(fb, x + scale, y + h - 2 * scale, 2 * scale, 2 * scale, c);
        break;
    case ':':
        fb_fill_rect(fb, x + scale, y + 5 * scale, 2 * scale, 2 * scale, c);
        fb_fill_rect(fb, x + scale, y + 12 * scale, 2 * scale, 2 * scale, c);
        break;
    case '/':
        for (int yy = 0; yy < h; yy++) {
            int px = x + 7 * scale - (yy * 7 * scale) / h;
            fb_fill_rect(fb, px, y + yy, t, 1, c);
        }
        break;
    case '%':
        fb_rect(fb, x, y + 2 * scale, 4 * scale, 4 * scale, c);
        fb_rect(fb, x + 9 * scale, y + 12 * scale, 4 * scale, 4 * scale, c);
        for (int yy = scale; yy < h - scale; yy++) {
            int px = x + 11 * scale - (yy * 9 * scale) / h;
            fb_fill_rect(fb, px, y + yy, t, 1, c);
        }
        break;
    default:
        fb_rect(fb, x, y + 2 * scale, 6 * scale, 12 * scale, c);
        break;
    }
}

int fb_number7(fb_t *fb, int x, int y, const char *s,
               fb_color_t c, int scale)
{
    if (!fb || !s)
        return 0;
    scale = num7_scale(scale);
    int cx = x;
    for (const char *p = s; *p; p++) {
        if (p != s)
            cx += scale;
        if (*p >= '0' && *p <= '9')
            draw_num7_digit(fb, cx, y, *p - '0', c, scale);
        else if (*p != ' ')
            draw_num7_symbol(fb, cx, y, *p, c, scale);
        cx += num7_char_width(*p, scale);
    }
    return cx - x;
}

/* ── text ─────────────────────────────────────────────────────────── */

int fb_char(fb_t *fb, int x, int y, char ch, fb_color_t c)
{
    int idx = (unsigned char)ch - 32;
    if (idx < 0 || idx >= 95) idx = 0;
    fb_bitmap(fb, x, y, 8, 16, font_ascii[idx], c);
    return 8;
}

int fb_string(fb_t *fb, int x, int y, const char *s, fb_color_t c)
{
    int cx = x;
    while (*s) {
        fb_char(fb, cx, y, *s, c);
        cx += 8;
        s++;
    }
    return cx - x;
}

static const uint8_t *find_zh_glyph(uint32_t cp)
{
    int lo = 0, hi = FONT_ZH_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_zh[mid].cp == cp) return font_zh[mid].bmp;
        if (font_zh[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static bool utf8_cont(uint8_t c)
{
    return (c & 0xC0) == 0x80;
}

static int decode_utf8(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    int cp;
    if (p[0] < 0x80) {
        cp = p[0]; *pp += 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        if (!p[1] || !utf8_cont(p[1])) {
            *pp += 1;
            return '?';
        }
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        if (cp < 0x80)
            cp = '?';
        *pp += 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        if (!p[1] || !utf8_cont(p[1]) || !p[2] || !utf8_cont(p[2])) {
            *pp += 1;
            return '?';
        }
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))
            cp = '?';
        *pp += 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        if (!p[1] || !utf8_cont(p[1]) || !p[2] || !utf8_cont(p[2]) ||
            !p[3] || !utf8_cont(p[3])) {
            *pp += 1;
            return '?';
        }
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
             ((p[2] & 0x3F) << 6)  | (p[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF)
            cp = '?';
        *pp += 4;
    } else {
        *pp += 1;
        return '?';
    }
    return cp;
}

static int normalize_compat_ascii(int cp)
{
    if (cp == 0x3000)
        return ' ';
    if (cp >= 0xFF01 && cp <= 0xFF5E)
        return cp - 0xFEE0;
    return cp;
}

static void fb_missing_glyph(fb_t *fb, int x, int y, fb_color_t c, int scale)
{
    if (scale < 1)
        scale = 1;
    int size = 16 * scale;
    fb_rect(fb, x, y, size, size, c);
    int pad = 3 * scale;
    for (int i = 0; i < size - 2 * pad; i += scale) {
        fb_pixel(fb, x + pad + i, y + pad + i, c);
        fb_pixel(fb, x + size - pad - 1 - i, y + pad + i, c);
    }
}

static bool glyph16_is_empty(const uint8_t *glyph)
{
    if (!glyph)
        return true;
    for (int i = 0; i < 32; i++) {
        if (glyph[i] != 0)
            return false;
    }
    return true;
}

int fb_utf8(fb_t *fb, int x, int y, const char *s, fb_color_t c)
{
    int cx = x;
    while (*s) {
        int cp = normalize_compat_ascii(decode_utf8(&s));
        if (cp < 0x80) {
            fb_char(fb, cx, y, (char)cp, c);
            cx += 8;
        } else {
            const uint8_t *glyph = find_zh_glyph((uint32_t)cp);
            if (!glyph16_is_empty(glyph))
                fb_bitmap(fb, cx, y, 16, 16, glyph, c);
            else
                fb_missing_glyph(fb, cx, y, c, 1);
            cx += 16;
        }
    }
    return cx - x;
}

/* ── scaled text (any integer scale 1..N) ────────────────────────── */

static void fb_bitmap_sc(fb_t *fb, int x, int y, int w, int h,
                          const uint8_t *data, fb_color_t c, int sc)
{
    int stride = (w + 7) / 8;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (data[j * stride + i / 8] & (0x80 >> (i % 8))) {
                int px = x + i * sc;
                int py = y + j * sc;
                for (int dy = 0; dy < sc; dy++)
                    for (int dx = 0; dx < sc; dx++)
                        fb_pixel(fb, px + dx, py + dy, c);
            }
        }
    }
}

static void fb_bitmap_sc_builtin_style(fb_t *fb, int x, int y, int w, int h,
                                       const uint8_t *data, fb_color_t c,
                                       int sc)
{
    int stride = (w + 7) / 8;
    int bold = (sc >= 3) ? 1 : 0;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (data[j * stride + i / 8] & (0x80 >> (i % 8))) {
                int px = x + i * sc;
                int py = y + j * sc;
                for (int dy = 0; dy < sc + bold; dy++)
                    for (int dx = 0; dx < sc + bold; dx++)
                        fb_pixel(fb, px + dx, py + dy, c);
            }
        }
    }
}

static bool fb_draw_text_glyph(fb_t *fb, int x, int y, int cp,
                               fb_color_t c, int scale, bool prefer_ext,
                               bool allow_ext, int *adv)
{
    bool use_ext = allow_ext;
    if (use_ext &&
        font_ext_draw_glyph(fb, x, y, (uint32_t)cp, c, scale, adv)) {
        return true;
    }

    if (cp < 0x80) {
        int idx = cp - 32;
        if (idx < 0 || idx >= 95) idx = 0;
        fb_bitmap_sc_builtin_style(fb, x, y, 8, 16, font_ascii[idx], c, scale);
        if (adv) *adv = 8 * scale;
        return true;
    }

    const uint8_t *glyph = find_zh_glyph((uint32_t)cp);
    if (!glyph16_is_empty(glyph))
        fb_bitmap_sc_builtin_style(fb, x, y, 16, 16, glyph, c, scale);
    else
        fb_missing_glyph(fb, x, y, c, scale);
    if (adv) *adv = 16 * scale;
    return true;
}

static int fb_text_glyph_advance_internal(int cp, int scale, bool prefer_ext,
                                          bool allow_ext)
{
    int adv = 0;
    bool use_ext = allow_ext;
    if (use_ext)
        (void)font_ext_probe_glyph((uint32_t)cp, scale, &adv);
    if (adv <= 0)
        adv = (cp < 0x80) ? (8 * scale) : (16 * scale);
    return adv;
}

static int fb_text_glyph_advance(int cp, int scale, bool prefer_ext)
{
    return fb_text_glyph_advance_internal(cp, scale, prefer_ext, true);
}

static int fb_utf8_scaled_width_internal(const char *s, int scale,
                                         bool prefer_ext)
{
    if (!s)
        return 0;
    if (scale < 1)
        scale = 1;
    int w = 0;
    while (*s) {
        int cp = normalize_compat_ascii(decode_utf8(&s));
        w += fb_text_glyph_advance(cp, scale, prefer_ext);
    }
    return w;
}

static int fb_utf8_scaled_draw_internal_ex(fb_t *fb, int x, int y,
                                           const char *s, fb_color_t c,
                                           int scale, bool prefer_ext,
                                           bool allow_ext);

static int fb_utf8_scaled_maxw_internal_ex(fb_t *fb, int x, int y,
                                           const char *s, fb_color_t c,
                                           int scale, int max_w,
                                           bool prefer_ext, bool allow_ext);

static int fb_utf8_scaled_draw_internal(fb_t *fb, int x, int y, const char *s,
                                        fb_color_t c, int scale,
                                        bool prefer_ext)
{
    return fb_utf8_scaled_draw_internal_ex(fb, x, y, s, c, scale,
                                           prefer_ext, true);
}

static int fb_utf8_scaled_draw_internal_ex(fb_t *fb, int x, int y,
                                           const char *s, fb_color_t c,
                                           int scale, bool prefer_ext,
                                           bool allow_ext)
{
    if (scale < 1)
        scale = 1;
    int cx = x;
    while (*s) {
        int cp = normalize_compat_ascii(decode_utf8(&s));
        int adv = 0;
        fb_draw_text_glyph(fb, cx, y, cp, c, scale, prefer_ext,
                           allow_ext, &adv);
        cx += adv;
    }
    return cx - x;
}

static int fb_utf8_scaled_maxw_internal(fb_t *fb, int x, int y, const char *s,
                                        fb_color_t c, int scale, int max_w,
                                        bool prefer_ext)
{
    return fb_utf8_scaled_maxw_internal_ex(fb, x, y, s, c, scale, max_w,
                                           prefer_ext, true);
}

static int fb_utf8_scaled_maxw_internal_ex(fb_t *fb, int x, int y,
                                           const char *s, fb_color_t c,
                                           int scale, int max_w,
                                           bool prefer_ext, bool allow_ext)
{
    if (scale < 1)
        scale = 1;
    if (max_w <= 0)
        return 0;
    int cx = x;
    const int lim = x + max_w;
    while (*s && cx < lim) {
        const char *before = s;
        int cp = normalize_compat_ascii(decode_utf8(&s));
        int adv = fb_text_glyph_advance_internal(cp, scale, prefer_ext,
                                                 allow_ext);
        if (cx + adv > lim) {
            s = before;
            break;
        }
        fb_draw_text_glyph(fb, cx, y, cp, c, scale, prefer_ext,
                           allow_ext, &adv);
        cx += adv;
    }
    return cx - x;
}

int fb_char_2x(fb_t *fb, int x, int y, char ch, fb_color_t c)
{
    int idx = (unsigned char)ch - 32;
    if (idx < 0 || idx >= 95) idx = 0;
    fb_bitmap_sc(fb, x, y, 8, 16, font_ascii[idx], c, 2);
    return 16;
}

int fb_utf8_2x(fb_t *fb, int x, int y, const char *s, fb_color_t c)
{
    return fb_utf8_scaled(fb, x, y, s, c, 2);
}

int fb_utf8_scaled(fb_t *fb, int x, int y, const char *s,
                   fb_color_t c, int scale)
{
    return fb_utf8_scaled_draw_internal(fb, x, y, s, c, scale, false);
}

int fb_utf8_scaled_builtin(fb_t *fb, int x, int y, const char *s,
                           fb_color_t c, int scale)
{
    return fb_utf8_scaled_draw_internal_ex(fb, x, y, s, c, scale, false, false);
}

int fb_utf8_scaled_builtin_maxw(fb_t *fb, int x, int y, const char *s,
                                fb_color_t c, int scale, int max_w)
{
    return fb_utf8_scaled_maxw_internal_ex(fb, x, y, s, c, scale, max_w,
                                           false, false);
}

int fb_utf8_scaled_width(const char *s, int scale)
{
    return fb_utf8_scaled_width_internal(s, scale, false);
}

int fb_codepoint_scaled_width(int cp, int scale, bool styled)
{
    if (scale < 1)
        scale = 1;
    cp = normalize_compat_ascii(cp);
    return fb_text_glyph_advance(cp, scale, styled);
}

int fb_utf8_scaled_styled(fb_t *fb, int x, int y, const char *s,
                          fb_color_t c, int scale)
{
    return fb_utf8_scaled_draw_internal(fb, x, y, s, c, scale, true);
}

int fb_utf8_scaled_styled_width(const char *s, int scale)
{
    return fb_utf8_scaled_width_internal(s, scale, true);
}

int fb_utf8_scaled_maxw(fb_t *fb, int x, int y, const char *s,
                        fb_color_t c, int scale, int max_w)
{
    return fb_utf8_scaled_maxw_internal(fb, x, y, s, c, scale, max_w, false);
}

int fb_utf8_scaled_styled_maxw(fb_t *fb, int x, int y, const char *s,
                               fb_color_t c, int scale, int max_w)
{
    return fb_utf8_scaled_maxw_internal(fb, x, y, s, c, scale, max_w, true);
}

static int fb_text_glyph_advance_px(int cp, int target_px)
{
    int adv = 0;
    if (target_px >= 16)
        (void)font_ext_probe_glyph_px((uint32_t)cp, target_px, &adv);
    if (adv <= 0) {
        int scale = (target_px + 15) / 16;
        if (scale < 1)
            scale = 1;
        adv = (cp < 0x80) ? (8 * scale) : (16 * scale);
    }
    return adv;
}

static bool fb_draw_text_glyph_px(fb_t *fb, int x, int y, int cp,
                                  fb_color_t c, int target_px, int *adv)
{
    if (target_px >= 16 &&
        font_ext_draw_glyph_px(fb, x, y, (uint32_t)cp, c, target_px, adv)) {
        return true;
    }

    int scale = (target_px + 15) / 16;
    if (scale < 1)
        scale = 1;
    return fb_draw_text_glyph(fb, x, y, cp, c, scale, true, true, adv);
}

int fb_utf8_px_width(const char *s, int target_px)
{
    if (!s)
        return 0;
    if (target_px < 8)
        target_px = 8;
    int w = 0;
    while (*s) {
        int cp = normalize_compat_ascii(decode_utf8(&s));
        w += fb_text_glyph_advance_px(cp, target_px);
    }
    return w;
}

int fb_utf8_px(fb_t *fb, int x, int y, const char *s,
               fb_color_t c, int target_px)
{
    if (!s)
        return 0;
    if (target_px < 8)
        target_px = 8;
    int cx = x;
    while (*s) {
        int cp = normalize_compat_ascii(decode_utf8(&s));
        int adv = 0;
        fb_draw_text_glyph_px(fb, cx, y, cp, c, target_px, &adv);
        cx += adv;
    }
    return cx - x;
}

int fb_utf8_px_maxw(fb_t *fb, int x, int y, const char *s,
                    fb_color_t c, int target_px, int max_w)
{
    if (!s || max_w <= 0)
        return 0;
    if (target_px < 8)
        target_px = 8;
    int cx = x;
    const int lim = x + max_w;
    while (*s && cx < lim) {
        const char *before = s;
        int cp = normalize_compat_ascii(decode_utf8(&s));
        int adv = fb_text_glyph_advance_px(cp, target_px);
        if (cx + adv > lim) {
            s = before;
            break;
        }
        fb_draw_text_glyph_px(fb, cx, y, cp, c, target_px, &adv);
        cx += adv;
    }
    return cx - x;
}

/* ── export ───────────────────────────────────────────────────────── */

static bool fb_write_all_chunked(FILE *f, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > FB_FILE_WRITE_CHUNK)
            n = FB_FILE_WRITE_CHUNK;
        if (fwrite(data + off, 1, n, f) != n)
            return false;
        off += n;
    }
    return true;
}

esp_err_t fb_export(const fb_t *fb, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create %s", path);
        return ESP_FAIL;
    }
    errno = 0;
    bool ok = fb_write_all_chunked(f, fb->black, (size_t)fb->plane_bytes);
    if (ok && epd_has_red())
        ok = fb_write_all_chunked(f, fb->red, (size_t)fb->plane_bytes);
    int close_ret = fclose(f);
    if (close_ret != 0)
        ok = false;
    if (!ok) {
        ESP_LOGE(TAG, "fb_export write failed: path=%s bytes=%u red=%d errno=%d",
                 path, (unsigned)fb->plane_bytes, epd_has_red() ? 1 : 0, errno);
        remove(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t fb_import(fb_t *fb, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return ESP_FAIL;
    }
    bool ok = (fread(fb->black, 1, fb->plane_bytes, f) == fb->plane_bytes);
    if (ok && epd_has_red())
        ok = (fread(fb->red, 1, fb->plane_bytes, f) == fb->plane_bytes);
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "fb_import read failed (file truncated?)");
        return ESP_FAIL;
    }
    return ESP_OK;
}
