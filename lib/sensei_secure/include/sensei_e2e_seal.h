/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_e2e_seal.h
 * @brief Sealed record format for secret settings (keys at rest). No Zephyr, no allocation; the key is an
 *        AES-128 key handle of the cipher backend. The Zephyr settings storage that uses it is in
 *        sensei_e2e_settings.h.
 *
 * Record: u8 version (1) | nonce[12] | AES-128-GCM ciphertext | tag[16]. 29 B more than the plain value.
 * Associated data: "sensei-e2e/" || setting name || version [|| context]. The name binds a record to its place: a
 * record copied to another name does not open. The context (up to 8 bytes, optional) binds it to more: the ratchet
 * state e2e_rat carries the key id of the key it belongs to. The Python mirror is sensei_sdk.secure.seal_record().
 */

#ifndef SENSEI_E2E_SEAL_H_
#define SENSEI_E2E_SEAL_H_

#include <stddef.h>
#include <stdint.h>

#include "sensei_secure.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSEI_E2E_SEAL_VERSION 1u
#define SENSEI_E2E_SEAL_NONCE_SIZE 12u
/** Version byte, nonce and tag. */
#define SENSEI_E2E_SEAL_OVERHEAD (1u + SENSEI_E2E_SEAL_NONCE_SIZE + SENSEI_SECURE_TAG_SIZE)
/** Longest setting name (without the prefix and the slash). */
#define SENSEI_E2E_SEAL_NAME_MAX 32u
/** Longest context that goes into the associated data after the version. */
#define SENSEI_E2E_SEAL_CTX_MAX 8u

/**
 * Seal len bytes of pt under key into out (len + SENSEI_E2E_SEAL_OVERHEAD bytes; out must not overlap pt).
 * nonce: 12 bytes, or NULL to take them from the random generator of c (the only use outside tests: a
 * nonce must never repeat under one key). Returns the record length, -EINVAL (name too long or empty,
 * bad argument), -ENOSPC (cap too small) or -EIO (cipher or random generator failed).
 */
int sensei_e2e_seal_record(const struct sensei_secure_crypto *c, sensei_secure_key_t key, const char *name,
                           const uint8_t *nonce, const uint8_t *pt, size_t len, uint8_t *out, size_t cap);

/**
 * Open a record. Returns the plain length, -EBADMSG (too short, unknown version), -EACCES (tag: wrong key,
 * wrong name, tampered), -ENOSPC (cap too small), -EINVAL or -EIO. No plain bytes are written if the tag
 * fails.
 */
int sensei_e2e_open_record(const struct sensei_secure_crypto *c, sensei_secure_key_t key, const char *name,
                           const uint8_t *rec, size_t len, uint8_t *out, size_t cap);

/** As sensei_e2e_seal_record(), with ctx_len bytes of context appended to the associated data (-EINVAL if more than
 * SENSEI_E2E_SEAL_CTX_MAX). With no context it is the same record. */
int sensei_e2e_seal_record_ctx(const struct sensei_secure_crypto *c, sensei_secure_key_t key, const char *name,
                               const uint8_t *ctx, size_t ctx_len, const uint8_t *nonce, const uint8_t *pt, size_t len,
                               uint8_t *out, size_t cap);

/** As sensei_e2e_open_record(), with the context the record was sealed with. */
int sensei_e2e_open_record_ctx(const struct sensei_secure_crypto *c, sensei_secure_key_t key, const char *name,
                               const uint8_t *ctx, size_t ctx_len, const uint8_t *rec, size_t len, uint8_t *out,
                               size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_E2E_SEAL_H_ */
