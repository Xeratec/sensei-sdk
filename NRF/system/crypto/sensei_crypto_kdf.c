/*
 * ----------------------------------------------------------------------
 *
 * File: sensei_crypto_kdf.c
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

#if defined(CONFIG_SENSEI_CRYPTO_HMAC)
sensei_crypto_status_t sensei_crypto_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                                                 uint8_t mac[SENSEI_CRYPTO_SHA256_SIZE]) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t id = 0;
  size_t mac_len = 0;
  psa_status_t st;

  if (key == NULL || key_len == 0 || mac == NULL || (msg == NULL && msg_len != 0)) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }

  psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
  psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));

  st = psa_import_key(&attr, key, key_len, &id);
  psa_reset_key_attributes(&attr);
  if (st != PSA_SUCCESS) {
    return sensei_crypto_from_psa(st);
  }

  st = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), SENSEI_CRYPTO_PTR(msg, msg_len), msg_len, mac,
                       SENSEI_CRYPTO_SHA256_SIZE, &mac_len);
  (void)psa_destroy_key(id);
  return sensei_crypto_from_psa(st);
}
#endif /* CONFIG_SENSEI_CRYPTO_HMAC */

#if defined(CONFIG_SENSEI_CRYPTO_HKDF)
sensei_crypto_status_t sensei_crypto_hkdf_sha256(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt,
                                                 size_t salt_len, const uint8_t *info, size_t info_len, uint8_t *okm,
                                                 size_t okm_len) {
  psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
  psa_status_t st;

  if (ikm == NULL || ikm_len == 0 || okm == NULL || okm_len == 0 || okm_len > SENSEI_CRYPTO_HKDF_MAX_OUTPUT_SIZE ||
      (salt == NULL && salt_len != 0) || (info == NULL && info_len != 0)) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }

  st = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
  if (st != PSA_SUCCESS) {
    goto out;
  }
  if (salt_len > 0) {
    st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
    if (st != PSA_SUCCESS) {
      goto out;
    }
  }
  st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, ikm, ikm_len);
  if (st != PSA_SUCCESS) {
    goto out;
  }
  /* Always give the info step, even when empty: mbed TLS refuses to output without it. */
  st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, info_len > 0 ? info : (const uint8_t *)"",
                                      info_len);
  if (st != PSA_SUCCESS) {
    goto out;
  }
  st = psa_key_derivation_output_bytes(&op, okm, okm_len);

out:
  (void)psa_key_derivation_abort(&op);
  return sensei_crypto_from_psa(st);
}
#endif /* CONFIG_SENSEI_CRYPTO_HKDF */
