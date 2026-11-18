/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Is a shield fitted (the start-up check) and the levels of the nRF21540 lines (lpwan_shield.h). */

#include <stdio.h>
#include <string.h>

#include "sensei_lpwan/lpwan_shield.h"

static int check_failures;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                                  \
      check_failures++;                                                                                                \
    }                                                                                                                  \
  } while (0)

static void test_probe(void) {
  /* the expander answers and its pins take the first writes: a shield */
  CHECK(lpwan_shield_probe(true, true) == LPWAN_SHIELD_PRESENT);
  /* the expander's driver did not come up (nothing at I2C 0x20): no shield, whatever the pins say */
  CHECK(lpwan_shield_probe(false, true) == LPWAN_SHIELD_ABSENT);
  CHECK(lpwan_shield_probe(false, false) == LPWAN_SHIELD_ABSENT);
  /* the driver is up but the pins fail: nothing to use either */
  CHECK(lpwan_shield_probe(true, false) == LPWAN_SHIELD_ABSENT);
  /* the probe never says "unknown" */
  for (int d = 0; d < 2; d++) {
    for (int p = 0; p < 2; p++) {
      CHECK(lpwan_shield_probe(d, p) != LPWAN_SHIELD_UNKNOWN);
    }
  }
}

static void test_names(void) {
  CHECK(strcmp(lpwan_shield_name(LPWAN_SHIELD_UNKNOWN), "unknown") == 0);
  CHECK(strcmp(lpwan_shield_name(LPWAN_SHIELD_PRESENT), "present") == 0);
  CHECK(strcmp(lpwan_shield_name(LPWAN_SHIELD_ABSENT), "absent") == 0);
}

static void test_fem_off(void) {
  /* extender not in use: both lines low, with the shield on or off, whatever MODE is configured to */
  for (int on = 0; on < 2; on++) {
    for (int mode = 0; mode < 2; mode++) {
      struct lpwan_fem_levels l = lpwan_fem_levels(false, on, mode);

      CHECK(!l.npd_high);
      CHECK(!l.mode_high);
    }
  }
}

static void test_fem_on(void) {
  struct lpwan_fem_levels l = lpwan_fem_levels(true, true, false);

  CHECK(l.npd_high && !l.mode_high);
  l = lpwan_fem_levels(true, true, true);
  CHECK(l.npd_high && l.mode_high);
  /* the shield's 3.3 V is off: power the extender down too */
  l = lpwan_fem_levels(true, false, true);
  CHECK(!l.npd_high);
}

int main(void) {
  test_probe();
  test_names();
  test_fem_off();
  test_fem_on();
  if (check_failures != 0) {
    printf("test_lpwan_shield: %d failed\n", check_failures);
    return 1;
  }
  printf("test_lpwan_shield: ok\n");
  return 0;
}
