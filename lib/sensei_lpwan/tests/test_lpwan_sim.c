/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Stream framing of the simulated shield link: frames split at any byte, several in one read, a broken length. */

#include <stdio.h>
#include <string.h>

#include "sensei_lpwan/lpwan_link.h"
#include "sensei_lpwan/lpwan_sim.h"

static int failures;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                                     \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static void test_round_trip(void) {
  uint8_t xfer[LPWAN_XFER_LEN], out[LPWAN_SIM_HEADER + LPWAN_XFER_LEN];
  struct lpwan_sim_rx rx = {0};
  const uint8_t *p;
  uint8_t type;
  size_t len;

  for (size_t i = 0; i < sizeof(xfer); i++) {
    xfer[i] = (uint8_t)(i * 7);
  }
  CHECK(lpwan_sim_pack(LPWAN_SIM_XFER, xfer, sizeof(xfer), out) == sizeof(out));
  /* one byte at a time: complete only with the last one */
  for (size_t i = 0; i < sizeof(out); i++) {
    CHECK(lpwan_sim_rx_next(&rx, &type, &p, &len) == 0);
    CHECK(lpwan_sim_rx_room(&rx) > 0);
    rx.buf[rx.n++] = out[i];
  }
  CHECK(lpwan_sim_rx_next(&rx, &type, &p, &len) == 1);
  CHECK(type == LPWAN_SIM_XFER && len == sizeof(xfer) && memcmp(p, xfer, len) == 0);
  lpwan_sim_rx_pop(&rx);
  CHECK(rx.n == 0 && lpwan_sim_rx_next(&rx, &type, &p, &len) == 0);
}

static void test_two_frames_in_one_read(void) {
  const uint8_t pins[2] = {1, 0};
  uint8_t buf[64];
  struct lpwan_sim_rx rx = {0};
  const uint8_t *p;
  uint8_t type;
  size_t len, n = 0;

  n += lpwan_sim_pack(LPWAN_SIM_PINS, pins, 2, buf + n);
  n += lpwan_sim_pack(LPWAN_SIM_PINS, NULL, 0, buf + n);
  n += lpwan_sim_pack(LPWAN_SIM_XFER_RSP, pins, 1, buf + n);
  memcpy(rx.buf, buf, n);
  rx.n = n + 1; /* the first byte of a next frame */
  rx.buf[n] = 0x01;
  CHECK(lpwan_sim_rx_next(&rx, &type, &p, &len) == 1 && type == LPWAN_SIM_PINS && len == 2 && p[0] == 1 && p[1] == 0);
  lpwan_sim_rx_pop(&rx);
  CHECK(lpwan_sim_rx_next(&rx, &type, &p, &len) == 1 && type == LPWAN_SIM_PINS && len == 0);
  lpwan_sim_rx_pop(&rx);
  CHECK(lpwan_sim_rx_next(&rx, &type, &p, &len) == 1 && type == LPWAN_SIM_XFER_RSP && len == 1);
  lpwan_sim_rx_pop(&rx);
  CHECK(rx.n == 1 && rx.buf[0] == 0x01); /* the partial frame stays */
  CHECK(lpwan_sim_rx_next(&rx, &type, &p, &len) == 0);
}

static void test_limits(void) {
  uint8_t out[LPWAN_SIM_HEADER + 4] = {0};
  struct lpwan_sim_rx rx = {0};
  const uint8_t *p;
  uint8_t type;
  size_t len;

  CHECK(lpwan_sim_pack(LPWAN_SIM_XFER, out, LPWAN_SIM_MAX_PAYLOAD + 1, out) == 0);
  CHECK(sizeof(rx.buf) >= LPWAN_SIM_HEADER + LPWAN_XFER_LEN); /* an SPI transfer fits */
  rx.buf[0] = LPWAN_SIM_XFER;
  rx.buf[1] = 0xFF;
  rx.buf[2] = 0xFF;
  rx.n = 3;
  CHECK(lpwan_sim_rx_next(&rx, &type, &p, &len) == -1); /* a length above the maximum: the stream is broken */
}

/* AES-CMAC with the test vectors of RFC 4493, and the join MIC (checked against another implementation). */
static void test_cmac_and_join_mic(void) {
  static const uint8_t key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                  0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
  static const uint8_t msg[40] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93,
                                  0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac,
                                  0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11};
  static const uint8_t want0[16] = {0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28,
                                    0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46};
  static const uint8_t want16[16] = {0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44,
                                     0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c};
  static const uint8_t want40[16] = {0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30,
                                     0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27};
  static const uint8_t want64[16] = {0x51, 0xf0, 0xbe, 0xbf, 0x7e, 0x3b, 0x9d, 0x92,
                                     0xfc, 0x49, 0x74, 0x17, 0x79, 0x36, 0x3c, 0xfe};
  static const uint8_t full[64] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73,
                                   0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7,
                                   0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4,
                                   0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef, 0xf6, 0x9f, 0x24, 0x45,
                                   0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
  uint8_t mac[16], mic[4], app_key[16], dev_eui[8] = {0x70, 0xb3, 0xd5, 0x7e, 0xd0, 0x00, 0x0a, 0x01};
  uint8_t join_eui[8] = {0, 0, 0, 0, 0, 0, 0, 1};

  lpwan_sim_cmac(key, msg, 0, mac);
  CHECK(memcmp(mac, want0, 16) == 0);
  lpwan_sim_cmac(key, msg, 16, mac);
  CHECK(memcmp(mac, want16, 16) == 0);
  lpwan_sim_cmac(key, msg, 40, mac);
  CHECK(memcmp(mac, want40, 16) == 0);
  lpwan_sim_cmac(key, full, 64, mac);
  CHECK(memcmp(mac, want64, 16) == 0);

  /* LoRaWAN join request: MHDR, JoinEUI, DevEUI, DevNonce (little endian); reference value from the cryptography
   * package */
  for (unsigned i = 0; i < 16; i++) {
    app_key[i] = (uint8_t)(0xA0 + i);
  }
  lpwan_sim_join_mic(app_key, join_eui, dev_eui, 0x1234, mic);
  CHECK(mic[0] == 0x6b && mic[1] == 0x75 && mic[2] == 0x12 && mic[3] == 0x6f);
  app_key[15] ^= 1; /* another key, another MIC */
  lpwan_sim_join_mic(app_key, join_eui, dev_eui, 0x1234, mic);
  CHECK(!(mic[0] == 0x6b && mic[1] == 0x75 && mic[2] == 0x12 && mic[3] == 0x6f));
}

int main(void) {
  test_cmac_and_join_mic();
  test_round_trip();
  test_two_frames_in_one_read();
  test_limits();
  if (failures) {
    printf("%d checks failed\n", failures);
    return 1;
  }
  printf("test_lpwan_sim: all checks passed\n");
  return 0;
}
