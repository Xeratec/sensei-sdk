/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * Fragmentation of a message into radio chunks of at most `mtu` bytes.
 *
 *   chunk = msg_id(1) | index(1) | count(1) | data
 *
 * msg_id identifies the message (the sender increments it), index runs 0 ... count - 1.
 * All chunks except the last carry mtu - 3 data bytes. A message has 1 ... 255 chunks.
 *
 * Acknowledgement (LTE: the server answers the last chunk of a message with it when it has nothing else to send):
 *
 *   ack = msg_id(1) | index(1) | 0
 *
 * It has the form of a chunk with count 0, which a chunk never has, so a receiver tells it from a chunk by that
 * byte. msg_id and index are those of the chunk it answers (the last one of the message).
 */
#ifndef SENSEI_LINK_FRAG_H
#define SENSEI_LINK_FRAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSEI_FRAG_HEADER_SIZE 3u

/* Number of chunks for `len` bytes, or 0 if mtu is too small (< 4) or more than 255 chunks. */
size_t sensei_frag_count(size_t len, size_t mtu);

/* Build chunk `index` of a message into `out`. Returns its length (header included), or 0 on error.
 */
size_t sensei_frag_chunk(uint8_t msg_id, const uint8_t *msg, size_t len, size_t mtu, size_t index, uint8_t *out,
                         size_t cap);

#define SENSEI_FRAG_ACK_SIZE 3u

/* Build the acknowledgement of chunk `index` of message `msg_id` into `out` (3 bytes). */
void sensei_frag_ack(uint8_t msg_id, uint8_t index, uint8_t out[SENSEI_FRAG_ACK_SIZE]);

/* True if the datagram is an acknowledgement (3 bytes, count 0). msg_id and index are set if not NULL. */
bool sensei_frag_is_ack(const uint8_t *data, size_t len, uint8_t *msg_id, uint8_t *index);

typedef enum {
  SENSEI_REASM_INCOMPLETE = 0,
  SENSEI_REASM_COMPLETE = 1,    /* message available, valid until the next feed */
  SENSEI_REASM_DUPLICATE = 2,   /* chunk already received (or message already delivered) */
  SENSEI_REASM_ERR_FORMAT = -1, /* bad header or wrong chunk size */
  SENSEI_REASM_ERR_OVERFLOW = -2
} sensei_reasm_status_t;

/* Reassembly of one message at a time. A chunk of another message, or a chunk that arrives after
 * the timeout, drops the message in progress. */
typedef struct {
  uint8_t *buf; /* caller storage for the message */
  size_t cap;
  size_t chunk_data; /* data bytes of a full chunk (mtu - 3) */
  uint32_t timeout_ms;
  /* state */
  bool active;
  bool done;
  uint8_t msg_id;
  uint8_t count;
  uint8_t received;
  uint8_t seen[32];
  size_t total_len;
  uint32_t last_rx_ms;
  /* counters */
  uint32_t n_complete;
  uint32_t n_timeout;
  uint32_t n_dropped;
  uint32_t n_errors;
} sensei_reasm_t;

void sensei_reasm_init(sensei_reasm_t *r, uint8_t *buf, size_t cap, size_t mtu, uint32_t timeout_ms);

/* Feed one chunk received at `now_ms` (any monotonic millisecond clock, wraps at 2^32). On
 * COMPLETE,
 * `*msg` and `*msg_len` give the message. */
sensei_reasm_status_t sensei_reasm_feed(sensei_reasm_t *r, const uint8_t *chunk, size_t len, uint32_t now_ms,
                                        const uint8_t **msg, size_t *msg_len);

/* Drop a message in progress that timed out. Returns true if one was dropped. */
bool sensei_reasm_poll(sensei_reasm_t *r, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
