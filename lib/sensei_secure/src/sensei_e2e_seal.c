/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_e2e_seal.h"

#include <errno.h>
#include <string.h>

#define AAD_PREFIX "sensei-e2e/"
#define AAD_PREFIX_LEN (sizeof(AAD_PREFIX) - 1)

/* AAD = prefix || name || version. Returns its length, or 0 for a bad name. */
static size_t make_aad(const char *name, uint8_t *aad) {
  size_t n = name ? strlen(name) : 0;

  if (n == 0 || n > SENSEI_E2E_SEAL_NAME_MAX) {
    return 0;
  }
  memcpy(aad, AAD_PREFIX, AAD_PREFIX_LEN);
  memcpy(aad + AAD_PREFIX_LEN, name, n);
  aad[AAD_PREFIX_LEN + n] = SENSEI_E2E_SEAL_VERSION;
  return AAD_PREFIX_LEN + n + 1;
}

int sensei_e2e_seal_record(const struct sensei_secure_crypto *c, sensei_secure_key_t key, const char *name,
                           const uint8_t *nonce, const uint8_t *pt, size_t len, uint8_t *out, size_t cap) {
  uint8_t aad[AAD_PREFIX_LEN + SENSEI_E2E_SEAL_NAME_MAX + 1];
  size_t aad_len = make_aad(name, aad);
  size_t n = 0;

  if (c == NULL || aad_len == 0 || (pt == NULL && len != 0) || out == NULL) {
    return -EINVAL;
  }
  if (cap < len + SENSEI_E2E_SEAL_OVERHEAD) {
    return -ENOSPC;
  }
  out[0] = SENSEI_E2E_SEAL_VERSION;
  if (nonce != NULL) {
    memcpy(out + 1, nonce, SENSEI_E2E_SEAL_NONCE_SIZE);
  } else if (c->random(c->ctx, out + 1, SENSEI_E2E_SEAL_NONCE_SIZE) != 0) {
    return -EIO;
  }
  if (c->aead_encrypt(c->ctx, key, out + 1, aad, aad_len, pt, len, out + 1 + SENSEI_E2E_SEAL_NONCE_SIZE,
                      cap - 1 - SENSEI_E2E_SEAL_NONCE_SIZE, &n) != 0 ||
      n != len + SENSEI_SECURE_TAG_SIZE) {
    return -EIO;
  }
  return (int)(1 + SENSEI_E2E_SEAL_NONCE_SIZE + n);
}

int sensei_e2e_open_record(const struct sensei_secure_crypto *c, sensei_secure_key_t key, const char *name,
                           const uint8_t *rec, size_t len, uint8_t *out, size_t cap) {
  uint8_t aad[AAD_PREFIX_LEN + SENSEI_E2E_SEAL_NAME_MAX + 1];
  size_t aad_len = make_aad(name, aad);
  size_t n = 0;

  if (c == NULL || aad_len == 0 || rec == NULL || (out == NULL && cap != 0)) {
    return -EINVAL;
  }
  if (len < SENSEI_E2E_SEAL_OVERHEAD || rec[0] != SENSEI_E2E_SEAL_VERSION) {
    return -EBADMSG;
  }
  if (cap < len - SENSEI_E2E_SEAL_OVERHEAD) {
    return -ENOSPC;
  }
  if (c->aead_decrypt(c->ctx, key, rec + 1, aad, aad_len, rec + 1 + SENSEI_E2E_SEAL_NONCE_SIZE,
                      len - 1 - SENSEI_E2E_SEAL_NONCE_SIZE, out, cap, &n) != 0) {
    return -EACCES;
  }
  return (int)n;
}
