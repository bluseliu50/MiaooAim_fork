#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     enabled;
    uint8_t  style;          /* 0 = digital 7-segment */
    bool     show_weather;   /* show weather summary below time */
} clock_config_t;

esp_err_t clock_display_init(void);
esp_err_t clock_display_get_config(clock_config_t *out);
esp_err_t clock_display_set_config(const clock_config_t *cfg);
esp_err_t clock_display_show(void);

/** 天气数据已更新且当前应由时钟展示时调用，尽快重绘时钟（嵌入天气） */
void clock_display_notify_weather_data(void);

/** 幻灯片等「谁占屏」配置变更时调用，唤醒时钟任务重新判断 */
void clock_display_notify_home_changed(void);

/** SNTP 校时成功后调用，尽快按新时间重绘（减小与 NTP 的显示滞后） */
void clock_display_notify_time_resync(void);
