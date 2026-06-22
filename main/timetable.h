#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TT_MAX_PERIODS   12
#define TT_DAYS          7
#define TT_NAME_LEN      40
#define TT_ROOM_LEN      20
#define TT_MAX_WEEKS     25

typedef struct {
    uint8_t start_hour;
    uint8_t start_minute;
    uint8_t end_hour;
    uint8_t end_minute;
} tt_period_def_t;

typedef struct {
    char     name[TT_NAME_LEN];
    char     room[TT_ROOM_LEN];
    uint32_t weeks;                 /* bitmask: bit 0 = week 1, bit 24 = week 25 */
} tt_cell_t;

typedef struct {
    bool            enabled;
    uint8_t         period_count;   /* 1..TT_MAX_PERIODS */
    uint8_t         show_days;      /* 5 (Mon-Fri) or 7 (Mon-Sun) */
    uint8_t         _pad;
    int32_t         semester_start; /* Unix timestamp of week-1 Monday 00:00 local */
    tt_period_def_t periods[TT_MAX_PERIODS];
    tt_cell_t       grid[TT_DAYS][TT_MAX_PERIODS];
} timetable_config_t;

esp_err_t timetable_init(void);
esp_err_t timetable_get_config(timetable_config_t *out);
esp_err_t timetable_set_config(const timetable_config_t *cfg);

/** Current semester week (1-based), 0 if unknown or out of range */
int       timetable_current_week(void);

/** Render today's agenda to EPD */
esp_err_t timetable_show(void);

/** Render a specific weekday to EPD (day: 0=Mon .. 6=Sun) */
esp_err_t timetable_show_day(int day);

/** Query helpers for clock summary.
 *  调用方提供 name/room 缓冲区，函数内部通过 timetable_get_config() 取
 *  快照后再拷出，规避后台 HTTP POST 改写时的撕裂读。
 *  name_buf / room_buf 可传 NULL。
 */
bool timetable_get_current_course(char *name_buf, size_t name_len,
                                  char *room_buf, size_t room_len,
                                  int *minutes_remaining);
bool timetable_get_next_course(char *name_buf, size_t name_len,
                               char *room_buf, size_t room_len,
                               int *minutes_until_start);

esp_err_t timetable_show_current(void);

/* HTTP handlers (called from http_app.c) */
esp_err_t timetable_http_get_handler(httpd_req_t *req);
esp_err_t timetable_http_post_handler(httpd_req_t *req);
esp_err_t timetable_show_http_handler(httpd_req_t *req);
