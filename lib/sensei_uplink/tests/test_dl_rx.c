/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The receive side of a radio downlink: the chunks that the collector makes (the fragment vectors of sensei_link,
 * written by its Python implementation) are joined to the message. The 3-byte acknowledgements are dropped. The
 * decoding of the Downlink and the node's buffer size are tested in the node (firmware/tests/host/comm). */

#include <errno.h>
#include <string.h>

#include "check.h"
#include "generated/vectors.h"
#include "sensei_link/frag.h"
#include "sensei_uplink/dl_rx.h"

#define RX_CAP 337 /* the node: a Downlink at its generated maximum (299 B) and the largest envelope (38 B) */

static uint8_t rxbuf[4096];
static struct dl_rx rx;

/* Chunks of vec_frag[i]: in = msg_id, mtu (u16 LE), message. out = chunks, each with its length
 * (u16 LE) in front. */
static size_t vec_mtu(int i) { return (size_t)vec_frag[i].in[1] | ((size_t)vec_frag[i].in[2] << 8); }
static const uint8_t *vec_msg(int i) { return vec_frag[i].in + 3; }
static size_t vec_len(int i) { return vec_frag[i].in_len - 3; }

/* Feed every chunk of a vector. Returns the number of messages that came out; the last one is in *msg. */
static int feed_vector(int i, uint32_t now, const uint8_t **msg, size_t *len) {
  const uint8_t *p = vec_frag[i].out;
  const uint8_t *end = p + vec_frag[i].out_len;
  int n = 0;

  while (p < end) {
    size_t cl = (size_t)p[0] | ((size_t)p[1] << 8);

    if (dl_rx_feed(&rx, p + 2, cl, now, msg, len)) {
      n++;
    }
    p += 2 + cl;
  }
  return n;
}

/* Every vector that fits the buffer comes out as the message that went in; a message over the buffer is refused. */
static void test_vectors(void) {
  int fitted = 0;

  for (int i = 0; i < VEC_FRAG_COUNT; i++) {
    const uint8_t *msg = NULL;
    size_t len = 0;

    dl_rx_init(&rx, rxbuf, RX_CAP, vec_mtu(i), 1000);
    if (vec_len(i) > RX_CAP) {
      CHECK(feed_vector(i, 0, &msg, &len) == 0 && rx.n_bad >= 1);
      continue;
    }
    CHECK(feed_vector(i, 0, &msg, &len) == 1);
    CHECK(len == vec_len(i) && memcmp(msg, vec_msg(i), len) == 0);
    fitted++;
  }
  CHECK(fitted == 5); /* 5 of the 9 sensei_link vectors fit RX_CAP */
  /* a buffer that holds every vector */
  for (int i = 0; i < VEC_FRAG_COUNT; i++) {
    const uint8_t *msg = NULL;
    size_t len = 0;

    dl_rx_init(&rx, rxbuf, sizeof(rxbuf), vec_mtu(i), 1000);
    CHECK(feed_vector(i, 0, &msg, &len) == 1 && len == vec_len(i) && memcmp(msg, vec_msg(i), len) == 0);
  }
}

/* An acknowledgement is not a downlink: dropped, counted, and it does not disturb a message in
 * progress. */
static void test_acks(void) {
  uint8_t ack[3];
  uint8_t chunk[260];
  uint8_t msg_in[238];
  const uint8_t *msg = NULL;
  size_t len = 0, n;

  for (size_t i = 0; i < sizeof(msg_in); i++) {
    msg_in[i] = (uint8_t)(i * 5 + 1);
  }
  dl_rx_init(&rx, rxbuf, RX_CAP, 222, 1000);
  sensei_frag_ack(7, 0, ack);
  CHECK(!dl_rx_feed(&rx, ack, sizeof(ack), 0, &msg, &len) && rx.n_acks == 1 && rx.n_bad == 0);
  sensei_frag_ack(0, 0, ack);
  CHECK(!dl_rx_feed(&rx, ack, sizeof(ack), 0, &msg, &len) && rx.n_acks == 2);

  /* chunk 0, an ack in between, chunk 1 */
  n = sensei_frag_chunk(40, msg_in, sizeof(msg_in), 222, 0, chunk, sizeof(chunk));
  CHECK(n == 222 && !dl_rx_feed(&rx, chunk, n, 10, &msg, &len));
  sensei_frag_ack(40, 1, ack);
  CHECK(!dl_rx_feed(&rx, ack, sizeof(ack), 20, &msg, &len) && rx.n_acks == 3);
  n = sensei_frag_chunk(40, msg_in, sizeof(msg_in), 222, 1, chunk, sizeof(chunk));
  CHECK(dl_rx_feed(&rx, chunk, n, 30, &msg, &len));
  CHECK(len == sizeof(msg_in) && memcmp(msg, msg_in, len) == 0 && rx.n_message == 1);

  /* A message of one chunk of 3 bytes of data is no ack (its count is not 0). */
  {
    uint8_t one[6] = {9, 0, 1, 0x0A, 0x01, 0x02};

    CHECK(dl_rx_feed(&rx, one, sizeof(one), 40, &msg, &len) && len == 3);
  }
}

/* A message of exactly the buffer size comes out whole as one LTE chunk and as LoRa chunks; one byte more is refused.
 * The node checks that its buffer is the largest sealed Downlink. */
static void test_largest_message(void) {
  static uint8_t m[RX_CAP + 1];
  static uint8_t chunk[SENSEI_FRAG_HEADER_SIZE + RX_CAP + 1];
  const uint8_t *msg = NULL;
  size_t len = 0, n, count;
  for (size_t i = 0; i < sizeof(m); i++) {
    m[i] = (uint8_t)(i * 7 + 3);
  }
  static const size_t mtus[] = {222, 1024}; /* LoRa, LTE */

  for (size_t k = 0; k < 2; k++) {
    size_t mtu = mtus[k];

    dl_rx_init(&rx, rxbuf, RX_CAP, mtu, 1000);
    count = sensei_frag_count(RX_CAP, mtu);
    for (size_t i = 0; i < count; i++) {
      n = sensei_frag_chunk(60, m, RX_CAP, mtu, i, chunk, sizeof(chunk));
      CHECK(n > 0);
      CHECK(dl_rx_feed(&rx, chunk, n, 10 + (uint32_t)i, &msg, &len) == (i + 1 == count));
    }
    CHECK(len == RX_CAP && memcmp(msg, m, len) == 0 && rx.n_bad == 0);

    count = sensei_frag_count(RX_CAP + 1, mtu);
    for (size_t i = 0; i < count; i++) {
      n = sensei_frag_chunk(61, m, RX_CAP + 1, mtu, i, chunk, sizeof(chunk));
      CHECK(!dl_rx_feed(&rx, chunk, n, 100 + (uint32_t)i, &msg, &len));
    }
    CHECK(rx.n_bad >= 1);
  }
}

/* A copy of a finished message is not a second message (the collector sends the same answer again
 * when it sees a chunk twice); after the timeout the id is free. Bad datagrams are counted. */
static void test_duplicates_and_bad(void) {
  uint8_t m[40];
  uint8_t chunk[60];
  const uint8_t *msg;
  size_t len, n;

  memset(m, 0x5A, sizeof(m));
  dl_rx_init(&rx, rxbuf, RX_CAP, 1024, 5000);
  n = sensei_frag_chunk(3, m, sizeof(m), 1024, 0, chunk, sizeof(chunk));
  CHECK(dl_rx_feed(&rx, chunk, n, 100, &msg, &len) && len == sizeof(m));
  CHECK(!dl_rx_feed(&rx, chunk, n, 2000, &msg, &len) && rx.n_message == 1);       /* a copy */
  CHECK(dl_rx_feed(&rx, chunk, n, 2000 + 5001, &msg, &len) && rx.n_message == 2); /* later */

  CHECK(!dl_rx_feed(&rx, chunk, 2, 9000, &msg, &len) && rx.n_bad == 1); /* shorter than a header */
  chunk[2] = 3;
  chunk[1] = 3; /* index >= count */
  CHECK(!dl_rx_feed(&rx, chunk, n, 9000, &msg, &len) && rx.n_bad == 2);
  {
    uint8_t big[1024];

    memset(big, 1, sizeof(big));
    big[0] = 4;
    big[1] = 0;
    big[2] = 1;
    CHECK(!dl_rx_feed(&rx, big, 700, 9000, &msg, &len) && rx.n_bad == 3); /* over the buffer */
  }
}

int main(void) {
  test_vectors();
  test_acks();
  test_largest_message();
  test_duplicates_and_bad();
  DONE();
}
