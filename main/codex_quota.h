#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool     enabled;
    char     api_url[192];   /* Full quota endpoint URL, e.g. https://relay.example.com/v1/usage */
    char     api_key[128];   /* Sent as Authorization: Bearer <api_key> */
    char     unit[16];       /* Display unit, e.g. USD / 元 / credits */
    uint32_t refresh_min;    /* 0=manual only; otherwise auto-refresh while quota page is active */
} codex_quota_config_t;

typedef struct {
    bool   valid;
    bool   have_total;
    bool   have_used;
    bool   have_remaining;
    double total;
    double used;
    double remaining;
    double today_cost;
    double total_tokens;
    bool   have_today_cost;
    bool   have_total_tokens;
    int    percent_used;
    int    request_count;
    char   unit[16];
    char   account[40];
    char   update_time[24];
    char   message[80];
} codex_quota_data_t;

esp_err_t codex_quota_init(void);
esp_err_t codex_quota_get_config(codex_quota_config_t *out);
esp_err_t codex_quota_set_config(const codex_quota_config_t *cfg);
void      codex_quota_get_data_copy(codex_quota_data_t *out);
esp_err_t codex_quota_show(void);
void      codex_quota_set_auto_network_allowed(bool allowed);
