#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int  year;
    int  month;
    int  day;
    bool is_leap;
} lunar_date_t;

bool        lunar_from_solar(int y, int m, int d, lunar_date_t *out);

const char *lunar_day_str(int day);
const char *lunar_month_str(int month);

const char *lunar_year_gz(int lunar_year);
const char *lunar_year_sx(int lunar_year);

const char *lunar_solar_term(int y, int m, int d);

const char *lunar_festival(int y, int m, int d, const lunar_date_t *ld);
