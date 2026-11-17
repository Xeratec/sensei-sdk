/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Arbitration logic of the shared SPI_A bus and LTE peaks, see bus_arb.h. */

#include <string.h>

#include "sensei_busarb/bus_arb.h"

void bus_arb_init(struct bus_arb *a) { memset(a, 0, sizeof(*a)); }

bool bus_arb_held(const struct bus_arb *a, enum bus_user u, int64_t now_ms) {
  return a->held[u] && now_ms < a->until_ms[u];
}

/* GAP9 against everybody else; the others are compatible. */
static bool conflict(enum bus_user a, enum bus_user b) { return a != b && (a == BUS_GAP9 || b == BUS_GAP9); }

bool bus_arb_try(struct bus_arb *a, enum bus_user u, int64_t now_ms, uint32_t max_hold_ms) {
  for (int o = 0; o < BUS_USER_COUNT; o++) {
    if (conflict(u, (enum bus_user)o) && bus_arb_held(a, (enum bus_user)o, now_ms)) {
      return false;
    }
  }
  a->held[u] = true;
  a->until_ms[u] = now_ms + max_hold_ms;
  return true;
}

void bus_arb_release(struct bus_arb *a, enum bus_user u) { a->held[u] = false; }
