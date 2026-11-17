/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Power on all RAM, very early (MCUboot, CONFIG_SENSEI_PWR_RAM_ON_BOOT). See Kconfig. */

#include <zephyr/init.h>

#include <hal/nrf_vmc.h>

static int ram_on(void) {
  nrf_vmc_ram_block_power_all_set(NRF_VMC);
  return 0;
}

SYS_INIT(ram_on, PRE_KERNEL_1, 0);
