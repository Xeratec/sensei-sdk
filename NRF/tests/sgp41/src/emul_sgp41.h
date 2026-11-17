/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#ifndef SENSEI_TEST_EMUL_SGP41_H_
#define SENSEI_TEST_EMUL_SGP41_H_

#include <stdbool.h>
#include <stdint.h>

#include "sensirion_emul.h"

struct sgp41_emul {
  /* Test controls */
  bool present;       /* false: the chip does not answer (no ACK) */
  uint16_t self_test; /* answer of execute_self_test: 0xD400 passed; bit 0 VOC pixel, bit 1 NOx pixel failed */
  uint16_t voc_raw;   /* SRAW_VOC and SRAW_NOX of the next commands */
  uint16_t nox_raw;
  int fail_after; /* NACK every transfer after this many (-1: never) */

  /* Observations */
  int transfers;
  int nacks;
  int early; /* sent while the sensor executes a command (a missing wait) */
  uint16_t log[SENSIRION_EMUL_LOG_LEN];
  int log_len;
  uint16_t last_rh; /* arguments of the last conditioning or measure command, ticks */
  uint16_t last_t;
  bool heater_on;       /* measurement mode (after conditioning or measure, until turn_heater_off or power loss) */
  int conditioning;     /* conditioning commands since the heater went on */
  int over_conditioned; /* conditioning commands more than 10 s after the first one (datasheet: 10 s at most) */
  int unconditioned;    /* measure commands without conditioning before them */
};

/* The one emulated SGP41. */
struct sgp41_emul *sgp41_emul_get(void);

/* Default state: present, self test passed, VOC 30000 and NOx 15000 ticks. Clears the log and the counters. */
void sgp41_emul_reset(void);

/* The sensor lost its supply: idle mode, no conditioning. */
void sgp41_emul_power_loss(void);

int sgp41_emul_count(uint16_t cmd);

/* Command codes (SGP41 datasheet v1.0, table 8). */
#define SGP41_CMD_CONDITIONING 0x2612
#define SGP41_CMD_MEASURE_RAW 0x2619
#define SGP41_CMD_SELF_TEST 0x280E
#define SGP41_CMD_HEATER_OFF 0x3615
#define SGP41_CMD_SERIAL 0x3682

#endif /* SENSEI_TEST_EMUL_SGP41_H_ */
