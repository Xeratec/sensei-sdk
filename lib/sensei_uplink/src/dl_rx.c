/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_uplink/dl_rx.h"

void dl_rx_init(struct dl_rx *rx, uint8_t *buf, size_t cap, size_t mtu, uint32_t timeout_ms) {
  rx->n_acks = 0;
  rx->n_bad = 0;
  rx->n_message = 0;
  sensei_reasm_init(&rx->reasm, buf, cap, mtu, timeout_ms);
}

bool dl_rx_feed(struct dl_rx *rx, const uint8_t *data, size_t len, uint32_t now_ms, const uint8_t **msg,
                size_t *msg_len) {
  if (sensei_frag_is_ack(data, len, NULL, NULL)) {
    rx->n_acks++;
    return false;
  }
  switch (sensei_reasm_feed(&rx->reasm, data, len, now_ms, msg, msg_len)) {
  case SENSEI_REASM_COMPLETE:
    rx->n_message++;
    return true;
  case SENSEI_REASM_ERR_FORMAT:
  case SENSEI_REASM_ERR_OVERFLOW:
    rx->n_bad++;
    return false;
  default:
    return false;
  }
}
