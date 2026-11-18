/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_lpwan/lpwan_sim.h"

#include <string.h>

size_t lpwan_sim_pack(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out) {
  if (len > LPWAN_SIM_MAX_PAYLOAD) {
    return 0;
  }
  out[0] = type;
  out[1] = (uint8_t)(len & 0xFFu);
  out[2] = (uint8_t)(len >> 8);
  if (len > 0) {
    memcpy(out + LPWAN_SIM_HEADER, payload, len);
  }
  return LPWAN_SIM_HEADER + len;
}

int lpwan_sim_rx_next(const struct lpwan_sim_rx *r, uint8_t *type, const uint8_t **payload, size_t *len) {
  size_t n;

  if (r->n < LPWAN_SIM_HEADER) {
    return 0;
  }
  n = (size_t)r->buf[1] | ((size_t)r->buf[2] << 8);
  if (n > LPWAN_SIM_MAX_PAYLOAD) {
    return -1;
  }
  if (r->n < LPWAN_SIM_HEADER + n) {
    return 0;
  }
  *type = r->buf[0];
  *payload = r->buf + LPWAN_SIM_HEADER;
  *len = n;
  return 1;
}

void lpwan_sim_rx_pop(struct lpwan_sim_rx *r) {
  size_t n;

  if (r->n < LPWAN_SIM_HEADER) {
    return;
  }
  n = LPWAN_SIM_HEADER + ((size_t)r->buf[1] | ((size_t)r->buf[2] << 8));
  if (n > r->n) {
    n = r->n;
  }
  memmove(r->buf, r->buf + n, r->n - n);
  r->n -= n;
}
