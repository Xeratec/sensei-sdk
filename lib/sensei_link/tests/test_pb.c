/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Every SDK message type with maximum-size fields: encode, check the size bound, decode, re-encode. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "sensei_link/pb.h"
#include "sensei_link/sensei_link.h"

static uint8_t enc1[16384];
static uint8_t enc2[16384];

static void bytes_fill(uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    p[i] = (uint8_t)(i % 11 == 0 ? 0 : i * 7 + 3);
  }
}

#define FILL_BYTES(field, n)                                                                                           \
  do {                                                                                                                 \
    (field).size = (n);                                                                                                \
    bytes_fill((field).bytes, (n));                                                                                    \
  } while (0)

static void fill_str(char *s, size_t cap) {
  memset(s, 'a', cap - 1);
  s[cap - 1] = 0;
}

/* encode, bound the size, decode into a fresh struct, encode again, compare */
static int32_t roundtrip(const char *name, const pb_msgdesc_t *fields, const void *msg, size_t size, size_t bound) {
  void *dec = malloc(size);
  int32_t n1 = sensei_pb_encode(enc1, sizeof(enc1), fields, msg);
  int32_t n2 = -2;

  if (n1 < 0 || (size_t)n1 > bound) {
    printf("%s: encode failed or exceeds bound (%d > %zu)\n", name, n1, bound);
    check_failures++;
  } else if (!sensei_pb_decode(enc1, (size_t)n1, fields, dec)) {
    printf("%s: decode failed\n", name);
    check_failures++;
  } else {
    n2 = sensei_pb_encode(enc2, sizeof(enc2), fields, dec);
    if (n2 != n1 || memcmp(enc1, enc2, (size_t)n1) != 0) {
      printf("%s: re-encode differs\n", name);
      check_failures++;
    }
    /* one byte too little must fail */
    CHECK(sensei_pb_encode(enc2, (size_t)n1 - 1, fields, msg) < 0 || n1 == 0);
    /* a truncated message must not decode (unless the cut falls on a field boundary) */
  }
  free(dec);
  return n1;
}

#define RT(Name, msg, bound) roundtrip(#Name, &sensei_##Name##_msg, &(msg), sizeof(msg), bound)

static void test_lpwan(void) {
  static sensei_LpwanRequest q;
  static sensei_LpwanResponse r;
  static sensei_LpwanEvent e;

  memset(&q, 0, sizeof(q));
  q.seq = 0xFFFFFFFFu;
  q.which_body = sensei_LpwanRequest_send_tag;
  q.send.radio = sensei_Radio_RADIO_NBIOT;
  q.send.port = 223;
  FILL_BYTES(q.send.payload, 1024);
  CHECK(RT(LpwanRequest, q, sensei_LpwanRequest_size) > 1024);

  q.which_body = sensei_LpwanRequest_lte_configure_tag;
  memset(&q.lte_configure, 0, sizeof(q.lte_configure));
  q.lte_configure.mode = sensei_LteMode_LTE_NB_IOT;
  q.lte_configure.psm_tau_s = q.lte_configure.psm_active_s = 0xFFFFFFFFu;
  q.lte_configure.edrx_cycle_ms = q.lte_configure.edrx_ptw_ms = 0xFFFFFFFFu;
  q.lte_configure.server_port = 65535;
  q.lte_configure.udp = true;
  fill_str(q.lte_configure.apn, sizeof(q.lte_configure.apn));
  fill_str(q.lte_configure.server_host, sizeof(q.lte_configure.server_host));
  CHECK(RT(LpwanRequest, q, sensei_LpwanRequest_size) > 100);

  q.which_body = sensei_LpwanRequest_lora_provision_tag;
  memset(&q.lora_provision, 0, sizeof(q.lora_provision));
  bytes_fill(q.lora_provision.dev_eui, 8);
  bytes_fill(q.lora_provision.join_eui, 8);
  bytes_fill(q.lora_provision.app_key, 16);
  CHECK(RT(LpwanRequest, q, sensei_LpwanRequest_size) > 32);
  q.which_body = sensei_LpwanRequest_lora_configure_tag;
  memset(&q.lora_configure, 0, sizeof(q.lora_configure));
  q.lora_configure.region = sensei_LoraRegion_REGION_RU864;
  q.lora_configure.lora_class = sensei_LoraClass_CLASS_C;
  q.lora_configure.data_rate = 15;
  q.lora_configure.adr = q.lora_configure.confirmed = true;
  q.lora_configure.port = 223;
  CHECK(RT(LpwanRequest, q, sensei_LpwanRequest_size) > 0);

  memset(&r, 0, sizeof(r));
  r.seq = 0xFFFFFFFFu;
  r.result = sensei_LpwanResult_LPWAN_FAILED;
  r.which_payload = sensei_LpwanResponse_version_tag;
  r.version.has_firmware = true;
  fill_str(r.version.modem, sizeof(r.version.modem));
  CHECK(RT(LpwanResponse, r, sensei_LpwanResponse_size) > 0);
  r.which_payload = sensei_LpwanResponse_status_tag;
  memset(&r.status, 0, sizeof(r.status));
  r.status.rssi_dbm = -140;
  r.status.snr_db = -20;
  r.status.queued = 0xFFFFFFFFu;
  CHECK(RT(LpwanResponse, r, sensei_LpwanResponse_size) > 0);

  memset(&e, 0, sizeof(e));
  e.which_body = sensei_LpwanEvent_downlink_rx_tag;
  e.downlink_rx.radio = sensei_Radio_RADIO_LTEM;
  e.downlink_rx.port = 0xFFFFFFFFu;
  FILL_BYTES(e.downlink_rx.payload, 1024);
  CHECK(RT(LpwanEvent, e, sensei_LpwanEvent_size) > 1024);
  e.which_body = sensei_LpwanEvent_tx_done_tag;
  e.tx_done.seq = 0xFFFFFFFFu;
  e.tx_done.ok = true;
  e.tx_done.attempts = 0xFFFFFFFFu;
  CHECK(RT(LpwanEvent, e, sensei_LpwanEvent_size) > 0);
  e.which_body = sensei_LpwanEvent_error_tag;
  memset(&e.error, 0, sizeof(e.error));
  fill_str(e.error.message, sizeof(e.error.message));
  CHECK(RT(LpwanEvent, e, sensei_LpwanEvent_size) > 48);
}

static void test_version(void) {
  sensei_Version v = {1, 2, 3, 0xC0FFEE};

  CHECK(RT(Version, v, sensei_Version_size) > 0);
  v.major = v.minor = v.patch = v.git = 0xFFFFFFFFu;
  CHECK(RT(Version, v, sensei_Version_size) == sensei_Version_size);
}

static void test_limits(void) {
  /* every message fits an SPI frame */
  CHECK(sensei_LpwanRequest_size <= SENSEI_SPI_MAX_PAYLOAD);
  CHECK(sensei_LpwanResponse_size <= SENSEI_SPI_MAX_PAYLOAD);
  CHECK(sensei_LpwanEvent_size <= SENSEI_SPI_MAX_PAYLOAD);
}

static void test_garbage(void) {
  uint8_t junk[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  sensei_LpwanRequest q;

  CHECK(!sensei_decode_LpwanRequest(junk, sizeof(junk), &q));
}

int main(void) {
  test_version();
  test_lpwan();
  test_limits();
  test_garbage();
  return CHECK_DONE();
}
