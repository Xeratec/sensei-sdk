/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_shield.h
 * @brief Is an LPWAN shield fitted, what the start-up check does about a shield that is there but not usable, and
 * the levels of the nRF21540 lines on it (pure, host tested).
 *
 * The client decides once, at start-up, whether a shield is fitted: the shield carries the I/O expander (TCAL6408,
 * I2C 0x20) and nothing else on the stack answers there. Three outcomes, kept until the next reboot:
 * - PRESENT: the expander answers and its pins and the EVENT interrupt are set up. The client runs.
 * - ABSENT: nothing at 0x20 answers like the expander. No shield: the client never powers anything, never retries and
 *   never starts its thread.
 * - FAULT: the expander answers, but the driver did not come up, its pins did not take the first writes (after a few
 *   tries) or the interrupt setup failed. A shield is there and may be powered: the 1 M pull-up of EN_3V3 turns the
 *   3.3 V on while the expander pins are inputs. The client tries to drive EN_3V3 low (also over plain I2C if the
 *   driver is down), and does not start.
 * In both cases a node sends over BLE whatever its radio setting says; a shield stacked later is used after the next
 * boot.
 *
 * nRF21540 range extender (BLE) on the shield: its power-down line (expander P5, nPD, low = off) and MODE (P6) are
 * driven by the client. With the extender not in use (the default) both are low from the first start-up of the
 * client until the node resets: a power-down extender draws no current and the expander never leaves the pins
 * floating. In use, nPD is raised after the shield's 3.3 V is on and lowered before it is cut.
 */

#ifndef SENSEI_LPWAN_SHIELD_H_
#define SENSEI_LPWAN_SHIELD_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum lpwan_shield_state {
  LPWAN_SHIELD_UNKNOWN, /* the client has not looked yet (or is not built in) */
  LPWAN_SHIELD_PRESENT, /* the expander answered and is set up */
  LPWAN_SHIELD_ABSENT,  /* nothing answers like the expander: no shield */
  LPWAN_SHIELD_FAULT,   /* the expander answers but cannot be used: a shield that may be powered */
};

/** Tries for the first writes to the expander's pins (a one-off I2C error must not make a shield unusable). */
#define LPWAN_SHIELD_PIN_TRIES 3

/** The hardware side of the start-up check, behind a table so the order and the retries are host tested. */
struct lpwan_shield_ops {
  /** True if the expander answers at 0x20 as a TCAL6408 does: its output drive strength register (0x40) reads 0xFF
   *  (a PCA6416A has no such register). Plain I2C, whether the driver came up or not. */
  bool (*expander_answers)(void *ctx);
  /** The shield's devices came up: SPI, the event pin and the expander with its pins. */
  bool (*devices_ready)(void *ctx);
  /** Shield off, the pins of the link set (EN_3V3 low, the extender's lines low). 0 or a negative errno. */
  int (*pins_init)(void *ctx);
  /** The EVENT interrupt. 0 or a negative errno. */
  int (*irq_setup)(void *ctx);
  /** Best effort: drive EN_3V3 low (the driver, else plain I2C). */
  void (*power_cut)(void *ctx);
  /** Nothing is there: give back the pins that a failed driver left set (the expander's INT line). */
  void (*release)(void *ctx);
  /** Wait a little between two tries of pins_init(); may be NULL. */
  void (*pause)(void *ctx);
};

/**
 * The start-up check. Nothing at 0x20 (expander_answers false, also for another chip there): release() and ABSENT.
 * An expander without a driver, pins that fail LPWAN_SHIELD_PIN_TRIES times or an interrupt that cannot be set up:
 * power_cut() and FAULT. Otherwise PRESENT. Never UNKNOWN.
 */
enum lpwan_shield_state lpwan_shield_start(const struct lpwan_shield_ops *ops, void *ctx);

/** "unknown", "present", "absent" or "fault". */
const char *lpwan_shield_name(enum lpwan_shield_state s);

/** Levels of the two nRF21540 lines on the expander. */
struct lpwan_fem_levels {
  bool npd_high;  /* P5 nRF21_nPD: high = the extender is powered up */
  bool mode_high; /* P6 nRF21_MODE */
};

/**
 * What the client drives. `fem_in_use`: the extender is wanted (CONFIG_SENSEI_LPWAN_FEM, off by default);
 * `shield_on`: the shield's 3.3 V is on. Not in use: both low, always. In use: nPD high only while the shield is on
 * (the client calls this after the power-on and before the power-off), MODE as `mode_high` says (the one that
 * CONFIG_SENSEI_LPWAN_FEM_MODE_HIGH sets).
 */
struct lpwan_fem_levels lpwan_fem_levels(bool fem_in_use, bool shield_on, bool mode_high);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_SHIELD_H_ */
