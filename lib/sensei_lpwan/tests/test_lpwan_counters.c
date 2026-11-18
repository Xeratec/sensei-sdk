/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* LTE counters of the shield (RRC connected time, attaches, PSM entries) and how they travel in LpwanStatus. */

#include <stdio.h>
#include <string.h>

#include "sensei_link/pb.h"
#include "sensei_lpwan/lpwan_counters.h"

static int failures;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                                     \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static void test_rrc(void) {
  struct lpwan_counters c;

  memset(&c, 0, sizeof(c));
  CHECK(lpwan_cnt_rrc_ms(&c, 5000) == 0);
  /* a period of 3 s */
  lpwan_cnt_rrc(&c, true, 1000);
  CHECK(lpwan_cnt_rrc_ms(&c, 2500) == 1500); /* a running period counts */
  lpwan_cnt_rrc(&c, true, 2000);             /* a repeated event does not restart it */
  lpwan_cnt_rrc(&c, false, 4000);
  CHECK(lpwan_cnt_rrc_ms(&c, 9000) == 3000);
  lpwan_cnt_rrc(&c, false, 9500); /* an idle without a connected: nothing */
  CHECK(lpwan_cnt_rrc_ms(&c, 9999) == 3000);
  /* a second one adds */
  lpwan_cnt_rrc(&c, true, 10000);
  lpwan_cnt_rrc(&c, false, 10250);
  CHECK(lpwan_cnt_rrc_ms(&c, 20000) == 3250);
  /* the uptime wraps in the middle of a period */
  lpwan_cnt_rrc(&c, true, 0xFFFFFF00u);
  lpwan_cnt_rrc(&c, false, 0x00000100u);
  CHECK(lpwan_cnt_rrc_ms(&c, 0x1000u) == 3250 + 0x200);
}

static void test_reg(void) {
  struct lpwan_counters c;

  memset(&c, 0, sizeof(c));
  lpwan_cnt_reg(&c, false, 0); /* searching: no attach */
  CHECK(c.attaches == 0);
  lpwan_cnt_reg(&c, true, 100);
  lpwan_cnt_reg(&c, true, 200); /* home then roaming: the same registration */
  CHECK(c.attaches == 1);
  /* coverage lost during a connected period: the period ends there (no idle event comes) */
  lpwan_cnt_rrc(&c, true, 1000);
  lpwan_cnt_reg(&c, false, 3000);
  CHECK(lpwan_cnt_rrc_ms(&c, 99999) == 2000);
  lpwan_cnt_reg(&c, true, 5000);
  CHECK(c.attaches == 2);
  lpwan_cnt_psm(&c);
  lpwan_cnt_psm(&c);
  CHECK(c.psm_entries == 2);
}

/* The status goes over the SPI link as protobuf: the counters are set only by a shield that counts, and survive. */
static void test_status(void) {
  sensei_LpwanStatus st = sensei_LpwanStatus_init_zero;
  sensei_LpwanStatus out = sensei_LpwanStatus_init_zero;
  struct lpwan_counters c;
  uint8_t buf[128];
  int32_t n;

  memset(&c, 0, sizeof(c));
  lpwan_cnt_reg(&c, true, 0);
  lpwan_cnt_rrc(&c, true, 1000);
  lpwan_cnt_rrc(&c, false, 4200);
  lpwan_cnt_psm(&c);
  st.has_rrc_connected_ms = st.has_attaches = st.has_psm_entries = true;
  st.rrc_connected_ms = lpwan_cnt_rrc_ms(&c, 5000);
  st.attaches = c.attaches;
  st.psm_entries = c.psm_entries;
  n = sensei_pb_encode(buf, sizeof(buf), &sensei_LpwanStatus_msg, &st);
  CHECK(n > 0);
  CHECK(sensei_pb_decode(buf, (size_t)n, &sensei_LpwanStatus_msg, &out));
  CHECK(out.has_rrc_connected_ms && out.rrc_connected_ms == 3200);
  CHECK(out.has_attaches && out.attaches == 1);
  CHECK(out.has_psm_entries && out.psm_entries == 1);

  /* 0 is a value, absent is "not counted" (an older shield, or one without LTE) */
  memset(&st, 0, sizeof(st));
  n = sensei_pb_encode(buf, sizeof(buf), &sensei_LpwanStatus_msg, &st);
  CHECK(n >= 0);
  memset(&out, 0, sizeof(out));
  CHECK(sensei_pb_decode(buf, (size_t)n, &sensei_LpwanStatus_msg, &out));
  CHECK(!out.has_rrc_connected_ms && !out.has_attaches && !out.has_psm_entries);
  st.has_rrc_connected_ms = st.has_attaches = st.has_psm_entries = true;
  n = sensei_pb_encode(buf, sizeof(buf), &sensei_LpwanStatus_msg, &st);
  CHECK(sensei_pb_decode(buf, (size_t)n, &sensei_LpwanStatus_msg, &out));
  CHECK(out.has_rrc_connected_ms && out.rrc_connected_ms == 0 && out.has_attaches && out.has_psm_entries);

  /* an old shield's answer (fields 1 to 12 only) decodes with the counters absent */
  memset(&st, 0, sizeof(st));
  st.queued = 2;
  st.boot_count = 7;
  n = sensei_pb_encode(buf, sizeof(buf), &sensei_LpwanStatus_msg, &st);
  memset(&out, 0, sizeof(out));
  CHECK(sensei_pb_decode(buf, (size_t)n, &sensei_LpwanStatus_msg, &out));
  CHECK(out.boot_count == 7 && !out.has_attaches);

  /* the wire numbers are the ones of lpwan.proto (13, 14, 15: varints, tags 0x68, 0x70, 0x78) */
  memset(&st, 0, sizeof(st));
  st.has_rrc_connected_ms = st.has_attaches = st.has_psm_entries = true;
  st.rrc_connected_ms = 1;
  st.attaches = 2;
  st.psm_entries = 3;
  n = sensei_pb_encode(buf, sizeof(buf), &sensei_LpwanStatus_msg, &st);
  CHECK(n == 6);
  CHECK(buf[0] == 0x68 && buf[1] == 1 && buf[2] == 0x70 && buf[3] == 2 && buf[4] == 0x78 && buf[5] == 3);
}

int main(void) {
  test_rrc();
  test_reg();
  test_status();
  if (failures != 0) {
    printf("%d checks failed\n", failures);
    return 1;
  }
  printf("lpwan counters: all tests passed\n");
  return 0;
}
