/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_uplink/msg_queue.h"

#include <errno.h>
#include <string.h>

static void ring_read(const struct msg_queue *q, size_t off, uint8_t *out, size_t n) {
  size_t first = q->cap - off < n ? q->cap - off : n;

  memcpy(out, q->buf + off, first);
  if (first < n) {
    memcpy(out + first, q->buf, n - first);
  }
}

static void ring_write(struct msg_queue *q, const uint8_t *in, size_t n) {
  size_t first = q->cap - q->tail < n ? q->cap - q->tail : n;

  memcpy(q->buf + q->tail, in, first);
  if (first < n) {
    memcpy(q->buf, in + first, n - first);
  }
  q->tail = (q->tail + n) % q->cap;
}

void mq_init(struct msg_queue *q, uint8_t *buf, size_t cap) {
  memset(q, 0, sizeof(*q));
  q->buf = buf;
  q->cap = cap;
}

int mq_push_flags(struct msg_queue *q, const uint8_t *msg, size_t len, uint8_t flags) {
  uint8_t hdr[MQ_HDR];

  if (len == 0 || len > MQ_MAX_MSG) {
    return -EINVAL;
  }
  if (q->cap - q->used < len + MQ_HDR) {
    q->dropped++;
    return -ENOMEM;
  }
  hdr[0] = (uint8_t)(len & 0xff);
  hdr[1] = (uint8_t)((len >> 8) | ((flags & MQ_FLAG_PLAIN) ? 0x80u : 0u) | ((flags & MQ_FLAG_REPORT) ? 0x40u : 0u));
  ring_write(q, hdr, MQ_HDR);
  ring_write(q, msg, len);
  q->used += len + MQ_HDR;
  q->count++;
  return 0;
}

int mq_push_flag(struct msg_queue *q, const uint8_t *msg, size_t len, bool flag) {
  return mq_push_flags(q, msg, len, flag ? MQ_FLAG_PLAIN : 0);
}

int mq_push(struct msg_queue *q, const uint8_t *msg, size_t len) { return mq_push_flag(q, msg, len, false); }

size_t mq_free(const struct msg_queue *q) { return q->cap - q->used; }

size_t mq_peek_len(const struct msg_queue *q) {
  uint8_t hdr[MQ_HDR];

  if (q->count == 0) {
    return 0;
  }
  ring_read(q, q->head, hdr, MQ_HDR);
  return (size_t)hdr[0] | ((size_t)(hdr[1] & 0x3fu) << 8);
}

uint8_t mq_peek_flags(const struct msg_queue *q) {
  uint8_t hdr[MQ_HDR];

  if (q->count == 0) {
    return 0;
  }
  ring_read(q, q->head, hdr, MQ_HDR);
  return (uint8_t)(((hdr[1] & 0x80u) ? MQ_FLAG_PLAIN : 0u) | ((hdr[1] & 0x40u) ? MQ_FLAG_REPORT : 0u));
}

bool mq_peek_flag(const struct msg_queue *q) { return (mq_peek_flags(q) & MQ_FLAG_PLAIN) != 0; }

size_t mq_peek(const struct msg_queue *q, uint8_t *out, size_t cap) {
  size_t len = mq_peek_len(q);

  if (len == 0 || len > cap) {
    return 0;
  }
  ring_read(q, (q->head + MQ_HDR) % q->cap, out, len);
  return len;
}

void mq_pop(struct msg_queue *q) {
  size_t len = mq_peek_len(q);

  if (len == 0) {
    return;
  }
  q->head = (q->head + len + MQ_HDR) % q->cap;
  q->used -= len + MQ_HDR;
  q->count--;
}

void mq_clear(struct msg_queue *q) {
  q->head = q->tail = q->used = 0;
  q->count = 0;
}
