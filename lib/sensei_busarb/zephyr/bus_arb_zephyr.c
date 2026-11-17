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

bool bus_claim(enum bus_user u, uint32_t max_hold_ms) {
  k_spinlock_key_t k = k_spin_lock(&lock);
  bool ok;

  if (!inited) {
    bus_arb_init(&arb);
    inited = true;
  }
  ok = bus_arb_try(&arb, u, k_uptime_get(), max_hold_ms);
  k_spin_unlock(&lock, k);
  return ok;
}

void bus_release(enum bus_user u) {
  k_spinlock_key_t k = k_spin_lock(&lock);

  bus_arb_release(&arb, u);
  k_spin_unlock(&lock, k);
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
