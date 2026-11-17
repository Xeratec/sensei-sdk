/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#ifndef SENSEI_TEST_EMUL_SCD41_H_
#define SENSEI_TEST_EMUL_SCD41_H_

#include <stdbool.h>
#include <stdint.h>

#include "sensirion_emul.h"

struct scd41_emul {
  /* Test controls */
  bool present;      /* false: the chip does not answer (no ACK) */
  uint16_t variant;  /* answer of get_sensor_variant, SCD41: 0x1xxx */
  uint16_t co2_ppm;  /* result of the next measurement */
  uint16_t temp_raw; /* raw words, as the sensor sends them */
  uint16_t rh_raw;
  uint16_t frc_result;  /* answer of perform_forced_recalibration */
  bool never_ready;     /* a measurement never finishes */
  bool ack_during_shot; /* answer commands during the 5 s of a single shot (datasheet: not allowed) */
  int fail_after;       /* NACK every transfer after this many (-1: never) */

  /* Observations */
  int transfers; /* all calls of the I2C transfer function */
  int nacks;     /* transfers refused: powered off, busy, asleep, bad CRC, unknown */
  int early;     /* of these: sent while the sensor executes a command or powers up (a missing wait) */
  uint16_t log[SENSIRION_EMUL_LOG_LEN]; /* commands accepted, in order */
  int log_len;
  bool asleep;      /* power_down received, no wake_up yet */
  bool asc_enabled; /* settings in the sensor RAM; the defaults come back after every power cycle */
  uint16_t asc_initial_h;
  uint16_t asc_standard_h;
  uint16_t pressure_hpa;
  int64_t shot_started_us; /* of the last single shot */
};

/* The one emulated SCD41. */
struct scd41_emul *scd41_emul_get(void);

/* Default state, present, 800 ppm, 25 degC, 50 %RH. Clears the log and the counters. */
void scd41_emul_reset(void);

/* Raw words of a temperature and humidity (datasheet: T = -45 + 175 * word / 2^16, RH = 100 * word / 2^16). */
uint16_t scd41_emul_temp_raw(int32_t mdeg);
uint16_t scd41_emul_rh_raw(int32_t mpct);

/* Power-up time after the supply: 1000 ms (datasheet 3.1). */
#define SCD41_POWER_UP_US (1000 * 1000)

/* Power loss: the sensor forgets its settings and wakes up idle (the test calls it after the rails went off). */
void scd41_emul_power_loss(void);

/* Count of an accepted command in the log. */
int scd41_emul_count(uint16_t cmd);

/* Command words used by the driver (Sensirion SCD4x datasheet, command tables). */
#define SCD41_CMD_STOP_PERIODIC 0x3F86
#define SCD41_CMD_READ_MEASUREMENT 0xEC05
#define SCD41_CMD_SET_AMBIENT_PRESSURE 0xE000
#define SCD41_CMD_FORCED_RECAL 0x362F
#define SCD41_CMD_SET_ASC_ENABLED 0x2416
#define SCD41_CMD_SET_ASC_INITIAL 0x2445
#define SCD41_CMD_SET_ASC_STANDARD 0x244E
#define SCD41_CMD_DATA_READY 0xE4B8
#define SCD41_CMD_GET_SERIAL 0x3682
#define SCD41_CMD_GET_VARIANT 0x202F
#define SCD41_CMD_SINGLE_SHOT 0x219D
#define SCD41_CMD_POWER_DOWN 0x36E0
#define SCD41_CMD_WAKE_UP 0x36F6

#endif /* SENSEI_TEST_EMUL_SCD41_H_ */
