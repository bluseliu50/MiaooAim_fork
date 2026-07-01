#include "font_ext.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "font_ext";

#define MEF_HEADER_BYTES 12u
#define FONT_EXT_BLOB_MAX_BYTES (2u * 1024u * 1024u)

typedef struct {
    uint16_t px;
    const char *path;
} font_slot_t;

typedef struct {
    bool valid;
    uint16_t px;
    uint32_t cp;
    uint16_t glyph_bytes;
    uint8_t advance_px;
    uint32_t age;
    uint8_t bitmap[FONT_EXT_MAX_GLYPH_BYTES];
} glyph_cache_t;

static const font_slot_t s_slots[FONT_EXT_COUNT] = {
    { 16, FONT_EXT_BASE_PATH "/cjk16.mef" },
    { 24, FONT_EXT_BASE_PATH "/cjk24.mef" },
    { 32, FONT_EXT_BASE_PATH "/cjk32.mef" },
};

static font_ext_info_t s_info[FONT_EXT_COUNT];
static uint8_t *s_font_blob[FONT_EXT_COUNT];
static uint32_t s_font_blob_size[FONT_EXT_COUNT];
static bool s_scanned;
static bool s_fontfs_mounted;
static glyph_cache_t s_cache[24];
static uint32_t s_cache_age;
static StaticSemaphore_t s_mutex_buf;
static SemaphoreHandle_t s_mutex;

static SemaphoreHandle_t font_mutex(void)
{
    if (!s_mutex)
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
    return s_mutex;
}

static bool fontfs_mount_if_needed(void)
{
    if (s_fontfs_mounted)
        return true;

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/fontfs",
        .partition_label = "fontfs",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK) {
        s_fontfs_mounted = true;
        size_t total = 0, used = 0;
        if (esp_spiffs_info("fontfs", &total, &used) == ESP_OK) {
            ESP_LOGI(TAG, "fontfs mounted: total=%u, used=%u",
                     (unsigned)total, (unsigned)used);
        }
        return true;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        s_fontfs_mounted = true;
        return true;
    }
    if (err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "fontfs mount failed: %s", esp_err_to_name(err));
    return false;
}

static uint16_t glyph_bytes_for_px(uint16_t px)
{
    return (uint16_t)(((px + 7u) / 8u) * px);
}

bool font_ext_supported_px(int px)
{
    return px == 16 || px == 24 || px == 32;
}

const char *font_ext_path_for_px(int px)
{
    for (int i = 0; i < FONT_EXT_COUNT; i++) {
        if (s_slots[i].px == px)
            return s_slots[i].path;
    }
    return NULL;
}

static int slot_index_for_px(int px)
{
    for (int i = 0; i < FONT_EXT_COUNT; i++) {
        if (s_slots[i].px == px)
            return i;
    }
    return -1;
}

static void free_font_blobs_locked(void)
{
    for (int i = 0; i < FONT_EXT_COUNT; i++) {
        if (s_font_blob[i]) {
            heap_caps_free(s_font_blob[i]);
            s_font_blob[i] = NULL;
            s_font_blob_size[i] = 0;
        }
    }
}

static uint16_t read_le16p(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32p(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool font_ext_validate_file(const char *path, int expected_px, font_ext_info_t *out)
{
    if (!path)
        return false;

    struct stat st;
    if (stat(path, &st) != 0)
        return false;

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    uint8_t hdr[MEF_HEADER_BYTES];
    bool ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    fclose(f);
    if (!ok)
        return false;

    uint16_t px = read_le16p(hdr + 4);
    uint16_t glyph_bytes = read_le16p(hdr + 6);
    uint32_t glyph_count = read_le32p(hdr + 8);
    uint32_t file_size = (uint32_t)st.st_size;

    uint8_t format = 0;
    if (memcmp(hdr, "MEF1", 4) == 0)
        format = 1;
    else if (memcmp(hdr, "MEF2", 4) == 0)
        format = 2;
    else
        return false;
    if (!font_ext_supported_px(px))
        return false;
    if (expected_px > 0 && px != (uint16_t)expected_px)
        return false;
    if (glyph_bytes != glyph_bytes_for_px(px) ||
        glyph_bytes == 0 || glyph_bytes > FONT_EXT_MAX_GLYPH_BYTES)
        return false;
    if (glyph_count == 0 || glyph_count > 200000)
        return false;

    uint16_t record_bytes = (uint16_t)(4u + glyph_bytes +
                                       (format == 2 ? 1u : 0u));
    uint64_t expected = MEF_HEADER_BYTES +
                        (uint64_t)glyph_count * record_bytes;
    if (expected != file_size)
        return false;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->present = true;
        out->px = px;
        out->glyph_bytes = glyph_bytes;
        out->format = format;
        out->record_bytes = record_bytes;
        out->glyph_count = glyph_count;
        out->file_size = file_size;
        snprintf(out->path, sizeof(out->path), "%s", path);
    }
    return true;
}

static void scan_locked(void)
{
    free_font_blobs_locked();
    for (int i = 0; i < FONT_EXT_COUNT; i++) {
        memset(&s_info[i], 0, sizeof(s_info[i]));
        s_info[i].px = s_slots[i].px;
        snprintf(s_info[i].path, sizeof(s_info[i].path), "%s", s_slots[i].path);
        if (font_ext_validate_file(s_slots[i].path, s_slots[i].px, &s_info[i])) {
            const font_ext_info_t *info = &s_info[i];
            if (info->file_size <= FONT_EXT_BLOB_MAX_BYTES) {
                uint8_t *blob = heap_caps_malloc(info->file_size,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!blob)
                    blob = heap_caps_malloc(info->file_size, MALLOC_CAP_8BIT);
                if (blob) {
                    FILE *f = fopen(info->path, "rb");
                    uint32_t off = 0;
                    bool ok = (f != NULL);
                    while (ok && off < info->file_size) {
                        size_t chunk = info->file_size - off;
                        if (chunk > 16384)
                            chunk = 16384;
                        size_t n = fread(blob + off, 1, chunk, f);
                        if (n != chunk) {
                            ok = false;
                            break;
                        }
                        off += (uint32_t)n;
                        if ((off & 0xFFFFu) == 0)
                            vTaskDelay(1);
                    }
                    if (f)
                        fclose(f);
                    if (ok) {
                        s_font_blob[i] = blob;
                        s_font_blob_size[i] = info->file_size;
                    } else {
                        heap_caps_free(blob);
                        ESP_LOGW(TAG, "external font %upx cache load failed",
                                 (unsigned)info->px);
                    }
                } else {
                    ESP_LOGW(TAG, "external font %upx cache alloc failed (%lu bytes)",
                             (unsigned)info->px, (unsigned long)info->file_size);
                }
            }
        }
        if (!s_info[i].present)
            s_info[i].px = s_slots[i].px;
    }
    memset(s_cache, 0, sizeof(s_cache));
    s_cache_age = 0;
    s_scanned = true;
}

static void ensure_scanned(void)
{
    if (s_scanned)
        return;
    if (!fontfs_mount_if_needed())
        return;

    SemaphoreHandle_t m = font_mutex();
    if (!m)
        return;
    xSemaphoreTake(m, portMAX_DELAY);
    if (!s_scanned)
        scan_locked();
    xSemaphoreGive(m);
}

void font_ext_init(void)
{
    font_ext_refresh();
}

void font_ext_refresh(void)
{
    SemaphoreHandle_t m = font_mutex();
    if (!m)
        return;
    xSemaphoreTake(m, portMAX_DELAY);
    fontfs_mount_if_needed();
    if (s_fontfs_mounted) {
        scan_locked();
        for (int i = 0; i < FONT_EXT_COUNT; i++) {
            if (s_info[i].present) {
                ESP_LOGI(TAG, "external font %upx: %lu glyphs, %lu bytes",
                         (unsigned)s_info[i].px,
                         (unsigned long)s_info[i].glyph_count,
                         (unsigned long)s_info[i].file_size);
            }
        }
    } else {
        s_scanned = false;
        memset(s_info, 0, sizeof(s_info));
    }
    xSemaphoreGive(m);
}

void font_ext_get_info(font_ext_info_t out[FONT_EXT_COUNT])
{
    if (!out)
        return;
    ensure_scanned();

    SemaphoreHandle_t m = font_mutex();
    if (!m)
        return;
    xSemaphoreTake(m, portMAX_DELAY);
    for (int i = 0; i < FONT_EXT_COUNT; i++) {
        out[i] = s_info[i];
        if (!out[i].px)
            out[i].px = s_slots[i].px;
        if (!out[i].path[0])
            snprintf(out[i].path, sizeof(out[i].path), "%s", s_slots[i].path);
    }
    xSemaphoreGive(m);
}

static bool choose_px_order(int scale, uint16_t order[FONT_EXT_COUNT])
{
    if (scale < 1)
        return false;
    if (scale == 1) {
        order[0] = 16; order[1] = 24; order[2] = 32;
    } else if (scale == 2) {
        order[0] = 32; order[1] = 24; order[2] = 16;
    } else {
        order[0] = 32; order[1] = 24; order[2] = 16;
    }
    return true;
}

static void choose_px_order_for_target(int target_px, uint16_t order[FONT_EXT_COUNT])
{
    /* All three sizes (16/24/32) are now native bitmap sources.
     * Prefer the closest native size to minimize runtime scaling. */
    if (target_px <= 20) {
        order[0] = 16; order[1] = 24; order[2] = 32;
    } else if (target_px <= 28) {
        order[0] = 24; order[1] = 16; order[2] = 32;
    } else {
        order[0] = 32; order[1] = 24; order[2] = 16;
    }
}

static glyph_cache_t *cache_find(uint16_t px, uint32_t cp)
{
    for (int i = 0; i < (int)(sizeof(s_cache) / sizeof(s_cache[0])); i++) {
        if (s_cache[i].valid && s_cache[i].px == px && s_cache[i].cp == cp) {
            s_cache[i].age = ++s_cache_age;
            return &s_cache[i];
        }
    }
    return NULL;
}

static glyph_cache_t *cache_alloc(void)
{
    int best = 0;
    for (int i = 0; i < (int)(sizeof(s_cache) / sizeof(s_cache[0])); i++) {
        if (!s_cache[i].valid)
            return &s_cache[i];
        if (s_cache[i].age < s_cache[best].age)
            best = i;
    }
    return &s_cache[best];
}

static bool load_glyph_locked(uint16_t px, uint32_t cp,
                              uint8_t *out, uint16_t *glyph_bytes,
                              uint8_t *advance_px)
{
    int idx = slot_index_for_px(px);
    if (idx < 0 || !s_info[idx].present)
        return false;

    glyph_cache_t *cached = cache_find(px, cp);
    if (cached) {
        memcpy(out, cached->bitmap, cached->glyph_bytes);
        *glyph_bytes = cached->glyph_bytes;
        if (advance_px)
            *advance_px = cached->advance_px;
        return true;
    }

    const font_ext_info_t *info = &s_info[idx];
    const uint8_t *blob = s_font_blob[idx];
    if (blob && s_font_blob_size[idx] == info->file_size) {
        uint32_t lo = 0;
        uint32_t hi = info->glyph_count;
        uint32_t rec_size = info->record_bytes ? info->record_bytes
                                               : (4u + info->glyph_bytes);
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            uint32_t off = MEF_HEADER_BYTES + mid * rec_size;
            if (off + rec_size > s_font_blob_size[idx])
                break;
            const uint8_t *rec = blob + off;
            uint32_t mid_cp = read_le32p(rec);
            if (mid_cp == cp) {
                uint8_t found_advance = 0;
                const uint8_t *bmp = rec + 4;
                if (info->format == 2) {
                    found_advance = rec[4];
                    bmp++;
                }
                memcpy(out, bmp, info->glyph_bytes);
                *glyph_bytes = info->glyph_bytes;
                if (advance_px)
                    *advance_px = found_advance;

                glyph_cache_t *slot = cache_alloc();
                slot->valid = true;
                slot->px = px;
                slot->cp = cp;
                slot->glyph_bytes = *glyph_bytes;
                slot->advance_px = found_advance;
                slot->age = ++s_cache_age;
                memcpy(slot->bitmap, out, *glyph_bytes);
                return true;
            }
            if (mid_cp < cp)
                lo = mid + 1u;
            else
                hi = mid;
        }
        return false;
    }

    FILE *f = fopen(info->path, "rb");
    if (!f)
        return false;

    uint32_t lo = 0;
    uint32_t hi = info->glyph_count;
    uint32_t rec_size = info->record_bytes ? info->record_bytes
                                           : (4u + info->glyph_bytes);
    bool found = false;
    uint8_t found_advance = 0;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        long off = (long)MEF_HEADER_BYTES + (long)mid * (long)rec_size;
        if (fseek(f, off, SEEK_SET) != 0)
            break;
        uint8_t raw_cp[4] = {0};
        if (fread(raw_cp, 1, sizeof(raw_cp), f) != sizeof(raw_cp))
            break;
        uint32_t mid_cp = read_le32p(raw_cp);
        if (mid_cp == cp) {
            if (info->format == 2) {
                uint8_t raw_adv = 0;
                if (fread(&raw_adv, 1, 1, f) != 1)
                    break;
                found_advance = raw_adv;
            }
            if (fread(out, 1, info->glyph_bytes, f) == info->glyph_bytes) {
                *glyph_bytes = info->glyph_bytes;
                found = true;
            }
            break;
        }
        if (mid_cp < cp)
            lo = mid + 1u;
        else
            hi = mid;
    }
    fclose(f);

    if (found) {
        glyph_cache_t *slot = cache_alloc();
        slot->valid = true;
        slot->px = px;
        slot->cp = cp;
        slot->glyph_bytes = *glyph_bytes;
        slot->advance_px = found_advance;
        slot->age = ++s_cache_age;
        memcpy(slot->bitmap, out, *glyph_bytes);
    }
    if (found && advance_px)
        *advance_px = found_advance;
    return found;
}

static bool load_best_glyph(uint32_t cp, int scale,
                            uint16_t *px, uint8_t *out,
                            uint16_t *glyph_bytes, uint8_t *advance_px)
{
    ensure_scanned();
    uint16_t order[FONT_EXT_COUNT] = {0};
    if (!choose_px_order(scale, order))
        return false;

    SemaphoreHandle_t m = font_mutex();
    if (!m)
        return false;
    bool found = false;
    xSemaphoreTake(m, portMAX_DELAY);
    for (int i = 0; i < FONT_EXT_COUNT && order[i]; i++) {
        if (load_glyph_locked(order[i], cp, out, glyph_bytes, advance_px)) {
            *px = order[i];
            found = true;
            break;
        }
    }
    xSemaphoreGive(m);
    return found;
}

static bool load_best_glyph_px(uint32_t cp, int target_px,
                               uint16_t *px, uint8_t *out,
                               uint16_t *glyph_bytes, uint8_t *advance_px)
{
    ensure_scanned();
    uint16_t order[FONT_EXT_COUNT] = {0};
    choose_px_order_for_target(target_px, order);

    SemaphoreHandle_t m = font_mutex();
    if (!m)
        return false;
    bool found = false;
    xSemaphoreTake(m, portMAX_DELAY);
    for (int i = 0; i < FONT_EXT_COUNT && order[i]; i++) {
        if (load_glyph_locked(order[i], cp, out, glyph_bytes, advance_px)) {
            *px = order[i];
            found = true;
            break;
        }
    }
    xSemaphoreGive(m);
    return found;
}

bool font_ext_probe_glyph(uint32_t cp, int scale, int *advance)
{
    uint8_t tmp[FONT_EXT_MAX_GLYPH_BYTES];
    uint16_t px = 0;
    uint16_t glyph_bytes = 0;
    uint8_t advance_px = 0;
    if (!load_best_glyph(cp, scale, &px, tmp, &glyph_bytes, &advance_px))
        return false;
    if (advance) {
        int adv = advance_px ? ((int)advance_px * 16 * scale + px / 2) / px
                             : ((cp < 0x80) ? (8 * scale) : (16 * scale));
        if (adv < scale)
            adv = scale;
        *advance = adv;
    }
    return true;
}

bool font_ext_draw_glyph(fb_t *fb, int x, int y, uint32_t cp,
                         fb_color_t c, int scale, int *advance)
{
    if (!fb)
        return false;

    uint8_t bmp[FONT_EXT_MAX_GLYPH_BYTES];
    uint16_t px = 0;
    uint16_t glyph_bytes = 0;
    uint8_t advance_px = 0;
    if (!load_best_glyph(cp, scale, &px, bmp, &glyph_bytes, &advance_px))
        return false;

    int target_w = 16 * scale;
    int adv_w = advance_px ? ((int)advance_px * 16 * scale + px / 2) / px
                           : ((cp < 0x80) ? (8 * scale) : target_w);
    if (adv_w < scale)
        adv_w = scale;
    if (adv_w > target_w)
        adv_w = target_w;
    int target_h = 16 * scale;
    int stride = (px + 7) / 8;
    for (int yy = 0; yy < target_h; yy++) {
        int sy0 = (yy * px) / target_h;
        int sy1 = ((yy + 1) * px) / target_h;
        /* Only sample the second source row when the target row actually
         * spans a row boundary (non-integer scale ratio). At 1:1 ratio
         * sy0 == sy1, so we sample exactly one row. */
        if (sy1 > sy0 + 1 && sy1 < px) {
            for (int xx = 0; xx < target_w; xx++) {
                int sx = (xx * px) / target_w;
                int bit = 0x80 >> (sx % 8);
                if (bmp[sy0 * stride + sx / 8] & bit) {
                    fb_pixel(fb, x + xx, y + yy, c);
                } else if (bmp[sy1 * stride + sx / 8] & bit) {
                    fb_pixel(fb, x + xx, y + yy, c);
                }
            }
        } else {
            const uint8_t *row = bmp + sy0 * stride;
            for (int xx = 0; xx < target_w; xx++) {
                int sx = (xx * px) / target_w;
                if (row[sx / 8] & (0x80 >> (sx % 8)))
                    fb_pixel(fb, x + xx, y + yy, c);
            }
        }
        if ((yy & 0x0F) == 0x0F)
            vTaskDelay(1);
    }
    if (advance)
        *advance = adv_w;
    (void)glyph_bytes;
    return true;
}

bool font_ext_probe_glyph_px(uint32_t cp, int target_px, int *advance)
{
    if (target_px < 8)
        return false;

    uint8_t tmp[FONT_EXT_MAX_GLYPH_BYTES];
    uint16_t px = 0;
    uint16_t glyph_bytes = 0;
    uint8_t advance_px = 0;
    if (!load_best_glyph_px(cp, target_px, &px, tmp, &glyph_bytes, &advance_px))
        return false;
    if (advance) {
        int adv = advance_px ? ((int)advance_px * target_px + px / 2) / px
                             : ((cp < 0x80) ? (target_px / 2) : target_px);
        if (adv < 1)
            adv = 1;
        if (adv > target_px)
            adv = target_px;
        *advance = adv;
    }
    (void)glyph_bytes;
    return true;
}

bool font_ext_draw_glyph_px(fb_t *fb, int x, int y, uint32_t cp,
                            fb_color_t c, int target_px, int *advance)
{
    if (!fb || target_px < 8)
        return false;

    uint8_t bmp[FONT_EXT_MAX_GLYPH_BYTES];
    uint16_t px = 0;
    uint16_t glyph_bytes = 0;
    uint8_t advance_px = 0;
    if (!load_best_glyph_px(cp, target_px, &px, bmp, &glyph_bytes, &advance_px))
        return false;

    int adv_w = advance_px ? ((int)advance_px * target_px + px / 2) / px
                           : ((cp < 0x80) ? (target_px / 2) : target_px);
    if (adv_w < 1)
        adv_w = 1;
    if (adv_w > target_px)
        adv_w = target_px;
    int stride = (px + 7) / 8;
    for (int yy = 0; yy < target_px; yy++) {
        int sy0 = (yy * px) / target_px;
        int sy1 = ((yy + 1) * px) / target_px;
        if (sy1 > sy0 + 1 && sy1 < px) {
            for (int xx = 0; xx < target_px; xx++) {
                int sx = (xx * px) / target_px;
                int bit = 0x80 >> (sx % 8);
                if (bmp[sy0 * stride + sx / 8] & bit) {
                    fb_pixel(fb, x + xx, y + yy, c);
                } else if (bmp[sy1 * stride + sx / 8] & bit) {
                    fb_pixel(fb, x + xx, y + yy, c);
                }
            }
        } else {
            const uint8_t *row = bmp + sy0 * stride;
            for (int xx = 0; xx < target_px; xx++) {
                int sx = (xx * px) / target_px;
                if (row[sx / 8] & (0x80 >> (sx % 8)))
                    fb_pixel(fb, x + xx, y + yy, c);
            }
        }
        if ((yy & 0x0F) == 0x0F)
            vTaskDelay(1);
    }
    if (advance)
        *advance = adv_w;
    (void)glyph_bytes;
    return true;
}
