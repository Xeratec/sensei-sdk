/*
 * ----------------------------------------------------------------------
 *
 * File: pwr.c
 *
 * Last edited: 17.11.2026
 *
 * Copyright (c) 2024 ETH Zurich and University of Bologna
 *
 * Authors:
 * - Philipp Schilk (schilkp@ethz.ch), ETH Zurich
 * - Philip Wiese (wiesep@iis.ee.ethz.ch), ETH Zurich
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

#include "pwr.h"

#include "bsp/pmic_bsp.h"
#include "bsp/pwr_bsp.h"
#include "config/pmic_default.h"
#include "pwr/pwr_board.h"
#include "pwr/pwr_priv.h"
#ifdef CONFIG_SENSEI_PWR_CHARGE
#include "pwr/pwr_charge.h"
#endif

#include "max77654.h"

#include "zephyr/sys/reboot.h"

#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

// ======== Defines/Variables ======================================================================

LOG_MODULE_REGISTER(pwr);

#define GPIO_NODE_led_pwr DT_NODELABEL(gpio_led_pwr)
#define GPIO_NODE_gap9_pwr DT_NODELABEL(gpio_gap9_pwr)

static const struct gpio_dt_spec gpio_gap9_pwr_sel = GPIO_DT_SPEC_GET(GPIO_NODE_gap9_pwr, gpios);
static const struct gpio_dt_spec gpio_led_pwr = GPIO_DT_SPEC_GET(GPIO_NODE_led_pwr, gpios);

// Lines that must be inactive before the rails come on: the GAP9 supply select, the I2C, SPI and shared-GPIO
// muxes to GAP9 and the GAP9 slot line P1.04 (nodes of the board devicetree). Floating, they could let GAP9 boot,
// use the buses, or be back-powered through the slot pad.
static const struct gpio_dt_spec gap9_lines[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_gap9_pwr), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_gap9_i2c_ctrl), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_gap9_spi_ctrl), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_gap9_shared_ctrl), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_gap9_slot_sel), gpios),
};

static const struct device *pwr_i2c = DEVICE_DT_GET(DT_ALIAS(i2ca));

// PWR mutex:
// Has to be held to operate on the power subsys. For the public pwr interface this is managed automatically.
K_MUTEX_DEFINE(pwr_mutex);

// Driver handle:
struct max77654_h pmic_h = {
    .read_regs = pmic_bsp_read_regs,
    .write_regs = pmic_bsp_write_regs,
    .adc_read = pmic_bsp_adc_read,
    .adc_read_fast = pmic_bsp_adc_read_fast,
    .log = pmic_bsp_log,
    .conf = PMIC_CONFIG_DEFAULT,
};

struct max77654_h *pwr_pmic(void) { return &pmic_h; }

atomic_t pwr_is_initialised = ATOMIC_INIT(0);
atomic_t pwr_is_running = ATOMIC_INIT(0);

// The PMIC was reset and configured (by the boot hook or by pwr_init):
static bool pmic_is_up;

// Time the rails get to settle before the shield drivers talk to their chips:
#define PWR_RAILS_SETTLE_MS 50

// ======== PMIC bring-up ==========================================================================

/*
 * Boot hook. The expander and the IMU of the sensor shield start at POST_KERNEL, in the order of their init
 * priority. The PMIC rails have to be on before that, and must not be cycled afterwards. The hook runs after the
 * I2C bus (I2C_INIT_PRIORITY) and before the expander (the shield sets GPIO_PCA_SERIES_INIT_PRIORITY above it).
 */
#define PWR_BOOT_INIT_PRIORITY 55

BUILD_ASSERT(CONFIG_I2C_INIT_PRIORITY < PWR_BOOT_INIT_PRIORITY, "the PMIC hook must run after the I2C bus");
#if defined(CONFIG_GPIO_PCA_SERIES_INIT_PRIORITY) && defined(CONFIG_SENSEI_PWR_BOOT_RAILS)
BUILD_ASSERT(PWR_BOOT_INIT_PRIORITY < CONFIG_GPIO_PCA_SERIES_INIT_PRIORITY,
             "the expander must start after the rails are on (see UT_SensorShield Kconfig.defconfig)");
#endif

// Rails of the sensor shield and GAP9: SBB0 3.3 V, SBB1 2.8 V, SBB2 1.2 V, LDO0 3.3 V.
static void rails_on(struct max77654_conf *c) {
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
}

static void gap9_lines_inactive(void) {
  for (size_t i = 0; i < ARRAY_SIZE(gap9_lines); i++) {
    if (!gpio_is_ready_dt(&gap9_lines[i]) || gpio_pin_configure_dt(&gap9_lines[i], GPIO_OUTPUT_INACTIVE) < 0) {
      LOG_ERR("GAP9 line %u init error", (unsigned)i);
    }
  }
}

// Reset the PMIC (this power-cycles the rails), configure it, and with boot rails switch them on.
static int pmic_bring_up(void) {
  int err;

  gap9_lines_inactive(); // before the rails

  if (!device_is_ready(pwr_i2c)) {
    LOG_ERR("I2C device %s is not ready", pwr_i2c->name);
    return -ENODEV;
  }

  k_mutex_lock(&pwr_mutex, K_FOREVER);

  err = max77654_reset(&pmic_h);
  if (err != E_MAX77654_SUCCESS) {
    // Reset is treated as best-effort: log the failure but continue, since
    // max77654_init() below re-applies the configuration regardless.
    LOG_ERR("RESET failed! (%d)", err);
  }

  k_sleep(K_MSEC(300));

  if (IS_ENABLED(CONFIG_SENSEI_PWR_BOOT_RAILS)) {
    rails_on(&pmic_h.conf);
  }

  err = max77654_init(&pmic_h);
  if (err != E_MAX77654_SUCCESS) {
    LOG_ERR("Driver init failed. (%d)", err);
    k_mutex_unlock(&pwr_mutex);
    return -EIO;
  }

  // LED driver: its supply (SDB) has to be on when its driver starts.
  if (gpio_is_ready_dt(&gpio_led_pwr) && gpio_pin_configure_dt(&gpio_led_pwr, GPIO_OUTPUT_ACTIVE) < 0) {
    LOG_ERR("LED EN GPIO init error");
  }

  if (IS_ENABLED(CONFIG_SENSEI_PWR_BOOT_RAILS)) {
    k_sleep(K_MSEC(PWR_RAILS_SETTLE_MS));
    LOG_INF("rails on");
  }
  pmic_is_up = true;
  k_mutex_unlock(&pwr_mutex);
  return 0;
}

static int pwr_boot_hook(void) {
  // A failure is reported again by pwr_init(), which tries once more.
  (void)pmic_bring_up();
  return 0;
}

SYS_INIT(pwr_boot_hook, POST_KERNEL, PWR_BOOT_INIT_PRIORITY);

// ======== Functions ==============================================================================

int pwr_init(void) {
  int err = 0;

  if (!pmic_is_up && pmic_bring_up() < 0) {
    return -1;
  }

  if (gpio_pin_configure_dt(&gpio_gap9_pwr_sel, GPIO_OUTPUT_INACTIVE) < 0) {
    LOG_ERR("GAP9 EN GPIO init error");
    return -1;
  }

  if (k_mutex_lock(&pwr_mutex, PWR_MUTEX_TIMEOUT) < 0) {
    LOG_ERR("PWR Deadlock!");
    return -1;
  }

  if (pwr_bsp_init()) {
    LOG_ERR("PMIC BSP Init failed!");
    err = -1;
    goto cleanup;
  }

  // Init pwr thread:
  if (thread_pwr_init() < 0) {
    LOG_ERR("pwr thread init failed.");
    err = -1;
    goto cleanup;
  }

  // Enabeling the i2cb switch seems to cause a glitch on the i2c bus which
  // has the PMIC fail it's next i2c transaction. This hotfix resets the
  // bus after to ensure the PMIC is happy.
  // pmic_i2c_reset_hotfix();

  atomic_set(&pwr_is_initialised, true);

  k_sleep(K_MSEC(100));

cleanup:
  k_mutex_unlock(&pwr_mutex);
  return err;
}

int pwr_start(void) {
  __ASSERT_NO_MSG(atomic_get(&pwr_is_initialised) == true);
  __ASSERT_NO_MSG(atomic_get(&pwr_is_running) == false);

  // Power up LED
  if (gpio_pin_configure_dt(&gpio_led_pwr, GPIO_OUTPUT_ACTIVE) < 0) {
    LOG_ERR("LED EN GPIO configuration error");
    return -1;
  }

  if (pwr_bsp_start()) {
    LOG_ERR("PMIC BSP Start failed!");
    return -1;
  }

  thread_pwr_start();

  atomic_set(&pwr_is_running, true);

  return 0;
}

#define PWR_OFF_TRIES 10

void pwr_off_and_reboot(void) {
  bool did_lock = false;
  // FIXME: This whole thing is very hacky and a workaround until the PMIC's FSM is working.

#ifdef CONFIG_SENSEI_PWR_CHARGE
  (void)pwr_charge_save(); // the counter survives the planned reset in the settings too
#endif

  LOG_INF("Stopping power subsystem...");
  for (uint32_t i = 0; i < 3; i++) {
    if (k_mutex_lock(&pwr_mutex, K_MSEC(10)) < 0) {
      LOG_ERR("PWR MUTEX Timeout!");
    } else {
      did_lock = true;
      break;
    }
  }

  if (!did_lock) {
    LOG_ERR("Ignoring pwr mutex. Forcibly shutting down!");
  }

  // hotfix PMIC i2c:
  // pmic_i2c_reset_hotfix();

  // Power down LED
  if (gpio_pin_set_dt(&gpio_led_pwr, 0) < 0) {
    LOG_ERR("Failed to power down LED");
  }

  int err = 0;
  for (uint32_t i = 0; i < PWR_OFF_TRIES; i++) {
    pmic_h.conf.sbb_conf[0].en = MAX77654_REG_OFF;
    pmic_h.conf.sbb_conf[1].en = MAX77654_REG_OFF;
    pmic_h.conf.sbb_conf[2].en = MAX77654_REG_OFF;
    pmic_h.conf.ldo_conf[0].en = MAX77654_REG_OFF;
    pmic_h.conf.ldo_conf[1].en = MAX77654_REG_OFF;
    pmic_h.conf.main_bias_force_enable = false;
    err = max77654_config(&pmic_h);
    if (err == 0) {
      break;
    }
    LOG_ERR("Shutdown error!... retrying...");
    k_sleep(K_MSEC(10));
  }

  LOG_INF("Rebooting...");

  k_sleep(K_MSEC(500));
  sys_reboot(SYS_REBOOT_COLD);

  LOG_ERR("REBOOT FAIL!");

  if (did_lock) {
    k_mutex_unlock(&pwr_mutex);
  }
}

int pwr_log_voltages(void) {
  if (k_mutex_lock(&pwr_mutex, PWR_MUTEX_TIMEOUT) < 0) {
    return -EBUSY;
  }
  int err = measure_voltages();

  k_mutex_unlock(&pwr_mutex);
  return err;
}

uint32_t pwr_bat_perc(void) {
  __ASSERT_NO_MSG(atomic_get(&pwr_is_initialised) == true);
  return atomic_get(&battery_perc);
}

uint32_t pwr_bat_mV(void) {
  __ASSERT_NO_MSG(atomic_get(&pwr_is_initialised) == true);
  return atomic_get(&battery_mV);
}

void gap9_pwr(bool on) {
  __ASSERT_NO_MSG(atomic_get(&pwr_is_initialised) == true);
  if (on) {
    atomic_set(&gap9_pwr_state, 1);
    gpio_pin_set_dt(&gpio_gap9_pwr_sel, 1);
  } else {
    atomic_set(&gap9_pwr_state, 0);
    gpio_pin_set_dt(&gpio_gap9_pwr_sel, 0);
  }
}
