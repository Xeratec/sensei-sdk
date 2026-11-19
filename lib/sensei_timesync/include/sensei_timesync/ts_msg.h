/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file ts_msg.h
 * @brief Wire format of the time samples the master sends to its followers.
 *
 * One sample: "the anchor point of connection event `counter` was at `anchor_us` on my controller clock, and
 * the connection interval is `interval_us`". 20 bytes, little endian, so it fits one ATT write with the default
 * MTU (23). Pure logic, tested on the host.
 *
 * | Offset | Size | Field |
 * |---|---|---|
 * | 0 | 1 | version (TS_MSG_VERSION) |
 * | 1 | 1 | flags (TS_MSG_F_*) |
 * | 2 | 2 | counter: connection event counter of the anchor point |
 * | 4 | 4 | epoch: random number the master draws at boot; a new value means a new master clock |
 * | 8 | 8 | anchor_us: anchor point on the master's controller clock (us) |
 * | 16 | 4 | interval_us: connection interval (us) |
 */

#ifndef SENSEI_TS_MSG_H_
#define SENSEI_TS_MSG_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TS_MSG_VERSION 1
#define TS_MSG_LEN 20

/** The master itself follows an upstream reference (multi-hop); informational. */
#define TS_MSG_F_MASTER_SYNCED 0x01

struct ts_msg {
  uint8_t version;
  uint8_t flags;
  uint16_t counter;
  uint32_t epoch;
  uint64_t anchor_us;
  uint32_t interval_us;
};

/**
 * Write m to buf (version is written as TS_MSG_VERSION whatever m->version holds).
 * @retval TS_MSG_LEN, or -EINVAL if len < TS_MSG_LEN or interval_us is 0.
 */
int ts_msg_encode(const struct ts_msg *m, uint8_t *buf, size_t len);

/**
 * Read buf into m.
 * @retval 0, -EINVAL if len != TS_MSG_LEN or interval_us is 0, -ENOTSUP if the version is not TS_MSG_VERSION.
 */
int ts_msg_decode(struct ts_msg *m, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_TS_MSG_H_ */
