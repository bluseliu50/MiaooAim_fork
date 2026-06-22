#include "image_convert.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "rom/tjpgd.h"
#include "lodepng.h"
#include "epd.h"

static const char *TAG = "img";

#define JPEG_FS_BAND_ROWS       16
#define PNG_MAX_FILE_BYTES      (1024u * 1024u)
#define PNG_MAX_DECODE_PIXELS   (1000u * 1000u)
#define PNG_MAX_SIDE_PIXELS     4096u
#define RAW_WRITE_CHUNK         4096u

typedef struct {
    int x;
    int y;
    int w;
    int h;
} image_fit_rect_t;

static image_fit_rect_t image_fit_contain_rect(int src_w, int src_h,
                                               int dst_w, int dst_h)
{
    image_fit_rect_t r = { 0, 0, dst_w, dst_h };
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return r;

    if ((int64_t)dst_w * src_h <= (int64_t)dst_h * src_w) {
        r.w = dst_w;
        r.h = (int)(((int64_t)dst_w * src_h + src_w / 2) / src_w);
    } else {
        r.h = dst_h;
        r.w = (int)(((int64_t)dst_h * src_w + src_h / 2) / src_h);
    }

    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    if (r.w > dst_w) r.w = dst_w;
    if (r.h > dst_h) r.h = dst_h;
    r.x = (dst_w - r.w) / 2;
    r.y = (dst_h - r.h) / 2;
    return r;
}

static void log_heap_state(const char *label)
{
    ESP_LOGI(TAG, "%s heap: free8=%lu largest8=%lu internal=%lu/%lu psram=%lu/%lu",
             label,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void *img_alloc(size_t size, const char *label)
{
    void *p = NULL;

#ifdef CONFIG_SPIRAM
    if (size >= 4096) {
        p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p)
            return p;
        ESP_LOGW(TAG, "%s PSRAM alloc(%u) failed", label, (unsigned)size);
    }
#endif

    p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGE(TAG, "%s alloc(%u) failed", label, (unsigned)size);
        log_heap_state("alloc failed");
    }
    return p;
}

static void *img_calloc(size_t count, size_t size, const char *label)
{
    if (size != 0 && count > SIZE_MAX / size) {
        ESP_LOGE(TAG, "%s calloc overflow", label);
        return NULL;
    }

    size_t total = count * size;
    void *p = img_alloc(total, label);
    if (p)
        memset(p, 0, total);
    return p;
}

static bool write_all_chunked(FILE *f, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > RAW_WRITE_CHUNK)
            n = RAW_WRITE_CHUNK;
        if (fwrite(data + off, 1, n, f) != n)
            return false;
        off += n;
    }
    return true;
}

static bool write_raw_planes(const char *out_path, const uint8_t *black,
                             const uint8_t *red, const uint8_t *yellow,
                             int plane_bytes, bool has_red, bool has_yellow)
{
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open out failed: %s", out_path);
        return false;
    }

    bool ok = write_all_chunked(f, black, (size_t)plane_bytes);
    if (ok && has_red)
        ok = write_all_chunked(f, red, (size_t)plane_bytes);
    if (ok && has_yellow)
        ok = write_all_chunked(f, yellow, (size_t)plane_bytes);
    if (fclose(f) != 0)
        ok = false;

    if (!ok) {
        ESP_LOGE(TAG, "write raw failed: %s", out_path);
        remove(out_path);
    }
    return ok;
}

static void flip_plane_rows(uint8_t *plane, int row_bytes, int height)
{
    if (!plane || row_bytes <= 0 || height <= 1)
        return;
    uint8_t *tmp = (uint8_t *)img_alloc((size_t)row_bytes, "row flip temp");
    if (!tmp) {
        ESP_LOGW(TAG, "row flip skipped: temp alloc failed");
        return;
    }
    for (int y = 0; y < height / 2; y++) {
        uint8_t *top = plane + (size_t)y * (size_t)row_bytes;
        uint8_t *bot = plane + (size_t)(height - 1 - y) * (size_t)row_bytes;
        memcpy(tmp, top, (size_t)row_bytes);
        memcpy(top, bot, (size_t)row_bytes);
        memcpy(bot, tmp, (size_t)row_bytes);
    }
    free(tmp);
}

static void compensate_panel_image_orientation(uint8_t *black, uint8_t *red,
                                               uint8_t *yellow, int plane_bytes,
                                               bool has_red, bool has_yellow)
{
    if (epd_get_panel() != EPD_PANEL_HINK_SSD1619_29_BWR)
        return;

    int w = epd_width();
    int h = epd_height();
    int row_bytes = w / 8;
    if (w <= 0 || h <= 0 || row_bytes <= 0 || plane_bytes != row_bytes * h)
        return;

    flip_plane_rows(black, row_bytes, h);
    if (has_red)
        flip_plane_rows(red, row_bytes, h);
    if (has_yellow)
        flip_plane_rows(yellow, row_bytes, h);
    ESP_LOGI(TAG, "HINK 2.9 image orientation compensation: flip Y");
}

/* ── Bayer 8x8 matrix (fallback when F-S allocation fails) ────────── */

static const uint8_t bayer8[8][8] = {
    {   0, 192,  48, 240,  12, 204,  60, 252 },
    { 128,  64, 176, 112, 140,  76, 188, 124 },
    {  32, 224,  16, 208,  44, 236,  28, 220 },
    { 160,  96, 144,  80, 172, 108, 156,  92 },
    {   8, 200,  56, 248,   4, 196,  52, 244 },
    { 136,  72, 184, 120, 132,  68, 180, 116 },
    {  40, 232,  24, 216,  36, 228,  20, 212 },
    { 168, 104, 152,  84, 164, 100, 148,  88 },
};

static bool red_orange_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    int ri = (int)r, gi = (int)g, bi = (int)b;
    int max_c = ri;
    if (gi > max_c) max_c = gi;
    if (bi > max_c) max_c = bi;
    int min_c = ri;
    if (gi < min_c) min_c = gi;
    if (bi < min_c) min_c = bi;
    int chroma = max_c - min_c;
    if (max_c < 55 || chroma < 22)
        return false;

    int hue = 0;
    if (max_c == ri)
        hue = ((gi - bi) * 60) / chroma;
    else if (max_c == gi)
        hue = 120 + ((bi - ri) * 60) / chroma;
    else
        hue = 240 + ((ri - gi) * 60) / chroma;
    if (hue < 0)
        hue += 360;

    int sat256 = (chroma * 256) / max_c;
    int lum = (ri * 77 + gi * 150 + bi * 29) >> 8;
    bool red_hue = (hue <= 55 || hue >= 305);
    bool orange_hue = (hue <= 72 && ri >= gi + 24 && ri >= bi + 34 &&
                       gi * 100 <= ri * 78 && bi * 100 <= ri * 74);
    bool red_dominant = (ri >= gi + 18) && (ri >= bi + 18);

    return (red_hue || orange_hue) && red_dominant &&
           sat256 >= 55 && lum >= 35 && lum <= 225;
}

static uint8_t red_ink_strength(uint8_t r, uint8_t g, uint8_t b)
{
    int ri = (int)r, gi = (int)g, bi = (int)b;
    int max_c = ri;
    if (gi > max_c) max_c = gi;
    if (bi > max_c) max_c = bi;
    int min_c = ri;
    if (gi < min_c) min_c = gi;
    if (bi < min_c) min_c = bi;
    int chroma = max_c - min_c;
    if (max_c < 55 || chroma < 22 || ri < gi + 14 || ri < bi + 18)
        return 0;

    int hue = 0;
    if (max_c == ri)
        hue = ((gi - bi) * 60) / chroma;
    else if (max_c == gi)
        hue = 120 + ((bi - ri) * 60) / chroma;
    else
        hue = 240 + ((ri - gi) * 60) / chroma;
    if (hue < 0)
        hue += 360;
    if (!(hue <= 72 || hue >= 305))
        return 0;

    int sat256 = (chroma * 256) / max_c;
    int lum = (ri * 77 + gi * 150 + bi * 29) >> 8;
    int dom = ri - (gi > bi ? gi : bi);
    int strength = sat256 + dom * 3 / 2 + lum - 210;

    if (hue > 45 && hue < 90)
        strength -= (hue - 45) * 4;
    if (lum < 70)
        strength -= (70 - lum);
    if (lum > 205)
        strength -= (lum - 205) * 2;

    if (strength < 0)
        return 0;
    if (strength > 245)
        return 245;
    return (uint8_t)strength;
}

static uint8_t yellow_ink_strength(uint8_t r, uint8_t g, uint8_t b)
{
    int ri = (int)r, gi = (int)g, bi = (int)b;
    int max_c = ri;
    if (gi > max_c) max_c = gi;
    if (bi > max_c) max_c = bi;
    int min_c = ri;
    if (gi < min_c) min_c = gi;
    if (bi < min_c) min_c = bi;
    int chroma = max_c - min_c;
    if (max_c < 70 || chroma < 28)
        return 0;

    int hue = 0;
    if (max_c == ri)
        hue = ((gi - bi) * 60) / chroma;
    else if (max_c == gi)
        hue = 120 + ((bi - ri) * 60) / chroma;
    else
        hue = 240 + ((ri - gi) * 60) / chroma;
    if (hue < 0)
        hue += 360;

    bool yellow_hue = (hue >= 38 && hue <= 92);
    bool yellow_channels = ri >= 95 && gi >= 85 &&
                           bi + 24 <= ri && bi + 18 <= gi &&
                           ri + 80 >= gi && gi + 80 >= ri;
    if (!yellow_hue || !yellow_channels)
        return 0;

    int sat256 = (chroma * 256) / max_c;
    int lum = (ri * 77 + gi * 150 + bi * 29) >> 8;
    int yellow_base = ((ri < gi) ? ri : gi) - bi;
    int strength = sat256 + yellow_base + lum - 245;
    if (hue > 78)
        strength -= (hue - 78) * 3; /* avoid green drifting into yellow */
    if (hue < 45)
        strength -= (45 - hue) * 2; /* leave orange/red-orange to red plane */
    if (lum < 80)
        strength -= (80 - lum);

    if (strength < 0)
        return 0;
    if (strength > 245)
        return 245;
    return (uint8_t)strength;
}

/* ── Bayer dithered pixel write (fallback) ────────────────────────── */

static inline void dither_pixel_bayer(uint8_t *black, uint8_t *red,
                                      uint8_t *yellow,
                                      int x, int y, int w,
                                      uint8_t r, uint8_t g, uint8_t b,
                                      bool has_red, bool has_yellow)
{
    size_t bp = (size_t)y * (w / 8) + (size_t)(x >> 3);
    uint8_t bit = (uint8_t)(0x80u >> (x & 7));
    uint8_t threshold = bayer8[y & 7][x & 7];

    int ri = r, gi = g, bi = b;
    int max_c = ri; if (gi > max_c) max_c = gi; if (bi > max_c) max_c = bi;
    int min_c = ri; if (gi < min_c) min_c = gi; if (bi < min_c) min_c = bi;
    int chroma = max_c - min_c;
    uint8_t lum = (uint8_t)((ri * 77u + gi * 150u + bi * 29u) >> 8);

    int yellow_strength = has_yellow ? yellow_ink_strength(r, g, b) : 0;
    if (yellow_strength > (int)threshold) {
        yellow[bp] |= bit;
        black[bp] |= bit;
        return;
    }

    int red_strength = has_red ? red_ink_strength(r, g, b) : 0;
    if (red_strength > (int)threshold) {
        red[bp] |= bit;
        black[bp] |= bit;
        return;
    }

    if (chroma >= 30 && max_c >= 40) {
        int hue6 = 0;
        if (max_c == ri)      hue6 = ((gi - bi) * 60) / chroma;
        else if (max_c == gi) hue6 = 120 + ((bi - ri) * 60) / chroma;
        else                  hue6 = 240 + ((ri - gi) * 60) / chroma;
        if (hue6 < 0) hue6 += 360;
        int sat256 = (chroma * 256) / max_c;
        if ((hue6 <= 30 || hue6 >= 330) && sat256 > 80) {
            int red_str = (sat256 * max_c) >> 8;
            if (has_red && red_str > (int)threshold) {
                red[bp] |= bit;
                black[bp] |= bit;
            } else if (lum < threshold) {
                black[bp] &= ~bit;
            }
            return;
        }
    }

    int bw_threshold = (threshold > 40) ? ((int)threshold - 40) : 0;
    if ((int)lum < bw_threshold)
        black[bp] &= ~bit;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Red-first dual-plane dithering.
 *
 *  Common 3-color e-paper examples treat black and red as two independent
 *  1-bit planes.  Classify red/orange/brown pixels before black-white
 *  dithering so dark red backgrounds do not collapse into black.
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct { int16_t r, g, b; } fs_err_t;

typedef enum {
    PQ_WHITE = 0,
    PQ_BLACK,
    PQ_RED,
    PQ_YELLOW,
    PQ_OTHER,
} prequantized_color_t;

static void rgba_on_white(uint8_t *r, uint8_t *g, uint8_t *b, uint8_t a)
{
    if (a >= 255)
        return;
    *r = (uint8_t)(((unsigned)*r * a + 255u * (255u - a) + 127u) / 255u);
    *g = (uint8_t)(((unsigned)*g * a + 255u * (255u - a) + 127u) / 255u);
    *b = (uint8_t)(((unsigned)*b * a + 255u * (255u - a) + 127u) / 255u);
}

static prequantized_color_t prequantized_pixel_class(uint8_t r, uint8_t g,
                                                     uint8_t b, uint8_t a,
                                                     bool has_red,
                                                     bool has_yellow)
{
    rgba_on_white(&r, &g, &b, a);

    if (r >= 235 && g >= 235 && b >= 235)
        return PQ_WHITE;
    if (r <= 36 && g <= 36 && b <= 36)
        return PQ_BLACK;

    bool red_like = r >= 140 && g <= 110 && b <= 110 &&
                    r >= g + 45 && r >= b + 45;
    if (red_like)
        return has_red ? PQ_RED : PQ_BLACK;

    bool yellow_like = r >= 150 && g >= 120 && b <= 110 &&
                       r >= b + 45 && g >= b + 35;
    if (yellow_like)
        return has_yellow ? PQ_YELLOW : PQ_BLACK;

    return PQ_OTHER;
}

static bool bw_or_color_quantized_pixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                        bool has_red, bool has_yellow,
                                        prequantized_color_t *out)
{
    prequantized_color_t cls = prequantized_pixel_class(r, g, b, a,
                                                        has_red, has_yellow);
    if (cls != PQ_OTHER) {
        if (out)
            *out = cls;
        return true;
    }

    rgba_on_white(&r, &g, &b, a);
    int max_c = r;
    if (g > max_c) max_c = g;
    if (b > max_c) max_c = b;
    int min_c = r;
    if (g < min_c) min_c = g;
    if (b < min_c) min_c = b;
    if (max_c - min_c > 12)
        return false;

    int lum = ((int)r * 77 + (int)g * 150 + (int)b * 29) >> 8;
    if (lum <= 72) {
        if (out)
            *out = PQ_BLACK;
        return true;
    }
    if (lum >= 183) {
        if (out)
            *out = PQ_WHITE;
        return true;
    }
    return false;
}

static esp_err_t png_try_prequantized_passthrough(const unsigned char *rgba,
                                                  unsigned pw, unsigned ph,
                                                  const char *out_path)
{
    const int EW = epd_width();
    const int EH = epd_height();
    const int EC = EW * EH / 8;
    const int RB = EW / 8;
    bool has_red = epd_has_red();
    bool has_yellow = epd_has_yellow();

    if (!rgba || !out_path || pw != (unsigned)EW || ph != (unsigned)EH)
        return ESP_ERR_NOT_SUPPORTED;

    unsigned black_count = 0;
    unsigned red_count = 0;
    unsigned yellow_count = 0;
    unsigned white_count = 0;
    for (unsigned i = 0; i < pw * ph; i++) {
        const uint8_t *p = rgba + (size_t)i * 4u;
        prequantized_color_t cls;
        if (!bw_or_color_quantized_pixel(p[0], p[1], p[2], p[3],
                                         has_red, has_yellow, &cls))
            return ESP_ERR_NOT_SUPPORTED;
        switch (cls) {
        case PQ_WHITE:  white_count++; break;
        case PQ_BLACK:  black_count++; break;
        case PQ_RED:    red_count++; break;
        case PQ_YELLOW: yellow_count++; break;
        case PQ_OTHER:  return ESP_ERR_NOT_SUPPORTED;
        }
    }

    uint8_t *black = (uint8_t *)img_alloc(EC, "png passthrough black");
    uint8_t *red = has_red ? (uint8_t *)img_alloc(EC, "png passthrough red") : NULL;
    uint8_t *yellow = has_yellow ? (uint8_t *)img_alloc(EC, "png passthrough yellow") : NULL;
    if (!black || (has_red && !red) || (has_yellow && !yellow)) {
        free(black);
        free(red);
        free(yellow);
        return ESP_ERR_NO_MEM;
    }
    memset(black, 0xFF, EC);
    if (red)
        memset(red, 0x00, EC);
    if (yellow)
        memset(yellow, 0x00, EC);

    for (int y = 0; y < EH; y++) {
        for (int x = 0; x < EW; x++) {
            const uint8_t *p = rgba + ((size_t)y * (size_t)EW + (size_t)x) * 4u;
            size_t bp = (size_t)y * (size_t)RB + (size_t)(x >> 3);
            uint8_t bit = (uint8_t)(0x80u >> (x & 7));
            prequantized_color_t cls = PQ_WHITE;
            (void)bw_or_color_quantized_pixel(p[0], p[1], p[2], p[3],
                                              has_red, has_yellow, &cls);
            switch (cls) {
            case PQ_BLACK:
                black[bp] &= (uint8_t)~bit;
                break;
            case PQ_RED:
                red[bp] |= bit;
                black[bp] |= bit;
                break;
            case PQ_YELLOW:
                yellow[bp] |= bit;
                black[bp] |= bit;
                break;
            case PQ_WHITE:
            case PQ_OTHER:
                break;
            default:
                break;
            }
        }
    }

    compensate_panel_image_orientation(black, red, yellow, EC, has_red, has_yellow);
    if (!write_raw_planes(out_path, black, red, yellow, EC, has_red, has_yellow)) {
        free(black);
        free(red);
        free(yellow);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "PNG prequantized passthrough: %ux%u -> %s (%u bytes/plane, white=%u black=%u red=%u yellow=%u)",
             pw, ph, out_path, (unsigned)EC, white_count, black_count,
             red_count, yellow_count);
    free(black);
    free(red);
    free(yellow);
    return ESP_OK;
}

static void sample_rgba_on_white(const unsigned char *rgba, unsigned pw,
                                 unsigned ph, int sx, int sy,
                                 uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= (int)pw) sx = (int)pw - 1;
    if (sy >= (int)ph) sy = (int)ph - 1;
    const uint8_t *p = rgba + ((size_t)sy * pw + (size_t)sx) * 4u;
    uint8_t a = p[3];
    if (a < 255) {
        *r = (uint8_t)(((unsigned)p[0] * a + 255u * (255u - a) + 127u) / 255u);
        *g = (uint8_t)(((unsigned)p[1] * a + 255u * (255u - a) + 127u) / 255u);
        *b = (uint8_t)(((unsigned)p[2] * a + 255u * (255u - a) + 127u) / 255u);
    } else {
        *r = p[0];
        *g = p[1];
        *b = p[2];
    }
}

static bool png_looks_bw_preprocessed(const unsigned char *rgba,
                                      unsigned pw, unsigned ph)
{
    if (!rgba || pw == 0 || ph == 0)
        return false;

    unsigned total = pw * ph;
    unsigned step = total / 4096u;
    if (step < 1)
        step = 1;

    unsigned samples = 0;
    unsigned bw_like = 0;
    unsigned chroma_like = 0;
    for (unsigned i = 0; i < total; i += step) {
        const uint8_t *p = rgba + (size_t)i * 4u;
        uint8_t r, g, b;
        uint8_t a = p[3];
        if (a < 255) {
            r = (uint8_t)(((unsigned)p[0] * a + 255u * (255u - a) + 127u) / 255u);
            g = (uint8_t)(((unsigned)p[1] * a + 255u * (255u - a) + 127u) / 255u);
            b = (uint8_t)(((unsigned)p[2] * a + 255u * (255u - a) + 127u) / 255u);
        } else {
            r = p[0]; g = p[1]; b = p[2];
        }
        int max_c = r;
        if (g > max_c) max_c = g;
        if (b > max_c) max_c = b;
        int min_c = r;
        if (g < min_c) min_c = g;
        if (b < min_c) min_c = b;
        int lum = ((int)r * 77 + (int)g * 150 + (int)b * 29) >> 8;
        if (max_c - min_c <= 18)
            chroma_like++;
        if (lum <= 48 || lum >= 207)
            bw_like++;
        samples++;
    }

    if (samples == 0)
        return false;
    return chroma_like * 100u >= samples * 96u &&
           bw_like * 100u >= samples * 92u;
}

static esp_err_t png_threshold_bw_passthrough(const unsigned char *rgba,
                                              unsigned pw, unsigned ph,
                                              const char *out_path)
{
    const int EW = epd_width();
    const int EH = epd_height();
    const int EC = EW * EH / 8;
    const int RB = EW / 8;
    bool has_red = epd_has_red();
    bool has_yellow = epd_has_yellow();

    if (!png_looks_bw_preprocessed(rgba, pw, ph))
        return ESP_ERR_NOT_SUPPORTED;

    uint8_t *black = (uint8_t *)img_alloc(EC, "png bw black");
    uint8_t *red = has_red ? (uint8_t *)img_alloc(EC, "png bw red clear") : NULL;
    uint8_t *yellow = has_yellow ? (uint8_t *)img_alloc(EC, "png bw yellow clear") : NULL;
    if (!black || (has_red && !red) || (has_yellow && !yellow)) {
        free(black);
        free(red);
        free(yellow);
        return ESP_ERR_NO_MEM;
    }

    memset(black, 0xFF, EC);
    if (red)
        memset(red, 0x00, EC);
    if (yellow)
        memset(yellow, 0x00, EC);

    image_fit_rect_t fit = image_fit_contain_rect((int)pw, (int)ph, EW, EH);
    for (int ey = fit.y; ey < fit.y + fit.h; ey++) {
        int sy = (ey - fit.y) * (int)ph / fit.h;
        for (int ex = fit.x; ex < fit.x + fit.w; ex++) {
            int sx = (ex - fit.x) * (int)pw / fit.w;
            uint8_t r, g, b;
            sample_rgba_on_white(rgba, pw, ph, sx, sy, &r, &g, &b);
            int lum = ((int)r * 77 + (int)g * 150 + (int)b * 29) >> 8;
            if (lum < 128) {
                size_t bp = (size_t)ey * (size_t)RB + (size_t)(ex >> 3);
                uint8_t bit = (uint8_t)(0x80u >> (ex & 7));
                black[bp] &= (uint8_t)~bit;
            }
        }
    }

    compensate_panel_image_orientation(black, red, yellow, EC, has_red, has_yellow);
    if (!write_raw_planes(out_path, black, red, yellow, EC, has_red, has_yellow)) {
        free(black);
        free(red);
        free(yellow);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG,
             "PNG BW threshold passthrough: %ux%u -> contain %dx%d at %d,%d (%u bytes/plane)",
             pw, ph, fit.w, fit.h, fit.x, fit.y, (unsigned)EC);
    free(black);
    free(red);
    free(yellow);
    return ESP_OK;
}

/**
 * 允许映射红墨的像素：在拦住真灰（chroma 过低）的前提下尽量放宽。
 * - 色相扇区 + 饱和度
 * - 或 R 为最大通道且明显强于 G/B（暗红、棕红、部分肤色旁物体）
 */
static bool fs_red_eligible(uint8_t r, uint8_t g, uint8_t b)
{
    int ri = (int)r, gi = (int)g, bi = (int)b;
    int max_c = ri;
    if (gi > max_c) max_c = gi;
    if (bi > max_c) max_c = bi;
    int min_c = ri;
    if (gi < min_c) min_c = gi;
    if (bi < min_c) min_c = bi;
    int chroma = max_c - min_c;
    if (chroma < 20 || max_c < 40)
        return false;

    int hue6 = 0;
    if (max_c == ri)
        hue6 = ((gi - bi) * 60) / chroma;
    else if (max_c == gi)
        hue6 = 120 + ((bi - ri) * 60) / chroma;
    else
        hue6 = 240 + ((ri - gi) * 60) / chroma;
    if (hue6 < 0)
        hue6 += 360;

    int sat256 = (chroma * 256) / (max_c > 0 ? max_c : 1);
    bool red_hue = (hue6 <= 55 || hue6 >= 305);
    bool orange_hue = hue6 <= 72 && gi * 100 <= ri * 78 && bi * 100 <= ri * 74;
    bool hue_sat = (red_hue || orange_hue) && sat256 > 55 &&
                   ri >= gi + 16 && ri >= bi + 20;

    bool r_led = (max_c == ri) && ri >= 55 &&
                 ri >= gi + 20 && ri >= bi + 28 &&
                 (gi + bi) * 2 <= ri * 3 && chroma >= 24;

    return hue_sat || r_led;
}

#define BW_LUMA_THRESHOLD 96

static void fs_dither_row(const uint8_t *rgb, fs_err_t *ec, fs_err_t *en,
                          uint8_t *blk, uint8_t *red_plane,
                          uint8_t *yellow_plane,
                          int y, int w, bool has_red, bool has_yellow)
{
    int rb = w >> 3;
    for (int x = 0; x < w; x++) {
        int ex = x + 1;
        uint8_t r0 = rgb[x * 3];
        uint8_t g0 = rgb[x * 3 + 1];
        uint8_t b0 = rgb[x * 3 + 2];
        int lum = ((int)r0 * 77 + (int)g0 * 150 + (int)b0 * 29) >> 8;
        int yv = lum + ec[ex].r;
        if (yv < 0) yv = 0;
        if (yv > 255) yv = 255;

        size_t bp = (size_t)y * rb + (x >> 3);
        uint8_t bit = 0x80u >> (x & 7);

        int yellow_strength = has_yellow ? yellow_ink_strength(r0, g0, b0) : 0;
        if (yellow_strength > 0 &&
            yellow_strength > (int)bayer8[y & 7][x & 7]) {
            yellow_plane[bp] |= bit;
            blk[bp] |= bit;
            continue;
        }

        int red_strength = has_red ? red_ink_strength(r0, g0, b0) : 0;
        if (red_strength > 0 && (red_orange_pixel(r0, g0, b0) || fs_red_eligible(r0, g0, b0)) &&
            red_strength > (int)bayer8[y & 7][x & 7]) {
            red_plane[bp] |= bit;
            blk[bp] |= bit;
            continue;
        }

        int out = 255;
        if (yv < BW_LUMA_THRESHOLD) {
            blk[bp] &= ~bit;
            if (has_red)
                red_plane[bp] &= ~bit;
            if (has_yellow)
                yellow_plane[bp] &= ~bit;
            out = 0;
        }

        int16_t err = (int16_t)(yv - out);
        ec[ex + 1].r += err * 7 / 16;
        en[ex - 1].r += err * 3 / 16;
        en[ex].r     += err * 5 / 16;
        en[ex + 1].r += err * 1 / 16;
    }
}

/* ── JPEG decode context ──────────────────────────────────────────── */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} jpeg_mem_t;

typedef struct {
    uint8_t *black;
    uint8_t *red;
    uint8_t *yellow;
    int w, h;               /* EPD screen size */
    int sw, sh;             /* decoded image size */
    image_fit_rect_t fit;   /* target area for aspect-ratio preserving display */
    /* F-S band state */
    uint8_t  *band;         /* source-resolution band: band_w × band_h × 3 */
    uint8_t  *rgb_row;      /* screen-width row for F-S sampling */
    int       band_w;       /* = sw (decoded width) */
    int       band_h;       /* allocated band height */
    int       band_top;     /* source Y of current band (-1 = none) */
    int       band_rows;    /* actual rows in current band */
    fs_err_t *ec, *en;
    bool      has_red;
    bool      has_yellow;
} epd_out_t;

typedef struct {
    jpeg_mem_t mem;
    FILE *fp;
    epd_out_t out;
} jpeg_ctx_t;

/* ── JPEG input callbacks ─────────────────────────────────────────── */

static UINT jpeg_infunc_mem(JDEC *jd, BYTE *buf, UINT len)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    if (!ctx) return 0;
    jpeg_mem_t *m = &ctx->mem;
    size_t remain = (m->pos < m->len) ? (m->len - m->pos) : 0;
    if (len > remain) len = (UINT)remain;
    if (buf) memcpy(buf, m->data + m->pos, len);
    m->pos += len;
    return len;
}

static UINT jpeg_infunc_file(JDEC *jd, BYTE *buf, UINT len)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    if (!ctx || !ctx->fp) return 0;
    if (buf) return (UINT)fread(buf, 1, len, ctx->fp);
    if (fseek(ctx->fp, (long)len, SEEK_CUR) == 0) return len;
    return 0;
}

/* ── JPEG output: Bayer (stretch-to-fill) ─────────────────────────── */

static UINT jpeg_outfunc_bayer(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    if (!ctx || !bitmap || !rect) return 0;
    epd_out_t *o = &ctx->out;

    const uint8_t *px = (const uint8_t *)bitmap;
    int bw = rect->right  - rect->left + 1;
    int bh = rect->bottom - rect->top  + 1;

    for (int y = 0; y < bh; y++) {
        int src_y = (int)rect->top + y;
        int fit_bottom = o->fit.y + o->fit.h;
        int ey0 = o->fit.y + src_y * o->fit.h / o->sh;
        int ey1 = o->fit.y + (src_y + 1) * o->fit.h / o->sh;
        if (ey0 >= fit_bottom) { px += (size_t)bw * 3; continue; }
        if (ey1 > fit_bottom) ey1 = fit_bottom;
        for (int x = 0; x < bw; x++) {
            int src_x = (int)rect->left + x;
            int fit_right = o->fit.x + o->fit.w;
            int ex0 = o->fit.x + src_x * o->fit.w / o->sw;
            int ex1 = o->fit.x + (src_x + 1) * o->fit.w / o->sw;
            uint8_t r = px[0], g = px[1], b = px[2];
            px += 3;
            if (ex0 >= fit_right) continue;
            if (ex1 > fit_right) ex1 = fit_right;
            for (int ey = ey0; ey < ey1; ey++)
                for (int ex = ex0; ex < ex1; ex++)
                    dither_pixel_bayer(o->black, o->red, o->yellow, ex, ey,
                                       o->w, r, g, b,
                                       o->has_red, o->has_yellow);
        }
    }
    return 1;
}

/* ── JPEG output: red-first F-S (band-buffered, stretch) ──────────── */

static void fs_flush_band(epd_out_t *o)
{
    int src_y0 = o->band_top;
    int src_y1 = src_y0 + o->band_rows;
    int fit_bottom = o->fit.y + o->fit.h;
    int fit_right = o->fit.x + o->fit.w;
    int ey_start = o->fit.y + (src_y0 * o->fit.h + o->sh - 1) / o->sh;
    int ey_end = (src_y1 < o->sh)
               ? o->fit.y + (src_y1 * o->fit.h + o->sh - 1) / o->sh
               : fit_bottom;
    if (ey_start < o->fit.y) ey_start = o->fit.y;
    if (ey_end > fit_bottom) ey_end = fit_bottom;

    for (int ey = ey_start; ey < ey_end; ey++) {
        if (ey < o->fit.y || ey >= fit_bottom) continue;
        int sy = (ey - o->fit.y) * o->sh / o->fit.h;
        int by = sy - src_y0;
        if (by < 0 || by >= o->band_rows) continue;

        for (int ex = 0; ex < o->w; ex++) {
            if (ex < o->fit.x || ex >= fit_right) {
                o->rgb_row[ex * 3]     = 255;
                o->rgb_row[ex * 3 + 1] = 255;
                o->rgb_row[ex * 3 + 2] = 255;
                continue;
            }
            int sx = (ex - o->fit.x) * o->sw / o->fit.w;
            if (sx >= o->sw) sx = o->sw - 1;
            int si = (by * o->band_w + sx) * 3;
            o->rgb_row[ex * 3]     = o->band[si];
            o->rgb_row[ex * 3 + 1] = o->band[si + 1];
            o->rgb_row[ex * 3 + 2] = o->band[si + 2];
        }

        memset(o->en, 0, ((size_t)o->w + 2) * sizeof(fs_err_t));
        fs_dither_row(o->rgb_row, o->ec, o->en, o->black, o->red, o->yellow,
                      ey, o->w, o->has_red, o->has_yellow);
        fs_err_t *tmp = o->ec; o->ec = o->en; o->en = tmp;
    }
}

static UINT jpeg_outfunc_fs(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    if (!ctx || !bitmap || !rect) return 0;
    epd_out_t *o = &ctx->out;
    const uint8_t *px = (const uint8_t *)bitmap;

    int bw  = (int)(rect->right  - rect->left + 1);
    int bh  = (int)(rect->bottom - rect->top  + 1);
    int top = (int)rect->top;

    if (bw <= 0 || bh <= 0 || bh > o->band_h) {
        ESP_LOGE(TAG, "JPEG block too large for band: %dx%d (band_h=%d)",
                 bw, bh, o->band_h);
        return 0;
    }

    if (o->band_top < 0) {
        o->band_top  = top;
        o->band_rows = bh;
    } else if (top != o->band_top) {
        fs_flush_band(o);
        o->band_top  = top;
        o->band_rows = bh;
        memset(o->band, 255, (size_t)o->band_h * o->band_w * 3);
    }

    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            int src_x = (int)rect->left + x;
            uint8_t r = px[0], g = px[1], b = px[2];
            px += 3;
            if (src_x < 0 || src_x >= o->sw) continue;
            int idx = (y * o->band_w + src_x) * 3;
            o->band[idx] = r; o->band[idx + 1] = g; o->band[idx + 2] = b;
        }
    }
    return 1;
}

/* ── JPEG scale selection (stretch-to-fill) ───────────────────────── */

static uint8_t choose_scale(uint16_t in_w, uint16_t in_h,
                             int target_w, int target_h,
                             int *out_w, int *out_h)
{
    /*
     * For contain-fit conversion, decode close to the panel pixel count while
     * avoiding tiny intermediate images that would be heavily upscaled.
     */
    uint8_t best_s = 0;
    int best_sw = (int)in_w, best_sh = (int)in_h;
    int32_t best_cost = INT32_MAX;

    for (uint8_t s = 0; s <= 3; s++) {
        int sw = (int)((in_w + ((1u << s) - 1u)) >> s);
        int sh = (int)((in_h + ((1u << s) - 1u)) >> s);
        image_fit_rect_t fit = image_fit_contain_rect(sw, sh, target_w, target_h);
        int32_t dx = fit.w - target_w;
        int32_t dy = fit.h - target_h;
        int32_t cost = dx * dx + dy * dy;
        if (fit.w > sw || fit.h > sh)
            cost = cost * 3 + 100000;
        if (cost < best_cost || (cost == best_cost && s > best_s)) {
            best_cost = cost;
            best_s  = s;
            best_sw = sw;
            best_sh = sh;
        }
    }

    if (out_w) *out_w = best_sw;
    if (out_h) *out_h = best_sh;
    return best_s;
}

/* ── JPEG common decode ───────────────────────────────────────────── */

static esp_err_t jpeg_decode_common(jpeg_ctx_t *ctx, UINT (*infunc)(JDEC*,BYTE*,UINT),
                                     const char *out_path)
{
    const int EW = epd_width();
    const int EH = epd_height();
    const int EC = EW * EH / 8;

    static const UINT POOL_SIZE = 20 * 1024;
    log_heap_state("jpeg begin");

    void *pool = img_alloc(POOL_SIZE, "jpeg pool");
    if (!pool) { ESP_LOGE(TAG, "pool alloc failed"); return ESP_ERR_NO_MEM; }

    JDEC jd = {0};
    jd.device = ctx;
    JRESULT r = jd_prepare(&jd, infunc, pool, POOL_SIZE, ctx);
    if (r != JDR_OK) {
        ESP_LOGE(TAG, "jd_prepare failed: %d", (int)r);
        free(pool);
        return ESP_FAIL;
    }
    if (jd.width == 0 || jd.height == 0) {
        ESP_LOGE(TAG, "invalid JPEG dimensions: %ux%u",
                 (unsigned)jd.width, (unsigned)jd.height);
        free(pool);
        return ESP_ERR_INVALID_SIZE;
    }

    bool has_red = epd_has_red();
    bool has_yellow = epd_has_yellow();
    uint8_t *black = (uint8_t *)img_alloc(EC, "jpeg black plane");
    uint8_t *red   = has_red ? (uint8_t *)img_alloc(EC, "jpeg red plane") : NULL;
    uint8_t *yellow = has_yellow ? (uint8_t *)img_alloc(EC, "jpeg yellow plane") : NULL;
    if (!black || (has_red && !red) || (has_yellow && !yellow)) {
        ESP_LOGE(TAG, "jpeg plane alloc failed: plane=%u bytes, panel=%dx%d, red=%d yellow=%d",
                 (unsigned)EC, EW, EH, has_red ? 1 : 0, has_yellow ? 1 : 0);
        free(pool); free(black); free(red); free(yellow);
        return ESP_ERR_NO_MEM;
    }
    memset(black, 0xFF, EC);
    if (red)
        memset(red,   0x00, EC);
    if (yellow)
        memset(yellow, 0x00, EC);

    int decoded_w = 0, decoded_h = 0;
    uint8_t scale = choose_scale(jd.width, jd.height, EW, EH,
                                 &decoded_w, &decoded_h);

    image_fit_rect_t fit = image_fit_contain_rect(decoded_w, decoded_h, EW, EH);
    ESP_LOGI(TAG, "jpeg %ux%u -> 1/%u => %dx%d, contain %dx%d at %d,%d on %dx%d",
             (unsigned)jd.width, (unsigned)jd.height,
             (unsigned)(1u << scale), decoded_w, decoded_h,
             fit.w, fit.h, fit.x, fit.y, EW, EH);

    ctx->out = (epd_out_t){
        .black = black, .red = red, .yellow = yellow,
        .w = EW, .h = EH,
        .sw = decoded_w, .sh = decoded_h,
        .fit = fit,
        .has_red = has_red,
        .has_yellow = has_yellow
    };

    /* Try color-first Floyd-Steinberg; fall back to Bayer if OOM */
    int bh = JPEG_FS_BAND_ROWS;
    size_t bsz = (size_t)bh * decoded_w * 3;
    ctx->out.band    = (uint8_t *)img_alloc(bsz, "jpeg fs band");
    ctx->out.rgb_row = (uint8_t *)img_alloc((size_t)EW * 3, "jpeg rgb row");
    ctx->out.ec      = (fs_err_t *)img_calloc((size_t)EW + 2, sizeof(fs_err_t), "jpeg fs err cur");
    ctx->out.en      = (fs_err_t *)img_calloc((size_t)EW + 2, sizeof(fs_err_t), "jpeg fs err next");
    bool use_fs = ctx->out.band && ctx->out.rgb_row &&
                  ctx->out.ec && ctx->out.en;

    if (use_fs) {
        memset(ctx->out.band, 255, bsz);
        ctx->out.band_w    = decoded_w;
        ctx->out.band_h    = bh;
        ctx->out.band_top  = -1;
        ctx->out.band_rows = 0;
        ESP_LOGI(TAG, "%s F-S (band=%u bytes)",
                 has_yellow ? "yellow/red-first" : "red-first",
                 (unsigned)bsz);
    } else {
        free(ctx->out.band); free(ctx->out.rgb_row);
        free(ctx->out.ec);   free(ctx->out.en);
        ctx->out.band = NULL; ctx->out.rgb_row = NULL;
        ctx->out.ec = NULL;   ctx->out.en = NULL;
        ESP_LOGW(TAG, "F-S alloc failed, using Bayer");
    }

    r = jd_decomp(&jd, use_fs ? jpeg_outfunc_fs : jpeg_outfunc_bayer, scale);

    if (use_fs && ctx->out.band_rows > 0)
        fs_flush_band(&ctx->out);

    free(pool);
    free(ctx->out.band); free(ctx->out.rgb_row);
    free(ctx->out.ec);   free(ctx->out.en);

    if (r != JDR_OK) {
        ESP_LOGE(TAG, "jd_decomp failed: %d", (int)r);
        free(black); free(red); free(yellow);
        return ESP_FAIL;
    }

    compensate_panel_image_orientation(black, red, yellow, EC, has_red, has_yellow);
    if (!write_raw_planes(out_path, black, red, yellow, EC, has_red, has_yellow)) {
        free(black); free(red); free(yellow);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Converted JPEG -> %s (%u bytes/plane, %s%s, contain)",
             out_path, (unsigned)EC, use_fs ? (has_yellow ? "yellow/red-first F-S" : "red-first F-S") : "Bayer",
             has_yellow ? ", BWRY" : "");

    free(black);
    free(red);
    free(yellow);
    return ESP_OK;
}

/* ── public JPEG API ──────────────────────────────────────────────── */

esp_err_t image_convert_jpeg_to_epd_raw(const uint8_t *jpeg, size_t jpeg_len,
                                         const char *out_path)
{
    if (!jpeg || jpeg_len == 0 || !out_path) return ESP_ERR_INVALID_ARG;
    jpeg_ctx_t ctx = { .mem = { .data = jpeg, .len = jpeg_len, .pos = 0 }, .fp = NULL };
    return jpeg_decode_common(&ctx, jpeg_infunc_mem, out_path);
}

static esp_err_t jpeg_convert_from_file(const char *input_path, const char *out_path)
{
    FILE *fp = fopen(input_path, "rb");
    if (!fp) { ESP_LOGE(TAG, "open input failed: %s", input_path); return ESP_FAIL; }
    jpeg_ctx_t ctx = { .mem = { 0 }, .fp = fp };
    esp_err_t ret = jpeg_decode_common(&ctx, jpeg_infunc_file, out_path);
    fclose(fp);
    return ret;
}

/* ── PNG conversion (stretch-to-fill, red-first F-S) ──────────────── */

static esp_err_t png_decode_to_epd(unsigned char *rgba, unsigned pw, unsigned ph,
                                     const char *out_path)
{
    const int EW = epd_width();
    const int EH = epd_height();
    const int EC = EW * EH / 8;

    log_heap_state("png begin");

    esp_err_t pass = png_try_prequantized_passthrough(rgba, pw, ph, out_path);
    if (pass == ESP_OK) {
        free(rgba);
        return ESP_OK;
    }
    if (pass == ESP_ERR_NO_MEM) {
        free(rgba);
        return pass;
    }

    pass = png_threshold_bw_passthrough(rgba, pw, ph, out_path);
    if (pass == ESP_OK) {
        free(rgba);
        return ESP_OK;
    }
    if (pass == ESP_ERR_NO_MEM) {
        free(rgba);
        return pass;
    }

    bool has_red = epd_has_red();
    bool has_yellow = epd_has_yellow();
    uint8_t *black = (uint8_t *)img_alloc(EC, "png black plane");
    uint8_t *red   = has_red ? (uint8_t *)img_alloc(EC, "png red plane") : NULL;
    uint8_t *yellow = has_yellow ? (uint8_t *)img_alloc(EC, "png yellow plane") : NULL;
    if (!black || (has_red && !red) || (has_yellow && !yellow)) {
        ESP_LOGE(TAG, "png plane alloc failed: plane=%u bytes, panel=%dx%d, red=%d yellow=%d",
                 (unsigned)EC, EW, EH, has_red ? 1 : 0, has_yellow ? 1 : 0);
        free(black); free(red); free(yellow); free(rgba); return ESP_ERR_NO_MEM;
    }
    memset(black, 0xFF, EC);
    if (red)
        memset(red,   0x00, EC);
    if (yellow)
        memset(yellow, 0x00, EC);

    image_fit_rect_t fit = image_fit_contain_rect((int)pw, (int)ph, EW, EH);
    ESP_LOGI(TAG, "PNG %ux%u -> contain %dx%d at %d,%d on %dx%d",
             pw, ph, fit.w, fit.h, fit.x, fit.y, EW, EH);

    uint8_t  *rgb_row = (uint8_t *)img_alloc((size_t)EW * 3, "png rgb row");
    fs_err_t *ec = (fs_err_t *)img_calloc((size_t)EW + 2, sizeof(fs_err_t), "png fs err cur");
    fs_err_t *en = (fs_err_t *)img_calloc((size_t)EW + 2, sizeof(fs_err_t), "png fs err next");
    bool use_fs = rgb_row && ec && en;

    if (use_fs) {
        for (int ey = 0; ey < EH; ey++) {
            int rel_y = ey - fit.y;
            bool in_y = rel_y >= 0 && rel_y < fit.h;
            int sy = in_y ? (rel_y * (int)ph / fit.h) : 0;
            if (sy >= (int)ph) sy = (int)ph - 1;
            for (int ex = 0; ex < EW; ex++) {
                int rel_x = ex - fit.x;
                bool in_x = rel_x >= 0 && rel_x < fit.w;
                uint8_t r = 255, g = 255, b = 255, a = 255;
                if (in_x && in_y) {
                    int sx = rel_x * (int)pw / fit.w;
                    if (sx >= (int)pw) sx = (int)pw - 1;
                    size_t si = ((size_t)sy * pw + (size_t)sx) * 4u;
                    r = rgba[si]; g = rgba[si + 1]; b = rgba[si + 2]; a = rgba[si + 3];
                }
                if (a < 255) {
                    r = (uint8_t)((r * a + 255u * (255u - a)) / 255u);
                    g = (uint8_t)((g * a + 255u * (255u - a)) / 255u);
                    b = (uint8_t)((b * a + 255u * (255u - a)) / 255u);
                }
                rgb_row[ex * 3]     = r;
                rgb_row[ex * 3 + 1] = g;
                rgb_row[ex * 3 + 2] = b;
            }
            memset(en, 0, ((size_t)EW + 2) * sizeof(fs_err_t));
            fs_dither_row(rgb_row, ec, en, black, red, yellow, ey, EW,
                          has_red, has_yellow);
            fs_err_t *tmp = ec; ec = en; en = tmp;
        }
    } else {
        free(rgb_row); free(ec); free(en);
        rgb_row = NULL; ec = NULL; en = NULL;
        ESP_LOGW(TAG, "F-S alloc failed, using Bayer");
        for (int ey = 0; ey < EH; ey++) {
            int rel_y = ey - fit.y;
            bool in_y = rel_y >= 0 && rel_y < fit.h;
            int sy = in_y ? (rel_y * (int)ph / fit.h) : 0;
            if (sy >= (int)ph) sy = (int)ph - 1;
            for (int ex = 0; ex < EW; ex++) {
                int rel_x = ex - fit.x;
                bool in_x = rel_x >= 0 && rel_x < fit.w;
                uint8_t r = 255, g = 255, b = 255, a = 255;
                if (in_x && in_y) {
                    int sx = rel_x * (int)pw / fit.w;
                    if (sx >= (int)pw) sx = (int)pw - 1;
                    size_t si = ((size_t)sy * pw + (size_t)sx) * 4u;
                    r = rgba[si]; g = rgba[si + 1]; b = rgba[si + 2]; a = rgba[si + 3];
                }
                if (a < 255) {
                    r = (uint8_t)((r * a + 255u * (255u - a)) / 255u);
                    g = (uint8_t)((g * a + 255u * (255u - a)) / 255u);
                    b = (uint8_t)((b * a + 255u * (255u - a)) / 255u);
                }
                dither_pixel_bayer(black, red, yellow, ex, ey, EW, r, g, b,
                                   has_red, has_yellow);
            }
        }
    }

    free(rgba);
    free(rgb_row); free(ec); free(en);

    compensate_panel_image_orientation(black, red, yellow, EC, has_red, has_yellow);
    if (!write_raw_planes(out_path, black, red, yellow, EC, has_red, has_yellow)) {
        free(black);
        free(red);
        free(yellow);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Converted PNG -> %s (%u bytes/plane, %s%s, contain)",
             out_path, (unsigned)EC, use_fs ? (has_yellow ? "yellow/red-first F-S" : "red-first F-S") : "Bayer",
             has_yellow ? ", BWRY" : "");
    free(black);
    free(red);
    free(yellow);
    return ESP_OK;
}

esp_err_t image_convert_png_to_epd_raw(const uint8_t *png, size_t png_len,
                                        const char *out_path)
{
    if (!png || png_len == 0 || !out_path) return ESP_ERR_INVALID_ARG;

    if (png_len >= 24) {
        unsigned pw_hdr = ((unsigned)png[16] << 24) | ((unsigned)png[17] << 16) |
                          ((unsigned)png[18] << 8)  | (unsigned)png[19];
        unsigned ph_hdr = ((unsigned)png[20] << 24) | ((unsigned)png[21] << 16) |
                          ((unsigned)png[22] << 8)  | (unsigned)png[23];
        if (pw_hdr == 0 || ph_hdr == 0 ||
            pw_hdr > PNG_MAX_SIDE_PIXELS || ph_hdr > PNG_MAX_SIDE_PIXELS ||
            (uint64_t)pw_hdr * (uint64_t)ph_hdr > PNG_MAX_DECODE_PIXELS) {
            ESP_LOGE(TAG, "PNG dimensions rejected before decode: %ux%u",
                     pw_hdr, ph_hdr);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    unsigned char *rgba = NULL;
    unsigned pw = 0, ph = 0;
    unsigned perr = lodepng_decode32(&rgba, &pw, &ph, png, png_len);
    if (perr || !rgba) {
        ESP_LOGE(TAG, "PNG decode error %u: %s", perr, lodepng_error_text(perr));
        free(rgba);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PNG decoded: %ux%u (free heap: %lu)", pw, ph,
             (unsigned long)esp_get_free_heap_size());

    return png_decode_to_epd(rgba, pw, ph, out_path);
}

/* ── BMP conversion (stretch-to-fill, red-first F-S) ──────────────── */

static inline uint16_t bmp_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t bmp_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t bmp_decode_core(FILE *fp, const uint8_t *mem, size_t mem_len,
                                  const char *out_path)
{
    uint8_t hdr[54];
    if (fp) {
        fseek(fp, 0, SEEK_SET);
        if (fread(hdr, 1, 54, fp) != 54) { ESP_LOGE(TAG, "BMP header too short"); return ESP_FAIL; }
    } else {
        if (mem_len < 54) return ESP_ERR_INVALID_ARG;
        memcpy(hdr, mem, 54);
    }

    if (hdr[0] != 0x42 || hdr[1] != 0x4D) return ESP_ERR_NOT_SUPPORTED;

    uint32_t pixel_off = bmp_le32(hdr + 10);
    uint32_t dib_size  = bmp_le32(hdr + 14);
    if (dib_size < 40) return ESP_ERR_NOT_SUPPORTED;

    int32_t  bmp_w = (int32_t)bmp_le32(hdr + 18);
    int32_t  bmp_h = (int32_t)bmp_le32(hdr + 22);
    uint16_t bpp   = bmp_le16(hdr + 28);
    uint32_t comp  = bmp_le32(hdr + 30);

    if (comp != 0 || (bpp != 24 && bpp != 32)) {
        ESP_LOGE(TAG, "BMP: unsupported (bpp=%u, compression=%lu)", bpp, (unsigned long)comp);
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool bottom_up = (bmp_h > 0);
    int w = (int)bmp_w;
    int h = bottom_up ? (int)bmp_h : (int)(-bmp_h);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return ESP_ERR_INVALID_ARG;

    const int EW = epd_width();
    const int EH = epd_height();
    const int EC = EW * EH / 8;

    int bpp_bytes  = bpp / 8;
    int row_stride = ((w * bpp_bytes) + 3) & ~3;

    log_heap_state("bmp begin");

    bool has_red_bmp = epd_has_red();
    bool has_yellow_bmp = epd_has_yellow();
    uint8_t *black = (uint8_t *)img_alloc(EC, "bmp black plane");
    uint8_t *red   = has_red_bmp ? (uint8_t *)img_alloc(EC, "bmp red plane") : NULL;
    uint8_t *yellow = has_yellow_bmp ? (uint8_t *)img_alloc(EC, "bmp yellow plane") : NULL;
    uint8_t *row   = (uint8_t *)img_alloc((size_t)row_stride, "bmp input row");
    if (!black || (has_red_bmp && !red) || (has_yellow_bmp && !yellow) || !row) {
        ESP_LOGE(TAG, "bmp alloc failed: plane=%u row=%u panel=%dx%d red=%d yellow=%d",
                 (unsigned)EC, (unsigned)row_stride, EW, EH,
                 has_red_bmp ? 1 : 0, has_yellow_bmp ? 1 : 0);
        free(black); free(red); free(yellow); free(row);
        return ESP_ERR_NO_MEM;
    }
    memset(black, 0xFF, EC);
    if (red)
        memset(red,   0x00, EC);
    if (yellow)
        memset(yellow, 0x00, EC);

    image_fit_rect_t fit = image_fit_contain_rect(w, h, EW, EH);
    ESP_LOGI(TAG, "BMP %dx%d %u-bit -> contain %dx%d at %d,%d on %dx%d",
             w, h, (unsigned)bpp, fit.w, fit.h, fit.x, fit.y, EW, EH);

    uint8_t  *rgb_row = (uint8_t *)img_alloc((size_t)EW * 3, "bmp rgb row");
    fs_err_t *ec_b    = (fs_err_t *)img_calloc((size_t)EW + 2, sizeof(fs_err_t), "bmp fs err cur");
    fs_err_t *en_b    = (fs_err_t *)img_calloc((size_t)EW + 2, sizeof(fs_err_t), "bmp fs err next");
    bool use_fs = rgb_row && ec_b && en_b;

    int last_br = -1;

    if (use_fs) {
        for (int ey = 0; ey < EH; ey++) {
            int rel_y = ey - fit.y;
            bool in_y = rel_y >= 0 && rel_y < fit.h;
            int src_y = in_y ? (rel_y * h / fit.h) : 0;
            if (src_y >= h) src_y = h - 1;
            int br = bottom_up ? (h - 1 - src_y) : src_y;
            if (in_y && br != last_br) {
                long off = (long)pixel_off + (long)br * (long)row_stride;
                if (fp) {
                    fseek(fp, off, SEEK_SET);
                    if (fread(row, 1, (size_t)row_stride, fp) != (size_t)row_stride)
                        memset(row, 0xFF, (size_t)row_stride);
                } else if ((size_t)off + (size_t)row_stride <= mem_len) {
                    memcpy(row, mem + off, (size_t)row_stride);
                } else {
                    memset(row, 0xFF, (size_t)row_stride);
                }
                last_br = br;
            }
            for (int ex = 0; ex < EW; ex++) {
                int rel_x = ex - fit.x;
                bool in_x = rel_x >= 0 && rel_x < fit.w;
                if (in_x && in_y) {
                    int src_x = rel_x * w / fit.w;
                    if (src_x >= w) src_x = w - 1;
                    int pi = src_x * bpp_bytes;
                    rgb_row[ex * 3]     = row[pi + 2];
                    rgb_row[ex * 3 + 1] = row[pi + 1];
                    rgb_row[ex * 3 + 2] = row[pi];
                } else {
                    rgb_row[ex * 3]     = 255;
                    rgb_row[ex * 3 + 1] = 255;
                    rgb_row[ex * 3 + 2] = 255;
                }
            }
            memset(en_b, 0, ((size_t)EW + 2) * sizeof(fs_err_t));
            fs_dither_row(rgb_row, ec_b, en_b, black, red, yellow, ey, EW,
                          has_red_bmp, has_yellow_bmp);
            fs_err_t *tmp = ec_b; ec_b = en_b; en_b = tmp;
        }
    } else {
        free(rgb_row); free(ec_b); free(en_b);
        rgb_row = NULL; ec_b = NULL; en_b = NULL;
        for (int ey = 0; ey < EH; ey++) {
            int rel_y = ey - fit.y;
            bool in_y = rel_y >= 0 && rel_y < fit.h;
            int src_y = in_y ? (rel_y * h / fit.h) : 0;
            if (src_y >= h) src_y = h - 1;
            int br = bottom_up ? (h - 1 - src_y) : src_y;
            if (in_y && br != last_br) {
                long off = (long)pixel_off + (long)br * (long)row_stride;
                if (fp) {
                    fseek(fp, off, SEEK_SET);
                    if (fread(row, 1, (size_t)row_stride, fp) != (size_t)row_stride) continue;
                } else {
                    if ((size_t)off + (size_t)row_stride > mem_len) continue;
                    memcpy(row, mem + off, (size_t)row_stride);
                }
                last_br = br;
            }
            for (int ex = 0; ex < EW; ex++) {
                int rel_x = ex - fit.x;
                bool in_x = rel_x >= 0 && rel_x < fit.w;
                uint8_t r = 255, g = 255, b = 255;
                if (in_x && in_y) {
                    int src_x = rel_x * w / fit.w;
                    if (src_x >= w) src_x = w - 1;
                    int pi = src_x * bpp_bytes;
                    r = row[pi + 2];
                    g = row[pi + 1];
                    b = row[pi + 0];
                }
                dither_pixel_bayer(black, red, yellow, ex, ey, EW, r, g, b,
                                   has_red_bmp, has_yellow_bmp);
            }
        }
    }

    free(row);
    free(rgb_row); free(ec_b); free(en_b);

    compensate_panel_image_orientation(black, red, yellow, EC,
                                       has_red_bmp, has_yellow_bmp);
    if (!write_raw_planes(out_path, black, red, yellow, EC,
                          has_red_bmp, has_yellow_bmp)) {
        free(black);
        free(red);
        free(yellow);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Converted BMP -> %s (%u bytes/plane, %s%s, contain)",
             out_path, (unsigned)EC, use_fs ? (has_yellow_bmp ? "yellow/red-first F-S" : "red-first F-S") : "Bayer",
             has_yellow_bmp ? ", BWRY" : "");
    free(black);
    free(red);
    free(yellow);
    return ESP_OK;
}

esp_err_t image_convert_bmp_to_epd_raw(const uint8_t *bmp, size_t bmp_len,
                                        const char *out_path)
{
    if (!bmp || bmp_len < 54 || !out_path) return ESP_ERR_INVALID_ARG;
    return bmp_decode_core(NULL, bmp, bmp_len, out_path);
}

/* ── unified converter (auto-detect by magic bytes) ───────────────── */

esp_err_t image_convert_to_epd_raw(const uint8_t *data, size_t len,
                                    const char *out_path)
{
    if (!data || len < 4) return ESP_ERR_INVALID_ARG;

    if (data[0] == 0xFF && data[1] == 0xD8)
        return image_convert_jpeg_to_epd_raw(data, len, out_path);
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
        return image_convert_png_to_epd_raw(data, len, out_path);
    if (data[0] == 0x42 && data[1] == 0x4D)
        return image_convert_bmp_to_epd_raw(data, len, out_path);

    ESP_LOGE(TAG, "Unknown image format (magic: %02X %02X %02X %02X)",
             data[0], data[1], data[2], data[3]);
    return ESP_ERR_NOT_SUPPORTED;
}

/* ── file-based converter (low-memory path) ───────────────────────── */

esp_err_t image_convert_file_to_epd_raw(const char *input_path, const char *out_path)
{
    if (!input_path || !out_path) return ESP_ERR_INVALID_ARG;

    uint8_t magic[4] = {0};
    FILE *mf = fopen(input_path, "rb");
    if (!mf) { ESP_LOGE(TAG, "open failed: %s", input_path); return ESP_FAIL; }
    size_t mr = fread(magic, 1, 4, mf);
    fclose(mf);
    if (mr < 2) { ESP_LOGE(TAG, "file too small: %s", input_path); return ESP_FAIL; }

    ESP_LOGI(TAG, "convert_file: %s (magic: %02X %02X), free heap: %lu",
             input_path, magic[0], magic[1], (unsigned long)esp_get_free_heap_size());

    if (magic[0] == 0xFF && magic[1] == 0xD8)
        return jpeg_convert_from_file(input_path, out_path);

    if (magic[0] == 0x89 && magic[1] == 0x50) {
        struct stat st;
        if (stat(input_path, &st) != 0 || st.st_size <= 0) return ESP_FAIL;
        if (st.st_size > PNG_MAX_FILE_BYTES) {
            ESP_LOGE(TAG, "PNG too large (%ld bytes, max %u bytes)",
                     (long)st.st_size, (unsigned)PNG_MAX_FILE_BYTES);
            return ESP_ERR_NO_MEM;
        }
        uint8_t *buf = (uint8_t *)img_alloc((size_t)st.st_size, "png file buffer");
        if (!buf) { ESP_LOGE(TAG, "PNG file malloc(%ld) failed", (long)st.st_size); return ESP_ERR_NO_MEM; }
        FILE *pf = fopen(input_path, "rb");
        if (!pf) { free(buf); return ESP_FAIL; }
        size_t got = fread(buf, 1, (size_t)st.st_size, pf);
        fclose(pf);

        if (got >= 24) {
            unsigned pw_hdr = ((unsigned)buf[16] << 24) | ((unsigned)buf[17] << 16) |
                              ((unsigned)buf[18] << 8)  | (unsigned)buf[19];
            unsigned ph_hdr = ((unsigned)buf[20] << 24) | ((unsigned)buf[21] << 16) |
                              ((unsigned)buf[22] << 8)  | (unsigned)buf[23];
            if (pw_hdr == 0 || ph_hdr == 0 ||
                pw_hdr > PNG_MAX_SIDE_PIXELS || ph_hdr > PNG_MAX_SIDE_PIXELS ||
                (uint64_t)pw_hdr * (uint64_t)ph_hdr > PNG_MAX_DECODE_PIXELS) {
                ESP_LOGE(TAG, "PNG dimensions rejected before decode: %ux%u",
                         pw_hdr, ph_hdr);
                free(buf);
                return ESP_ERR_INVALID_SIZE;
            }
        }

        unsigned char *rgba = NULL;
        unsigned pw = 0, ph = 0;
        unsigned perr = lodepng_decode32(&rgba, &pw, &ph, buf, got);
        free(buf);
        if (perr || !rgba) {
            ESP_LOGE(TAG, "PNG decode error %u: %s", perr, lodepng_error_text(perr));
            free(rgba);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "PNG decoded: %ux%u (free heap: %lu)", pw, ph,
                 (unsigned long)esp_get_free_heap_size());
        return png_decode_to_epd(rgba, pw, ph, out_path);
    }

    if (magic[0] == 0x42 && magic[1] == 0x4D) {
        FILE *bf = fopen(input_path, "rb");
        if (!bf) return ESP_FAIL;
        esp_err_t ret = bmp_decode_core(bf, NULL, 0, out_path);
        fclose(bf);
        return ret;
    }

    ESP_LOGE(TAG, "Unknown format in file %s (magic: %02X %02X %02X %02X)",
             input_path, magic[0], magic[1], magic[2], magic[3]);
    return ESP_ERR_NOT_SUPPORTED;
}
