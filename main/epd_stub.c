/**
 * EPD 驱动 — 主测 SSD1619 / UC8179，并接入 ATC/Solum 实验面板配置
 *
 * 引脚与工程原理图一致：SCK=4 MOSI=5 DC=7 CS=15 RST=6 BUSY=16，SPI2，Mode0，2MHz。
 * BUSY：Waveshare SSD1619 4.2 B（epd4in2b_V2 ReadBusy，flag=新）为「忙=高、空闲=低」，默认 idle=0。
 * 其它模组（如部分 GDE）可能为空闲高，可在 NVS epd/busy_idle=1。
 * 参考：https://github.com/waveshareteam/e-Paper/blob/master/Arduino/epd4in2b_V2/epd4in2b_V2.cpp
 */

#include "epd.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "power_mgr.h"

/* ── 硬件引脚 ─────────────────────────────────────────────────────── */

#define PIN_EPD_SCK   4
#define PIN_EPD_MOSI  5
#define PIN_EPD_DC    7
#define PIN_EPD_CS    15
#define PIN_EPD_RST   6
#define PIN_EPD_BUSY  16

#define EPD_SPI_HOST      SPI2_HOST
#define EPD_SPI_HZ        (2 * 1000 * 1000)
#define EPD_SPI_MAX_TX    16384
#define EPD_FILL_CHUNK    4096
#define EPD_BUSY_TIMEOUT_MS  60000
#define EPD_ATC29_BUSY_TIMEOUT_MS  2500
#define EPD_ATC29_SAFE_REFRESH_MS  12000
#define HINK29_REFRESH_MS          15000
#define HINK29_LOGICAL_W 128
#define HINK29_LOGICAL_H 296
#define EPD_REPAIR_MAX_CYCLES      8

static const char *TAG = "epd";

static spi_device_handle_t s_spi;
static SemaphoreHandle_t   s_epd_mutex;
static epd_panel_t         s_panel = EPD_PANEL_42_BWR;

/** 已为当前面板跑过完整上电初始化序列（避免每次刷屏都硬件复位） */
static bool s_panel_seq_done;

/** epd_init() 完全结束后再为 true，供 HTTP 与其它任务判断 */
static bool s_epd_ready;

static atomic_bool s_force_full_refresh_next = ATOMIC_VAR_INIT(false);

/**
 * NVS 键 busy_idle：0 = 空闲时 BUSY 为低；1 = 空闲时为高。
 * 未配置时：SSD1619 4.2 默认 0（与 Waveshare 官方一致）；5.83 UC8179 默认 1。
 */
static int s_busy_idle_level = -1;

static int busy_idle_effective(void);

/* ── 面板规格 / 回调 ─────────────────────────────────────────────── */

typedef enum {
    EPD_CMD_DATA_24_26,          /* black:0x24, red:0x26, red bit 1 = red */
    EPD_CMD_DATA_24_26_SSD16XX,  /* SSD1677: black:0x24, color:0x26, red bit 0 = red */
    EPD_CMD_DATA_10_13,          /* black:0x10, red:0x13, red bit 1 = red */
    EPD_CMD_DATA_10_92_13,       /* black:0x10, partial-out marker, red:0x13 */
    EPD_CMD_DATA_10_13_INV_RED,  /* black:0x10, red:0x13, red bit 0 = red */
    EPD_CMD_DATA_UC81XX_REF,     /* UC8176/UC8179 reference path with partial window */
    EPD_CMD_DATA_10_13_DUMMY,    /* dummy data byte, black:0x10, red:0x13 */
    EPD_CMD_DATA_10_13_INV_BLACK_DUMMY, /* dummy byte, inverted black on 0x10 */
    EPD_CMD_DATA_DUAL_SSD,       /* split 792x272 frame over two SSD controllers */
    EPD_CMD_DATA_UC8159_INTERLEAVED, /* 2-bit UC8159 packed color stream on 0x10 */
    EPD_CMD_DATA_JD796XX_2BPP,   /* JD79665/668 native 2-bit-per-pixel stream on 0x10 */
} epd_data_mode_t;

typedef struct epd_panel_desc {
    int w;
    int h;
    bool has_red;
    int default_busy_idle;
    bool busy_probe_0x71;
    epd_data_mode_t data_mode;
    const char *name;
    void (*init)(void);
    void (*refresh)(void);
    void (*cursor_home)(void);
    uint16_t x_offset;
    bool mirror_v;
    bool has_yellow;
} epd_panel_desc_t;

static void ssd1619_init_sequence(void);
static void ssd1619_refresh(void);
static void ssd1619_cursor_home(void);
static void uc8179_init_sequence(void);
static void uc8179_refresh(void);
static void ws_583_bwr_init(void);
static void atc_unissd_init(void);
static void hink_ssd1619_29_init(void);
static void hink_ssd1619_29_refresh(void);
static void atc_unissd_97_init(void);
static void atc_ucvar29_init(void);
static void atc_ucvar43_init(void);
static void atc_dualssd_init(void);
static void atc_uc8159_init(void);
static void atc_uc8179_init(void);
static void ref_uc81xx_init(void);
static void ref_uc8159_init(void);
static void ref_ssd1619_init(void);
static void ref_ssd1683_init(void);
static void ref_ssd1677_init(void);
static void ref_jd796xx_init(void);
static void refresh_22_f7_20(void);
static void refresh_12(void);
static void refresh_power_on_12(void);
static void ref_uc81xx_refresh(void);
static void ref_ssd16xx_refresh(void);
static void ref_ssd1683_refresh(void);
static void ref_jd796xx_refresh(void);
static void refresh_atc_dualssd(void);
static void panel_ensure_ready(void);
static void atc_unissd_cursor_home(void);
static void hink_ssd1619_29_cursor_home(void);
static void atc_unissd_97_cursor_home(void);
static void ref_uc81xx_cursor_home(void);
static void ref_ssd1619_cursor_home(void);
static void ref_ssd1683_cursor_home(void);
static void ref_ssd1677_cursor_home(void);
static void ref_jd796xx_cursor_home(void);
static void cursor_none(void);

static const epd_panel_desc_t s_specs[EPD_PANEL_COUNT] = {
    [EPD_PANEL_42_BWR]     = { 400, 300, true,  0, false, EPD_CMD_DATA_24_26,         "SSD1619 4.2\" BWR",         ssd1619_init_sequence, ssd1619_refresh, cursor_none },
    [EPD_PANEL_583_BWR]    = { 648, 480, true,  1, true,  EPD_CMD_DATA_10_13,         "UC8179 5.83\" BWR",         uc8179_init_sequence,  uc8179_refresh,  cursor_none },
    [EPD_PANEL_WS_583_BWR] = { 648, 480, true,  1, true,  EPD_CMD_DATA_10_13,         "Waveshare 5.83\" BWR B V2", ws_583_bwr_init,      refresh_12,       cursor_none },
    [EPD_PANEL_ATC_SSD1619_16_BWR] = { 200, 200, true,  0, false, EPD_CMD_DATA_24_26, "ATC/Solum 1.6\" BWR SSD1619 exp", atc_unissd_init, refresh_22_f7_20, atc_unissd_cursor_home, 0, true },
    [EPD_PANEL_ATC_SSD1619_22_BWR] = { 160, 296, true,  0, false, EPD_CMD_DATA_24_26, "ATC/Solum 2.2\" BWR SSD1619 exp", atc_unissd_init, refresh_22_f7_20, atc_unissd_cursor_home, 8, false },
    [EPD_PANEL_ATC_SSD1619_26_BWR] = { 184, 360, true,  0, false, EPD_CMD_DATA_24_26, "ATC/Solum 2.6\" BWR SSD1619 exp", atc_unissd_init, refresh_22_f7_20, atc_unissd_cursor_home, 8, false },
    [EPD_PANEL_HINK_SSD1619_29_BWR] = { HINK29_LOGICAL_W, HINK29_LOGICAL_H, true,  0, false, EPD_CMD_DATA_24_26, "HINK-E029A14-A1 2.9\" BWR SSD1619", hink_ssd1619_29_init, hink_ssd1619_29_refresh, hink_ssd1619_29_cursor_home, 8, false },
    [EPD_PANEL_ATC_UC8151_29_BWR]  = { 168, 384, true,  1, false, EPD_CMD_DATA_10_13_INV_BLACK_DUMMY, "ATC/Solum 2.9\" BWR UC8151 exp", atc_ucvar29_init, refresh_power_on_12, cursor_none },
    [EPD_PANEL_ATC_UCVAR43_BWR]    = { 152, 522, true,  1, false, EPD_CMD_DATA_10_13_DUMMY, "ATC/Solum 4.3\" BWR UC variant exp", atc_ucvar43_init, refresh_power_on_12, cursor_none },
    [EPD_PANEL_ATC_DUALSSD_585_BWR] = { 792, 272, true, 0, false, EPD_CMD_DATA_DUAL_SSD, "ATC/Solum 5.85\" BWR dual SSD exp", atc_dualssd_init, refresh_atc_dualssd, cursor_none },
    [EPD_PANEL_ATC_DUALSSD_585_BW]  = { 792, 272, false, 0, false, EPD_CMD_DATA_DUAL_SSD, "ATC/Solum 5.85\" BW dual SSD exp", atc_dualssd_init, refresh_atc_dualssd, cursor_none },
    [EPD_PANEL_ATC_UC8159_60_BWR]  = { 600, 448, true,  1, false, EPD_CMD_DATA_UC8159_INTERLEAVED, "ATC/Solum 6.0\" BWR UC8159 exp", atc_uc8159_init, refresh_12, cursor_none },
    [EPD_PANEL_ATC_UC8179_74_BWR]  = { 800, 480, true,  1, false, EPD_CMD_DATA_10_13, "ATC/Solum 7.4\" BWR UC8179 exp", atc_uc8179_init, refresh_power_on_12, cursor_none },
    [EPD_PANEL_ATC_SSD_97_BWR]     = { 960, 672, true,  0, false, EPD_CMD_DATA_24_26, "ATC/Solum 9.7\" BWR SSD exp", atc_unissd_97_init, refresh_22_f7_20, atc_unissd_97_cursor_home },
    [EPD_PANEL_ATC_PEGHOOK_13_BWR] = { 144, 200, true,  0, false, EPD_CMD_DATA_24_26, "ATC/Solum 1.3\" peghook BWR SSD exp", atc_unissd_init, refresh_22_f7_20, atc_unissd_cursor_home, 8, false },
    [EPD_PANEL_REF_UC8176_42_BW]      = { 400, 300, false, 1, false, EPD_CMD_DATA_UC81XX_REF, "Ref UC8176 4.2\" BW exp", ref_uc81xx_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home },
    [EPD_PANEL_REF_UC8176_42_BWR]     = { 400, 300, true,  1, false, EPD_CMD_DATA_UC81XX_REF, "Ref UC8176 4.2\" BWR exp", ref_uc81xx_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home },
    [EPD_PANEL_REF_UC8179_75_BW]      = { 800, 480, false, 1, false, EPD_CMD_DATA_UC81XX_REF, "Ref UC8179 7.5\" BW exp", ref_uc81xx_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home },
    [EPD_PANEL_REF_UC8179_75_BWR]     = { 800, 480, true,  1, true,  EPD_CMD_DATA_UC81XX_REF, "Ref UC8179 7.5\" BWR exp", ref_uc81xx_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home },
    [EPD_PANEL_REF_UC8159_583_BW]     = { 600, 448, false, 1, false, EPD_CMD_DATA_UC8159_INTERLEAVED, "Ref UC8159 5.83\" BW exp", ref_uc8159_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home },
    [EPD_PANEL_REF_UC8159_75_LOW_BWR] = { 640, 384, true,  1, false, EPD_CMD_DATA_UC8159_INTERLEAVED, "Ref UC8159 7.5\" low BWR exp", ref_uc8159_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home },
    [EPD_PANEL_REF_SSD1677_75_HD_BW]  = { 880, 528, false, 0, false, EPD_CMD_DATA_24_26_SSD16XX, "Ref SSD1677 7.5\" HD BW exp", ref_ssd1677_init, ref_ssd16xx_refresh, ref_ssd1677_cursor_home },
    [EPD_PANEL_REF_SSD1677_75_HD_BWR] = { 880, 528, true,  0, false, EPD_CMD_DATA_24_26_SSD16XX, "Ref SSD1677 7.5\" HD BWR exp", ref_ssd1677_init, ref_ssd16xx_refresh, ref_ssd1677_cursor_home },
    [EPD_PANEL_REF_JD79665_75_BWRY]   = { 800, 480, true,  1, false, EPD_CMD_DATA_JD796XX_2BPP, "Ref JD79665 7.5\" BWRY exp", ref_jd796xx_init, ref_jd796xx_refresh, ref_jd796xx_cursor_home, .has_yellow = true },
    [EPD_PANEL_REF_SSD1619_42_BW]     = { 400, 300, false, 0, false, EPD_CMD_DATA_24_26_SSD16XX, "Ref SSD1619 4.2\" BW exp", ref_ssd1619_init, ref_ssd16xx_refresh, ref_ssd1619_cursor_home },
    [EPD_PANEL_REF_SSD1619_42_BWR]    = { 400, 300, true,  0, false, EPD_CMD_DATA_24_26_SSD16XX, "Ref SSD1619 4.2\" BWR exp", ref_ssd1619_init, ref_ssd16xx_refresh, ref_ssd1619_cursor_home },
    [EPD_PANEL_REF_JD79668_42_BWRY]   = { 400, 300, true,  1, false, EPD_CMD_DATA_JD796XX_2BPP, "Ref JD79668 4.2\" BWRY exp", ref_jd796xx_init, ref_jd796xx_refresh, ref_jd796xx_cursor_home, .has_yellow = true },
    [EPD_PANEL_REF_SSD1683_42_BWR]    = { 400, 300, true,  0, false, EPD_CMD_DATA_24_26_SSD16XX, "Ref SSD1683 4.2\" BWR exp", ref_ssd1683_init, ref_ssd1683_refresh, ref_ssd1683_cursor_home },
    [EPD_PANEL_REF_SSD1683_42_LEGACY] = { 400, 300, true,  0, false, EPD_CMD_DATA_24_26_SSD16XX, "Ref SSD1683 4.2\" BWR exp", ref_ssd1683_init, ref_ssd1683_refresh, ref_ssd1683_cursor_home },
};

static bool panel_valid(epd_panel_t panel)
{
    return panel >= 0 && panel < EPD_PANEL_COUNT && s_specs[panel].name != NULL;
}

static const epd_panel_desc_t *panel_desc(void)
{
    if (!panel_valid(s_panel))
        s_panel = EPD_PANEL_42_BWR;
    return &s_specs[s_panel];
}

static epd_panel_t panel_normalize(epd_panel_t panel)
{
    return panel == EPD_PANEL_REF_SSD1683_42_LEGACY
               ? EPD_PANEL_REF_SSD1683_42_BWR
               : panel;
}

int  epd_width(void)       { return panel_desc()->w; }
int  epd_height(void)      { return panel_desc()->h; }
int  epd_plane_bytes(void) { return panel_desc()->w * panel_desc()->h / 8; }
bool epd_has_red(void)     { return panel_desc()->has_red; }
bool epd_has_yellow(void)  { return panel_desc()->has_yellow; }
epd_panel_t epd_get_panel(void) { return s_panel; }

static bool panel_is_atc_29_flexible(void)
{
    return s_panel == EPD_PANEL_HINK_SSD1619_29_BWR ||
           s_panel == EPD_PANEL_ATC_UC8151_29_BWR;
}

static bool panel_is_hink_29(void)
{
    return s_panel == EPD_PANEL_HINK_SSD1619_29_BWR;
}

static bool panel_is_ref_ssd1683(void)
{
    return s_panel == EPD_PANEL_REF_SSD1683_42_BWR ||
           s_panel == EPD_PANEL_REF_SSD1683_42_LEGACY;
}

static bool panel_needs_inverted_black_plane(void)
{
    return s_panel == EPD_PANEL_HINK_SSD1619_29_BWR;
}

esp_err_t epd_save_panel_config(epd_panel_t panel, int busy_idle)
{
    panel = panel_normalize(panel);
    if (!panel_valid(panel))
        return ESP_ERR_INVALID_ARG;
    if (busy_idle != 0 && busy_idle != 1)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open("epd", NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    e = nvs_set_u8(h, "panel", (uint8_t)panel);
    if (e == ESP_OK)
        e = nvs_set_u8(h, "busy_idle", (uint8_t)busy_idle);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) {
        bool applied_now = (panel == s_panel);
        if (applied_now)
            s_busy_idle_level = busy_idle;
        ESP_LOGI(TAG, "Panel config saved%s: %s, busy_idle=%d",
                 applied_now ? " and applied" : " for next boot",
                 s_specs[panel].name, busy_idle);
    }
    return e;
}

esp_err_t epd_get_saved_panel_config(epd_panel_t *panel, int *busy_idle)
{
    if (!panel && !busy_idle)
        return ESP_ERR_INVALID_ARG;

    epd_panel_t saved_panel = s_panel;
    int saved_busy = busy_idle_effective();

    nvs_handle_t h;
    esp_err_t e = nvs_open("epd", NVS_READONLY, &h);
    if (e == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "panel", &v) == ESP_OK && panel_valid((epd_panel_t)v))
            saved_panel = panel_normalize((epd_panel_t)v);
        uint8_t bi;
        if (nvs_get_u8(h, "busy_idle", &bi) == ESP_OK && bi <= 1)
            saved_busy = (int)bi;
        nvs_close(h);
    } else if (e != ESP_ERR_NVS_NOT_FOUND) {
        return e;
    }

    if (panel)
        *panel = saved_panel;
    if (busy_idle)
        *busy_idle = saved_busy;
    return ESP_OK;
}

esp_err_t epd_set_panel(epd_panel_t panel)
{
    panel = panel_normalize(panel);
    if (!panel_valid(panel))
        return ESP_ERR_INVALID_ARG;

    if (s_epd_mutex)
        xSemaphoreTake(s_epd_mutex, portMAX_DELAY);

    if (s_panel != panel) {
        fb_release_reserved_planes();
        s_panel_seq_done = false;
        ESP_LOGW(TAG, "Panel changed to %s, will re-init on next display",
                 s_specs[panel].name);
    }

    s_panel = panel;

    if (s_epd_mutex)
        xSemaphoreGive(s_epd_mutex);

    nvs_handle_t h;
    if (nvs_open("epd", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "panel", (uint8_t)panel);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Panel -> %s (%dx%d)", s_specs[panel].name,
             s_specs[panel].w, s_specs[panel].h);
    return ESP_OK;
}

int epd_busy_idle_level(void)
{
    return busy_idle_effective();
}

int epd_panel_default_busy_idle(epd_panel_t panel)
{
    if (!panel_valid(panel))
        return -1;
    return s_specs[panel].default_busy_idle;
}

esp_err_t epd_set_busy_idle(int level)
{
    if (level != 0 && level != 1)
        return ESP_ERR_INVALID_ARG;

    s_busy_idle_level = level;

    nvs_handle_t h;
    esp_err_t e = nvs_open("epd", NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    e = nvs_set_u8(h, "busy_idle", (uint8_t)level);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK)
        ESP_LOGI(TAG, "busy_idle -> %d (0=idle low, 1=idle high)", level);
    return e;
}

static void nvs_load_panel(void)
{
    nvs_handle_t h;
    if (nvs_open("epd", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "panel", &v) == ESP_OK) {
            if (panel_valid((epd_panel_t)v)) {
                s_panel = panel_normalize((epd_panel_t)v);
            } else {
                ESP_LOGW(TAG, "Saved panel id %u is no longer supported; using default panel", (unsigned)v);
            }
        }
        uint8_t bi;
        if (nvs_get_u8(h, "busy_idle", &bi) == ESP_OK && bi <= 1)
            s_busy_idle_level = (int)bi;
        nvs_close(h);
    }
}

void epd_load_panel_from_nvs(void)
{
    nvs_load_panel();
}

static int busy_idle_effective(void)
{
    if (s_busy_idle_level == 0 || s_busy_idle_level == 1)
        return s_busy_idle_level;
    return panel_desc()->default_busy_idle;
}

/* ── 延时 / SPI 底层 ──────────────────────────────────────────────── */

static inline void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static esp_err_t spi_tx(const void *data, size_t len_bytes)
{
    spi_transaction_t t = {0};
    t.length = (uint32_t)(len_bytes * 8);
    t.tx_buffer = data;
    return spi_device_transmit(s_spi, &t);
}

static void epd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_EPD_DC, 0);
    esp_err_t e = spi_tx(&cmd, 1);
    if (e != ESP_OK) ESP_LOGE(TAG, "cmd 0x%02X spi err %s", cmd, esp_err_to_name(e));
}

static void epd_dat(uint8_t data)
{
    gpio_set_level(PIN_EPD_DC, 1);
    esp_err_t e = spi_tx(&data, 1);
    if (e != ESP_OK) ESP_LOGE(TAG, "dat spi err %s", esp_err_to_name(e));
}

static void epd_bulk(const uint8_t *data, size_t len)
{
    gpio_set_level(PIN_EPD_DC, 1);

    bool src_is_dma = esp_ptr_dma_capable(data);
    uint8_t *bounce = NULL;
    if (!src_is_dma) {
        bounce = heap_caps_malloc(EPD_FILL_CHUNK, MALLOC_CAP_DMA);
        if (!bounce) {
            ESP_LOGE(TAG, "DMA bounce alloc failed");
            return;
        }
    }

    size_t offset = 0;
    while (offset < len) {
        size_t n = (len - offset > EPD_FILL_CHUNK) ? EPD_FILL_CHUNK : (len - offset);
        const uint8_t *tx_ptr;
        if (bounce) {
            memcpy(bounce, data + offset, n);
            tx_ptr = bounce;
        } else {
            tx_ptr = data + offset;
        }
        esp_err_t e = spi_tx(tx_ptr, n);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "bulk spi err %s", esp_err_to_name(e));
            break;
        }
        offset += n;
    }
    free(bounce);
}

static void epd_fill(uint8_t value, size_t len)
{
    uint8_t *buf = heap_caps_malloc(EPD_FILL_CHUNK, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "epd_fill: DMA malloc failed");
        return;
    }
    memset(buf, value, EPD_FILL_CHUNK);
    gpio_set_level(PIN_EPD_DC, 1);
    size_t offset = 0;
    while (offset < len) {
        size_t n = (len - offset > EPD_FILL_CHUNK) ? EPD_FILL_CHUNK : (len - offset);
        esp_err_t e = spi_tx(buf, n);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "fill spi err %s", esp_err_to_name(e));
            break;
        }
        offset += n;
    }
    free(buf);
}

/* ── 复位 / BUSY ─────────────────────────────────────────────────── */

static void epd_hw_reset(void)
{
    gpio_set_level(PIN_EPD_RST, 1);
    delay_ms(20);
    gpio_set_level(PIN_EPD_RST, 0);
    delay_ms(40);
    gpio_set_level(PIN_EPD_RST, 1);
    delay_ms(200);
}

/**
 * 等待 BUSY 释放。idle 电平见 busy_idle_effective()。
 * UC8179：轮询前可发 0x71；SSD1619 一般直接读 GPIO。
 *
 * after_update_cmd：发完 0x20（SSD1619）/0x12（UC8179 等）刷新命令后须置 true。
 * 否则 BUSY 可能仍短暂保持“空闲”电平（上拉），会误判为已就绪（仅 ~10ms 就返回）。
 */
static void epd_wait_ready(bool after_update_cmd)
{
    delay_ms(10);
    const bool probe_0x71 = panel_desc()->busy_probe_0x71;
    int idle = busy_idle_effective();
    const bool flexible_busy = panel_is_atc_29_flexible();
    const int timeout_ms = flexible_busy ? EPD_ATC29_BUSY_TIMEOUT_MS : EPD_BUSY_TIMEOUT_MS;

    if (probe_0x71)
        epd_cmd(0x71);

    /* 阶段 1：等到离开空闲态（进入忙），避免刷新命令刚发出时仍读到空闲 */
    if (after_update_cmd) {
        int phase1 = 0;
        const int phase1_max_ms = 800;
        while (gpio_get_level(PIN_EPD_BUSY) == idle && phase1 < phase1_max_ms) {
            delay_ms(20);
            phase1 += 20;
            if (probe_0x71)
                epd_cmd(0x71);
        }
        if (phase1 >= phase1_max_ms && !flexible_busy)
            ESP_LOGW(TAG, "BUSY: no busy phase after update (still idle %d ms). "
                          "Check GPIO%d / NVS epd/busy_idle (0=Waveshare idle-low).",
                     phase1_max_ms, PIN_EPD_BUSY);
    }

    if (flexible_busy && !after_update_cmd && gpio_get_level(PIN_EPD_BUSY) != idle) {
        int busy_level = gpio_get_level(PIN_EPD_BUSY);
        delay_ms(80);
        if (gpio_get_level(PIN_EPD_BUSY) == busy_level) {
            ESP_LOGW(TAG, "%s: BUSY stayed at %d while idle=%d; "
                          "using flexible idle=%d for this boot",
                     panel_desc()->name, busy_level, idle, busy_level);
            idle = busy_level;
        }
    }

    int waited = 0;
    bool timed_out = false;
    while (gpio_get_level(PIN_EPD_BUSY) != idle) {
        if (probe_0x71)
            epd_cmd(0x71);
        delay_ms(200);
        waited += 200;
        if (waited >= timeout_ms) {
            ESP_LOGW(TAG, "BUSY timeout %d ms (expect idle=%d, gpio=%d). "
                          "Try NVS key busy_idle=0 or 1 on namespace 'epd'. "
                          "Display may continue with conservative fixed waits.",
                     waited, idle, (int)gpio_get_level(PIN_EPD_BUSY));
            timed_out = true;
            break;
        }
    }
    if (!timed_out && waited > 200)
        ESP_LOGI(TAG, "ready after %d ms", waited);
}

/* ── Waveshare-style helpers ───────────────────────────────────────── */

static void cmd1(uint8_t c, uint8_t v1)
{
    epd_cmd(c);
    epd_dat(v1);
}

static void cmd2(uint8_t c, uint8_t v1, uint8_t v2)
{
    epd_cmd(c);
    epd_dat(v1);
    epd_dat(v2);
}

static void cmd3(uint8_t c, uint8_t v1, uint8_t v2, uint8_t v3)
{
    epd_cmd(c);
    epd_dat(v1);
    epd_dat(v2);
    epd_dat(v3);
}

static void cmd4(uint8_t c, uint8_t v1, uint8_t v2, uint8_t v3, uint8_t v4)
{
    epd_cmd(c);
    epd_dat(v1);
    epd_dat(v2);
    epd_dat(v3);
    epd_dat(v4);
}

static void cmd5(uint8_t c, uint8_t v1, uint8_t v2, uint8_t v3, uint8_t v4, uint8_t v5)
{
    epd_cmd(c);
    epd_dat(v1);
    epd_dat(v2);
    epd_dat(v3);
    epd_dat(v4);
    epd_dat(v5);
}

static void cursor_none(void)
{
}

static void atc_unissd_cursor_home(void)
{
    const epd_panel_desc_t *d = panel_desc();
    uint16_t y = d->mirror_v ? 0u : (uint16_t)(d->h - 1u);
    epd_cmd(0x4E);
    epd_dat((uint8_t)(d->x_offset / 8u));
    epd_cmd(0x4F);
    epd_dat((uint8_t)y);
    epd_dat((uint8_t)(y >> 8));
}

static void hink_ssd1619_29_cursor_home(void)
{
    const epd_panel_desc_t *d = panel_desc();
    uint16_t y = (uint16_t)(HINK29_LOGICAL_H - 1u);
    epd_cmd(0x4E);
    epd_dat((uint8_t)(d->x_offset / 8u));
    epd_cmd(0x4F);
    epd_dat((uint8_t)y);
    epd_dat((uint8_t)(y >> 8));
}

static void atc_unissd_97_cursor_home(void)
{
    epd_cmd(0x4E);
    epd_dat(0xBF);
    epd_dat(0x03);
    epd_cmd(0x4F);
    epd_dat(0x00);
    epd_dat(0x00);
}

static void ref_send_tres(void)
{
    const epd_panel_desc_t *d = panel_desc();
    cmd4(0x61, (uint8_t)(d->w >> 8), (uint8_t)d->w,
         (uint8_t)(d->h >> 8), (uint8_t)d->h);
}

static void ref_uc81xx_set_window(int x, int y, int w, int h)
{
    int xe = (x + w - 1) | 0x07;
    int ye = y + h - 1;
    x &= ~0x07;

    epd_cmd(0x90);
    epd_dat((uint8_t)(x >> 8));
    epd_dat((uint8_t)x);
    epd_dat((uint8_t)(xe >> 8));
    epd_dat((uint8_t)xe);
    epd_dat((uint8_t)(y >> 8));
    epd_dat((uint8_t)y);
    epd_dat((uint8_t)(ye >> 8));
    epd_dat((uint8_t)ye);
    epd_dat(0x00);
}

static void ref_uc81xx_full_window(void)
{
    const epd_panel_desc_t *d = panel_desc();
    ref_uc81xx_set_window(0, 0, d->w, d->h);
}

static void ref_ssd1619_full_window(void)
{
    const epd_panel_desc_t *d = panel_desc();
    uint16_t x0 = 0;
    uint16_t x1 = (uint16_t)(d->w - 1);
    uint16_t y0 = 0;
    uint16_t y1 = (uint16_t)(d->h - 1);

    cmd1(0x11, 0x03);
    cmd2(0x44, (uint8_t)(x0 / 8u), (uint8_t)(x1 / 8u));
    cmd4(0x45, (uint8_t)y0, (uint8_t)(y0 >> 8),
         (uint8_t)y1, (uint8_t)(y1 >> 8));
    cmd1(0x4E, (uint8_t)(x0 / 8u));
    cmd2(0x4F, (uint8_t)y0, (uint8_t)(y0 >> 8));
}

static void ref_ssd1683_set_window(int x, int y, int w, int h)
{
    const epd_panel_desc_t *d = panel_desc();
    int x0 = x & ~0x07;
    int x1 = (x + w - 1) | 0x07;
    int y0 = y;
    int y1 = y + h - 1;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= d->w) x1 = d->w - 1;
    if (y1 >= d->h) y1 = d->h - 1;

    cmd1(0x11, 0x03);
    cmd2(0x44, (uint8_t)(x0 / 8), (uint8_t)(x1 / 8));
    cmd4(0x45, (uint8_t)y0, (uint8_t)(y0 >> 8),
         (uint8_t)y1, (uint8_t)(y1 >> 8));
    cmd1(0x4E, (uint8_t)(x0 / 8));
    cmd2(0x4F, (uint8_t)y0, (uint8_t)(y0 >> 8));
}

static void ref_ssd1683_full_window(void)
{
    const epd_panel_desc_t *d = panel_desc();
    ref_ssd1683_set_window(0, 0, d->w, d->h);
}

static void ref_ssd1677_full_window(void)
{
    const epd_panel_desc_t *d = panel_desc();
    uint16_t x0 = 0;
    uint16_t x1 = (uint16_t)(d->w - 1);
    uint16_t y0 = 0;
    uint16_t y1 = (uint16_t)(d->h - 1);

    cmd1(0x11, 0x03);
    cmd4(0x44, (uint8_t)x0, (uint8_t)(x0 >> 8),
         (uint8_t)x1, (uint8_t)(x1 >> 8));
    cmd4(0x45, (uint8_t)y0, (uint8_t)(y0 >> 8),
         (uint8_t)y1, (uint8_t)(y1 >> 8));
    cmd2(0x4E, (uint8_t)x0, (uint8_t)(x0 >> 8));
    cmd2(0x4F, (uint8_t)y0, (uint8_t)(y0 >> 8));
}

static void ref_jd796xx_full_window(void)
{
    const epd_panel_desc_t *d = panel_desc();
    uint16_t x0 = 0;
    uint16_t x1 = (uint16_t)(d->w - 1);
    uint16_t y0 = 0;
    uint16_t y1 = (uint16_t)(d->h - 1);

    epd_cmd(0x83);
    epd_dat((uint8_t)(x0 >> 8));
    epd_dat((uint8_t)x0);
    epd_dat((uint8_t)(x1 >> 8));
    epd_dat((uint8_t)x1);
    epd_dat((uint8_t)(y0 >> 8));
    epd_dat((uint8_t)y0);
    epd_dat((uint8_t)(y1 >> 8));
    epd_dat((uint8_t)y1);
    epd_dat(0x01);
}

static void ref_uc81xx_cursor_home(void)
{
}

static void ref_ssd1619_cursor_home(void)
{
    ref_ssd1619_full_window();
}

static void ref_ssd1683_cursor_home(void)
{
    ref_ssd1683_full_window();
}

static void ref_ssd1677_cursor_home(void)
{
    ref_ssd1677_full_window();
}

static void ref_jd796xx_cursor_home(void)
{
    ref_jd796xx_full_window();
}

static void refresh_22_f7_20(void)
{
    cmd1(0x22, 0xF7);
    epd_cmd(0x20);
    epd_wait_ready(true);
    delay_ms(panel_is_atc_29_flexible() ? EPD_ATC29_SAFE_REFRESH_MS : 200);
}

static void hink_ssd1619_29_refresh(void)
{
    cmd1(0x22, 0xCF);
    epd_cmd(0x20);
    /*
     * HINK-E029A14-A1 real boards have shown BUSY staying high during the
     * full refresh even when the image is displayed correctly.  Polling it
     * here creates a false timeout, so use a conservative fixed refresh wait.
     */
    delay_ms(HINK29_REFRESH_MS);
}

static void refresh_12(void)
{
    epd_cmd(0x12);
    delay_ms(100);
    epd_wait_ready(true);
    delay_ms(200);
}

static void refresh_power_on_12(void)
{
    epd_cmd(0x04);
    delay_ms(20);
    epd_wait_ready(false);
    epd_cmd(0x12);
    delay_ms(100);
    epd_wait_ready(true);
    delay_ms(panel_is_atc_29_flexible() ? EPD_ATC29_SAFE_REFRESH_MS : 200);
}

static void ref_uc81xx_refresh(void)
{
    epd_cmd(0x04);
    delay_ms(20);
    epd_wait_ready(false);
    epd_cmd(0x91);
    ref_uc81xx_full_window();
    epd_cmd(0x12);
    delay_ms(100);
    epd_wait_ready(true);
    epd_cmd(0x92);
    epd_cmd(0x02);
    epd_wait_ready(false);
    delay_ms(200);
}

static void ref_ssd16xx_refresh(void)
{
    cmd2(0x21, panel_desc()->has_red ? 0x80 : 0x40, 0x00);
    cmd1(0x22, 0xF7);
    epd_cmd(0x20);
    epd_wait_ready(true);
    if (s_panel == EPD_PANEL_REF_SSD1619_42_BW || s_panel == EPD_PANEL_REF_SSD1619_42_BWR)
        ref_ssd1619_full_window();
    else
        ref_ssd1677_full_window();
    cmd1(0x22, 0x83);
    epd_cmd(0x20);
    delay_ms(200);
}

static uint32_t ref_ssd1683_update(uint8_t ctrl2, const char *label)
{
    ESP_LOGI(TAG, "SSD1683 %s start: ctrl2=0x%02X", label, ctrl2);
    epd_cmd(0x22);
    epd_dat(ctrl2);
    delay_ms(2);
    epd_cmd(0x20);

    TickType_t t0 = xTaskGetTickCount();
    epd_wait_ready(true);
    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - t0);

    if (elapsed_ms < 100) {
        ESP_LOGW(TAG, "SSD1683 %s BUSY only %u ms, waiting 2s safety",
                 label, (unsigned)elapsed_ms);
        delay_ms(2000);
    } else {
        delay_ms(200);
    }
    ESP_LOGI(TAG, "SSD1683 %s done in %u ms", label, (unsigned)elapsed_ms);
    return elapsed_ms;
}

static void ref_ssd1683_refresh(void)
{
    ref_ssd1683_update(0xF7, "full refresh");
    ref_ssd1683_full_window();
}

static void ref_jd796xx_refresh(void)
{
    ref_jd796xx_full_window();
    epd_cmd(0x12);
    delay_ms(100);
    epd_wait_ready(true);
    delay_ms(200);
}

static void refresh_atc_dualssd(void)
{
    epd_cmd(0x20);
    epd_wait_ready(true);
    delay_ms(200);
}

static void panel_write_plane(uint8_t cmd, const uint8_t *data, uint8_t fill, bool invert, int pb)
{
    epd_cmd(cmd);
    if (data && !invert) {
        epd_bulk(data, (size_t)pb);
        return;
    }
    if (!data) {
        epd_fill(fill, (size_t)pb);
        return;
    }

    uint8_t *buf = heap_caps_malloc(EPD_FILL_CHUNK, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "invert plane: DMA malloc failed");
        return;
    }
    size_t offset = 0;
    while (offset < (size_t)pb) {
        size_t n = ((size_t)pb - offset > EPD_FILL_CHUNK) ? EPD_FILL_CHUNK : ((size_t)pb - offset);
        for (size_t i = 0; i < n; i++)
            buf[i] = (uint8_t)~data[offset + i];
        epd_bulk(buf, n);
        offset += n;
    }
    free(buf);
}

static void hink_ssd1619_write_plane(uint8_t cmd, const uint8_t *data,
                                     uint8_t fill, bool invert, int pb)
{
    const epd_panel_desc_t *d = panel_desc();
    int rb = d->w / 8;
    if (d->w != HINK29_LOGICAL_W || d->h != HINK29_LOGICAL_H ||
        rb <= 0 || rb > 64 || pb != rb * d->h) {
        ESP_LOGE(TAG, "HINK plane geometry invalid: rb=%d pb=%d panel=%dx%d",
                 rb, pb, d->w, d->h);
        return;
    }

    uint8_t line[64];
    epd_cmd(cmd);
    for (int y = 0; y < d->h; y++) {
        const size_t row = (size_t)y * (size_t)rb;
        for (int x = 0; x < rb; x++) {
            uint8_t v = data ? data[row + (size_t)x] : fill;
            if (invert)
                v = (uint8_t)~v;
            line[x] = v;
        }
        epd_bulk(line, (size_t)rb);
    }
}

static esp_err_t hink_ssd1619_file_plane(uint8_t cmd, FILE *f, int pb, bool has_data,
                                         bool invert, uint8_t fill)
{
    uint8_t *plane = NULL;
    if (has_data) {
        plane = heap_caps_malloc((size_t)pb, MALLOC_CAP_8BIT);
        if (!plane) {
            ESP_LOGE(TAG, "HINK file plane malloc failed: %d", pb);
            return ESP_ERR_NO_MEM;
        }
        if (fread(plane, 1, (size_t)pb, f) != (size_t)pb) {
            free(plane);
            ESP_LOGE(TAG, "HINK file plane short read");
            return ESP_ERR_INVALID_SIZE;
        }
    }
    hink_ssd1619_write_plane(cmd, plane, fill, invert, pb);
    free(plane);
    return ESP_OK;
}

static uint8_t plane_byte_or_fill(const uint8_t *data, size_t index, uint8_t fill)
{
    return data ? data[index] : fill;
}

static bool plane_bit_set(uint8_t byte, int bit)
{
    return (byte & (uint8_t)(0x80u >> bit)) != 0;
}

static void panel_write_dualssd_plane(uint8_t cmd, const uint8_t *data,
                                      uint8_t fill, bool right_half)
{
    int rb = epd_width() / 8;
    int len = rb / 2 + 1;
    int start = right_half ? rb / 2 : 0;
    int H = epd_height();

    if (len <= 0 || len > 128) {
        ESP_LOGE(TAG, "dual SSD unsupported row split: rb=%d len=%d", rb, len);
        return;
    }

    uint8_t line[128];
    epd_cmd(cmd);
    for (int y = 0; y < H; y++) {
        const size_t row = (size_t)y * (size_t)rb;
        for (int i = 0; i < len; i++)
            line[i] = plane_byte_or_fill(data, row + (size_t)start + (size_t)i, fill);
        epd_bulk(line, (size_t)len);
    }
}

static void panel_write_dualssd_data(const uint8_t *bw, const uint8_t *red,
                                     uint8_t bw_fill, uint8_t red_fill)
{
    const epd_panel_desc_t *d = panel_desc();

    panel_write_dualssd_plane(0x24, bw, bw_fill, true);
    if (d->has_red)
        panel_write_dualssd_plane(0x26, red, red_fill, true);
    panel_write_dualssd_plane(0xA4, bw, bw_fill, false);
    if (d->has_red)
        panel_write_dualssd_plane(0xA6, red, red_fill, false);
}

static void uc8159_interleave_color(uint8_t *dst, uint8_t bw, uint8_t red)
{
    for (int pair = 0; pair < 4; pair++) {
        int bit0 = pair * 2;
        int bit1 = bit0 + 1;
        uint8_t out = 0;
        bool r0 = plane_bit_set(red, bit0);
        bool b0 = !plane_bit_set(bw, bit0);
        bool r1 = plane_bit_set(red, bit1);
        bool b1 = !plane_bit_set(bw, bit1);

        out |= (uint8_t)((r0 ? 0x04 : (b0 ? 0x00 : 0x03)) << 4);
        out |= (uint8_t)(r1 ? 0x04 : (b1 ? 0x00 : 0x03));
        *dst++ = out;
    }
}

static void panel_write_uc8159_interleaved(const uint8_t *bw, const uint8_t *red,
                                           uint8_t bw_fill, uint8_t red_fill)
{
    int rb = epd_width() / 8;
    int H = epd_height();
    uint8_t *line = heap_caps_malloc((size_t)rb * 4u, MALLOC_CAP_DMA);
    if (!line) {
        ESP_LOGE(TAG, "UC8159 line DMA malloc failed");
        return;
    }

    epd_cmd(0x10);
    for (int y = 0; y < H; y++) {
        const size_t row = (size_t)y * (size_t)rb;
        for (int x = 0; x < rb; x++) {
            uint8_t b = plane_byte_or_fill(bw, row + (size_t)x, bw_fill);
            uint8_t r = plane_byte_or_fill(red, row + (size_t)x, red_fill);
            uc8159_interleave_color(line + (size_t)x * 4u, b, r);
        }
        epd_bulk(line, (size_t)rb * 4u);
    }
    free(line);
    epd_cmd(0x11);
}

static uint8_t jd796xx_pack4(uint8_t bw, uint8_t red, uint8_t yellow, int start_bit)
{
    uint8_t out = 0;
    for (int i = 0; i < 4; i++) {
        int bit = start_bit + i;
        uint8_t px = 0x01; /* white, matching the reference clear pattern 0x55 */
        if (!plane_bit_set(bw, bit))
            px = 0x00;     /* black */
        else if (plane_bit_set(yellow, bit))
            px = 0x02;     /* yellow */
        else if (plane_bit_set(red, bit))
            px = 0x03;     /* red */
        out = (uint8_t)((out << 2) | px);
    }
    return out;
}

static void panel_write_jd796xx_2bpp(const uint8_t *bw, const uint8_t *red,
                                     const uint8_t *yellow,
                                     uint8_t bw_fill, uint8_t red_fill,
                                     uint8_t yellow_fill)
{
    int rb = epd_width() / 8;
    int H = epd_height();
    uint8_t *line = heap_caps_malloc((size_t)rb * 2u, MALLOC_CAP_DMA);
    if (!line) {
        ESP_LOGE(TAG, "JD796xx line DMA malloc failed");
        return;
    }

    epd_cmd(0x10);
    for (int y = 0; y < H; y++) {
        const size_t row = (size_t)y * (size_t)rb;
        for (int x = 0; x < rb; x++) {
            uint8_t b = plane_byte_or_fill(bw, row + (size_t)x, bw_fill);
            uint8_t r = plane_byte_or_fill(red, row + (size_t)x, red_fill);
            uint8_t yel = plane_byte_or_fill(yellow, row + (size_t)x, yellow_fill);
            line[(size_t)x * 2u] = jd796xx_pack4(b, r, yel, 0);
            line[(size_t)x * 2u + 1u] = jd796xx_pack4(b, r, yel, 4);
        }
        epd_bulk(line, (size_t)rb * 2u);
    }
    free(line);
}

static void panel_write_bw_red_data(const uint8_t *bw, const uint8_t *red, int pb)
{
    const epd_panel_desc_t *d = panel_desc();
    if (d->cursor_home)
        d->cursor_home();

    switch (d->data_mode) {
    case EPD_CMD_DATA_24_26:
        if (panel_is_hink_29()) {
            if (d->has_red) {
                hink_ssd1619_write_plane(0x26, red, 0x00, false, pb);
                if (d->cursor_home)
                    d->cursor_home();
            }
            hink_ssd1619_write_plane(0x24, bw, 0xFF, true, pb);
            break;
        }
        panel_write_plane(0x24, bw, 0xFF, panel_needs_inverted_black_plane(), pb);
        if (d->has_red) {
            if (d->cursor_home)
                d->cursor_home();
            panel_write_plane(0x26, red, 0x00, false, pb);
        }
        break;
    case EPD_CMD_DATA_24_26_SSD16XX:
        panel_write_plane(0x24, bw, 0xFF, false, pb);
        if (d->has_red) {
            if (d->cursor_home)
                d->cursor_home();
            if (panel_is_ref_ssd1683())
                panel_write_plane(0x26, red, 0x00, false, pb);
            else
                panel_write_plane(0x26, red, 0xFF, true, pb);
        } else {
            if (d->cursor_home)
                d->cursor_home();
            panel_write_plane(0x26, bw, 0xFF, false, pb);
        }
        break;
    case EPD_CMD_DATA_10_13:
        panel_write_plane(0x10, bw, 0xFF, false, pb);
        panel_write_plane(0x13, d->has_red ? red : bw, d->has_red ? 0x00 : 0xFF, false, pb);
        break;
    case EPD_CMD_DATA_10_92_13:
    case EPD_CMD_DATA_UC81XX_REF:
        epd_cmd(0x91);
        ref_uc81xx_full_window();
        panel_write_plane(0x10, d->has_red ? bw : NULL, 0xFF, false, pb);
        epd_cmd(0x92);
        epd_cmd(0x91);
        ref_uc81xx_full_window();
        panel_write_plane(0x13, d->has_red ? red : bw, d->has_red ? 0xFF : 0xFF, d->has_red, pb);
        epd_cmd(0x92);
        break;
    case EPD_CMD_DATA_10_13_INV_RED:
        panel_write_plane(0x10, bw, 0xFF, false, pb);
        panel_write_plane(0x13, red, 0xFF, true, pb);
        break;
    case EPD_CMD_DATA_10_13_DUMMY:
        epd_dat(0x00);
        panel_write_plane(0x10, bw, 0xFF, false, pb);
        panel_write_plane(0x13, d->has_red ? red : NULL, d->has_red ? 0x00 : 0xFF, false, pb);
        break;
    case EPD_CMD_DATA_10_13_INV_BLACK_DUMMY:
        epd_dat(0x00);
        panel_write_plane(0x10, bw, 0xFF, true, pb);
        panel_write_plane(0x13, red, 0x00, false, pb);
        break;
    case EPD_CMD_DATA_DUAL_SSD:
        panel_write_dualssd_data(bw, red, 0xFF, 0x00);
        break;
    case EPD_CMD_DATA_UC8159_INTERLEAVED:
        panel_write_uc8159_interleaved(bw, red, 0xFF, 0x00);
        break;
    case EPD_CMD_DATA_JD796XX_2BPP:
        panel_write_jd796xx_2bpp(bw, red, NULL, 0xFF, 0x00, 0x00);
        break;
    }
}

static void panel_write_bw_red(const uint8_t *bw, const uint8_t *red, int pb)
{
    panel_write_bw_red_data(bw, red, pb);
    panel_desc()->refresh();
}

static esp_err_t file_send_plane(FILE *f, int pb, bool invert)
{
    uint8_t *buf = heap_caps_malloc(EPD_FILL_CHUNK, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "file plane DMA malloc failed");
        return ESP_ERR_NO_MEM;
    }

    gpio_set_level(PIN_EPD_DC, 1);
    size_t sent = 0;
    esp_err_t ret = ESP_OK;
    while (sent < (size_t)pb) {
        size_t n = ((size_t)pb - sent > EPD_FILL_CHUNK) ? EPD_FILL_CHUNK : ((size_t)pb - sent);
        size_t got = fread(buf, 1, n, f);
        if (got != n) {
            ESP_LOGE(TAG, "file plane short: %u / %u", (unsigned)got, (unsigned)n);
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (invert) {
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint8_t)~buf[i];
        }
        ret = spi_tx(buf, n);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "file plane spi err %s", esp_err_to_name(ret));
            break;
        }
        sent += n;
    }

    free(buf);
    return ret;
}

static esp_err_t file_read_row(FILE *f, long base, int row, int rb,
                               uint8_t *dst, uint8_t fill)
{
    if (base < 0) {
        memset(dst, fill, (size_t)rb);
        return ESP_OK;
    }
    if (fseek(f, base + (long)row * rb, SEEK_SET) != 0)
        return ESP_FAIL;
    if (fread(dst, 1, (size_t)rb, f) != (size_t)rb)
        return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

static esp_err_t file_send_dualssd_plane(FILE *f, long base, int rb, int H,
                                         uint8_t fill, uint8_t cmd,
                                         bool right_half)
{
    int len = rb / 2 + 1;
    int start = right_half ? rb / 2 : 0;
    uint8_t *row = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    uint8_t *out = heap_caps_malloc((size_t)len, MALLOC_CAP_DMA);
    if (!row || !out) {
        free(row);
        free(out);
        return ESP_ERR_NO_MEM;
    }

    epd_cmd(cmd);
    esp_err_t ret = ESP_OK;
    for (int y = 0; y < H; y++) {
        ret = file_read_row(f, base, y, rb, row, fill);
        if (ret != ESP_OK)
            break;
        memcpy(out, row + start, (size_t)len);
        epd_bulk(out, (size_t)len);
    }

    free(row);
    free(out);
    return ret;
}

static esp_err_t file_send_dualssd(FILE *f, int pb, bool has_red_file)
{
    const epd_panel_desc_t *d = panel_desc();
    int rb = d->w / 8;
    long red_base = has_red_file ? (long)pb : -1;
    esp_err_t ret = file_send_dualssd_plane(f, 0, rb, d->h, 0xFF, 0x24, true);
    if (ret == ESP_OK && d->has_red)
        ret = file_send_dualssd_plane(f, red_base, rb, d->h, 0x00, 0x26, true);
    if (ret == ESP_OK)
        ret = file_send_dualssd_plane(f, 0, rb, d->h, 0xFF, 0xA4, false);
    if (ret == ESP_OK && d->has_red)
        ret = file_send_dualssd_plane(f, red_base, rb, d->h, 0x00, 0xA6, false);
    return ret;
}

static esp_err_t file_send_uc8159_interleaved(FILE *f, int pb, bool has_red_file)
{
    int rb = epd_width() / 8;
    int H = epd_height();
    uint8_t *bw = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    uint8_t *red = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    uint8_t *out = heap_caps_malloc((size_t)rb * 4u, MALLOC_CAP_DMA);
    if (!bw || !red || !out) {
        free(bw);
        free(red);
        free(out);
        return ESP_ERR_NO_MEM;
    }

    long red_base = has_red_file ? (long)pb : -1;
    epd_cmd(0x10);
    esp_err_t ret = ESP_OK;
    for (int y = 0; y < H; y++) {
        ret = file_read_row(f, 0, y, rb, bw, 0xFF);
        if (ret != ESP_OK)
            break;
        ret = file_read_row(f, red_base, y, rb, red, 0x00);
        if (ret != ESP_OK)
            break;
        for (int x = 0; x < rb; x++)
            uc8159_interleave_color(out + (size_t)x * 4u, bw[x], red[x]);
        epd_bulk(out, (size_t)rb * 4u);
    }
    epd_cmd(0x11);

    free(bw);
    free(red);
    free(out);
    return ret;
}

static esp_err_t file_send_jd796xx_2bpp(FILE *f, int pb, bool has_red_file,
                                        bool has_yellow_file)
{
    int rb = epd_width() / 8;
    int H = epd_height();
    uint8_t *bw = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    uint8_t *red = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    uint8_t *yellow = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    uint8_t *out = heap_caps_malloc((size_t)rb * 2u, MALLOC_CAP_DMA);
    if (!bw || !red || !yellow || !out) {
        free(bw);
        free(red);
        free(yellow);
        free(out);
        return ESP_ERR_NO_MEM;
    }

    long red_base = has_red_file ? (long)pb : -1;
    long yellow_base = has_yellow_file ? (long)pb * 2L : -1;
    epd_cmd(0x10);
    esp_err_t ret = ESP_OK;
    for (int y = 0; y < H; y++) {
        ret = file_read_row(f, 0, y, rb, bw, 0xFF);
        if (ret != ESP_OK)
            break;
        ret = file_read_row(f, red_base, y, rb, red, 0x00);
        if (ret != ESP_OK)
            break;
        ret = file_read_row(f, yellow_base, y, rb, yellow, 0x00);
        if (ret != ESP_OK)
            break;
        for (int x = 0; x < rb; x++) {
            out[(size_t)x * 2u] = jd796xx_pack4(bw[x], red[x], yellow[x], 0);
            out[(size_t)x * 2u + 1u] = jd796xx_pack4(bw[x], red[x], yellow[x], 4);
        }
        epd_bulk(out, (size_t)rb * 2u);
    }

    free(bw);
    free(red);
    free(yellow);
    free(out);
    return ret;
}

static esp_err_t panel_write_bw_red_file(FILE *f, int pb, bool has_red_file,
                                         bool has_yellow_file)
{
    const epd_panel_desc_t *d = panel_desc();
    esp_err_t ret = ESP_OK;
    if (d->cursor_home)
        d->cursor_home();

    switch (d->data_mode) {
    case EPD_CMD_DATA_24_26:
        if (panel_is_hink_29()) {
            if (has_red_file && fseek(f, (long)pb, SEEK_SET) != 0)
                return ESP_FAIL;
            ret = hink_ssd1619_file_plane(0x26, f, pb, has_red_file, false, 0x00);
            if (ret != ESP_OK)
                return ret;
            if (fseek(f, 0, SEEK_SET) != 0)
                return ESP_FAIL;
            if (d->cursor_home)
                d->cursor_home();
            ret = hink_ssd1619_file_plane(0x24, f, pb, true, true, 0xFF);
        } else {
            epd_cmd(0x24);
            ret = file_send_plane(f, pb, panel_needs_inverted_black_plane());
            if (ret != ESP_OK)
                return ret;
            if (d->has_red) {
                if (d->cursor_home)
                    d->cursor_home();
                epd_cmd(0x26);
                ret = has_red_file ? file_send_plane(f, pb, false)
                                   : (epd_fill(0x00, (size_t)pb), ESP_OK);
            }
        }
        break;
    case EPD_CMD_DATA_24_26_SSD16XX:
        epd_cmd(0x24);
        ret = file_send_plane(f, pb, false);
        if (ret != ESP_OK)
            return ret;
        if (d->cursor_home)
            d->cursor_home();
        epd_cmd(0x26);
        if (d->has_red) {
            if (panel_is_ref_ssd1683())
                ret = has_red_file ? file_send_plane(f, pb, false)
                                   : (epd_fill(0x00, (size_t)pb), ESP_OK);
            else
                ret = has_red_file ? file_send_plane(f, pb, true)
                                   : (epd_fill(0xFF, (size_t)pb), ESP_OK);
        } else {
            if (fseek(f, 0, SEEK_SET) != 0)
                return ESP_FAIL;
            ret = file_send_plane(f, pb, false);
        }
        break;
    case EPD_CMD_DATA_10_13:
        epd_cmd(0x10);
        ret = file_send_plane(f, pb, false);
        if (ret != ESP_OK)
            return ret;
        epd_cmd(0x13);
        if (d->has_red) {
            ret = has_red_file ? file_send_plane(f, pb, false)
                               : (epd_fill(0x00, (size_t)pb), ESP_OK);
        } else {
            if (fseek(f, 0, SEEK_SET) != 0)
                return ESP_FAIL;
            ret = file_send_plane(f, pb, false);
        }
        break;
    case EPD_CMD_DATA_10_92_13:
    case EPD_CMD_DATA_UC81XX_REF:
        epd_cmd(0x91);
        ref_uc81xx_full_window();
        epd_cmd(0x10);
        if (d->has_red) {
            ret = file_send_plane(f, pb, false);
            if (ret != ESP_OK)
                return ret;
        } else {
            epd_fill(0xFF, (size_t)pb);
        }
        epd_cmd(0x92);
        epd_cmd(0x91);
        ref_uc81xx_full_window();
        epd_cmd(0x13);
        if (d->has_red) {
            ret = has_red_file ? file_send_plane(f, pb, true)
                               : (epd_fill(0xFF, (size_t)pb), ESP_OK);
        } else {
            if (fseek(f, 0, SEEK_SET) != 0)
                return ESP_FAIL;
            ret = file_send_plane(f, pb, false);
        }
        epd_cmd(0x92);
        break;
    case EPD_CMD_DATA_10_13_INV_RED:
        epd_cmd(0x10);
        ret = file_send_plane(f, pb, false);
        if (ret != ESP_OK)
            return ret;
        epd_cmd(0x13);
        ret = has_red_file ? file_send_plane(f, pb, true)
                           : (epd_fill(0xFF, (size_t)pb), ESP_OK);
        break;
    case EPD_CMD_DATA_10_13_DUMMY:
        epd_dat(0x00);
        epd_cmd(0x10);
        ret = file_send_plane(f, pb, false);
        if (ret != ESP_OK)
            return ret;
        epd_cmd(0x13);
        ret = has_red_file ? file_send_plane(f, pb, false)
                           : (epd_fill(d->has_red ? 0x00 : 0xFF, (size_t)pb), ESP_OK);
        break;
    case EPD_CMD_DATA_10_13_INV_BLACK_DUMMY:
        epd_dat(0x00);
        epd_cmd(0x10);
        ret = file_send_plane(f, pb, true);
        if (ret != ESP_OK)
            return ret;
        epd_cmd(0x13);
        ret = has_red_file ? file_send_plane(f, pb, false)
                           : (epd_fill(0x00, (size_t)pb), ESP_OK);
        break;
    case EPD_CMD_DATA_DUAL_SSD:
        ret = file_send_dualssd(f, pb, has_red_file);
        break;
    case EPD_CMD_DATA_UC8159_INTERLEAVED:
        ret = file_send_uc8159_interleaved(f, pb, has_red_file);
        break;
    case EPD_CMD_DATA_JD796XX_2BPP:
        ret = file_send_jd796xx_2bpp(f, pb, has_red_file, has_yellow_file);
        break;
    }

    if (ret == ESP_OK)
        d->refresh();
    return ret;
}

static void panel_fill_bw_red(uint8_t bw_byte, uint8_t red_byte, int pb)
{
    const epd_panel_desc_t *d = panel_desc();
    if (d->cursor_home)
        d->cursor_home();

    switch (d->data_mode) {
    case EPD_CMD_DATA_24_26:
        if (panel_is_hink_29()) {
            if (d->has_red) {
                hink_ssd1619_write_plane(0x26, NULL, red_byte, false, pb);
                if (d->cursor_home)
                    d->cursor_home();
            }
            hink_ssd1619_write_plane(0x24, NULL, bw_byte, true, pb);
            break;
        }
        panel_write_plane(0x24, NULL,
                          panel_needs_inverted_black_plane() ? (uint8_t)~bw_byte : bw_byte,
                          false, pb);
        if (d->has_red) {
            if (d->cursor_home)
                d->cursor_home();
            panel_write_plane(0x26, NULL, red_byte, false, pb);
        }
        break;
    case EPD_CMD_DATA_24_26_SSD16XX:
        panel_write_plane(0x24, NULL, bw_byte, false, pb);
        if (d->cursor_home)
            d->cursor_home();
        if (d->has_red && panel_is_ref_ssd1683())
            panel_write_plane(0x26, NULL, red_byte, false, pb);
        else
            panel_write_plane(0x26, NULL, d->has_red ? (uint8_t)~red_byte : bw_byte, false, pb);
        break;
    case EPD_CMD_DATA_10_13:
        panel_write_plane(0x10, NULL, bw_byte, false, pb);
        panel_write_plane(0x13, NULL, d->has_red ? red_byte : bw_byte, false, pb);
        break;
    case EPD_CMD_DATA_10_92_13:
    case EPD_CMD_DATA_UC81XX_REF:
        epd_cmd(0x91);
        ref_uc81xx_full_window();
        panel_write_plane(0x10, NULL, d->has_red ? bw_byte : 0xFF, false, pb);
        epd_cmd(0x92);
        epd_cmd(0x91);
        ref_uc81xx_full_window();
        panel_write_plane(0x13, NULL, d->has_red ? (uint8_t)~red_byte : bw_byte, false, pb);
        epd_cmd(0x92);
        break;
    case EPD_CMD_DATA_10_13_INV_RED:
        panel_write_plane(0x10, NULL, bw_byte, false, pb);
        panel_write_plane(0x13, NULL, (uint8_t)~red_byte, false, pb);
        break;
    case EPD_CMD_DATA_10_13_DUMMY:
        epd_dat(0x00);
        panel_write_plane(0x10, NULL, bw_byte, false, pb);
        panel_write_plane(0x13, NULL, d->has_red ? red_byte : bw_byte, false, pb);
        break;
    case EPD_CMD_DATA_10_13_INV_BLACK_DUMMY:
        epd_dat(0x00);
        panel_write_plane(0x10, NULL, (uint8_t)~bw_byte, false, pb);
        panel_write_plane(0x13, NULL, red_byte, false, pb);
        break;
    case EPD_CMD_DATA_DUAL_SSD:
        panel_write_dualssd_data(NULL, NULL, bw_byte, red_byte);
        break;
    case EPD_CMD_DATA_UC8159_INTERLEAVED:
        panel_write_uc8159_interleaved(NULL, NULL, bw_byte, red_byte);
        break;
    case EPD_CMD_DATA_JD796XX_2BPP:
        panel_write_jd796xx_2bpp(NULL, NULL, NULL, bw_byte, red_byte, 0x00);
        break;
    }

    d->refresh();
}

/* Experimental ATC/Solum tag controller sequences, adapted from
 * atc1441/Tag_FW_nRF52811 tag_fw/src/epd_driver. The nRF project reads
 * tag metadata from UICR; this firmware exposes common tag types manually
 * and keeps the existing ESP-IDF SPI/BUSY/framebuffer layer.
 */

static void atc_unissd_init(void)
{
    const epd_panel_desc_t *d = panel_desc();
    uint16_t y0 = 0;
    uint16_t y1 = (uint16_t)d->h;

    epd_hw_reset();
    epd_cmd(0x12);
    delay_ms(10);

    cmd3(0x01, (uint8_t)(d->h & 0xFF), (uint8_t)(d->h >> 8), 0x00);
    if (d->mirror_v) {
        cmd1(0x11, 0x03);
        cmd4(0x45, (uint8_t)y0, (uint8_t)(y0 >> 8), (uint8_t)y1, (uint8_t)(y1 >> 8));
    } else {
        cmd1(0x11, 0x01);
        cmd4(0x45, (uint8_t)y1, (uint8_t)(y1 >> 8), (uint8_t)y0, (uint8_t)(y0 >> 8));
    }
    cmd2(0x44, (uint8_t)(d->x_offset / 8u), (uint8_t)(((d->x_offset + d->w) / 8u) - 1u));
    cmd1(0x3C, 0x05);
    cmd1(0x18, 0x80);
    cmd2(0x21, d->has_red ? 0x08 : 0x48, 0x00);
    atc_unissd_cursor_home();
    if (s_panel == EPD_PANEL_ATC_SSD1619_29_BWR)
        ESP_LOGI(TAG, "%s init OK", d->name);
    else
        ESP_LOGI(TAG, "%s init OK (experimental)", d->name);
}

static void hink_ssd1619_29_init(void)
{
    const epd_panel_desc_t *d = panel_desc();
    uint16_t y0 = 0;
    uint16_t y1 = (uint16_t)(HINK29_LOGICAL_H - 1u);
    uint8_t x0 = (uint8_t)(d->x_offset / 8u);
    uint8_t x1 = (uint8_t)(((d->x_offset + HINK29_LOGICAL_W) / 8u) - 1u);

    epd_hw_reset();
    epd_cmd(0x12);
    delay_ms(10);

    cmd1(0x74, 0x54);
    cmd1(0x7E, 0x3B);
    cmd2(0x2B, 0x04, 0x63);
    cmd4(0x0C, 0x8F, 0x8F, 0x8F, 0x3F);
    cmd3(0x01, (uint8_t)(y1 & 0xFF), (uint8_t)(y1 >> 8), 0x00);
    cmd1(0x11, 0x01);
    cmd2(0x44, x0, x1);
    cmd4(0x45, (uint8_t)y1, (uint8_t)(y1 >> 8),
         (uint8_t)y0, (uint8_t)(y0 >> 8));
    cmd1(0x3C, 0x01);
    cmd1(0x18, 0x80);
    cmd2(0x21, 0x08, 0x00);
    cmd1(0x22, 0xB1);
    epd_cmd(0x20);
    epd_wait_ready(true);
    hink_ssd1619_29_cursor_home();
    ESP_LOGI(TAG, "%s init OK (%dx%d, x window=%u..%u)",
             panel_desc()->name, HINK29_LOGICAL_W, HINK29_LOGICAL_H,
             (unsigned)x0, (unsigned)x1);
}

static void atc_unissd_97_init(void)
{
    epd_hw_reset();
    cmd1(0x46, 0xF7);
    delay_ms(15);
    cmd1(0x47, 0xF7);
    delay_ms(15);
    cmd5(0x0C, 0xAE, 0xC7, 0xC3, 0xC0, 0x80);
    cmd3(0x01, 0x9F, 0x02, 0x00);
    cmd1(0x11, 0x02);
    cmd4(0x44, 0xBF, 0x03, 0x00, 0x00);
    cmd4(0x45, 0x00, 0x00, 0x9F, 0x02);
    cmd1(0x3C, 0x01);
    cmd1(0x18, 0x80);
    cmd1(0x22, 0xF7);
    cmd2(0x21, 0x08, 0x00);
    atc_unissd_97_cursor_home();
    ESP_LOGI(TAG, "ATC/Solum 9.7 SSD init OK (experimental)");
}

static void atc_ucvar29_init(void)
{
    epd_hw_reset();
    cmd1(0x4D, 0x55);
    cmd1(0xF3, 0x0A);
    cmd1(0x31, 0x00);
    cmd3(0x06, 0xE5, 0x35, 0x3C);
    cmd1(0x50, 0x57);
    cmd2(0x00, 0x07, 0x09);
    ESP_LOGI(TAG, "ATC/Solum UC8151 2.9 init OK (experimental)");
}

static void atc_ucvar43_init(void)
{
    epd_hw_reset();
    cmd2(0xF8, 0x60, 0x05);
    cmd2(0xF8, 0xA1, 0x00);
    cmd2(0xF8, 0x73, 0x05);
    cmd2(0xF8, 0x7E, 0x31);
    cmd2(0xF8, 0xB8, 0x80);
    cmd2(0xF8, 0x92, 0x00);
    cmd2(0xF8, 0x87, 0x11);
    cmd2(0xF8, 0x88, 0x06);
    cmd2(0xF8, 0xA8, 0x30);
    cmd4(0x61, 0x00, 0x98, 0x02, 0x0A);
    cmd3(0x06, 0x57, 0x63, 0x3A);
    cmd1(0x50, 0x87);
    ESP_LOGI(TAG, "ATC/Solum UC 4.3 variant init OK (experimental)");
}

static void atc_dualssd_init(void)
{
    epd_hw_reset();
    epd_cmd(0x12);
    delay_ms(10);
    cmd1(0x11, 0x02);
    cmd1(0x91, 0x03);
    cmd2(0x21, panel_desc()->has_red ? 0x08 : 0x48, 0x10);
    cmd2(0x44, 0x31, 0x00);
    cmd4(0x45, 0x00, 0x00, 0x0F, 0x01);
    cmd1(0x4E, 0x31);
    cmd2(0x4F, 0x00, 0x00);
    cmd2(0xC4, 0x00, 0x31);
    cmd4(0xC5, 0x00, 0x00, 0x0F, 0x01);
    cmd1(0xCE, 0x00);
    cmd2(0xCF, 0x0F, 0x01);
    cmd1(0x3C, 0x01);
    ESP_LOGI(TAG, "%s init OK (experimental)", panel_desc()->name);
}

static void atc_uc8159_init(void)
{
    epd_hw_reset();
    cmd2(0x00, 0xEF, 0x08);
    cmd4(0x01, 0x37, 0x00, 0x05, 0x05);
    cmd1(0x03, 0x00);
    cmd3(0x06, 0xC7, 0xCC, 0x1D);
    epd_wait_ready(false);
    epd_cmd(0x04);
    epd_wait_ready(false);
    cmd1(0x13, 0x00);
    cmd1(0x30, 0x3C);
    cmd1(0x41, 0x00);
    cmd1(0x50, 0x77);
    cmd1(0x60, 0x22);
    cmd4(0x61, 0x02, 0x58, 0x01, 0xC0);
    cmd1(0x65, 0x00);
    cmd1(0x82, 0x1E);
    ESP_LOGW(TAG, "ATC/Solum UC8159 init uses fallback VCOM/PLL; source driver expects external EEPROM pins not present here");
}

static void atc_uc8179_init(void)
{
    epd_hw_reset();
    cmd1(0x00, 0x0F);
    cmd2(0x50, 0x30, 0x07);
    cmd4(0x61, 0x03, 0x20, 0x01, 0xE0);
    ESP_LOGI(TAG, "ATC/Solum UC8179 7.4 init OK (experimental)");
}

static void ref_uc81xx_init(void)
{
    const epd_panel_desc_t *d = panel_desc();
    epd_hw_reset();
    cmd1(0x00, d->has_red ? 0x0F : 0x1F);
    cmd1(0x50, d->has_red ? 0x77 : 0x97);
    ref_send_tres();
    ref_uc81xx_full_window();
    ESP_LOGI(TAG, "%s init OK (experimental reference)", d->name);
}

static void ref_uc8159_init(void)
{
    const epd_panel_desc_t *d = panel_desc();
    epd_hw_reset();
    cmd2(0x01, 0x37, 0x00);
    cmd2(0x00, 0xCF, 0x08);
    cmd1(0x30, 0x3A);
    cmd1(0x82, 0x28);
    cmd3(0x06, 0xC7, 0xCC, 0x15);
    cmd1(0x50, 0x77);
    cmd1(0x60, 0x22);
    cmd1(0x65, 0x00);
    cmd1(0xE5, 0x03);
    ref_send_tres();
    ref_uc81xx_full_window();
    ESP_LOGI(TAG, "%s init OK (experimental reference)", d->name);
}

static void ref_ssd1619_init(void)
{
    const epd_panel_desc_t *d = panel_desc();
    epd_hw_reset();
    epd_cmd(0x12);
    epd_wait_ready(false);
    cmd1(0x3C, 0x01);
    cmd1(0x18, 0x80);
    ref_ssd1619_full_window();
    ESP_LOGI(TAG, "%s init OK (experimental reference)", d->name);
}

static void ref_ssd1683_init(void)
{
    const epd_panel_desc_t *d = panel_desc();
    epd_hw_reset();
    epd_cmd(0x12);
    epd_wait_ready(false);

    cmd3(0x01, (uint8_t)((d->h - 1) & 0xFF), (uint8_t)((d->h - 1) >> 8), 0x00);
    cmd1(0x3C, 0x05);
    cmd1(0x18, 0x80);
    ref_ssd1683_full_window();
    ESP_LOGI(TAG, "%s init OK (experimental reference)", d->name);
}

static void ref_ssd1677_init(void)
{
    const epd_panel_desc_t *d = panel_desc();
    epd_hw_reset();
    epd_cmd(0x12);
    epd_wait_ready(false);
    cmd1(0x3C, 0x01);
    cmd1(0x18, 0x80);
    ref_ssd1677_full_window();
    ESP_LOGI(TAG, "%s init OK (experimental reference)", d->name);
}

static void ref_jd796xx_init(void)
{
    const epd_panel_desc_t *d = panel_desc();
    epd_hw_reset();
    delay_ms(50);
    cmd1(0x4D, 0x78);
    cmd2(0x00, 0x0F, 0x29);
    epd_cmd(0x06);
    epd_dat(0x0D); epd_dat(0x12); epd_dat(0x24); epd_dat(0x25);
    epd_dat(0x12); epd_dat(0x29); epd_dat(0x10);
    cmd1(0x30, 0x08);
    cmd1(0x50, 0x37);
    ref_send_tres();
    cmd1(0xAE, 0xCF);
    cmd1(0xB0, 0x13);
    cmd1(0xBD, 0x07);
    cmd1(0xBE, 0xFE);
    cmd1(0xE9, 0x01);
    ref_jd796xx_full_window();
    epd_cmd(0x04);
    epd_wait_ready(false);
    ESP_LOGW(TAG, "%s init OK (experimental BWRY 2bpp)", d->name);
}

/* ═══════════════════════════════════════════════════════════════════
 * SSD1619 — 4.2" 400×300
 * ═══════════════════════════════════════════════════════════════════ */

static void ssd1619_cursor_home(void)
{
    epd_cmd(0x4E);
    epd_dat(0x00);
    epd_cmd(0x4F);
    epd_dat(0x00);
    epd_dat(0x00);
}

static void ssd1619_init_sequence(void)
{
    epd_hw_reset();
    ESP_LOGI(TAG, "SSD1619: BUSY gpio after reset = %d (idle expect %d)",
             (int)gpio_get_level(PIN_EPD_BUSY), busy_idle_effective());
    if (busy_idle_effective() == 1 && gpio_get_level(PIN_EPD_BUSY) == 0)
        ESP_LOGW(TAG, "BUSY low while idle=1 — set NVS epd/busy_idle=0 or use default for 4.2\".");
    epd_wait_ready(false);

    epd_cmd(0x12);
    epd_wait_ready(false);

    epd_cmd(0x74);
    epd_dat(0x54);
    epd_cmd(0x7E);
    epd_dat(0x3B);

    epd_cmd(0x01);
    epd_dat(0x2B);
    epd_dat(0x01);
    epd_dat(0x00);

    epd_cmd(0x11);
    epd_dat(0x03);

    epd_cmd(0x44);
    epd_dat(0x00);
    epd_dat(0x31);

    epd_cmd(0x45);
    epd_dat(0x00);
    epd_dat(0x00);
    epd_dat(0x2B);
    epd_dat(0x01);

    /* BorderWaveform：Waveshare EPD_4in2b_V2_Init_new 用 0x05，减轻屏体边缘红框；0x03 易偏红 */
    epd_cmd(0x3C);
    epd_dat(0x05);
    epd_cmd(0x2C);
    epd_dat(0x55);

    epd_cmd(0x03);
    epd_dat(0x15);
    epd_cmd(0x04);
    epd_dat(0x41);
    epd_dat(0xA8);
    epd_dat(0x32);

    epd_cmd(0x3A);
    epd_dat(0x2C);
    epd_cmd(0x3B);
    epd_dat(0x0B);

    ssd1619_cursor_home();

    ESP_LOGI(TAG, "SSD1619 init OK");
}

static void ssd1619_refresh(void)
{
    /* 每帧刷新前再写边框，避免部分序列下边缘仍按红边驱动 */
    epd_cmd(0x3C);
    epd_dat(0x05);

    /* Waveshare DisplayFrame(new): 0x22 0xF7 再 0x20；与 epd4in2b_V2 一致 */
    epd_cmd(0x22);
    epd_dat(0xF7);
    delay_ms(2);
    epd_cmd(0x20);

    TickType_t t0 = xTaskGetTickCount();
    epd_wait_ready(true);
    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - t0);

    /* BUSY 未接或恒为高时：wait_ready 几毫秒内就“就绪”，全刷实际未完成，屏不更新 */
    if (elapsed_ms < 250) {
        ESP_LOGW(TAG, "SSD1619: refresh BUSY only %u ms — likely BUSY floating/wrong. "
                      "Waiting 14s for full e-paper update (check GPIO4 BUSY).",
                 (unsigned)elapsed_ms);
        delay_ms(14000);
    } else {
        delay_ms(500);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * UC8179 — 5.83" 648×480 BWR（参考 Waveshare 5.83 BWR）
 * ═══════════════════════════════════════════════════════════════════ */

static void uc8179_init_sequence(void)
{
    epd_hw_reset();
    epd_wait_ready(false);

    epd_cmd(0x01);
    epd_dat(0x07);
    epd_dat(0x07);
    epd_dat(0x3F);
    epd_dat(0x3F);

    epd_cmd(0x04);
    delay_ms(100);
    epd_wait_ready(false);

    epd_cmd(0x00);
    epd_dat(0x0F);

    epd_cmd(0x61);
    epd_dat(0x02);
    epd_dat(0x88);
    epd_dat(0x01);
    epd_dat(0xE0);

    epd_cmd(0x15);
    epd_dat(0x00);
    epd_cmd(0x50);
    epd_dat(0x11);
    epd_dat(0x07);
    epd_cmd(0x60);
    epd_dat(0x22);

    ESP_LOGI(TAG, "UC8179 init OK (BWR)");
}

static void uc8179_refresh(void)
{
    epd_cmd(0x12);
    delay_ms(200);
    epd_wait_ready(true);
}

static void ws_583_bwr_init(void)
{
    epd_hw_reset();
    cmd4(0x01, 0x07, 0x07, 0x3F, 0x3F);
    epd_cmd(0x04);
    delay_ms(100);
    epd_wait_ready(false);
    cmd1(0x00, 0x0F);
    cmd4(0x61, 0x02, 0x88, 0x01, 0xE0);
    cmd1(0x15, 0x00);
    cmd2(0x50, 0x11, 0x07);
    cmd1(0x60, 0x22);
    ESP_LOGI(TAG, "Waveshare 5.83 BWR B V2 init OK");
}



/* ── 完整初始化（每面板一次；换面板或 NVS 覆盖后需重跑） ───────────── */

static void panel_run_full_init(void)
{
    panel_desc()->init();
    s_panel_seq_done = true;
}

static bool consume_force_full_refresh_request(void)
{
    return atomic_exchange(&s_force_full_refresh_next, false);
}

static void prepare_for_forced_full_refresh(void)
{
    s_panel_seq_done = false;
}

void epd_request_full_refresh_next(void)
{
    atomic_store(&s_force_full_refresh_next, true);
}

static void panel_ensure_ready(void)
{
    if (!s_panel_seq_done)
        panel_run_full_init();
}

/** 调用方已持有 s_epd_mutex */
static void epd_clear_to_white_locked(void)
{
    int pb = epd_plane_bytes();
    panel_ensure_ready();
    panel_fill_bw_red(0xFF, 0x00, pb);
}

bool epd_is_ready(void)
{
    return s_epd_ready;
}

bool epd_wait_idle(uint32_t timeout_ms)
{
    if (!s_epd_mutex || !s_epd_ready)
        return false;
    if (xSemaphoreTake(s_epd_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return false;
    xSemaphoreGive(s_epd_mutex);
    return true;
}

static bool fb_matches_current_panel(const fb_t *fb)
{
    return fb &&
           fb->width == epd_width() &&
           fb->height == epd_height() &&
           fb->plane_bytes == epd_plane_bytes() &&
           fb->row_bytes == epd_width() / 8;
}

/* ── 对外 API ─────────────────────────────────────────────────────── */

esp_err_t epd_init(void)
{
    s_epd_ready = false;

    if (!s_epd_mutex) {
        s_epd_mutex = xSemaphoreCreateMutex();
        if (!s_epd_mutex)
            return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_EPD_DC) | (1ULL << PIN_EPD_CS) | (1ULL << PIN_EPD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&out);
    if (err != ESP_OK)
        goto fail;

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_EPD_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
    };
    err = gpio_config(&in);
    if (err != ESP_OK)
        goto fail;

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_EPD_SCK,
        .mosi_io_num = PIN_EPD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_SPI_MAX_TX,
    };
    err = spi_bus_initialize(EPD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
        goto fail;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_EPD_CS,
        .queue_size = 1,
    };
    err = spi_bus_add_device(EPD_SPI_HOST, &devcfg, &s_spi);
    if (err != ESP_OK)
        goto fail;

    nvs_load_panel();

    ESP_LOGI(TAG, "EPD start: %s %dx%d, BUSY idle_level=%d (4.2\" default 0=Waveshare; NVS busy_idle=1 if GDE idle-high)",
              s_specs[s_panel].name, epd_width(), epd_height(), busy_idle_effective());

    panel_run_full_init();
    epd_clear_to_white_locked();

    s_epd_ready = true;
    xSemaphoreGive(s_epd_mutex);

    return ESP_OK;

fail:
    ESP_LOGE(TAG, "EPD init failed: %s", esp_err_to_name(err));
    xSemaphoreGive(s_epd_mutex);
    return err;
}

esp_err_t epd_clear_screen(void)
{
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    epd_clear_to_white_locked();
    xSemaphoreGive(s_epd_mutex);
    return ESP_OK;
}

esp_err_t epd_display_test_pattern(void)
{
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);

    int W = epd_width();
    int H = epd_height();
    int pb = epd_plane_bytes();
    int rb = W / 8;

    uint8_t *buf = heap_caps_malloc((size_t)pb, MALLOC_CAP_DMA);
    if (!buf) {
        xSemaphoreGive(s_epd_mutex);
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0xFF, (size_t)pb);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (y < 2 || y >= H - 2 || x < 2 || x >= W - 2)
                buf[y * rb + (x >> 3)] &= ~(0x80 >> (x & 7));
        }
    }

    int mid_x = W / 2, mid_y = H / 2;
    for (int y = 20; y < mid_y; y++) {
        if ((y / 10) & 1) continue;
        for (int x = 20; x < mid_x - 20; x++)
            buf[y * rb + (x >> 3)] &= ~(0x80 >> (x & 7));
    }

    uint8_t *red = heap_caps_calloc(1, (size_t)pb, MALLOC_CAP_DMA);
    uint8_t *yellow = epd_has_yellow() ? heap_caps_calloc(1, (size_t)pb, MALLOC_CAP_DMA) : NULL;
    if (!red || (epd_has_yellow() && !yellow)) {
        free(red);
        free(yellow);
        free(buf);
        xSemaphoreGive(s_epd_mutex);
        return ESP_ERR_NO_MEM;
    }
    for (int y = mid_y + 20; y < H - 20; y++) {
        for (int x = mid_x + 20; x < W - 20; x++) {
            size_t off = (size_t)y * rb + (size_t)(x >> 3);
            uint8_t bit = (uint8_t)(0x80u >> (x & 7));
            red[off] |= bit;
            buf[off] |= bit;
        }
    }
    if (yellow) {
        for (int y = mid_y + 20; y < H - 20; y++) {
            for (int x = 20; x < mid_x - 20; x++) {
                size_t off = (size_t)y * rb + (size_t)(x >> 3);
                uint8_t bit = (uint8_t)(0x80u >> (x & 7));
                yellow[off] |= bit;
                buf[off] |= bit;
            }
        }
    }

    panel_ensure_ready();

    if (epd_has_yellow() && panel_desc()->data_mode == EPD_CMD_DATA_JD796XX_2BPP) {
        panel_write_jd796xx_2bpp(buf, red, yellow, 0xFF, 0x00, 0x00);
        panel_desc()->refresh();
    } else {
        panel_write_bw_red(buf, red, pb);
    }

    free(yellow);
    free(red);
    free(buf);
    xSemaphoreGive(s_epd_mutex);
    ESP_LOGI(TAG, "test pattern done");
    return ESP_OK;
}

static void repair_fill_pattern(uint8_t *bw, uint8_t *red, uint8_t *yellow,
                                int variant, bool color_pattern)
{
    const int W = epd_width();
    const int H = epd_height();
    const int rb = W / 8;
    const int pb = epd_plane_bytes();
    memset(bw, 0xFF, (size_t)pb);
    if (red)
        memset(red, 0x00, (size_t)pb);
    if (yellow)
        memset(yellow, 0x00, (size_t)pb);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            bool active;
            switch (variant & 3) {
            case 0: active = (((x >> 3) + (y >> 3)) & 1) == 0; break; /* 8x8 checker */
            case 1: active = (((x >> 3) + (y >> 3)) & 1) != 0; break;
            case 2: active = ((x >> 2) & 1) == 0; break;              /* vertical stripes */
            default: active = ((y >> 2) & 1) == 0; break;             /* horizontal stripes */
            }
            if (!active)
                continue;
            size_t off = (size_t)y * (size_t)rb + (size_t)(x >> 3);
            uint8_t bit = (uint8_t)(0x80u >> (x & 7));
            if (color_pattern && red && epd_has_red() && ((variant + x / 16 + y / 16) & 1)) {
                red[off] |= bit;
                bw[off] |= bit;
            } else if (color_pattern && yellow && epd_has_yellow()) {
                yellow[off] |= bit;
                bw[off] |= bit;
            } else {
                bw[off] &= (uint8_t)~bit;
                if (red)
                    red[off] &= (uint8_t)~bit;
                if (yellow)
                    yellow[off] &= (uint8_t)~bit;
            }
        }
    }
}

static void repair_display_pattern(const uint8_t *bw, const uint8_t *red,
                                   const uint8_t *yellow)
{
    if (epd_has_yellow() && panel_desc()->data_mode == EPD_CMD_DATA_JD796XX_2BPP) {
        panel_write_jd796xx_2bpp(bw, red, yellow, 0xFF, 0x00, 0x00);
        panel_desc()->refresh();
    } else {
        panel_write_bw_red(bw, red, epd_plane_bytes());
    }
}

esp_err_t epd_repair_ghosting(int cycles, int pattern)
{
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;
    if (cycles < 1)
        cycles = 1;
    if (cycles > EPD_REPAIR_MAX_CYCLES)
        cycles = EPD_REPAIR_MAX_CYCLES;
    if (pattern < 0 || pattern > 2)
        pattern = 0;

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);

    int pb = epd_plane_bytes();
    uint8_t *bw = NULL;
    uint8_t *red = NULL;
    uint8_t *yellow = NULL;
    if (pattern != 0) {
        bw = heap_caps_malloc((size_t)pb, MALLOC_CAP_DMA);
        red = heap_caps_malloc((size_t)pb, MALLOC_CAP_DMA);
        yellow = epd_has_yellow() ? heap_caps_malloc((size_t)pb, MALLOC_CAP_DMA) : NULL;
        if (!bw || !red || (epd_has_yellow() && !yellow)) {
            free(yellow);
            free(red);
            free(bw);
            xSemaphoreGive(s_epd_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "ghost repair start: cycles=%d, pattern=%d, panel=%s",
             cycles, pattern, panel_desc()->name);
    panel_ensure_ready();

    for (int i = 0; i < cycles; i++) {
        if (pattern == 0 || pattern == 2) {
            ESP_LOGI(TAG, "ghost repair cycle %d/%d: white", i + 1, cycles);
            panel_fill_bw_red(0xFF, 0x00, pb);
            delay_ms(300);

            ESP_LOGI(TAG, "ghost repair cycle %d/%d: black", i + 1, cycles);
            panel_fill_bw_red(0x00, 0x00, pb);
            delay_ms(300);

            if (epd_has_red() || epd_has_yellow()) {
                ESP_LOGI(TAG, "ghost repair cycle %d/%d: color", i + 1, cycles);
                panel_fill_bw_red(0xFF, 0xFF, pb);
                delay_ms(300);
            }
        }

        if (pattern == 1 || pattern == 2) {
            ESP_LOGI(TAG, "ghost repair cycle %d/%d: checker A", i + 1, cycles);
            repair_fill_pattern(bw, red, yellow, i * 2, false);
            repair_display_pattern(bw, red, yellow);
            delay_ms(300);

            ESP_LOGI(TAG, "ghost repair cycle %d/%d: checker B", i + 1, cycles);
            repair_fill_pattern(bw, red, yellow, i * 2 + 1, epd_has_red() || epd_has_yellow());
            repair_display_pattern(bw, red, yellow);
            delay_ms(300);
        }
    }

    ESP_LOGI(TAG, "ghost repair finish: white");
    panel_fill_bw_red(0xFF, 0x00, pb);

    free(yellow);
    free(red);
    free(bw);
    xSemaphoreGive(s_epd_mutex);
    ESP_LOGI(TAG, "ghost repair done");
    return ESP_OK;
}

esp_err_t epd_display_from_file(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    int pb = epd_plane_bytes();

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return ESP_FAIL;
    }

    struct stat st;
    bool have_size = (stat(path, &st) == 0);
    if (have_size && st.st_size < pb) {
        ESP_LOGE(TAG, "file too small: %ld / %u", (long)st.st_size, (unsigned)pb);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    bool has_red_file = have_size && st.st_size >= (off_t)pb * 2;
    bool has_yellow_file = have_size && st.st_size >= (off_t)pb * 3;
    if (epd_has_red() && !has_red_file)
        ESP_LOGW(TAG, "red plane missing, zero fill");
    if (epd_has_yellow() && !has_yellow_file)
        ESP_LOGW(TAG, "yellow plane missing, zero fill");

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    if (pb != epd_plane_bytes()) {
        ESP_LOGE(TAG, "file/panel mismatch: file plane=%d current=%d",
                 pb, epd_plane_bytes());
        xSemaphoreGive(s_epd_mutex);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    if (consume_force_full_refresh_request())
        prepare_for_forced_full_refresh();
    panel_ensure_ready();
    esp_err_t ret = panel_write_bw_red_file(f, pb, has_red_file, has_yellow_file);
    if (ret == ESP_OK)
        power_mgr_note_epd_refresh_complete();
    xSemaphoreGive(s_epd_mutex);
    fclose(f);
    if (ret != ESP_OK)
        return ret;

    ESP_LOGI(TAG, "display from %s", path);
    return ESP_OK;
}

esp_err_t epd_display_from_buffer(const fb_t *fb)
{
    if (!fb) return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    if (!fb_matches_current_panel(fb)) {
        ESP_LOGE(TAG, "fb/panel mismatch: fb=%dx%d/%d current=%dx%d/%d",
                 fb->width, fb->height, fb->plane_bytes,
                 epd_width(), epd_height(), epd_plane_bytes());
        xSemaphoreGive(s_epd_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    int pb = epd_plane_bytes();
    if (consume_force_full_refresh_request())
        prepare_for_forced_full_refresh();
    panel_ensure_ready();

    panel_write_bw_red(fb->black, fb->red, pb);

    power_mgr_note_epd_refresh_complete();
    xSemaphoreGive(s_epd_mutex);

    ESP_LOGI(TAG, "display from buffer");
    return ESP_OK;
}

esp_err_t epd_display_fb_free(fb_t *fb)
{
    if (!fb) return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready) {
        fb_destroy(fb);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    if (!fb_matches_current_panel(fb)) {
        ESP_LOGE(TAG, "fb/panel mismatch: fb=%dx%d/%d current=%dx%d/%d",
                 fb->width, fb->height, fb->plane_bytes,
                 epd_width(), epd_height(), epd_plane_bytes());
        xSemaphoreGive(s_epd_mutex);
        fb_destroy(fb);
        return ESP_ERR_INVALID_SIZE;
    }
    int pb = epd_plane_bytes();
    bool force_full = consume_force_full_refresh_request();
    if (force_full)
        prepare_for_forced_full_refresh();
    panel_ensure_ready();

    panel_write_bw_red_data(fb->black, fb->red, pb);
    panel_desc()->refresh();
    fb_destroy(fb);

    power_mgr_note_epd_refresh_complete();
    xSemaphoreGive(s_epd_mutex);

    ESP_LOGI(TAG, "display from buffer");
    return ESP_OK;
}
