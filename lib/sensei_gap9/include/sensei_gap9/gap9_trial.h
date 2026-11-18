/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#ifndef SENSEI_GAP9_TRIAL_H_
#define SENSEI_GAP9_TRIAL_H_

/**
 * @file gap9_trial.h
 * @brief Trial boot of a GAP9 firmware slot as a pure function (no Zephyr, host-testable).
 *
 * The nRF5340 owns the decision (the GAP9 SSBL only follows the slot line). State: the confirmed slot (known good),
 * the slot on trial and the boots tried. The app stores the state in NVS before it acts on an action, so that a reset
 * of the nRF5340 in the middle finds the trial again (event NRF_RESET).
 *
 * Flow of an update:
 *   APPLY(X)    -> BOOT X        the app sets the slot line to X and power cycles GAP9 (gap9_link_reboot())
 *   HELLO_OK    -> NONE          GAP9 answered with the right major version
 *   STATUS_OK   -> CONFIRM X     the app asked the new image (ota status job): booted slot X, image id as expected, and
 *                                its self-test passed. The app stores confirmed = X.
 *   anything else -> REVERT      the app boots the confirmed slot (action slot) and drops the trial.
 * A boot without HELLO may be a power glitch: it is tried SENSEI_GAP9_TRIAL_MAX_BOOTS times, a wrong major version or a
 * bad status is not. A host REVERT command is APPLY of the other slot.
 *
 * Lost settings: sensei_gap9_trial_lost() probes slot A, then slot B, and confirms the first that answers.
 */

#include <stdbool.h>
#include <stdint.h>

#define SENSEI_GAP9_TRIAL_MAX_BOOTS 2u

enum sensei_gap9_trial_event {
  SENSEI_GAP9_TRIAL_APPLY,      /**< a verified image waits in slot `slot`: start the trial */
  SENSEI_GAP9_TRIAL_HELLO_OK,   /**< HELLO of the new boot, major version right */
  SENSEI_GAP9_TRIAL_HELLO_BAD,  /**< HELLO with another major version (or one that does not decode) */
  SENSEI_GAP9_TRIAL_TIMEOUT,    /**< no HELLO in the boot time */
  SENSEI_GAP9_TRIAL_STATUS_OK,  /**< status answered: booted slot, image id and self-test fit */
  SENSEI_GAP9_TRIAL_STATUS_BAD, /**< no answer, other slot, other image id or failed self-test */
  SENSEI_GAP9_TRIAL_NRF_RESET,  /**< the nRF5340 started and found the trial open */
};

enum sensei_gap9_trial_action {
  SENSEI_GAP9_TRIAL_NONE = 0, /**< nothing to do */
  SENSEI_GAP9_TRIAL_BOOT,     /**< gap9_link_reboot(slot): then feed HELLO_OK, HELLO_BAD or TIMEOUT back */
  SENSEI_GAP9_TRIAL_CONFIRM,  /**< `slot` is good: store it as confirmed */
  SENSEI_GAP9_TRIAL_REVERT,   /**< the trial failed: boot `slot` (the confirmed slot) */
  SENSEI_GAP9_TRIAL_GIVE_UP,  /**< lost settings and neither slot answers: slot A stays selected */
};

struct sensei_gap9_trial {
  uint8_t confirmed; /**< slot known good: 0 = A, 1 = B */
  uint8_t trial;     /**< slot on trial (valid while `open`) */
  uint8_t boots;     /**< boots tried in this trial */
  bool open;         /**< a trial or a probe is running */
  bool probing;      /**< lost settings: looking for a slot that answers */
};

struct sensei_gap9_trial_out {
  enum sensei_gap9_trial_action action;
  uint8_t slot;
};

static inline void sensei_gap9_trial_init(struct sensei_gap9_trial *st, uint8_t confirmed) {
  st->confirmed = confirmed ? 1 : 0;
  st->trial = st->confirmed;
  st->boots = 0;
  st->open = false;
  st->probing = false;
}

/** No stored state: probe slot A, then B. Returns the first action (BOOT A). */
static inline struct sensei_gap9_trial_out sensei_gap9_trial_lost(struct sensei_gap9_trial *st) {
  sensei_gap9_trial_init(st, 0);
  st->open = true;
  st->probing = true;
  st->trial = 0;
  st->boots = 1;
  return (struct sensei_gap9_trial_out){SENSEI_GAP9_TRIAL_BOOT, 0};
}

static inline struct sensei_gap9_trial_out sensei_gap9_trial_step(struct sensei_gap9_trial *st,
                                                                  enum sensei_gap9_trial_event ev, uint8_t slot) {
  struct sensei_gap9_trial_out none = {SENSEI_GAP9_TRIAL_NONE, st->confirmed};

  if (ev == SENSEI_GAP9_TRIAL_APPLY) {
    if (st->open || slot > 1 || slot == st->confirmed) {
      return none;
    }
    st->open = true;
    st->trial = slot;
    st->boots = 1;
    return (struct sensei_gap9_trial_out){SENSEI_GAP9_TRIAL_BOOT, slot};
  }
  if (!st->open) {
    return none;
  }
  switch (ev) {
  case SENSEI_GAP9_TRIAL_HELLO_OK:
    return (struct sensei_gap9_trial_out){SENSEI_GAP9_TRIAL_NONE, st->trial};
  case SENSEI_GAP9_TRIAL_STATUS_OK: {
    uint8_t s = st->trial;

    st->confirmed = s;
    st->open = false;
    st->probing = false;
    return (struct sensei_gap9_trial_out){SENSEI_GAP9_TRIAL_CONFIRM, s};
  }
  case SENSEI_GAP9_TRIAL_TIMEOUT:
    if (!st->probing && st->boots < SENSEI_GAP9_TRIAL_MAX_BOOTS) {
      st->boots++;
      return (struct sensei_gap9_trial_out){SENSEI_GAP9_TRIAL_BOOT, st->trial};
    }
    break;
  case SENSEI_GAP9_TRIAL_NRF_RESET:
    if (st->probing) {
      return sensei_gap9_trial_lost(st);
    }
    break;
  default: /* HELLO_BAD, STATUS_BAD */
    break;
  }
  /* The trial failed. */
  if (st->probing) {
    if (st->trial == 0) {
      st->trial = 1;
      st->boots = 1;
      return (struct sensei_gap9_trial_out){SENSEI_GAP9_TRIAL_BOOT, 1};
    }
    st->open = false;
    st->probing = false;
    st->confirmed = 0;
    return (struct sensei_gap9_trial_out){SENSEI_GAP9_TRIAL_GIVE_UP, 0};
  }
  st->open = false;
  return (struct sensei_gap9_trial_out){SENSEI_GAP9_TRIAL_REVERT, st->confirmed};
}

#endif /* SENSEI_GAP9_TRIAL_H_ */
