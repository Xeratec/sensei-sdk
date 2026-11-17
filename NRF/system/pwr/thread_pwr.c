/*
 * ----------------------------------------------------------------------
 *
 * File: thread_pwr.c
 *
 * Last edited: 30.10.2025
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

#include "pwr/pwr.h"
#include "pwr/pwr_priv.h"

#include "config/pmic_default.h"
#include "config/tasks.h"

#include "max77654.h"
#include "sensei_pwr_math.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>

#ifdef CONFIG_SENSEI_GAP9
#include "sensei_gap9/gap9_zephyr.h"
#endif

// ======== Definitions/Variables ==================================================================

LOG_MODULE_REGISTER(thread_pwr);

/* The devicetree node identifier */
#define GPIO_NODE_pmic_irq DT_NODELABEL(gpio_pmic_irq)
#define GPIO_NODE_soft_nrst DT_NODELABEL(gpio_soft_nrst)
#define GPIO_NODE_gap9_pwr DT_NODELABEL(gpio_gap9_pwr)

/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static const struct gpio_dt_spec gpio_pmic_irq = GPIO_DT_SPEC_GET(GPIO_NODE_pmic_irq, gpios);
static const struct gpio_dt_spec gpio_soft_nrst = GPIO_DT_SPEC_GET(GPIO_NODE_soft_nrst, gpios);
#ifndef CONFIG_SENSEI_GAP9
static const struct gpio_dt_spec gpio_gap9_pwr_sel = GPIO_DT_SPEC_GET(GPIO_NODE_gap9_pwr, gpios);
#endif

// Semaphore to indicate that an update should be performed:
K_SEM_DEFINE(pmic_update_sem, 0, 2);

// Current battery percentage (0-100):
atomic_t battery_perc = ATOMIC_INIT(0);

// Current battery voltage (mV):
atomic_t battery_mV = ATOMIC_INIT(0);

atomic_t gap9_pwr_state = ATOMIC_INIT(0);

// PMIC interrupt callback:
static struct gpio_callback pmic_int_cb;
static void pmic_int_cb_handler(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins);

// Soft Reset interrupt callback:
static struct gpio_callback softRst_int_cb;
static void softRst_int_cb_handler(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins);

// Button: key-down is the nEN falling edge (flag read from the PMIC), key-up is the Soft_nRST edge. The interrupt
// handlers only store a time and wake the thread. pmic_irq_ms is the time of the last PMIC interrupt, which is
// the time of the key-down when the flags say nEN fell.
static struct sensei_button button;
static volatile uint32_t pmic_irq_ms;
static volatile uint32_t keyup_ms;
static atomic_t keyup_pending;

// Check the current PMIC/charger status:
static int check_status();

// Measure the current battery percentage:
static int check_battery();

// Handle interrupts from the PMIC:
static int handle_interrupts();

// Re-apply charger configuration to the PMIC:
static int reapply_config();

// Convert battery mV to percentage:
static uint32_t battery_perc_conversion(uint32_t mV);

struct max77654_device {
  struct max77654_h *pmic_h;
  struct max77654_int flags;
  struct max77654_stat status;
};

struct max77654_device pmic;

// The interrupt flags clear when read: they count as events as soon as the read worked, even if the status read after
// it failed.
static bool flags_valid;
static bool status_valid;

// Last status and event callback, see pwr.h:
static struct pwr_status pwr_status_last;
static struct k_spinlock pwr_status_lock;
static pwr_event_cb_t pwr_event_cb;
static void *pwr_event_cb_data;

// Battery level class for the BAT_OK / LOW / CRITICAL events (0 = unknown, 1 critical, 2 low, 3 ok):
static uint8_t battery_class;

// Update the status snapshot and return the events since the last call:
static uint32_t collect_events();

// ======== Thread =================================================================================

int thread_pwr_init() {
  pmic.pmic_h = &pmic_h;

  // The button state starts clean; the interrupts are enabled last, after the callbacks:
  memset(&button, 0, sizeof(button));
  atomic_clear(&keyup_pending);

  // Create a callback:
  gpio_init_callback(&pmic_int_cb, pmic_int_cb_handler, BIT(gpio_pmic_irq.pin));
  gpio_init_callback(&softRst_int_cb, softRst_int_cb_handler, BIT(gpio_soft_nrst.pin));

  // Attach callback to pin interrupt:
  if (gpio_add_callback(gpio_pmic_irq.port, &pmic_int_cb) < 0) {
    LOG_ERR("GPIO callback add error!");
    return -1;
  }

  if (gpio_add_callback(gpio_soft_nrst.port, &softRst_int_cb) < 0) {
    LOG_ERR("GPIO callback add error!");
    return -1;
  }

  // Setup PMIC int pin interrupt:
  if (gpio_pin_interrupt_configure_dt(&gpio_pmic_irq, GPIO_INT_EDGE_FALLING) < 0) {
    LOG_ERR("GPIO Int. config error!");
    return -1;
  }

  // Setup soft reset pin:
  if (gpio_pin_interrupt_configure_dt(&gpio_soft_nrst, GPIO_INT_EDGE_RISING) < 0) {
    LOG_ERR("GAP9 EN GPIO configuration error");
    return -1;
  }

  check_battery();

  return 0;
}

static void thread_pwr(void *_a, void *_b, void *_c) {
  LOG_INF("Running.");

  // Keep track of the last time the charger was re-configured (millisec):
  int64_t last_reconfig_ms = k_uptime_get();

  while (1) {

    int err = 0;
    uint32_t events = 0;
    struct pwr_status status_copy;

    // Grab power mutex to be allowed to operate on PMIC:
    if (k_mutex_lock(&pwr_mutex, PWR_MUTEX_TIMEOUT) < 0) {
      LOG_ERR("Power deadlock!");
      err = -1;
    } else {
      // Grab current PMIC status, issuing charger on / charger off events:
      if (check_status() < 0) {
        err = -1;
      }

      // Measure battery level, issuing battery ok / critical / low events:
      if (check_battery() < 0) {
        err = -1;
      }

      // Handle interrupts from PMIC
      if (handle_interrupts() < 0) {
        err = -1;
      }

      // Snapshot for pwr_get_status() and the event callback. The flags were read (and cleared in the PMIC)
      // even when something after them failed, so their events must not be dropped:
      if (flags_valid) {
        events = collect_events();
      }

      // nEN pressed (key-down), Soft_nRST edge (key-up). A press longer than 2 seconds switches off and reboots:
      if (pmic.flags.nen_f) {
        sensei_button_down(&button, pmic_irq_ms);
      }
      // Only after a good flag read, so that a key-down in the flags is seen before its key-up:
      if (flags_valid && atomic_cas(&keyup_pending, 1, 0)) {
        if (sensei_button_up(&button, keyup_ms, THREAD_PWR_SOFT_RESET_LATENCY_MS) == SENSEI_BUTTON_LONG) {
          LOG_INF("Button pressed for more than 2 seconds");
          pwr_off_and_reboot();
        }
      }

      // Re-apply charger configuration periodically. Recommended by PMIC Datasheet.
      if ((k_uptime_get() - last_reconfig_ms) > THREAD_PWR_CHARGER_RECONIFG_PERIOD_MS) {
        last_reconfig_ms = k_uptime_get();
        LOG_INF("Re-applying PMIC configuration");
        if (reapply_config() < 0) {
          err = -1;
        };
      }

      k_mutex_unlock(&pwr_mutex);
    }

    if (err < 0) {
      LOG_ERR("Error in power thread!");
    }

    if (events != 0 && pwr_event_cb != NULL) {
      (void)pwr_get_status(&status_copy);
      pwr_event_cb(events, &status_copy, pwr_event_cb_data);
    }

    // Update on pmic interrupt but at least every PWR_THREAD_UPDATE_PERIOD_MS
    k_sem_take(&pmic_update_sem, K_MSEC(THREAD_PWR_UPDATE_PERIOD_MS));
  }
}

// ======== Functions ==============================================================================

static void pmic_int_cb_handler(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins) {
  LOG_DBG("PMIC interrupt!");
  pmic_irq_ms = k_uptime_get_32();
  k_sem_give(&pmic_update_sem);
}

static void softRst_int_cb_handler(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins) {
  LOG_DBG("Soft RST interrupt!");
  keyup_ms = k_uptime_get_32();
  atomic_set(&keyup_pending, 1);
  k_sem_give(&pmic_update_sem);
}

static int check_status() {
  // Flags of a failed read must not be taken as new events:
  memset(&pmic.flags, 0, sizeof(pmic.flags));
  flags_valid = false;
  status_valid = false;

  // Grab pmic flags to clear interrupt:
  if (max77654_get_int_flags(&pmic_h, &pmic.flags) != E_MAX77654_SUCCESS) {
    LOG_ERR("pmic get_int_flags failed!");
    memset(&pmic.flags, 0, sizeof(pmic.flags));
    return -1;
  }
  flags_valid = true;

  // Get current status:
  if (max77654_get_stat(&pmic_h, &pmic.status) != E_MAX77654_SUCCESS) {
    LOG_ERR("pmic get_stat failed!");
    return -1;
  }
  status_valid = true;

  // Notify state machine of current charger status:
  // bool chg_active = status.chgin_status != MAX77654_CHGIN_DTLS_UVLO;
  // LOG_INF("Charger %s\r\n", chg_active ? "active" : "inactive");

  return 0;
}

static int handle_interrupts() {
  // Check PMIC interrupt flags if button was pressed:
  if (pmic.flags.nen_f) {
#ifdef CONFIG_SENSEI_GAP9
    // The GAP9 link owns the GAP9 rail: it power cycles GAP9 on its own thread and keeps its state right.
    gap9_link_reset();
#else
    if (atomic_get(&gap9_pwr_state) == 1) {
      // Reset GAP9
      LOG_WRN("Resetting GAP9");
      if (gpio_pin_set_dt(&gpio_gap9_pwr_sel, 0) < 0) {
        LOG_ERR("GAP9 EN GPIO configuration error");
        return -1;
      }
      k_msleep(100);
      if (gpio_pin_set_dt(&gpio_gap9_pwr_sel, 1) < 0) {
        LOG_ERR("GAP9 EN GPIO configuration error");
        return -1;
      }
    } else {
      LOG_INF("GAP9 is off. Ignoring reset.");
    }
#endif
  }

  return 0;
}

static int check_battery() {

  // Measure battery 3 times:
  uint32_t battery_mV_avg = 0;
  uint32_t measurements = 0;

  for (uint32_t i = 0; i < 3; i++) {
    uint32_t result;
    if (max77654_measure(&pmic_h, MAX77654_BATT_V, &result) != E_MAX77654_SUCCESS) {
      LOG_ERR("pmic measure_bat failed!");
      return -1;
    }

    // Ignore any measurements below 100mV:
    // Usually the first ever ADC reading is 0-10mV, this prevents
    // this reading (or other fluke readings) from triggering a low/critical battery
    // warning.
    if (result <= 100) {
      LOG_ERR("Ignoring measurement %i mV", result);
    } else {
      measurements++;
      battery_mV_avg += result;
    }

    k_sleep(K_MSEC(1));
  }

  // If however, all measurements were invalid, fail:
  if (measurements == 0) {
    LOG_ERR("All measurements invalid!");
    return -1;
  }

  // Average the performed measurements:
  battery_mV_avg /= measurements;

  // LOG_INF("Battery Average Voltage: %i mV", battery_mV_avg);

  // Store battery mV:
  atomic_set(&battery_mV, battery_mV_avg);

  // Convert the mV reading to percentage and store:
  atomic_set(&battery_perc, battery_perc_conversion(battery_mV_avg));

  return 0;
}

int measure_currents() {
  int current;

  // Array with name, unit and index
  const struct {
    const char *name;
    const char *unit;
    int index;
  } current_names[] = {
      {"CHGIN Current             ", "mA", MAX77654_CHGIN_I},
      {"Battery Current           ", "%", MAX77654_BATT_I_CHG},
      {"Battery Discharge Current (   8.2 mA)", "mA", MAX77654_BATT_I_8MA2},
      {"Battery Discharge Current ( 40.5 mA)", "mA", MAX77654_BATT_I_40MA5},
      {"Battery Discharge Current ( 72.3 mA)", "mA", MAX77654_BATT_I_72MA3},
      {"Battery Discharge Current (103.4 mA)", "mA", MAX77654_BATT_I_103MA4},
      {"Battery Discharge Current (134.1 mA)", "mA", MAX77654_BATT_I_134MA1},
      {"Battery Discharge Current (164.1 mA)", "mA", MAX77654_BATT_I_164MA1},
      {"Battery Discharge Current (193.7 mA)", "mA", MAX77654_BATT_I_193MA7},
      {"Battery Discharge Current (222.7 mA)", "mA", MAX77654_BATT_I_222MA7},
      {"Battery Discharge Current (251.2 mA)", "mA", MAX77654_BATT_I_251MA2},
      {"Battery Discharge Current (279.3 mA)", "mA", MAX77654_BATT_I_279MA3},
      {"Battery Discharge Current (300.0 mA)", "mA", MAX77654_BATT_I_300MA},
  };

  // Iterate over all max77654_measure_t types:
  LOG_INF("Measuring PMIC Currents:");
  for (uint32_t i = 0; i < sizeof(current_names) / sizeof(current_names[0]); i++) {
    if (max77654_measure(&pmic_h, current_names[i].index, &current) != E_MAX77654_SUCCESS) {
      LOG_ERR("pmic measure failed!");
      return -1;
    }
    LOG_INF("  %s: %i %s", current_names[i].name, current, current_names[i].unit);
  }

  return 0;
}

int measure_voltages() {
  int voltage;

  // Array with name, unit and index
  const struct {
    const char *name;
    const char *unit;
    int index;
  } voltage_names[] = {
      {"AGND Voltage              ", "mV", MAX77654_AGND},
      {"VSYS Voltage              ", "mV", MAX77654_VSYS},
      {"CHGIN Voltage             ", "mV", MAX77654_CHGIN_V},
      {"CHGIN Current             ", "mA", MAX77654_CHGIN_I},
      {"Battery Voltage           ", "mV", MAX77654_BATT_V},
      {"Battery Current           ", "%", MAX77654_BATT_I_CHG},
      {"Battery Discharge Current ", "mA", MAX77654_BATT_I_8MA2},
      {"Thermistor Voltage        ", "mV", MAX77654_THM},
      {"Thermistor Bias           ", "mV", MAX77654_TBIAS},
  };

  // Iterate over all max77654_measure_t types:
  LOG_INF("Measuring PMIC Voltages:");
  for (uint32_t i = 0; i < sizeof(voltage_names) / sizeof(voltage_names[0]); i++) {
    if (max77654_measure(&pmic_h, voltage_names[i].index, &voltage) != E_MAX77654_SUCCESS) {
      LOG_ERR("pmic measure failed!");
      return -1;
    }
    LOG_INF("  %s: %i %s", voltage_names[i].name, voltage, voltage_names[i].unit);
  }

  return 0;
}

static int reapply_config() {

  // Apply charger/global resource config to PMIC:
  if (max77654_config_charger(&pmic_h) != E_MAX77654_SUCCESS) {
    LOG_ERR("PMIC config_charger failed!");
    return -1;
  }

  if (max77654_config_global(&pmic_h) != E_MAX77654_SUCCESS) {
    LOG_ERR("PMIC config_charger failed!");
    return -1;
  }
  return 0;
}

static uint32_t battery_perc_conversion(uint32_t mV) {
  return sensei_battery_perc(
      IS_ENABLED(CONFIG_SENSEI_PWR_BATTERY_LIION) ? SENSEI_BATTERY_LIION : SENSEI_BATTERY_LISOCL2, mV);
}

// ======== Thread Setup ===========================================================================

static K_THREAD_STACK_DEFINE(thread_pwr_stack, 1024);
static struct k_thread thread_pwr_data;
k_tid_t pwr_t;

void thread_pwr_start() {

  pwr_t = k_thread_create(&thread_pwr_data,                                          // Thread data
                          thread_pwr_stack, K_THREAD_STACK_SIZEOF(thread_pwr_stack), // Thread Stack
                          thread_pwr,                                                // Entry
                          NULL, NULL, NULL,                                          // Args...
                          CONFIG_PWR_THREAD_PRIORITY,                                // Priority
                          0,                                                         // Options
                          K_NO_WAIT                                                  // Delay
  );
  k_thread_name_set(pwr_t, "pwr");
}

// ======== Status and events ======================================================================

static enum pwr_charger_state charger_state_from_dtls(max77654_chg_dtls_t d) {
  switch (d) {
  case MAX77654_CHG_DTLS_OFF:
    return PWR_CHG_OFF;
  case MAX77654_CHG_DTLS_PREQUAL:
    return PWR_CHG_PREQUAL;
  case MAX77654_CHG_DTLS_CC:
  case MAX77654_CHG_DTLS_CC_JEITA:
    return PWR_CHG_CC;
  case MAX77654_CHG_DTLS_CV:
  case MAX77654_CHG_DTLS_CV_JEITA:
    return PWR_CHG_CV;
  case MAX77654_CHG_DTLS_TOP_OFF:
  case MAX77654_CHG_DTLS_TOP_OFF_JEITA:
    return PWR_CHG_TOP_OFF;
  case MAX77654_CHG_DTLS_DONE:
  case MAX77654_CHG_DTLS_DONE_JEITA:
    return PWR_CHG_DONE;
  default:
    return PWR_CHG_FAULT;
  }
}

// Called by the power thread with the power mutex held, after check_status() and check_battery(), when the flags
// were read.
static uint32_t collect_events() {
  uint32_t events = 0;
  uint32_t mv = (uint32_t)atomic_get(&battery_mV);
  uint8_t cls = battery_class;

  if (pmic.flags.chgin) {
    events |= PWR_EVT_CHGIN_CHANGE;
  }
  if (pmic.flags.chg) {
    events |= PWR_EVT_CHG_CHANGE;
  }
  if (pmic.flags.chgin_ctrl) {
    events |= PWR_EVT_CHGIN_CTRL;
  }
  if (pmic.flags.nen_f) {
    events |= PWR_EVT_BUTTON;
  }
  if (pmic.flags.tj_reg || pmic.flags.tjal1_r || pmic.flags.tjal2_r) {
    events |= PWR_EVT_THERMAL;
  }

  // Three levels with hysteresis: LOW is left upwards only above CRITICAL + 100 mV, OK is entered at
  // PWR_BAT_OK_MV. Between LOW and OK the class stays as it was.
  if (mv != 0) {
    if (mv <= PWR_BAT_CRITICAL_MV) {
      cls = 1;
    } else if (mv <= PWR_BAT_LOW_MV) {
      cls = (cls == 1 && mv <= PWR_BAT_CRITICAL_MV + 100) ? 1 : 2;
    } else if (mv >= PWR_BAT_OK_MV || cls == 0) {
      cls = 3;
    }
  }
  if (cls != battery_class) {
    // The first reading only sets the class, it is no event.
    if (battery_class != 0) {
      events |= (cls == 3) ? PWR_EVT_BAT_OK : (cls == 2) ? PWR_EVT_BAT_LOW : PWR_EVT_BAT_CRITICAL;
    }
    battery_class = cls;
  }

  k_spinlock_key_t key = k_spin_lock(&pwr_status_lock);
  pwr_status_last.bat_mv = mv;
  pwr_status_last.bat_perc = (uint32_t)atomic_get(&battery_perc);
  if (status_valid) {
    // A failed status read keeps the last charger state.
    pwr_status_last.charger = charger_state_from_dtls(pmic.status.charger_status);
    pwr_status_last.chgin = (enum pwr_chgin_state)pmic.status.chgin_status;
    pwr_status_last.charging = pmic.status.charging_active;
    pwr_status_last.valid = true;
  }
  k_spin_unlock(&pwr_status_lock, key);

  return events;
}

int pwr_get_status(struct pwr_status *status) {
  k_spinlock_key_t key = k_spin_lock(&pwr_status_lock);
  *status = pwr_status_last;
  k_spin_unlock(&pwr_status_lock, key);
  return status->valid ? 0 : -EAGAIN;
}

void pwr_set_event_cb(pwr_event_cb_t cb, void *user_data) {
  pwr_event_cb_data = user_data;
  pwr_event_cb = cb;
}

void pwr_refresh(void) { k_sem_give(&pmic_update_sem); }
