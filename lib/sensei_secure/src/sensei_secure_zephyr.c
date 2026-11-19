/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Device backend on the SENSEI SDK crypto library (PSA Crypto). No allocation. */

#include "crypto/sensei_crypto.h"
#include "sensei_secure.h"

static int c_random(void *ctx, uint8_t *out, size_t len) {
  (void)ctx;
  return sensei_crypto_random(out, len) != SENSEI_CRYPTO_OK;
}

static int c_ecdh_import(void *ctx, const uint8_t *priv, size_t len, sensei_secure_key_t *key) {
  sensei_crypto_key_id_t id = 0;

  (void)ctx;
  if (sensei_crypto_ecdh_import_private_key(priv, len, &id) != SENSEI_CRYPTO_OK) {
    return -1;
  }
  *key = id;
  return 0;
}

static int c_ecdh_public(void *ctx, sensei_secure_key_t key, uint8_t *pub) {
  size_t n = 0;

  (void)ctx;
  if (sensei_crypto_ecdh_export_public_key((sensei_crypto_key_id_t)key, pub, SENSEI_CRYPTO_P256_PUBLIC_KEY_SIZE, &n) !=
      SENSEI_CRYPTO_OK) {
    return -1;
  }
  return n == SENSEI_CRYPTO_P256_PUBLIC_KEY_SIZE ? 0 : -1;
}

static int c_ecdh_shared(void *ctx, sensei_secure_key_t key, const uint8_t *peer_pub, uint8_t *secret) {
  size_t n = 0;

  (void)ctx;
  if (sensei_crypto_ecdh_shared_secret((sensei_crypto_key_id_t)key, peer_pub, SENSEI_CRYPTO_P256_PUBLIC_KEY_SIZE,
                                       secret, SENSEI_CRYPTO_ECDH_SECRET_SIZE, &n) != SENSEI_CRYPTO_OK) {
    return -1;
  }
  return n == SENSEI_CRYPTO_ECDH_SECRET_SIZE ? 0 : -1;
}

static int c_hkdf(void *ctx, const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                  const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len) {
  (void)ctx;
  return sensei_crypto_hkdf_sha256(ikm, ikm_len, salt, salt_len, info, info_len, okm, okm_len) != SENSEI_CRYPTO_OK;
}

static int c_aead_import(void *ctx, const uint8_t *key, sensei_secure_key_t *handle) {
  sensei_crypto_key_id_t id = 0;

  (void)ctx;
  if (sensei_crypto_aead_import_key(key, SENSEI_SECURE_KEY_SIZE, &id) != SENSEI_CRYPTO_OK) {
    return -1;
  }
  *handle = id;
  return 0;
}

static int c_key_destroy(void *ctx, sensei_secure_key_t key) {
  (void)ctx;
  return sensei_crypto_destroy_key((sensei_crypto_key_id_t)key) != SENSEI_CRYPTO_OK;
}

static int c_aead_encrypt(void *ctx, sensei_secure_key_t key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                          const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len) {
  (void)ctx;
  return sensei_crypto_aead_encrypt((sensei_crypto_key_id_t)key, nonce, aad, aad_len, in, in_len, out, out_cap,
                                    out_len) != SENSEI_CRYPTO_OK;
}

static int c_aead_decrypt(void *ctx, sensei_secure_key_t key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                          const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len) {
  (void)ctx;
  return sensei_crypto_aead_decrypt((sensei_crypto_key_id_t)key, nonce, aad, aad_len, in, in_len, out, out_cap,
                                    out_len) != SENSEI_CRYPTO_OK;
}

static int c_hmac_sha256(void *ctx, const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                         uint8_t out[32]) {
  (void)ctx;
  return sensei_crypto_hmac_sha256(key, key_len, msg, msg_len, out) != SENSEI_CRYPTO_OK;
}

#ifdef CONFIG_SENSEI_SECURE_RATCHET
static int c_x25519_keygen(void *ctx, uint8_t priv[32], uint8_t pub[32]) {
  (void)ctx;
  return sensei_crypto_x25519_keygen(priv, pub) != SENSEI_CRYPTO_OK;
}

static int c_x25519(void *ctx, const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32]) {
  (void)ctx;
  return sensei_crypto_x25519(priv, peer, out) != SENSEI_CRYPTO_OK;
}
#endif

const struct sensei_secure_crypto *sensei_secure_crypto_zephyr(void) {
  static const struct sensei_secure_crypto c = {
      .ctx = NULL,
      .random = c_random,
      .ecdh_import = c_ecdh_import,
      .ecdh_public = c_ecdh_public,
      .ecdh_shared = c_ecdh_shared,
      .hkdf = c_hkdf,
      .aead_import = c_aead_import,
      .key_destroy = c_key_destroy,
      .aead_encrypt = c_aead_encrypt,
      .aead_decrypt = c_aead_decrypt,
      .hmac_sha256 = c_hmac_sha256,
#ifdef CONFIG_SENSEI_SECURE_RATCHET
      .x25519_keygen = c_x25519_keygen,
      .x25519 = c_x25519,
#endif
  };
  static int inited;

  if (!inited) {
    (void)sensei_crypto_init();
    inited = 1;
  }
  return &c;
}
