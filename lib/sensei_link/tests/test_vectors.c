/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Checks the C implementation against the vectors that the Python one writes (generated/vectors.h, from
 * python -m sensei_sdk.vectors). */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "generated/vectors.h"
#include "sensei_link/cobs.h"
#include "sensei_link/crc.h"
#include "sensei_link/frag.h"
#include "sensei_link/spi.h"
#include "sensei_link/stream.h"

static void test_crc(void) {
  for (int i = 0; i < VEC_CRC16_COUNT; i++) {
    uint16_t c = sensei_crc16(SENSEI_CRC16_INIT, vec_crc16[i].in, vec_crc16[i].in_len);
    CHECK(vec_crc16[i].out[0] == (c & 0xFF) && vec_crc16[i].out[1] == (c >> 8));
  }
  for (int i = 0; i < VEC_CRC32_COUNT; i++) {
    uint32_t c = sensei_crc32(vec_crc32[i].in, vec_crc32[i].in_len);
    CHECK(memcmp(vec_crc32[i].out, &(uint8_t[4]){c, c >> 8, c >> 16, c >> 24}, 4) == 0);
  }
}

static void test_cobs(void) {
  static uint8_t out[4096];
  for (int i = 0; i < VEC_COBS_COUNT; i++) {
    size_t n = sensei_cobs_encode(vec_cobs[i].in, vec_cobs[i].in_len, out, sizeof(out));
    CHECK(n == vec_cobs[i].out_len && memcmp(out, vec_cobs[i].out, n) == 0);
  }
}

static void test_stream(void) {
  static uint8_t out[8192];
  static uint8_t rxbuf[8192];
  for (int i = 0; i < VEC_STREAM_COUNT; i++) {
    sensei_stream_rx_t rx;
    size_t n =
        sensei_stream_encode(vec_stream[i].in[0], vec_stream[i].in + 1, vec_stream[i].in_len - 1, out, sizeof(out));
    CHECK(n == vec_stream[i].out_len && memcmp(out, vec_stream[i].out, n) == 0);
    /* the Python frame decodes in C */
    sensei_stream_rx_init(&rx, rxbuf, sizeof(rxbuf));
    int r = 0;
    for (size_t k = 0; k < vec_stream[i].out_len; k++) {
      r = sensei_stream_rx_feed(&rx, vec_stream[i].out[k]);
    }
    CHECK(r == SENSEI_STREAM_FRAME && rx.channel == vec_stream[i].in[0] && rx.payload_len == vec_stream[i].in_len - 1 &&
          memcmp(rx.payload, vec_stream[i].in + 1, rx.payload_len) == 0);
  }
}

static void test_spi(void) {
  static uint8_t out[9000];
  for (int i = 0; i < VEC_SPI_COUNT; i++) {
    sensei_spi_header_t h;
    const uint8_t *p;
    size_t n = sensei_spi_frame_encode(vec_spi[i].in[0], vec_spi[i].in[1], vec_spi[i].in + 2, vec_spi[i].in_len - 2,
                                       out, sizeof(out));
    CHECK(n == vec_spi[i].out_len && memcmp(out, vec_spi[i].out, n) == 0);
    CHECK(sensei_spi_frame_decode(vec_spi[i].out, vec_spi[i].out_len, &h, &p) == SENSEI_SPI_OK);
    CHECK(h.type == vec_spi[i].in[0] && h.flags == vec_spi[i].in[1]);
  }
}

static void test_frag(void) {
  static uint8_t chunk[1100];
  static uint8_t rbuf[4096];
  for (int i = 0; i < VEC_FRAG_COUNT; i++) {
    uint8_t id = vec_frag[i].in[0];
    size_t mtu = (size_t)(vec_frag[i].in[1] | (vec_frag[i].in[2] << 8));
    const uint8_t *msg = vec_frag[i].in + 3;
    size_t len = vec_frag[i].in_len - 3;
    const uint8_t *exp = vec_frag[i].out;
    size_t count = sensei_frag_count(len, mtu);
    sensei_reasm_t r;
    const uint8_t *o = NULL;
    size_t ol = 0;
    sensei_reasm_init(&r, rbuf, sizeof(rbuf), mtu, 1000);
    for (size_t k = 0; k < count; k++) {
      size_t cn = sensei_frag_chunk(id, msg, len, mtu, k, chunk, sizeof(chunk));
      size_t en = (size_t)(exp[0] | (exp[1] << 8));
      CHECK(cn == en && memcmp(chunk, exp + 2, cn) == 0);
      CHECK(sensei_reasm_feed(&r, exp + 2, en, 0, &o, &ol) ==
            (k + 1 == count ? SENSEI_REASM_COMPLETE : SENSEI_REASM_INCOMPLETE));
      exp += 2 + en;
    }
    CHECK((size_t)(exp - vec_frag[i].out) == vec_frag[i].out_len);
    CHECK(ol == len && memcmp(o, msg, len) == 0);
  }
}

static void test_ack(void) {
  for (int i = 0; i < VEC_ACK_COUNT; i++) {
    uint8_t ack[SENSEI_FRAG_ACK_SIZE], id = 0, idx = 0;

    sensei_frag_ack(vec_ack[i].in[0], vec_ack[i].in[1], ack);
    CHECK(vec_ack[i].out_len == sizeof(ack) && memcmp(ack, vec_ack[i].out, sizeof(ack)) == 0);
    CHECK(sensei_frag_is_ack(vec_ack[i].out, vec_ack[i].out_len, &id, &idx) && id == vec_ack[i].in[0] &&
          idx == vec_ack[i].in[1]);
  }
}

int main(void) {
  test_crc();
  test_cobs();
  test_stream();
  test_spi();
  test_frag();
  test_ack();
  return CHECK_DONE();
}
