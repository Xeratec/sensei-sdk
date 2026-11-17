/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* I2C emulator of the Sensirion SCD41 (address 0x62), for the driver "sensei,scd41".
 *
 * Follows the Sensirion SCD4x datasheet, version 1.3 (September 2022): 3.1 power-up (1000 ms to idle), 3.3 command
 * sequences (commands must not be sent while one is executed), 3.4 command table 9 (codes and execution times),
 * 3.5 basic commands, 3.6 output signal compensation, 3.7 field calibration (ASC, forced recalibration), 3.8 data
 * ready status, 3.9 advanced features, 3.10 low power single shot (SCD41), 3.11 CRC-8. Modelled:
 *  - no answer until 1000 ms after the supply came up (3.1);
 *  - commands with CRC-protected words and the execution times of table 9: the sensor does not acknowledge while it
 *    executes a command (stop_periodic_measurement 500 ms, forced recalibration 400 ms, wake_up 20 ms, others
 *    1 ms), so a missing wait of the library shows up as an early access;
 *  - single shot (SCD41 only, 3.10.1): the command takes 5000 ms and nothing is acknowledged meanwhile. With
 *    ack_during_shot the emulator answers data ready = 0 instead (what the driver's -EBUSY path is made for);
 *  - data ready status: 11 least significant bits zero = no data; read_measurement clears it, a read without
 *    data is not acknowledged;
 *  - power_down: only wake_up is accepted, and it is not acknowledged (datasheet: "the sensor does not
 *    acknowledge the wake up command");
 *  - ASC settings and ambient pressure in RAM: back to the defaults after a power loss (ASC on, 44 h, 156 h);
 *  - the sensor does not answer while one of the two supply rails (gpio-controlled regulators of the node) is off.
 * Not modelled: periodic measurement, temperature offset, altitude, persist settings, self test, factory reset. */

#define DT_DRV_COMPAT sensei_scd41

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "emul_scd41.h"

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "one SCD41 node");

static struct scd41_emul st;
static int64_t busy_until_us; /* the sensor executes a command until then */
static uint8_t resp[9];
static size_t resp_len;
static bool data_ready;
static int64_t data_ready_at_us;
#define RAIL_TICK_US (10 * 1000)      /* the timer sees the edge up to one tick late */
static int64_t rail_on_since_us = -1; /* set by the timer below */
static struct k_timer rail_timer;

static const struct gpio_dt_spec rail_vdd = GPIO_DT_SPEC_GET(DT_INST_PHANDLE(0, vdd_supply), enable_gpios);
static const struct gpio_dt_spec rail_bus = GPIO_DT_SPEC_GET(DT_INST_PHANDLE(0, bus_switch_supply), enable_gpios);

struct scd41_emul *scd41_emul_get(void) { return &st; }

uint16_t scd41_emul_temp_raw(int32_t mdeg) { return (uint16_t)(((int64_t)mdeg + 45000) * 65536 / 175000); }
uint16_t scd41_emul_rh_raw(int32_t mpct) { return (uint16_t)((int64_t)mpct * 65536 / 100000); }

static void settings_default(void) {
  st.asc_enabled = true;
  st.asc_initial_h = 44;
  st.asc_standard_h = 156;
  st.pressure_hpa = 1013;
}

/* Notes the moment the supply is on (10 ms resolution, the tick of native_sim): the 1000 ms power-up time run from
 * there. Supply off is a power loss: the sensor wakes up in idle with the default settings. */
static void rail_poll(struct k_timer *t) {
  ARG_UNUSED(t);
  bool on = gpio_emul_output_get_dt(&rail_vdd) == 1;

  if (!on) {
    if (rail_on_since_us >= 0) {
      scd41_emul_power_loss();
    }
    rail_on_since_us = -1;
  } else if (rail_on_since_us < 0) {
    rail_on_since_us = sensirion_emul_now_us();
  }
}

void scd41_emul_power_loss(void) {
  settings_default();
  st.asleep = false;
  busy_until_us = 0;
  rail_on_since_us = -1;
  resp_len = 0;
  data_ready = false;
}

void scd41_emul_reset(void) {
  memset(&st, 0, sizeof(st));
  st.present = true;
  st.variant = 0x1234; /* SCD41: upper nibble 0x1 */
  st.co2_ppm = 800;
  st.temp_raw = scd41_emul_temp_raw(25000);
  st.rh_raw = scd41_emul_rh_raw(50000);
  st.frc_result = 0x8000;
  st.fail_after = -1;
  scd41_emul_power_loss();
}

int scd41_emul_count(uint16_t cmd) {
  int n = 0;

  for (int i = 0; i < st.log_len; i++) {
    n += st.log[i] == cmd;
  }
  return n;
}

/* Command execution time in us, or -1 for an unknown command (datasheet command tables). */
static int32_t exec_us(uint16_t cmd) {
  switch (cmd) {
  case SCD41_CMD_STOP_PERIODIC:
    return 500 * 1000;
  case SCD41_CMD_FORCED_RECAL:
    return 400 * 1000;
  case SCD41_CMD_SINGLE_SHOT:
    return st.ack_during_shot ? 0 : 5000 * 1000;
  case SCD41_CMD_WAKE_UP:
    return 20 * 1000;
  case SCD41_CMD_READ_MEASUREMENT:
  case SCD41_CMD_SET_AMBIENT_PRESSURE:
  case SCD41_CMD_SET_ASC_ENABLED:
  case SCD41_CMD_SET_ASC_INITIAL:
  case SCD41_CMD_SET_ASC_STANDARD:
  case SCD41_CMD_DATA_READY:
  case SCD41_CMD_GET_SERIAL:
  case SCD41_CMD_GET_VARIANT:
  case SCD41_CMD_POWER_DOWN:
    return 1000;
  default:
    return -1;
  }
}

static bool rails_on(void) {
  return gpio_emul_output_get_dt(&rail_vdd) == 1 && gpio_emul_output_get_dt(&rail_bus) == 1;
}

static int nack(void) {
  st.nacks++;
  return -EIO;
}

static void log_cmd(uint16_t cmd) {
  if (st.log_len < SENSIRION_EMUL_LOG_LEN) {
    st.log[st.log_len++] = cmd;
  }
}

static int do_write(const uint8_t *buf, size_t len) {
  uint16_t cmd, arg[1] = {0};
  int64_t now = sensirion_emul_now_us();
  int n = sensirion_emul_parse(buf, len, &cmd, arg, 1);
  int32_t t;

  if (n < 0) {
    return nack();
  }
  if (now < busy_until_us) {
    st.early++;
    return nack();
  }
  if (st.asleep) {
    if (cmd == SCD41_CMD_WAKE_UP && n == 0) {
      st.asleep = false;
      busy_until_us = now + 20 * 1000;
      log_cmd(cmd);
    }
    return nack(); /* wake_up is not acknowledged either */
  }
  t = exec_us(cmd);
  if (t < 0) {
    return nack();
  }
  resp_len = 0;
  switch (cmd) {
  case SCD41_CMD_STOP_PERIODIC:
  case SCD41_CMD_POWER_DOWN:
  case SCD41_CMD_SINGLE_SHOT:
    if (n != 0) {
      return nack();
    }
    break;
  case SCD41_CMD_GET_SERIAL: {
    const uint16_t sn[3] = {0x1111, 0x2222, 0x3333};

    resp_len = sensirion_emul_put_words(resp, sn, 3);
    break;
  }
  case SCD41_CMD_GET_VARIANT:
    resp_len = sensirion_emul_put_words(resp, &st.variant, 1);
    break;
  case SCD41_CMD_DATA_READY: {
    uint16_t v = (data_ready && now >= data_ready_at_us) ? 0x8006 : 0x8000;

    resp_len = sensirion_emul_put_words(resp, &v, 1);
    break;
  }
  case SCD41_CMD_READ_MEASUREMENT:
    if (!data_ready || now < data_ready_at_us) {
      return nack(); /* nothing to read */
    }
    {
      const uint16_t w[3] = {st.co2_ppm, st.temp_raw, st.rh_raw};

      resp_len = sensirion_emul_put_words(resp, w, 3);
    }
    data_ready = false; /* one read per measurement */
    break;
  case SCD41_CMD_SET_ASC_ENABLED:
    if (n != 1) {
      return nack();
    }
    st.asc_enabled = arg[0] != 0;
    break;
  case SCD41_CMD_SET_ASC_INITIAL:
    if (n != 1 || arg[0] % 4 != 0) { /* the datasheet wants a multiple of 4 h */
      return nack();
    }
    st.asc_initial_h = arg[0];
    break;
  case SCD41_CMD_SET_ASC_STANDARD:
    if (n != 1 || arg[0] % 4 != 0) {
      return nack();
    }
    st.asc_standard_h = arg[0];
    break;
  case SCD41_CMD_SET_AMBIENT_PRESSURE:
    if (n != 1 || arg[0] < 700 || arg[0] > 1200) { /* 700 ... 1200 hPa */
      return nack();
    }
    st.pressure_hpa = arg[0];
    break;
  case SCD41_CMD_FORCED_RECAL:
    if (n != 1) {
      return nack();
    }
    resp_len = sensirion_emul_put_words(resp, &st.frc_result, 1);
    break;
  case SCD41_CMD_WAKE_UP: /* awake already: not acknowledged, no state change */
    return nack();
  default:
    return nack();
  }
  if (cmd == SCD41_CMD_POWER_DOWN) {
    st.asleep = true;
  }
  if (cmd == SCD41_CMD_SINGLE_SHOT) {
    st.shot_started_us = now;
    data_ready = !st.never_ready;
    data_ready_at_us = now + 5000 * 1000;
  }
  busy_until_us = now + t;
  log_cmd(cmd);
  return 0;
}

static int do_read(uint8_t *buf, size_t len) {
  int64_t now = sensirion_emul_now_us();

  if (now < busy_until_us) {
    st.early++;
    return nack();
  }
  if (st.asleep || resp_len == 0 || len != resp_len) {
    return nack();
  }
  memcpy(buf, resp, len);
  resp_len = 0;
  return 0;
}

static int scd41_emul_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr) {
  ARG_UNUSED(target);
  int64_t now = sensirion_emul_now_us();

  st.transfers++;
  if (st.present && addr == DT_INST_REG_ADDR(0) && rails_on() && rail_on_since_us >= 0 &&
      now + RAIL_TICK_US < rail_on_since_us + SCD41_POWER_UP_US) {
    st.early++; /* still powering up */
    return nack();
  }
  if (!st.present || addr != DT_INST_REG_ADDR(0) || !rails_on()) {
    return nack();
  }
  if (st.fail_after >= 0 && st.transfers > st.fail_after) {
    return nack();
  }
  for (int i = 0; i < num_msgs; i++) {
    int ret = (msgs[i].flags & I2C_MSG_READ) ? do_read(msgs[i].buf, msgs[i].len) : do_write(msgs[i].buf, msgs[i].len);

    if (ret != 0) {
      return ret;
    }
  }
  return 0;
}

static const struct i2c_emul_api scd41_emul_api = {
    .transfer = scd41_emul_transfer,
};

static int scd41_emul_init(const struct emul *target, const struct device *parent) {
  ARG_UNUSED(target);
  ARG_UNUSED(parent);
  scd41_emul_reset();
  k_timer_init(&rail_timer, rail_poll, NULL);
  k_timer_start(&rail_timer, K_MSEC(10), K_MSEC(10));
  return 0;
}

#define SCD41_EMUL(n) EMUL_DT_INST_DEFINE(n, scd41_emul_init, NULL, NULL, &scd41_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(SCD41_EMUL)
