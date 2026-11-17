/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_lpwan/lpwan_upd.h"

#include <string.h>

#include "sensei_lpwan/lpwan_dfu.h"

void lpwan_upd_init(struct lpwan_upd *u) {
  memset(u, 0, sizeof(*u));
  u->swap_maybe = true;
}

uint32_t lpwan_upd_boot_timeout(const struct lpwan_upd *u, uint32_t normal_ms) {
  return u->swap_maybe && normal_ms < LPWAN_UPD_SWAP_TIMEOUT_MS ? LPWAN_UPD_SWAP_TIMEOUT_MS : normal_ms;
}

void lpwan_upd_bringup_end(struct lpwan_upd *u, bool waited_long, bool ok) { u->swap_maybe = !ok && !waited_long; }

void lpwan_upd_apply_sent(struct lpwan_upd *u) { u->swap_maybe = true; }

void lpwan_upd_power_cut(struct lpwan_upd *u) {
  if (u->trial) {
    u->swap_maybe = true;
    u->trial = false; /* the next power-up shows what is running */
  }
}

void lpwan_upd_version(struct lpwan_upd *u, const sensei_LpwanVersion *v, int64_t now_ms) {
  if (v->trial && !u->trial) {
    u->trial_since = now_ms;
  }
  u->trial = v->trial;
  if (!v->trial) {
    u->known_targets = v->dfu_targets;
  }
}

bool lpwan_upd_confirm_due(const struct lpwan_upd *u) { return u->trial; }

bool lpwan_upd_may_confirm(const struct lpwan_upd *u, const sensei_LpwanVersion *v, uint32_t protocol, bool status_ok) {
  return v->trial && status_ok && v->protocol == protocol && (v->dfu_targets & LPWAN_DFU_TARGET_APP_BIT) != 0 &&
         (v->dfu_targets & u->known_targets) == u->known_targets;
}

bool lpwan_upd_keep_power(const struct lpwan_upd *u, int64_t now_ms) {
  return u->trial && now_ms - u->trial_since < (int64_t)LPWAN_UPD_TRIAL_HOLD_MS;
}

enum lpwan_upd_end lpwan_upd_session_check(int64_t start_ms, int64_t last_req_ms, int64_t now_ms) {
  if (now_ms - start_ms > (int64_t)LPWAN_UPD_SESSION_MAX_MS) {
    return LPWAN_UPD_END_TOTAL;
  }
  if (now_ms - last_req_ms > (int64_t)LPWAN_UPD_SESSION_IDLE_MS) {
    return LPWAN_UPD_END_IDLE;
  }
  return LPWAN_UPD_CONTINUE;
}

bool lpwan_upd_cooling(int64_t until_ms, int64_t now_ms) { return now_ms < until_ms; }

bool lpwan_upd_poll_due(bool modem_busy, int64_t last_req_ms, int64_t now_ms) {
  return modem_busy && now_ms - last_req_ms >= (int64_t)LPWAN_UPD_POLL_MS;
}

bool lpwan_upd_modem_busy(const sensei_DfuStatus *st) {
  return st->target == sensei_DfuTarget_DFU_TARGET_MODEM &&
         (st->state == sensei_DfuState_DFU_STATE_ERASING || st->state == sensei_DfuState_DFU_STATE_APPLYING);
}

bool lpwan_upd_apply_took_effect(const sensei_DfuStatus *st) {
  return st->target == sensei_DfuTarget_DFU_TARGET_APP && st->state == sensei_DfuState_DFU_STATE_APPLYING;
}
