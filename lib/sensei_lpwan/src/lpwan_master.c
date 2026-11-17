/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * Adapted from the LPWAN shield commissioning project (ETH Zurich, 2026).
 */

#include "sensei_lpwan/lpwan_master.h"

#include <errno.h>
#include <string.h>

#include "sensei_link/pb.h"

enum rx_kind { RX_NONE, RX_BAD, RX_RESPONSE, RX_EVENT, RX_IDLE };

void lpwan_master_init(struct lpwan_master *m, const struct lpwan_master_ops *ops, void *ctx) {
  memset(m, 0, sizeof(*m));
  m->ops = ops;
  m->ctx = ctx;
  m->seq = 0;
  m->ready_timeout_ms = 1000;
  m->retries = 2;
}

/* Send m->tx (already a frame), parse what came back. */
static enum rx_kind exchange(struct lpwan_master *m, sensei_LpwanResponse *rsp, int *err) {
  sensei_spi_header_t h;
  const uint8_t *payload;

  *err = m->ops->wait_ready(m->ctx, m->ready_timeout_ms);
  if (*err) {
    return RX_NONE;
  }
  *err = m->ops->xfer(m->ctx, m->tx, m->rx, LPWAN_XFER_LEN);
  m->n_transfers++;
  if (*err) {
    return RX_NONE;
  }
  if (sensei_spi_frame_decode(m->rx, LPWAN_XFER_LEN, &h, &payload) != SENSEI_SPI_OK ||
      sensei_spi_check_payload(&h, payload) != SENSEI_SPI_OK) {
    m->n_bad_frames++;
    m->rx_error = true;
    return RX_BAD;
  }
  m->rx_error = false;
  if (h.flags & SENSEI_SPI_FLAG_ERROR) {
    m->peer_error = true;
  }
  if (h.type == SENSEI_SPI_LPWAN_RESPONSE || h.type == SENSEI_SPI_LPWAN_EVENT) {
    bool tog = (h.flags & LPWAN_FLAG_TOGGLE) != 0;

    m->ack_pending = true; /* confirm it, also if it is a repeat */
    if (tog == m->last_tog) {
      return RX_IDLE; /* a repeat of a message we already took */
    }
    m->last_tog = tog;
  }
  switch (h.type) {
  case SENSEI_SPI_LPWAN_RESPONSE:
    if (rsp && sensei_decode_LpwanResponse(payload, h.len, rsp)) {
      return RX_RESPONSE;
    }
    m->rx_error = true;
    return RX_BAD;
  case SENSEI_SPI_LPWAN_EVENT: {
    sensei_LpwanEvent evt;

    if (!sensei_decode_LpwanEvent(payload, h.len, &evt)) {
      m->rx_error = true;
      return RX_BAD;
    }
    m->n_events++;
    m->ops->on_event(m->ctx, &evt);
    return RX_EVENT;
  }
  default:
    return RX_IDLE;
  }
}

static uint8_t tx_flags(struct lpwan_master *m) {
  uint8_t flags = m->rx_error ? SENSEI_SPI_FLAG_ERROR : 0;

  if (m->ack_pending) {
    flags |= LPWAN_FLAG_ACK;
    m->ack_pending = false; /* sent with this frame */
  }
  return flags;
}

static void build_idle(struct lpwan_master *m) {
  uint8_t flags = tx_flags(m);

  memset(m->tx, 0, sizeof(m->tx));
  sensei_spi_header_pack(m->tx, LPWAN_FRAME_IDLE, flags, m->tx + SENSEI_SPI_HEADER_SIZE, 0);
}

static int build_request(struct lpwan_master *m, const sensei_LpwanRequest *req) {
  int32_t n;

  memset(m->tx, 0, sizeof(m->tx));
  n = sensei_encode_LpwanRequest(m->tx + SENSEI_SPI_HEADER_SIZE, LPWAN_MAX_PAYLOAD, req);
  if (n < 0) {
    return -EMSGSIZE;
  }
  sensei_spi_header_pack(m->tx, SENSEI_SPI_LPWAN_REQUEST, tx_flags(m), m->tx + SENSEI_SPI_HEADER_SIZE, (size_t)n);
  return 0;
}

int lpwan_master_request(struct lpwan_master *m, sensei_LpwanRequest *req, sensei_LpwanResponse *rsp,
                         uint32_t timeout_ms) {
  int err;

  m->seq++;
  if (m->seq == 0) {
    m->seq = 1;
  }
  req->seq = m->seq;

  for (unsigned attempt = 0; attempt <= m->retries; attempt++) {
    bool sent = false;
    uint32_t t0 = m->ops->now_ms(m->ctx);

    if (attempt > 0) {
      m->n_retries++;
    }
    err = build_request(m, req);
    if (err) {
      return err;
    }
    for (;;) {
      enum rx_kind k;

      m->peer_error = false;
      k = exchange(m, rsp, &err);
      if (k == RX_NONE) {
        if (err == -ETIMEDOUT) {
          break; /* slave not ready: try the request again */
        }
        return err;
      }
      if (!sent) {
        sent = true; /* the request went out in this transfer */
      }
      if (m->peer_error) {
        /* The slave could not read our request: send it again, same seq. */
        m->n_retries++;
        err = build_request(m, req);
        if (err) {
          return err;
        }
      } else {
        build_idle(m);
      }
      if (k == RX_RESPONSE && rsp->seq == req->seq) {
        return 0; /* a stale response (other seq) is ignored */
      }
      if ((uint32_t)(m->ops->now_ms(m->ctx) - t0) >= timeout_ms) {
        break;
      }
    }
  }
  return -ETIMEDOUT;
}

int lpwan_master_fetch_events(struct lpwan_master *m, unsigned max_frames) {
  int delivered = 0;

  for (unsigned i = 0; i < max_frames; i++) {
    int err = 0;
    enum rx_kind k;

    if (!m->ops->event_pending(m->ctx) && !m->rx_error && !(i > 0 && m->ack_pending)) {
      break;
    }
    build_idle(m);
    k = exchange(m, NULL, &err);
    if (k == RX_NONE) {
      return err ? err : delivered;
    }
    if (k == RX_EVENT) {
      delivered++;
    }
  }
  return delivered;
}
