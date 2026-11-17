/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file main.c
 * @brief nRF9151 LPWAN service: SPI slave to the nRF5340, LTE-M/NB-IoT and LoRaWAN (LR1121).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lora.h"
#include "lte.h"

LOG_MODULE_REGISTER(main, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

int service_start(void);

int main(void) {
  int err;

  /* The link comes first so the master gets answers while the radios start. */
  err = service_start();
  if (err) {
    LOG_ERR("link start failed: %d", err);
    return err;
  }
#ifdef CONFIG_LPWAN_SHIELD_LTE
  err = lte_init();
  if (err) {
    LOG_ERR("LTE init failed: %d", err);
  }
#endif
#ifdef CONFIG_LPWAN_SHIELD_LORA
  err = lora_init();
  if (err) {
    LOG_ERR("LoRa init failed: %d", err);
  }
#endif
  LOG_INF("nRF9151 LPWAN service up");
  /* Everything else runs in threads: the main thread has nothing to do. */
  return 0;
}
