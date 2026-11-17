/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Zephyr wrapper of the bus arbiter (lib/sensei_busarb/zephyr) with the real kernel: uptime clock, expiry, listener,
 * bus_claim_wait() against another thread. The pure logic is tested on the host. */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "sensei_busarb/bus_arb.h"

/* Calls of the listener. */
static struct {
  int calls;
  enum bus_user user;
  bool held;
  void *ctx;
  bool held_inside; /* what bus_is_held(BUS_GAP9) said inside the callback */
} lis;

static void listener(enum bus_user u, bool held, void *ctx) {
  lis.calls++;
  lis.user = u;
  lis.held = held;
  lis.ctx = ctx;
  lis.held_inside = bus_is_held(BUS_GAP9); /* must not deadlock: the callback runs outside the lock */
}

static void before(void *f) {
  ARG_UNUSED(f);
  memset(&lis, 0, sizeof(lis));
}

/* The arbiter is global: leave it empty. */
static void after(void *f) {
  ARG_UNUSED(f);
  bus_set_listener(NULL, NULL);
  for (int u = 0; u < BUS_USER_COUNT; u++) {
    bus_release((enum bus_user)u);
  }
}

ZTEST(sensei_busarb, test_claim_release) {
  for (int u = 0; u < BUS_USER_COUNT; u++) {
    zassert_false(bus_is_held(u));
    zassert_true(bus_claim(u, 1000));
    zassert_true(bus_is_held(u));
    bus_release(u);
    zassert_false(bus_is_held(u));
  }
}

ZTEST(sensei_busarb, test_gap9_conflicts) {
  zassert_true(bus_claim(BUS_GAP9, 1000));
  zassert_false(bus_claim(BUS_SPI, 1000));
  zassert_false(bus_claim(BUS_LTE_CONN, 1000));
  zassert_false(bus_claim(BUS_LTE_TX, 1000));
  zassert_false(bus_is_held(BUS_SPI));
  bus_release(BUS_GAP9);

  /* the other users do not conflict with each other, but all conflict with GAP9 */
  zassert_true(bus_claim(BUS_SPI, 1000));
  zassert_true(bus_claim(BUS_LTE_CONN, 1000));
  zassert_true(bus_claim(BUS_LTE_TX, 1000));
  zassert_false(bus_claim(BUS_GAP9, 1000));
  bus_release(BUS_SPI);
  bus_release(BUS_LTE_CONN);
  zassert_false(bus_claim(BUS_GAP9, 1000)); /* LTE_TX is still held */
  bus_release(BUS_LTE_TX);
  zassert_true(bus_claim(BUS_GAP9, 1000));
}

ZTEST(sensei_busarb, test_claim_expires) {
  zassert_true(bus_claim(BUS_GAP9, 50));
  k_msleep(30);
  zassert_true(bus_is_held(BUS_GAP9));
  zassert_false(bus_claim(BUS_SPI, 1000));
  k_msleep(30);
  /* 60 ms: the claim is over, nobody released it */
  zassert_false(bus_is_held(BUS_GAP9));
  zassert_true(bus_claim(BUS_SPI, 1000));
}

ZTEST(sensei_busarb, test_claim_renewed) {
  zassert_true(bus_claim(BUS_SPI, 100));
  k_msleep(60);
  zassert_true(bus_claim(BUS_SPI, 100)); /* same user: new limit */
  k_msleep(60);
  zassert_true(bus_is_held(BUS_SPI)); /* 120 ms after the first claim */
  k_msleep(60);
  zassert_false(bus_is_held(BUS_SPI));
}

ZTEST(sensei_busarb, test_listener_gap9_only) {
  int arg;

  bus_set_listener(listener, &arg);
  zassert_true(bus_claim(BUS_SPI, 1000));
  bus_release(BUS_SPI);
  zassert_true(bus_claim(BUS_LTE_TX, 1000));
  bus_release(BUS_LTE_TX);
  zassert_equal(lis.calls, 0, "only BUS_GAP9 is reported");

  zassert_true(bus_claim(BUS_GAP9, 1000));
  zassert_equal(lis.calls, 1);
  zassert_equal(lis.user, BUS_GAP9);
  zassert_true(lis.held);
  zassert_equal(lis.ctx, &arg);
  zassert_true(lis.held_inside, "claim is visible in the callback");

  zassert_true(bus_claim(BUS_GAP9, 1000)); /* renewal, not a new claim */
  zassert_equal(lis.calls, 1);

  zassert_false(bus_claim(BUS_SPI, 1000)); /* refused: no report */
  zassert_equal(lis.calls, 1);

  bus_release(BUS_GAP9);
  zassert_equal(lis.calls, 2);
  zassert_false(lis.held);
  zassert_false(lis.held_inside);

  bus_release(BUS_GAP9); /* nothing held: no report */
  zassert_equal(lis.calls, 2);

  bus_set_listener(NULL, NULL);
  zassert_true(bus_claim(BUS_GAP9, 1000));
  zassert_equal(lis.calls, 2);
}

ZTEST(sensei_busarb, test_listener_after_expiry) {
  bus_set_listener(listener, NULL);
  zassert_true(bus_claim(BUS_GAP9, 30));
  zassert_equal(lis.calls, 1);
  k_msleep(40);
  /* expired: no report, but the next claim is a new one (the level shifter may have been switched in between) */
  zassert_equal(lis.calls, 1);
  zassert_true(bus_claim(BUS_GAP9, 1000));
  zassert_equal(lis.calls, 2);
  zassert_true(lis.held);
}

ZTEST(sensei_busarb, test_claim_wait_free) {
  int64_t t0 = k_uptime_get();

  zassert_true(bus_claim_wait(BUS_GAP9, 1000, 500));
  zassert_true(k_uptime_get() - t0 < 20, "no wait when free");
}

ZTEST(sensei_busarb, test_claim_wait_timeout) {
  int64_t t0;
  int64_t dt;

  zassert_true(bus_claim(BUS_GAP9, 10000));
  t0 = k_uptime_get();
  zassert_false(bus_claim_wait(BUS_SPI, 1000, 100));
  dt = k_uptime_get() - t0;
  zassert_true(dt >= 100 && dt <= 160, "waited %d ms", (int)dt);
  zassert_false(bus_is_held(BUS_SPI));
}

ZTEST(sensei_busarb, test_claim_wait_zero) {
  zassert_true(bus_claim(BUS_GAP9, 10000));
  zassert_false(bus_claim_wait(BUS_SPI, 1000, 0));
}

/* Another thread releases GAP9 after 100 ms; the waiter gets the bus right after. */
static K_THREAD_STACK_DEFINE(rel_stack, 1024);
static struct k_thread rel_thread;

static void releaser(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  k_msleep(100);
  bus_release(BUS_GAP9);
}

ZTEST(sensei_busarb, test_claim_wait_other_thread) {
  int64_t t0;
  int64_t dt;

  zassert_true(bus_claim(BUS_GAP9, 10000));
  k_thread_create(&rel_thread, rel_stack, K_THREAD_STACK_SIZEOF(rel_stack), releaser, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
  t0 = k_uptime_get();
  zassert_true(bus_claim_wait(BUS_SPI, 1000, 1000));
  dt = k_uptime_get() - t0;
  zassert_true(dt >= 100 && dt <= 140, "got the bus after %d ms", (int)dt);
  zassert_true(bus_is_held(BUS_SPI));
  zassert_equal(k_thread_join(&rel_thread, K_SECONDS(1)), 0);
}

/* A claim that nobody renews ends by itself: a waiter gets the bus when the limit is over. */
ZTEST(sensei_busarb, test_claim_wait_until_expiry) {
  zassert_true(bus_claim(BUS_GAP9, 80));
  zassert_true(bus_claim_wait(BUS_LTE_TX, 1000, 500));
}

ZTEST_SUITE(sensei_busarb, NULL, NULL, before, after, NULL);
