/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <string.h>

#include "sensei_lpwan/lpwan_lora.h"

uint32_t lpwan_lora_max_payload(uint8_t data_rate, bool adr) {
  static const uint8_t eu868[] = {51, 51, 51, 115, 222, 222, 222, 222};

  if (adr || data_rate >= sizeof(eu868)) {
    return eu868[0];
  }
  return eu868[data_rate];
}

uint32_t lpwan_lora_chunk_max(uint8_t data_rate, bool adr, uint32_t next_max) {
  uint32_t nominal = lpwan_lora_max_payload(data_rate, adr);

  if (next_max == 0 || next_max >= nominal) {
    return nominal;
  }
  /* The LR1121 owes MAC answers: they take room now, and the network may ask for more. */
  return next_max > LPWAN_LORA_MAC_MARGIN + 4u ? next_max - LPWAN_LORA_MAC_MARGIN : 4u;
}

bool lpwan_lora_chunk_too_big(uint8_t data_rate, bool adr, uint32_t next_max, uint32_t len) {
  return next_max != 0 && lpwan_lora_chunk_max(data_rate, adr, next_max) < len;
}

uint32_t lpwan_lora_airtime_ms(uint8_t data_rate, uint32_t phy_len) {
  static const uint8_t sf[] = {12, 11, 10, 9, 8, 7, 7};
  uint32_t bw, tsym_us, de, sfv, n_pl;
  int32_t num;

  if (data_rate == 7) { /* FSK 50 kbit/s: preamble 5, sync 3, length 1, payload, CRC 2 (160 us per byte) */
    return ((11u + phy_len) * 160u + 999u) / 1000u;
  }
  if (data_rate > 6) {
    data_rate = 0;
  }
  sfv = sf[data_rate];
  bw = data_rate == 6 ? 250000u : 125000u;
  tsym_us = (1u << sfv) * 1000000u / bw;
  de = (sfv >= 11 && bw == 125000u) ? 1u : 0u;
  /* Semtech AN1200.13: explicit header, CRC on, coding rate 4/5, 8 preamble symbols. */
  num = (int32_t)(8u * phy_len) - 4 * (int32_t)sfv + 28 + 16;
  n_pl = 8u;
  if (num > 0) {
    uint32_t den = 4u * (sfv - 2u * de);

    n_pl += (((uint32_t)num + den - 1u) / den) * 5u;
  }
  /* (8 + 4.25 + n_pl) symbols, in quarters to stay in integers */
  return (uint32_t)(((uint64_t)(49u + 4u * n_pl) * tsym_us / 4u + 999u) / 1000u);
}

static uint32_t sat32(uint64_t v) { return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v; }

uint32_t lpwan_lora_tx_ms(uint8_t data_rate, bool adr, uint32_t app_len, uint32_t nb_trans, bool confirmed) {
  uint64_t n = (nb_trans >= 1 && nb_trans <= LPWAN_LORA_NB_TRANS_MAX ? nb_trans : 1u) + (confirmed ? 1u : 0u);
  uint64_t air = lpwan_lora_airtime_ms(adr ? 0 : data_rate, app_len + LPWAN_LORA_PHY_OVERHEAD);
  uint64_t gap = 99u * air > LPWAN_LORA_RX_MS ? 99u * air : LPWAN_LORA_RX_MS;

  return sat32(n * air + (n - 1u) * gap + LPWAN_LORA_RX_MS);
}

void lpwan_lora_estimate(const struct lpwan_lora_est_in *in, struct lpwan_lora_est *out) {
  bool nb_ok = in->nb_known && in->nb_trans >= 1 && in->nb_trans <= LPWAN_LORA_NB_TRANS_MAX;
  bool conservative = !in->duty_known || !nb_ok;
  uint8_t dr = conservative ? 0 : in->data_rate;
  bool adr = conservative ? false : in->adr;
  uint32_t nb = nb_ok ? in->nb_trans : 1u;
  bool confirmed = conservative ? true : in->confirmed;

  if (in->duty_known) {
    out->wait_ms = in->duty < 0 ? (uint32_t)(-(int64_t)in->duty) : 0u;
  } else {
    /* As if a frame of the same size had just gone out. */
    out->wait_ms = sat32(99u * (uint64_t)lpwan_lora_airtime_ms(dr, in->app_len + LPWAN_LORA_PHY_OVERHEAD));
  }
  out->tx_ms = lpwan_lora_tx_ms(dr, adr, in->app_len, nb, confirmed);
}

static uint32_t est_total_ms(const struct lpwan_lora_est *e) {
  uint64_t t = (uint64_t)e->wait_ms + e->tx_ms;

  return t > LPWAN_SEND_MAX_MS ? LPWAN_SEND_MAX_MS : (uint32_t)t;
}

uint32_t lpwan_lora_expected_ms(uint8_t data_rate, bool adr, uint32_t app_len, uint32_t duty_wait_ms, uint32_t nb_trans,
                                bool confirmed) {
  struct lpwan_lora_est e = {.wait_ms = duty_wait_ms,
                             .tx_ms = lpwan_lora_tx_ms(data_rate, adr, app_len, nb_trans, confirmed)};

  return est_total_ms(&e);
}

uint32_t lpwan_lora_retry_after_ms(const struct lpwan_lora_est *est) {
  uint32_t allowed = est->tx_ms < LPWAN_SEND_MAX_MS ? LPWAN_SEND_MAX_MS - est->tx_ms : 0u;

  return est->wait_ms > allowed ? est->wait_ms - allowed : 0u;
}

void lpwan_lora_plan_send(const struct lpwan_lora_send_in *in, struct lpwan_lora_send_out *out) {
  struct lpwan_lora_est_in ei = in->est;
  struct lpwan_lora_est est;
  bool max_known = in->max_known && in->max != 0; /* 0: the LR1121 did not say, not "no room" */
  bool flush = false;
  uint32_t retry;

  memset(out, 0, sizeof(*out));
  out->result = sensei_LpwanResult_LPWAN_OK;
  if (in->len == 0 || in->len > 222u) {
    out->result = sensei_LpwanResult_LPWAN_INVALID;
    return;
  }
  if (in->held) {
    out->result = sensei_LpwanResult_LPWAN_BUSY;
    out->queued_seq = in->held_seq;
    out->expected_ms = in->held_left_ms;
    return;
  }
  if (!in->joined) {
    out->result = sensei_LpwanResult_LPWAN_NOT_JOINED;
    return;
  }
  if (max_known) {
    out->next_max = in->max;
    if (in->len > in->max) {
      if (in->max >= LPWAN_LORA_MIN_CHUNK) {
        out->result = sensei_LpwanResult_LPWAN_INVALID; /* the master chunks again */
        return;
      }
      flush = true; /* MAC answers use the room: an empty uplink carries them */
    }
  }
  ei.app_len = flush ? 0u : in->len;
  ei.confirmed = flush ? false : in->est.confirmed;
  lpwan_lora_estimate(&ei, &est);
  retry = lpwan_lora_retry_after_ms(&est);
  if (retry != 0 || flush) {
    out->result = sensei_LpwanResult_LPWAN_BUSY;
    out->expected_ms = retry != 0 ? retry : est_total_ms(&est);
    out->flush = flush && retry == 0;
    return;
  }
  out->expected_ms = est_total_ms(&est);
}

/* ---- shield side: the send in flight ---- */

void lpwan_txt_init(struct lpwan_txtrack *t) { memset(t, 0, sizeof(*t)); }

void lpwan_txt_open(struct lpwan_txtrack *t, uint32_t seq) {
  t->open_seq = seq;
  t->wd_seq = 0;
}

void lpwan_txt_refused(struct lpwan_txtrack *t, uint32_t seq) {
  if (t->open_seq == seq) {
    t->open_seq = 0;
    t->wd_seq = 0;
  }
}

bool lpwan_txt_armed(struct lpwan_txtrack *t, uint32_t seq, uint32_t now_ms, uint32_t limit_ms) {
  if (seq == 0 || t->open_seq != seq) {
    return false;
  }
  t->wd_seq = seq;
  t->start_ms = now_ms;
  t->limit_ms = limit_ms;
  return true;
}

bool lpwan_txt_wants(const struct lpwan_txtrack *t, uint32_t seq) { return t->open_seq != 0 && t->open_seq == seq; }

void lpwan_txt_done(struct lpwan_txtrack *t, uint32_t seq) {
  if (t->open_seq == seq) {
    t->open_seq = 0;
    t->wd_seq = 0;
  }
}

void lpwan_txt_late(struct lpwan_txtrack *t, uint32_t seq, bool ok) {
  if (seq != 0) {
    t->late_seq = seq;
    t->late_ok = ok;
  }
}

bool lpwan_txt_take_late(struct lpwan_txtrack *t, uint32_t *seq, bool *ok) {
  if (t->late_seq == 0) {
    return false;
  }
  *seq = t->late_seq;
  *ok = t->late_ok;
  t->late_seq = 0;
  t->late_ok = false;
  return true;
}

enum lpwan_wd_action lpwan_txt_wd(const struct lpwan_txtrack *t, uint32_t now_ms, bool frame_held, uint32_t *seq,
                                  uint32_t *rearm_ms) {
  uint32_t elapsed = now_ms - t->start_ms;
  uint32_t hard = sat32(2u * (uint64_t)t->limit_ms);

  *seq = t->open_seq;
  *rearm_ms = 0;
  /* A watchdog that was armed for a send that has its result, or for another send than the open one, does nothing. */
  if (t->open_seq == 0 || t->wd_seq != t->open_seq) {
    return LPWAN_WD_NONE;
  }
  if (elapsed < t->limit_ms) { /* a stale run of an earlier arming: not due */
    *rearm_ms = t->limit_ms - elapsed;
    return LPWAN_WD_WAIT;
  }
  if (!frame_held) {
    return LPWAN_WD_FAIL;
  }
  if (elapsed < hard) {
    *rearm_ms = hard - elapsed;
    return LPWAN_WD_WAIT;
  }
  return LPWAN_WD_RESET;
}

/* ---- client side: the frame the shield may still hold ---- */

static uint32_t hash32(const uint8_t *p, size_t n) {
  uint32_t h = 2166136261u; /* FNV-1a */

  while (n--) {
    h = (h ^ *p++) * 16777619u;
  }
  return h;
}

void lpwan_held_arm(struct lpwan_held *h, uint32_t seq, const uint8_t *data, size_t len) {
  h->held = true;
  h->until_ms = 0;
  h->seq = seq;
  h->len = (uint16_t)len;
  h->hash = hash32(data, len);
}

void lpwan_held_result(struct lpwan_held *h, uint32_t seq) {
  if (h->held && h->seq == seq) {
    h->held = false;
  }
}

void lpwan_held_clear(struct lpwan_held *h) { h->held = false; }

void lpwan_held_expect(struct lpwan_held *h, uint32_t now_ms, uint32_t period_ms) {
  h->until_ms = now_ms + sat32(2u * (uint64_t)period_ms);
}

bool lpwan_held_keeps_power(const struct lpwan_held *h, uint32_t now_ms) {
  return h->held && (int32_t)(h->until_ms - now_ms) > 0;
}

enum lpwan_busy_action lpwan_held_busy(const struct lpwan_held *h, const uint8_t *data, size_t len,
                                       uint32_t queued_seq) {
  if (queued_seq == 0) {
    return LPWAN_BUSY_RETRY_LATER;
  }
  if (h->held && h->seq == queued_seq && h->len == len && h->hash == hash32(data, len)) {
    return LPWAN_BUSY_ADOPT;
  }
  return LPWAN_BUSY_WAIT_RESEND;
}

enum lpwan_deadline_action lpwan_held_deadline(uint32_t seq, bool extended, bool status_known,
                                               const sensei_LpwanStatus *st) {
  if (!status_known) {
    return LPWAN_DL_GIVE_UP;
  }
  if (seq != 0 && st->late_tx_seq == seq) {
    return st->late_tx_ok ? LPWAN_DL_OK : LPWAN_DL_FAILED;
  }
  return st->queued != 0 && !extended ? LPWAN_DL_EXTEND : LPWAN_DL_GIVE_UP;
}

uint32_t lpwan_send_limit_ms(uint32_t expected_ms, uint32_t floor_ms, uint32_t extra_ms) {
  uint32_t t = (expected_ms > LPWAN_SEND_MAX_MS ? LPWAN_SEND_MAX_MS : expected_ms) + extra_ms;

  return t > floor_ms ? t : floor_ms;
}
