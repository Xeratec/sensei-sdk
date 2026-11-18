/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_pwr_math.h
 * @brief Pure logic of the power module (NRF/system/pwr): battery charge estimate and button press detection.
 * Plain C, no Zephyr, so the host tests use the same code.
 */

#ifndef SENSEI_PWR_MATH_H
#define SENSEI_PWR_MATH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Battery chemistries of sensei_battery_perc(). */
enum sensei_battery_chem {
  SENSEI_BATTERY_LISOCL2, /**< Li-SOCl2 primary cell, 3.6 V nominal (Saft LS33600) */
  SENSEI_BATTERY_LIION,   /**< Li-ion or LiPo, 4.2 V full */
};

/**
 * @brief Rough charge estimate in percent from the battery voltage (0 to 100).
 *
 * Linear between table points. A Li-SOCl2 cell has a flat curve, so its value is coarse: use it as a hint, not as
 * a fuel gauge.
 */
uint8_t sensei_battery_perc(enum sensei_battery_chem chem, uint32_t mv);

/** @brief State of one push button. Start from a zeroed struct. */
struct sensei_button {
  bool pressed;     /**< a key-down was seen and the key-up did not follow yet */
  uint32_t down_ms; /**< time of the key-down */
};

/** @brief Result of a key-up. */
enum sensei_button_press {
  SENSEI_BUTTON_NONE,  /**< no key-down before: the edge is ignored */
  SENSEI_BUTTON_SHORT, /**< pressed for long_ms or less (or a stale key-down, see sensei_button_up()) */
  SENSEI_BUTTON_LONG,  /**< pressed for more than long_ms and not more than max_ms */
};

/** @brief Key-down. A second key-down without a key-up replaces the first time (its key-up was missed). */
void sensei_button_down(struct sensei_button *b, uint32_t now_ms);

/** @brief Key-up. Returns what the press was, and clears the state. Presses are measured with a signed 32-bit
 * difference of the times: they wrap after 49 days, a press must be shorter than 24 days, and a key-up that is
 * earlier than the key-down is a short press. A press longer than max_ms (0: no limit) cannot be real, its key-up was
 * lost and this edge belongs to something else: it is a short press, never a long one. */
enum sensei_button_press sensei_button_up(struct sensei_button *b, uint32_t now_ms, uint32_t long_ms, uint32_t max_ms);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_PWR_MATH_H */
