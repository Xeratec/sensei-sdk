/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Bus arbiter: GAP9 against SPI exchange and LTE, time limits, renewal. */

#include <stdio.h>

#include "sensei_busarb/bus_arb.h"

static int fails, checks;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    checks++;                                                                                                          \
    if (!(c)) {                                                                                                        \
      fails++;                                                                                                         \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                                              \
    }                                                                                                                  \
  } while (0)

int main(void) {
  struct bus_arb a;

  bus_arb_init(&a);

  /* Free bus: anybody gets it. */
  CHECK(bus_arb_try(&a, BUS_GAP9, 0, 1000));
  /* One owner at a time: GAP9 holds, the others are refused. */
  CHECK(!bus_arb_try(&a, BUS_SPI, 10, 100));
  CHECK(!bus_arb_try(&a, BUS_LTE_TX, 10, 1000));
  CHECK(!bus_arb_try(&a, BUS_LTE_CONN, 10, 1000));
  /* Renewal by the owner. */
  CHECK(bus_arb_try(&a, BUS_GAP9, 900, 1000));
  CHECK(bus_arb_held(&a, BUS_GAP9, 1800));
  /* Released: the others follow. */
  bus_arb_release(&a, BUS_GAP9);
  CHECK(!bus_arb_held(&a, BUS_GAP9, 1000));
  CHECK(bus_arb_try(&a, BUS_SPI, 1000, 100));

  /* An SPI exchange and an LTE transmission together are fine; GAP9 waits for both. */
  CHECK(bus_arb_try(&a, BUS_LTE_TX, 1000, 60000));
  CHECK(!bus_arb_try(&a, BUS_GAP9, 1050, 1000));
  bus_arb_release(&a, BUS_SPI);
  CHECK(!bus_arb_try(&a, BUS_GAP9, 2000, 1000)); /* LTE still running */
  bus_arb_release(&a, BUS_LTE_TX);
  CHECK(bus_arb_try(&a, BUS_GAP9, 2000, 1000));

  /* Time limit: a holder that never releases cannot block for ever. */
  bus_arb_init(&a);
  CHECK(bus_arb_try(&a, BUS_LTE_CONN, 0, 5000));
  CHECK(!bus_arb_try(&a, BUS_GAP9, 4999, 1000));
  CHECK(bus_arb_try(&a, BUS_GAP9, 5000, 1000));
  /* ... and the same the other way round. */
  CHECK(!bus_arb_try(&a, BUS_SPI, 5500, 100));
  CHECK(bus_arb_try(&a, BUS_SPI, 6000, 100));

  /* Releasing a user that holds nothing is harmless. */
  bus_arb_init(&a);
  bus_arb_release(&a, BUS_LTE_TX);
  CHECK(bus_arb_try(&a, BUS_GAP9, 0, 10));

  printf("%d checks, %d failed\n", checks, fails);
  return fails != 0;
}
