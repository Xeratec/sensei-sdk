/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Client rules of the shield update (lpwan_upd.c): when a swap may be running, the trial of a new image, the end of a
 * session. The bring-up loop of the client is modelled to count the power cuts during a swap. */

#include <stdio.h>
#include <string.h>

#include "sensei_lpwan/lpwan_dfu.h"
#include "sensei_lpwan/lpwan_upd.h"

static int check_failures;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                                  \
      check_failures++;                                                                                                \
    }                                                                                                                  \
  } while (0)

#define SHORT_MS 3000u /* CONFIG_SENSEI_LPWAN_BOOT_TIMEOUT_MS */
#define PROTO 2u

static sensei_LpwanVersion version(bool trial, uint32_t targets) {
  sensei_LpwanVersion v = sensei_LpwanVersion_init_zero;

  v.protocol = PROTO;
  v.trial = trial;
  v.dfu_targets = targets;
  return v;
}

/* The bring-up loop of the client against a shield that is busy swapping for `swap_ms` after each power-up.
 * Returns the power cuts and the time until the shield answered, or -1. */
static int bring_up(struct lpwan_upd *u, uint32_t swap_ms, int max_attempts, int *cuts, int64_t *elapsed) {
  *cuts = 0;
  *elapsed = 0;
  for (int i = 0; i < max_attempts; i++) {
    uint32_t wait = lpwan_upd_boot_timeout(u, SHORT_MS);
    bool lng = wait > SHORT_MS;

    if (swap_ms <= wait) {
      *elapsed += swap_ms;
      lpwan_upd_bringup_end(u, lng, true);
      return 0;
    }
    *elapsed += wait;
    lpwan_upd_bringup_end(u, lng, false);
    (*cuts)++; /* the client cuts the power and retries after 15 s */
    *elapsed += 15000;
  }
  return -1;
}

/* K1: a swap outside dfu_restart_shield (revert of an unconfirmed image, node reset, lost apply) is waited for. */
static void test_swap_wait(void) {
  struct lpwan_upd u;
  int cuts;
  int64_t t;

  /* first bring-up after a node reset: the shield may be swapping */
  lpwan_upd_init(&u);
  CHECK(lpwan_upd_boot_timeout(&u, SHORT_MS) == LPWAN_UPD_SWAP_TIMEOUT_MS);
  CHECK(bring_up(&u, 40000, 20, &cuts, &t) == 0 && cuts == 0);
  CHECK(lpwan_upd_boot_timeout(&u, SHORT_MS) == SHORT_MS); /* it answered: normal again */

  /* a normal power-up takes 3 s at most: no long wait, the dead shield is cut after 3 s */
  CHECK(bring_up(&u, 1500, 20, &cuts, &t) == 0 && cuts == 0 && t == 1500);

  /* an image on trial is cut (link lost): the next power-up is a revert swap of 40 s */
  {
    sensei_LpwanVersion v = version(true, 1);

    lpwan_upd_version(&u, &v, 1000);
    lpwan_upd_power_cut(&u);
    CHECK(lpwan_upd_boot_timeout(&u, SHORT_MS) == LPWAN_UPD_SWAP_TIMEOUT_MS);
    CHECK(bring_up(&u, 40000, 20, &cuts, &t) == 0 && cuts == 0); /* was: 13 cuts */
  }

  /* no trial: a power cut is no swap */
  {
    sensei_LpwanVersion v = version(false, 1);

    lpwan_upd_version(&u, &v, 2000);
    lpwan_upd_power_cut(&u);
    CHECK(lpwan_upd_boot_timeout(&u, SHORT_MS) == SHORT_MS);
  }

  /* an apply was sent (the answer may be lost) */
  lpwan_upd_apply_sent(&u);
  CHECK(bring_up(&u, 44000, 20, &cuts, &t) == 0 && cuts == 0);

  /* simplest rule: after a short attempt that timed out the next one waits long, once */
  CHECK(bring_up(&u, 20000, 20, &cuts, &t) == 0 && cuts == 1);
  CHECK(lpwan_upd_boot_timeout(&u, SHORT_MS) == SHORT_MS);

  /* a dead shield is cut again and again, but a long wait is followed by a short one */
  lpwan_upd_init(&u);
  CHECK(bring_up(&u, 1000000, 4, &cuts, &t) == -1 && cuts == 4);
  CHECK(t > 4 * 15000);
  CHECK(lpwan_upd_boot_timeout(&u, SHORT_MS) == LPWAN_UPD_SWAP_TIMEOUT_MS || cuts == 4);
}

static void test_trial(void) {
  struct lpwan_upd u;
  sensei_LpwanVersion v;

  lpwan_upd_init(&u);
  v = version(false, 3);
  lpwan_upd_version(&u, &v, 0);
  CHECK(!lpwan_upd_confirm_due(&u) && !lpwan_upd_keep_power(&u, 0));
  CHECK(u.known_targets == 3);

  v = version(true, 3);
  lpwan_upd_version(&u, &v, 10000);
  CHECK(lpwan_upd_confirm_due(&u));
  CHECK(lpwan_upd_keep_power(&u, 10000 + 60000));
  CHECK(!lpwan_upd_keep_power(&u, 10000 + LPWAN_UPD_TRIAL_HOLD_MS)); /* the image reverts by itself */
  lpwan_upd_version(&u, &v, 10000 + 100000);                         /* more trial versions do not extend it */
  CHECK(!lpwan_upd_keep_power(&u, 10000 + LPWAN_UPD_TRIAL_HOLD_MS));

  /* confirmed: the client lets go */
  v = version(false, 3);
  lpwan_upd_version(&u, &v, 30000);
  CHECK(!lpwan_upd_confirm_due(&u) && !lpwan_upd_keep_power(&u, 30000));

  /* K3: only an image that can be updated again and that answers is confirmed */
  v = version(true, 3);
  CHECK(lpwan_upd_may_confirm(&u, &v, PROTO, true));
  CHECK(!lpwan_upd_may_confirm(&u, &v, PROTO, false));    /* the status round trip failed */
  CHECK(!lpwan_upd_may_confirm(&u, &v, PROTO + 1, true)); /* other protocol */
  v = version(true, 0);
  CHECK(!lpwan_upd_may_confirm(&u, &v, PROTO, true)); /* no update support: the next update is lost */
  v = version(true, LPWAN_DFU_TARGET_APP_BIT);
  CHECK(!lpwan_upd_may_confirm(&u, &v, PROTO, true)); /* the modem update support is gone */
  v = version(true, LPWAN_DFU_TARGET_MODEM_BIT);
  CHECK(!lpwan_upd_may_confirm(&u, &v, PROTO, true)); /* no app support */
  v = version(true, 3);
  v.trial = false;
  CHECK(!lpwan_upd_may_confirm(&u, &v, PROTO, true)); /* not on trial */
  lpwan_upd_init(&u);                                 /* nothing known: the app bit is enough */
  v = version(true, LPWAN_DFU_TARGET_APP_BIT);
  CHECK(lpwan_upd_may_confirm(&u, &v, PROTO, true));
}

static void test_session(void) {
  int64_t start = 100000;

  CHECK(lpwan_upd_session_check(start, start, start) == LPWAN_UPD_CONTINUE);
  CHECK(lpwan_upd_session_check(start, start + 1000, start + 1000 + LPWAN_UPD_SESSION_IDLE_MS) == LPWAN_UPD_CONTINUE);
  CHECK(lpwan_upd_session_check(start, start + 1000, start + 1001 + LPWAN_UPD_SESSION_IDLE_MS) == LPWAN_UPD_END_IDLE);
  /* K15: renewed for ever: the total time ends it */
  CHECK(lpwan_upd_session_check(start, start + LPWAN_UPD_SESSION_MAX_MS, start + LPWAN_UPD_SESSION_MAX_MS) ==
        LPWAN_UPD_CONTINUE);
  CHECK(lpwan_upd_session_check(start, start + LPWAN_UPD_SESSION_MAX_MS, start + LPWAN_UPD_SESSION_MAX_MS + 1001) ==
        LPWAN_UPD_END_TOTAL);
  CHECK(LPWAN_UPD_BUS_HOLD_MS > LPWAN_UPD_SESSION_IDLE_MS);
  /* the pause after a session that was too long */
  CHECK(lpwan_upd_cooling(start + LPWAN_UPD_COOLDOWN_MS, start + LPWAN_UPD_COOLDOWN_MS - 1));
  CHECK(!lpwan_upd_cooling(start + LPWAN_UPD_COOLDOWN_MS, start + LPWAN_UPD_COOLDOWN_MS));
  CHECK(!lpwan_upd_cooling(0, start));
}

static void test_modem_poll(void) {
  sensei_DfuStatus st = sensei_DfuStatus_init_zero;

  st.target = sensei_DfuTarget_DFU_TARGET_MODEM;
  st.state = sensei_DfuState_DFU_STATE_ERASING;
  CHECK(lpwan_upd_modem_busy(&st));
  st.state = sensei_DfuState_DFU_STATE_APPLYING;
  CHECK(lpwan_upd_modem_busy(&st));
  st.state = sensei_DfuState_DFU_STATE_RECEIVING;
  CHECK(!lpwan_upd_modem_busy(&st));
  st.state = sensei_DfuState_DFU_STATE_DONE;
  CHECK(!lpwan_upd_modem_busy(&st));
  st.target = sensei_DfuTarget_DFU_TARGET_APP;
  st.state = sensei_DfuState_DFU_STATE_APPLYING;
  CHECK(!lpwan_upd_modem_busy(&st) && lpwan_upd_apply_took_effect(&st));
  st.state = sensei_DfuState_DFU_STATE_VERIFIED;
  CHECK(!lpwan_upd_apply_took_effect(&st)); /* the apply did not happen: the host repeats it */

  /* the host is gone: the client asks the shield itself, every 5 s, only while the modem works */
  CHECK(!lpwan_upd_poll_due(true, 1000, 1000 + LPWAN_UPD_POLL_MS - 1));
  CHECK(lpwan_upd_poll_due(true, 1000, 1000 + LPWAN_UPD_POLL_MS));
  CHECK(!lpwan_upd_poll_due(false, 1000, 1000 + 10 * LPWAN_UPD_POLL_MS));
}

int main(void) {
  test_swap_wait();
  test_trial();
  test_session();
  test_modem_poll();
  printf("%s\n", check_failures ? "FAILED" : "ok");
  return check_failures ? 1 : 0;
}
