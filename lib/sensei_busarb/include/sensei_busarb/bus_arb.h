/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file bus_arb.h
 * @brief One owner at a time for the shared SPI_A pins and the LTE current peaks.
 *
 * GAP9 (SPIS3) and the nRF9151 link (SPIM4) use the same pins, and the supply cannot carry a GAP9
 * job and an LTE transmission together. GAP9 conflicts with every other user; the other users
 * do not conflict with each other. Each claim has a time limit, so a user that never releases
 * (crashed thread, missed error path) cannot block the others for ever.
 *
 * bus_arb_* is the pure logic (time is an argument, no locking, tested on the host).
 * bus_claim() and friends (zephyr/bus_arb_zephyr.c) wrap one global instance with a lock and the uptime clock.
 */

#ifndef SENSEI_BUS_ARB_H_
#define SENSEI_BUS_ARB_H_

#include <stdbool.h>
#include <stdint.h>

enum bus_user {
  BUS_GAP9,     /* GAP9 powered or booting, SPI_A theirs */
  BUS_SPI,      /* one SPI exchange with the nRF9151 */
  BUS_LTE_CONN, /* LTE registration in progress */
  BUS_LTE_TX,   /* LTE transmission in progress */
  BUS_USER_COUNT
};

struct bus_arb {
  bool held[BUS_USER_COUNT];
  int64_t until_ms[BUS_USER_COUNT];
};

void bus_arb_init(struct bus_arb *a);

/**
 * Claim for max_hold_ms. Granted (true) if no conflicting user holds an unexpired claim. Claiming
 * again as the same user renews the limit.
 */
bool bus_arb_try(struct bus_arb *a, enum bus_user u, int64_t now_ms, uint32_t max_hold_ms);

void bus_arb_release(struct bus_arb *a, enum bus_user u);

/** True while u holds an unexpired claim. */
bool bus_arb_held(const struct bus_arb *a, enum bus_user u, int64_t now_ms);

/* ---- Global instance (zephyr/bus_arb_zephyr.c) ---- */

bool bus_claim(enum bus_user u, uint32_t max_hold_ms);
void bus_release(enum bus_user u);
bool bus_is_held(enum bus_user u);

/** Claim, retrying every 20 ms for up to wait_ms. Blocks: not for the app work queue. */
bool bus_claim_wait(enum bus_user u, uint32_t max_hold_ms, uint32_t wait_ms);

#endif /* SENSEI_BUS_ARB_H_ */
