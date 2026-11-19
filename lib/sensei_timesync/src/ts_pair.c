/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_timesync/ts_pair.h"

#include <string.h>

/* Times in ns must fit int64: microsecond values above this are refused (about 292000 years). */
#define US_LIMIT ((int64_t)INT64_MAX / 1000)

void ts_pair_init(struct ts_pairer *p, uint16_t max_span_events) {
  memset(p, 0, sizeof(*p));
  if (max_span_events < 1) {
    max_span_events = 1;
  }
  if (max_span_events > 32767) {
    max_span_events = 32767;
  }
  p->max_span = max_span_events;
}

static void clear_local(struct ts_pairer *p) {
  p->head = 0;
  p->count = 0;
}

void ts_pair_invalidate(struct ts_pairer *p) {
  p->ref_valid = false;
  clear_local(p);
}

/* Master time (ns) of the connection event `counter`, paired with the local time. 0 on success, -1 if refused. */
static int project(const struct ts_pairer *p, uint16_t counter, uint64_t local_us, struct ts_point *out) {
  int32_t dist = (int16_t)(uint16_t)(counter - p->ref.counter);
  int32_t span = dist < 0 ? -dist : dist;
  if (span > p->max_span || local_us > (uint64_t)US_LIMIT || p->ref.anchor_us > (uint64_t)US_LIMIT) {
    return -1;
  }
  int64_t master_us = (int64_t)p->ref.anchor_us + (int64_t)dist * (int64_t)p->ref.interval_us;
  if (master_us > US_LIMIT || master_us < -US_LIMIT) {
    return -1;
  }
  out->local_ns = (int64_t)local_us * 1000;
  out->ref_ns = master_us * 1000;
  return 0;
}

int ts_pair_on_master(struct ts_pairer *p, const struct ts_msg *s, struct ts_point *out, int max_out) {
  if (s == NULL || s->interval_us == 0) {
    return 0;
  }
  if (p->ref_valid && p->ref.epoch != s->epoch) {
    clear_local(p);
  }
  p->ref = *s;
  p->ref_valid = true;

  int n = 0;
  for (uint8_t i = 0; i < p->count && n < max_out; i++) {
    const struct ts_pair_anchor *a =
        &p->local[(p->head + TS_PAIR_LOCAL_ANCHORS - p->count + i) % TS_PAIR_LOCAL_ANCHORS];
    if (project(p, a->counter, a->local_us, &out[n]) == 0) {
      n++;
    }
  }
  clear_local(p);
  return n;
}

int ts_pair_on_local(struct ts_pairer *p, uint16_t counter, uint64_t local_us, struct ts_point *out) {
  if (p->ref_valid && project(p, counter, local_us, out) == 0) {
    return 1;
  }
  p->local[p->head].counter = counter;
  p->local[p->head].local_us = local_us;
  p->head = (p->head + 1) % TS_PAIR_LOCAL_ANCHORS;
  if (p->count < TS_PAIR_LOCAL_ANCHORS) {
    p->count++;
  }
  return 0;
}
