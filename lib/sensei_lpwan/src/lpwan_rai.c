/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_lpwan/lpwan_rai.h"

#include "sensei_link/frag.h"

sensei_LteRai lpwan_rai_for_chunk(sensei_LteRai mode, bool last_chunk) {
  if (!last_chunk || (mode != sensei_LteRai_LTE_RAI_LAST && mode != sensei_LteRai_LTE_RAI_ONE_RESP)) {
    return sensei_LteRai_LTE_RAI_OFF;
  }
  return mode;
}

struct lpwan_rai_plan lpwan_rai_plan_for(sensei_LteRai flag) {
  struct lpwan_rai_plan p = {LPWAN_RAI_OPT_NONE, false, LPWAN_RAI_OPT_NONE};

  if (flag == sensei_LteRai_LTE_RAI_LAST) {
    p.before = LPWAN_RAI_OPT_LAST;
  } else if (flag == sensei_LteRai_LTE_RAI_ONE_RESP) {
    p.before = LPWAN_RAI_OPT_ONE_RESP;
    p.wait_answer = true;
    p.after = LPWAN_RAI_OPT_NO_DATA;
  }
  return p;
}

static void set_rai(const struct lpwan_rai_io *io, void *ctx, enum lpwan_rai_opt opt, struct lpwan_rai_out *out) {
  if (opt != LPWAN_RAI_OPT_NONE && io->set_rai(ctx, opt) != 0) {
    out->rai_errors++;
  }
}

void lpwan_rai_send(const struct lpwan_rai_io *io, void *ctx, sensei_LteRai flag, const uint8_t *data, size_t len,
                    uint32_t wait_ms, struct lpwan_rai_out *out) {
  struct lpwan_rai_plan p = lpwan_rai_plan_for(flag);

  out->sent = false;
  out->answered = false;
  out->rai_errors = 0;
  set_rai(io, ctx, p.before, out);
  out->sent = io->send(ctx, data, len) == 0;
  if (out->sent && p.wait_answer && wait_ms != 0) {
    out->answered = io->wait_answer(ctx, wait_ms);
  }
  /* After the answer or the end of the wait. If the send failed nothing is expected any more either. */
  set_rai(io, ctx, p.after, out);
}

bool lpwan_rai_send_ok(sensei_LteRai flag, uint32_t wait_ms, const struct lpwan_rai_out *out) {
  if (!out->sent) {
    return false;
  }
  return flag != sensei_LteRai_LTE_RAI_ONE_RESP || wait_ms == 0 || out->answered;
}

enum lpwan_answer lpwan_answer_classify(const uint8_t *data, size_t len, bool has_chunk, uint8_t chunk_msg_id,
                                        uint8_t chunk_index) {
  uint8_t id, idx;

  if (!sensei_frag_is_ack(data, len, &id, &idx)) {
    return LPWAN_ANSWER_DOWNLINK;
  }
  return has_chunk && id == chunk_msg_id && idx == chunk_index ? LPWAN_ANSWER_ACK : LPWAN_ANSWER_OLD_ACK;
}

sensei_EnergyEstimate lpwan_energy_from_modem(int v) {
  switch (v) {
  case 5:
    return sensei_EnergyEstimate_ENERGY_EXCESSIVE;
  case 6:
    return sensei_EnergyEstimate_ENERGY_INCREASED;
  case 7:
    return sensei_EnergyEstimate_ENERGY_NORMAL;
  case 8:
    return sensei_EnergyEstimate_ENERGY_REDUCED;
  case 9:
    return sensei_EnergyEstimate_ENERGY_EFFICIENT;
  default:
    return sensei_EnergyEstimate_ENERGY_UNKNOWN;
  }
}

bool lpwan_energy_is_bad(sensei_EnergyEstimate e) { return e == sensei_EnergyEstimate_ENERGY_EXCESSIVE; }

int lpwan_rsrp_dbm(int idx) { return idx < 0 ? idx - 140 : idx - 141; }

int lpwan_rsrq_db(int idx) {
  if (idx < 0) {
    return (idx - 39) / 2;
  }
  return idx < 35 ? (idx - 40) / 2 : (idx - 41) / 2;
}

sensei_RaiSupport lpwan_rai_support(bool as_rai, bool cp_rai) {
  if (as_rai && cp_rai) {
    return sensei_RaiSupport_RAI_AS_CP;
  }
  if (as_rai) {
    return sensei_RaiSupport_RAI_AS;
  }
  return cp_rai ? sensei_RaiSupport_RAI_CP : sensei_RaiSupport_RAI_NONE;
}

enum lpwan_eval_action lpwan_eval_decide(const struct lpwan_eval_state *s, uint32_t now, uint32_t valid_ms,
                                         uint32_t timeout_ms, uint32_t retry_ms) {
  if (s->have && (uint32_t)(now - s->at) < valid_ms) {
    return s->bad ? LPWAN_EVAL_BAD : LPWAN_EVAL_OK;
  }
  if (s->pending && (uint32_t)(now - s->asked) < timeout_ms) {
    return LPWAN_EVAL_WAIT;
  }
  if (s->failed && (uint32_t)(now - s->failed_at) < retry_ms) {
    return LPWAN_EVAL_OK;
  }
  return LPWAN_EVAL_ASK;
}

void lpwan_eval_result(struct lpwan_eval_state *s, uint32_t now, bool valid, bool bad) {
  s->pending = false;
  if (valid) {
    s->have = true;
    s->at = now;
    s->bad = bad;
    s->failed = false;
  } else {
    s->failed = true;
    s->failed_at = now;
  }
}
