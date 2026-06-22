/**
 * @file http_canvas.c
 *
 * HTTP 处理器：画布留言板 WYSIWYG 编辑器。
 *
 * 路由：
 *   GET  /board               — 返回 board.html 编辑器页面
 *   GET  /canvas_layout       — 返回当前布局 JSON
 *   POST /canvas_layout       — 保存布局 JSON（可选立即渲染）
 *   POST /canvas_show         — 触发渲染并推送到墨水屏
 *   GET  /canvas_icons        — 返回 {"builtin":[...], "user":[...]}
 *   POST /canvas_icon_upload  — 上传用户图标（multipart/form-data 或 raw）
 *   POST /canvas_icon_delete  — 删除用户图标（?name=xxx）
 */

#include "http_internal.h"
#include <sys/stat.h>
#include <sys/unistd.h>
#include <errno.h>
#include "canvas_board.h"
#include "display_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "http_canvas";
static SemaphoreHandle_t s_layout_lock;

static SemaphoreHandle_t layout_lock(void)
{
    if (!s_layout_lock) {
        s_layout_lock = xSemaphoreCreateMutex();
    }
    return s_layout_lock;
}

/**
 * 校验画布资源（图标 / 用户图片）的 name 参数。
 * 路径形如 "/spiffs/cimg/<name>.bin" 或 "/spiffs/icons/<name>.bin"。
 * SPIFFS 受 CONFIG_SPIFFS_OBJ_NAME_LEN（含 NUL）限制（默认 32），
 * 去掉前缀后留给 name 的安全上限 ≈ 20，这里收紧到 16 字符以方便前端预览。
 *
 * 返回 false 时已经向客户端发出 4xx 响应，调用方直接 return ESP_OK 即可。
 */
#define CANVAS_NAME_MAX 16

static bool canvas_validate_name(httpd_req_t *req, const char *name)
{
    if (!name || !*name) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "名称不能为空");
        return false;
    }
    size_t nlen = strlen(name);
    if (nlen > CANVAS_NAME_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "名称过长（最多 16 个字符）");
        return false;
    }
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "名称含非法字符（仅允许 a-zA-Z0-9_-）");
            return false;
        }
    }
    return true;
}

/* ── GET /board ─────────────────────────────────────────────────────── */

esp_err_t canvas_board_ui_get(httpd_req_t *req)
{
    return http_send_embedded_html(req, board_html_start, board_html_end);
}

/* ── GET /canvas_layout ─────────────────────────────────────────────── */

esp_err_t canvas_layout_get(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char *buf = malloc(CANVAS_LAYOUT_MAX_BYTES + 4);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }
    esp_err_t err = canvas_board_get_layout(buf, CANVAS_LAYOUT_MAX_BYTES + 2);
    if (err != ESP_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "读取布局失败");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

/* ── POST /canvas_layout ────────────────────────────────────────────── */

esp_err_t canvas_layout_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    int len = req->content_len;
    if (len <= 0 || len > CANVAS_LAYOUT_MAX_BYTES) {
        ESP_LOGW(TAG, "layout_post rejected: content_len=%d (limit=%d)",
                 len, CANVAS_LAYOUT_MAX_BYTES);
        char msg[96];
        if (len <= 0) {
            snprintf(msg, sizeof(msg), "请求体为空（content-length=%d）", len);
        } else {
            snprintf(msg, sizeof(msg),
                     "布局过大：%d 字节，上限 %d 字节",
                     len, CANVAS_LAYOUT_MAX_BYTES);
        }
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_OK;
    }

    /* 最优策略：流式接收 → 临时文件 → 校验 → 原子替换。
     * 收发阶段只占用一个 1KB 栈级 chunk，避免在 HTTP 任务里 malloc 12KB 大块。
     * 校验阶段才把临时文件读回内存（由 canvas_board_commit_layout_from_file 完成）。 */
    static const char *TMP_PATH = "/spiffs/canvas_layout.tmp";
    SemaphoreHandle_t lock = layout_lock();
    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(8000)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "画布保存忙，请稍后重试");
        return ESP_OK;
    }

    FILE *tmp = fopen(TMP_PATH, "w");
    if (!tmp) {
        ESP_LOGE(TAG, "fopen(%s) failed errno=%d", TMP_PATH, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "无法创建临时文件");
        xSemaphoreGive(lock);
        return ESP_OK;
    }

    char chunk[1024];
    int total = 0;
    while (total < len) {
        int want = len - total;
        if (want > (int)sizeof(chunk)) want = sizeof(chunk);
        int r = httpd_req_recv(req, chunk, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            fclose(tmp);
            unlink(TMP_PATH);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败");
            xSemaphoreGive(lock);
            return ESP_OK;
        }
        if ((int)fwrite(chunk, 1, (size_t)r, tmp) != r) {
            fclose(tmp);
            unlink(TMP_PATH);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "写盘失败");
            xSemaphoreGive(lock);
            return ESP_OK;
        }
        total += r;
    }
    fclose(tmp);

    /* 校验 + 原子替换正式文件；失败时该函数自行清理 tmp */
    esp_err_t err = canvas_board_commit_layout_from_file(TMP_PATH);
    xSemaphoreGive(lock);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "布局 JSON 无效");
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "布局过大");
        return ESP_OK;
    }
    if (err == ESP_ERR_NO_MEM) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存布局失败");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── POST /canvas_show ──────────────────────────────────────────────── */

esp_err_t canvas_show_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (!http_require_epd_ready(req)) return ESP_OK;

    /* 消耗请求体 */
    char tmp[64];
    int remain = req->content_len;
    while (remain > 0) {
        int to_recv = remain < (int)sizeof(tmp) ? remain : (int)sizeof(tmp);
        int r = httpd_req_recv(req, tmp, to_recv);
        if (r <= 0) break;
        remain -= r;
    }

    unsigned epoch = display_policy_display_epoch();
    esp_err_t err = canvas_board_show_queued(&epoch);
    if (err == ESP_OK)
        canvas_board_wait_idle();
    if (err == ESP_OK && !display_policy_epoch_is_current(epoch))
        err = ESP_ERR_INVALID_STATE;
    /* 不切换循环模式，留言板属于手动触发内容 */

    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":%s,\"canceled\":%s}",
             err == ESP_OK ? "true" : "false",
             err == ESP_ERR_INVALID_STATE ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── GET /canvas_icons ──────────────────────────────────────────────── */

esp_err_t canvas_icons_get(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char *builtin = malloc(2048);
    char *user    = malloc(1024);
    if (!builtin || !user) {
        free(builtin); free(user);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }

    canvas_board_list_builtin_icons(builtin, 2048);
    canvas_board_list_user_icons(user, 1024);

    size_t total = strlen(builtin) + strlen(user) + 32;
    char *resp = malloc(total);
    if (!resp) {
        free(builtin); free(user);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }
    snprintf(resp, total, "{\"builtin\":%s,\"user\":%s}", builtin, user);
    free(builtin); free(user);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    free(resp);
    return ESP_OK;
}

/* ── POST /canvas_icon_upload ───────────────────────────────────────── */
/*
 * 接受两种格式：
 *   A) Content-Type: application/octet-stream  —  body 即 32 字节 raw 1-bit 位图
 *   B) multipart/form-data（未来扩展，此处先实现 A）
 *
 * 图标名通过 query param ?name=xxx 传递。
 */
esp_err_t canvas_icon_upload_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char name[CANVAS_NAME_MAX + 4] = {0};
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 name 参数");
        return ESP_OK;
    }
    if (!canvas_validate_name(req, name)) return ESP_OK;

    int len = req->content_len;
    if (len <= 0 || len > 4096) {
        ESP_LOGW(TAG, "icon_upload rejected: name=%s content_len=%d", name, len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "内容长度无效（期望 32 字节 1-bit 位图）");
        return ESP_OK;
    }

    uint8_t *buf = malloc((size_t)len);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }
    int total = 0;
    while (total < len) {
        int r = httpd_req_recv(req, (char *)buf + total, len - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败"); return ESP_OK; }
        total += r;
    }

    /* 仅接受 32 字节 */
    if (total != 32) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "图标必须是 16×16 1-bit 位图（32 字节）");
        return ESP_OK;
    }

    esp_err_t err = canvas_board_save_user_icon(name, buf, 32);
    free(buf);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存图标失败");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "User icon saved: %s", name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── GET /canvas_image_list ─────────────────────────────────────────── */

esp_err_t canvas_image_list_get(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char *list = malloc(2048);
    if (!list) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }
    canvas_board_list_images(list, 2048);

    size_t total = strlen(list) + 16;
    char *resp = malloc(total);
    if (!resp) {
        free(list);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }
    snprintf(resp, total, "{\"images\":%s}", list);
    free(list);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    free(resp);
    return ESP_OK;
}

/* ── GET /canvas_image ──────────────────────────────────────────────── */
/* 流式读取文件发送，避免大 malloc                                       */

esp_err_t canvas_image_get(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char name[CANVAS_NAME_MAX + 4] = {0};
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 name 参数");
        return ESP_OK;
    }
    if (!canvas_validate_name(req, name)) return ESP_OK;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.bin", CANVAS_IMAGES_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "图片不存在");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char chunk[512];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0)
        httpd_resp_send_chunk(req, chunk, (ssize_t)r);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── POST /canvas_image_upload ──────────────────────────────────────── */
/* 流式接收并直接写入 SPIFFS，无需大 malloc                              */

esp_err_t canvas_image_upload_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char name[CANVAS_NAME_MAX + 4] = {0};
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 name 参数");
        return ESP_OK;
    }
    if (!canvas_validate_name(req, name)) return ESP_OK;

    int content_len = req->content_len;
    if (content_len <= 4 || content_len > CANVAS_IMAGE_MAX_BYTES) {
        ESP_LOGW(TAG, "image_upload rejected: name=%s content_len=%d (limit=%d)",
                 name, content_len, CANVAS_IMAGE_MAX_BYTES);
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "图片大小无效：%d 字节，需 5..%d",
                 content_len, CANVAS_IMAGE_MAX_BYTES);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_OK;
    }

    /* 直接流式写入文件 */
    struct stat _st;
    if (stat(CANVAS_IMAGES_DIR, &_st) != 0) mkdir(CANVAS_IMAGES_DIR, 0755);
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.bin", CANVAS_IMAGES_DIR, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d", path, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "创建文件失败");
        return ESP_OK;
    }

    char chunk[512];
    int remaining = content_len, total = 0;
    bool ok = true;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(chunk) ? remaining : (int)sizeof(chunk);
        int r = httpd_req_recv(req, chunk, to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { ok = false; break; }
        if ((int)fwrite(chunk, 1, r, f) != r) { ok = false; break; }
        total += r;
        remaining -= r;
    }
    fclose(f);

    if (!ok || total != content_len) {
        remove(path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ok ? "写入不完整" : "接收失败");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Canvas image saved: %s (%d bytes)", name, total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── POST /canvas_image_delete ──────────────────────────────────────── */

esp_err_t canvas_image_delete_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char name[CANVAS_NAME_MAX + 4] = {0};
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 name 参数");
        return ESP_OK;
    }
    if (!canvas_validate_name(req, name)) return ESP_OK;
    char tmp[64];
    int remain = req->content_len;
    while (remain > 0) {
        int to_recv = remain < (int)sizeof(tmp) ? remain : (int)sizeof(tmp);
        int r = httpd_req_recv(req, tmp, to_recv);
        if (r <= 0) break;
        remain -= r;
    }

    esp_err_t err = canvas_board_delete_image(name);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "名称无效");
        return ESP_OK;
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


esp_err_t canvas_icon_delete_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char name[CANVAS_NAME_MAX + 4] = {0};
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 name 参数");
        return ESP_OK;
    }
    if (!canvas_validate_name(req, name)) return ESP_OK;

    /* 消耗请求体 */
    char tmp[64];
    int remain = req->content_len;
    while (remain > 0) {
        int to_recv = remain < (int)sizeof(tmp) ? remain : (int)sizeof(tmp);
        int r = httpd_req_recv(req, tmp, to_recv);
        if (r <= 0) break;
        remain -= r;
    }

    esp_err_t err = canvas_board_delete_user_icon(name);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "名称无效");
        return ESP_OK;
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
