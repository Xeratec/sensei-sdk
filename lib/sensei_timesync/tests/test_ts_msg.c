/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Wire format of the time samples. */

#include <errno.h>
#include <string.h>

#include "sensei_timesync/ts_msg.h"
#include "test_util.h"

static void test_round_trip(void) {
  struct ts_msg m = {.version = 77,
                     .flags = TS_MSG_F_MASTER_SYNCED,
                     .counter = 0xBEEF,
                     .epoch = 0xDEADBEEF,
                     .anchor_us = 0x0102030405060708ull,
                     .interval_us = 100000};
  uint8_t buf[TS_MSG_LEN + 4];
  memset(buf, 0xAA, sizeof(buf));
  CHECK(ts_msg_encode(&m, buf, sizeof(buf)) == TS_MSG_LEN);
  CHECK(buf[TS_MSG_LEN] == 0xAA); /* nothing written past the message */

  /* explicit little-endian layout */
  CHECK(buf[0] == TS_MSG_VERSION && buf[1] == TS_MSG_F_MASTER_SYNCED);
  CHECK(buf[2] == 0xEF && buf[3] == 0xBE);
  CHECK(buf[4] == 0xEF && buf[5] == 0xBE && buf[6] == 0xAD && buf[7] == 0xDE);
  for (int i = 0; i < 8; i++) {
    CHECK(buf[8 + i] == 8 - i);
  }
  CHECK(buf[16] == 0xA0 && buf[17] == 0x86 && buf[18] == 0x01 && buf[19] == 0x00);

  struct ts_msg d;
  CHECK(ts_msg_decode(&d, buf, TS_MSG_LEN) == 0);
  CHECK(d.version == TS_MSG_VERSION && d.flags == m.flags && d.counter == m.counter && d.epoch == m.epoch);
  CHECK(d.anchor_us == m.anchor_us && d.interval_us == m.interval_us);

  /* extremes */
  m.counter = 0xFFFF;
  m.epoch = 0xFFFFFFFF;
  m.anchor_us = UINT64_MAX;
  m.interval_us = UINT32_MAX;
  CHECK(ts_msg_encode(&m, buf, TS_MSG_LEN) == TS_MSG_LEN);
  CHECK(ts_msg_decode(&d, buf, TS_MSG_LEN) == 0);
  CHECK(d.counter == 0xFFFF && d.epoch == 0xFFFFFFFF && d.anchor_us == UINT64_MAX && d.interval_us == UINT32_MAX);
}

static void test_bad_input(void) {
  struct ts_msg m = {.counter = 1, .epoch = 2, .anchor_us = 3, .interval_us = 4}, d;
  uint8_t buf[TS_MSG_LEN + 1];
  CHECK(ts_msg_encode(&m, buf, TS_MSG_LEN - 1) == -EINVAL); /* buffer too short */
  CHECK(ts_msg_encode(&m, buf, 0) == -EINVAL);
  m.interval_us = 0;
  CHECK(ts_msg_encode(&m, buf, TS_MSG_LEN) == -EINVAL);
  m.interval_us = 4;
  CHECK(ts_msg_encode(&m, buf, TS_MSG_LEN) == TS_MSG_LEN);

  CHECK(ts_msg_decode(&d, buf, TS_MSG_LEN - 1) == -EINVAL);
  CHECK(ts_msg_decode(&d, buf, TS_MSG_LEN + 1) == -EINVAL); /* exact length required */
  CHECK(ts_msg_decode(&d, buf, 0) == -EINVAL);

  buf[0] = TS_MSG_VERSION + 1;
  CHECK(ts_msg_decode(&d, buf, TS_MSG_LEN) == -ENOTSUP);
  buf[0] = 0;
  CHECK(ts_msg_decode(&d, buf, TS_MSG_LEN) == -ENOTSUP);

  buf[0] = TS_MSG_VERSION;
  memset(&buf[16], 0, 4); /* zero interval */
  memset(&d, 0x55, sizeof(d));
  CHECK(ts_msg_decode(&d, buf, TS_MSG_LEN) == -EINVAL);
  CHECK(d.interval_us == 0x55555555); /* output untouched on error */
}

int main(void) {
  test_round_trip();
  test_bad_input();
  printf("ts_msg: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
