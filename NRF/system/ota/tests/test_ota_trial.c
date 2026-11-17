/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Host test of sensei_ota_trial_step(): when a trial image confirms itself, waits or reverts. */

#include <stdio.h>

#include "sensei_ota_trial.h"

static int failures;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                                                  \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static struct sensei_ota_trial_in base(void) {
  struct sensei_ota_trial_in in = {
      .up_s = 0,
      .confirm_enabled = true,
      .confirm_delay_s = 30,
      .timeout_s = 300,
      .need_link = true,
      .link_seen = false,
      .healthy = true,
  };
  return in;
}

int main(void) {
  struct sensei_ota_trial_in in;

  /* Release: healthy but nobody reached the node: no confirm, revert at the timeout. */
  in = base();
  in.up_s = 29;
  in.link_seen = true;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_WAIT); /* before the delay */
  in.up_s = 30;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_CONFIRM);
  in.link_seen = false;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_WAIT);
  in.up_s = 299;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_WAIT);
  in.up_s = 300;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_REVERT);
  in.up_s = 100000;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_REVERT);

  /* The link is seen late, still in time: confirm. At the timeout itself the confirm wins over the revert. */
  in = base();
  in.up_s = 299;
  in.link_seen = true;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_CONFIRM);
  in.up_s = 300;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_CONFIRM);

  /* Unhealthy never confirms, link or not. */
  in = base();
  in.link_seen = true;
  in.healthy = false;
  in.up_s = 120;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_WAIT);
  in.up_s = 300;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_REVERT);

  /* Debug: no proof needed, the health check alone confirms. */
  in = base();
  in.need_link = false;
  in.up_s = 30;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_CONFIRM);
  in.healthy = false;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_WAIT);

  /* Self-confirm off: only the host confirms; otherwise the trial times out. */
  in = base();
  in.confirm_enabled = false;
  in.link_seen = true;
  in.up_s = 200;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_WAIT);
  in.up_s = 300;
  CHECK(sensei_ota_trial_step(&in) == SENSEI_OTA_TRIAL_REVERT);

  if (failures == 0) {
    printf("ota trial: all checks passed\n");
  }
  return failures != 0;
}
