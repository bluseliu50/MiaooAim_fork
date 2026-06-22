#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define QW_ICON_SMALL_SIZE 24
#define QW_ICON_LARGE_SIZE 48

typedef struct {
    int code;
    const uint8_t *small;
    const uint8_t *large;
} qw_icon_entry_t;

const qw_icon_entry_t *qw_icon_find(int code);
const uint8_t *qw_icon_bitmap(int code, int size, int *actual_size);
