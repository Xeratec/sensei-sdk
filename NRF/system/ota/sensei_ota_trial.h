/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#ifndef SENSEI_OTA_TRIAL_H_
#define SENSEI_OTA_TRIAL_H_

/**
 * @file sensei_ota_trial.h
 * @brief The decision of the trial supervision as a pure function (no Zephyr, host-testable).
 *
 * sensei_ota.c calls it every few seconds while the running image is on trial and acts on the result.
 */

#include <stdbool.h>
#include <stdint.h>

enum sensei_ota_trial_action {
  SENSEI_OTA_TRIAL_WAIT = 0, /**< do nothing yet */
  SENSEI_OTA_TRIAL_CONFIRM,  /**< make the image permanent */
  SENSEI_OTA_TRIAL_REVERT,   /**< reset the node, MCUboot goes back to the old image */
};

struct sensei_ota_trial_in {
  uint32_t up_s;            /**< run time of this image, s */
  bool confirm_enabled;     /**< the node may confirm by itself (CONFIG_SENSEI_OTA_CONFIRM) */
  uint32_t confirm_delay_s; /**< earliest self-confirm, s */
  uint32_t timeout_s;       /**< trial time: reset when still unconfirmed, s */
  bool need_link;           /**< confirm only after the link was seen (CONFIG_SENSEI_OTA_CONFIRM_NEEDS_LINK) */
  bool link_seen;           /**< an SMP request or a host session was seen on this image */
  bool healthy;             /**< result of the app's check */
};

/**
 * Self-confirm when the check passed after the delay and, if required, the link was seen: an image
 * that cannot be reached any more must not become permanent, or the revert is lost exactly when a
 * broken Bluetooth or USB link makes a new update impossible. Reset when the trial time is over.
 */
static inline enum sensei_ota_trial_action sensei_ota_trial_step(const struct sensei_ota_trial_in *in) {
  if (in->confirm_enabled && in->up_s >= in->confirm_delay_s && in->healthy && (!in->need_link || in->link_seen)) {
    return SENSEI_OTA_TRIAL_CONFIRM;
  }
  if (in->up_s >= in->timeout_s) {
    return SENSEI_OTA_TRIAL_REVERT;
  }
  return SENSEI_OTA_TRIAL_WAIT;
}

#endif /* SENSEI_OTA_TRIAL_H_ */
