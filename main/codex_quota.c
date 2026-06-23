#include "codex_quota.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "battery_mon.h"
#include "display_mode.h"
#include "display_policy.h"
#include "epd.h"
#include "fb_render.h"
#include "time_sync.h"
#include "ui_theme.h"

static const char *TAG = "codex_quota";
static const char *NVS_NS = "codex_quota";

#define QUOTA_HTTP_TIMEOUT_MS 30000
#define QUOTA_HTTP_RETRIES 2
#define QUOTA_MAX_RESP_LEN 12288
#define QUOTA_SHOW_STACK 16384
#define QUOTA_AUTO_STACK 4096
#define QUOTA_AUTO_POLL_MS 5000
#define QUOTA_MIN_REFRESH_MIN 5
#define QUOTA_MAX_REFRESH_MIN 1440
#define ONE_API_QUOTA_PER_USD 500000.0

/* Tier name constants (mirror cc-switch semantics). */
#define TIER_FIVE_HOUR "five_hour"
#define TIER_WEEKLY "weekly_limit"
#define TIER_MONTHLY "monthly"
#define TIER_SEVEN_DAY "seven_day"
#define TIER_GEMINI_PRO "gemini_pro"
#define TIER_GEMINI_FLASH "gemini_flash"
#define TIER_GEMINI_LITE "gemini_flash_lite"

static codex_quota_config_t s_cfg = {
    .enabled = false,
    .api_url = "",
    .api_key = "",
    .unit = "USD",
    .provider = QUOTA_PROVIDER_CUSTOM,
    .refresh_min = QUOTA_MIN_REFRESH_MIN,
};

static codex_quota_data_t s_data;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_auto_task;
static volatile TickType_t s_last_fetch_tick;
static bool s_auto_network_allowed = true;

/* ── Provider display names ─────────────────────────────────────────────────
 */
static const char *const PROVIDER_NAMES[QUOTA_PROVIDER_MAX] = {
    [QUOTA_PROVIDER_CUSTOM] = "中转站",
    [QUOTA_PROVIDER_KIMI] = "Kimi",
    [QUOTA_PROVIDER_ZHIPU_CN] = "智谱GLM",
    [QUOTA_PROVIDER_ZHIPU_EN] = "Z.AI",
    [QUOTA_PROVIDER_MINIMAX_CN] = "MiniMax",
    [QUOTA_PROVIDER_MINIMAX_EN] = "MiniMax",
    [QUOTA_PROVIDER_ZENMUX] = "ZenMux",
    [QUOTA_PROVIDER_VOLCENGINE] = "火山方舟",
    [QUOTA_PROVIDER_CODEX] = "Codex",
    [QUOTA_PROVIDER_GEMINI] = "Gemini",
    [QUOTA_PROVIDER_COPILOT] = "Copilot",
    [QUOTA_PROVIDER_DEEPSEEK] = "DeepSeek",
    [QUOTA_PROVIDER_STEPFUN] = "StepFun",
    [QUOTA_PROVIDER_SILICONFLOW] = "硅基流动",
    [QUOTA_PROVIDER_SILICONFLOW_EN] = "SiliconFlow",
    [QUOTA_PROVIDER_OPENROUTER] = "OpenRouter",
    [QUOTA_PROVIDER_NOVITA] = "Novita",
};

const char *codex_quota_provider_name(quota_provider_t p) {
  if (p >= QUOTA_PROVIDER_MAX)
    return "?";
  return PROVIDER_NAMES[p] ? PROVIDER_NAMES[p] : "?";
}

/* ── Config helpers ─────────────────────────────────────────────────────────
 */

typedef struct {
  char *buf;
  int len;
  int cap;
} resp_buf_t;

static void config_snapshot(codex_quota_config_t *out) {
  portENTER_CRITICAL(&s_cfg_mux);
  *out = s_cfg;
  portEXIT_CRITICAL(&s_cfg_mux);
}

static bool config_ready(const codex_quota_config_t *cfg) {
  if (!cfg || !cfg->enabled)
    return false;
  if (cfg->provider == QUOTA_PROVIDER_VOLCENGINE)
    return cfg->access_key_id[0] && cfg->secret_access_key[0];
  if (cfg->provider == QUOTA_PROVIDER_CUSTOM)
    return cfg->api_url[0] && cfg->api_key[0];
  return cfg->api_key[0] != '\0';
}

static uint32_t normalize_refresh_min(uint32_t refresh_min) {
  if (refresh_min == 0)
    return 0;
  if (refresh_min < QUOTA_MIN_REFRESH_MIN)
    return QUOTA_MIN_REFRESH_MIN;
  if (refresh_min > QUOTA_MAX_REFRESH_MIN)
    return QUOTA_MAX_REFRESH_MIN;
  return refresh_min;
}

static void normalize_config(codex_quota_config_t *cfg) {
  if (!cfg)
    return;
  if (cfg->provider >= QUOTA_PROVIDER_MAX)
    cfg->provider = QUOTA_PROVIDER_CUSTOM;
  if (cfg->unit[0] == '\0')
    snprintf(cfg->unit, sizeof(cfg->unit), "USD");
  cfg->refresh_min = normalize_refresh_min(cfg->refresh_min);
}

/* Per-provider credential storage: credentials are keyed by provider index so
 * that switching providers in the web UI preserves each provider's saved token
 * independently (e.g. Kimi's key survives switching to Zhipu and back).
 * NVS keys: p<N>key / p<N>url / p<N>akid / p<N>aksk. The active provider's
 * credentials are mirrored into s_cfg for runtime use. Shared settings
 * (enabled/unit/refresh) are not per-provider. */
static void cred_key(char *out, size_t out_len, uint8_t provider, char suffix) {
  snprintf(out, out_len, "p%u%c", (unsigned)provider, suffix);
}

/* Load one provider's credentials into the given buffers (empty if none). */
static void nvs_load_cred(nvs_handle_t h, uint8_t provider,
                          codex_quota_config_t *cfg) {
  char k[8];
  size_t len;
  len = sizeof(cfg->api_key);
  cred_key(k, sizeof(k), provider, 'k');
  nvs_get_str(h, k, cfg->api_key, &len);
  len = sizeof(cfg->api_url);
  cred_key(k, sizeof(k), provider, 'u');
  nvs_get_str(h, k, cfg->api_url, &len);
  len = sizeof(cfg->access_key_id);
  cred_key(k, sizeof(k), provider, 'i');
  nvs_get_str(h, k, cfg->access_key_id, &len);
  len = sizeof(cfg->secret_access_key);
  cred_key(k, sizeof(k), provider, 's');
  nvs_get_str(h, k, cfg->secret_access_key, &len);
}

/* Persist one provider's credentials from cfg into per-provider NVS slots. */
static void nvs_save_cred(nvs_handle_t h, uint8_t provider,
                          const codex_quota_config_t *cfg) {
  char k[8];
  cred_key(k, sizeof(k), provider, 'k');
  nvs_set_str(h, k, cfg->api_key);
  cred_key(k, sizeof(k), provider, 'u');
  nvs_set_str(h, k, cfg->api_url);
  cred_key(k, sizeof(k), provider, 'i');
  nvs_set_str(h, k, cfg->access_key_id);
  cred_key(k, sizeof(k), provider, 's');
  nvs_set_str(h, k, cfg->secret_access_key);
}

static void nvs_load(void) {
  nvs_handle_t h;
  bool need_migrate = false;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
    return;

  uint8_t en = 0;
  if (nvs_get_u8(h, "enabled", &en) == ESP_OK)
    s_cfg.enabled = en != 0;
  uint8_t prov = QUOTA_PROVIDER_CUSTOM;
  if (nvs_get_u8(h, "provider", &prov) == ESP_OK && prov < QUOTA_PROVIDER_MAX)
    s_cfg.provider = prov;

  /* load the active provider's credentials */
  nvs_load_cred(h, s_cfg.provider, &s_cfg);

  /* One-time migration from the legacy single-slot credentials (api_key /
   * api_url / ak_id / ak_sk) to the active provider's per-provider slot, if
   * the new slot is empty but the legacy key exists. */
  if (s_cfg.api_key[0] == '\0' && s_cfg.api_url[0] == '\0' &&
      s_cfg.access_key_id[0] == '\0' && s_cfg.secret_access_key[0] == '\0') {
    size_t len = sizeof(s_cfg.api_key);
    nvs_get_str(h, "api_key", s_cfg.api_key, &len);
    len = sizeof(s_cfg.api_url);
    nvs_get_str(h, "api_url", s_cfg.api_url, &len);
    len = sizeof(s_cfg.access_key_id);
    nvs_get_str(h, "ak_id", s_cfg.access_key_id, &len);
    len = sizeof(s_cfg.secret_access_key);
    nvs_get_str(h, "ak_sk", s_cfg.secret_access_key, &len);
    if (s_cfg.api_key[0] || s_cfg.api_url[0] || s_cfg.access_key_id[0] ||
        s_cfg.secret_access_key[0])
      need_migrate = true;
  }

  size_t len = sizeof(s_cfg.unit);
  nvs_get_str(h, "unit", s_cfg.unit, &len);
  uint32_t refresh = s_cfg.refresh_min;
  if (nvs_get_u32(h, "refresh", &refresh) == ESP_OK)
    s_cfg.refresh_min = refresh;
  normalize_config(&s_cfg);
  nvs_close(h);

  /* persist migrated credentials into the per-provider slot */
  if (need_migrate) {
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
      nvs_save_cred(h, s_cfg.provider, &s_cfg);
      nvs_commit(h);
      nvs_close(h);
      ESP_LOGI(TAG, "migrated legacy creds to provider %u",
               (unsigned)s_cfg.provider);
    }
  }
}

/* ── HTTP helpers ───────────────────────────────────────────────────────────
 */

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  resp_buf_t *rb = (resp_buf_t *)evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA && rb && evt->data &&
      evt->data_len > 0) {
    int avail = rb->cap - 1 - rb->len;
    if (avail > 0) {
      int n = evt->data_len;
      if (n > avail)
        n = avail;
      memcpy(rb->buf + rb->len, evt->data, (size_t)n);
      rb->len += n;
      rb->buf[rb->len] = '\0';
    }
  }
  return ESP_OK;
}

static void log_tls_heap_state(void) {
  ESP_LOGI(TAG, "heap: free=%lu largest=%lu internal=%lu psram=%lu",
           (unsigned long)esp_get_free_heap_size(),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                  MALLOC_CAP_8BIT),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM |
                                                  MALLOC_CAP_8BIT));
}

/* Generic HTTP request. method: 0=GET, 1=POST. headers is a NULL-terminated
 * array of {key, value} pairs. Returns malloc'd body (caller frees) + status.
 */
typedef struct {
  const char *key;
  const char *val;
} http_hdr_t;

static char *http_request(int method, const char *url,
                          const http_hdr_t *headers, const char *body,
                          int body_len, int *out_status) {
  resp_buf_t rb = {
      .buf = malloc(QUOTA_MAX_RESP_LEN),
      .len = 0,
      .cap = QUOTA_MAX_RESP_LEN,
  };
  if (!rb.buf)
    return NULL;
  if (out_status)
    *out_status = 0;

  esp_err_t err = ESP_FAIL;
  int status = 0;

  for (int attempt = 0; attempt < QUOTA_HTTP_RETRIES; attempt++) {
    rb.len = 0;
    rb.buf[0] = '\0';
    if (attempt > 0)
      vTaskDelay(pdMS_TO_TICKS(1000));

    log_tls_heap_state();
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &rb,
        .timeout_ms = QUOTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
      free(rb.buf);
      return NULL;
    }
    if (method == 1) {
      esp_http_client_set_method(client, HTTP_METHOD_POST);
      if (body && body_len >= 0)
        esp_http_client_set_post_field(client, body, body_len);
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "User-Agent", "miaooaim/1.0");
    if (headers) {
      for (const http_hdr_t *h = headers; h->key; h++)
        esp_http_client_set_header(client, h->key, h->val);
    }
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status >= 200 && status < 500)
      break;
    ESP_LOGW(TAG, "HTTP %s attempt %d: err=%s status=%d",
             method == 1 ? "POST" : "GET", attempt + 1, esp_err_to_name(err),
             status);
  }

  if (out_status)
    *out_status = status;
  if (err != ESP_OK || status < 200 || status >= 500) {
    ESP_LOGW(TAG, "HTTP %s failed: err=%s status=%d",
             method == 1 ? "POST" : "GET", esp_err_to_name(err), status);
    /* still return body for 4xx so caller can inspect error envelope */
    if (status < 400 || status >= 500) {
      free(rb.buf);
      return NULL;
    }
  }

  ESP_LOGI(TAG, "HTTP %s status=%d (%d bytes)", method == 1 ? "POST" : "GET",
           status, rb.len);
  return rb.buf;
}

/* ── JSON traversal helpers (cJSON based) ────────────────────────────────────
 */

static cJSON *json_get_path(cJSON *root, const char *path) {
  if (!root || !path || !path[0])
    return NULL;
  char tmp[96];
  snprintf(tmp, sizeof(tmp), "%s", path);
  cJSON *cur = root;
  char *save = NULL;
  for (char *tok = strtok_r(tmp, ".", &save); tok;
       tok = strtok_r(NULL, ".", &save)) {
    /* support [n] array index */
    char *lb = strchr(tok, '[');
    if (lb) {
      *lb = '\0';
      cur = cJSON_GetObjectItem(cur, tok);
      if (!cur)
        return NULL;
      int idx = atoi(lb + 1);
      cur = cJSON_GetArrayItem(cur, idx);
      if (!cur)
        return NULL;
    } else {
      cur = cJSON_GetObjectItem(cur, tok);
      if (!cur)
        return NULL;
    }
  }
  return cur;
}

static double json_num(cJSON *j) {
  if (!j)
    return 0;
  if (cJSON_IsNumber(j))
    return j->valuedouble;
  if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) {
    char *end = NULL;
    double v = strtod(j->valuestring, &end);
    if (end && end != j->valuestring)
      return v;
  }
  return 0;
}

static double json_num_path(cJSON *root, const char *path) {
  return json_num(json_get_path(root, path));
}

static bool json_has_num(cJSON *root, const char *path) {
  cJSON *j = json_get_path(root, path);
  if (cJSON_IsNumber(j))
    return true;
  if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) {
    char *end = NULL;
    strtod(j->valuestring, &end);
    return end && end != j->valuestring;
  }
  return false;
}

static bool json_bool_path(cJSON *root, const char *path) {
  cJSON *j = json_get_path(root, path);
  return cJSON_IsTrue(j);
}

static const char *json_str_path(cJSON *root, const char *path) {
  cJSON *j = json_get_path(root, path);
  return cJSON_GetStringValue(j);
}

/* Parse a reset-time field that may be ISO string / seconds / milliseconds.
 * Returns epoch seconds, or 0 if none/invalid. */
static int64_t parse_reset_ts(cJSON *n) {
  if (!n)
    return 0;
  if (cJSON_IsString(n) && n->valuestring && n->valuestring[0]) {
    /* Try ISO parse — best effort via strptime. */
    struct tm tm = {0};
    if (strptime(n->valuestring, "%Y-%m-%dT%H:%M:%S", &tm)) {
      time_t t = mktime(&tm);
      if (t > 0)
        return (int64_t)t;
    }
    /* numeric string */
    char *end = NULL;
    double v = strtod(n->valuestring, &end);
    if (end && end != n->valuestring && v > 0) {
      if (v < 1e12)
        return (int64_t)(v);
      return (int64_t)(v / 1000.0);
    }
    return 0;
  }
  if (cJSON_IsNumber(n)) {
    double v = n->valuedouble;
    if (v <= 0)
      return 0;
    if (v < 1e12)
      return (int64_t)v;
    return (int64_t)(v / 1000.0);
  }
  return 0;
}

/* ── Tier helpers ───────────────────────────────────────────────────────────
 */

static void add_tier(codex_quota_data_t *out, const char *name,
                     double utilization, int64_t reset_ts, double used_val,
                     double max_val, bool have_abs) {
  if (!out || out->tier_count >= QUOTA_MAX_TIERS)
    return;
  quota_tier_t *t = &out->tiers[out->tier_count++];
  snprintf(t->name, sizeof(t->name), "%s", name);
  t->utilization = utilization;
  t->reset_ts = reset_ts;
  t->used_value = used_val;
  t->max_value = max_val;
  t->have_abs = have_abs;
}

static void set_plan(codex_quota_data_t *out, const char *plan) {
  if (out && plan && plan[0])
    snprintf(out->plan, sizeof(out->plan), "%s", plan);
}

static void set_message(codex_quota_data_t *out, const char *msg) {
  if (out && msg && msg[0])
    snprintf(out->message, sizeof(out->message), "%s", msg);
}

/* ── Custom relay (flexible balance parser, original logic preserved) ────────
 */

static void build_quota_url(const codex_quota_config_t *cfg, char *out,
                            size_t out_len) {
  if (!out || out_len == 0)
    return;
  if (!cfg || !cfg->api_url[0]) {
    out[0] = '\0';
    return;
  }
  snprintf(out, out_len, "%s", cfg->api_url);
  if (strstr(out, "/v1/usage"))
    return;
  char *old = strstr(out, "/api/usage/token");
  if (!old)
    old = strstr(out, "/api/user/self");
  if (old)
    *old = '\0';
  size_t n = strlen(out);
  while (n > 0 && out[n - 1] == '/') {
    out[n - 1] = '\0';
    n--;
  }
  if (n >= 3 && strcmp(out + n - 3, "/v1") == 0)
    snprintf(out + n, out_len > n ? out_len - n : 0, "/usage");
  else
    snprintf(out + n, out_len > n ? out_len - n : 0, "/v1/usage");
}

static bool json_number_any(cJSON *root, const char *const *paths, int n,
                            double *out) {
  for (int i = 0; i < n; i++) {
    if (json_has_num(root, paths[i])) {
      *out = json_num_path(root, paths[i]);
      return true;
    }
  }
  return false;
}

static bool parse_custom_balance(const char *json, codex_quota_data_t *out,
                                 const codex_quota_config_t *cfg) {
  cJSON *root = cJSON_Parse(json);
  if (!root)
    return false;

  memset(out, 0, sizeof(*out));
  out->kind = 1; /* balance */
  snprintf(out->unit, sizeof(out->unit), "%s",
           cfg->unit[0] ? cfg->unit : "USD");
  const char *u = json_str_path(root, "unit");
  if (!u)
    u = json_str_path(root, "data.unit");
  if (u && u[0])
    snprintf(out->unit, sizeof(out->unit), "%s", u);

  static const char *const remain_paths[] = {
      "total_available", "available",    "balance",
      "remain",          "remaining",    "data.total_available",
      "data.available",  "data.balance", "data.remain",
      "data.remaining",  "data.quota",   "quota"};
  static const char *const used_paths[] = {"total_used",
                                           "used",
                                           "used_quota",
                                           "usage",
                                           "actual_cost",
                                           "cost",
                                           "usage.total.actual_cost",
                                           "usage.total.cost",
                                           "data.total_used",
                                           "data.used",
                                           "data.used_quota",
                                           "data.usage",
                                           "data.usage.total.actual_cost",
                                           "data.usage.total.cost"};
  static const char *const total_paths[] = {"total_granted",
                                            "total",
                                            "credit",
                                            "credits",
                                            "hard_limit_usd",
                                            "data.total_granted",
                                            "data.total",
                                            "data.credit",
                                            "data.credits",
                                            "subscription.hard_limit_usd",
                                            "data.subscription.hard_limit_usd"};

  out->have_remaining = json_number_any(
      root, remain_paths, (int)(sizeof(remain_paths) / sizeof(remain_paths[0])),
      &out->remaining);
  out->have_used = json_number_any(
      root, used_paths, (int)(sizeof(used_paths) / sizeof(used_paths[0])),
      &out->used);
  out->have_total = json_number_any(
      root, total_paths, (int)(sizeof(total_paths) / sizeof(total_paths[0])),
      &out->total);

  if (!out->have_total && out->have_remaining && out->have_used) {
    out->total = out->remaining + out->used;
    out->have_total = true;
  }
  if (!out->have_remaining && out->have_total && out->have_used) {
    out->remaining = out->total - out->used;
    out->have_remaining = true;
  }
  if (!out->have_used && out->have_total && out->have_remaining) {
    out->used = out->total - out->remaining;
    out->have_used = true;
  }

  double max_abs = 0;
  if (out->have_total && fabs(out->total) > max_abs)
    max_abs = fabs(out->total);
  if (out->have_used && fabs(out->used) > max_abs)
    max_abs = fabs(out->used);
  if (out->have_remaining && fabs(out->remaining) > max_abs)
    max_abs = fabs(out->remaining);
  if (max_abs > 10000.0) {
    if (out->have_total)
      out->total /= ONE_API_QUOTA_PER_USD;
    if (out->have_used)
      out->used /= ONE_API_QUOTA_PER_USD;
    if (out->have_remaining)
      out->remaining /= ONE_API_QUOTA_PER_USD;
    set_message(out, "已按 One API 额度比例换算");
  }

  if (out->have_total && out->total > 0 && out->have_used) {
    int pct = (int)((out->used * 100.0 / out->total) + 0.5);
    if (pct < 0)
      pct = 0;
    if (pct > 100)
      pct = 100;
    out->percent_used = pct;
  }

  out->valid = out->have_remaining || out->have_used || out->have_total;
  if (!out->valid) {
    const char *m = json_str_path(root, "message");
    if (!m)
      m = json_str_path(root, "msg");
    if (!m)
      m = json_str_path(root, "error.message");
    set_message(out, m ? m : "未识别额度字段");
  }

  cJSON_Delete(root);
  return out->valid;
}

/* ── Provider adapters ──────────────────────────────────────────────────────
 * Each returns true on "produced a result" (valid or auth_error), false on
 * hard transport failure (network). Status code captured for auth detection. */

typedef struct {
  bool produced;     /* got a response we could interpret */
  bool auth_failed;  /* 401/403 */
  const char *error; /* static error message for non-auth failures */
} adapt_t;

static adapt_t mark_auth_fail(codex_quota_data_t *out) {
  if (out) {
    out->auth_ok = false;
    out->valid = false;
    set_message(out, "凭据无效或已过期");
  }
  adapt_t a = {.produced = true, .auth_failed = true};
  return a;
}

static adapt_t mark_err(const char *msg) {
  adapt_t a = {.produced = true, .error = msg};
  return a;
}

/* ---- Kimi For Coding ---- */
static adapt_t adapt_kimi(const codex_quota_config_t *cfg,
                          codex_quota_data_t *out) {
  char auth[160];
  snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
  http_hdr_t hdrs[] = {{"Authorization", auth}, {NULL, NULL}};
  int status = 0;
  char *body = http_request(0, "https://api.kimi.com/coding/v1/usages", hdrs,
                            NULL, 0, &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("Kimi 接口错误");
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root)
    return mark_err("Kimi 响应解析失败");

  out->kind = 0;
  cJSON *limits = cJSON_GetObjectItem(root, "limits");
  cJSON *lim;
  cJSON_ArrayForEach(lim, limits) {
    cJSON *d = cJSON_GetObjectItem(lim, "detail");
    if (!d)
      continue;
    double limit = json_num(cJSON_GetObjectItem(d, "limit"));
    double remaining = json_num(cJSON_GetObjectItem(d, "remaining"));
    if (limit == 0)
      limit = 1;
    double used = limit - remaining;
    if (used < 0)
      used = 0;
    int64_t reset = parse_reset_ts(cJSON_GetObjectItem(d, "resetTime"));
    add_tier(out, TIER_FIVE_HOUR, used / limit * 100.0, reset, 0, 0, false);
  }
  cJSON *usage = cJSON_GetObjectItem(root, "usage");
  if (usage) {
    double limit = json_num(cJSON_GetObjectItem(usage, "limit"));
    double remaining = json_num(cJSON_GetObjectItem(usage, "remaining"));
    if (limit == 0)
      limit = 1;
    double used = limit - remaining;
    if (used < 0)
      used = 0;
    int64_t reset = parse_reset_ts(cJSON_GetObjectItem(usage, "resetTime"));
    add_tier(out, TIER_WEEKLY, used / limit * 100.0, reset, 0, 0, false);
  }
  set_plan(out, "Kimi");
  out->valid = out->tier_count > 0;
  cJSON_Delete(root);
  return (adapt_t){.produced = true};
}

/* ---- Zhipu GLM (NO Bearer prefix) ---- */
static adapt_t adapt_zhipu(const codex_quota_config_t *cfg,
                           codex_quota_data_t *out, bool is_cn) {
  const char *base = is_cn ? "https://open.bigmodel.cn" : "https://api.z.ai";
  char url[160];
  snprintf(url, sizeof(url), "%s/api/monitor/usage/quota/limit", base);

  /* Zhipu uses bare token (no Bearer) — unique quirk. */
  http_hdr_t hdrs[] = {{"Authorization", cfg->api_key},
                       {"Accept-Language", "en-US,en"},
                       {NULL, NULL}};
  int status = 0;
  char *body = http_request(0, url, hdrs, NULL, 0, &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("智谱接口错误");
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root)
    return mark_err("智谱响应解析失败");

  if (!json_bool_path(root, "success")) {
    const char *msg = json_str_path(root, "msg");
    cJSON_Delete(root);
    return mark_err(msg && msg[0] ? msg : "智谱业务错误");
  }
  cJSON *data = cJSON_GetObjectItem(root, "data");
  if (!data) {
    cJSON_Delete(root);
    return mark_err("智谱响应缺少 data");
  }

  out->kind = 0;
  /* Classify by `unit`: 3→five_hour, 6→weekly. NEVER sort by reset time. */
  cJSON *limits = cJSON_GetObjectItem(data, "limits");
  bool have_5h = false, have_w = false;
  cJSON *lim;
  cJSON_ArrayForEach(lim, limits) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(lim, "type"));
    if (!type || strcasecmp(type, "TOKENS_LIMIT") != 0)
      continue;
    double pct = json_num(cJSON_GetObjectItem(lim, "percentage"));
    int64_t unit = (int64_t)json_num(cJSON_GetObjectItem(lim, "unit"));
    int64_t reset = parse_reset_ts(cJSON_GetObjectItem(lim, "nextResetTime"));
    if (unit == 3 && !have_5h) {
      add_tier(out, TIER_FIVE_HOUR, pct, reset, 0, 0, false);
      have_5h = true;
    } else if (unit == 6 && !have_w) {
      add_tier(out, TIER_WEEKLY, pct, reset, 0, 0, false);
      have_w = true;
    }
  }
  /* Fallback: entries with unknown unit → fill empty slots by has-reset-first.
   */
  if (!have_5h || !have_w) {
    cJSON_ArrayForEach(lim, limits) {
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(lim, "type"));
      if (!type || strcasecmp(type, "TOKENS_LIMIT") != 0)
        continue;
      int64_t unit = (int64_t)json_num(cJSON_GetObjectItem(lim, "unit"));
      if (unit == 3 || unit == 6)
        continue;
      double pct = json_num(cJSON_GetObjectItem(lim, "percentage"));
      int64_t reset = parse_reset_ts(cJSON_GetObjectItem(lim, "nextResetTime"));
      if (!have_5h) {
        add_tier(out, TIER_FIVE_HOUR, pct, reset, 0, 0, false);
        have_5h = true;
      } else if (!have_w) {
        add_tier(out, TIER_WEEKLY, pct, reset, 0, 0, false);
        have_w = true;
      }
    }
  }
  const char *level = json_str_path(root, "data.level");
  set_plan(out, level && level[0] ? level : "智谱GLM");
  out->valid = out->tier_count > 0;
  cJSON_Delete(root);
  return (adapt_t){.produced = true};
}

/* ---- MiniMax ---- */
static adapt_t adapt_minimax(const codex_quota_config_t *cfg,
                             codex_quota_data_t *out, bool is_cn) {
  const char *domain = is_cn ? "api.minimaxi.com" : "api.minimax.io";
  char url[160];
  snprintf(url, sizeof(url),
           "https://%s/v1/api/openplatform/coding_plan/remains", domain);

  char auth[160];
  snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
  http_hdr_t hdrs[] = {{"Authorization", auth}, {NULL, NULL}};
  int status = 0;
  char *body = http_request(0, url, hdrs, NULL, 0, &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("MiniMax 接口错误");
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root)
    return mark_err("MiniMax 响应解析失败");

  cJSON *br = json_get_path(root, "base_resp");
  if (br) {
    double sc = json_num(cJSON_GetObjectItem(br, "status_code"));
    if ((int64_t)sc != 0) {
      const char *msg = json_str_path(root, "base_resp.status_msg");
      cJSON_Delete(root);
      return mark_err(msg && msg[0] ? msg : "MiniMax 业务错误");
    }
  }

  out->kind = 0;
  cJSON *mrs = cJSON_GetObjectItem(root, "model_remains");
  cJSON *general = NULL;
  cJSON *m;
  cJSON_ArrayForEach(m, mrs) {
    const char *name =
        cJSON_GetStringValue(cJSON_GetObjectItem(m, "model_name"));
    if (name && strcmp(name, "general") == 0) {
      general = m;
      break;
    }
  }
  if (general) {
    double rp5 = json_num(
        cJSON_GetObjectItem(general, "current_interval_remaining_percent"));
    int64_t end = (int64_t)json_num(cJSON_GetObjectItem(general, "end_time"));
    add_tier(out, TIER_FIVE_HOUR, 100.0 - rp5, end > 0 ? end / 1000 : 0, 0, 0,
             false);
    double wstatus =
        json_num(cJSON_GetObjectItem(general, "current_weekly_status"));
    if ((int64_t)wstatus == 1) {
      double rpw = json_num(
          cJSON_GetObjectItem(general, "current_weekly_remaining_percent"));
      int64_t wend =
          (int64_t)json_num(cJSON_GetObjectItem(general, "weekly_end_time"));
      add_tier(out, TIER_WEEKLY, 100.0 - rpw, wend > 0 ? wend / 1000 : 0, 0, 0,
               false);
    }
  }
  set_plan(out, "MiniMax");
  out->valid = out->tier_count > 0;
  cJSON_Delete(root);
  return (adapt_t){.produced = true};
}

/* ---- ZenMux (base_url used as full query URL; usage_percentage is 0-1) ----
 */
static adapt_t adapt_zenmux(const codex_quota_config_t *cfg,
                            codex_quota_data_t *out) {
  char auth[160];
  snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
  http_hdr_t hdrs[] = {{"Authorization", auth}, {NULL, NULL}};
  int status = 0;
  char *body = http_request(0, cfg->api_url, hdrs, NULL, 0, &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("ZenMux 接口错误");
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root)
    return mark_err("ZenMux 响应解析失败");
  if (!json_bool_path(root, "success")) {
    const char *msg = json_str_path(root, "message");
    cJSON_Delete(root);
    return mark_err(msg && msg[0] ? msg : "ZenMux 业务错误");
  }
  cJSON *data = cJSON_GetObjectItem(root, "data");
  if (!data) {
    cJSON_Delete(root);
    return mark_err("ZenMux 响应缺少 data");
  }

  out->kind = 0;
  cJSON *q5 = cJSON_GetObjectItem(data, "quota_5_hour");
  if (q5) {
    double pct = json_num(cJSON_GetObjectItem(q5, "usage_percentage"));
    double uv = json_num(cJSON_GetObjectItem(q5, "used_value_usd"));
    double mv = json_num(cJSON_GetObjectItem(q5, "max_value_usd"));
    int64_t reset = parse_reset_ts(cJSON_GetObjectItem(q5, "resets_at"));
    add_tier(out, TIER_FIVE_HOUR, pct * 100.0, reset, uv, mv, true);
  }
  cJSON *q7 = cJSON_GetObjectItem(data, "quota_7_day");
  if (q7) {
    double pct = json_num(cJSON_GetObjectItem(q7, "usage_percentage"));
    double uv = json_num(cJSON_GetObjectItem(q7, "used_value_usd"));
    double mv = json_num(cJSON_GetObjectItem(q7, "max_value_usd"));
    int64_t reset = parse_reset_ts(cJSON_GetObjectItem(q7, "resets_at"));
    add_tier(out, TIER_WEEKLY, pct * 100.0, reset, uv, mv, true);
  }
  const char *tier = json_str_path(root, "data.plan.tier");
  set_plan(out, tier && tier[0] ? tier : "ZenMux");
  out->valid = out->tier_count > 0;
  cJSON_Delete(root);
  return (adapt_t){.produced = true};
}

/* ---- Codex / ChatGPT (token in api_key; primary_window = 5h limit) ---- */
static const char *codex_window_name(int64_t secs) {
  if (secs == 18000)
    return TIER_FIVE_HOUR;
  if (secs == 604800)
    return TIER_SEVEN_DAY;
  static char buf[16];
  int hours = (int)(secs / 3600);
  if (hours >= 24)
    snprintf(buf, sizeof(buf), "%d_day", hours / 24);
  else
    snprintf(buf, sizeof(buf), "%d_hour", hours);
  return buf;
}

static adapt_t adapt_codex(const codex_quota_config_t *cfg,
                           codex_quota_data_t *out) {
  char auth[160];
  snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
  http_hdr_t hdrs[] = {
      {"Authorization", auth}, {"User-Agent", "codex-cli"}, {NULL, NULL}};
  int status = 0;
  char *body = http_request(0, "https://chatgpt.com/backend-api/wham/usage",
                            hdrs, NULL, 0, &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("Codex 接口错误");
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root)
    return mark_err("Codex 响应解析失败");

  out->kind = 0;
  cJSON *rl = cJSON_GetObjectItem(root, "rate_limit");
  if (rl) {
    const char *wins[] = {"primary_window", "secondary_window"};
    for (int i = 0; i < 2; i++) {
      cJSON *w = cJSON_GetObjectItem(rl, wins[i]);
      if (!w)
        continue;
      double used = json_num(cJSON_GetObjectItem(w, "used_percent"));
      int64_t secs =
          (int64_t)json_num(cJSON_GetObjectItem(w, "limit_window_seconds"));
      int64_t reset = (int64_t)json_num(cJSON_GetObjectItem(w, "reset_at"));
      add_tier(out, codex_window_name(secs), used, reset, 0, 0, false);
    }
  }
  set_plan(out, "Codex");
  out->valid = out->tier_count > 0;
  cJSON_Delete(root);
  return (adapt_t){.produced = true};
}

/* ---- Gemini (two-step: loadCodeAssist → retrieveUserQuota) ---- */
static const char *classify_gemini(const char *mid) {
  if (strstr(mid, "flash-lite"))
    return TIER_GEMINI_LITE;
  if (strstr(mid, "flash"))
    return TIER_GEMINI_FLASH;
  if (strstr(mid, "pro"))
    return TIER_GEMINI_PRO;
  return mid;
}

static adapt_t adapt_gemini(const codex_quota_config_t *cfg,
                            codex_quota_data_t *out) {
  char auth[160];
  snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
  http_hdr_t hdrs[] = {{"Authorization", auth},
                       {"Content-Type", "application/json"},
                       {NULL, NULL}};
  /* Step 1: loadCodeAssist */
  const char *load_body =
      "{\"metadata\":{\"ideType\":\"GEMINI_CLI\",\"pluginType\":\"GEMINI\"}}";
  int status = 0;
  char *body = http_request(
      1, "https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist", hdrs,
      load_body, (int)strlen(load_body), &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("Gemini loadCodeAssist 失败");
  }

  char pid[48] = {0};
  cJSON *lroot = cJSON_Parse(body);
  free(body);
  if (lroot) {
    const char *id = json_str_path(lroot, "cloudaicompanionProject.id");
    if (!id)
      id = json_str_path(lroot, "cloudaicompanionProject.projectId");
    if (id)
      snprintf(pid, sizeof(pid), "%s", id);
    cJSON_Delete(lroot);
  }

  /* Step 2: retrieveUserQuota */
  char qbody[80];
  if (pid[0])
    snprintf(qbody, sizeof(qbody), "{\"project\":\"%s\"}", pid);
  else
    snprintf(qbody, sizeof(qbody), "{}");
  body = http_request(
      1, "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuota",
      hdrs, qbody, (int)strlen(qbody), &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("Gemini retrieveUserQuota 失败");
  }

  cJSON *qroot = cJSON_Parse(body);
  free(body);
  if (!qroot)
    return mark_err("Gemini 响应解析失败");

  out->kind = 0;
  /* Group buckets by model category, take min remainingFraction. */
  struct gem_cat {
    char name[20];
    double min_rem;
    int64_t reset;
    bool set;
  } cats[6] = {0};
  int ncat = 0;
  cJSON *buckets = cJSON_GetObjectItem(qroot, "buckets");
  cJSON *b;
  cJSON_ArrayForEach(b, buckets) {
    const char *mid = cJSON_GetStringValue(cJSON_GetObjectItem(b, "modelId"));
    if (!mid || !mid[0])
      mid = "unknown";
    const char *cat = classify_gemini(mid);
    double rem = json_num(cJSON_GetObjectItem(b, "remainingFraction"));
    if (rem < 0)
      rem = 0;
    if (rem > 1)
      rem = 1;
    int64_t reset = parse_reset_ts(cJSON_GetObjectItem(b, "resetTime"));
    int idx = -1;
    for (int i = 0; i < ncat; i++) {
      if (strcmp(cats[i].name, cat) == 0) {
        idx = i;
        break;
      }
    }
    if (idx < 0 && ncat < 6) {
      idx = ncat++;
      snprintf(cats[idx].name, sizeof(cats[idx].name), "%s", cat);
      cats[idx].min_rem = rem;
      cats[idx].reset = reset;
      cats[idx].set = true;
    }
    if (idx >= 0 && rem < cats[idx].min_rem) {
      cats[idx].min_rem = rem;
      cats[idx].reset = reset;
    }
  }
  /* Sort Pro→Flash→FlashLite. */
  for (int pass = 0; pass < ncat - 1; pass++) {
    for (int i = 0; i < ncat - 1 - pass; i++) {
      int ai = 3, bi = 3;
      if (strcmp(cats[i].name, TIER_GEMINI_PRO) == 0)
        ai = 0;
      else if (strcmp(cats[i].name, TIER_GEMINI_FLASH) == 0)
        ai = 1;
      else if (strcmp(cats[i].name, TIER_GEMINI_LITE) == 0)
        ai = 2;
      if (strcmp(cats[i + 1].name, TIER_GEMINI_PRO) == 0)
        bi = 0;
      else if (strcmp(cats[i + 1].name, TIER_GEMINI_FLASH) == 0)
        bi = 1;
      else if (strcmp(cats[i + 1].name, TIER_GEMINI_LITE) == 0)
        bi = 2;
      if (ai > bi) {
        struct gem_cat tmp = cats[i];
        cats[i] = cats[i + 1];
        cats[i + 1] = tmp;
      }
    }
  }
  for (int i = 0; i < ncat; i++) {
    add_tier(out, cats[i].name, (1.0 - cats[i].min_rem) * 100.0, cats[i].reset,
             0, 0, false);
  }
  set_plan(out, "Gemini");
  out->valid = out->tier_count > 0;
  cJSON_Delete(qroot);
  return (adapt_t){.produced = true};
}

/* ---- GitHub Copilot (token prefix, not Bearer) ---- */
static adapt_t adapt_copilot(const codex_quota_config_t *cfg,
                             codex_quota_data_t *out) {
  char auth[160];
  snprintf(auth, sizeof(auth), "token %s", cfg->api_key);
  http_hdr_t hdrs[] = {{"Authorization", auth},
                       {"editor-version", "Neovim/0.10.0"},
                       {"editor-plugin-version", "copilot.lua/0.1.0"},
                       {"x-github-api-version", "2025-10-01"},
                       {NULL, NULL}};
  int status = 0;
  char *body = http_request(0, "https://api.github.com/copilot_internal/user",
                            hdrs, NULL, 0, &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("Copilot 接口错误");
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root)
    return mark_err("Copilot 响应解析失败");

  out->kind = 0;
  cJSON *pi = json_get_path(root, "quota_snapshots.premium_interactions");
  if (pi &&
      !json_bool_path(root, "quota_snapshots.premium_interactions.unlimited")) {
    double pct = json_num(cJSON_GetObjectItem(pi, "percent_remaining"));
    double used = 100.0;
    if (pct > 0)
      used = 100.0 - pct;
    else {
      double ent = json_num(cJSON_GetObjectItem(pi, "entitlement"));
      double rem = json_num(cJSON_GetObjectItem(pi, "remaining"));
      if (ent > 0)
        used = (1.0 - rem / ent) * 100.0;
    }
    int64_t reset =
        parse_reset_ts(cJSON_GetObjectItem(root, "quota_reset_date"));
    add_tier(out, "premium_interactions", used, reset, 0, 0, false);
  }
  const char *plan = json_str_path(root, "copilot_plan");
  set_plan(out, plan && plan[0] ? plan : "Copilot");
  out->valid = out->tier_count > 0;
  if (!out->valid)
    set_message(out, "无配额限制");
  cJSON_Delete(root);
  return (adapt_t){.produced = true};
}

/* ---- Balance providers (single absolute value) ---- */

static adapt_t adapt_deepseek(const codex_quota_config_t *cfg,
                              codex_quota_data_t *out) {
  char auth[160];
  snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
  http_hdr_t hdrs[] = {{"Authorization", auth}, {NULL, NULL}};
  int status = 0;
  char *body = http_request(0, "https://api.deepseek.com/user/balance", hdrs,
                            NULL, 0, &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("DeepSeek 接口错误");
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root)
    return mark_err("DeepSeek 响应解析失败");
  out->kind = 1;
  bool avail = true;
  cJSON *av = cJSON_GetObjectItem(root, "is_available");
  if (cJSON_IsBool(av))
    avail = cJSON_IsTrue(av);
  cJSON *infos = cJSON_GetObjectItem(root, "balance_infos");
  cJSON *info;
  cJSON_ArrayForEach(info, infos) {
    const char *cur =
        cJSON_GetStringValue(cJSON_GetObjectItem(info, "currency"));
    if (!cur || !cur[0])
      cur = "CNY";
    snprintf(out->unit, sizeof(out->unit), "%s", cur);
    out->remaining = json_num(cJSON_GetObjectItem(info, "total_balance"));
    out->have_remaining = true;
    set_plan(out, "DeepSeek");
    break; /* first currency only */
  }
  (void)avail;
  out->valid = out->have_remaining;
  cJSON_Delete(root);
  return (adapt_t){.produced = true};
}

static adapt_t adapt_stepfun(const codex_quota_config_t *cfg,
                             codex_quota_data_t *out) {
  char auth[160];
  snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
  http_hdr_t hdrs[] = {{"Authorization", auth}, {NULL, NULL}};
  int status = 0;
  char *body = http_request(0, "https://api.stepfun.com/v1/accounts", hdrs,
                            NULL, 0, &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("StepFun 接口错误");
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root)
    return mark_err("StepFun 响应解析失败");
  out->kind = 1;
  out->remaining = json_num(cJSON_GetObjectItem(root, "balance"));
  out->have_remaining = true;
  snprintf(out->unit, sizeof(out->unit), "CNY");
  set_plan(out, "StepFun");
  out->valid = true;
  cJSON_Delete(root);
  return (adapt_t){.produced = true};
}

static adapt_t adapt_siliconflow(const codex_quota_config_t *cfg,
                                 codex_quota_data_t *out, bool is_cn) {
  const char *domain = is_cn ? "api.siliconflow.cn" : "api.siliconflow.com";
  char url[128];
  snprintf(url, sizeof(url), "https://%s/v1/user/info", domain);
  char auth[160];
  snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
  http_hdr_t hdrs[] = {{"Authorization", auth}, {NULL, NULL}};
  int status = 0;
  char *body = http_request(0, url, hdrs, NULL, 0, &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("SiliconFlow 接口错误");
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root)
    return mark_err("SiliconFlow 响应解析失败");
  out->kind = 1;
  out->remaining = json_num_path(root, "data.totalBalance");
  out->have_remaining = true;
  snprintf(out->unit, sizeof(out->unit), "%s", is_cn ? "CNY" : "USD");
  set_plan(out, is_cn ? "硅基流动" : "SiliconFlow");
  out->valid = true;
  cJSON_Delete(root);
  return (adapt_t){.produced = true};
}

static adapt_t adapt_openrouter(const codex_quota_config_t *cfg,
                                codex_quota_data_t *out) {
  char auth[160];
  snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
  http_hdr_t hdrs[] = {{"Authorization", auth}, {NULL, NULL}};
  int status = 0;
  char *body = http_request(0, "https://openrouter.ai/api/v1/credits", hdrs,
                            NULL, 0, &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("OpenRouter 接口错误");
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root)
    return mark_err("OpenRouter 响应解析失败");
  out->kind = 1;
  cJSON *data = cJSON_GetObjectItem(root, "data");
  if (!data)
    data = root;
  out->total = json_num(cJSON_GetObjectItem(data, "total_credits"));
  out->used = json_num(cJSON_GetObjectItem(data, "total_usage"));
  out->remaining = out->total - out->used;
  out->have_total = out->have_used = out->have_remaining = true;
  if (out->total > 0)
    out->percent_used = (int)((out->used * 100.0 / out->total) + 0.5);
  snprintf(out->unit, sizeof(out->unit), "USD");
  set_plan(out, "OpenRouter");
  out->valid = out->remaining > 0;
  if (!out->valid)
    set_message(out, "余额已用尽");
  cJSON_Delete(root);
  return (adapt_t){.produced = true};
}

static adapt_t adapt_novita(const codex_quota_config_t *cfg,
                            codex_quota_data_t *out) {
  char auth[160];
  snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
  http_hdr_t hdrs[] = {{"Authorization", auth}, {NULL, NULL}};
  int status = 0;
  char *body = http_request(0, "https://api.novita.ai/v3/user/balance", hdrs,
                            NULL, 0, &status);
  if (!body)
    return (adapt_t){0};
  if (status == 401 || status == 403) {
    free(body);
    return mark_auth_fail(out);
  }
  if (status / 100 != 2) {
    free(body);
    return mark_err("Novita 接口错误");
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root)
    return mark_err("Novita 响应解析失败");
  out->kind = 1;
  out->remaining = json_num(cJSON_GetObjectItem(root, "availableBalance")) /
                   10000.0; /* /10000 unit */
  out->have_remaining = true;
  snprintf(out->unit, sizeof(out->unit), "USD");
  set_plan(out, "Novita AI");
  out->valid = out->remaining > 0;
  cJSON_Delete(root);
  return (adapt_t){.produced = true};
}

/* ── Volcengine (Volcano Signature V4, AK/SK) ───────────────────────────────
 */

static const char *VOLC_HOST = "open.volcengineapi.com";
static const char *VOLC_VERSION = "2024-01-01";
static const char *VOLC_SERVICE = "ark";
static const char *VOLC_CT = "application/json; charset=utf-8";

static void hex_encode(const unsigned char *in, size_t n, char *out) {
  static const char hd[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hd[(in[i] >> 4) & 0xf];
    out[i * 2 + 1] = hd[in[i] & 0xf];
  }
  out[n * 2] = '\0';
}

static void hmac_sha256(const unsigned char *key, size_t key_len,
                        const unsigned char *data, size_t data_len,
                        unsigned char *out) {
  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, key, key_len);
  mbedtls_md_hmac_update(&ctx, data, data_len);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

/* RFC3986 encode (unreserved chars kept). */
static void uri_encode(const char *s, char *out, size_t out_len) {
  static const char hexd[] = "0123456789ABCDEF";
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p && o + 4 < out_len;
       p++) {
    unsigned char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out[o++] = c;
    } else {
      out[o++] = '%';
      out[o++] = hexd[(c >> 4) & 0xf];
      out[o++] = hexd[c & 0xf];
    }
  }
  out[o] = '\0';
}

static void volc_region(const char *base_url, char *region, size_t len) {
  snprintf(region, len, "cn-beijing");
  const char *host = strstr(base_url, "://");
  host = host ? host + 3 : base_url;
  char hbuf[128];
  snprintf(hbuf, sizeof(hbuf), "%s", host);
  char *sl = strchr(hbuf, '/');
  if (sl)
    *sl = '\0';
  char *tok = strtok(hbuf, ".");
  while (tok) {
    if (strncmp(tok, "cn-", 3) == 0 || strncmp(tok, "ap-", 3) == 0) {
      snprintf(region, len, "%s", tok);
      return;
    }
    tok = strtok(NULL, ".");
  }
}

/* Build canonical query (alphabetical): Action=X&Region=Y&Version=Z */
static void volc_canon_query(const char *action, const char *region, char *out,
                             size_t len) {
  char enc_action[64], enc_region[32], enc_ver[32];
  uri_encode(action, enc_action, sizeof(enc_action));
  uri_encode(region, enc_region, sizeof(enc_region));
  uri_encode(VOLC_VERSION, enc_ver, sizeof(enc_ver));
  snprintf(out, len, "Action=%s&Region=%s&Version=%s", enc_action, enc_region,
           enc_ver);
}

/* Compute Volcano Signature V4. */
static void volc_sign(const char *ak, const char *sk, const char *region,
                      const char *canon_query, const char *x_date,
                      const char *x_content, char *auth_out, size_t auth_len) {
  char short_date[16];
  snprintf(short_date, sizeof(short_date), "%.8s", x_date);

  /* canonical request */
  char canon_headers[256];
  snprintf(canon_headers, sizeof(canon_headers),
           "host:%s\nx-date:%s\nx-content-sha256:%s\ncontent-type:%s\n",
           VOLC_HOST, x_date, x_content, VOLC_CT);
  char canon_req[1024];
  snprintf(canon_req, sizeof(canon_req),
           "POST\n/\n%s\n%s\nhost;x-date;x-content-sha256;content-type\n%s",
           canon_query, canon_headers, x_content);

  unsigned char req_hash[32];
  mbedtls_sha256((const unsigned char *)canon_req, strlen(canon_req), req_hash,
                 0);
  char req_hex[65];
  hex_encode(req_hash, 32, req_hex);

  /* credential scope + string to sign */
  char cred_scope[96];
  snprintf(cred_scope, sizeof(cred_scope), "%s/%s/%s/request", short_date,
           region, VOLC_SERVICE);
  char sts[640];
  snprintf(sts, sizeof(sts), "HMAC-SHA256\n%s\n%s\n%s", x_date, cred_scope,
           req_hex);

  /* signing key chain: kDate=HMAC(SK,date), kRegion, kService, kSigning */
  unsigned char kDate[32], kRegion[32], kService[32], kSigning[32], sig[32];
  hmac_sha256((const unsigned char *)sk, strlen(sk),
              (const unsigned char *)short_date, strlen(short_date), kDate);
  hmac_sha256(kDate, 32, (const unsigned char *)region, strlen(region),
              kRegion);
  hmac_sha256(kRegion, 32, (const unsigned char *)VOLC_SERVICE,
              strlen(VOLC_SERVICE), kService);
  hmac_sha256(kService, 32, (const unsigned char *)"request", 7, kSigning);
  hmac_sha256(kSigning, 32, (const unsigned char *)sts, strlen(sts), sig);
  char sig_hex[65];
  hex_encode(sig, 32, sig_hex);

  snprintf(
      auth_out, auth_len,
      "HMAC-SHA256 Credential=%s/%s, "
      "SignedHeaders=host;x-date;x-content-sha256;content-type, Signature=%s",
      ak, cred_scope, sig_hex);
}

static bool volc_is_auth_error_code(const char *code) {
  if (!code)
    return false;
  static const char *const needles[] = {
      "auth",      "signature",  "accessdenied", "denied", "unauthorized",
      "forbidden", "credential", "token",        NULL};
  char low[64];
  snprintf(low, sizeof(low), "%s", code);
  for (char *p = low; *p; p++)
    *p = tolower((unsigned char)*p);
  for (int i = 0; needles[i]; i++)
    if (strstr(low, needles[i]))
      return true;
  return false;
}

/* One Volcengine OpenAPI call. action e.g. "GetAFPUsage". */
typedef struct {
  cJSON *body; /* parsed response root (caller frees), NULL on error */
  bool auth_failed;
  const char *error; /* static error string for soft errors */
} volc_call_t;

static volc_call_t volc_call(const char *ak, const char *sk, const char *region,
                             const char *action) {
  volc_call_t r = {0};
  char canon_query[160];
  volc_canon_query(action, region, canon_query, sizeof(canon_query));
  char url[256];
  snprintf(url, sizeof(url), "https://%s/?%s", VOLC_HOST, canon_query);

  /* empty body → sha256 of empty string */
  const char *empty_hash =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  time_t now = time(NULL);
  struct tm tm_utc;
  gmtime_r(&now, &tm_utc);
  char x_date[24];
  strftime(x_date, sizeof(x_date), "%Y%m%dT%H%M%SZ", &tm_utc);

  char auth[512];
  volc_sign(ak, sk, region, canon_query, x_date, empty_hash, auth,
            sizeof(auth));

  http_hdr_t hdrs[] = {{"X-Date", x_date},
                       {"X-Content-Sha256", empty_hash},
                       {"Content-Type", VOLC_CT},
                       {"Authorization", auth},
                       {NULL, NULL}};
  int status = 0;
  char *body = http_request(1, url, hdrs, "", 0, &status);
  if (!body) {
    r.error = "火山网络错误";
    return r;
  }
  if (status == 401 || status == 403) {
    free(body);
    r.auth_failed = true;
    return r;
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root) {
    r.error = "火山响应解析失败";
    return r;
  }
  /* check error envelope */
  cJSON *meta = json_get_path(root, "ResponseMetadata.Error");
  if (!meta)
    meta = cJSON_GetObjectItem(root, "Error");
  if (meta) {
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "Code"));
    const char *msg =
        cJSON_GetStringValue(cJSON_GetObjectItem(meta, "Message"));
    if (code || msg) {
      if (code && volc_is_auth_error_code(code)) {
        cJSON_Delete(root);
        r.auth_failed = true;
        return r;
      }
      cJSON_Delete(root);
      r.error = msg && msg[0] ? msg : "火山API错误";
      return r;
    }
  }
  if (status / 100 != 2) {
    cJSON_Delete(root);
    r.error = "火山接口错误";
    return r;
  }
  r.body = root;
  return r;
}

/* AFP tiers (Agent Plan, absolute quota). */
static void parse_afp_tiers(cJSON *result, codex_quota_data_t *out) {
  struct {
    const char *key;
    const char *name;
  } wins[] = {
      {"AFPFiveHour", TIER_FIVE_HOUR},
      {"AFPWeekly", TIER_WEEKLY},
      {"AFPMonthly", TIER_MONTHLY},
  };
  for (int i = 0; i < 3; i++) {
    cJSON *w = cJSON_GetObjectItem(result, wins[i].key);
    if (!w)
      continue;
    double quota = json_num(cJSON_GetObjectItem(w, "Quota"));
    if (quota <= 0)
      continue;
    double used = json_num(cJSON_GetObjectItem(w, "Used"));
    int64_t reset = parse_reset_ts(cJSON_GetObjectItem(w, "ResetTime"));
    add_tier(out, wins[i].name, used / quota * 100.0, reset, used, quota, true);
  }
}

/* Coding Plan tiers (percent native). */
static void parse_coding_plan_tiers(cJSON *result, codex_quota_data_t *out) {
  cJSON *arr = NULL;
  const char *keys[] = {"QuotaUsage", "Usages", "Details", NULL};
  for (int i = 0; keys[i] && !arr; i++) {
    cJSON *a = cJSON_GetObjectItem(result, keys[i]);
    if (cJSON_IsArray(a) && cJSON_GetArraySize(a) > 0)
      arr = a;
  }
  if (!arr)
    return;
  cJSON *item;
  cJSON_ArrayForEach(item, arr) {
    const char *label = NULL;
    const char *lkeys[] = {"Level", "Type", "Period", "Label", "Window", NULL};
    for (int i = 0; lkeys[i] && !(label && label[0]); i++) {
      const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(item, lkeys[i]));
      if (v && v[0])
        label = v;
    }
    if (!label)
      continue;
    const char *name = NULL;
    if (!strcasecmp(label, "session") || !strcasecmp(label, "5h") ||
        !strcasecmp(label, "fivehour") || !strcasecmp(label, "five_hour") ||
        !strcasecmp(label, "rolling_5h"))
      name = TIER_FIVE_HOUR;
    else if (!strcasecmp(label, "weekly") || !strcasecmp(label, "week") ||
             !strcasecmp(label, "7d"))
      name = TIER_WEEKLY;
    else if (!strcasecmp(label, "monthly") || !strcasecmp(label, "month"))
      name = TIER_MONTHLY;
    if (!name)
      continue;
    double pct = 0;
    const char *pkeys[] = {"Percent", "UsedPercent", "UsagePercent", NULL};
    for (int i = 0; pkeys[i]; i++) {
      cJSON *p = cJSON_GetObjectItem(item, pkeys[i]);
      if (cJSON_IsNumber(p)) {
        pct = p->valuedouble;
        break;
      }
    }
    int64_t reset = 0;
    cJSON *rt = cJSON_GetObjectItem(item, "ResetTime");
    if (!rt)
      rt = cJSON_GetObjectItem(item, "ResetTimestamp");
    reset = parse_reset_ts(rt);
    add_tier(out, name, pct, reset, 0, 0, false);
  }
}

static adapt_t adapt_volcengine(const codex_quota_config_t *cfg,
                                codex_quota_data_t *out) {
  if (!cfg->access_key_id[0] || !cfg->secret_access_key[0])
    return mark_err("需要账号级 AccessKey (非推理 Key)");
  char region[24];
  volc_region(cfg->api_url, region, sizeof(region));

  /* 1) Agent Plan: GetAFPUsage */
  volc_call_t c = volc_call(cfg->access_key_id, cfg->secret_access_key, region,
                            "GetAFPUsage");
  if (c.auth_failed)
    return mark_auth_fail(out);
  if (c.body) {
    out->kind = 0;
    cJSON *result = cJSON_GetObjectItem(c.body, "Result");
    if (!result)
      result = c.body;
    parse_afp_tiers(result, out);
    if (out->tier_count > 0) {
      const char *pt =
          cJSON_GetStringValue(cJSON_GetObjectItem(result, "PlanType"));
      set_plan(out, pt && pt[0] ? pt : "Agent Plan");
      out->valid = true;
      cJSON_Delete(c.body);
      return (adapt_t){.produced = true};
    }
    cJSON_Delete(c.body);
  } else if (c.error) {
    /* record but try coding plan next */
  }

  /* 2) Coding Plan: GetCodingPlanUsage */
  c = volc_call(cfg->access_key_id, cfg->secret_access_key, region,
                "GetCodingPlanUsage");
  if (c.auth_failed)
    return mark_auth_fail(out);
  if (c.body) {
    out->kind = 0;
    cJSON *result = cJSON_GetObjectItem(c.body, "Result");
    if (!result)
      result = c.body;
    parse_coding_plan_tiers(result, out);
    if (out->tier_count > 0) {
      set_plan(out, "Coding Plan");
      out->valid = true;
      cJSON_Delete(c.body);
      return (adapt_t){.produced = true};
    }
    cJSON_Delete(c.body);
    return mark_err("火山无活跃订阅");
  }
  return mark_err(c.error ? c.error : "火山查询失败");
}

/* ── Dispatch ───────────────────────────────────────────────────────────────
 */

static adapt_t run_provider(const codex_quota_config_t *cfg,
                            codex_quota_data_t *out) {
  switch ((quota_provider_t)cfg->provider) {
  case QUOTA_PROVIDER_CUSTOM: {
    char url[224];
    build_quota_url(cfg, url, sizeof(url));
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
    http_hdr_t hdrs[] = {{"Authorization", auth}, {NULL, NULL}};
    int status = 0;
    char *body = http_request(0, url, hdrs, NULL, 0, &status);
    if (!body)
      return (adapt_t){0};
    if (status == 401 || status == 403) {
      free(body);
      return mark_auth_fail(out);
    }
    if (status / 100 != 2) {
      free(body);
      return mark_err("中转站接口错误");
    }
    bool ok = parse_custom_balance(body, out, cfg);
    free(body);
    set_plan(out, codex_quota_provider_name(QUOTA_PROVIDER_CUSTOM));
    return (adapt_t){.produced = true, .error = ok ? NULL : NULL};
  }
  case QUOTA_PROVIDER_KIMI:
    return adapt_kimi(cfg, out);
  case QUOTA_PROVIDER_ZHIPU_CN:
    return adapt_zhipu(cfg, out, true);
  case QUOTA_PROVIDER_ZHIPU_EN:
    return adapt_zhipu(cfg, out, false);
  case QUOTA_PROVIDER_MINIMAX_CN:
    return adapt_minimax(cfg, out, true);
  case QUOTA_PROVIDER_MINIMAX_EN:
    return adapt_minimax(cfg, out, false);
  case QUOTA_PROVIDER_ZENMUX:
    return adapt_zenmux(cfg, out);
  case QUOTA_PROVIDER_VOLCENGINE:
    return adapt_volcengine(cfg, out);
  case QUOTA_PROVIDER_CODEX:
    return adapt_codex(cfg, out);
  case QUOTA_PROVIDER_GEMINI:
    return adapt_gemini(cfg, out);
  case QUOTA_PROVIDER_COPILOT:
    return adapt_copilot(cfg, out);
  case QUOTA_PROVIDER_DEEPSEEK:
    return adapt_deepseek(cfg, out);
  case QUOTA_PROVIDER_STEPFUN:
    return adapt_stepfun(cfg, out);
  case QUOTA_PROVIDER_SILICONFLOW:
    return adapt_siliconflow(cfg, out, true);
  case QUOTA_PROVIDER_SILICONFLOW_EN:
    return adapt_siliconflow(cfg, out, false);
  case QUOTA_PROVIDER_OPENROUTER:
    return adapt_openrouter(cfg, out);
  case QUOTA_PROVIDER_NOVITA:
    return adapt_novita(cfg, out);
  default:
    return mark_err("未知平台");
  }
}

/* ── UI: time format (6/23 10:58) ───────────────────────────────────────────
 */

static void format_update_time(char *buf, size_t len) {
  struct tm tm;
  if (time_sync_get_local_relaxed(&tm))
    snprintf(buf, len, "%d/%d %02d:%02d \xe6\x9b\xb4\xe6\x96\xb0",
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
  else
    snprintf(buf, len, "\xe6\x9c\xaa\xe5\x90\x8c\xe6\xad\xa5");
}

/* friendly tier label in Chinese */
static const char *tier_label_cn(const char *name) {
  if (!name)
    return "?";
  if (strcmp(name, TIER_FIVE_HOUR) == 0)
    return "5小时限额";
  if (strcmp(name, TIER_WEEKLY) == 0)
    return "每周限额";
  if (strcmp(name, TIER_SEVEN_DAY) == 0)
    return "7天限额";
  if (strcmp(name, TIER_MONTHLY) == 0)
    return "每月限额";
  if (strcmp(name, TIER_GEMINI_PRO) == 0)
    return "Gemini Pro";
  if (strcmp(name, TIER_GEMINI_FLASH) == 0)
    return "Gemini Flash";
  if (strcmp(name, TIER_GEMINI_LITE) == 0)
    return "Gemini Lite";
  if (strcmp(name, "premium_interactions") == 0)
    return "高级交互";
  return name;
}

/* format countdown to reset: e.g. "3h12m" / "2d5h" / "--" */
static void format_reset_countdown(int64_t reset_ts, char *buf, size_t len) {
  if (reset_ts <= 0) {
    snprintf(buf, len, "--");
    return;
  }
  time_t now = time(NULL);
  double diff = difftime((time_t)reset_ts, now);
  if (diff < 0) {
    snprintf(buf, len, "已重置");
    return;
  }
  int days = (int)(diff / 86400);
  int hours = (int)((diff - days * 86400) / 3600);
  int mins = (int)((diff - days * 86400 - hours * 3600) / 60);
  if (days > 0)
    snprintf(buf, len, "%dd%dh", days, hours);
  else if (hours > 0)
    snprintf(buf, len, "%dh%dm", hours, mins);
  else
    snprintf(buf, len, "%dm", mins);
}

static void format_amount(char *buf, size_t len, bool have, double value,
                          const char *unit) {
  if (!have) {
    snprintf(buf, len, "--");
    return;
  }
  if (fabs(value) >= 1000.0)
    snprintf(buf, len, "%.0f %s", value, unit ? unit : "");
  else
    snprintf(buf, len, "%.2f %s", value, unit ? unit : "");
}

static void format_abs_value(char *buf, size_t len, double val) {
  if (val >= 1000.0)
    snprintf(buf, len, "%.0f", val);
  else if (val >= 10.0)
    snprintf(buf, len, "%.1f", val);
  else
    snprintf(buf, len, "%.2f", val);
}

/* ── UI rendering ───────────────────────────────────────────────────────────
 */

/* draw a clean progress bar */
static void draw_bar(fb_t *fb, int x, int y, int w, int h, int percent,
                     fb_color_t fill) {
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;
  fb_rect(fb, x, y, w, h, COLOR_BLACK);
  if (percent > 0) {
    int inner = (w - 4) * percent / 100;
    if (inner < 2)
      inner = 2;
    fb_fill_rect(fb, x + 2, y + 2, inner, h - 4, fill);
  }
}

/* Header: red band, title "Coding额度", center time, battery right. */
static void draw_header(fb_t *fb, const codex_quota_data_t *data) {
  const int W = fb->width;
  const int hh = 30;
  fb_fill_rect(fb, 6, 6, W - 12, hh, COLOR_RED);
  fb_utf8_scaled(fb, 16, 13, "Coding额度", COLOR_WHITE, 1);

  char center[32];
  format_update_time(center, sizeof(center));
  int cw = ui_text_width(center, 1);
  fb_utf8_scaled(fb, (W - cw) / 2, 14, center, COLOR_WHITE, 1);

  battery_status_t bat = {0};
  battery_mon_get_status(&bat);
  char right[12];
  if (bat.valid)
    snprintf(right, sizeof(right), "%u%%", (unsigned)bat.percent);
  else
    snprintf(right, sizeof(right), "--");
  int rw = ui_text_width(right, 1);
  fb_utf8_scaled(fb, W - 16 - rw, 14, right, COLOR_WHITE, 1);
  (void)data;
}

/* draw one tier row: label (left), bar (center), pct + countdown (right).
 * Layout is right-anchored so the bar never overlaps the pct/countdown text. */
static void draw_tier_row(fb_t *fb, int y, const quota_tier_t *t, int bar_x,
                          int bar_w) {
  const int W = fb->width;
  const int right_pad = 14; /* right margin */
  const int gap = 4;        /* gap between pct and countdown */

  int pct = (int)(t->utilization + 0.5);
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  fb_color_t color = (pct >= 80) ? COLOR_RED : COLOR_BLACK;

  /* right cluster: countdown (right-aligned) then pct to its left */
  char cd[16];
  format_reset_countdown(t->reset_ts, cd, sizeof(cd));
  int cd_x = W - right_pad - ui_text_width(cd, 1);
  fb_utf8_scaled(fb, cd_x, y - 2, cd, COLOR_BLACK, 1);

  char pct_s[10];
  snprintf(pct_s, sizeof(pct_s), "%d%%", pct);
  int pct_w = ui_text_width(pct_s, 1);
  int pct_x = cd_x - gap - pct_w;
  fb_utf8_scaled(fb, pct_x, y - 2, pct_s, color, 1);

  /* bar occupies [bar_x .. pct_x - gap). */
  int bar_end = pct_x - gap;
  const char *lbl = tier_label_cn(t->name);
  fb_utf8_scaled_maxw(fb, 14, y - 2, lbl, COLOR_BLACK, 1, bar_x - 16);
  int bw = bar_end - bar_x;
  if (bw > bar_w)
    bw = bar_w; /* caller's cap */
  if (bw < 40)
    bw = 40;
  draw_bar(fb, bar_x, y, bw, 12, pct, color);

  /* abs value line for tiers that carry it (ZenMux/Volcengine AFP) */
  if (t->have_abs && t->max_value > 0) {
    char abs[32];
    char used_s[12], max_s[12];
    format_abs_value(used_s, sizeof(used_s), t->used_value);
    format_abs_value(max_s, sizeof(max_s), t->max_value);
    snprintf(abs, sizeof(abs), "%s/%s", used_s, max_s);
    fb_utf8_scaled(fb, 14, y + 10, abs, COLOR_BLACK, 1);
  }
}

/* ── Plan detection: choose primary scene ───────────────────────────────────
 * Returns a "scene" hint for layout:
 *  SCENE_QUOTA   - multi-window percent tiers
 * (Kimi/Zhipu/MiniMax/ZenMux/Volc/Codex/Gemini/Copilot) SCENE_BALANCE - single
 * absolute balance (Custom/DeepSeek/StepFun/SiliconFlow/OpenRouter/Novita)
 *  SCENE_CODEX_5H - Codex primary 5h-hour limit (highlighted) */
enum { SCENE_QUOTA = 0, SCENE_BALANCE = 1, SCENE_CODEX_5H = 2 };

static int detect_scene(const codex_quota_config_t *cfg,
                        const codex_quota_data_t *data) {
  if (!data || !data->valid)
    return SCENE_QUOTA;
  if (data->kind == 1)
    return SCENE_BALANCE;
  /* Codex/ChatGPT plan: highlight 5-hour primary window */
  if (cfg->provider == QUOTA_PROVIDER_CODEX && data->tier_count > 0)
    return SCENE_CODEX_5H;
  return SCENE_QUOTA;
}

static void render_quota_page(const codex_quota_config_t *cfg,
                              const codex_quota_data_t *data,
                              const char *error) {
  fb_t *fb = fb_create();
  if (!fb)
    return;

  const int H = fb->height;
  const int W = fb->width;

  fb_rect(fb, 6, 6, W - 12, H - 12, COLOR_BLACK);
  draw_header(fb, data);

  const char *prov = codex_quota_provider_name((quota_provider_t)cfg->provider);

  if (error) {
    ui_draw_empty_state(fb, "额度不可用", error);
    fb_hline(fb, 14, H - 28, W - 28, COLOR_BLACK);
    fb_utf8_scaled(fb, 20, H - 20, prov, COLOR_BLACK, 1);
    char rinfo[24];
    snprintf(rinfo, sizeof(rinfo), "显示时更新");
    int rw = ui_text_width(rinfo, 1);
    fb_utf8_scaled(fb, W - 18 - rw, H - 20, rinfo, COLOR_BLACK, 1);
    epd_display_fb_free(fb);
    return;
  }

  int scene = detect_scene(cfg, data);
  int y = 48;

  /* Plan label bar: provider left, plan right (no separator dot). */
  fb_fill_rect(fb, 12, y, W - 24, 18, COLOR_BLACK);
  fb_utf8_scaled(fb, 18, y + 4, prov, COLOR_WHITE, 1);
  if (data->plan[0]) {
    int pw = ui_text_width(data->plan, 1);
    fb_utf8_scaled(fb, W - 18 - pw, y + 4, data->plan, COLOR_WHITE, 1);
  }
  y += 26;

  if (scene == SCENE_BALANCE) {
    /* Balance scene: big remaining value + usage breakdown */
    char remain_s[32], used_s[32], total_s[32];
    format_amount(remain_s, sizeof(remain_s), data->have_remaining,
                  data->remaining, data->unit);
    format_amount(used_s, sizeof(used_s), data->have_used, data->used,
                  data->unit);
    format_amount(total_s, sizeof(total_s), data->have_total, data->total,
                  data->unit);

    fb_utf8_scaled(fb, 18, y, "剩余额度", COLOR_BLACK, 1);
    y += 24;
    fb_utf8_scaled(fb, 18, y, remain_s,
                   data->have_remaining && data->remaining <= 0 ? COLOR_RED
                                                                : COLOR_BLACK,
                   2);
    y += 34;

    int pct = data->percent_used;
    if (pct <= 0 && data->have_total && data->total > 0 && data->have_used)
      pct = (int)((data->used * 100.0 / data->total) + 0.5);
    fb_color_t bc = (pct >= 80) ? COLOR_RED : COLOR_BLACK;
    draw_bar(fb, 18, y, W - 36, 14, pct, bc);
    char pct_s[16];
    snprintf(pct_s, sizeof(pct_s), "已用 %d%%", pct);
    fb_utf8_scaled(fb, 18, y + 18, pct_s, bc, 1);
    y += 40;

    /* detail rows */
    if (data->have_used) {
      fb_utf8_scaled(fb, 18, y, "已用", COLOR_BLACK, 1);
      int uw = ui_text_width(used_s, 1);
      fb_utf8_scaled(fb, W - 18 - uw, y, used_s, COLOR_BLACK, 1);
      y += 20;
    }
    if (data->have_total) {
      fb_utf8_scaled(fb, 18, y, "总额", COLOR_BLACK, 1);
      int tw = ui_text_width(total_s, 1);
      fb_utf8_scaled(fb, W - 18 - tw, y, total_s, COLOR_BLACK, 1);
      y += 20;
    }
  } else {
    /* Quota scene (also SCENE_CODEX_5H): list tiers with bars.
     * For Codex 5h, emphasize primary window with larger first card. */
    int bar_x = 100;
    int bar_w = W - 100 /*left*/ - 14 /*right pad*/ - 50 /*pct*/ - 14;
    if (bar_w < 80)
      bar_w = 80;

    if (scene == SCENE_CODEX_5H && data->tier_count > 0) {
      /* Emphasized 5h card */
      const quota_tier_t *t = &data->tiers[0];
      int pct = (int)(t->utilization + 0.5);
      if (pct < 0)
        pct = 0;
      if (pct > 100)
        pct = 100;
      fb_color_t color = (pct >= 80) ? COLOR_RED : COLOR_BLACK;

      fb_utf8_scaled(fb, 18, y, "5小时限额", COLOR_BLACK, 2);
      char big[16];
      snprintf(big, sizeof(big), "%d%%", pct);
      int bw = ui_text_width(big, 2);
      fb_utf8_scaled(fb, W - 18 - bw, y, big, color, 2);
      y += 30;
      draw_bar(fb, 18, y, W - 36, 16, pct, color);
      y += 22;

      char cd[16];
      format_reset_countdown(t->reset_ts, cd, sizeof(cd));
      char reset_line[40];
      snprintf(reset_line, sizeof(reset_line), "剩余 %s", cd);
      fb_utf8_scaled(fb, 18, y, reset_line, COLOR_BLACK, 1);
      y += 26;

      /* secondary windows compact */
      for (int i = 1; i < data->tier_count && y < H - 60; i++) {
        draw_tier_row(fb, y, &data->tiers[i], bar_x, bar_w);
        y += 24;
      }
    } else {
      /* Generic multi-window list */
      for (int i = 0; i < data->tier_count && y < H - 60; i++) {
        draw_tier_row(fb, y, &data->tiers[i], bar_x, bar_w);
        y += 26;
      }
    }

    /* message line (e.g. One-API conversion note) */
    if (data->message[0] && y < H - 56) {
      fb_utf8_scaled_maxw(fb, 18, y, data->message, COLOR_BLACK, 1, W - 36);
    }
  }

  /* footer */
  ui_draw_dotted_hline(fb, 14, H - 30, W - 28, COLOR_BLACK, 4);
  /* provider left, refresh info right (no separator dot) */
  fb_utf8_scaled(fb, 18, H - 22, prov, COLOR_BLACK, 1);
  uint32_t rm = cfg->refresh_min;
  char rinfo[24];
  if (rm > 0)
    snprintf(rinfo, sizeof(rinfo), "自动%lum", (unsigned long)rm);
  else
    snprintf(rinfo, sizeof(rinfo), "显示时更新");
  int rinfo_w = ui_text_width(rinfo, 1);
  /* rinfo right-aligned before the page indicator */
  char pg[8];
  snprintf(pg, sizeof(pg), "%d/1", cfg->enabled ? 1 : 0);
  int pgw = ui_text_width(pg, 1);
  fb_utf8_scaled(fb, W - 18 - pgw - 6 - rinfo_w, H - 22, rinfo, COLOR_BLACK, 1);
  fb_utf8_scaled(fb, W / 2 - 16, H - 22, "[Zz]", COLOR_BLACK, 1);
  fb_utf8_scaled(fb, W - 18 - pgw, H - 22, pg, COLOR_BLACK, 1);

  epd_display_fb_free(fb);
}

/* ── Auto refresh task ──────────────────────────────────────────────────────
 */

static bool quota_auto_refresh_due(uint32_t refresh_min) {
  TickType_t last = s_last_fetch_tick;
  if (last == 0)
    return true;
  TickType_t interval = pdMS_TO_TICKS(refresh_min * 60UL * 1000UL);
  return (xTaskGetTickCount() - last) >= interval;
}

static void codex_quota_auto_task(void *arg) {
  (void)arg;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(QUOTA_AUTO_POLL_MS));

    codex_quota_config_t cfg;
    config_snapshot(&cfg);
    if (!s_auto_network_allowed)
      continue;
    if (!config_ready(&cfg) || cfg.refresh_min == 0)
      continue;
    if (display_mode_active() != DISPLAY_MODE_CODEX_QUOTA)
      continue;
    if (!quota_auto_refresh_due(cfg.refresh_min))
      continue;

    ESP_LOGI(TAG, "auto refresh (%s) every %lum",
             codex_quota_provider_name((quota_provider_t)cfg.provider),
             (unsigned long)cfg.refresh_min);
    esp_err_t err = codex_quota_show();
    if (err != ESP_OK)
      ESP_LOGW(TAG, "auto refresh failed: %s", esp_err_to_name(err));
  }
}

void codex_quota_set_auto_network_allowed(bool allowed) {
  s_auto_network_allowed = allowed;
}

esp_err_t codex_quota_init(void) {
  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex)
    return ESP_ERR_NO_MEM;
  memset(&s_data, 0, sizeof(s_data));
  nvs_load();
  ESP_LOGI(TAG,
           "init enabled=%d provider=%s url_set=%d key_set=%d refresh=%lum",
           s_cfg.enabled,
           codex_quota_provider_name((quota_provider_t)s_cfg.provider),
           s_cfg.api_url[0] != '\0', s_cfg.api_key[0] != '\0',
           (unsigned long)s_cfg.refresh_min);
  if (!s_auto_task) {
    BaseType_t ok = xTaskCreate(codex_quota_auto_task, "quota_auto",
                                QUOTA_AUTO_STACK, NULL, 3, &s_auto_task);
    if (ok != pdPASS) {
      ESP_LOGE(TAG, "auto task create failed");
      return ESP_ERR_NO_MEM;
    }
  }
  return ESP_OK;
}

esp_err_t codex_quota_get_config(codex_quota_config_t *out) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  config_snapshot(out);
  return ESP_OK;
}

/* Return shared settings + a specific provider's saved credentials, without
 * changing the active provider. Used by the web UI to load per-provider token
 * state when the user switches the dropdown. If provider is out of range the
 * active provider is used. */
esp_err_t codex_quota_get_provider_config(codex_quota_config_t *out,
                                          uint8_t provider) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  config_snapshot(out);
  if (provider >= QUOTA_PROVIDER_MAX)
    return ESP_OK; /* active provider's creds already in snapshot */

  /* Clear credentials first: nvs_load_cred only writes keys that exist, so
   * without clearing, a provider with no saved key would inherit the active
   * provider's key (the "已保存" leak across providers). */
  out->api_key[0] = '\0';
  out->api_url[0] = '\0';
  out->access_key_id[0] = '\0';
  out->secret_access_key[0] = '\0';

  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
    return ESP_OK; /* nothing stored yet; creds stay empty */
  nvs_load_cred(h, provider, out);
  nvs_close(h);
  return ESP_OK;
}

/* Credentials in cfg belong to cfg->provider (the web UI loads per-provider
 * state before saving). We persist them into that provider's NVS slot, update
 * the shared settings, and if the provider changed, load the newly-active
 * provider's credentials into the runtime config so the next GET reflects it.
 */
esp_err_t codex_quota_set_config(const codex_quota_config_t *cfg) {
  if (!cfg)
    return ESP_ERR_INVALID_ARG;
  codex_quota_config_t snap = *cfg;
  normalize_config(&snap);

  uint8_t old_provider;
  portENTER_CRITICAL(&s_cfg_mux);
  old_provider = s_cfg.provider;
  portEXIT_CRITICAL(&s_cfg_mux);

  nvs_handle_t h;
  bool have_nvs = (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK);
  if (have_nvs) {
    /* persist shared settings */
    nvs_set_u8(h, "enabled", snap.enabled ? 1 : 0);
    nvs_set_u8(h, "provider", snap.provider);
    nvs_set_str(h, "unit", snap.unit);
    nvs_set_u32(h, "refresh", snap.refresh_min);
    /* persist the selected provider's credentials */
    nvs_save_cred(h, snap.provider, &snap);
    nvs_commit(h);
  }

  /* If the active provider changed, reload credentials for the new provider
   * so the runtime config (and next GET) shows the new provider's saved state
   * rather than the just-saved form (which may have been for the old one). */
  if (old_provider != snap.provider && have_nvs)
    nvs_load_cred(h, snap.provider, &snap);

  if (have_nvs)
    nvs_close(h);

  portENTER_CRITICAL(&s_cfg_mux);
  s_cfg = snap;
  portEXIT_CRITICAL(&s_cfg_mux);
  ESP_LOGI(TAG, "config saved enabled=%d provider=%s refresh=%lum",
           snap.enabled,
           codex_quota_provider_name((quota_provider_t)snap.provider),
           (unsigned long)snap.refresh_min);
  return ESP_OK;
}

void codex_quota_get_data_copy(codex_quota_data_t *out) {
  if (!out)
    return;
  if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    *out = s_data;
    xSemaphoreGive(s_mutex);
  } else {
    memset(out, 0, sizeof(*out));
  }
}

/* ── Fetch + render ─────────────────────────────────────────────────────────
 */

static esp_err_t codex_quota_show_inner(void) {
  if (!epd_is_ready())
    return ESP_ERR_INVALID_STATE;

  codex_quota_config_t cfg;
  config_snapshot(&cfg);
  if (!config_ready(&cfg)) {
    render_quota_page(&cfg, NULL, "请先在网页配置平台与凭据");
    return ESP_ERR_INVALID_STATE;
  }

  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(90000)) != pdTRUE)
    return ESP_ERR_TIMEOUT;

  s_last_fetch_tick = xTaskGetTickCount();
  codex_quota_data_t parsed;
  memset(&parsed, 0, sizeof(parsed));
  parsed.auth_ok = true;

  adapt_t r = run_provider(&cfg, &parsed);
  esp_err_t ret = ESP_OK;

  if (!r.produced) {
    render_quota_page(&cfg, NULL, "网络请求失败，请检查网络");
    ret = ESP_FAIL;
    goto out;
  }
  if (r.auth_failed) {
    render_quota_page(&cfg, &parsed,
                      parsed.message[0] ? parsed.message : "凭据无效或已过期");
    s_data = parsed;
    ret = ESP_FAIL;
    goto out;
  }
  if (!parsed.valid) {
    render_quota_page(
        &cfg, &parsed,
        r.error ? r.error
                : (parsed.message[0] ? parsed.message : "未获取到额度数据"));
    s_data = parsed;
    ret = ESP_FAIL;
    goto out;
  }

  /* stamp update time + provider unit fallback */
  format_update_time(parsed.update_time, sizeof(parsed.update_time));
  if (!parsed.unit[0])
    snprintf(parsed.unit, sizeof(parsed.unit), "%s",
             cfg.unit[0] ? cfg.unit : "USD");
  s_data = parsed;
  render_quota_page(&cfg, &s_data, NULL);

out:
  xSemaphoreGive(s_mutex);
  return ret;
}

typedef struct {
  TaskHandle_t waiter;
  esp_err_t result;
} quota_show_job_t;

static void codex_quota_show_task(void *arg) {
  quota_show_job_t *job = (quota_show_job_t *)arg;
  job->result = codex_quota_show_inner();
  xTaskNotifyGive(job->waiter);
  vTaskDelete(NULL);
}

esp_err_t codex_quota_show(void) {
  quota_show_job_t job = {
      .waiter = xTaskGetCurrentTaskHandle(),
      .result = ESP_FAIL,
  };
  BaseType_t ok = xTaskCreatePinnedToCore(codex_quota_show_task, "quota_show",
                                          QUOTA_SHOW_STACK, &job, 4, NULL, 0);
  if (ok != pdPASS) {
    ESP_LOGW(TAG, "quota_show task create failed; running inline");
    return codex_quota_show_inner();
  }
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  return job.result;
}
