/*
 * ----------------------------------------------------------------------
 *
 * File: sensei_crypto_ecdsa.c
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

#include "sensei_crypto_priv.h"

#define ECDSA_ALG PSA_ALG_ECDSA(PSA_ALG_SHA_256)

static void ecdsa_pair_attributes(psa_key_attributes_t *attr) {
  psa_set_key_type(attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(attr, 256);
  psa_set_key_usage_flags(attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH);
  psa_set_key_algorithm(attr, ECDSA_ALG);
}

sensei_crypto_status_t sensei_crypto_ecdsa_generate_keypair(sensei_crypto_key_id_t *key_id) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t id = 0;
  psa_status_t st;

  if (key_id == NULL) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  ecdsa_pair_attributes(&attr);
  st = psa_generate_key(&attr, &id);
  psa_reset_key_attributes(&attr);
  if (st == PSA_SUCCESS) {
    *key_id = id;
  }
  return sensei_crypto_from_psa(st);
}

sensei_crypto_status_t sensei_crypto_ecdsa_import_private_key(const uint8_t *priv, size_t priv_len,
                                                              sensei_crypto_key_id_t *key_id) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t id = 0;
  psa_status_t st;

  if (priv == NULL || key_id == NULL || priv_len != SENSEI_CRYPTO_P256_PRIVATE_KEY_SIZE) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  ecdsa_pair_attributes(&attr);
  st = psa_import_key(&attr, priv, priv_len, &id);
  psa_reset_key_attributes(&attr);
  if (st == PSA_SUCCESS) {
    *key_id = id;
  }
  return sensei_crypto_from_psa(st);
}

sensei_crypto_status_t sensei_crypto_ecdsa_import_public_key(const uint8_t *pub, size_t pub_len,
                                                             sensei_crypto_key_id_t *key_id) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t id = 0;
  psa_status_t st;

  if (pub == NULL || key_id == NULL || pub_len != SENSEI_CRYPTO_P256_PUBLIC_KEY_SIZE) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&attr, 256);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
  psa_set_key_algorithm(&attr, ECDSA_ALG);
  st = psa_import_key(&attr, pub, pub_len, &id);
  psa_reset_key_attributes(&attr);
  if (st == PSA_SUCCESS) {
    *key_id = id;
  }
  return sensei_crypto_from_psa(st);
}

sensei_crypto_status_t sensei_crypto_ecdsa_export_public_key(sensei_crypto_key_id_t key_id, uint8_t *pub,
                                                             size_t pub_size, size_t *pub_len) {
  if (pub == NULL || pub_len == NULL) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  return sensei_crypto_from_psa(psa_export_public_key((psa_key_id_t)key_id, pub, pub_size, pub_len));
}

sensei_crypto_status_t sensei_crypto_ecdsa_sign_hash(sensei_crypto_key_id_t key_id, const uint8_t *hash,
                                                     size_t hash_len, uint8_t *sig, size_t sig_size, size_t *sig_len) {
  if (hash == NULL || hash_len != SENSEI_CRYPTO_SHA256_SIZE || sig == NULL || sig_len == NULL) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  if (sig_size < SENSEI_CRYPTO_ECDSA_SIGNATURE_SIZE) {
    return SENSEI_CRYPTO_ERR_BUFFER_TOO_SMALL;
  }
  return sensei_crypto_from_psa(psa_sign_hash((psa_key_id_t)key_id, ECDSA_ALG, hash, hash_len, sig, sig_size, sig_len));
}

sensei_crypto_status_t sensei_crypto_ecdsa_verify_hash(sensei_crypto_key_id_t key_id, const uint8_t *hash,
                                                       size_t hash_len, const uint8_t *sig, size_t sig_len) {
  if (hash == NULL || hash_len != SENSEI_CRYPTO_SHA256_SIZE || sig == NULL) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  return sensei_crypto_from_psa(psa_verify_hash((psa_key_id_t)key_id, ECDSA_ALG, hash, hash_len, sig, sig_len));
}

sensei_crypto_status_t sensei_crypto_ecdsa_sign(sensei_crypto_key_id_t key_id, const uint8_t *msg, size_t msg_len,
                                                uint8_t *sig, size_t sig_size, size_t *sig_len) {
  uint8_t digest[SENSEI_CRYPTO_SHA256_SIZE];
  sensei_crypto_status_t st = sensei_crypto_sha256(msg, msg_len, digest);

  if (st != SENSEI_CRYPTO_OK) {
    return st;
  }
  return sensei_crypto_ecdsa_sign_hash(key_id, digest, sizeof(digest), sig, sig_size, sig_len);
}

sensei_crypto_status_t sensei_crypto_ecdsa_verify(sensei_crypto_key_id_t key_id, const uint8_t *msg, size_t msg_len,
                                                  const uint8_t *sig, size_t sig_len) {
  uint8_t digest[SENSEI_CRYPTO_SHA256_SIZE];
  sensei_crypto_status_t st = sensei_crypto_sha256(msg, msg_len, digest);

  if (st != SENSEI_CRYPTO_OK) {
    return st;
  }
  return sensei_crypto_ecdsa_verify_hash(key_id, digest, sizeof(digest), sig, sig_len);
}
