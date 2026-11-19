/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Sealing key provider for tests (CONFIG_SENSEI_SECURE_SETTINGS_SEAL_TEST_KEY): a fixed key in the image, and a
 * switch that makes the provider fail. Never for a product. */

#include <errno.h>
#include <string.h>

#include "sensei_e2e_settings.h"

static uint8_t key[SENSEI_SECURE_KEY_SIZE] = {0x53, 0x45, 0x4E, 0x53, 0x45, 0x49, 0x2D, 0x54,
                                              0x45, 0x53, 0x54, 0x2D, 0x4B, 0x45, 0x59, 0x21}; /* "SENSEI-TEST-KEY!" */
static bool failing;

void sensei_e2e_testkey_set(const uint8_t *k) {
  failing = k == NULL;
  if (k != NULL) {
    memcpy(key, k, sizeof(key));
  }
}

int sensei_e2e_settings_seal_key(uint8_t out[SENSEI_SECURE_KEY_SIZE]) {
  if (failing) {
    return -EIO;
  }
  memcpy(out, key, sizeof(key));
  return 0;
}
