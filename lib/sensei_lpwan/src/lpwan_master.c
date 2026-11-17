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

void lpwan_master_reset(struct lpwan_master *m) {
  m->rx_error = false;
  m->peer_error = false;
  m->ack_pending = false;
  m->tx_ack = false;
  m->have_tog = false;
  m->last_tog = false;
  m->version_errs = 0;
}

void lpwan_master_wipe(struct lpwan_master *m) { memset(m->tx, 0, sizeof(m->tx)); }

/* Send m->tx (already a frame), parse what came back. */
static enum rx_kind exchange(struct lpwan_master *m, sensei_LpwanResponse *rsp, int *err) {
  sensei_spi_header_t h;
  const uint8_t *payload;
  sensei_spi_status_t st;

  *err = m->ops->wait_ready(m->ctx, m->ready_timeout_ms);
  if (*err) {
    return RX_NONE;
  }
  *err = m->ops->xfer(m->ctx, m->tx, m->rx, LPWAN_XFER_LEN);
  m->n_transfers++;
  if (*err) {
    return RX_NONE;
  }
  if (m->tx_ack) {
    m->ack_pending = false; /* the ACK went out with this transfer */
    m->tx_ack = false;
  }
  st = sensei_spi_frame_decode(m->rx, LPWAN_XFER_LEN, &h, &payload); /* checks the CRC */
  if (st == SENSEI_SPI_ERR_VERSION && ++m->version_errs >= 2) {
    *err = -EPROTONOSUPPORT; /* two frames in a row with another version: not a bit error */
    return RX_NONE;
  }
  if (st != SENSEI_SPI_ERR_VERSION) {
    m->version_errs = 0;
  }
  if (st != SENSEI_SPI_OK) {
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
    sensei_LpwanResponse scratch; /* a response nobody waits for (late, after a timeout) */
    sensei_LpwanEvent evt;

    if (m->have_tog && tog == m->last_tog) {
      m->ack_pending = true; /* a repeat of a message already taken: confirm it again */
      m->ack_tog = tog;
      return RX_IDLE;
    }
    if (h.type == SENSEI_SPI_LPWAN_RESPONSE ? !sensei_decode_LpwanResponse(payload, h.len, rsp ? rsp : &scratch)
                                            : !sensei_decode_LpwanEvent(payload, h.len, &evt)) {
      m->rx_error = true; /* not confirmed: the slave sends it again */
      return RX_BAD;
    }
    m->last_tog = tog;
    m->have_tog = true;
    m->ack_pending = true;
    m->ack_tog = tog;
    if (h.type == SENSEI_SPI_LPWAN_RESPONSE) {
      return rsp ? RX_RESPONSE : RX_IDLE;
    }
    m->n_events++;
    m->ops->on_event(m->ctx, &evt);
    return RX_EVENT;
  }
  return RX_IDLE;
}

/* ACK stays pending until a frame that carries it has been sent (exchange() clears it). */
static uint8_t tx_flags(struct lpwan_master *m) {
  uint8_t flags = m->rx_error ? SENSEI_SPI_FLAG_ERROR : 0;

  m->tx_ack = m->ack_pending;
  if (m->ack_pending) {
    flags |= LPWAN_FLAG_ACK | (m->ack_tog ? LPWAN_FLAG_ACK_TOGGLE : 0);
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

static int request(struct lpwan_master *m, sensei_LpwanRequest *req, sensei_LpwanResponse *rsp, uint32_t timeout_ms) {
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

int lpwan_master_request(struct lpwan_master *m, sensei_LpwanRequest *req, sensei_LpwanResponse *rsp,
                         uint32_t timeout_ms) {
  int err = request(m, req, rsp, timeout_ms);

  if (err == 0) {
    m->failures = 0;
  } else if (err != -EMSGSIZE) {
    m->failures++;
  }
  return err;
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
