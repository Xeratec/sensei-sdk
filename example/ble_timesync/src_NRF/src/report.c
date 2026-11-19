/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <errno.h>

#include <zephyr/sys/byteorder.h>

#include "app.h"

void app_report_pack(const struct app_report *r, uint8_t buf[APP_REPORT_LEN]) {
  sys_put_le32(r->seq, &buf[0]);
  sys_put_le64(r->global_ns, &buf[4]);
  sys_put_le32(r->uncert_ns, &buf[12]);
  buf[16] = r->state;
}

int app_report_unpack(const uint8_t *buf, uint16_t len, struct app_report *r) {
  if (len != APP_REPORT_LEN) {
    return -EINVAL;
  }
  r->seq = sys_get_le32(&buf[0]);
  r->global_ns = sys_get_le64(&buf[4]);
  r->uncert_ns = sys_get_le32(&buf[12]);
  r->state = buf[16];
  return 0;
}

const char *app_state_str(enum sensei_ts_state state) {
  switch (state) {
  case SENSEI_TS_UNSYNCED:
    return "UNSYNCED";
  case SENSEI_TS_SYNCED:
    return "SYNCED";
  case SENSEI_TS_HOLDOVER:
    return "HOLDOVER";
  case SENSEI_TS_MASTER:
    return "MASTER";
  }
  return "?";
}
