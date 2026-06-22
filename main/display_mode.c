#include "display_mode.h"
#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "display_policy.h"
#include "epd.h"

static const char *TAG = "dmode";

static display_mode_entry_t s_modes[DISPLAY_MODE_MAX];
static int s_count;
static atomic_int s_active_mode = ATOMIC_VAR_INIT(-1);

int display_mode_register(const display_mode_entry_t *entry)
{
    if (!entry || !entry->name || !entry->show) return -1;
    if (s_count >= DISPLAY_MODE_MAX) {
        ESP_LOGE(TAG, "mode registry full (%d)", DISPLAY_MODE_MAX);
        return -1;
    }
    s_modes[s_count] = *entry;
    ESP_LOGI(TAG, "[%d] %s (%s)", s_count, entry->name, entry->label_cn);
    return s_count++;
}

int display_mode_count(void) { return s_count; }

const display_mode_entry_t *display_mode_get(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_modes[idx];
}

static esp_err_t display_mode_show_internal(int idx, bool new_request, unsigned *epoch_out)
{
    const display_mode_entry_t *m = display_mode_get(idx);
    if (!m) return ESP_ERR_INVALID_ARG;
    int prev = display_mode_active();
    unsigned epoch;
    if (new_request) {
        epoch = display_policy_begin_manual_display();
    } else {
        epd_request_full_refresh_next();
        epoch = display_policy_display_epoch();
    }
    if (epoch_out)
        *epoch_out = epoch;
    display_mode_set_active(idx);
    esp_err_t err = m->show();
    if (err != ESP_OK)
        display_mode_set_active(prev);
    return err;
}

esp_err_t display_mode_show(int idx)
{
    return display_mode_show_internal(idx, false, NULL);
}

esp_err_t display_mode_show_request(int idx, unsigned *epoch_out)
{
    return display_mode_show_internal(idx, true, epoch_out);
}

const char *display_mode_name(int idx)
{
    const display_mode_entry_t *m = display_mode_get(idx);
    return m ? m->name : "?";
}

const char *display_mode_label(int idx)
{
    const display_mode_entry_t *m = display_mode_get(idx);
    return m ? m->label_cn : "?";
}

int display_mode_active(void)
{
    return atomic_load(&s_active_mode);
}

void display_mode_set_active(int idx)
{
    if (idx < 0 || idx >= s_count) {
        atomic_store(&s_active_mode, -1);
        return;
    }
    atomic_store(&s_active_mode, idx);
}
