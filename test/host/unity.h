/*
 * Minimal Unity-compatible test header for ESP-IDF-free host builds.
 *
 * Provides the subset of macros used by test/lunar/main/test_lunar.c:
 *   UNITY_BEGIN / UNITY_END / RUN_TEST
 *   TEST_ASSERT_TRUE / FALSE / NULL
 *   TEST_ASSERT_TRUE_MESSAGE / EQUAL_MESSAGE / EQUAL_INT_MESSAGE / EQUAL_STRING
 *
 * This is NOT Espressif's full Unity framework — only what the lunar tests
 * need to compile and report pass/fail with a CI-friendly exit code.
 * It deliberately lives only in the host test dir and is never linked into
 * firmware.
 */
#pragma once

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static int g_unity_failures = 0;
static int g_unity_tests = 0;

#define UNITY_BEGIN()                                                          \
  do {                                                                         \
    g_unity_failures = 0;                                                      \
    g_unity_tests = 0;                                                         \
    printf("---------------------\n");                                         \
    printf("Unity host test run\n");                                           \
  } while (0)

/* RUN_TEST: invoke a void(void) test function, count result */
#define RUN_TEST(func)                                                         \
  do {                                                                         \
    g_unity_tests++;                                                           \
    int _fails_before = g_unity_failures;                                      \
    func();                                                                    \
    if (g_unity_failures == _fails_before) {                                   \
      printf("%-55s PASSED\n", #func);                                         \
    }                                                                          \
  } while (0)

#define UNITY_END()                                                            \
  (printf("---------------------\n%u Tests %u Failures 0 Ignored\n",           \
          (unsigned)g_unity_tests, (unsigned)g_unity_failures),                \
   g_unity_failures)

/* ---- Assertions ---- */

#define TEST_ASSERT_TRUE_MESSAGE(cond, msg)                                    \
  do {                                                                         \
    if (!(cond)) {                                                             \
      g_unity_failures++;                                                      \
      printf("%-55s FAILED: %s (%s:%d)\n", __func__, (msg), __FILE__,          \
             __LINE__);                                                        \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define TEST_ASSERT_FALSE_MESSAGE(cond, msg)                                   \
  TEST_ASSERT_TRUE_MESSAGE(!(cond), msg)

#define TEST_ASSERT_TRUE(cond) TEST_ASSERT_TRUE_MESSAGE((cond), #cond)

#define TEST_ASSERT_FALSE(cond) TEST_ASSERT_FALSE_MESSAGE((cond), #cond)

#define TEST_ASSERT_NULL_MESSAGE(ptr, msg)                                     \
  TEST_ASSERT_TRUE_MESSAGE((ptr) == NULL, msg)

#define TEST_ASSERT_EQUAL_MESSAGE(exp, act, msg)                               \
  do {                                                                         \
    if ((exp) != (act)) {                                                      \
      g_unity_failures++;                                                      \
      printf("%-55s FAILED: %s (expected %d, got %d) (%s:%d)\n", __func__,     \
             (msg), (int)(exp), (int)(act), __FILE__, __LINE__);               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define TEST_ASSERT_EQUAL_INT_MESSAGE(exp, act, msg)                           \
  TEST_ASSERT_EQUAL_MESSAGE((exp), (act), msg)

#define TEST_ASSERT_EQUAL_STRING_MESSAGE(exp, act, msg)                        \
  do {                                                                         \
    const char *_e = (exp);                                                    \
    const char *_a = (act);                                                    \
    if (_e == NULL || _a == NULL || strcmp(_e, _a) != 0) {                     \
      g_unity_failures++;                                                      \
      printf("%-55s FAILED: %s (expected \"%s\", got \"%s\") (%s:%d)\n",       \
             __func__, (msg), _e ? _e : "(null)", _a ? _a : "(null)",          \
             __FILE__, __LINE__);                                              \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define TEST_ASSERT_EQUAL_STRING(exp, act)                                     \
  TEST_ASSERT_EQUAL_STRING_MESSAGE((exp), (act), #exp " == " #act)

#define TEST_ASSERT_NULL(ptr) TEST_ASSERT_NULL_MESSAGE((ptr), #ptr " == NULL")

#ifdef __cplusplus
}
#endif
