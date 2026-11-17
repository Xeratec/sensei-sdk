/*
 * ----------------------------------------------------------------------
 *
 * File: sensei_crypto_ecdh.c
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

#define ECDH_KEY_TYPE PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1)

static void ecdh_attributes(psa_key_attributes_t *attr) {
  psa_set_key_type(attr, ECDH_KEY_TYPE);
  psa_set_key_bits(attr, 256);
  psa_set_key_usage_flags(attr, PSA_KEY_USAGE_DERIVE);
  psa_set_key_algorithm(attr, PSA_ALG_ECDH);
}

sensei_crypto_status_t sensei_crypto_ecdh_generate_keypair(sensei_crypto_key_id_t *key_id) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t id = 0;
  psa_status_t st;

  if (key_id == NULL) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  ecdh_attributes(&attr);
  st = psa_generate_key(&attr, &id);
  psa_reset_key_attributes(&attr);
  if (st == PSA_SUCCESS) {
    *key_id = id;
  }
  return sensei_crypto_from_psa(st);
}

sensei_crypto_status_t sensei_crypto_ecdh_import_private_key(const uint8_t *priv, size_t priv_len,
                                                             sensei_crypto_key_id_t *key_id) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t id = 0;
  psa_status_t st;

  if (priv == NULL || key_id == NULL || priv_len != SENSEI_CRYPTO_P256_PRIVATE_KEY_SIZE) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  ecdh_attributes(&attr);
  st = psa_import_key(&attr, priv, priv_len, &id);
  psa_reset_key_attributes(&attr);
  if (st == PSA_SUCCESS) {
    *key_id = id;
  }
  return sensei_crypto_from_psa(st);
}

sensei_crypto_status_t sensei_crypto_ecdh_export_public_key(sensei_crypto_key_id_t key_id, uint8_t *pub,
                                                            size_t pub_size, size_t *pub_len) {
  if (pub == NULL || pub_len == NULL) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  return sensei_crypto_from_psa(psa_export_public_key((psa_key_id_t)key_id, pub, pub_size, pub_len));
}

sensei_crypto_status_t sensei_crypto_ecdh_shared_secret(sensei_crypto_key_id_t key_id, const uint8_t *peer_pub,
                                                        size_t peer_len, uint8_t *secret, size_t secret_size,
                                                        size_t *secret_len) {
  if (peer_pub == NULL || peer_len != SENSEI_CRYPTO_P256_PUBLIC_KEY_SIZE || secret == NULL || secret_len == NULL) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  if (secret_size < SENSEI_CRYPTO_ECDH_SECRET_SIZE) {
    return SENSEI_CRYPTO_ERR_BUFFER_TOO_SMALL;
  }
  return sensei_crypto_from_psa(
      psa_raw_key_agreement(PSA_ALG_ECDH, (psa_key_id_t)key_id, peer_pub, peer_len, secret, secret_size, secret_len));
}
