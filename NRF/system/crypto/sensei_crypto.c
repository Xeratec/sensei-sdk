/*
 * ----------------------------------------------------------------------
 *
 * File: sensei_crypto.c
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

LOG_MODULE_REGISTER(sensei_crypto, CONFIG_SENSEI_CRYPTO_LOG_LEVEL);

const uint8_t sensei_crypto_empty[1] = {0};

sensei_crypto_status_t sensei_crypto_from_psa(psa_status_t status) {
  if (status == PSA_SUCCESS) {
    return SENSEI_CRYPTO_OK;
  }

  LOG_DBG("PSA error %d", (int)status);

  switch (status) {
  case PSA_ERROR_INVALID_ARGUMENT:
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  case PSA_ERROR_NOT_SUPPORTED:
    return SENSEI_CRYPTO_ERR_NOT_SUPPORTED;
  case PSA_ERROR_BUFFER_TOO_SMALL:
    return SENSEI_CRYPTO_ERR_BUFFER_TOO_SMALL;
  case PSA_ERROR_BAD_STATE:
    return SENSEI_CRYPTO_ERR_BAD_STATE;
  case PSA_ERROR_INVALID_SIGNATURE:
    return SENSEI_CRYPTO_ERR_AUTH_FAILED;
  case PSA_ERROR_INSUFFICIENT_MEMORY:
    return SENSEI_CRYPTO_ERR_NO_MEMORY;
  case PSA_ERROR_INVALID_HANDLE:
  case PSA_ERROR_DOES_NOT_EXIST:
    return SENSEI_CRYPTO_ERR_NOT_FOUND;
  case PSA_ERROR_NOT_PERMITTED:
    return SENSEI_CRYPTO_ERR_NOT_PERMITTED;
  case PSA_ERROR_HARDWARE_FAILURE:
  case PSA_ERROR_INSUFFICIENT_ENTROPY:
    return SENSEI_CRYPTO_ERR_HARDWARE;
  default:
    return SENSEI_CRYPTO_ERR_GENERIC;
  }
}

sensei_crypto_status_t sensei_crypto_init(void) { return sensei_crypto_from_psa(psa_crypto_init()); }

sensei_crypto_status_t sensei_crypto_random(uint8_t *out, size_t len) {
  if (out == NULL && len != 0) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  if (len == 0) {
    return SENSEI_CRYPTO_OK;
  }
  return sensei_crypto_from_psa(psa_generate_random(out, len));
}

sensei_crypto_status_t sensei_crypto_destroy_key(sensei_crypto_key_id_t key) {
  return sensei_crypto_from_psa(psa_destroy_key((psa_key_id_t)key));
}

sensei_crypto_status_t sensei_crypto_sha256(const uint8_t *msg, size_t msg_len,
                                            uint8_t digest[SENSEI_CRYPTO_SHA256_SIZE]) {
  psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
  size_t out_len = 0;
  psa_status_t st;

  if (digest == NULL || (msg == NULL && msg_len != 0)) {
    return SENSEI_CRYPTO_ERR_INVALID_ARG;
  }
  /* The multi-part calls, not psa_hash_compute(): the Oberon driver has a second copy of SHA-256 for the one-shot call
   * (about 1 kB) and the incremental one is in the image already (HMAC and HKDF use it). */
  st = psa_hash_setup(&op, PSA_ALG_SHA_256);
  if (st == PSA_SUCCESS) {
    st = psa_hash_update(&op, SENSEI_CRYPTO_PTR(msg, msg_len), msg_len);
  }
  if (st == PSA_SUCCESS) {
    st = psa_hash_finish(&op, digest, SENSEI_CRYPTO_SHA256_SIZE, &out_len);
  }
  if (st != PSA_SUCCESS) {
    (void)psa_hash_abort(&op);
  }
  return sensei_crypto_from_psa(st);
}
