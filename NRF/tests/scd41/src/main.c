/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* SCD41 driver (NRF/drivers/sensor/scd41) against the I2C emulator in emul_scd41.c: sensor API, split API, power
 * (rails and the sleep command), settings after a cold start, probe and re-probe.
 *
 * The driver keeps some state for good (chip found, settings given), so the tests run in name order and the
 * first ones need a driver that has not found the chip yet. */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/ztest.h>

#include <sensei/drivers/scd41.h>

#include "emul_scd41.h"

static const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(scd41));
static const struct device *const gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static struct scd41_emul *em;

static bool rail_on(void) { return gpio_emul_output_get(gpio, 1) == 1 && gpio_emul_output_get(gpio, 2) == 1; }
static bool rail_off(void) { return gpio_emul_output_get(gpio, 1) == 0 && gpio_emul_output_get(gpio, 2) == 0; }

static void *setup(void) {
  em = scd41_emul_get();
  zassert_true(device_is_ready(dev));
  return NULL;
}

static void before(void *f) {
  ARG_UNUSED(f);
  scd41_emul_reset();
}

/* Every test ends with the sensor off. */
static void after(void *f) {
  ARG_UNUSED(f);
  zassert_true(rail_off(), "rails are off at the end of a test");
  zassert_equal(em->early, 0, "no command was sent while the sensor was busy");
}

static int32_t ref_temp_mdeg(uint16_t raw) { return (int32_t)(-45000 + (int64_t)175000 * raw / 65536); }
static int32_t ref_rh_mpct(uint16_t raw) { return (int32_t)((int64_t)100000 * raw / 65536); }

static int32_t milli(const struct sensor_value *v) { return v->val1 * 1000 + v->val2 / 1000; }

/* The emulator itself: it refuses what the datasheet says the sensor refuses. (Own rail control, not the driver.) */
ZTEST(sensei_scd41, test_00_emulator_self_check) {
  const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
  const struct device *vdd = DEVICE_DT_GET(DT_NODELABEL(rail_vdd));
  const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(rail_bus));
  const uint8_t stop[2] = {0x3F, 0x86}, serial[2] = {0x36, 0x82};
  uint8_t pdown[2] = {0x36, 0xE0}, buf[9];
  uint8_t bad_crc[5] = {0x24, 0x16, 0x00, 0x01, 0x00}; /* ASC enable with a wrong CRC (correct: 0xB0) */

  zassert_not_equal(i2c_write(i2c, stop, 2, 0x62), 0, "rails off: no answer");
  zassert_equal(regulator_enable(vdd), 0);
  zassert_equal(regulator_enable(bus), 0);
  zassert_not_equal(i2c_write(i2c, stop, 2, 0x62), 0, "the 1000 ms power-up are not waited");
  zassert_equal(em->early, 1);
  em->early = 0;
  k_msleep(1000);
  zassert_equal(i2c_write(i2c, bad_crc, sizeof(bad_crc), 0x62), -EIO, "wrong CRC");
  zassert_equal(i2c_write(i2c, stop, 2, 0x62), 0);
  zassert_not_equal(i2c_write(i2c, serial, 2, 0x62), 0, "500 ms of stop_periodic_measurement not waited");
  zassert_equal(em->early, 1);
  em->early = 0;
  k_msleep(501);
  zassert_equal(i2c_write(i2c, serial, 2, 0x62), 0);
  zassert_not_equal(i2c_read(i2c, buf, 9, 0x62), 0, "1 ms of get_serial_number not waited");
  k_msleep(2);
  zassert_equal(i2c_read(i2c, buf, 9, 0x62), 0);
  zassert_equal(buf[2], sensirion_emul_crc8(buf, 2));
  zassert_not_equal(i2c_read(i2c, buf, 9, 0x62), 0, "nothing more to read");
  zassert_equal(i2c_write(i2c, pdown, 2, 0x62), 0);
  k_msleep(2);
  zassert_not_equal(i2c_write(i2c, serial, 2, 0x62), 0, "asleep");
  em->early = 0;
  zassert_equal(regulator_disable(bus), 0);
  zassert_equal(regulator_disable(vdd), 0);
}

/* The driver does not touch the bus before it is used (the board rails may not be up at boot). */
ZTEST(sensei_scd41, test_01_no_bus_traffic_at_boot) {
  zassert_equal(em->transfers, 0);
  zassert_true(rail_off());
}

/* No chip: probe fails, the rails go off again, and the next try asks the chip again. */
ZTEST(sensei_scd41, test_02_probe_without_chip) {
  em->present = false;
  zassert_equal(sensei_scd41_probe(dev), -ENODEV);
  zassert_true(rail_off());
  zassert_true(em->transfers > 0);

  int before_n = em->transfers;

  zassert_equal(pm_device_runtime_get(dev), -ENODEV);
  zassert_true(rail_off());
  zassert_true(em->transfers > before_n, "asked again");
  zassert_equal(sensei_scd41_probe(dev), -ENODEV);
}

/* An SCD40 is not accepted (the driver needs the single shot of the SCD41). */
ZTEST(sensei_scd41, test_03_probe_wrong_variant) {
  em->variant = 0x0000;
  zassert_equal(sensei_scd41_probe(dev), -ENODEV);
  zassert_true(rail_off());
}

/* The chip shows up later: found without a reboot. Power on, stop periodic mode, serial number, variant. */
ZTEST(sensei_scd41, test_04_reprobe_finds_chip) {
  zassert_equal(sensei_scd41_probe(dev), 0);
  zassert_true(rail_off());
  zassert_equal(em->log_len, 3);
  zassert_equal(em->log[0], SCD41_CMD_STOP_PERIODIC);
  zassert_equal(em->log[1], SCD41_CMD_GET_SERIAL);
  zassert_equal(em->log[2], SCD41_CMD_GET_VARIANT);
}

/* Once the chip answered, a power-up does not ask again. */
ZTEST(sensei_scd41, test_05_probe_result_is_kept) {
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_true(rail_on());
  zassert_equal(em->log_len, 0);
  zassert_equal(sensei_scd41_probe(dev), 0);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

/* Without a reference (rails off) the split API refuses. */
ZTEST(sensei_scd41, test_06_split_api_needs_power) {
  zassert_equal(sensei_scd41_start(dev), -ENOTCONN);
  zassert_equal(sensei_scd41_fetch(dev), -ENOTCONN);
  zassert_equal(sensei_scd41_forced_recalibration(dev, 420, NULL), -ENOTCONN);
  zassert_equal(sensei_scd41_sleep(dev), 0); /* nothing to put to sleep */
  zassert_equal(em->transfers, 0);
}

/* sensor_sample_fetch: power up, single shot (5 s), read, power down. Conversions as in the datasheet. */
ZTEST(sensei_scd41, test_07_sample_fetch_and_convert) {
  struct sensor_value v;
  int64_t t0;
  int64_t dt;

  em->co2_ppm = 1234;
  em->temp_raw = 0x6667; /* the datasheet example: 25 degC */
  em->rh_raw = 0x8000;   /* 50 %RH */
  t0 = k_uptime_get();
  zassert_equal(sensor_sample_fetch(dev), 0);
  dt = k_uptime_get() - t0;
  zassert_true(dt >= 6000 && dt < 6700, "1 s power-up and 5 s single shot took %d ms", (int)dt);
  zassert_true(rail_off());
  zassert_equal(scd41_emul_count(SCD41_CMD_SINGLE_SHOT), 1);
  zassert_equal(scd41_emul_count(SCD41_CMD_READ_MEASUREMENT), 1);

  zassert_equal(sensor_channel_get(dev, SENSOR_CHAN_CO2, &v), 0);
  zassert_equal(v.val1, 1234);
  zassert_equal(sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &v), 0);
  zassert_within(milli(&v), ref_temp_mdeg(0x6667), 1);
  zassert_within(milli(&v), 25000, 5);
  zassert_equal(sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &v), 0);
  zassert_within(milli(&v), ref_rh_mpct(0x8000), 1);
  zassert_equal(milli(&v), 50000);

  /* extremes of the range */
  em->temp_raw = 0;
  em->rh_raw = 0xFFFF;
  zassert_equal(sensor_sample_fetch_chan(dev, SENSOR_CHAN_CO2), 0);
  sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &v);
  zassert_equal(milli(&v), -45000);
  sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &v);
  zassert_within(milli(&v), 100000, 2);
}

ZTEST(sensei_scd41, test_08_unsupported_channels) {
  struct sensor_value v;

  zassert_equal(sensor_sample_fetch_chan(dev, SENSOR_CHAN_PRESS), -ENOTSUP);
  zassert_equal(em->transfers, 0, "no power and no traffic for a channel the driver does not have");
  zassert_equal(sensor_channel_get(dev, SENSOR_CHAN_PRESS, &v), -ENOTSUP);
}

/* Split API: start returns the 5 s, the result is not there before, then it is. The datasheet does not let the
 * master talk to the sensor during the shot; with a sensor that answers "not ready" (ack_during_shot) the driver
 * reports -EBUSY. */
ZTEST(sensei_scd41, test_09_split_api_timing) {
  struct sensor_value v;

  em->ack_during_shot = true;
  em->co2_ppm = 555;
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_true(rail_on());
  zassert_equal(sensei_scd41_start(dev), 5000);
  zassert_equal(sensei_scd41_fetch(dev), -EBUSY);
  k_msleep(2000);
  zassert_equal(sensei_scd41_fetch(dev), -EBUSY);
  k_msleep(3100);
  zassert_equal(sensei_scd41_fetch(dev), 0);
  zassert_equal(sensor_channel_get(dev, SENSOR_CHAN_CO2, &v), 0);
  zassert_equal(v.val1, 555);
  /* one read per measurement: the ready flag is gone */
  zassert_equal(sensei_scd41_fetch(dev), -EBUSY);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

/* A sensor that behaves as the datasheet says (no answer during the shot): fetching early is a bus error. */
ZTEST(sensei_scd41, test_09b_early_fetch_is_refused) {
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_equal(sensei_scd41_start(dev), 5000);
  zassert_equal(sensei_scd41_fetch(dev), -EIO);
  zassert_equal(em->early, 1);
  em->early = 0;
  k_msleep(5100);
  zassert_equal(sensei_scd41_fetch(dev), 0);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

/* Between shots the sensor can sleep. Then it answers nothing but wake_up (which it does not acknowledge), and the
 * driver wakes it at the next start. Settings given while it sleeps are written after the wake-up. */
ZTEST(sensei_scd41, test_10_sleep_and_wake) {
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_equal(sensei_scd41_sleep(dev), 0);
  zassert_true(em->asleep);
  zassert_equal(sensei_scd41_fetch(dev), -EIO, "a sleeping sensor does not answer");

  int n = em->log_len;

  zassert_equal(sensei_scd41_set_asc(dev, true, 8, 24), 0);
  zassert_equal(em->log_len, n, "nothing is sent to a sleeping sensor");

  zassert_equal(sensei_scd41_start(dev), 5000);
  zassert_false(em->asleep);
  zassert_equal(scd41_emul_count(SCD41_CMD_WAKE_UP), 1);
  zassert_true(em->asc_enabled);
  zassert_equal(em->asc_initial_h, 8);
  zassert_equal(em->asc_standard_h, 24);
  /* order: wake up, settings, single shot */
  zassert_equal(em->log[n], SCD41_CMD_WAKE_UP);
  zassert_equal(em->log[n + 1], SCD41_CMD_SET_ASC_ENABLED);
  zassert_equal(em->log[em->log_len - 1], SCD41_CMD_SINGLE_SHOT);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

/* The sensor loses its settings with the power: the driver writes them again after every cold start. */
ZTEST(sensei_scd41, test_11_asc_after_cold_start) {
  zassert_equal(sensei_scd41_set_asc(dev, true, 48, 156), 0); /* rails off: stored only */
  zassert_equal(em->transfers, 0);

  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_true(em->asc_enabled);
  zassert_equal(em->asc_initial_h, 48);
  zassert_equal(em->asc_standard_h, 156);
  zassert_equal(pm_device_runtime_put(dev), 0);

  /* power loss: the sensor is back to its defaults (44 h and 156 h), a cold start writes the settings again */
  scd41_emul_power_loss();
  zassert_equal(em->asc_initial_h, 44);
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_equal(em->asc_initial_h, 48);
  zassert_equal(pm_device_runtime_put(dev), 0);

  /* off: only the enable flag is written */
  zassert_equal(sensei_scd41_set_asc(dev, false, 48, 156), 0);
  scd41_emul_power_loss();
  em->log_len = 0;
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_false(em->asc_enabled);
  zassert_equal(scd41_emul_count(SCD41_CMD_SET_ASC_ENABLED), 1);
  zassert_equal(scd41_emul_count(SCD41_CMD_SET_ASC_INITIAL), 0);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

/* A change while the sensor is on is written at once, an invalid period is refused by the sensor (multiple of 4 h). */
ZTEST(sensei_scd41, test_12_asc_change_while_on) {
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_equal(sensei_scd41_set_asc(dev, true, 96, 312), 0);
  zassert_true(em->asc_enabled);
  zassert_equal(em->asc_initial_h, 96);
  zassert_equal(em->asc_standard_h, 312);

  int n = em->log_len;

  zassert_equal(sensei_scd41_set_asc(dev, true, 96, 312), 0, "same values");
  zassert_equal(em->log_len, n, "unchanged: nothing is sent");
  zassert_equal(sensei_scd41_set_asc(dev, true, 97, 312), -EIO);
  zassert_equal(sensei_scd41_set_asc(dev, true, 96, 312), 0); /* back to a valid value */
  zassert_equal(pm_device_runtime_put(dev), 0);
}

/* Ambient pressure goes to the sensor before a shot (hPa), values out of 700 ... 1200 hPa are not sent. */
ZTEST(sensei_scd41, test_13_ambient_pressure) {
  zassert_equal(pm_device_runtime_get(dev), 0);
  sensei_scd41_set_pressure(dev, 98700);
  zassert_equal(sensei_scd41_start(dev), 5000);
  zassert_equal(em->pressure_hpa, 987);
  zassert_equal(scd41_emul_count(SCD41_CMD_SET_AMBIENT_PRESSURE), 1);

  k_msleep(5100); /* the first shot is over */
  em->log_len = 0;
  sensei_scd41_set_pressure(dev, 50000);
  zassert_equal(sensei_scd41_start(dev), 5000);
  zassert_equal(scd41_emul_count(SCD41_CMD_SET_AMBIENT_PRESSURE), 0);
  zassert_equal(em->pressure_hpa, 987);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

ZTEST(sensei_scd41, test_14_forced_recalibration) {
  int16_t corr = 0;

  zassert_equal(pm_device_runtime_get(dev), 0);
  em->frc_result = 0x8000 - 25;
  zassert_equal(sensei_scd41_forced_recalibration(dev, 420, &corr), 0);
  zassert_equal(corr, -25);
  em->frc_result = 0x8000 + 40;
  zassert_equal(sensei_scd41_forced_recalibration(dev, 420, NULL), 0);
  em->frc_result = 0xFFFF; /* the datasheet: failed */
  zassert_equal(sensei_scd41_forced_recalibration(dev, 420, &corr), -EIO);
  /* the 400 ms of the command are waited (em->early stays 0, checked after the test) */
  zassert_equal(sensei_scd41_set_asc(dev, true, 4, 4), 0);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

ZTEST(sensei_scd41, test_15_bus_error_during_fetch) {
  zassert_equal(pm_device_runtime_get(dev), 0);
  zassert_equal(sensei_scd41_start(dev), 5000);
  k_msleep(5100);
  em->fail_after = em->transfers; /* the bus fails from now on */
  zassert_equal(sensei_scd41_fetch(dev), -EIO);
  zassert_equal(sensei_scd41_start(dev), -EIO);
  em->fail_after = -1;
  zassert_equal(sensei_scd41_fetch(dev), 0);
  zassert_equal(pm_device_runtime_put(dev), 0);
}

/* A shot that never finishes ends with a timeout, the rails still go off. */
ZTEST(sensei_scd41, test_16_timeout) {
  em->never_ready = true;
  zassert_equal(sensor_sample_fetch(dev), -ETIMEDOUT);
  zassert_true(rail_off());
}

ZTEST_SUITE(sensei_scd41, NULL, setup, before, after, NULL);
