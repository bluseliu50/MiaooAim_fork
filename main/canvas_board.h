#pragma once

/**
 * @file canvas_board.h
 *
 * WYSIWYG 画布留言板：接受来自 Web 编辑器的 JSON 布局，在 fb_t 上逐元素渲染后
 * 输出到墨水屏。
 *
 * JSON 布局格式（数组，顺序即图层从底到顶）：
 * [
 *   { "type":"text",    "x":10, "y":20, "color":0, "scale":2, "text":"Hello" },
 *   { "type":"rect",    "x":5,  "y":5,  "w":100, "h":50, "color":1, "fill":false },
 *   { "type":"ellipse", "x":50, "y":80, "rx":30, "ry":20, "color":2, "fill":true },
 *   { "type":"line",    "x1":0, "y1":0, "x2":100, "y2":100, "color":0 },
 *   { "type":"icon",    "x":10, "y":10, "color":1, "scale":2, "name":"sun" },
 * ]
 *
 * type:
 *   "text"    — 文本块，支持 UTF-8 + 汉字；scale=1..6
 *   "rect"    — 矩形；fill=true 填充，false 仅描边
 *   "ellipse" — 椭圆；rx/ry 半径；fill 同上
 *   "line"    — 直线（Bresenham）
 *   "icon"    — 内置或用户上传的 16×16 1-bit 图标，scale=1..4
 *
 * color: 0=黑, 1=红, 2=白
 */

#include "esp_err.h"
#include "fb_render.h"
#include <stdbool.h>
#include <stdint.h>

/* 最大布局 JSON 字节数（存于 SPIFFS）。
 * 受限于渲染时同时驻留：78KB 帧缓冲 + 本字符串 + ~5x 的 cJSON 树。
 * ESP32-S3 在 WiFi+AP 共存下可用堆 ~150KB，12KB 已是安全上限。 */
#define CANVAS_LAYOUT_MAX_BYTES  12288

/* 图标存储目录 */
#define CANVAS_ICONS_DIR  "/spiffs/icons"

/* ── 公开 API ───────────────────────────────────────────────────────── */

esp_err_t canvas_board_init(void);

/**
 * 获取当前布局 JSON 字符串（调用者提供缓冲区）。
 * 若尚无布局，写入 "[]" 并返回 ESP_OK。
 */
esp_err_t canvas_board_get_layout(char *buf, size_t buf_len);

/**
 * 保存布局 JSON（写入 SPIFFS /spiffs/canvas_layout.json）。
 */
esp_err_t canvas_board_set_layout(const char *json, size_t len);

/**
 * 提交一个已经流式落盘到临时文件的布局：cJSON 校验通过后原子替换正式文件。
 * 失败时自动删除临时文件。tmp_path 必须存在；成功返回 ESP_OK。
 *   - ESP_ERR_INVALID_ARG: tmp 文件内容不是合法 JSON 数组
 *   - ESP_ERR_INVALID_SIZE: tmp 文件 >  CANVAS_LAYOUT_MAX_BYTES
 */
esp_err_t canvas_board_commit_layout_from_file(const char *tmp_path);

/**
 * 从当前布局渲染并推送到墨水屏（异步任务）。
 */
esp_err_t canvas_board_show(void);
esp_err_t canvas_board_show_queued(unsigned *out_epoch);

/**
 * 等待当前渲染任务完成（阻塞）。
 */
void canvas_board_wait_idle(void);

/* ── 图标管理 ───────────────────────────────────────────────────────── */

/**
 * 获取内置图标名列表（JSON 数组字符串，调用者提供缓冲区）。
 * 格式：["sun","cloud","rain",...]
 */
esp_err_t canvas_board_list_builtin_icons(char *buf, size_t buf_len);

/**
 * 获取用户上传图标名列表（JSON 数组字符串）。
 */
esp_err_t canvas_board_list_user_icons(char *buf, size_t buf_len);

/**
 * 保存用户上传的图标（原始 1-bit 16×16 大端位图，32 字节）。
 * name 必须符合 [A-Za-z0-9_-] 且不超过 32 字节（不含 .bin 扩展名）。
 */
esp_err_t canvas_board_save_user_icon(const char *name,
                                       const uint8_t *data, size_t len);

/**
 * 删除用户上传的图标。
 */
esp_err_t canvas_board_delete_user_icon(const char *name);

/* ── 图片管理 ───────────────────────────────────────────────────────── */

/* 图片存储目录：
 * 旧格式：4 字节头(w u16le + h u16le) + 黑色 1-bit 行主序位图。
 * 新格式：旧头 + "BW" + flags + reserved + 黑色平面 + 红色平面。
 */
#define CANVAS_IMAGES_DIR       "/spiffs/cimg"
#define CANVAS_IMAGE_MAX_BYTES  (8 + FB_MAX_PLANE_BYTES * 2)  /* header + max black/red planes */

/**
 * 获取已上传图片名列表（JSON 数组，调用者提供缓冲区）。
 * 格式：["photo","logo",...]
 */
esp_err_t canvas_board_list_images(char *buf, size_t buf_len);

/**
 * 保存图片（4字节头 + 1-bit 位图数据）。
 * name 必须符合 [A-Za-z0-9_-]。
 */
esp_err_t canvas_board_save_image(const char *name,
                                   const uint8_t *data, size_t len);

/**
 * 读取图片原始数据到 buf（含 4 字节头）。
 * out_len 返回实际字节数。
 */
esp_err_t canvas_board_get_image_data(const char *name,
                                       uint8_t *buf, size_t buf_len,
                                       size_t *out_len);

/**
 * 删除图片。
 */
esp_err_t canvas_board_delete_image(const char *name);
