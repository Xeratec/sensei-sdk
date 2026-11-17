/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#ifndef SENSEI_TEST_EMUL_BH1730_H_
#define SENSEI_TEST_EMUL_BH1730_H_

#include <stdbool.h>
#include <stdint.h>

#define BH1730_EMUL_LOG_LEN 64

struct bh1730_emul {
  /* Test controls */
  bool present;    /* false: the chip does not answer (no ACK) */
  uint8_t part_id; /* register 0x12, 0x7B on a real chip (part number 0111, revision does not matter) */
  uint16_t data0;  /* result of the next measurement: visible + IR channel (Type0) and IR channel (Type1) */
  uint16_t data1;
  bool never_valid; /* a measurement never finishes */
  int fail_after;   /* NACK every transfer after this many (-1: never) */

  /* Observations */
  int transfers;
  int nacks;
  int measurements;                /* started */
  int writes[BH1730_EMUL_LOG_LEN]; /* register writes: (register << 8) | value, in order */
  int n_writes;
  uint8_t reg[0x18]; /* register file (CONTROL bits 4 and 5, ID and data registers are computed on a read) */
};

struct bh1730_emul *bh1730_emul_get(void);

/* Registers at their reset values (power-down), present, 0x7B, no data. Clears the log and the counters. */
void bh1730_emul_reset(void);

/* CONTROL register as a read would show it now. */
uint8_t bh1730_emul_control(void);

/* Measurement time (datasheet: ITIME_ms = Tint * 964 * (256 - ITIME), Tmt = ITIME_ms + Tint * 714, Tint 2.8 us), us. */
uint32_t bh1730_emul_meas_us(uint8_t itime);

#endif /* SENSEI_TEST_EMUL_BH1730_H_ */
