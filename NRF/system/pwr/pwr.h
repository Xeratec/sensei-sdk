/*
 * ----------------------------------------------------------------------
 *
 * File: pwr.h
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

#ifndef PWR_H_
#define PWR_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

/*
 * Battery level thresholds in mV. They follow the battery chemistry of CONFIG_SENSEI_PWR_BATTERY_*, see
 * pwr/Kconfig. The battery table and the charger voltage follow the same setting.
 */
#define PWR_BAT_CRITICAL_MV (CONFIG_SENSEI_PWR_BAT_CRITICAL_MV)
#define PWR_BAT_LOW_MV (CONFIG_SENSEI_PWR_BAT_LOW_MV)
#define PWR_BAT_OK_MV (CONFIG_SENSEI_PWR_BAT_OK_MV)

/**
 * @brief Prepare the power sub-system: pin set-up, board hooks, battery reading.
 *
 * The PMIC itself is already up: a boot-time hook (SYS_INIT) resets it and, with CONFIG_SENSEI_PWR_BOOT_RAILS, turns
 * the shield rails on before the shield drivers start. Call this once from main().
 * @return 0, or a negative value on error
 */
int pwr_init(void);

/**
 * @brief Start the power sub-system: board hook (switches), LED supply and the power thread.
 * @note Call once, after pwr_init().
 * @return 0, or a negative value on error
 */
int pwr_start(void);

/**
 * @brief Switch the PMIC rails off and reboot the node. Does not return.
 *
 * Used for the long button press. It retries the PMIC write a few times, then reboots anyway.
 */
void pwr_off_and_reboot(void);

/** @brief Log the PMIC voltages (bring-up). Takes the power mutex. @return 0, or a negative value on error */
int pwr_log_voltages(void);

/** @brief Battery charge estimate in percent, from the last reading. Needs pwr_init(). */
uint32_t pwr_bat_perc(void);

/** @brief Battery voltage in mV, from the last reading (0 before the first). Needs pwr_init(). */
uint32_t pwr_bat_mV(void);

/**
 * @brief Switch the GAP9 supply (P1.15). In the node firmware the GAP9 link library calls this; applications
 * that use it need CONFIG_SENSEI_PWR.
 */
void gap9_pwr(bool on);

/** Charger state, from the MAX77654 CHG_DTLS field. */
enum pwr_charger_state {
  PWR_CHG_OFF = 0,     /**< not charging (no input, or battery full and charger idle) */
  PWR_CHG_PREQUAL = 1, /**< pre-qualification, battery deeply discharged */
  PWR_CHG_CC = 2,      /**< constant current */
  PWR_CHG_CV = 3,      /**< constant voltage */
  PWR_CHG_TOP_OFF = 4, /**< top-off */
  PWR_CHG_DONE = 5,    /**< charging done */
  PWR_CHG_FAULT = 6,   /**< timer or temperature fault */
};

/** Charger input (CHGIN) state. */
enum pwr_chgin_state {
  PWR_CHGIN_UVLO = 0,     /**< no input or too low */
  PWR_CHGIN_OVP = 1,      /**< input over-voltage */
  PWR_CHGIN_DEBOUNCE = 2, /**< input appeared, debouncing */
  PWR_CHGIN_OK = 3,       /**< input valid */
};

/** Snapshot of the power state, refreshed by the power thread (every 20 s and on PMIC interrupts). */
struct pwr_status {
  uint32_t bat_mv;                /**< battery voltage, from the PMIC AMUX through the SAADC (P0.07) */
  uint32_t bat_perc;              /**< rough charge estimate from the voltage */
  enum pwr_charger_state charger; /**< charger state */
  enum pwr_chgin_state chgin;     /**< charger input state */
  bool charging;                  /**< charger is delivering current */
  bool valid;                     /**< false until the power thread has read the PMIC once */
};

/** PMIC events for pwr_set_event_cb(). */
#define PWR_EVT_CHGIN_CHANGE BIT(0) /**< charger input status changed (cable in or out, over-voltage) */
#define PWR_EVT_CHG_CHANGE BIT(1)   /**< charger state changed (charging, done, fault) */
#define PWR_EVT_CHGIN_CTRL BIT(2)   /**< input under-voltage or over-current regulation */
#define PWR_EVT_BUTTON BIT(3)       /**< nEN pressed */
#define PWR_EVT_THERMAL BIT(4)      /**< thermal alarm or junction temperature regulation */
#define PWR_EVT_BAT_OK BIT(5)       /**< battery recovered above PWR_BAT_OK_MV */
#define PWR_EVT_BAT_LOW BIT(6)      /**< battery fell below PWR_BAT_LOW_MV */
#define PWR_EVT_BAT_CRITICAL BIT(7) /**< battery fell below PWR_BAT_CRITICAL_MV */

/**
 * @brief Event callback. Runs in the power thread after the PMIC was read, without the power mutex.
 * Keep it short; hand work to another thread.
 */
typedef void (*pwr_event_cb_t)(uint32_t events, const struct pwr_status *status, void *user_data);

/** @brief Register the event callback (one only). Pass NULL to remove it. */
void pwr_set_event_cb(pwr_event_cb_t cb, void *user_data);

/**
 * @brief Copy the last power status. Does not touch the PMIC, so it is cheap and safe from any thread.
 * @return 0, or -EAGAIN if the power thread has not read the PMIC yet
 */
int pwr_get_status(struct pwr_status *status);

/** @brief Wake the power thread to read the PMIC now instead of waiting for the 20 s period. */
void pwr_refresh(void);

#endif /* PWR_H_ */
