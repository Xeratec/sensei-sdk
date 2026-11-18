/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file gap9_ota_zephyr.h
 * @brief Update of the GAP9 application on the nRF5340 (Zephyr): the flow of g9ota_flow.h with the settings, the
 *        signature check (sensei_crypto), the GAP9 link (gap9_zephyr.h) and the housekeeping timer. Needs
 *        CONFIG_SENSEI_GAP9_OTA. The rules are in OTA.md.
 *
 * The app brings what is its own: the public key of the image signature, how a request becomes a job of its job
 * format (and the answer an Gap9OtaInfo), the checks of a new image on trial, and the work queue that runs the flow.
 * It calls the three request functions from its host link. The trial state and the image ids that were verified are
 * stored in the settings under "sensei_gap9/trial" before the flow acts on them.
 */

#ifndef SENSEI_GAP9_GAP9_OTA_ZEPHYR_H_
#define SENSEI_GAP9_GAP9_OTA_ZEPHYR_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "sensei_gap9/g9ota_flow.h"

#ifdef __cplusplus
extern "C" {
#endif

struct sensei_gap9_ota_cfg {
  /** Public key of the image signature: the uncompressed P-256 point (65 B), and the SHA-256 of it (32 B). An image
   * that carries another key hash is refused. */
  const uint8_t *pubkey;
  size_t pubkey_len;
  const uint8_t *key_hash;
  size_t key_hash_len;
  /** Submit the update job for `req` in the app's job format (the request is valid during the call) and call `cb` once
   * with the Gap9OtaInfo of the answer, or with the error (see g9ota_job_cb). Runs on the work queue `wq`. */
  int (*job)(const struct g9ota_req *req, g9ota_job_cb cb, void *user);
  /** The app's checks of a new image on trial (see g9ota_ops.trial_check). NULL: none. */
  int (*trial_check)(g9ota_check_cb cb, void *user);
  /** Work queue that runs the requests, the callbacks of the link and the housekeeping timer. Needed. */
  struct k_work_q *wq;
};

/** Load the stored trial state, set the slot line and start the flow. Call after gap9_link_init(). The configuration
 * must stay valid. */
int sensei_gap9_ota_init(const struct sensei_gap9_ota_cfg *cfg);

/** The flow, for the host requests: g9ota_flow_begin(), g9ota_flow_chunk() and g9ota_flow_command() (see g9ota_flow.h
 * for the results). Call them on the work queue of the configuration; the housekeeping timer starts itself. */
struct g9ota_flow *sensei_gap9_ota_flow(void);

/** Slot GAP9 boots from now (the confirmed one, or the one on trial), and the image id of the running image (0 if not
 * known). */
uint8_t sensei_gap9_ota_slot(void);
uint32_t sensei_gap9_ota_image_id(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_GAP9_GAP9_OTA_ZEPHYR_H_ */
