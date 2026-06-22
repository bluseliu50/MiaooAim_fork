/**
 * @file button.c
 *
 * Polling-based button driver with debounce.
 * Directly switches display modes: SW3=prev, SW5=next, SW4=refresh.
 *
 * Architecture: lightweight button_task (2.5KB) polls GPIOs and posts
 * commands to display_worker_task (10KB).
 *
 * Display modes come from the display_mode registry (display_mode.h).
 */

#include "button.h"

#include <stdbool.h>
#include <stdatomic.h>

#include "calendar_display.h"
#include "display_mode.h"
#include "display_policy.h"
#include "driver/gpio.h"
#include "epd.h"
#include "esp_log.h"
#include "fb_render.h"
#include "ui_theme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "power_mgr.h"

static const char *TAG = "button";

/* Schematic: SW3->GPIO9(prev), SW4->GPIO46(refresh), SW5->GPIO3(next), 10k pulldown. */
#define BTN_LEFT    GPIO_NUM_9
#define BTN_MID     GPIO_NUM_46
#define BTN_RIGHT   GPIO_NUM_3

#define BTN_COUNT       3
#define POLL_MS         20
#define DEBOUNCE_MS     40
#define DEBOUNCE_TICKS  (DEBOUNCE_MS / POLL_MS)
#define WORKER_STACK    10240

typedef enum {
    CMD_NEXT = 0,
    CMD_PREV,
    CMD_REFRESH,
} btn_cmd_t;

static int            s_mode_idx;
static atomic_bool    s_busy;
static QueueHandle_t  s_cmd_queue;

int  button_get_current_mode(void) { return s_mode_idx; }
void button_set_current_mode(int mode)
{
    /* Clamp external callers so the button loop never lands on an invalid entry. */
    int n = display_mode_count();
    if (n <= 0) return;
    if (mode < 0) mode = 0;
    if (mode >= n) mode = n - 1;
    if (s_mode_idx != mode)
        epd_request_full_refresh_next();
    s_mode_idx = mode;
    display_mode_set_active(mode);
}

static const gpio_num_t btn_pins[BTN_COUNT] = {
    BTN_LEFT, BTN_MID, BTN_RIGHT,
};

typedef struct {
    uint8_t stable;
    uint8_t count;
} debounce_t;

static debounce_t s_db[BTN_COUNT];

/* display worker (runs on 10KB stack) */

static void show_unconfigured_hint(int idx, unsigned epoch)
{
    fb_t *fb = fb_create();
    if (!fb) return;

    const char *label = display_mode_label(idx);

    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
    ui_draw_header(fb, label, "\xe6\x9c\xaa\xe9\x85\x8d\xe7\xbd\xae", true);
    ui_draw_empty_state(fb, "\xe6\x9c\xaa\xe9\x85\x8d\xe7\xbd\xae",
                        "\xe8\xaf\xb7\xe9\x80\x9a\xe8\xbf\x87\xe7\xbd\x91\xe9\xa1\xb5\xe8\xae\xbe\xe7\xbd\xae");
    ui_draw_footer(fb, "SETUP", "192.168.4.1");

    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        return;
    }

    epd_display_fb_free(fb);
}

static void switch_mode(int dir)
{
    int n = display_mode_count();
    if (n == 0) return;

    s_mode_idx = (s_mode_idx + dir + n) % n;

    unsigned epoch = 0;
    esp_err_t err = display_mode_show_request(s_mode_idx, &epoch);
    if (err == ESP_OK && s_mode_idx == DISPLAY_MODE_CALENDAR)
        calendar_display_wait_render_idle();

    if (!display_policy_epoch_is_current(epoch))
        return;

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "switched to %s", display_mode_name(s_mode_idx));
        power_mgr_save_mode(s_mode_idx);
        return;
    }

    ESP_LOGW(TAG, "%s unavailable, showing hint", display_mode_name(s_mode_idx));
    show_unconfigured_hint(s_mode_idx, epoch);
}

static void display_worker_task(void *arg)
{
    (void)arg;
    btn_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE)
            continue;

        s_busy = true;

        switch (cmd) {
        case CMD_PREV:
            switch_mode(-1);
            break;
        case CMD_NEXT:
            switch_mode(+1);
            break;
        case CMD_REFRESH:
            {
                unsigned epoch = 0;
                esp_err_t err;
                if (s_mode_idx == DISPLAY_MODE_CALENDAR) {
                    epoch = display_policy_begin_manual_display();
                    err = calendar_display_toggle_style();
                    calendar_display_wait_render_idle();
                } else {
                    err = display_mode_show_request(s_mode_idx, &epoch);
                }
                if (err == ESP_OK && display_policy_epoch_is_current(epoch))
                    ESP_LOGI(TAG, "refreshed %s", display_mode_name(s_mode_idx));
            }
            break;
        }

        s_busy = false;
    }
}

/* button polling */

static void on_press(int idx)
{
    power_mgr_reset_activity();

    if (s_busy) {
        ESP_LOGW(TAG, "display busy, press ignored");
        return;
    }

    btn_cmd_t cmd;
    switch (idx) {
    case 0: cmd = CMD_PREV;    ESP_LOGI(TAG, "SW3 (prev)");    break;
    case 1: cmd = CMD_REFRESH; ESP_LOGI(TAG, "SW4 (refresh)"); break;
    case 2: cmd = CMD_NEXT;    ESP_LOGI(TAG, "SW5 (next)");    break;
    default: return;
    }

    xQueueOverwrite(s_cmd_queue, &cmd);
}

static void button_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (int i = 0; i < BTN_COUNT; i++) {
            int raw = gpio_get_level(btn_pins[i]);
            if (raw != s_db[i].stable) {
                s_db[i].count++;
                if (s_db[i].count >= DEBOUNCE_TICKS) {
                    s_db[i].stable = raw;
                    s_db[i].count = 0;
                    if (raw == 1)
                        on_press(i);
                }
            } else {
                s_db[i].count = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

/* init */

esp_err_t button_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BTN_COUNT; i++)
        mask |= (1ULL << btn_pins[i]);

    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < BTN_COUNT; i++) {
        s_db[i].stable = gpio_get_level(btn_pins[i]);
        s_db[i].count = 0;
    }

    s_cmd_queue = xQueueCreate(1, sizeof(btn_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "queue create failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok;
    TaskHandle_t worker_h = NULL;
    ok = xTaskCreate(display_worker_task, "btn_disp", WORKER_STACK, NULL, 4, &worker_h);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreate(button_task, "btn_poll", 2560, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "poll task create failed");
        if (worker_h) vTaskDelete(worker_h);
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "3 buttons on GPIO %d/%d/%d, %d modes registered",
             BTN_LEFT, BTN_MID, BTN_RIGHT, display_mode_count());
    return ESP_OK;
}
