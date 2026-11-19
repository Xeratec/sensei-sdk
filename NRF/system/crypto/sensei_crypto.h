/*
 * ----------------------------------------------------------------------
 *
 * File: sensei_crypto.h
 *
 * Copyright (C) 2025 ETH Zurich and University of Bologna
 *
 * Based on the semester project of Ahmed Malanovic (ETH Zurich, 2025).
 *
 * ----------------------------------------------------------------------
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SENSEI_CRYPTO_H_
#define SENSEI_CRYPTO_H_

/**
 * @file sensei_crypto.h
 * @brief Simplified crypto library of the SENSEI SDK (thin layer over the PSA Crypto API).
 *
 * Features are enabled with CONFIG_SENSEI_CRYPTO and the sub-options
 * CONFIG_SENSEI_CRYPTO_{AEAD,ECDH,X25519,ECDSA,HKDF,HMAC}. Declarations of a feature are only
 * visible if the corresponding option is enabled.
 *
 * Keys are never stored in the library. Functions that create a key return a PSA key id
 * that the caller owns and must release with sensei_crypto_destroy_key().
 * Private keys cannot be exported.
 *
 * All functions are thread-safe to the extent PSA Crypto is. Call sensei_crypto_init()
 * once before using anything else.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SHA-256 digest size in bytes. */
#define SENSEI_CRYPTO_SHA256_SIZE 32
/** AES-GCM nonce size in bytes. */
#define SENSEI_CRYPTO_AES_GCM_NONCE_SIZE 12
/** AES-GCM authentication tag size in bytes (appended to the ciphertext). */
#define SENSEI_CRYPTO_AES_GCM_TAG_SIZE 16
/** P-256 private key (raw scalar) size in bytes. */
#define SENSEI_CRYPTO_P256_PRIVATE_KEY_SIZE 32
/** P-256 public key size in bytes (uncompressed point 0x04 || X || Y). */
#define SENSEI_CRYPTO_P256_PUBLIC_KEY_SIZE 65
/** X25519 private key, public key and shared secret size in bytes. */
#define SENSEI_CRYPTO_X25519_SIZE 32
/** ECDH P-256 shared secret size in bytes. */
#define SENSEI_CRYPTO_ECDH_SECRET_SIZE 32
/** ECDSA P-256 signature size in bytes (raw r || s). */
#define SENSEI_CRYPTO_ECDSA_SIGNATURE_SIZE 64
/** Maximum HKDF-SHA256 output size in bytes (255 * hash size). */
#define SENSEI_CRYPTO_HKDF_MAX_OUTPUT_SIZE (255 * SENSEI_CRYPTO_SHA256_SIZE)

/** Key handle (PSA key id). Zero is never a valid key. */
typedef uint32_t sensei_crypto_key_id_t;

/** Status codes returned by all library functions. */
typedef enum {
  SENSEI_CRYPTO_OK = 0,
  SENSEI_CRYPTO_ERR_INVALID_ARG = -1,      /**< NULL pointer, bad length or bad key. */
  SENSEI_CRYPTO_ERR_NOT_SUPPORTED = -2,    /**< Not enabled in Kconfig or unsupported. */
  SENSEI_CRYPTO_ERR_BUFFER_TOO_SMALL = -3, /**< Output buffer too small. */
  SENSEI_CRYPTO_ERR_BAD_STATE = -4,        /**< sensei_crypto_init() not called. */
  SENSEI_CRYPTO_ERR_AUTH_FAILED = -5,      /**< AEAD tag or signature verification failed. */
  SENSEI_CRYPTO_ERR_NO_MEMORY = -6,        /**< Out of memory (see MBEDTLS_HEAP_SIZE). */
  SENSEI_CRYPTO_ERR_NOT_FOUND = -7,        /**< Unknown key id. */
  SENSEI_CRYPTO_ERR_NOT_PERMITTED = -8,    /**< Key usage policy forbids the operation. */
  SENSEI_CRYPTO_ERR_HARDWARE = -9,         /**< Hardware or entropy source failure. */
  SENSEI_CRYPTO_ERR_GENERIC = -10          /**< Any other failure. */
} sensei_crypto_status_t;

/**
 * @brief Initialize the crypto subsystem. Safe to call more than once.
 * @return SENSEI_CRYPTO_OK on success.
 */
sensei_crypto_status_t sensei_crypto_init(void);

/**
 * @brief Fill a buffer with cryptographically secure random bytes.
 * @param[out] out  Output buffer.
 * @param[in]  len  Number of bytes.
 */
sensei_crypto_status_t sensei_crypto_random(uint8_t *out, size_t len);

/**
 * @brief Destroy a key created by this library and free its resources.
 * @param[in] key  Key id.
 */
sensei_crypto_status_t sensei_crypto_destroy_key(sensei_crypto_key_id_t key);

/**
 * @brief Compute the SHA-256 digest of a message.
 * @param[in]  msg      Message (may be NULL if msg_len is 0).
 * @param[in]  msg_len  Message length in bytes.
 * @param[out] digest   Output, SENSEI_CRYPTO_SHA256_SIZE bytes.
 */
sensei_crypto_status_t sensei_crypto_sha256(const uint8_t *msg, size_t msg_len,
                                            uint8_t digest[SENSEI_CRYPTO_SHA256_SIZE]);

#if defined(CONFIG_SENSEI_CRYPTO_HMAC) || defined(__DOXYGEN__)
/**
 * @brief Compute HMAC-SHA256.
 * @param[in]  key      MAC key (not stored, must not be empty).
 * @param[in]  key_len  Key length in bytes.
 * @param[in]  msg      Message (may be NULL if msg_len is 0).
 * @param[in]  msg_len  Message length in bytes.
 * @param[out] mac      Output, SENSEI_CRYPTO_SHA256_SIZE bytes.
 */
sensei_crypto_status_t sensei_crypto_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                                                 uint8_t mac[SENSEI_CRYPTO_SHA256_SIZE]);
#endif

#if defined(CONFIG_SENSEI_CRYPTO_HKDF) || defined(__DOXYGEN__)
/**
 * @brief HKDF-SHA256 (RFC 5869) extract-and-expand.
 * @param[in]  ikm       Input keying material (e.g. an ECDH shared secret).
 * @param[in]  ikm_len   Length of @p ikm.
 * @param[in]  salt      Optional salt (NULL or empty means a string of zeros).
 * @param[in]  salt_len  Length of @p salt.
 * @param[in]  info      Optional context (may be NULL).
 * @param[in]  info_len  Length of @p info.
 * @param[out] okm       Output keying material.
 * @param[in]  okm_len   Requested length, at most SENSEI_CRYPTO_HKDF_MAX_OUTPUT_SIZE.
 */
sensei_crypto_status_t sensei_crypto_hkdf_sha256(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt,
                                                 size_t salt_len, const uint8_t *info, size_t info_len, uint8_t *okm,
                                                 size_t okm_len);
#endif

#if defined(CONFIG_SENSEI_CRYPTO_AEAD) || defined(__DOXYGEN__)
/**
 * @brief Import an AES key for AES-GCM.
 * @param[in]  key      Raw key (16 or 32 bytes).
 * @param[in]  key_len  Key length in bytes.
 * @param[out] key_id   Receives the key id.
 */
sensei_crypto_status_t sensei_crypto_aead_import_key(const uint8_t *key, size_t key_len,
                                                     sensei_crypto_key_id_t *key_id);

/**
 * @brief Generate a random non-exportable AES-GCM key.
 * @param[in]  key_bits  128 or 256.
 * @param[out] key_id    Receives the key id.
 */
sensei_crypto_status_t sensei_crypto_aead_generate_key(size_t key_bits, sensei_crypto_key_id_t *key_id);

/**
 * @brief Generate a random AES-GCM nonce.
 *
 * A nonce must never be reused with the same key. Random 96-bit nonces are safe for
 * up to about 2^32 messages per key.
 */
sensei_crypto_status_t sensei_crypto_aead_generate_nonce(uint8_t nonce[SENSEI_CRYPTO_AES_GCM_NONCE_SIZE]);

/**
 * @brief AES-GCM encrypt. The output is ciphertext followed by the 16 byte tag.
 * @param[in]  key_id     Key id.
 * @param[in]  nonce      Nonce, SENSEI_CRYPTO_AES_GCM_NONCE_SIZE bytes.
 * @param[in]  aad        Additional authenticated data (may be NULL if aad_len is 0).
 * @param[in]  aad_len    Length of @p aad.
 * @param[in]  pt         Plaintext (may be NULL if pt_len is 0).
 * @param[in]  pt_len     Length of @p pt.
 * @param[out] ct         Output buffer, needs pt_len + SENSEI_CRYPTO_AES_GCM_TAG_SIZE bytes.
 * @param[in]  ct_size    Size of @p ct.
 * @param[out] ct_len     Receives the number of bytes written.
 */
sensei_crypto_status_t sensei_crypto_aead_encrypt(sensei_crypto_key_id_t key_id,
                                                  const uint8_t nonce[SENSEI_CRYPTO_AES_GCM_NONCE_SIZE],
                                                  const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len,
                                                  uint8_t *ct, size_t ct_size, size_t *ct_len);

/**
 * @brief AES-GCM decrypt and verify (input is ciphertext followed by the tag).
 * @return SENSEI_CRYPTO_ERR_AUTH_FAILED if the tag does not match; no plaintext is released.
 * @param[in]  key_id     Key id.
 * @param[in]  nonce      Nonce, SENSEI_CRYPTO_AES_GCM_NONCE_SIZE bytes.
 * @param[in]  aad        Additional authenticated data (may be NULL if aad_len is 0).
 * @param[in]  aad_len    Length of @p aad.
 * @param[in]  ct         Ciphertext with appended tag.
 * @param[in]  ct_len     Length of @p ct (at least the tag size).
 * @param[out] pt         Output buffer, needs ct_len - SENSEI_CRYPTO_AES_GCM_TAG_SIZE bytes.
 * @param[in]  pt_size    Size of @p pt.
 * @param[out] pt_len     Receives the number of bytes written.
 */
sensei_crypto_status_t sensei_crypto_aead_decrypt(sensei_crypto_key_id_t key_id,
                                                  const uint8_t nonce[SENSEI_CRYPTO_AES_GCM_NONCE_SIZE],
                                                  const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len,
                                                  uint8_t *pt, size_t pt_size, size_t *pt_len);
#endif

#if defined(CONFIG_SENSEI_CRYPTO_ECDH) || defined(__DOXYGEN__)
/** @brief Generate an ECDH P-256 key pair. */
sensei_crypto_status_t sensei_crypto_ecdh_generate_keypair(sensei_crypto_key_id_t *key_id);

/**
 * @brief Import a raw ECDH P-256 private key (32 bytes, big endian).
 *
 * Intended for keys provisioned at manufacturing time and for known-answer tests.
 */
sensei_crypto_status_t sensei_crypto_ecdh_import_private_key(const uint8_t *priv, size_t priv_len,
                                                             sensei_crypto_key_id_t *key_id);

/**
 * @brief Export the public key (uncompressed point, 65 bytes).
 * @param[in]  key_id    ECDH key pair.
 * @param[out] pub       Output buffer.
 * @param[in]  pub_size  Size of @p pub.
 * @param[out] pub_len   Receives the number of bytes written.
 */
sensei_crypto_status_t sensei_crypto_ecdh_export_public_key(sensei_crypto_key_id_t key_id, uint8_t *pub,
                                                            size_t pub_size, size_t *pub_len);

/**
 * @brief Compute the raw ECDH shared secret (X coordinate, 32 bytes).
 *
 * The raw secret is not uniformly random. Feed it through sensei_crypto_hkdf_sha256()
 * before using it as a key.
 *
 * @param[in]  key_id      Own ECDH key pair.
 * @param[in]  peer_pub    Peer public key (uncompressed point).
 * @param[in]  peer_len    Length of @p peer_pub.
 * @param[out] secret      Output buffer.
 * @param[in]  secret_size Size of @p secret (at least SENSEI_CRYPTO_ECDH_SECRET_SIZE).
 * @param[out] secret_len  Receives the number of bytes written.
 */
sensei_crypto_status_t sensei_crypto_ecdh_shared_secret(sensei_crypto_key_id_t key_id, const uint8_t *peer_pub,
                                                        size_t peer_len, uint8_t *secret, size_t secret_size,
                                                        size_t *secret_len);
#endif

#if defined(CONFIG_SENSEI_CRYPTO_X25519) || defined(__DOXYGEN__)
/**
 * @brief X25519 (RFC 7748): scalar multiplication of a u coordinate.
 *
 * The scalar is clamped as the RFC says and the top bit of the u coordinate is ignored, so any 32 bytes are valid. With
 * the base point u = 9 the result is the public key. If the peer has low order the result is all zero (or the call
 * fails): check it before you use it, and feed it through sensei_crypto_hkdf_sha256().
 *
 * @param[in]  priv  Private scalar, SENSEI_CRYPTO_X25519_SIZE bytes (the input is not changed).
 * @param[in]  peer  Peer u coordinate, SENSEI_CRYPTO_X25519_SIZE bytes, little endian.
 * @param[out] out   Result, SENSEI_CRYPTO_X25519_SIZE bytes.
 */
sensei_crypto_status_t sensei_crypto_x25519(const uint8_t priv[SENSEI_CRYPTO_X25519_SIZE],
                                            const uint8_t peer[SENSEI_CRYPTO_X25519_SIZE],
                                            uint8_t out[SENSEI_CRYPTO_X25519_SIZE]);

/** @brief New X25519 key: 32 random bytes as private key and the public key X25519(priv, 9). */
sensei_crypto_status_t sensei_crypto_x25519_keygen(uint8_t priv[SENSEI_CRYPTO_X25519_SIZE],
                                                   uint8_t pub[SENSEI_CRYPTO_X25519_SIZE]);
#endif

#if defined(CONFIG_SENSEI_CRYPTO_ECDSA) || defined(__DOXYGEN__)
/** @brief Generate an ECDSA P-256 key pair. */
sensei_crypto_status_t sensei_crypto_ecdsa_generate_keypair(sensei_crypto_key_id_t *key_id);

/** @brief Import a raw ECDSA P-256 private key (32 bytes, big endian). */
sensei_crypto_status_t sensei_crypto_ecdsa_import_private_key(const uint8_t *priv, size_t priv_len,
                                                              sensei_crypto_key_id_t *key_id);

/** @brief Import an ECDSA P-256 public key (uncompressed point, 65 bytes) for verification only. */
sensei_crypto_status_t sensei_crypto_ecdsa_import_public_key(const uint8_t *pub, size_t pub_len,
                                                             sensei_crypto_key_id_t *key_id);

/** @brief Export the public key (uncompressed point, 65 bytes). */
sensei_crypto_status_t sensei_crypto_ecdsa_export_public_key(sensei_crypto_key_id_t key_id, uint8_t *pub,
                                                             size_t pub_size, size_t *pub_len);

/**
 * @brief Sign a precomputed SHA-256 digest with ECDSA P-256.
 * @param[in]  key_id    Key pair.
 * @param[in]  hash      Digest, SENSEI_CRYPTO_SHA256_SIZE bytes.
 * @param[in]  hash_len  Length of @p hash.
 * @param[out] sig       Output buffer (raw r || s).
 * @param[in]  sig_size  Size of @p sig (at least SENSEI_CRYPTO_ECDSA_SIGNATURE_SIZE).
 * @param[out] sig_len   Receives the number of bytes written.
 */
sensei_crypto_status_t sensei_crypto_ecdsa_sign_hash(sensei_crypto_key_id_t key_id, const uint8_t *hash,
                                                     size_t hash_len, uint8_t *sig, size_t sig_size, size_t *sig_len);

/**
 * @brief Verify an ECDSA P-256 signature over a SHA-256 digest.
 * @return SENSEI_CRYPTO_OK if valid, SENSEI_CRYPTO_ERR_AUTH_FAILED if the signature is invalid.
 */
sensei_crypto_status_t sensei_crypto_ecdsa_verify_hash(sensei_crypto_key_id_t key_id, const uint8_t *hash,
                                                       size_t hash_len, const uint8_t *sig, size_t sig_len);

/** @brief Hash a message with SHA-256 and sign the digest (see sensei_crypto_ecdsa_sign_hash()). */
sensei_crypto_status_t sensei_crypto_ecdsa_sign(sensei_crypto_key_id_t key_id, const uint8_t *msg, size_t msg_len,
                                                uint8_t *sig, size_t sig_size, size_t *sig_len);

/** @brief Hash a message with SHA-256 and verify the signature (see sensei_crypto_ecdsa_verify_hash()). */
sensei_crypto_status_t sensei_crypto_ecdsa_verify(sensei_crypto_key_id_t key_id, const uint8_t *msg, size_t msg_len,
                                                  const uint8_t *sig, size_t sig_len);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_CRYPTO_H_ */
