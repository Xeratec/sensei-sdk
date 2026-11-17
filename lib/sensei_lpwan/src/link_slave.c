/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * Adapted from the LPWAN shield commissioning project (ETH Zurich, 2026).
 */

#include "sensei_lpwan/link_slave.h"

#include <errno.h>
#include <string.h>

#include "sensei_link/pb.h"

#define ARMED_IDLE 0
#define ARMED_RESPONSE 1
#define ARMED_EVENT 2

static void lock(struct link_slave *s) {
  if (s->ops->lock) {
    s->ops->lock(s->ctx);
  }
}

static void unlock(struct link_slave *s) {
  if (s->ops->unlock) {
    s->ops->unlock(s->ctx);
  }
}

void link_slave_init(struct link_slave *s, const struct link_slave_ops *ops, void *ctx,
                     link_slave_request_fn on_request, void *user) {
  memset(s, 0, sizeof(*s));
  s->ops = ops;
  s->ctx = ctx;
  s->on_request = on_request;
  s->user = user;
}

static void load_idle(struct link_slave *s) {
  uint8_t flags = s->master_error ? SENSEI_SPI_FLAG_ERROR : 0;

  memset(s->tx, 0, sizeof(s->tx));
  sensei_spi_header_pack(s->tx, LPWAN_FRAME_IDLE, flags, s->tx + SENSEI_SPI_HEADER_SIZE, 0);
}

/* Fill s->tx with the frame to send in the next transfer and arm it. */
static int arm_next(struct link_slave *s) {
  const struct link_slave_frame *f = NULL;
  uint8_t flags = s->master_error ? SENSEI_SPI_FLAG_ERROR : 0;
  int err;

  lock(s);
  if (s->armed == ARMED_IDLE) {
    /* Nothing is waiting for a confirmation: take the next message. */
    if (s->response_pending) {
      s->armed = ARMED_RESPONSE;
      s->tog = (uint8_t)!s->tog;
    } else if (s->ev_count > 0) {
      s->armed = ARMED_EVENT;
      s->tog = (uint8_t)!s->tog;
    }
  }
  if (s->armed != ARMED_IDLE) {
    f = s->armed == ARMED_RESPONSE ? &s->response : &s->events[s->ev_head];
    bool more = (s->armed == ARMED_RESPONSE ? s->ev_count : s->ev_count - 1u) > 0;
    memset(s->tx, 0, sizeof(s->tx));
    memcpy(s->tx, f->data, f->len);
    /* The CRC covers the payload only, so the flag byte can be set here. */
    s->tx[3] = (uint8_t)(flags | (more ? SENSEI_SPI_FLAG_MORE : 0) | (s->tog ? LPWAN_FLAG_TOGGLE : 0));
  } else {
    load_idle(s);
  }
  /* EVENT stays high until the master has confirmed the last event. */
  bool ev = s->ev_count > 0;

  unlock(s);

  s->ops->set_event(s->ctx, ev);
  memset(s->rx, 0, sizeof(s->rx));
  err = s->ops->arm(s->ctx, s->tx, s->rx, LPWAN_XFER_LEN);
  if (err == 0) {
    s->ops->set_busy(s->ctx, false);
  }
  return err;
}

int link_slave_start(struct link_slave *s) {
  s->ops->set_busy(s->ctx, true);
  s->master_error = false;
  return arm_next(s);
}

int link_slave_respond(struct link_slave *s, const sensei_LpwanResponse *rsp) {
  int32_t n = sensei_encode_LpwanResponse(s->response.data + SENSEI_SPI_HEADER_SIZE, LPWAN_MAX_PAYLOAD, rsp);

  if (n < 0) {
    return -EMSGSIZE;
  }
  sensei_spi_header_pack(s->response.data, SENSEI_SPI_LPWAN_RESPONSE, 0, s->response.data + SENSEI_SPI_HEADER_SIZE,
                         (size_t)n);
  s->response.len = (uint16_t)(SENSEI_SPI_HEADER_SIZE + n);
  if (s->armed == ARMED_RESPONSE) {
    s->armed = ARMED_IDLE; /* the master moved on: the old response is obsolete */
  }
  s->response_pending = true;
  return 0;
}

int link_slave_push_event(struct link_slave *s, const sensei_LpwanEvent *evt) {
  struct link_slave_frame *f;
  int32_t n;
  int ret = 0;
  uint8_t slot;

  lock(s);
  if (s->ev_count >= LINK_SLAVE_EVENT_SLOTS) {
    s->n_dropped_events++;
    unlock(s);
    return -ENOMEM;
  }
  slot = (uint8_t)((s->ev_head + s->ev_count) % LINK_SLAVE_EVENT_SLOTS);
  f = &s->events[slot];
  n = sensei_encode_LpwanEvent(f->data + SENSEI_SPI_HEADER_SIZE, LPWAN_MAX_PAYLOAD, evt);
  if (n < 0) {
    ret = -EMSGSIZE;
  } else {
    sensei_spi_header_pack(f->data, SENSEI_SPI_LPWAN_EVENT, 0, f->data + SENSEI_SPI_HEADER_SIZE, (size_t)n);
    f->len = (uint16_t)(SENSEI_SPI_HEADER_SIZE + n);
    s->ev_count++;
  }
  unlock(s);
  if (ret == 0) {
    s->ops->set_event(s->ctx, true);
  }
  return ret;
}

unsigned link_slave_events_pending(const struct link_slave *s) { return s->ev_count; }

/* The master confirmed our armed message: release it. */
static void release_armed(struct link_slave *s) {
  lock(s);
  if (s->armed == ARMED_RESPONSE) {
    s->response_pending = false; /* kept in s->response for a repeated request */
  } else if (s->armed == ARMED_EVENT && s->ev_count > 0) {
    s->ev_head = (uint8_t)((s->ev_head + 1u) % LINK_SLAVE_EVENT_SLOTS);
    s->ev_count--;
  }
  s->armed = ARMED_IDLE;
  unlock(s);
}

int link_slave_done(struct link_slave *s, int err) {
  sensei_spi_header_t h;
  const uint8_t *payload = NULL;
  bool valid = false;

  s->n_transfers++;
  if (err == 0 && sensei_spi_frame_decode(s->rx, LPWAN_XFER_LEN, &h, &payload) == SENSEI_SPI_OK &&
      sensei_spi_check_payload(&h, payload) == SENSEI_SPI_OK) {
    valid = true;
  }

  if (!valid) {
    /* Bad or empty frame: the armed message stays, the master is asked to repeat. */
    bool blank = true;

    for (size_t i = 0; i < SENSEI_SPI_HEADER_SIZE; i++) {
      if (s->rx[i] != 0x00 && s->rx[i] != 0xFF) {
        blank = false;
        break;
      }
    }
    if (!blank) {
      s->n_bad_frames++;
      s->master_error = true;
    }
    return arm_next(s);
  }

  s->master_error = false;
  if (h.flags & SENSEI_SPI_FLAG_ERROR) {
    s->n_repeats++; /* informative: the master could not read our last frame */
  }
  if ((h.flags & LPWAN_FLAG_ACK) && s->armed != ARMED_IDLE) {
    release_armed(s);
  } else if (s->armed != ARMED_IDLE && !(h.flags & LPWAN_FLAG_ACK)) {
    s->n_repeats++; /* the master has not confirmed it yet: it goes out again */
  }

  if (h.type == SENSEI_SPI_LPWAN_REQUEST) {
    sensei_LpwanRequest req;

    if (!sensei_decode_LpwanRequest(payload, h.len, &req)) {
      s->master_error = true;
      s->n_bad_frames++;
    } else if (s->have_last && req.seq == s->last_seq) {
      s->n_repeats++;
      s->response_pending = true; /* same seq: repeat the stored response */
    } else {
      s->n_requests++;
      s->last_seq = req.seq;
      s->have_last = true;
      s->response_pending = false;
      s->on_request(s, &req, s->user);
    }
  }
  return arm_next(s);
}
