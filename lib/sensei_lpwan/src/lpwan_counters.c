/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_lpwan/lpwan_counters.h"

static void rrc_end(struct lpwan_counters *c, uint32_t now_ms) {
  if (c->rrc_on) {
    int32_t d = (int32_t)(now_ms - c->rrc_since);

    c->rrc_ms += d > 0 ? (uint32_t)d : 0;
    c->rrc_on = false;
  }
}

void lpwan_cnt_rrc(struct lpwan_counters *c, bool connected, uint32_t now_ms) {
  if (connected) {
    if (!c->rrc_on) {
      c->rrc_on = true;
      c->rrc_since = now_ms;
    }
  } else {
    rrc_end(c, now_ms);
  }
}

void lpwan_cnt_reg(struct lpwan_counters *c, bool registered, uint32_t now_ms) {
  if (registered && !c->registered) {
    c->attaches++;
  } else if (!registered) {
    rrc_end(c, now_ms);
  }
  c->registered = registered;
}

void lpwan_cnt_psm(struct lpwan_counters *c) { c->psm_entries++; }

uint32_t lpwan_cnt_rrc_ms(const struct lpwan_counters *c, uint32_t now_ms) {
  uint32_t ms = c->rrc_ms;

  if (c->rrc_on) {
    int32_t d = (int32_t)(now_ms - c->rrc_since);

    ms += d > 0 ? (uint32_t)d : 0;
  }
  return ms;
}
