/*
 * ESP-IDF-free host test driver for main/lunar.c.
 *
 * Strategy: this file pulls in the compatibility shims (sdkconfig.h + unity.h,
 * both local to this dir), then includes the EXISTING test cases from
 * test/lunar/main/test_lunar.c verbatim — so assertions stay single-sourced.
 * test_lunar.c defines app_main(); we rename it via macro and call it from a
 * real main().
 *
 * No ESP-IDF, no idf.py. Build with:
 *   make -C test/host run
 */

/* Shims must come first so test_lunar.c sees them instead of ESP-IDF headers.
 */
#include "sdkconfig.h"
#include "unity.h"

/* Rename app_main() in test_lunar.c so we can call it from our own main(). */
#define app_main lunar_test_app_main

/* Pull in all RUN_TEST(...) cases from the canonical test file. */
#include "../lunar/main/test_lunar.c"

int main(void) {
  lunar_test_app_main();
  /* UNITY_END() (called inside app_main) returns failure count;
   * app_main already exits on linux, but keep this for safety. */
  return 0;
}
