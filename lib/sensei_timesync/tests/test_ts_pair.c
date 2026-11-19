/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Anchor pairing: projection of the master sample onto the follower's anchor reports. */

#include <string.h>

#include "sensei_timesync/ts_pair.h"
#include "test_util.h"

#define INTERVAL_US 100000

static struct ts_msg sample(uint16_t counter, uint64_t anchor_us, uint32_t epoch) {
  struct ts_msg s = {.version = TS_MSG_VERSION,
                     .counter = counter,
                     .epoch = epoch,
                     .anchor_us = anchor_us,
                     .interval_us = INTERVAL_US};
  return s;
}

static void test_exact(void) {
  struct ts_pairer p;
  struct ts_point pt;
  ts_pair_init(&p, 100);
  struct ts_msg s = sample(1000, 5000000, 1);
  CHECK(ts_pair_on_master(&p, &s, &pt, 1) == 0); /* nothing stored */

  /* same event: the master anchor itself */
  CHECK(ts_pair_on_local(&p, 1000, 7777777, &pt) == 1);
  CHECK(pt.local_ns == 7777777000ll && pt.ref_ns == 5000000000ll);
  /* later and earlier events (both directions) */
  CHECK(ts_pair_on_local(&p, 1010, 8777790, &pt) == 1);
  CHECK(pt.local_ns == 8777790000ll && pt.ref_ns == (5000000ll + 10 * INTERVAL_US) * 1000);
  CHECK(ts_pair_on_local(&p, 995, 7277700, &pt) == 1);
  CHECK(pt.local_ns == 7277700000ll && pt.ref_ns == (5000000ll - 5 * INTERVAL_US) * 1000);
}

static void test_wrap(void) {
  struct ts_pairer p;
  struct ts_point pt;
  ts_pair_init(&p, 100);
  struct ts_msg s = sample(65530, 1000000000, 1);
  ts_pair_on_master(&p, &s, &pt, 1);
  /* forward across 65535 -> 0 */
  CHECK(ts_pair_on_local(&p, 65535, 1, &pt) == 1 && pt.ref_ns == (1000000000ll + 5 * INTERVAL_US) * 1000);
  CHECK(ts_pair_on_local(&p, 0, 1, &pt) == 1 && pt.ref_ns == (1000000000ll + 6 * INTERVAL_US) * 1000);
  CHECK(ts_pair_on_local(&p, 4, 1, &pt) == 1 && pt.ref_ns == (1000000000ll + 10 * INTERVAL_US) * 1000);
  /* backward across the wrap: the sample is the newer one */
  s = sample(3, 1000000000, 1);
  ts_pair_on_master(&p, &s, &pt, 1);
  CHECK(ts_pair_on_local(&p, 65534, 1, &pt) == 1 && pt.ref_ns == (1000000000ll - 5 * INTERVAL_US) * 1000);
  CHECK(ts_pair_on_local(&p, 0, 1, &pt) == 1 && pt.ref_ns == (1000000000ll - 3 * INTERVAL_US) * 1000);
}

static void test_span(void) {
  struct ts_pairer p;
  struct ts_point pt;
  ts_pair_init(&p, 50);
  struct ts_msg s = sample(1000, 5000000, 1);
  ts_pair_on_master(&p, &s, &pt, 1);
  CHECK(ts_pair_on_local(&p, 1050, 1, &pt) == 1);
  CHECK(ts_pair_on_local(&p, 950, 1, &pt) == 1);
  CHECK(ts_pair_on_local(&p, 1051, 1, &pt) == 0); /* refused, stored */
  CHECK(ts_pair_on_local(&p, 949, 1, &pt) == 0);
  CHECK(ts_pair_on_local(&p, 1000 + 32768, 1, &pt) == 0); /* half the counter space away */
  CHECK(p.count == 3);

  /* max_span limits */
  ts_pair_init(&p, 0);
  CHECK(p.max_span == 1);
  ts_pair_init(&p, 65535);
  CHECK(p.max_span == 32767);
  ts_pair_on_master(&p, &s, &pt, 1);
  CHECK(ts_pair_on_local(&p, 1000 + 32767, 1, &pt) == 1);
  CHECK(ts_pair_on_local(&p, 1000 + 32768, 1, &pt) == 0);
  CHECK(ts_pair_on_local(&p, 1000 - 32767, 1, &pt) == 1);
}

static void test_sample_after_local(void) {
  struct ts_pairer p;
  struct ts_point pt, out[TS_PAIR_LOCAL_ANCHORS];
  ts_pair_init(&p, 100);
  /* three local reports before the master sample arrives */
  CHECK(ts_pair_on_local(&p, 10, 1000000, &pt) == 0);
  CHECK(ts_pair_on_local(&p, 11, 1100000, &pt) == 0);
  CHECK(ts_pair_on_local(&p, 12, 1200000, &pt) == 0);
  struct ts_msg s = sample(11, 9000000, 1);
  int n = ts_pair_on_master(&p, &s, out, TS_PAIR_LOCAL_ANCHORS);
  CHECK(n == 3);
  CHECK(out[0].local_ns == 1000000000ll && out[0].ref_ns == (9000000ll - INTERVAL_US) * 1000); /* oldest first */
  CHECK(out[1].local_ns == 1100000000ll && out[1].ref_ns == 9000000000ll);
  CHECK(out[2].local_ns == 1200000000ll && out[2].ref_ns == (9000000ll + INTERVAL_US) * 1000);
  CHECK(p.count == 0); /* dropped */
  s = sample(12, 9100000, 1);
  CHECK(ts_pair_on_master(&p, &s, out, TS_PAIR_LOCAL_ANCHORS) == 0);

  /* the store keeps the newest 8; max_out limits the output */
  ts_pair_invalidate(&p);
  for (int i = 0; i < 12; i++) {
    CHECK(ts_pair_on_local(&p, (uint16_t)(100 + i), 1000000ull * i, &pt) == 0);
  }
  CHECK(p.count == TS_PAIR_LOCAL_ANCHORS);
  s = sample(105, 50000000, 1);
  n = ts_pair_on_master(&p, &s, out, 2);
  CHECK(n == 2 && out[0].local_ns == 4000000000ll && out[1].local_ns == 5000000000ll);

  /* reports outside the span of the sample are not paired but dropped */
  ts_pair_init(&p, 5);
  ts_pair_on_local(&p, 100, 1, &pt);
  ts_pair_on_local(&p, 200, 2, &pt);
  s = sample(201, 1000, 1);
  n = ts_pair_on_master(&p, &s, out, TS_PAIR_LOCAL_ANCHORS);
  CHECK(n == 1 && out[0].local_ns == 2000);
  CHECK(p.count == 0);
}

static void test_epoch_and_invalidate(void) {
  struct ts_pairer p;
  struct ts_point pt, out[4];
  ts_pair_init(&p, 100);
  struct ts_msg s = sample(10, 1000000, 111);
  ts_pair_on_master(&p, &s, out, 4);
  /* a local report out of span is stored; a sample of the same epoch would pair it, another epoch must not */
  ts_pair_init(&p, 5);
  ts_pair_on_master(&p, &s, out, 4);
  CHECK(ts_pair_on_local(&p, 12, 1, &pt) == 1); /* paired with epoch 111 */
  CHECK(ts_pair_on_local(&p, 40, 1, &pt) == 0); /* stored */
  s = sample(41, 2000000, 222);                 /* new master clock */
  CHECK(ts_pair_on_master(&p, &s, out, 4) == 0);
  CHECK(p.count == 0);
  CHECK(ts_pair_on_local(&p, 42, 5, &pt) == 1 && pt.ref_ns == (2000000ll + INTERVAL_US) * 1000);

  /* same epoch pairs the stored report */
  ts_pair_init(&p, 100);
  s = sample(10, 1000000, 5);
  ts_pair_on_master(&p, &s, out, 4);
  CHECK(ts_pair_on_local(&p, 500, 1, &pt) == 0);
  s = sample(500, 2000000, 5);
  CHECK(ts_pair_on_master(&p, &s, out, 4) == 1);

  /* invalidate */
  CHECK(ts_pair_on_local(&p, 501, 1, &pt) == 1);
  ts_pair_invalidate(&p);
  CHECK(!p.ref_valid && p.count == 0);
  CHECK(ts_pair_on_local(&p, 501, 1, &pt) == 0); /* no master sample: stored */
  CHECK(p.count == 1);
}

static void test_limits(void) {
  struct ts_pairer p;
  struct ts_point pt;
  ts_pair_init(&p, 100);
  struct ts_msg s = sample(10, 5000000, 1);
  ts_pair_on_master(&p, &s, &pt, 1);
  /* large but valid: 1e12 us = 11.6 days */
  CHECK(ts_pair_on_local(&p, 10, 1000000000000ull, &pt) == 1 && pt.local_ns == 1000000000000000ll);
  /* local time that would overflow ns */
  CHECK(ts_pair_on_local(&p, 10, UINT64_MAX, &pt) == 0);
  /* huge interval: 32767 events of 71 minutes stay exact */
  s = sample(0, 0, 1);
  s.interval_us = 4000000000u;
  ts_pair_init(&p, 32767);
  ts_pair_on_master(&p, &s, &pt, 1);
  CHECK(ts_pair_on_local(&p, 32767, 1, &pt) == 1 && pt.ref_ns == 32767ll * 4000000000ll * 1000);
  CHECK(ts_pair_on_local(&p, 65535 - 32766, 1, &pt) == 1 && pt.ref_ns == -32767ll * 4000000000ll * 1000);
  /* zero interval sample is ignored */
  s.interval_us = 0;
  CHECK(ts_pair_on_master(&p, &s, &pt, 1) == 0 && p.ref.interval_us == 4000000000u);
}

int main(void) {
  test_exact();
  test_wrap();
  test_span();
  test_sample_after_local();
  test_epoch_and_invalidate();
  test_limits();
  printf("ts_pair: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
