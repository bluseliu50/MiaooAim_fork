#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t image_convert_jpeg_to_epd_raw(const uint8_t *jpeg, size_t jpeg_len, const char *out_path);
esp_err_t image_convert_png_to_epd_raw(const uint8_t *png, size_t png_len, const char *out_path);
esp_err_t image_convert_bmp_to_epd_raw(const uint8_t *bmp, size_t bmp_len, const char *out_path);

/* Auto-detect format (JPEG/PNG/BMP) by magic bytes and convert. */
esp_err_t image_convert_to_epd_raw(const uint8_t *data, size_t len, const char *out_path);

/* Read image from file, auto-detect format, convert. Low-memory path. */
esp_err_t image_convert_file_to_epd_raw(const char *input_path, const char *out_path);
