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

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>

#ifdef CONFIG_LPWAN_SHIELD_LTE
#include <modem/nrf_modem_lib.h>
#endif

#include "lora.h"
#include "lte.h"

LOG_MODULE_REGISTER(main, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

int service_start(void);

#ifdef CONFIG_LPWAN_SHIELD_LTE
/*
 * The modem core crashed. The default handler only logs, and the LTE backend would then wait for ever
 * for a modem that is gone. Reset the whole chip: the nRF5340 sees the link drop, power-cycles the shield
 * if it does not come back, and connects again (CONFIG_NRF_MODEM_LIB_ON_FAULT_APPLICATION_SPECIFIC).
 */
void nrf_modem_fault_handler(struct nrf_modem_fault_info *fault) {
  LOG_ERR("modem fault 0x%x at 0x%x: reset", (unsigned)fault->reason, (unsigned)fault->program_counter);
  sys_reboot(SYS_REBOOT_COLD);
}
#endif

int main(void) {
  int err;

  /* Task watchdog with the hardware watchdog behind it: the link thread feeds it (link.c). A frozen kernel or a
   * link thread that cannot re-arm the SPI slave resets the chip. */
  err = task_wdt_init(DEVICE_DT_GET(DT_ALIAS(watchdog0)));
  if (err) {
    LOG_ERR("watchdog init failed: %d", err);
  }
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
