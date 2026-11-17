/*
 * ----------------------------------------------------------------------
 *
 * File: sensei_crypto_aead.c
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

static sensei_crypto_status_t import_aes_key(const uint8_t *key, size_t key_len, sensei_crypto_key_id_t *key_id) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t id = 0;
  psa_status_t st;

  psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&attr, PSA_ALG_GCM);

  st = psa_import_key(&attr, key, key_len, &id);
  psa_reset_key_attributes(&attr);
  if (st == PSA_SUCCESS) {
    *key_id = id;
  }
  return sensei_crypto_from_psa(st);
}

sensei_crypto_status_t sensei_crypto_aead_import_key(const uint8_t *key, size_t key_len,
                                                     sensei_crypto_key_id_t *key_id) {
  if (key == NULL || key_id == NULL || (key_len != 16 && key_len != 32)) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  return import_aes_key(key, key_len, key_id);
}

sensei_crypto_status_t sensei_crypto_aead_generate_key(size_t key_bits, sensei_crypto_key_id_t *key_id) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t id = 0;
  psa_status_t st;

  if (key_id == NULL || (key_bits != 128 && key_bits != 256)) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }

  psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attr, key_bits);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&attr, PSA_ALG_GCM);

  st = psa_generate_key(&attr, &id);
  psa_reset_key_attributes(&attr);
  if (st == PSA_SUCCESS) {
    *key_id = id;
  }
  return sensei_crypto_from_psa(st);
}

sensei_crypto_status_t sensei_crypto_aead_generate_nonce(uint8_t nonce[SENSEI_CRYPTO_AES_GCM_NONCE_SIZE]) {
  return sensei_crypto_random(nonce, SENSEI_CRYPTO_AES_GCM_NONCE_SIZE);
}

sensei_crypto_status_t sensei_crypto_aead_encrypt(sensei_crypto_key_id_t key_id,
                                                  const uint8_t nonce[SENSEI_CRYPTO_AES_GCM_NONCE_SIZE],
                                                  const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len,
                                                  uint8_t *ct, size_t ct_size, size_t *ct_len) {
  if (nonce == NULL || ct == NULL || ct_len == NULL || (aad == NULL && aad_len != 0) || (pt == NULL && pt_len != 0)) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  if (pt_len > SIZE_MAX - SENSEI_CRYPTO_AES_GCM_TAG_SIZE || ct_size < pt_len + SENSEI_CRYPTO_AES_GCM_TAG_SIZE) {
    return SENSEI_CRYPTO_ERR_BUFFER_TOO_SMALL;
  }

  return sensei_crypto_from_psa(psa_aead_encrypt((psa_key_id_t)key_id, PSA_ALG_GCM, nonce,
                                                 SENSEI_CRYPTO_AES_GCM_NONCE_SIZE, SENSEI_CRYPTO_PTR(aad, aad_len),
                                                 aad_len, SENSEI_CRYPTO_PTR(pt, pt_len), pt_len, ct, ct_size, ct_len));
}

sensei_crypto_status_t sensei_crypto_aead_decrypt(sensei_crypto_key_id_t key_id,
                                                  const uint8_t nonce[SENSEI_CRYPTO_AES_GCM_NONCE_SIZE],
                                                  const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len,
                                                  uint8_t *pt, size_t pt_size, size_t *pt_len) {
  if (nonce == NULL || ct == NULL || pt_len == NULL || (aad == NULL && aad_len != 0) ||
      ct_len < SENSEI_CRYPTO_AES_GCM_TAG_SIZE) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  if (pt_size < ct_len - SENSEI_CRYPTO_AES_GCM_TAG_SIZE) {
    return SENSEI_CRYPTO_ERR_BUFFER_TOO_SMALL;
  }
  if (pt == NULL) {
    /* Zero-length plaintext: PSA still needs a valid output pointer. */
    static uint8_t dummy[1];
    pt = dummy;
    pt_size = 0;
  }

  return sensei_crypto_from_psa(psa_aead_decrypt((psa_key_id_t)key_id, PSA_ALG_GCM, nonce,
                                                 SENSEI_CRYPTO_AES_GCM_NONCE_SIZE, SENSEI_CRYPTO_PTR(aad, aad_len),
                                                 aad_len, ct, ct_len, pt, pt_size, pt_len));
}
