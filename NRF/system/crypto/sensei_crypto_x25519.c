/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <string.h>

#include "sensei_crypto_priv.h"

#define X25519_KEY_TYPE PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY)

/* A plain memset of a buffer that is not read again may be removed by the compiler. */
static void wipe(void *p, size_t n) {
  volatile uint8_t *v = (volatile uint8_t *)p;

  while (n--) {
    *v++ = 0;
  }
}

sensei_crypto_status_t sensei_crypto_x25519(const uint8_t priv[SENSEI_CRYPTO_X25519_SIZE],
                                            const uint8_t peer[SENSEI_CRYPTO_X25519_SIZE],
                                            uint8_t out[SENSEI_CRYPTO_X25519_SIZE]) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  uint8_t scalar[SENSEI_CRYPTO_X25519_SIZE];
  uint8_t u[SENSEI_CRYPTO_X25519_SIZE];
  psa_key_id_t id = 0;
  size_t n = 0;
  psa_status_t st;

  if (priv == NULL || peer == NULL || out == NULL) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  /* RFC 7748 section 5: clamp the scalar and ignore the top bit of u. The driver may insist on a clamped key, and
   * clamping twice changes nothing. */
  memcpy(scalar, priv, sizeof(scalar));
  scalar[0] &= 248;
  scalar[31] &= 127;
  scalar[31] |= 64;
  memcpy(u, peer, sizeof(u));
  u[31] &= 127;

  psa_set_key_type(&attr, X25519_KEY_TYPE);
  psa_set_key_bits(&attr, 255);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
  psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
  st = psa_import_key(&attr, scalar, sizeof(scalar), &id);
  psa_reset_key_attributes(&attr);
  wipe(scalar, sizeof(scalar));
  if (st != PSA_SUCCESS) {
    return sensei_crypto_from_psa(st);
  }
  st = psa_raw_key_agreement(PSA_ALG_ECDH, id, u, sizeof(u), out, SENSEI_CRYPTO_X25519_SIZE, &n);
  (void)psa_destroy_key(id);
  if (st == PSA_SUCCESS && n != SENSEI_CRYPTO_X25519_SIZE) {
    st = PSA_ERROR_GENERIC_ERROR;
  }
  return sensei_crypto_from_psa(st);
}

sensei_crypto_status_t sensei_crypto_x25519_keygen(uint8_t priv[SENSEI_CRYPTO_X25519_SIZE],
                                                   uint8_t pub[SENSEI_CRYPTO_X25519_SIZE]) {
  static const uint8_t base[SENSEI_CRYPTO_X25519_SIZE] = {9};
  sensei_crypto_status_t st;

  if (priv == NULL || pub == NULL) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  st = sensei_crypto_random(priv, SENSEI_CRYPTO_X25519_SIZE);
  if (st == SENSEI_CRYPTO_OK) {
    st = sensei_crypto_x25519(priv, base, pub);
  }
  if (st != SENSEI_CRYPTO_OK) {
    wipe(priv, SENSEI_CRYPTO_X25519_SIZE);
  }
  return st;
}
