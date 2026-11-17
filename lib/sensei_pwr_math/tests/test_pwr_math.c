/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <stdio.h>
#include <string.h>

#include "sensei_pwr_math.h"

static int fails;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                              \
      fails++;                                                                                                         \
    }                                                                                                                  \
  } while (0)

static void test_battery(void) {
  /* Li-ion table of the first firmware */
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LIION, 4300) == 100);
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LIION, 4200) == 100);
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LIION, 3900) == 60);
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LIION, 3800) == 50);
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LIION, 3300) == 0);
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LIION, 3000) == 0);
  /* Li-SOCl2: a fresh cell (3.65 V and more) is full; 4.2 V is not needed */
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LISOCL2, 3700) == 100);
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LISOCL2, 3600) == 80);
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LISOCL2, 3550) == 60);
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LISOCL2, 3300) == 10);
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LISOCL2, 3000) == 0);
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LISOCL2, 0) == 0);
  /* a fresh Li-SOCl2 cell reads well above 50 %; the Li-ion table would say 0 to 20 % at 3.6 V */
  CHECK(sensei_battery_perc(SENSEI_BATTERY_LISOCL2, 3600) > sensei_battery_perc(SENSEI_BATTERY_LIION, 3600));
  /* monotonic over the whole range */
  for (int c = 0; c < 2; c++) {
    uint8_t prev = 0;
    for (uint32_t mv = 0; mv <= 4500; mv++) {
      uint8_t p = sensei_battery_perc((enum sensei_battery_chem)c, mv);
      CHECK(p >= prev && p <= 100);
      prev = p;
    }
  }
}

static void test_button(void) {
  struct sensei_button b;

  memset(&b, 0, sizeof(b));
  /* a key-up without a key-down is ignored (this rebooted the node after a short press) */
  CHECK(sensei_button_up(&b, 50000, 2000) == SENSEI_BUTTON_NONE);
  CHECK(!b.pressed);

  /* short press */
  sensei_button_down(&b, 10000);
  CHECK(b.pressed);
  CHECK(sensei_button_up(&b, 10150, 2000) == SENSEI_BUTTON_SHORT);
  CHECK(!b.pressed);
  CHECK(sensei_button_up(&b, 10200, 2000) == SENSEI_BUTTON_NONE); /* bounce on release */

  /* long press: more than the limit */
  sensei_button_down(&b, 20000);
  CHECK(sensei_button_up(&b, 22000, 2000) == SENSEI_BUTTON_SHORT); /* exactly the limit is still short */
  sensei_button_down(&b, 20000);
  CHECK(sensei_button_up(&b, 22001, 2000) == SENSEI_BUTTON_LONG);

  /* a missed key-up: the next key-down replaces the old time, so a short press stays short (E8) */
  sensei_button_down(&b, 30000);
  sensei_button_down(&b, 90000);
  CHECK(sensei_button_up(&b, 90150, 2000) == SENSEI_BUTTON_SHORT);
  sensei_button_down(&b, 30000);
  sensei_button_down(&b, 31500);
  CHECK(sensei_button_up(&b, 33600, 2000) == SENSEI_BUTTON_LONG);

  /* a key-up that is older than the key-down (read late) is never long */
  sensei_button_down(&b, 50000);
  CHECK(sensei_button_up(&b, 49000, 2000) == SENSEI_BUTTON_SHORT);

  /* the millisecond counter wraps */
  sensei_button_down(&b, 0xFFFFFF00u);
  CHECK(sensei_button_up(&b, 0x00000100u, 2000) == SENSEI_BUTTON_SHORT); /* 512 ms */
  sensei_button_down(&b, 0xFFFFFC00u);
  CHECK(sensei_button_up(&b, 0x00000C00u, 2000) == SENSEI_BUTTON_LONG); /* 2 s + 1 s */
}

int main(void) {
  test_battery();
  test_button();
  if (fails) {
    printf("%d check(s) failed\n", fails);
    return 1;
  }
  printf("sensei_pwr_math: all checks passed\n");
  return 0;
}
