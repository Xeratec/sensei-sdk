/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* SGP41 driver (NRF/drivers/sensor/sgp41) against the I2C emulator in emul_sgp41.c: sensor API with conditioning,
 * split calls, compensation ticks, heater off, power (rails), probe and re-probe.
 *
 * The driver keeps some state for good (chip found), so the tests run in name order and the first ones need a
 * driver that has not found the chip yet. Built twice (testcase.yaml): with the 10 s of conditioning in
 * sensor_sample_fetch() and without. */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/ztest.h>

#include <sensei/drivers/sgp41.h>

#include "emul_sgp41.h"

#define RH_DEFAULT 0x8000 /* 50 %RH, datasheet table 12 */
#define T_DEFAULT 0x6666  /* 25 degC */

static const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(sgp41));
static const struct device *const gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static struct sgp41_emul *em;

static bool rail_on(void) { return gpio_emul_output_get(gpio, 1) == 1 && gpio_emul_output_get(gpio, 2) == 1; }
static bool rail_off(void) { return gpio_emul_output_get(gpio, 1) == 0 && gpio_emul_output_get(gpio, 2) == 0; }

static void *setup(void) {
  em = sgp41_emul_get();
  zassert_true(device_is_ready(dev));
  return NULL;
}

static void before(void *f) {
  ARG_UNUSED(f);
  sgp41_emul_reset();
}

static void after(void *f) {
  ARG_UNUSED(f);
  zassert_true(rail_off(), "rails are off at the end of a test");
  zassert_equal(em->early, 0, "no command was sent while the sensor was busy");
}

/* The emulator itself: it refuses what the datasheet says the sensor refuses. (Own rail control, not the driver.) */
ZTEST(sensei_sgp41, test_00_emulator_self_check) {
  const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
  const struct device *vdd = DEVICE_DT_GET(DT_NODELABEL(rail_vdd));
  const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(rail_bus));
  const uint8_t serial[2] = {0x36, 0x82}, heater_off[2] = {0x36, 0x15};
  /* conditioning with the default compensation values of the datasheet (table 9: 0x8000 + CRC 0xA2, 0x6666 + CRC 0x93)
   */
  const uint8_t cond[8] = {0x26, 0x12, 0x80, 0x00, 0xA2, 0x66, 0x66, 0x93};
  uint8_t cond_bad[8] = {0x26, 0x12, 0x80, 0x00, 0xA2, 0x66, 0x66, 0x94};
  uint8_t buf[9];

  zassert_not_equal(i2c_write(i2c, serial, 2, 0x59), 0, "rails off: no answer");
  zassert_equal(regulator_enable(vdd), 0);
  zassert_equal(regulator_enable(bus), 0);
  zassert_equal(sensirion_emul_crc8((const uint8_t[]){0xBE, 0xEF}, 2), 0x92, "datasheet table 7 example");
  zassert_equal(i2c_write(i2c, cond_bad, 8, 0x59), -EIO, "wrong CRC");
  zassert_equal(i2c_write(i2c, cond, 8, 0x59), 0);
  zassert_not_equal(i2c_write(i2c, serial, 2, 0x59), 0, "the 50 ms of the conditioning are not waited");
  zassert_equal(em->early, 1);
  em->early = 0;
  k_msleep(60);
  zassert_equal(i2c_read(i2c, buf, 3, 0x59), 0);
  zassert_equal(buf[2], sensirion_emul_crc8(buf, 2));
  zassert_not_equal(i2c_read(i2c, buf, 3, 0x59), 0, "a result can be read once");
  zassert_equal(i2c_write(i2c, heater_off, 2, 0x59), 0);
  k_msleep(20);
  zassert_equal(regulator_disable(bus), 0);
  zassert_equal(regulator_disable(vdd), 0);
}

ZTEST(sensei_sgp41, test_01_no_bus_traffic_at_boot) {
  zassert_equal(em->transfers, 0);
  zassert_true(rail_off());
}

/* No chip: probe fails, the rails go off again, and the next try asks the chip again. */
ZTEST(sensei_sgp41, test_02_probe_without_chip) {
  em->present = false;
  zassert_equal(sensei_sgp41_probe(dev), -ENODEV);
  zassert_true(rail_off());
  zassert_true(em->transfers > 0);

  int before_n = em->transfers;

  zassert_equal(pm_device_runtime_get(dev), -ENODEV);
  zassert_true(rail_off());
  zassert_true(em->transfers > before_n, "asked again");
}

/* The chip shows up later: found without a reboot (serial number, then self test). A pixel that fails the self test
 * is only logged. */
ZTEST(sensei_sgp41, test_03_reprobe_finds_chip) {
  em->self_test = 0xD401;
  zassert_equal(sensei_sgp41_probe(dev), 0);
  zassert_true(rail_off());
  zassert_equal(em->log_len, 2);
  zassert_equal(em->log[0], SGP41_CMD_SERIAL);
  zassert_equal(em->log[1], SGP41_CMD_SELF_TEST);
}

/* Once the chip answered, a power-up does not ask again. */
ZTEST(sensei_sgp41, test_04_probe_result_is_kept) {
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_true(rail_on());
  zassert_equal(em->log_len, 0);
  zassert_equal(sensei_sgp41_probe(dev), 0);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

/* Without a reference (rails off) the calls refuse; heater off has nothing to do. */
ZTEST(sensei_sgp41, test_05_needs_power) {
  uint16_t v;

  zassert_equal(sensei_sgp41_condition(dev, RH_DEFAULT, T_DEFAULT, &v), -ENOTCONN);
  zassert_equal(sensei_sgp41_measure(dev, RH_DEFAULT, T_DEFAULT, &v, &v), -ENOTCONN);
  zassert_equal(sensei_sgp41_heater_off(dev), 0);
  zassert_equal(em->transfers, 0);
}

/* One conditioning command with the compensation values, the VOC raw value comes back. */
ZTEST(sensei_sgp41, test_06_conditioning_command) {
  uint16_t voc = 0;

  em->voc_raw = 31234;
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_equal(sensei_sgp41_condition(dev, 0x1234, 0x4321, &voc), 0);
  zassert_equal(voc, 31234);
  zassert_equal(em->last_rh, 0x1234);
  zassert_equal(em->last_t, 0x4321);
  zassert_equal(sensei_sgp41_condition(dev, RH_DEFAULT, T_DEFAULT, NULL), 0, "result is optional");
  zassert_equal(sgp41_emul_count(SGP41_CMD_CONDITIONING), 2);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

ZTEST(sensei_sgp41, test_07_measure_and_channels) {
  struct sensor_value v;
  uint16_t voc = 0, nox = 0;

  em->voc_raw = 28000;
  em->nox_raw = 1100;
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_equal(sensei_sgp41_condition(dev, RH_DEFAULT, T_DEFAULT, NULL), 0);
  zassert_equal(sensei_sgp41_measure(dev, 0x2000, 0x7000, &voc, &nox), 0);
  zassert_equal(voc, 28000);
  zassert_equal(nox, 1100);
  zassert_equal(em->last_rh, 0x2000);
  zassert_equal(em->last_t, 0x7000);
  zassert_equal(em->unconditioned, 0);
  zassert_equal(sensor_channel_get(dev, SENSEI_SGP41_CHAN_VOC_RAW, &v), 0);
  zassert_equal(v.val1, 28000);
  zassert_equal(sensor_channel_get(dev, SENSEI_SGP41_CHAN_NOX_RAW, &v), 0);
  zassert_equal(v.val1, 1100);
  zassert_equal(sensor_channel_get(dev, SENSOR_CHAN_CO2, &v), -ENOTSUP);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

ZTEST(sensei_sgp41, test_08_unsupported_channels) {
  zassert_equal(sensor_sample_fetch_chan(dev, SENSOR_CHAN_PRESS), -ENOTSUP);
  zassert_equal(em->transfers, 0, "no power and no traffic for a channel the driver does not have");
}

/* sensor_sample_fetch: power up, conditioning (one command per second), one measurement, power down. */
ZTEST(sensei_sgp41, test_09_sample_fetch) {
  struct sensor_value v;
  int64_t t0;
  int64_t dt;
  const int cond = CONFIG_SGP41_SENSEI_CONDITIONING_S;

  em->voc_raw = 29000;
  em->nox_raw = 1500;
  t0 = k_uptime_get();
  zassert_equal(sensor_sample_fetch(dev), 0);
  dt = k_uptime_get() - t0;
  zassert_true(rail_off());
  zassert_equal(sgp41_emul_count(SGP41_CMD_CONDITIONING), cond);
  zassert_equal(sgp41_emul_count(SGP41_CMD_MEASURE_RAW), 1);
  zassert_equal(em->over_conditioned, 0, "conditioning stays within the 10 s of the datasheet");
  zassert_equal(em->unconditioned, cond == 0 ? 1 : 0);
  /* default compensation: 50 %RH, 25 degC */
  zassert_equal(em->last_rh, RH_DEFAULT);
  zassert_equal(em->last_t, T_DEFAULT);
  zassert_true(dt >= cond * 1000 && dt < cond * 1000 + 500, "took %d ms for %d s of conditioning", (int)dt, cond);
  sensor_channel_get(dev, SENSEI_SGP41_CHAN_VOC_RAW, &v);
  zassert_equal(v.val1, 29000);
  sensor_channel_get(dev, SENSEI_SGP41_CHAN_NOX_RAW, &v);
  zassert_equal(v.val1, 1500);

  /* new compensation values are used by the next sample */
  sensei_sgp41_set_compensation(dev, 0x4000, 0x5000);
  zassert_equal(sensor_sample_fetch_chan(dev, SENSEI_SGP41_CHAN_VOC_RAW), 0);
  zassert_equal(em->last_rh, 0x4000);
  zassert_equal(em->last_t, 0x5000);
}

/* Heater off: idle mode while the rails stay on. The next measurement then needs conditioning again. */
ZTEST(sensei_sgp41, test_10_heater_off) {
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_equal(sensei_sgp41_condition(dev, RH_DEFAULT, T_DEFAULT, NULL), 0);
  zassert_true(em->heater_on);
  zassert_equal(sensei_sgp41_heater_off(dev), 0);
  zassert_false(em->heater_on);
  zassert_equal(sgp41_emul_count(SGP41_CMD_HEATER_OFF), 1);
  zassert_equal(sensei_sgp41_measure(dev, RH_DEFAULT, T_DEFAULT, NULL, NULL), 0);
  zassert_equal(em->unconditioned, 1);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

/* The bus fails: the driver says -EIO, in a sample the rails still go off. */
ZTEST(sensei_sgp41, test_11_bus_error) {
  zassert_equal(pm_device_runtime_get(dev), 0);
  em->fail_after = em->transfers;
  zassert_equal(sensei_sgp41_condition(dev, RH_DEFAULT, T_DEFAULT, NULL), -EIO);
  zassert_equal(sensei_sgp41_measure(dev, RH_DEFAULT, T_DEFAULT, NULL, NULL), -EIO);
  zassert_equal(sensei_sgp41_heater_off(dev), -EIO);
  em->fail_after = -1;
  zassert_equal(pm_device_runtime_put(dev), 0);

  em->fail_after = em->transfers; /* the chip is known: no traffic at power-up, the first command fails */
  zassert_equal(sensor_sample_fetch(dev), -EIO);
  zassert_true(rail_off());
}

ZTEST_SUITE(sensei_sgp41, NULL, setup, before, after, NULL);
