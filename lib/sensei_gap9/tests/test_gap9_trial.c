/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* GAP9 slot trial rule: every event in every state. */

#include <stdio.h>

#include "sensei_gap9/gap9_trial.h"

static int fails, checks;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    checks++;                                                                                                          \
    if (!(c)) {                                                                                                        \
      fails++;                                                                                                         \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                                              \
    }                                                                                                                  \
  } while (0)

#define IS(o, a, s) ((o).action == (a) && (o).slot == (s))

static void test_good_update(void) {
  struct sensei_gap9_trial st;
  struct sensei_gap9_trial_out o;

  sensei_gap9_trial_init(&st, 0);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_APPLY, 1);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_BOOT, 1) && st.open && st.trial == 1 && st.confirmed == 0 && st.boots == 1);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_HELLO_OK, 0);
  CHECK(o.action == SENSEI_GAP9_TRIAL_NONE && st.open);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_STATUS_OK, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_CONFIRM, 1) && !st.open && st.confirmed == 1);

  /* and back (host REVERT = APPLY of the other slot) */
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_APPLY, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_BOOT, 0));
  sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_HELLO_OK, 0);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_STATUS_OK, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_CONFIRM, 0) && st.confirmed == 0);
}

static void test_apply_refused(void) {
  struct sensei_gap9_trial st;

  sensei_gap9_trial_init(&st, 1);
  CHECK(sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_APPLY, 1).action ==
        SENSEI_GAP9_TRIAL_NONE); /* the confirmed slot */
  CHECK(sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_APPLY, 2).action == SENSEI_GAP9_TRIAL_NONE);
  CHECK(!st.open);
  CHECK(sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_APPLY, 0).action == SENSEI_GAP9_TRIAL_BOOT);
  CHECK(sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_APPLY, 0).action == SENSEI_GAP9_TRIAL_NONE); /* a trial is open */
  /* events without a trial do nothing */
  sensei_gap9_trial_init(&st, 0);
  for (int e = SENSEI_GAP9_TRIAL_HELLO_OK; e <= SENSEI_GAP9_TRIAL_NRF_RESET; e++) {
    CHECK(sensei_gap9_trial_step(&st, (enum sensei_gap9_trial_event)e, 1).action == SENSEI_GAP9_TRIAL_NONE);
    CHECK(!st.open && st.confirmed == 0);
  }
}

static void test_failures(void) {
  struct sensei_gap9_trial st;
  struct sensei_gap9_trial_out o;

  /* wrong major version, bad status, nRF reset: back to the confirmed slot at once */
  enum sensei_gap9_trial_event fatal[] = {SENSEI_GAP9_TRIAL_HELLO_BAD, SENSEI_GAP9_TRIAL_STATUS_BAD,
                                          SENSEI_GAP9_TRIAL_NRF_RESET};
  for (unsigned i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
    sensei_gap9_trial_init(&st, 0);
    sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_APPLY, 1);
    o = sensei_gap9_trial_step(&st, fatal[i], 0);
    CHECK(IS(o, SENSEI_GAP9_TRIAL_REVERT, 0) && !st.open && st.confirmed == 0);
    /* a late event does nothing */
    CHECK(sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_STATUS_OK, 0).action == SENSEI_GAP9_TRIAL_NONE);
    CHECK(st.confirmed == 0);
  }

  /* a failed status after a good HELLO */
  sensei_gap9_trial_init(&st, 1);
  sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_APPLY, 0);
  sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_HELLO_OK, 0);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_STATUS_BAD, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_REVERT, 1) && st.confirmed == 1);

  /* no HELLO: one more boot, then back */
  sensei_gap9_trial_init(&st, 0);
  sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_APPLY, 1);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_TIMEOUT, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_BOOT, 1) && st.boots == 2 && st.open);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_TIMEOUT, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_REVERT, 0) && !st.open);
  /* the second boot may work */
  sensei_gap9_trial_init(&st, 0);
  sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_APPLY, 1);
  sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_TIMEOUT, 0);
  sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_HELLO_OK, 0);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_STATUS_OK, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_CONFIRM, 1));
  /* a new trial counts again from one */
  sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_APPLY, 0);
  CHECK(st.boots == 1);
}

static void test_lost_settings(void) {
  struct sensei_gap9_trial st;
  struct sensei_gap9_trial_out o;

  /* A answers */
  o = sensei_gap9_trial_lost(&st);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_BOOT, 0) && st.open && st.probing);
  sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_HELLO_OK, 0);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_STATUS_OK, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_CONFIRM, 0) && !st.open && !st.probing && st.confirmed == 0);

  /* A does not boot (no retry while probing), B answers */
  sensei_gap9_trial_lost(&st);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_TIMEOUT, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_BOOT, 1) && st.probing);
  sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_HELLO_OK, 0);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_STATUS_OK, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_CONFIRM, 1) && st.confirmed == 1);

  /* A has a bad status, B answers wrong too: give up */
  sensei_gap9_trial_lost(&st);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_STATUS_BAD, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_BOOT, 1));
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_HELLO_BAD, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_GIVE_UP, 0) && !st.open && st.confirmed == 0);

  /* nRF reset while probing starts the probe again */
  sensei_gap9_trial_lost(&st);
  sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_STATUS_BAD, 0);
  o = sensei_gap9_trial_step(&st, SENSEI_GAP9_TRIAL_NRF_RESET, 0);
  CHECK(IS(o, SENSEI_GAP9_TRIAL_BOOT, 0) && st.trial == 0);
}

int main(void) {
  test_good_update();
  test_apply_refused();
  test_failures();
  test_lost_settings();
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
