#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ── Platform provider (auto-detected from base_url, or explicit override) ──
 * Mirrors cc-switch usage-query module naming. CUSTOM = legacy relay / 中转站
 * using the generic flexible balance parser. */
typedef enum {
  QUOTA_PROVIDER_CUSTOM = 0, /* 中转站 / generic balance (flexible parse) */
  QUOTA_PROVIDER_KIMI,       /* Kimi For Coding        (subscription) */
  QUOTA_PROVIDER_ZHIPU_CN,   /* 智谱 GLM bigmodel.cn   (subscription) */
  QUOTA_PROVIDER_ZHIPU_EN,   /* 智谱 GLM z.ai          (subscription) */
  QUOTA_PROVIDER_MINIMAX_CN, /* MiniMax minimaxi.com   (subscription) */
  QUOTA_PROVIDER_MINIMAX_EN, /* MiniMax minimax.io     (subscription) */
  QUOTA_PROVIDER_ZENMUX,     /* ZenMux                 (subscription) */
  QUOTA_PROVIDER_VOLCENGINE, /* 火山方舟 Agent/Coding   (subscription, AK/SK) */
  QUOTA_PROVIDER_CODEX,      /* Codex / ChatGPT        (subscription, token) */
  QUOTA_PROVIDER_GEMINI,     /* Gemini                 (subscription, token) */
  QUOTA_PROVIDER_COPILOT,    /* GitHub Copilot         (subscription, token) */
  QUOTA_PROVIDER_DEEPSEEK,   /* DeepSeek               (balance) */
  QUOTA_PROVIDER_STEPFUN,    /* StepFun                (balance) */
  QUOTA_PROVIDER_SILICONFLOW,    /* 硅基流动 siliconflow.cn(balance) */
  QUOTA_PROVIDER_SILICONFLOW_EN, /* SiliconFlow .com      (balance) */
  QUOTA_PROVIDER_OPENROUTER,     /* OpenRouter             (balance) */
  QUOTA_PROVIDER_NOVITA,         /* Novita AI              (balance) */
  QUOTA_PROVIDER_MAX
} quota_provider_t;

/* Configuration. Most providers need api_key only; custom needs api_url too;
 * volcengine needs access_key_id + secret_access_key instead of api_key;
 * token-based providers (codex/gemini/copilot) take the access_token in
 * api_key. */
typedef struct {
  bool enabled;
  char api_url[192]; /* Full quota endpoint URL (custom relay) or data-plane
                        base */
  char api_key[128]; /* Bearer key / access token / AK-derived (provider
                        dependent) */
  char access_key_id[96];     /* Volcengine only: account AccessKey ID */
  char secret_access_key[96]; /* Volcengine only: account Secret Access Key */
  char unit[16];              /* Display unit, e.g. USD / 元 / credits */
  uint8_t provider;           /* quota_provider_t */
  uint32_t refresh_min; /* 0=manual only; otherwise auto-refresh interval */
} codex_quota_config_t;

/* One usage window (5h / weekly / monthly / per-model). Utilization is the
 * USED percent 0-100. reset_ts is epoch seconds (0 = none). */
typedef struct {
  char name[20];      /* five_hour / weekly_limit / monthly / ... */
  double utilization; /* USED percent 0-100 */
  int64_t reset_ts;   /* epoch seconds, 0 if none */
  double used_value;  /* optional abs used (ZenMux USD / Volcengine) */
  double max_value;   /* optional abs max  (ZenMux USD / Volcengine) */
  bool have_abs;      /* true if used_value/max_value valid */
} quota_tier_t;

#define QUOTA_MAX_TIERS 6

/* Normalized result. kind distinguishes the two result families. */
typedef struct {
  bool valid;
  uint8_t kind; /* 0 = subscription quota (tiers), 1 = balance */
  bool auth_ok; /* false = credential rejected (401/403) */
  /* subscription quota fields */
  quota_tier_t tiers[QUOTA_MAX_TIERS];
  int tier_count;
  /* balance fields (absolute value, single entry) */
  double remaining;
  double total;
  double used;
  bool have_remaining;
  bool have_total;
  bool have_used;
  int percent_used;
  /* shared metadata */
  char unit[16];
  char plan[40]; /* plan tier / account status (e.g. "Coding Plan", "Pro") */
  char update_time[24]; /* "6/23 10:58" */
  char message[96];     /* human-readable status / error */
} codex_quota_data_t;

esp_err_t codex_quota_init(void);
esp_err_t codex_quota_get_config(codex_quota_config_t *out);
esp_err_t codex_quota_get_provider_config(codex_quota_config_t *out,
                                          uint8_t provider);
esp_err_t codex_quota_set_config(const codex_quota_config_t *cfg);
void codex_quota_get_data_copy(codex_quota_data_t *out);
esp_err_t codex_quota_show(void);
void codex_quota_set_auto_network_allowed(bool allowed);

/* Provider display name (for UI / logs). */
const char *codex_quota_provider_name(quota_provider_t p);
