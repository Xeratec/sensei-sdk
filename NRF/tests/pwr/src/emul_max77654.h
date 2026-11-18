/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#ifndef SENSEI_TEST_EMUL_MAX77654_H_
#define SENSEI_TEST_EMUL_MAX77654_H_

#include <stdbool.h>
#include <stdint.h>

#define PMIC_EMUL_REGS 0x40
#define PMIC_EMUL_WLOG_LEN 64

/* Time the PMIC does not answer after a soft reset (the power module waits 300 ms). */
#define PMIC_EMUL_RESET_BUSY_MS 250

struct pmic_wlog {
  int64_t t_ms;
  uint8_t reg;
  uint8_t val;
};

struct pmic_emul {
  /* Test controls */
  bool present;                  /* false: no ACK */
  uint8_t cid;                   /* answer of the CID register, the driver expects 0x02 */
  int fail_read_reg;             /* NACK a read that touches this register (-1: none) */
  int fail_write_reg;            /* NACK a write to this register (-1: none) */
  void (*first_access_cb)(void); /* called once, at the first transfer (before it is handled) */

  /* Observations */
  int transfers;
  int nacks;
  int early;                                 /* of these: while the PMIC power-cycles after a reset (a missing wait) */
  int resets;                                /* soft resets (CNFG_GLBL, SFT_CTRL = SFT_CRST) */
  int64_t reset_ms;                          /* time of the last one */
  struct pmic_wlog wlog[PMIC_EMUL_WLOG_LEN]; /* the first writes, in order */
  int wlog_len;
  int64_t last_write_ms;
  int wcount[PMIC_EMUL_REGS]; /* writes per register */
};

struct pmic_emul *pmic_emul_get(void);

/* Default state: present, CID 2, no flags, nothing in the log. Registers are not touched (see pmic_emul_por()). */
void pmic_emul_reset_test_state(void);

/* Register contents after power-up (all rails off). */
void pmic_emul_por(void);

/* Raw register access for the test. */
uint8_t pmic_emul_reg(uint8_t reg);
void pmic_emul_set_reg(uint8_t reg, uint8_t val);

/* Interrupt flags that the next read of INT_GLBL0, INT_GLBL1 and INT_CHG returns (they clear on read). */
void pmic_emul_raise(uint8_t glbl0, uint8_t glbl1, uint8_t chg);
/* True when a flag is still pending (not read yet). */
bool pmic_emul_pending(void);

/* STAT_CHG_B: charger details (CHG_DTLS, 0..12), input details (CHGIN_DTLS, 0..3), charging. */
void pmic_emul_set_charger(uint8_t chg_dtls, uint8_t chgin_dtls, bool charging);

/* Output voltage of a rail in mV as the registers set it, 0 when it is not enabled. */
int pmic_emul_sbb_mv(int sbb);
int pmic_emul_ldo_mv(int ldo);
/* Charger: enable bit, charge voltage (mV), fast charge current (x10 mA). */
bool pmic_emul_charger_enabled(void);
int pmic_emul_charge_cv_mv(void);
int pmic_emul_charge_cc_x10ma(void);
/* AMUX selection (0 = off). */
int pmic_emul_amux(void);

#endif /* SENSEI_TEST_EMUL_MAX77654_H_ */
