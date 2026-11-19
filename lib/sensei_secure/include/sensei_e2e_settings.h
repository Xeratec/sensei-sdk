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
 * Keys under CONFIG_SENSEI_SECURE_SETTINGS_PREFIX: e2e_priv (private key, plain), e2e_bpub (collector public
 * key), e2e_tx (high-water mark of the sent counters), e2e_rxs (table of senders), e2e_rxf (floor of the
 * downlink counters).
 *
 * With CONFIG_SENSEI_SECURE_SETTINGS_SEAL the private key is stored as the sealed record e2e_privx (format in
 * sensei_e2e_seal.h) under a key from sensei_e2e_settings_seal_key(); the HUK provider derives it from the
 * hardware unique key of the nRF5340. A plain e2e_priv of an earlier version is sealed at the first start but stays
 * until the application confirms the new image (sensei_e2e_settings_confirm()), so a revert to the old image finds its
 * key. The name e2e_rat is reserved for the ratchet state of a later version.
 * Without sealing the private key is plain in flash: protect the chip (APPROTECT) in the field. With sealing
 * a flash image copied to another chip, and stale settings pages, do not give up the key; code that runs on the
 * chip still can use it (APPROTECT stays the main barrier).
 */

#ifndef SENSEI_E2E_SETTINGS_H_
#define SENSEI_E2E_SETTINGS_H_

#include <stdbool.h>

#include "sensei_e2e.h"
#include "sensei_e2e_seal.h"

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
 *
 * With sealing the key record is opened here, and a plain e2e_priv of an earlier version is migrated:
 * sealed to e2e_privx, read back and compared (the key and its key id stay the same). The plain record is kept: if the
 * new image is only on trial and MCUboot goes back to the old one, the old image still has its key. The application
 * calls sensei_e2e_settings_confirm() once the image is confirmed, and that deletes it (the old flash page stays
 * readable until the settings garbage collection). If both records are there: the same key (a trial image that is not
 * confirmed yet, or the migration stopped): both stay; different keys (an older firmware made a new key after the
 * downgrade): the plain one is the newer, it is sealed over the old one; a plain record that is not a valid scalar is
 * junk: the sealed key stands. A plain record that is not a valid scalar and no sealed record: the key is damaged, it
 * is not sealed, sensei_e2e_init() replaces it. If the sealing key is not available or the record does not open,
 * nv->key_unreadable is set, nv->have_priv is false and the
 * function returns the error (-EACCES for a record that does not open, the error of the provider
 * otherwise). sensei_e2e_init() then refuses to start. The stored record is not touched.
 */
int sensei_e2e_settings_load(struct sensei_e2e_nv *nv);

/* ---- sealing (CONFIG_SENSEI_SECURE_SETTINGS_SEAL; without it these return -ENOTSUP and sealed() is false) ---- */

/** True if the sealing is built in. */
bool sensei_e2e_settings_sealed(void);

/**
 * The sealing key provider: write the 16 B AES-128 key. Returns 0 or a negative errno. Defined by the HUK
 * provider (zephyr/sensei_e2e_huk.c), the test provider (sensei_e2e_testkey.c) or the application. Called
 * from the thread that uses the key store, once per start (the key is then held as a volatile PSA key and the
 * copy in RAM is wiped).
 */
int sensei_e2e_settings_seal_key(uint8_t key[SENSEI_SECURE_KEY_SIZE]);

/**
 * 0 if the sealing key is available (it is fetched on the first call), else the error of the provider.
 * A failure is remembered until sensei_e2e_settings_seal_close().
 */
int sensei_e2e_settings_seal_status(void);

/**
 * Optional: call it once, early (in main() before other threads write to flash), so the provider can do its one-time
 * setup. The HUK provider writes the hardware unique key here if there is none (hw_unique_key_write_random() writes
 * the UICR through the NVMC without the flash driver lock, so no other flash write may run at the same time). Without
 * this call the first use does it. Returns 0 or the error of the provider; providers that need no setup return 0.
 */
int sensei_e2e_settings_seal_prepare(void);

/**
 * Call before sensei_e2e_reset(): with sealing, forgets a cached failure, asks the provider again and returns 0 only
 * if a new key can be sealed, else its error. reset erases the stored key first, so a reset while the sealing key is
 * missing would leave no key at all. Without sealing it returns 0.
 */
int sensei_e2e_settings_reset_check(void);

/**
 * Call once the running image is confirmed (for MCUboot: after boot_write_img_confirmed() returned 0; calling it at
 * every start after the confirmation is fine, it does nothing when there is no plain record). Deletes the plain
 * e2e_priv that the migration kept. Not called before the confirmation: an image that MCUboot reverts must find its
 * key. Returns 0 (also when there is nothing to delete) or the error of the delete: the record stays and the next
 * call tries again. Call it after sensei_e2e_settings_load() and from the thread that uses the key store. Without
 * sealing it returns 0.
 */
int sensei_e2e_settings_confirm(void);

/** True if sensei_e2e_settings_load() found a plain e2e_priv that sensei_e2e_settings_confirm() will delete. */
bool sensei_e2e_settings_plain_pending(void);

/** Destroy the key handle and forget a failure: the next use asks the provider again (a "reboot" in tests). */
void sensei_e2e_settings_seal_close(void);

/** Seal pt as the record of setting `name` (without the prefix). Returns the record length (len +
 * SENSEI_E2E_SEAL_OVERHEAD) or a negative errno (see sensei_e2e_seal_record()). */
int sensei_e2e_settings_seal(const char *name, const uint8_t *pt, size_t len, uint8_t *out, size_t cap);

/** Open a record of setting `name`. Returns the plain length or a negative errno (-EACCES: does not open). */
int sensei_e2e_settings_unseal(const char *name, const uint8_t *rec, size_t len, uint8_t *out, size_t cap);

/** Test provider (CONFIG_SENSEI_SECURE_SETTINGS_SEAL_TEST_KEY): use this 16 B key, or NULL to make the provider
 * fail. The default is a fixed key. Call sensei_e2e_settings_seal_close() afterwards. */
void sensei_e2e_testkey_set(const uint8_t *key);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_E2E_SETTINGS_H_ */
