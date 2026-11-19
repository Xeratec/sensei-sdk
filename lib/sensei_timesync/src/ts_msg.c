/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_timesync/ts_msg.h"

#include <errno.h>

static void put_le(uint8_t *p, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; i++) {
    p[i] = (uint8_t)(v >> (8 * i));
  }
}

static uint64_t get_le(const uint8_t *p, int bytes) {
  uint64_t v = 0;
  for (int i = 0; i < bytes; i++) {
    v |= (uint64_t)p[i] << (8 * i);
  }
  return v;
}

int ts_msg_encode(const struct ts_msg *m, uint8_t *buf, size_t len) {
  if (m == NULL || buf == NULL || len < TS_MSG_LEN || m->interval_us == 0) {
    return -EINVAL;
  }
  buf[0] = TS_MSG_VERSION;
  buf[1] = m->flags;
  put_le(&buf[2], m->counter, 2);
  put_le(&buf[4], m->epoch, 4);
  put_le(&buf[8], m->anchor_us, 8);
  put_le(&buf[16], m->interval_us, 4);
  return TS_MSG_LEN;
}

int ts_msg_decode(struct ts_msg *m, const uint8_t *buf, size_t len) {
  if (m == NULL || buf == NULL || len != TS_MSG_LEN) {
    return -EINVAL;
  }
  if (buf[0] != TS_MSG_VERSION) {
    return -ENOTSUP;
  }
  uint32_t interval = (uint32_t)get_le(&buf[16], 4);
  if (interval == 0) {
    return -EINVAL;
  }
  m->version = buf[0];
  m->flags = buf[1];
  m->counter = (uint16_t)get_le(&buf[2], 2);
  m->epoch = (uint32_t)get_le(&buf[4], 4);
  m->anchor_us = get_le(&buf[8], 8);
  m->interval_us = interval;
  return 0;
}
