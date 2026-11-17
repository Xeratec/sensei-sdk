/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* I2C emulator of the Sensirion SGP41 (address 0x59), for the driver "sensirion,sgp41".
 *
 * Follows the Sensirion SGP41 datasheet, version 1.0 (December 2021): 3.1 conditioning (command from idle mode,
 * default compensation values, 10 s at most), 3.2 raw signal measurement, 3.4 built-in self-test, 3.5 serial
 * number, 3.6 start-up (idle after 0.6 ms), 4.5 I2C sequences (no communication while a command runs, the sensor
 * answers a NACK; a result can be read once), 4.6 CRC-8, 4.7 command table 8 (codes, argument and response
 * lengths, execution times: conditioning and measurement 50 ms, self-test 320 ms, heater off and serial number 1 ms),
 * table 15 (self-test result: bit 0 VOC pixel, bit 1 NOx pixel failed).
 * The sensor does not answer while one of the two supply rails (gpio-controlled regulators of the node) is off.
 * Not modelled: the value of SRAW_NOX during the run-in of the NOx pixel. */

#define DT_DRV_COMPAT sensirion_sgp41

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "emul_sgp41.h"

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "one SGP41 node");

static struct sgp41_emul st;
static int64_t busy_until_us;
static int64_t cond_first_us;
static uint8_t resp[9];
static size_t resp_len;

static const struct gpio_dt_spec rail_vdd = GPIO_DT_SPEC_GET(DT_INST_PHANDLE(0, vdd_supply), enable_gpios);
static const struct gpio_dt_spec rail_bus = GPIO_DT_SPEC_GET(DT_INST_PHANDLE(0, bus_switch_supply), enable_gpios);

struct sgp41_emul *sgp41_emul_get(void) { return &st; }

void sgp41_emul_power_loss(void) {
  st.heater_on = false;
  st.conditioning = 0;
  busy_until_us = 0;
  resp_len = 0;
}

void sgp41_emul_reset(void) {
  memset(&st, 0, sizeof(st));
  st.present = true;
  st.self_test = 0xD400;
  st.voc_raw = 30000;
  st.nox_raw = 15000;
  st.fail_after = -1;
  sgp41_emul_power_loss();
}

int sgp41_emul_count(uint16_t cmd) {
  int n = 0;

  for (int i = 0; i < st.log_len; i++) {
    n += st.log[i] == cmd;
  }
  return n;
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
  uint16_t cmd, arg[2] = {0, 0};
  int64_t now = sensirion_emul_now_us();
  int n = sensirion_emul_parse(buf, len, &cmd, arg, 2);
  int32_t t_us;

  if (n < 0) {
    return nack();
  }
  if (now < busy_until_us) {
    st.early++;
    return nack();
  }
  resp_len = 0;
  switch (cmd) {
  case SGP41_CMD_CONDITIONING:
    if (n != 2) {
      return nack();
    }
    if (st.conditioning == 0) {
      cond_first_us = now;
    } else if (now - cond_first_us > 10 * 1000 * 1000) {
      st.over_conditioned++;
    }
    st.conditioning++;
    st.heater_on = true;
    st.last_rh = arg[0];
    st.last_t = arg[1];
    resp_len = sensirion_emul_put_words(resp, &st.voc_raw, 1);
    t_us = 50 * 1000;
    break;
  case SGP41_CMD_MEASURE_RAW: {
    const uint16_t w[2] = {st.voc_raw, st.nox_raw};

    if (n != 2) {
      return nack();
    }
    if (st.conditioning == 0) {
      st.unconditioned++;
    }
    st.heater_on = true;
    st.last_rh = arg[0];
    st.last_t = arg[1];
    resp_len = sensirion_emul_put_words(resp, w, 2);
    t_us = 50 * 1000;
    break;
  }
  case SGP41_CMD_SELF_TEST:
    if (n != 0) {
      return nack();
    }
    resp_len = sensirion_emul_put_words(resp, &st.self_test, 1);
    t_us = 320 * 1000;
    break;
  case SGP41_CMD_HEATER_OFF:
    if (n != 0) {
      return nack();
    }
    st.heater_on = false;
    st.conditioning = 0;
    t_us = 1000;
    break;
  case SGP41_CMD_SERIAL: {
    const uint16_t sn[3] = {0x0000, 0x0123, 0x4567};

    if (n != 0) {
      return nack();
    }
    resp_len = sensirion_emul_put_words(resp, sn, 3);
    t_us = 1000;
    break;
  }
  default:
    return nack();
  }
  busy_until_us = now + t_us;
  log_cmd(cmd);
  return 0;
}

static int do_read(uint8_t *buf, size_t len) {
  if (sensirion_emul_now_us() < busy_until_us) {
    st.early++;
    return nack();
  }
  if (resp_len == 0 || len != resp_len) {
    return nack();
  }
  memcpy(buf, resp, len);
  resp_len = 0; /* a result can be read once */
  return 0;
}

static int sgp41_emul_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr) {
  ARG_UNUSED(target);
  st.transfers++;
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

static const struct i2c_emul_api sgp41_emul_api = {
    .transfer = sgp41_emul_transfer,
};

static int sgp41_emul_init(const struct emul *target, const struct device *parent) {
  ARG_UNUSED(target);
  ARG_UNUSED(parent);
  sgp41_emul_reset();
  return 0;
}

#define SGP41_EMUL(n) EMUL_DT_INST_DEFINE(n, sgp41_emul_init, NULL, NULL, &sgp41_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(SGP41_EMUL)
