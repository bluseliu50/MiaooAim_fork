/*
 * Unit tests for main/lunar.c (lunar calendar algorithm, 1900-2100).
 *
 * Pure host-side tests using the Unity framework on the ESP-IDF Linux target.
 * They exercise the public API only and do NOT modify any production logic.
 *
 * Reference anchors used below are well-documented public facts:
 *   - Chinese New Year (lunar 1/1) dates: 2000-02-05, 2023-01-22,
 *     2024-02-10, 2025-01-29, 2026-02-17, 2027-02-06
 *   - 2023 leap 2nd month (闰二月) begins 2023-03-22
 *   - Mid-Autumn 2024 (lunar 8/15) = 2024-09-17
 *   - Dragon Boat 2024 (lunar 5/5) = 2024-06-10
 *   - 干支/生肖 60/12-year cycles anchored at 1984 = 甲子年 (鼠)
 * Solar-term expectations use Beijing-time public almanac values.
 */

#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "unity.h"
#include "lunar.h"

#if !CONFIG_IDF_TARGET_LINUX
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

/* ── lunar_from_solar: Gregorian → lunar conversion ──────────────── */

static void check_solar_to_lunar(int gy, int gm, int gd,
                                 int ly, int lm, int ld, bool leap)
{
    lunar_date_t out;
    memset(&out, 0, sizeof(out));
    bool ok = lunar_from_solar(gy, gm, gd, &out);
    TEST_ASSERT_TRUE_MESSAGE(ok, "lunar_from_solar should succeed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ly, out.year, "lunar year mismatch");
    TEST_ASSERT_EQUAL_INT_MESSAGE(lm, out.month, "lunar month mismatch");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ld, out.day, "lunar day mismatch");
    TEST_ASSERT_EQUAL_MESSAGE(leap, out.is_leap, "leap flag mismatch");
}

void test_from_solar_base_date(void)
{
    /* 1900-01-31 is the table anchor = lunar 1900 正月初一. */
    check_solar_to_lunar(1900, 1, 31, 1900, 1, 1, false);
}

void test_from_solar_chinese_new_year(void)
{
    check_solar_to_lunar(2000, 2, 5,  2000, 1, 1, false);  /* CNY 2000 */
    check_solar_to_lunar(2023, 1, 22, 2023, 1, 1, false);  /* CNY 2023 */
    check_solar_to_lunar(2024, 2, 10, 2024, 1, 1, false);  /* CNY 2024 */
    check_solar_to_lunar(2025, 1, 29, 2025, 1, 1, false);  /* CNY 2025 */
    check_solar_to_lunar(2026, 2, 17, 2026, 1, 1, false);  /* CNY 2026 */
    check_solar_to_lunar(2027, 2, 6,  2027, 1, 1, false);  /* CNY 2027 */
}

void test_from_solar_leap_month_2023(void)
{
    /* 2023 has a leap 2nd month (29 days). 二月 has 30 days. */
    check_solar_to_lunar(2023, 3, 21, 2023, 2, 30, false); /* 二月三十 */
    check_solar_to_lunar(2023, 3, 22, 2023, 2, 1,  true);  /* 闰二月初一 */
}

void test_from_solar_festivals_dates(void)
{
    check_solar_to_lunar(2024, 9, 17, 2024, 8, 15, false); /* 中秋 */
    check_solar_to_lunar(2024, 6, 10, 2024, 5, 5,  false); /* 端午 */
}

void test_from_solar_recent_festival_dates(void)
{
    check_solar_to_lunar(2025, 1, 29, 2025, 1, 1, false);  /* 春节 */
    check_solar_to_lunar(2025, 5, 31, 2025, 5, 5, false);  /* 端午 */
    check_solar_to_lunar(2025, 10, 6, 2025, 8, 15, false); /* 中秋 */

    check_solar_to_lunar(2026, 2, 17, 2026, 1, 1, false);  /* 春节 */
    check_solar_to_lunar(2026, 6, 19, 2026, 5, 5, false);  /* 端午 */
    check_solar_to_lunar(2026, 9, 25, 2026, 8, 15, false); /* 中秋 */

    check_solar_to_lunar(2027, 2, 6, 2027, 1, 1, false);   /* 春节 */
    check_solar_to_lunar(2027, 6, 9, 2027, 5, 5, false);   /* 端午 */
    check_solar_to_lunar(2027, 9, 15, 2027, 8, 15, false); /* 中秋 */
}

void test_from_solar_out_of_range(void)
{
    lunar_date_t out;
    TEST_ASSERT_FALSE(lunar_from_solar(1899, 12, 31, &out)); /* before table */
    TEST_ASSERT_FALSE(lunar_from_solar(2101, 1, 1, &out));   /* after table */
    TEST_ASSERT_FALSE(lunar_from_solar(1900, 1, 30, &out));  /* before anchor */
}

/* ── lunar_day_str / lunar_month_str ─────────────────────────────── */

void test_day_str(void)
{
    TEST_ASSERT_EQUAL_STRING("初一", lunar_day_str(1));
    TEST_ASSERT_EQUAL_STRING("初十", lunar_day_str(10));
    TEST_ASSERT_EQUAL_STRING("十五", lunar_day_str(15));
    TEST_ASSERT_EQUAL_STRING("二十", lunar_day_str(20));
    TEST_ASSERT_EQUAL_STRING("廿一", lunar_day_str(21));
    TEST_ASSERT_EQUAL_STRING("三十", lunar_day_str(30));
    TEST_ASSERT_EQUAL_STRING("", lunar_day_str(0));   /* out of range */
    TEST_ASSERT_EQUAL_STRING("", lunar_day_str(31));  /* out of range */
}

void test_month_str(void)
{
    TEST_ASSERT_EQUAL_STRING("正月", lunar_month_str(1));
    TEST_ASSERT_EQUAL_STRING("冬月", lunar_month_str(11));
    TEST_ASSERT_EQUAL_STRING("腊月", lunar_month_str(12));
    TEST_ASSERT_EQUAL_STRING("", lunar_month_str(0));   /* out of range */
    TEST_ASSERT_EQUAL_STRING("", lunar_month_str(13));  /* out of range */
}

/* ── 干支 (sexagenary) + 生肖 (zodiac) ───────────────────────────── */

void test_year_ganzhi(void)
{
    TEST_ASSERT_EQUAL_STRING("甲子", lunar_year_gz(1984)); /* cycle origin */
    TEST_ASSERT_EQUAL_STRING("庚子", lunar_year_gz(2020));
    TEST_ASSERT_EQUAL_STRING("甲辰", lunar_year_gz(2024));
}

void test_year_zodiac(void)
{
    TEST_ASSERT_EQUAL_STRING("鼠", lunar_year_sx(1984));
    TEST_ASSERT_EQUAL_STRING("鼠", lunar_year_sx(2020));
    TEST_ASSERT_EQUAL_STRING("龙", lunar_year_sx(2024));
}

/* ── 24 solar terms ──────────────────────────────────────────────── */

void test_solar_terms(void)
{
    /* Stable Beijing-time lookup values for 2000-2099. */
    TEST_ASSERT_EQUAL_STRING("清明", lunar_solar_term(2024, 4, 4));
    TEST_ASSERT_EQUAL_STRING("冬至", lunar_solar_term(2024, 12, 21));
    TEST_ASSERT_NULL(lunar_solar_term(2024, 4, 15)); /* no term that day */
}

void test_solar_terms_2025_to_2027(void)
{
    TEST_ASSERT_EQUAL_STRING("小寒", lunar_solar_term(2025, 1, 5));
    TEST_ASSERT_EQUAL_STRING("清明", lunar_solar_term(2025, 4, 4));
    TEST_ASSERT_EQUAL_STRING("大暑", lunar_solar_term(2025, 7, 22));
    TEST_ASSERT_EQUAL_STRING("冬至", lunar_solar_term(2025, 12, 21));

    TEST_ASSERT_EQUAL_STRING("小寒", lunar_solar_term(2026, 1, 5));
    TEST_ASSERT_EQUAL_STRING("清明", lunar_solar_term(2026, 4, 5));
    TEST_ASSERT_EQUAL_STRING("大暑", lunar_solar_term(2026, 7, 23));
    TEST_ASSERT_EQUAL_STRING("冬至", lunar_solar_term(2026, 12, 22));

    TEST_ASSERT_EQUAL_STRING("小寒", lunar_solar_term(2027, 1, 5));
    TEST_ASSERT_EQUAL_STRING("清明", lunar_solar_term(2027, 4, 5));
    TEST_ASSERT_EQUAL_STRING("大暑", lunar_solar_term(2027, 7, 23));
    TEST_ASSERT_EQUAL_STRING("冬至", lunar_solar_term(2027, 12, 22));
}

void test_solar_terms_out_of_range(void)
{
    TEST_ASSERT_NULL(lunar_solar_term(1999, 4, 4)); /* before 2000 */
    TEST_ASSERT_NULL(lunar_solar_term(2100, 4, 4)); /* after 2099 */
}

/* ── festivals (Gregorian + lunar) ───────────────────────────────── */

void test_gregorian_festivals(void)
{
    TEST_ASSERT_EQUAL_STRING("元旦",  lunar_festival(2024, 1, 1, NULL));
    TEST_ASSERT_EQUAL_STRING("劳动节", lunar_festival(2024, 5, 1, NULL));
    TEST_ASSERT_EQUAL_STRING("国庆节", lunar_festival(2024, 10, 1, NULL));
    TEST_ASSERT_EQUAL_STRING("妇女节", lunar_festival(2024, 3, 8, NULL));
    TEST_ASSERT_EQUAL_STRING("儿童节", lunar_festival(2024, 6, 1, NULL));
}

void test_lunar_festivals(void)
{
    lunar_date_t ld;

    /* 春节 = lunar 1/1 → Gregorian 2024-02-10 */
    TEST_ASSERT_TRUE(lunar_from_solar(2024, 2, 10, &ld));
    TEST_ASSERT_EQUAL_STRING("春节", lunar_festival(2024, 2, 10, &ld));

    /* 中秋 = lunar 8/15 → Gregorian 2024-09-17 */
    TEST_ASSERT_TRUE(lunar_from_solar(2024, 9, 17, &ld));
    TEST_ASSERT_EQUAL_STRING("中秋", lunar_festival(2024, 9, 17, &ld));

    /* 端午 = lunar 5/5 → Gregorian 2024-06-10 */
    TEST_ASSERT_TRUE(lunar_from_solar(2024, 6, 10, &ld));
    TEST_ASSERT_EQUAL_STRING("端午", lunar_festival(2024, 6, 10, &ld));

    /* 2025 腊月 has 29 days; 除夕 must not be hard-coded to 腊月三十. */
    TEST_ASSERT_TRUE(lunar_from_solar(2025, 1, 28, &ld));
    TEST_ASSERT_EQUAL_STRING("除夕", lunar_festival(2025, 1, 28, &ld));
}

void test_recent_lunar_festivals(void)
{
    lunar_date_t ld;

    TEST_ASSERT_TRUE(lunar_from_solar(2025, 1, 29, &ld));
    TEST_ASSERT_EQUAL_STRING("春节", lunar_festival(2025, 1, 29, &ld));
    TEST_ASSERT_TRUE(lunar_from_solar(2025, 5, 31, &ld));
    TEST_ASSERT_EQUAL_STRING("端午", lunar_festival(2025, 5, 31, &ld));
    TEST_ASSERT_TRUE(lunar_from_solar(2025, 10, 6, &ld));
    TEST_ASSERT_EQUAL_STRING("中秋", lunar_festival(2025, 10, 6, &ld));

    TEST_ASSERT_TRUE(lunar_from_solar(2026, 2, 17, &ld));
    TEST_ASSERT_EQUAL_STRING("春节", lunar_festival(2026, 2, 17, &ld));
    TEST_ASSERT_TRUE(lunar_from_solar(2026, 6, 19, &ld));
    TEST_ASSERT_EQUAL_STRING("端午", lunar_festival(2026, 6, 19, &ld));
    TEST_ASSERT_TRUE(lunar_from_solar(2026, 9, 25, &ld));
    TEST_ASSERT_EQUAL_STRING("中秋", lunar_festival(2026, 9, 25, &ld));

    TEST_ASSERT_TRUE(lunar_from_solar(2027, 2, 6, &ld));
    TEST_ASSERT_EQUAL_STRING("春节", lunar_festival(2027, 2, 6, &ld));
    TEST_ASSERT_TRUE(lunar_from_solar(2027, 6, 9, &ld));
    TEST_ASSERT_EQUAL_STRING("端午", lunar_festival(2027, 6, 9, &ld));
    TEST_ASSERT_TRUE(lunar_from_solar(2027, 9, 15, &ld));
    TEST_ASSERT_EQUAL_STRING("中秋", lunar_festival(2027, 9, 15, &ld));

    TEST_ASSERT_TRUE(lunar_from_solar(2026, 2, 16, &ld));
    TEST_ASSERT_EQUAL_STRING("除夕", lunar_festival(2026, 2, 16, &ld));
    TEST_ASSERT_TRUE(lunar_from_solar(2027, 2, 5, &ld));
    TEST_ASSERT_EQUAL_STRING("除夕", lunar_festival(2027, 2, 5, &ld));
}

void test_no_festival(void)
{
    lunar_date_t ld;
    /* 2024-03-15 is neither a Gregorian nor a lunar festival. */
    TEST_ASSERT_TRUE(lunar_from_solar(2024, 3, 15, &ld));
    TEST_ASSERT_NULL(lunar_festival(2024, 3, 15, &ld));
}

/* ── Unity runner ────────────────────────────────────────────────── */

void app_main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_from_solar_base_date);
    RUN_TEST(test_from_solar_chinese_new_year);
    RUN_TEST(test_from_solar_leap_month_2023);
    RUN_TEST(test_from_solar_festivals_dates);
    RUN_TEST(test_from_solar_recent_festival_dates);
    RUN_TEST(test_from_solar_out_of_range);

    RUN_TEST(test_day_str);
    RUN_TEST(test_month_str);

    RUN_TEST(test_year_ganzhi);
    RUN_TEST(test_year_zodiac);

    RUN_TEST(test_solar_terms);
    RUN_TEST(test_solar_terms_2025_to_2027);
    RUN_TEST(test_solar_terms_out_of_range);

    RUN_TEST(test_gregorian_festivals);
    RUN_TEST(test_lunar_festivals);
    RUN_TEST(test_recent_lunar_festivals);
    RUN_TEST(test_no_festival);

    int failures = UNITY_END();

#if CONFIG_IDF_TARGET_LINUX
    /* On the Linux host target, exit with a CI-friendly status code. */
    exit(failures == 0 ? 0 : 1);
#else
    /* On a real device the results are printed over the serial monitor;
     * just stop here instead of returning into the startup code. */
    (void)failures;
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
#endif
}
