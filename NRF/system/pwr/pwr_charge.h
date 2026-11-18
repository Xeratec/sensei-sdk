/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file pwr_charge.h
 * @brief Battery charge counter of the power module (CONFIG_SENSEI_PWR_CHARGE): the Zephyr side of lib/sensei_charge.
 *
 * The application gives the table and the settings, tells the module which states are on and which events happened
 * (pwr_charge_state(), pwr_charge_event()), and reads the report. The module keeps the counter in a retained RAM
 * record and in the settings (flash), feeds it the battery voltage, the external power state and the AMUX samples of
 * the power thread, and resets it when a Li-ion charger is done. Untested on hardware.
 */

#ifndef PWR_CHARGE_H_
#define PWR_CHARGE_H_

#include <stdbool.h>
#include <stdint.h>

#include "pwr/pwr.h"
#include "sensei_charge.h"

/** Settings of the counter and what the start knows. */
struct pwr_charge_cfg {
  const struct sensei_charge_table *table; /**< consumption table, must stay valid */
  uint32_t capacity_mah;                   /**< 0: the build default (flagged) */
  uint32_t usable_pct;                     /**< usable share of the capacity for days left, 0: 90 */
  uint32_t floor_ua;                       /**< sleep floor of this board at 3.6 V (uA) */
  uint32_t addon_ua;                       /**< constant extra current of the settings at 3.6 V (uA) */
  bool power_on_reset;                     /**< the reset cause was a power-on reset */
  bool brownout_reset;                     /**< the reset cause was a brown-out */
  uint32_t (*wall_s)(void);                /**< unix time in s, 0 if not set; may be NULL */
};

/** Called when the swap button (three short presses, first 5 min after a power-on reset) declared a new battery. */
typedef void (*pwr_charge_swap_cb_t)(void);

/**
 * @brief Start the counter. Call after pwr_init() (battery voltage) and after the settings are loaded
 * (settings_subsys_init()), once. Restores the retained record, else the settings record, else starts at zero;
 * applies the swap rules and writes both records.
 * @return 0, or a negative errno (-EALREADY, -EINVAL)
 */
int pwr_charge_init(const struct pwr_charge_cfg *cfg);

/** @brief Settings changed (capacity, usable share, floor, add-on). Same meaning as in pwr_charge_cfg. */
void pwr_charge_configure(uint32_t capacity_mah, uint32_t usable_pct, uint32_t floor_ua, uint32_t addon_ua);

/** @brief A state turns on or off (ref-counted). Cheap, from any thread or interrupt. Ignored before pwr_charge_init().
 */
void pwr_charge_state(unsigned id, bool on);

/** @brief An event happened n times. Cheap, from any thread or interrupt. Ignored before pwr_charge_init(). */
void pwr_charge_event(unsigned id, uint32_t n);

/** @brief Battery temperature in 0.1 degC (for the self-discharge). */
void pwr_charge_set_temp(int32_t t_c10);

/** @brief Report of the counter. @return 0, or -EAGAIN before pwr_charge_init() */
int pwr_charge_report(struct sensei_charge_report *r);

/** @brief Copy the ledger: seconds per state and counts per event (sizes of the table). @return 0, or -EAGAIN */
int pwr_charge_ledger(uint32_t *state_s, uint32_t *event_n);

/** @brief The battery is new (or part used, used_mah): restart the counter. capacity_mah 0 keeps the capacity. Saves at
 * once. */
int pwr_charge_new(uint32_t capacity_mah, uint32_t used_mah, uint32_t battery_id);

/** @brief Write the counter to the settings now (before a planned reboot, with the daily status). May block on flash.
 */
int pwr_charge_save(void);

/**
 * @brief AMUX sample of the battery current for a check class of the table (see sensei_charge_check_sample()). Takes
 * the power mutex, so it must not run in an interrupt. The class must have an expected current in the table.
 * @return 0, -ENOTSUP (class has no expected current), -EBUSY (power mutex), or a PMIC error
 */
int pwr_charge_check(unsigned cls);

/** @brief Called once when the swap button declares a new battery (blink the LED). NULL removes it. */
void pwr_charge_set_swap_cb(pwr_charge_swap_cb_t cb);

/** @brief A short press of the power button (thread_pwr.c). The third one within 3 s in the first 5 min after a
 * power-on reset declares a new battery. */
void pwr_charge_button_short(uint32_t now_ms);

/* ---- used by thread_pwr.c, with the power mutex held ---- */

/**
 * @brief Pass of the power thread: voltage, external power, charger state; Li-ion charge current and "done"; the
 * floor watchdog sample; accounting up to now; retained record; save timer.
 */
void pwr_charge_pmic_tick(uint32_t bat_mv, bool chgin_ok, bool charging, enum pwr_charger_state charger_state);

#endif /* PWR_CHARGE_H_ */
