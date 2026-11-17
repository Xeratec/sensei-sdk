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

/* The Zephyr wrapper (bus_arb_zephyr.c) runs here against the stub in stub/zephyr/kernel.h. */
int64_t stub_uptime_ms;

static int listener_calls, listener_last_held;
static void listener(enum bus_user u, bool held, void *ctx) {
  (void)ctx;
  if (u == BUS_GAP9) {
    listener_calls++;
    listener_last_held = held;
  }
}

static int fails, checks;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    checks++;                                                                                                          \
    if (!(c)) {                                                                                                        \
      fails++;                                                                                                         \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                                              \
    }                                                                                                                  \
  } while (0)

/* D8: the listener hears of every GAP9 claim, also one made after an earlier claim ran out without a release. */
static void test_listener_after_expiry(void) {
  stub_uptime_ms = 1000;
  bus_set_listener(listener, NULL);

  CHECK(bus_claim(BUS_GAP9, 500));
  CHECK(listener_calls == 1 && listener_last_held == 1);
  CHECK(bus_claim(BUS_GAP9, 500)); /* renewal inside the limit: no change to report */
  CHECK(listener_calls == 1);

  stub_uptime_ms += 600; /* the claim ran out, GAP9 never released it */
  CHECK(!bus_is_held(BUS_GAP9));
  CHECK(bus_claim(BUS_GAP9, 500)); /* the client may have switched the level shifter on meanwhile: tell it */
  CHECK(listener_calls == 2 && listener_last_held == 1);

  bus_release(BUS_GAP9);
  CHECK(listener_calls == 3 && listener_last_held == 0);
  bus_release(BUS_GAP9); /* nothing held: nothing to report */
  CHECK(listener_calls == 3);

  /* other users never reach the listener */
  CHECK(bus_claim(BUS_SPI, 100));
  bus_release(BUS_SPI);
  CHECK(listener_calls == 3);
  bus_set_listener(NULL, NULL);
}

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

  test_listener_after_expiry();

  printf("%d checks, %d failed\n", checks, fails);
  return fails != 0;
}
