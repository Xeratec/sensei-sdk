/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_shield.h
 * @brief Is an LPWAN shield fitted, and the levels of the nRF21540 lines on it (pure, host tested).
 *
 * The client decides once, at start-up, whether a shield is fitted: the shield carries the I/O expander (TCAL6408,
 * I2C 0x20) and nothing else on the stack answers there. If the expander does not answer, its driver does not come up
 * and the client treats the shield as absent until the next reboot. It never powers the shield, never retries and
 * never starts its thread. A node then sends over BLE whatever its radio setting says, and a shield stacked later is
 * used after the next boot.
 *
 * nRF21540 range extender (BLE) on the shield: its power-down line (expander P5, nPD, low = off) and MODE (P6) are
 * driven by the client. With the extender not in use (the default) both are low, from the first start-up of the
 * expander until the node resets: a power-down extender draws no current and the expander never leaves the pins
 * floating.
 */

#ifndef SENSEI_LPWAN_SHIELD_H_
#define SENSEI_LPWAN_SHIELD_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum lpwan_shield_state {
  LPWAN_SHIELD_UNKNOWN, /* the client has not looked yet (or is not built in) */
  LPWAN_SHIELD_PRESENT, /* the expander answered */
  LPWAN_SHIELD_ABSENT,  /* it did not: no shield, or one that cannot be used */
};

/**
 * The state after the start-up check. `devices_ready`: the shield's devices (SPI, event pin, the expander and its
 * pins) came up; `pins_ok`: the first writes to the expander's pins went through (only looked at if the devices are
 * ready). Anything else is absent.
 */
enum lpwan_shield_state lpwan_shield_probe(bool devices_ready, bool pins_ok);

/** "unknown", "present" or "absent". */
const char *lpwan_shield_name(enum lpwan_shield_state s);

/** Levels of the two nRF21540 lines on the expander. */
struct lpwan_fem_levels {
  bool npd_high;  /* P5 nRF21_nPD: high = the extender is powered up */
  bool mode_high; /* P6 nRF21_MODE */
};

/**
 * What the client drives. `fem_in_use`: the extender is wanted (CONFIG_SENSEI_LPWAN_FEM, off by default);
 * `shield_on`: the shield's 3.3 V is on. Not in use: both low, always. In use: nPD high only while the shield is on,
 * MODE as `mode_high` says (the one that CONFIG_SENSEI_LPWAN_FEM_MODE_HIGH sets).
 */
struct lpwan_fem_levels lpwan_fem_levels(bool fem_in_use, bool shield_on, bool mode_high);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_SHIELD_H_ */
