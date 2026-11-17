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

/* Rails as in the node firmware: SBB0 3.3 V, SBB1 2.8 V, SBB2 1.2 V, LDO0 3.3 V. */
int pwr_bsp_start() {
  struct max77654_conf *c = &pwr_pmic()->conf;
  static const uint16_t sbb_mv[3] = {3300, 2800, 1200};

  for (int i = 0; i < 3; i++) {
    c->sbb_conf[i].mode = MAX77654_SBB_MODE_BUCKBOOST;
    c->sbb_conf[i].peak_current = MAX77654_SBB_PEAK_CURRENT_1A;
    c->sbb_conf[i].active_discharge = false;
    c->sbb_conf[i].en = MAX77654_REG_ON;
    c->sbb_conf[i].output_voltage_mV = sbb_mv[i];
  }
  c->ldo_conf[0].mode = MAX77654_LDO_MODE_LDO;
  c->ldo_conf[0].active_discharge = false;
  c->ldo_conf[0].en = MAX77654_REG_ON;
  c->ldo_conf[0].output_voltage_mV = 3300;

  if (max77654_config(pwr_pmic()) != E_MAX77654_SUCCESS) {
    LOG_ERR("PMIC configuration failed");
    return -1;
  }
  return 0;
}
