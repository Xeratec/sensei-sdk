/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* BH1730 driver (NRF/drivers/sensor/bh1730) against the I2C emulator in emul_bh1730.c: sensor API, split API,
 * lux conversion against the datasheet formula, power-down between measurements, probe and re-probe.
 *
 * The driver keeps some state for good (chip found), so the tests run in name order and the first ones need a
 * driver that has not found the chip yet. */

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/ztest.h>

#include <sensei/drivers/bh1730.h>

#include "emul_bh1730.h"

#define ITIME 0xED /* devicetree: 51 ms */
#define GAIN_X 64  /* devicetree gain = 2 */

static const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(bh1730));
static struct bh1730_emul *em;

static void *setup(void) {
  em = bh1730_emul_get();
  zassert_true(device_is_ready(dev));
  return NULL;
}

static void before(void *f) {
  ARG_UNUSED(f);
  bh1730_emul_reset();
}

/* Every test ends with the sensor in power down (POWER bit 0). */
static void after(void *f) {
  ARG_UNUSED(f);
  zassert_equal(bh1730_emul_control() & 0x01, 0, "POWER bit cleared at the end of a test");
}

/* Lux as the datasheet (page 13) computes it, floating point. */
static double ds_lux(uint16_t d0, uint16_t d1, int gain, int itime) {
  double itime_ms = 2.8e-3 * 964 * (256 - itime);
  double r = d0 ? (double)d1 / d0 : 9;
  double x;

  if (d0 == 0) {
    return 0;
  }
  if (r < 0.26) {
    x = 1.290 * d0 - 2.733 * d1;
  } else if (r < 0.55) {
    x = 0.795 * d0 - 0.859 * d1;
  } else if (r < 1.09) {
    x = 0.510 * d0 - 0.345 * d1;
  } else if (r < 2.13) {
    x = 0.276 * d0 - 0.130 * d1;
  } else {
    return 0;
  }
  return x > 0 ? x / gain * 102.6 / itime_ms : 0;
}

/* The emulator itself: it follows the datasheet. (Raw I2C, not the driver.) */
ZTEST(sensei_bh1730, test_00_emulator_self_check) {
  const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
  uint8_t id = 0, buf[4], one = 0;
  const uint8_t cmd_ctl_write[2] = {0x80, 0x0B}; /* COMMAND = CONTROL, then CONTROL = POWER | ADC_EN | ONE_TIME */
  const uint8_t cmd_data[1] = {0x94};            /* COMMAND = DATA0LOW (datasheet page 12) */
  const uint8_t bad_cmd[1] = {0x08};             /* CMD bit 7 not set */
  const uint8_t bad_addr[1] = {0x8A};            /* register 0x0A does not exist */
  const uint8_t id_cmd[1] = {0x92};
  uint32_t tmt = bh1730_emul_meas_us(0xDA);

  zassert_within(tmt, 104600, 3000, "datasheet table: 104.6 ms typical for ITIME 0xDA");
  zassert_equal(i2c_write_read(i2c, 0x29, id_cmd, 1, &id, 1), 0);
  zassert_equal(id & 0xF0, 0x70, "part number 0111");
  zassert_not_equal(i2c_write(i2c, bad_cmd, 1, 0x29), 0);
  zassert_not_equal(i2c_write(i2c, bad_addr, 1, 0x29), 0);
  zassert_not_equal(i2c_write(i2c, bad_cmd, 1, 0x30), 0, "wrong address");

  zassert_equal(i2c_write(i2c, cmd_ctl_write, 2, 0x29), 0);
  em->data0 = 0x1234;
  em->data1 = 0x0567;
  zassert_equal(bh1730_emul_control() & 0x10, 0, "ADC_VALID is not set before the measurement time is over");
  k_msleep(1000);
  zassert_equal(bh1730_emul_control(), 0x18, "one time: POWER and ADC_EN off, ONE_TIME kept, data valid");
  zassert_equal(i2c_write_read(i2c, 0x29, cmd_data, 1, buf, 4), 0);
  zassert_equal(buf[0], 0x34);
  zassert_equal(buf[1], 0x12);
  zassert_equal(buf[2], 0x67);
  zassert_equal(buf[3], 0x05);
  zassert_equal(bh1730_emul_control() & 0x10, 0, "reading the data clears ADC_VALID");
  zassert_equal(i2c_write(i2c, (const uint8_t[]){0xE4}, 1, 0x29), 0, "software reset");
  zassert_equal(i2c_write_read(i2c, 0x29, (const uint8_t[]){0x81}, 1, &one, 1), 0);
  zassert_equal(one, 0xDA, "TIMING is back to its default");
}

ZTEST(sensei_bh1730, test_01_no_bus_traffic_at_boot) { zassert_equal(em->transfers, 0); }

/* No chip: probe fails and the next try asks again. */
ZTEST(sensei_bh1730, test_02_probe_without_chip) {
  em->present = false;
  zassert_equal(sensei_bh1730_probe(dev), -ENODEV);
  zassert_true(em->transfers > 0);

  int n = em->transfers;

  zassert_equal(pm_device_runtime_get(dev), -ENODEV);
  zassert_true(em->transfers > n, "asked again");
  zassert_equal(sensor_sample_fetch(dev), -ENODEV);
}

/* Something answers, but it is no BH1730 (part number is not 0111). */
ZTEST(sensei_bh1730, test_03_probe_wrong_id) {
  em->part_id = 0x1B;
  zassert_equal(sensei_bh1730_probe(dev), -ENODEV);
}

/* The chip shows up later: found without a reboot, and left in power down. */
ZTEST(sensei_bh1730, test_04_reprobe_finds_chip) {
  em->reg[0] = 0x01; /* it was on */
  zassert_equal(sensei_bh1730_probe(dev), 0);
  zassert_equal(em->reg[0] & 0x01, 0);
  zassert_equal(em->measurements, 0);
}

/* Once found, a second probe does not touch the bus. */
ZTEST(sensei_bh1730, test_05_probe_result_is_kept) {
  zassert_equal(sensei_bh1730_probe(dev), 0);
  zassert_equal(em->transfers, 0);
}

ZTEST(sensei_bh1730, test_06_split_api_needs_power) {
  zassert_equal(sensei_bh1730_start(dev), -ENOTCONN);
  zassert_equal(sensei_bh1730_fetch(dev), -ENOTCONN);
  zassert_equal(em->transfers, 0);
}

/* sensor_sample_fetch: power on, gain and integration time from the devicetree, one-time measurement, read, power
 * down. The lux value is within 3 % of the datasheet formula (the driver library rounds 102.6 to 100). */
ZTEST(sensei_bh1730, test_07_sample_fetch_and_convert) {
  struct sensor_value v;
  double want;

  em->data0 = 1000;
  em->data1 = 200;
  zassert_equal(sensor_sample_fetch(dev), 0);
  zassert_equal(em->measurements, 1);
  zassert_equal(em->reg[0x01], ITIME);
  zassert_equal(em->reg[0x07], 2);
  zassert_equal(em->reg[0x00] & 0x0F, 0x00, "power down at the end");
  /* the control write of the start: POWER | ADC_EN | ONE_TIME */
  bool seen = false;

  for (int i = 0; i < em->n_writes; i++) {
    seen |= em->writes[i] == 0x000B;
  }
  zassert_true(seen, "CONTROL = 0x0B (POWER, ADC_EN, ONE_TIME)");

  zassert_equal(sensor_channel_get(dev, SENSEI_BH1730_CHAN_VIS_RAW, &v), 0);
  zassert_equal(v.val1, 1000);
  zassert_equal(sensor_channel_get(dev, SENSEI_BH1730_CHAN_IR_RAW, &v), 0);
  zassert_equal(v.val1, 200);
  zassert_equal(sensor_channel_get(dev, SENSOR_CHAN_LIGHT, &v), 0);
  want = ds_lux(1000, 200, GAIN_X, ITIME);
  zassert_within((double)v.val1, want, want * 0.03 + 1, "lux %d, datasheet %d", v.val1, (int)want);
  zassert_true(v.val1 > 0);
}

/* The branches of the lux formula (ratio of the two channels): 0.26, 0.55, 1.09 and 2.13 are the limits. */
ZTEST(sensei_bh1730, test_08_lux_ratio_branches) {
  static const struct {
    uint16_t d0, d1;
  } c[] = {
      {20000, 1000},  /* < 0.26 */
      {20000, 8000},  /* < 0.55 */
      {20000, 16000}, /* < 1.09 */
      {20000, 30000}, /* < 2.13 */
      {20000, 42500}, /* 2.125: the last branch is slightly negative, 0 lx */
      {20000, 50000}, /* >= 2.13: 0 lx */
      {0, 0},         /* dark */
      {0, 100},
  };
  struct sensor_value v;

  for (size_t i = 0; i < ARRAY_SIZE(c); i++) {
    double want = ds_lux(c[i].d0, c[i].d1, GAIN_X, ITIME);

    em->data0 = c[i].d0;
    em->data1 = c[i].d1;
    zassert_equal(sensor_sample_fetch(dev), 0);
    zassert_equal(sensor_channel_get(dev, SENSOR_CHAN_LIGHT, &v), 0);
    zassert_within((double)v.val1, want, want * 0.03 + 1, "case %d: lux %d, datasheet %d", (int)i, v.val1, (int)want);
  }
}

/* Bright light at gain x64: counts above about 33000 must not overflow the lux calculation. */
ZTEST(sensei_bh1730, test_09_bright_light) {
  struct sensor_value v;
  double want = ds_lux(40000, 2000, GAIN_X, ITIME);

  em->data0 = 40000;
  em->data1 = 2000;
  zassert_equal(sensor_sample_fetch(dev), 0);
  zassert_equal(sensor_channel_get(dev, SENSOR_CHAN_LIGHT, &v), 0);
  zassert_within((double)v.val1, want, want * 0.03 + 1, "lux %d, datasheet %d", v.val1, (int)want);

  em->data0 = 65535;
  em->data1 = 0;
  want = ds_lux(65535, 0, GAIN_X, ITIME);
  zassert_equal(sensor_sample_fetch(dev), 0);
  zassert_equal(sensor_channel_get(dev, SENSOR_CHAN_LIGHT, &v), 0);
  zassert_within((double)v.val1, want, want * 0.03 + 1, "lux %d, datasheet %d", v.val1, (int)want);
}

/* Split API: the POWER bit follows the reference, start returns the wait, the result is not there before. */
ZTEST(sensei_bh1730, test_10_split_api_timing) {
  struct sensor_value v;
  int ms;

  em->data0 = 500;
  em->data1 = 100;
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_equal(em->reg[0] & 0x01, 0x01, "POWER set");
  ms = sensei_bh1730_start(dev);
  zassert_true(ms >= (int)(bh1730_emul_meas_us(ITIME) / 1000), "start() returns %d ms, the sensor needs %d", ms,
               (int)(bh1730_emul_meas_us(ITIME) / 1000));
  zassert_equal(sensei_bh1730_fetch(dev), -EBUSY);
  k_msleep(ms);
  zassert_equal(sensei_bh1730_fetch(dev), 0);
  zassert_equal(sensor_channel_get(dev, SENSEI_BH1730_CHAN_VIS_RAW, &v), 0);
  zassert_equal(v.val1, 500);
  /* the read cleared ADC_VALID: there is nothing new */
  zassert_equal(sensei_bh1730_fetch(dev), -EBUSY);
  zassert_equal(pm_device_runtime_put(dev), 0);
  zassert_equal(em->reg[0] & 0x01, 0x00, "POWER cleared");
}

ZTEST(sensei_bh1730, test_11_unsupported_channels) {
  struct sensor_value v;

  zassert_equal(sensor_sample_fetch_chan(dev, SENSOR_CHAN_PRESS), -ENOTSUP);
  zassert_equal(em->transfers, 0);
  zassert_equal(sensor_channel_get(dev, SENSOR_CHAN_PRESS, &v), -ENOTSUP);
  zassert_equal(sensor_sample_fetch_chan(dev, SENSEI_BH1730_CHAN_IR_RAW), 0);
}

ZTEST(sensei_bh1730, test_12_bus_error) {
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_true(sensei_bh1730_start(dev) > 0);
  k_msleep(200);
  em->fail_after = em->transfers;
  zassert_equal(sensei_bh1730_fetch(dev), -EIO);
  zassert_equal(sensei_bh1730_start(dev), -EIO);
  em->fail_after = -1;
  zassert_equal(sensei_bh1730_fetch(dev), 0);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

/* A measurement that never ends: timeout, and the sensor is powered down again. */
ZTEST(sensei_bh1730, test_13_timeout) {
  em->never_valid = true;
  zassert_equal(sensor_sample_fetch(dev), -ETIMEDOUT);
}

ZTEST_SUITE(sensei_bh1730, NULL, setup, before, after, NULL);
