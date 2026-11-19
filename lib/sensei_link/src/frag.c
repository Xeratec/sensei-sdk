/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_link/frag.h"

#include <string.h>

size_t sensei_frag_count(size_t len, size_t mtu) {
  size_t data;
  size_t n;

  if (mtu <= SENSEI_FRAG_HEADER_SIZE) {
    return 0;
  }
  data = mtu - SENSEI_FRAG_HEADER_SIZE;
  n = len == 0 ? 1 : (len + data - 1) / data;
  return n > 255 ? 0 : n;
}

size_t sensei_frag_chunk(uint8_t msg_id, const uint8_t *msg, size_t len, size_t mtu, size_t index, uint8_t *out,
                         size_t cap) {
  size_t count = sensei_frag_count(len, mtu);
  size_t data = mtu - SENSEI_FRAG_HEADER_SIZE;
  size_t off;
  size_t n;

  if (count == 0 || index >= count) {
    return 0;
  }
  off = index * data;
  n = len - off < data ? len - off : data;
  if (cap < SENSEI_FRAG_HEADER_SIZE + n) {
    return 0;
  }
  out[0] = msg_id;
  out[1] = (uint8_t)index;
  out[2] = (uint8_t)count;
  if (n) {
    memcpy(out + SENSEI_FRAG_HEADER_SIZE, msg + off, n);
  }
  return SENSEI_FRAG_HEADER_SIZE + n;
}

void sensei_frag_ack(uint8_t msg_id, uint8_t index, uint8_t out[SENSEI_FRAG_ACK_SIZE]) {
  out[0] = msg_id;
  out[1] = index;
  out[2] = 0;
}

bool sensei_frag_is_ack(const uint8_t *data, size_t len, uint8_t *msg_id, uint8_t *index) {
  if (data == NULL || len != SENSEI_FRAG_ACK_SIZE || data[2] != 0) {
    return false;
  }
  if (msg_id != NULL) {
    *msg_id = data[0];
  }
  if (index != NULL) {
    *index = data[1];
  }
  return true;
}

void sensei_reasm_init(sensei_reasm_t *r, uint8_t *buf, size_t cap, size_t mtu, uint32_t timeout_ms) {
  memset(r, 0, sizeof(*r));
  r->buf = buf;
  r->cap = cap;
  r->chunk_data = mtu > SENSEI_FRAG_HEADER_SIZE ? mtu - SENSEI_FRAG_HEADER_SIZE : 0;
  r->timeout_ms = timeout_ms;
  r->dup_ms = timeout_ms;
}

void sensei_reasm_set_timeouts(sensei_reasm_t *r, uint32_t timeout_ms, uint32_t dup_ms) {
  r->timeout_ms = timeout_ms;
  r->dup_ms = dup_ms;
}

static void reasm_reset(sensei_reasm_t *r) {
  r->active = false;
  r->done = false;
  r->last_stashed = false;
  r->csize = 0;
  r->last_n = 0;
  r->received = 0;
  r->total_len = 0;
  memset(r->seen, 0, sizeof(r->seen));
}

static bool reasm_expired(const sensei_reasm_t *r, uint32_t now_ms) {
  return (uint32_t)(now_ms - r->last_rx_ms) > (r->done ? r->dup_ms : r->timeout_ms);
}

bool sensei_reasm_poll(sensei_reasm_t *r, uint32_t now_ms) {
  if (r->active && !r->done && reasm_expired(r, now_ms)) {
    r->n_timeout++;
    reasm_reset(r);
    return true;
  }
  return false;
}

sensei_reasm_status_t sensei_reasm_feed(sensei_reasm_t *r, const uint8_t *chunk, size_t len, uint32_t now_ms,
                                        const uint8_t **msg, size_t *msg_len) {
  uint8_t id;
  uint8_t index;
  uint8_t count;
  size_t n;
  size_t off;
  bool last;
  bool stash;

  if (len < SENSEI_FRAG_HEADER_SIZE || r->chunk_data == 0) {
    r->n_errors++;
    return SENSEI_REASM_ERR_FORMAT;
  }
  id = chunk[0];
  index = chunk[1];
  count = chunk[2];
  n = len - SENSEI_FRAG_HEADER_SIZE;
  if (count == 0 || index >= count || n > r->chunk_data || (index + 1u < count && n == 0)) {
    r->n_errors++;
    return SENSEI_REASM_ERR_FORMAT;
  }

  if (r->active && reasm_expired(r, now_ms)) {
    if (!r->done) {
      r->n_timeout++;
    }
    reasm_reset(r);
  }
  if (r->active && r->done && id == r->msg_id) {
    return SENSEI_REASM_DUPLICATE;
  }
  if (r->active && (id != r->msg_id || count != r->count || r->done)) {
    if (!r->done) {
      r->n_dropped++;
    }
    reasm_reset(r);
  }
  if (!r->active) {
    r->active = true;
    r->msg_id = id;
    r->count = count;
  }

  last = (index + 1u == count);
  if (r->seen[index >> 3] & (1u << (index & 7))) {
    r->last_rx_ms = now_ms;
    return SENSEI_REASM_DUPLICATE;
  }
  /* All chunks but the last have the size of the first one that comes; the last is not longer. */
  if ((!last && r->csize != 0 && n != r->csize) || (last && r->csize != 0 && n > r->csize)) {
    r->n_errors++;
    return SENSEI_REASM_ERR_FORMAT;
  }
  if (!last && r->csize == 0) { /* the chunk size of this message is known now */
    size_t last_off = (size_t)(count - 1u) * n;

    if (r->last_stashed && r->last_n > n) {
      r->n_errors++;
      reasm_reset(r);
      return SENSEI_REASM_ERR_FORMAT;
    }
    if (last_off + (r->last_stashed ? r->last_n : 0) > r->cap) {
      r->n_errors++;
      reasm_reset(r);
      return SENSEI_REASM_ERR_OVERFLOW;
    }
    r->csize = n;
    if (r->last_stashed) { /* the last chunk came first: from the top of the buffer to its place */
      memmove(r->buf + last_off, r->buf + r->cap - r->last_n, r->last_n);
      r->total_len = last_off + r->last_n;
      r->last_stashed = false;
    }
  }
  stash = last && count > 1 && r->csize == 0;
  if (stash) {
    off = r->cap - (n > r->cap ? r->cap : n);
  } else {
    off = last ? (size_t)(count - 1u) * r->csize : (size_t)index * r->csize;
  }
  if (n > r->cap || (!stash && off + n > r->cap)) {
    r->n_errors++;
    reasm_reset(r);
    return SENSEI_REASM_ERR_OVERFLOW;
  }
  r->last_rx_ms = now_ms;
  r->seen[index >> 3] |= (uint8_t)(1u << (index & 7));
  r->received++;
  if (n) {
    memcpy(r->buf + off, chunk + SENSEI_FRAG_HEADER_SIZE, n);
  }
  if (stash) {
    r->last_stashed = true;
    r->last_n = n;
  } else if (last) {
    r->total_len = off + n;
  }
  if (r->received < r->count) {
    return SENSEI_REASM_INCOMPLETE;
  }
  r->done = true;
  r->n_complete++;
  *msg = r->buf;
  *msg_len = r->total_len;
  return SENSEI_REASM_COMPLETE;
}
