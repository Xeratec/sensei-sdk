/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Sealing key from the hardware unique key (CONFIG_SENSEI_SECURE_SETTINGS_SEAL_HUK), nRF5340 application core.
 *
 * The CryptoCell-312 and its KMU hold random 256-bit keys in key slots that the CPU cannot read. The first call of
 * sensei_e2e_settings_seal_prepare() writes them (hw_unique_key_write_random(), once, until the chip is erased; the
 * application calls it early in main(), else the first key request does); the sealing key is then derived
 * from the MEXT slot by the CryptoCell key derivation with the label below and returned in RAM. The caller
 * imports it as a volatile PSA key and wipes the copy (sensei_e2e_settings.c). No TF-M needed. */

#include <errno.h>

#include <hw_unique_key.h>
#include <zephyr/logging/log.h>

#include "sensei_e2e_settings.h"

LOG_MODULE_DECLARE(sensei_e2e_settings);

#define LABEL "SENSEI-E2E-AT-REST-v1"

/* Write the HUK if there is none. hw_unique_key_write_random() writes the UICR through the NVMC inside the CryptoCell
 * platform library, without the flash driver lock: call it before other threads write flash (sensei_e2e_settings.h). */
int sensei_e2e_settings_seal_prepare(void) {
  int err;

  if (!hw_unique_key_are_any_written()) {
    LOG_INF("first start: writing the hardware unique key");
    err = hw_unique_key_write_random();
    if (err != HW_UNIQUE_KEY_SUCCESS) {
      LOG_ERR("HUK write: %d", err);
      return -EIO;
    }
  }
  return 0;
}

int sensei_e2e_settings_seal_key(uint8_t key[SENSEI_SECURE_KEY_SIZE]) {
  int err = sensei_e2e_settings_seal_prepare(); /* a no-op after the early call in main() */

  if (err) {
    return err;
  }
  err = hw_unique_key_derive_key(HUK_KEYSLOT_MEXT, NULL, 0, (const uint8_t *)LABEL, sizeof(LABEL) - 1, key,
                                 SENSEI_SECURE_KEY_SIZE);
  if (err != HW_UNIQUE_KEY_SUCCESS) {
    LOG_ERR("HUK derive: %d", err);
    return -EIO;
  }
  return 0;
}
