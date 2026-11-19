/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_commission_settings.h
 * @brief Zephyr settings storage of the commissioning (CONFIG_SENSEI_SECURE_COMMISSION with
 *        CONFIG_SENSEI_SECURE_SETTINGS).
 *
 * Two records under CONFIG_SENSEI_SECURE_SETTINGS_PREFIX (the keys of sensei_e2e_settings.h):
 *
 *   e2e_cmv  the verifier record (sensei_commission.h, 121 B). With CONFIG_SENSEI_SECURE_SETTINGS_SEAL it is a sealed
 *            record of sensei_e2e_seal.h (150 B, name "e2e_cmv" in the associated data), like the private key: a flash
 *            image or a stale page does not give the verifier up, and a verifier does not open under another chip.
 *            Without sealing it is stored plain only if CONFIG_SENSEI_SECURE_COMMISSION_PLAIN_VERIFIER (simulators):
 *            else store returns -ENOTSUP. A record of the other size is not read (-EBADMSG).
 *   e2e_cmf  the attempt counters, 8 B plain (u32 fails_total, u32 fails_since_ok, little endian). Not secret, and it
 *            must be readable without the sealing key.
 *
 * sensei_e2e_reset() (sensei_e2e_settings_ops.erase) erases both. All calls run on the thread that uses the key store.
 */

#ifndef SENSEI_COMMISSION_SETTINGS_H_
#define SENSEI_COMMISSION_SETTINGS_H_

#include "sensei_commission.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Read and open the verifier record. 0: *rec is filled. -ENOENT: none stored. -EACCES: stored, but it does not open
 * (another chip, changed, wrong name) or the sealing key is not available (the error of the provider is returned for
 * that). -EBADMSG: wrong size, or the content is not an acceptable record (sensei_commission_record_decode()). Another
 * negative errno: the settings subsystem failed. *rec is not written on an error. */
int sensei_commission_settings_verifier_load(struct sensei_commission_record *rec);

/** Check the record (as sensei_commission_record_encode()), seal it and store it. 0, -EINVAL (not an acceptable
 * record), -ENOTSUP (sealing is off and plain verifiers are not allowed), the error of the sealing key provider (what
 * the node calls AT_REST_FAILED), or the error of the write. Nothing is stored on an error: an older record stays. */
int sensei_commission_settings_verifier_store(const struct sensei_commission_record *rec);

/** Delete the verifier record. 0 also if there is none. */
int sensei_commission_settings_verifier_erase(void);

/** Read the counters. 0: *cnt is filled, zeros if none is stored. -EBADMSG: a record of the wrong size: the counters
 * are set to the maximum (locked) so that a damaged record does not give attempts back; commission_clear writes it
 * anew. Another negative errno: the settings subsystem failed (counters are set to the maximum too). */
int sensei_commission_settings_counters_load(struct sensei_commission_counters *cnt);

/** Store the counters (one atomic settings write). Has the signature of the save_counters hook of
 * struct sensei_commission_verifier_cfg: pass it with user NULL. */
int sensei_commission_settings_counters_save(void *user, const struct sensei_commission_counters *cnt);

/** Delete the counters record (equal to zeros). 0 also if there is none. */
int sensei_commission_settings_counters_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_COMMISSION_SETTINGS_H_ */
