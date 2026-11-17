/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <string.h>

#include "check.h"
#include "sensei_link/cobs.h"
#include "sensei_link/crc.h"
#include "sensei_link/frag.h"
#include "sensei_link/spi.h"
#include "sensei_link/stream.h"

#define TEST_MAX_MSG 2144u /* a long message for the tests */
#define TEST_CH_A 0x01
#define TEST_CH_B 0x02
#define TEST_CH_C 0x03

static uint32_t rng_state = 12345;

static uint8_t rnd(void) {
  rng_state = rng_state * 1664525u + 1013904223u;
  return (uint8_t)(rng_state >> 24);
}

/* random bytes, zero with probability 1/zero_div (0 = never) */
static void fill(uint8_t *p, size_t n, unsigned zero_div) {
  for (size_t i = 0; i < n; i++) {
    p[i] = rnd();
    if (zero_div && p[i] % zero_div == 0) {
      p[i] = 0;
    } else if (!zero_div && p[i] == 0) {
      p[i] = 1;
    }
  }
}

static void test_crc(void) {
  CHECK(sensei_crc16(SENSEI_CRC16_INIT, (const uint8_t *)"123456789", 9) == 0x29B1);
  CHECK(sensei_crc16(SENSEI_CRC16_INIT, (const uint8_t *)"", 0) == 0xFFFF);
  CHECK(sensei_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
  /* incremental */
  uint16_t c = sensei_crc16(SENSEI_CRC16_INIT, (const uint8_t *)"1234", 4);
  c = sensei_crc16(c, (const uint8_t *)"56789", 5);
  CHECK(c == 0x29B1);
}

static void test_cobs(void) {
  static const uint8_t a[] = {0x00};
  static const uint8_t a_enc[] = {0x01, 0x01};
  static const uint8_t b[] = {0x11, 0x22, 0x00, 0x33};
  static const uint8_t b_enc[] = {0x03, 0x11, 0x22, 0x02, 0x33};
  uint8_t out[1024];
  uint8_t dec[1024];
  size_t n;

  n = sensei_cobs_encode(a, 1, out, sizeof(out));
  CHECK(n == 2 && memcmp(out, a_enc, 2) == 0);
  n = sensei_cobs_encode(b, 4, out, sizeof(out));
  CHECK(n == 5 && memcmp(out, b_enc, 5) == 0);
  n = sensei_cobs_encode(a, 0, out, sizeof(out));
  CHECK(n == 1 && out[0] == 1);

  /* round trips over all lengths around the 254 block size */
  for (size_t len = 0; len < 600; len++) {
    for (unsigned z = 0; z < 3; z++) {
      uint8_t in[600];
      size_t dn;
      fill(in, len, z == 0 ? 0 : (z == 1 ? 3 : 200));
      n = sensei_cobs_encode(in, len, out, sizeof(out));
      CHECK(n > 0 && n <= SENSEI_COBS_MAX_ENCODED(len));
      for (size_t i = 0; i < n; i++) {
        CHECK(out[i] != 0);
      }
      CHECK(sensei_cobs_decode(out, n, dec, sizeof(dec), &dn) && dn == len && memcmp(dec, in, len) == 0);
      /* in place */
      memcpy(dec, out, n);
      CHECK(sensei_cobs_decode(dec, n, dec, sizeof(dec), &dn) && dn == len && memcmp(dec, in, len) == 0);
    }
  }

  /* too small output buffers */
  CHECK(sensei_cobs_encode(b, 4, out, 4) == 0);
  CHECK(sensei_cobs_encode(b, 4, out, 0) == 0);
  /* malformed input */
  {
    static const uint8_t bad1[] = {0x05, 0x01}; /* block longer than data */
    static const uint8_t bad2[] = {0x02, 0x00}; /* zero in data */
    static const uint8_t bad3[] = {0x00, 0x01}; /* zero code */
    size_t dn;
    CHECK(!sensei_cobs_decode(bad1, 2, dec, sizeof(dec), &dn));
    CHECK(!sensei_cobs_decode(bad2, 2, dec, sizeof(dec), &dn));
    CHECK(!sensei_cobs_decode(bad3, 2, dec, sizeof(dec), &dn));
    CHECK(!sensei_cobs_decode(bad1, 0, dec, sizeof(dec), &dn));
    CHECK(!sensei_cobs_decode(b_enc, 5, dec, 2, &dn)); /* output too small */
  }
}

static int feed_all(sensei_stream_rx_t *rx, const uint8_t *d, size_t n, int *frames) {
  int last = 0;
  for (size_t i = 0; i < n; i++) {
    int r = sensei_stream_rx_feed(rx, d[i]);
    if (r == SENSEI_STREAM_FRAME) {
      (*frames)++;
    }
    if (r != SENSEI_STREAM_NONE) {
      last = r;
    }
  }
  return last;
}

static void test_stream(void) {
  static uint8_t frame[SENSEI_STREAM_FRAME_MAX(TEST_MAX_MSG)];
  static uint8_t rxbuf[SENSEI_STREAM_FRAME_MAX(TEST_MAX_MSG)];
  static uint8_t payload[TEST_MAX_MSG];
  sensei_stream_rx_t rx;
  size_t lens[] = {0, 1, 2, 100, 251, 252, 253, 254, 255, 1000, TEST_MAX_MSG};

  sensei_stream_rx_init(&rx, rxbuf, sizeof(rxbuf));
  for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
    size_t len = lens[k];
    int frames = 0;
    size_t fn;
    fill(payload, len, 17);
    fn = sensei_stream_encode(TEST_CH_B, payload, len, frame, sizeof(frame));
    CHECK(fn > 0 && fn <= SENSEI_STREAM_FRAME_MAX(len));
    CHECK(frame[fn - 1] == 0);
    for (size_t i = 0; i + 1 < fn; i++) {
      CHECK(frame[i] != 0);
    }
    CHECK(feed_all(&rx, frame, fn, &frames) == SENSEI_STREAM_FRAME && frames == 1);
    CHECK(rx.channel == TEST_CH_B && rx.payload_len == len && memcmp(rx.payload, payload, len) == 0);
    /* too small output */
    CHECK(sensei_stream_encode(1, payload, len, frame, fn - 1) == 0);
  }

  /* corrupted byte: CRC or format error, then the next frame is fine */
  {
    uint8_t good[64];
    uint8_t two[200];
    int frames = 0;
    size_t gn;
    fill(payload, 40, 5);
    gn = sensei_stream_encode(TEST_CH_C, payload, 40, good, sizeof(good));
    for (size_t pos = 0; pos + 1 < gn; pos++) {
      memcpy(two, good, gn);
      two[pos] ^= 0x40;
      if (two[pos] == 0) {
        two[pos] = 0x41;
      }
      memcpy(two + gn, good, gn);
      frames = 0;
      feed_all(&rx, two, 2 * gn, &frames);
      /* the damaged frame is dropped (or, if it is split by a flipped code, both lose sync
       * only until the delimiter), the second one always arrives */
      CHECK(frames >= 1);
      CHECK(rx.channel == TEST_CH_C && rx.payload_len == 40);
    }
    /* a single flipped payload bit must not pass */
    memcpy(two, good, gn);
    two[10] ^= 0x01;
    frames = 0;
    CHECK(feed_all(&rx, two, gn, &frames) < 0 && frames == 0);
  }

  /* garbage before a frame, empty frames, byte at a time */
  {
    uint8_t buf[300];
    int frames = 0;
    size_t fn;
    fill(payload, 100, 0);
    fn = sensei_stream_encode(TEST_CH_A, payload, 100, buf + 5, sizeof(buf) - 5);
    memset(buf, 0x55, 5);
    buf[5 - 1] = 0; /* delimiter ends the garbage */
    fn = sensei_stream_encode(TEST_CH_A, payload, 100, buf + 5, sizeof(buf) - 5);
    feed_all(&rx, buf, fn + 5, &frames);
    CHECK(frames == 1);
    CHECK(rx.n_crc_err + rx.n_format_err > 0);
  }

  /* overflow: frame longer than the receive buffer */
  {
    uint8_t small[16];
    sensei_stream_rx_t r2;
    int frames = 0;
    size_t fn;
    fill(payload, 100, 0);
    fn = sensei_stream_encode(1, payload, 100, frame, sizeof(frame));
    sensei_stream_rx_init(&r2, small, sizeof(small));
    CHECK(feed_all(&r2, frame, fn, &frames) == SENSEI_STREAM_ERR_OVERFLOW && frames == 0);
    fn = sensei_stream_encode(1, payload, 5, frame, sizeof(frame));
    CHECK(feed_all(&r2, frame, fn, &frames) == SENSEI_STREAM_FRAME && frames == 1);
  }
}

static void test_spi(void) {
  static uint8_t buf[SENSEI_SPI_HEADER_SIZE + SENSEI_SPI_MAX_PAYLOAD];
  static uint8_t payload[SENSEI_SPI_MAX_PAYLOAD];
  sensei_spi_header_t h;
  const uint8_t *p;
  size_t lens[] = {0, 1, 7, 1046, SENSEI_SPI_MAX_PAYLOAD};

  for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
    size_t n;
    fill(payload, lens[k], 9);
    n = sensei_spi_frame_encode(SENSEI_SPI_GAP9_JOB, SENSEI_SPI_FLAG_MORE, payload, lens[k], buf, sizeof(buf));
    CHECK(n == SENSEI_SPI_HEADER_SIZE + lens[k]);
    CHECK(buf[0] == 0x5E && buf[1] == SENSEI_LINK_VERSION && buf[2] == SENSEI_SPI_GAP9_JOB && buf[3] == 2);
    CHECK(sensei_spi_frame_decode(buf, n, &h, &p) == SENSEI_SPI_OK);
    CHECK(h.len == lens[k] && h.type == SENSEI_SPI_GAP9_JOB && h.flags == 2 && memcmp(p, payload, lens[k]) == 0);
    CHECK(sensei_spi_frame_decode(buf, n + 3, &h, &p) == SENSEI_SPI_OK); /* trailing bytes */
    if (lens[k] > 0) {
      CHECK(sensei_spi_frame_decode(buf, n - 1, &h, &p) == SENSEI_SPI_ERR_SHORT);
      buf[n - 1] ^= 1;
      CHECK(sensei_spi_frame_decode(buf, n, &h, &p) == SENSEI_SPI_ERR_CRC);
    }
  }
  CHECK(sensei_spi_frame_encode(1, 0, payload, SENSEI_SPI_MAX_PAYLOAD + 1, buf, sizeof(buf)) == 0);
  CHECK(sensei_spi_frame_encode(1, 0, payload, 10, buf, 17) == 0);
  {
    uint8_t hdr[8];
    sensei_spi_header_pack(hdr, 1, 0, payload, 0);
    hdr[0] = 0x5F;
    CHECK(sensei_spi_header_parse(hdr, &h) == SENSEI_SPI_ERR_MAGIC);
    hdr[0] = 0x5E;
    hdr[1] = 9;
    CHECK(sensei_spi_header_parse(hdr, &h) == SENSEI_SPI_ERR_VERSION);
    hdr[1] = SENSEI_LINK_VERSION;
    hdr[4] = 0xFF;
    hdr[5] = 0xFF;
    CHECK(sensei_spi_header_parse(hdr, &h) == SENSEI_SPI_ERR_LEN);
  }
}

/* The CRC covers header bytes 0 to 5: a flipped type, flags or length bit is caught, not only a
 * damaged payload. */
static void test_spi_header_crc(void) {
  static const uint8_t payload[3] = {'a', 'b', 'c'};
  uint8_t buf[SENSEI_SPI_HEADER_SIZE + 8];
  sensei_spi_header_t h;
  const uint8_t *p;
  size_t n = sensei_spi_frame_encode(SENSEI_SPI_LPWAN_REQUEST, 0x04, payload, sizeof(payload), buf, sizeof(buf));
  uint16_t crc = sensei_crc16(sensei_crc16(SENSEI_CRC16_INIT, buf, 6), payload, sizeof(payload));

  CHECK(n == SENSEI_SPI_HEADER_SIZE + 3);
  CHECK(buf[6] == (crc & 0xFF) && buf[7] == (crc >> 8));
  CHECK(sensei_spi_frame_decode(buf, n, &h, &p) == SENSEI_SPI_OK);
  for (size_t byte = 2; byte <= 3; byte++) { /* type, flags */
    for (unsigned bit = 0; bit < 8; bit++) {
      buf[byte] ^= (uint8_t)(1u << bit);
      CHECK(sensei_spi_frame_decode(buf, n, &h, &p) == SENSEI_SPI_ERR_CRC);
      buf[byte] ^= (uint8_t)(1u << bit);
    }
  }
  buf[4] ^= 0x01; /* length 3 -> 2: still inside the frame, header CRC fails */
  CHECK(sensei_spi_frame_decode(buf, n, &h, &p) == SENSEI_SPI_ERR_CRC);
  buf[4] ^= 0x01;
  buf[6] ^= 0x80; /* the CRC itself */
  CHECK(sensei_spi_frame_decode(buf, n, &h, &p) == SENSEI_SPI_ERR_CRC);
  buf[6] ^= 0x80;
  CHECK(sensei_spi_frame_decode(buf, n, &h, &p) == SENSEI_SPI_OK);
  /* check_payload on a parsed header agrees with the decode */
  CHECK(sensei_spi_header_parse(buf, &h) == SENSEI_SPI_OK && sensei_spi_check_payload(&h, buf + 8) == SENSEI_SPI_OK);
  h.flags ^= 0x08;
  CHECK(sensei_spi_check_payload(&h, buf + 8) == SENSEI_SPI_ERR_CRC);
}

static void test_frag(void) {
  static uint8_t msg[3000];
  static uint8_t chunk[1100];
  static uint8_t rbuf[3000];
  sensei_reasm_t r;
  const uint8_t *out = NULL;
  size_t out_len = 0;
  size_t mtus[] = {222, 244, 1024, 4, 10};
  size_t lens[] = {0, 1, 218, 219, 220, 221, 438, 439, 2086};

  CHECK(sensei_frag_count(0, 222) == 1);
  CHECK(sensei_frag_count(219, 222) == 1);
  CHECK(sensei_frag_count(220, 222) == 2);
  CHECK(sensei_frag_count(10, 3) == 0);
  CHECK(sensei_frag_count(255, 4) == 255);
  CHECK(sensei_frag_count(256, 4) == 0);
  CHECK(sensei_frag_count(255u * 219u, 222) == 255);
  CHECK(sensei_frag_count(255u * 219u + 1, 222) == 0);

  for (size_t m = 0; m < sizeof(mtus) / sizeof(mtus[0]); m++) {
    for (size_t l = 0; l < sizeof(lens) / sizeof(lens[0]); l++) {
      size_t count = sensei_frag_count(lens[l], mtus[m]);
      sensei_reasm_status_t st = SENSEI_REASM_INCOMPLETE;
      if (count == 0) {
        continue;
      }
      fill(msg, lens[l], 13);
      sensei_reasm_init(&r, rbuf, sizeof(rbuf), mtus[m], 1000);
      for (size_t i = 0; i < count; i++) {
        size_t cn = sensei_frag_chunk(7, msg, lens[l], mtus[m], i, chunk, sizeof(chunk));
        CHECK(cn >= SENSEI_FRAG_HEADER_SIZE && cn <= mtus[m]);
        CHECK(chunk[0] == 7 && chunk[1] == i && chunk[2] == count);
        st = sensei_reasm_feed(&r, chunk, cn, (uint32_t)(i * 10), &out, &out_len);
        CHECK(st == (i + 1 == count ? SENSEI_REASM_COMPLETE : SENSEI_REASM_INCOMPLETE));
      }
      CHECK(out_len == lens[l] && memcmp(out, msg, lens[l]) == 0);
    }
  }

  /* out of order, duplicates, reverse order */
  {
    size_t len = 1000;
    size_t count = sensei_frag_count(len, 222);
    sensei_reasm_status_t st;
    fill(msg, len, 13);
    sensei_reasm_init(&r, rbuf, sizeof(rbuf), 222, 1000);
    for (size_t i = count; i-- > 0;) {
      size_t cn = sensei_frag_chunk(1, msg, len, 222, i, chunk, sizeof(chunk));
      st = sensei_reasm_feed(&r, chunk, cn, (uint32_t)(10 * (count - i)), &out, &out_len);
      if (i > 0) {
        CHECK(st == SENSEI_REASM_INCOMPLETE);
        st = sensei_reasm_feed(&r, chunk, cn, (uint32_t)(10 * (count - i) + 1), &out, &out_len);
        CHECK(st == SENSEI_REASM_DUPLICATE);
      }
    }
    CHECK(st == SENSEI_REASM_COMPLETE && out_len == len && memcmp(out, msg, len) == 0);
    /* a late duplicate of the last chunk does not restart the message */
    CHECK(sensei_reasm_feed(&r, chunk, 222, 100, &out, &out_len) == SENSEI_REASM_DUPLICATE);
    CHECK(r.n_complete == 1);
  }

  /* timeout: chunk 0, wait, chunk 1 of the same message does not complete it */
  {
    size_t len = 400;
    size_t cn;
    fill(msg, len, 13);
    sensei_reasm_init(&r, rbuf, sizeof(rbuf), 222, 100);
    cn = sensei_frag_chunk(2, msg, len, 222, 0, chunk, sizeof(chunk));
    CHECK(sensei_reasm_feed(&r, chunk, cn, 1000, &out, &out_len) == SENSEI_REASM_INCOMPLETE);
    CHECK(!sensei_reasm_poll(&r, 1100));
    CHECK(sensei_reasm_poll(&r, 1101));
    CHECK(r.n_timeout == 1);
    CHECK(sensei_reasm_feed(&r, chunk, cn, 2000, &out, &out_len) == SENSEI_REASM_INCOMPLETE);
    cn = sensei_frag_chunk(2, msg, len, 222, 1, chunk, sizeof(chunk));
    CHECK(sensei_reasm_feed(&r, chunk, cn, 2050, &out, &out_len) == SENSEI_REASM_COMPLETE);
    /* timeout seen at feed time, clock wrap */
    cn = sensei_frag_chunk(3, msg, len, 222, 0, chunk, sizeof(chunk));
    CHECK(sensei_reasm_feed(&r, chunk, cn, 0xFFFFFFF0u, &out, &out_len) == SENSEI_REASM_INCOMPLETE);
    cn = sensei_frag_chunk(3, msg, len, 222, 1, chunk, sizeof(chunk));
    CHECK(sensei_reasm_feed(&r, chunk, cn, 20, &out, &out_len) == SENSEI_REASM_COMPLETE);
    cn = sensei_frag_chunk(4, msg, len, 222, 0, chunk, sizeof(chunk));
    sensei_reasm_feed(&r, chunk, cn, 0, &out, &out_len);
    cn = sensei_frag_chunk(4, msg, len, 222, 1, chunk, sizeof(chunk));
    CHECK(sensei_reasm_feed(&r, chunk, cn, 500, &out, &out_len) == SENSEI_REASM_INCOMPLETE);
    CHECK(r.n_timeout == 2);
  }

  /* a new message id drops the one in progress */
  {
    size_t len = 400;
    size_t cn;
    fill(msg, len, 13);
    sensei_reasm_init(&r, rbuf, sizeof(rbuf), 222, 1000);
    cn = sensei_frag_chunk(5, msg, len, 222, 0, chunk, sizeof(chunk));
    sensei_reasm_feed(&r, chunk, cn, 0, &out, &out_len);
    cn = sensei_frag_chunk(6, msg, len, 222, 0, chunk, sizeof(chunk));
    CHECK(sensei_reasm_feed(&r, chunk, cn, 1, &out, &out_len) == SENSEI_REASM_INCOMPLETE);
    CHECK(r.n_dropped == 1);
    cn = sensei_frag_chunk(6, msg, len, 222, 1, chunk, sizeof(chunk));
    CHECK(sensei_reasm_feed(&r, chunk, cn, 2, &out, &out_len) == SENSEI_REASM_COMPLETE);
  }

  /* malformed chunks and overflow */
  {
    uint8_t bad[10] = {1, 0, 0, 9, 9, 9};
    sensei_reasm_init(&r, rbuf, 100, 222, 1000);
    CHECK(sensei_reasm_feed(&r, bad, 2, 0, &out, &out_len) == SENSEI_REASM_ERR_FORMAT);
    CHECK(sensei_reasm_feed(&r, bad, 6, 0, &out, &out_len) == SENSEI_REASM_ERR_FORMAT); /* count 0 */
    bad[1] = 3;
    bad[2] = 3;
    CHECK(sensei_reasm_feed(&r, bad, 6, 0, &out, &out_len) == SENSEI_REASM_ERR_FORMAT); /* index >= count */
    bad[1] = 0;
    bad[2] = 2; /* not the last chunk but short */
    CHECK(sensei_reasm_feed(&r, bad, 6, 0, &out, &out_len) == SENSEI_REASM_ERR_FORMAT);
    fill(msg, 300, 0);
    sensei_frag_chunk(1, msg, 300, 222, 0, chunk, sizeof(chunk));
    CHECK(sensei_reasm_feed(&r, chunk, 222, 0, &out, &out_len) == SENSEI_REASM_ERR_OVERFLOW);
    CHECK(r.n_errors == 5);
  }

  /* encoder errors */
  CHECK(sensei_frag_chunk(1, msg, 100, 222, 1, chunk, sizeof(chunk)) == 0);
  CHECK(sensei_frag_chunk(1, msg, 100, 222, 0, chunk, 50) == 0);
  CHECK(sensei_frag_chunk(1, msg, 100, 3, 0, chunk, sizeof(chunk)) == 0);
}

int main(void) {
  test_crc();
  test_cobs();
  test_stream();
  test_spi();
  test_spi_header_crc();
  test_frag();
  return CHECK_DONE();
}
