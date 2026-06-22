#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "fb_render.h"

typedef enum {
    EPD_PANEL_42_BWR        = 0,  /* 4.2" 400x300 BW+Red, SSD1619 */
    EPD_PANEL_583_BWR       = 1,  /* 5.83" 648x480 BW+Red, UC8179 */

    EPD_PANEL_WS_583_BWR    = 10, /* Verified Waveshare 5.83" 648x480 BW+Red B V2 */

    /* Experimental Solum/ATC tag drivers ported from atc1441/Tag_FW_nRF52811. */
    EPD_PANEL_ATC_SSD1619_16_BWR = 14, /* 1.6" 200x200 BW+Red, SSD1619 family */
    EPD_PANEL_ATC_SSD1619_22_BWR = 15, /* 2.2" 160x296 BW+Red, SSD1619 family */
    EPD_PANEL_ATC_SSD1619_26_BWR = 16, /* 2.6" 184x360 BW+Red, SSD1619 family */
    EPD_PANEL_HINK_SSD1619_29_BWR = 17, /* HINK-E029A14-A1 2.9" 128x296 BW+Red, SSD1619 */
    EPD_PANEL_ATC_SSD1619_29_BWR = EPD_PANEL_HINK_SSD1619_29_BWR, /* backward compatible id */
    EPD_PANEL_ATC_UC8151_29_BWR  = 18, /* 2.9" 168x384 BW+Red, UC8151 variant */
    EPD_PANEL_ATC_UCVAR43_BWR    = 19, /* 4.3" 152x522 BW+Red, UC variant */
    EPD_PANEL_ATC_DUALSSD_585_BWR = 20, /* 5.85" 792x272 BW+Red, dual SSD */
    EPD_PANEL_ATC_DUALSSD_585_BW  = 21, /* 5.85" 792x272 BW, dual SSD */
    EPD_PANEL_ATC_UC8159_60_BWR  = 22, /* 6.0" 600x448 BW+Red, UC8159 */
    EPD_PANEL_ATC_UC8179_74_BWR  = 23, /* 7.4" 800x480 BW+Red, UC8179 */
    EPD_PANEL_ATC_SSD_97_BWR     = 24, /* 9.7" 960x672 BW+Red, SSD family */
    EPD_PANEL_ATC_PEGHOOK_13_BWR = 25, /* 1.3" 144x200 BW+Red, SSD family */

    /* Experimental reference drivers reimplemented from EPD-nRF5 command models. */
    EPD_PANEL_REF_UC8176_42_BW       = 26, /* 4.2" 400x300 BW, UC8176 */
    EPD_PANEL_REF_UC8176_42_BWR      = 27, /* 4.2" 400x300 BW+Red, UC8176 */
    EPD_PANEL_REF_UC8179_75_BW       = 28, /* 7.5" 800x480 BW, UC8179 */
    EPD_PANEL_REF_UC8179_75_BWR      = 29, /* 7.5" 800x480 BW+Red, UC8179 */
    EPD_PANEL_REF_UC8159_583_BW      = 30, /* 5.83" 600x448 BW, UC8159 */
    EPD_PANEL_REF_UC8159_75_LOW_BWR  = 31, /* 7.5" 640x384 BW+Red low-res, UC8159 */
    EPD_PANEL_REF_SSD1677_75_HD_BW   = 32, /* 7.5" 880x528 BW HD, SSD1677 */
    EPD_PANEL_REF_SSD1677_75_HD_BWR  = 33, /* 7.5" 880x528 BW+Red HD, SSD1677 */
    EPD_PANEL_REF_JD79665_75_BWRY    = 34, /* 7.5" 800x480 BWRY, JD79665; native 2bpp red/yellow */
    EPD_PANEL_REF_SSD1619_42_BW      = 35, /* 4.2" 400x300 BW, SSD1619 */
    EPD_PANEL_REF_SSD1619_42_BWR     = 36, /* 4.2" 400x300 BW+Red, SSD1619 */
    EPD_PANEL_REF_JD79668_42_BWRY    = 37, /* 4.2" 400x300 BWRY, JD79668; native 2bpp red/yellow */
    EPD_PANEL_REF_SSD1683_42_BWR     = 38, /* 4.2" 400x300 BW+Red, SSD1683 experimental */
    EPD_PANEL_REF_SSD1683_42_LEGACY  = 39, /* legacy saved id; uses the same full-refresh SSD1683 path */

    EPD_PANEL_COUNT
} epd_panel_t;

/**
 * 从 NVS 读取面板类型 / busy_idle（不写 SPI、不初始化墨水屏）。
 * 在 epd_init() 之前调用，便于 HTTP 已启动后 /panel_config 与网页使用正确分辨率；
 * 换屏后若 NVS 仍是小屏，先起网再改配置可避免长时间卡在 BUSY 等待。
 */
void         epd_load_panel_from_nvs(void);

esp_err_t    epd_init(void);
/** false 直至 epd_init() 成功返回；HTTP 等应在刷图前检查 */
bool         epd_is_ready(void);
/** Wait until no EPD transfer/refresh function is holding the display mutex. */
bool         epd_wait_idle(uint32_t timeout_ms);
esp_err_t    epd_display_from_file(const char *path);
esp_err_t    epd_display_from_buffer(const fb_t *fb);
/** 同上，但 SPI 传输完成后立即释放 fb，再等待 EPD 刷新——节省 18s 内存占用 */
esp_err_t    epd_display_fb_free(fb_t *fb);
/** Force the next framebuffer/file display to reload the full refresh setup. */
void         epd_request_full_refresh_next(void);
esp_err_t    epd_clear_screen(void);
esp_err_t    epd_display_test_pattern(void);
esp_err_t    epd_repair_ghosting(int cycles, int pattern);

epd_panel_t  epd_get_panel(void);
esp_err_t    epd_set_panel(epd_panel_t panel);
esp_err_t    epd_save_panel_config(epd_panel_t panel, int busy_idle);
esp_err_t    epd_get_saved_panel_config(epd_panel_t *panel, int *busy_idle);

/** 当前 BUSY 空闲电平：0=空闲时低，1=空闲时高（与 NVS epd/busy_idle 一致） */
int          epd_busy_idle_level(void);
/** 指定面板的默认 BUSY 空闲电平；非法面板返回 -1。 */
int          epd_panel_default_busy_idle(epd_panel_t panel);
esp_err_t    epd_set_busy_idle(int level); /* 0 或 1，写入 NVS 并立即生效 */

int          epd_width(void);
int          epd_height(void);
int          epd_plane_bytes(void);
bool         epd_has_red(void);
bool         epd_has_yellow(void);
