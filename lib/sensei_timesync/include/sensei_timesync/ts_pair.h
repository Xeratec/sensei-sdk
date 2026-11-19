/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file ts_pair.h
 * @brief Follower side: turn anchor point reports into matched (local, master) points.
 *
 * Both ends of a BLE connection see the anchor point of each connection event: the master (central) at its
 * scheduled transmission, the follower (peripheral) when it receives the master's first packet. The Bluetooth
 * controller reports these instants on its own clock, so the same event counter on both sides is the same
 * physical instant (up to about 1 us of controller jitter and the radio propagation time, 3.3 ns per meter).
 *
 * The master sends samples (counter, anchor_us, interval_us), see ts_msg.h. The master schedules its anchor
 * points exactly interval_us apart on its own clock, so the master time of any other event c' is
 *     anchor_us + (c' - counter) * interval_us
 * with no drift error (the projection is done in the master's time domain). The follower pairs each of its own
 * anchor reports (c', local) with that projection. Projections farther than max_span events are refused (they
 * may cross a connection update). The event counter is 16 bit and wraps: distances are taken modulo 2^16.
 *
 * The follower may receive a sample after its own report of the same event (the sample travels in a later
 * event), so the last TS_PAIR_LOCAL_ANCHORS local reports are kept and paired when a sample arrives.
 *
 * Pure logic, tested on the host.
 */

#ifndef SENSEI_TS_PAIR_H_
#define SENSEI_TS_PAIR_H_

#include <stdbool.h>
#include <stdint.h>

#include "sensei_timesync/ts_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TS_PAIR_LOCAL_ANCHORS 8

/** A matched point: the same instant on the local and on the master clock (ns). */
struct ts_point {
  int64_t local_ns;
  int64_t ref_ns;
};

struct ts_pair_anchor {
  uint16_t counter;
  uint64_t local_us;
};

struct ts_pairer {
  bool ref_valid;
  struct ts_msg ref; /* last master sample */
  struct ts_pair_anchor local[TS_PAIR_LOCAL_ANCHORS];
  uint8_t head;  /* index of the next write */
  uint8_t count; /* reports stored */
  uint16_t max_span;
};

/** max_span_events: largest |c' - counter| that is projected (1 to 32767). */
void ts_pair_init(struct ts_pairer *p, uint16_t max_span_events);

/** Drop the master sample and the stored local reports (connection update, disconnection, new master). */
void ts_pair_invalidate(struct ts_pairer *p);

/**
 * A new master sample. It replaces the previous one. The stored local reports within max_span are paired with it
 * and written to out (oldest first, at most max_out), then dropped from the store.
 * A sample with another epoch than the previous one also drops the stored reports (they were paired with the old
 * master); the caller resets its model in that case (compare epochs before calling).
 * @return number of points written to out.
 */
int ts_pair_on_master(struct ts_pairer *p, const struct ts_msg *s, struct ts_point *out, int max_out);

/**
 * A local anchor report (event counter and anchor point on the local controller clock, us).
 * With a master sample within max_span: writes one point to out and returns 1. Otherwise the report is stored
 * for a later sample and 0 is returned.
 */
int ts_pair_on_local(struct ts_pairer *p, uint16_t counter, uint64_t local_us, struct ts_point *out);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_TS_PAIR_H_ */
