/*
 * ----------------------------------------------------------------------
 *
 * File: sensei_crypto_priv.h
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

#ifndef SENSEI_CRYPTO_PRIV_H_
#define SENSEI_CRYPTO_PRIV_H_

/* Internal helpers shared by the sensei_crypto_*.c files. Not part of the public API. */

#include <psa/crypto.h>

#include <zephyr/logging/log.h>

#include "sensei_crypto.h"

/* The public key handle type must be able to hold a PSA key id. */
_Static_assert(sizeof(psa_key_id_t) == sizeof(sensei_crypto_key_id_t),
               "psa_key_id_t does not fit sensei_crypto_key_id_t");

/** Translate a PSA status code to a sensei_crypto_status_t. */
sensei_crypto_status_t sensei_crypto_from_psa(psa_status_t status);

/** Dummy buffer substituted for NULL pointers with zero length (PSA rejects NULL in some drivers). */
extern const uint8_t sensei_crypto_empty[1];

#define SENSEI_CRYPTO_PTR(p, len) (((len) == 0 && (p) == NULL) ? sensei_crypto_empty : (p))

#endif /* SENSEI_CRYPTO_PRIV_H_ */
