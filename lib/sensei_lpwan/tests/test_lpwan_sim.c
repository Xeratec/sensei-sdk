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
  uint8_t out[LPWAN_SIM_HEADER + 4];
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

int main(void) {
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
