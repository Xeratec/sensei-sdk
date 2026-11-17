/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Global bus arbiter (bus_arb.h): the GAP9 link and the LPWAN client of the SDK claim through it. */

#include <zephyr/kernel.h>

#include "sensei_busarb/bus_arb.h"

static struct bus_arb arb;
static struct k_spinlock lock;
static bool inited;

static bus_listener_fn listener;
static void *listener_ctx;

void bus_set_listener(bus_listener_fn fn, void *ctx) {
  k_spinlock_key_t k = k_spin_lock(&lock);

  listener = fn;
  listener_ctx = ctx;
  k_spin_unlock(&lock, k);
}

/* Tell the listener about a change of BUS_GAP9 (outside the lock). */
static void notify(enum bus_user u, bool held) {
  bus_listener_fn fn = listener;

  if (u == BUS_GAP9 && fn != NULL) {
    fn(u, held, listener_ctx);
  }
}

bool bus_claim(enum bus_user u, uint32_t max_hold_ms) {
  k_spinlock_key_t k = k_spin_lock(&lock);
  int64_t now = k_uptime_get();
  bool ok, was;

  if (!inited) {
    bus_arb_init(&arb);
    inited = true;
  }
  /* An expired claim is not held: claiming again is a new claim and the listener hears of it (the level shifter
   * may have been switched on in between). */
  was = bus_arb_held(&arb, u, now);
  ok = bus_arb_try(&arb, u, now, max_hold_ms);
  k_spin_unlock(&lock, k);
  if (ok && !was) {
    notify(u, true);
  }
  return ok;
}

void bus_release(enum bus_user u) {
  k_spinlock_key_t k = k_spin_lock(&lock);
  bool was = arb.held[u];

  bus_arb_release(&arb, u);
  k_spin_unlock(&lock, k);
  if (was) {
    notify(u, false);
  }
}

bool bus_is_held(enum bus_user u) {
  k_spinlock_key_t k = k_spin_lock(&lock);
  bool h = bus_arb_held(&arb, u, k_uptime_get());

  k_spin_unlock(&lock, k);
  return h;
}

bool bus_claim_wait(enum bus_user u, uint32_t max_hold_ms, uint32_t wait_ms) {
  int64_t end = k_uptime_get() + wait_ms;

  while (!bus_claim(u, max_hold_ms)) {
    if (k_uptime_get() >= end) {
      return false;
    }
    k_msleep(20);
  }
  return true;
}
