/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_e2e_settings.h
 * @brief Zephyr settings storage for sensei_e2e (CONFIG_SENSEI_SECURE_SETTINGS).
 *
 * Keys under CONFIG_SENSEI_SECURE_SETTINGS_PREFIX: e2e_priv (private key), e2e_bpub (collector public
 * key), e2e_tx (high-water mark of the sent counters), e2e_rxs (table of senders), e2e_rxf (floor of the
 * downlink counters). The private key is not encrypted at rest: the SDK crypto library has no persistent
 * keys, so protect the chip (APPROTECT) in the field.
 */

#ifndef SENSEI_E2E_SETTINGS_H_
#define SENSEI_E2E_SETTINGS_H_

#include "sensei_e2e.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Storage callbacks for sensei_e2e_init(): each one is a settings_save_one(), erase deletes all keys. */
extern const struct sensei_e2e_ops sensei_e2e_settings_ops;

/**
 * Read the stored state into nv (all empty if nothing is stored). Loads the subtree of the prefix,
 * so it works whether or not the application has called settings_load() already. Call it from one
 * thread (the one that calls sensei_e2e_init()) after settings_subsys_init(). Returns 0 or the error
 * of settings_load_subtree().
 */
int sensei_e2e_settings_load(struct sensei_e2e_nv *nv);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_E2E_SETTINGS_H_ */
