/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Host test of sensei_ota_reset_allowed(): which SMP reset requests the node accepts. */

#include <stdio.h>

#include "sensei_ota_reset.h"

static int failures;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                                                  \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

int main(void) {
  /* A normal reset (also over Bluetooth, without a cable) always goes through. */
  CHECK(sensei_ota_reset_allowed(0, false));
  CHECK(sensei_ota_reset_allowed(0, true));

  /* A reset into the MCUboot recovery (boot mode 1) from Bluetooth: no USB power, refused. Any other mode too. */
  CHECK(!sensei_ota_reset_allowed(1, false));
  CHECK(!sensei_ota_reset_allowed(2, false));
  CHECK(!sensei_ota_reset_allowed(255, false));

  /* With the cable plugged in the recovery may be asked for. */
  CHECK(sensei_ota_reset_allowed(1, true));

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("ota_reset: ok\n");
  return 0;
}
