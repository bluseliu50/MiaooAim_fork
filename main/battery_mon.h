#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "fb_render.h"

/**
 * 墨水屏硬件：BAT_DET = GPIO10，100k+100k 1:2 分压（与 otto power_manager 一致）。
 * 若原理图将 TP4054 CHRG 接到 MCU，可设置 BAT_MON_CHG_GPIO；未接则用 GPIO_NUM_NC。
 */
#ifndef BAT_MON_ADC_GPIO
#define BAT_MON_ADC_GPIO GPIO_NUM_10
#endif
#ifndef BAT_MON_CHG_GPIO
#define BAT_MON_CHG_GPIO GPIO_NUM_NC
#endif

typedef struct {
    uint8_t  percent;   /* 0–100，未初始化成功时为 0 */
    bool     charging;  /* 仅当 CHG 引脚有效时可信 */
    int      adc_raw;   /* 最近一次滑动平均 ADC 值或 0 */
    int      voltage_mv; /* 估算电池端电压，单位 mV；无效时为 0 */
    bool     valid;     /* ADC 已就绪且至少采样过一次 */
} battery_status_t;

esp_err_t battery_mon_init(void);
void      battery_mon_deinit(void);

/** 立即刷新一次采样（可选，默认 1s 定时器也会更新） */
void battery_mon_poll(void);

void battery_mon_get_status(battery_status_t *out);

/** 在帧缓冲上绘制一行电量文案（会先 poll）；例：「电量 85%」「充电 85%」 */
void battery_mon_draw_on_fb(fb_t *fb, int x, int y, fb_color_t color, int scale);

/** 仅绘制「85%」或「--%」，用于欢迎页标题栏等窄区域 */
void battery_mon_draw_percent_compact(fb_t *fb, int x, int y, fb_color_t color, int scale);
