#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

#define TODO_MAX_ITEMS   16
#define TODO_TEXT_LEN    48

typedef struct {
    char    text[TODO_TEXT_LEN];
    bool    done;
    uint8_t priority;   /* 0=normal, 1=high, 2=urgent */
} todo_item_t;

typedef struct {
    bool        enabled;
    uint8_t     count;
    todo_item_t items[TODO_MAX_ITEMS];
} todo_config_t;

esp_err_t todo_init(void);
esp_err_t todo_get_config(todo_config_t *out);
esp_err_t todo_set_config(const todo_config_t *cfg);
esp_err_t todo_show(void);

/* HTTP handlers (called from http_app.c) */
esp_err_t todo_http_get_handler(httpd_req_t *req);
esp_err_t todo_http_post_handler(httpd_req_t *req);
esp_err_t todo_show_http_handler(httpd_req_t *req);
