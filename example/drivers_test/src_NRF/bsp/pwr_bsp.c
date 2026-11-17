/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Power hooks for the driver test: PMIC rails on, sensor rails and GAP9 off. */

#include "bsp/pwr_bsp.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "max77654.h"
#include "pwr/pwr_board.h"

LOG_MODULE_REGISTER(pwr_bsp, LOG_LEVEL_INF);

#define GPIO_SPEC(label) GPIO_DT_SPEC_GET(DT_NODELABEL(label), gpios)

/* SCD41 and SGP41 rails stay off: this test does not use them. */
static const struct gpio_dt_spec scd41_pwr = GPIO_SPEC(gpio_scd41_pwr);
static const struct gpio_dt_spec sgp41_pwr = GPIO_SPEC(gpio_sgp41_pwr);

int pwr_bsp_init() {
  if (!device_is_ready(scd41_pwr.port)) {
    return -1;
  }
  if (gpio_pin_configure_dt(&scd41_pwr, GPIO_OUTPUT_INACTIVE) < 0 ||
      gpio_pin_configure_dt(&sgp41_pwr, GPIO_OUTPUT_INACTIVE) < 0) {
    LOG_ERR("rail pin init failed");
    return -1;
  }
  return 0;
}

/* The PMIC rails are on since boot (SDK power module, CONFIG_SENSEI_PWR_BOOT_RAILS). */
int pwr_bsp_start() { return 0; }
