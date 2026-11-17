/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * SCD41 in single-shot mode, on the Sensirion SCD4x library.
 *
 * Power: the rail and the bus switch come from the devicetree (vdd-supply, bus-switch-supply) and follow
 * runtime PM (resume: on, suspend: off). Nothing touches the hardware before the first resume, so the driver
 * can be set up before the board rails are up. The settings of the sensor live in RAM, so after every cold
 * start the driver writes the ASC settings again.
 *
 * Measurement: scd4x_measure_single_shot() sleeps for the whole 5 s inside the library. The HAL defers that
 * sleep and start() returns it to the caller, who waits on its own schedule. fetch() checks the data-ready
 * flag. An idle sensor sleeps with the power-down command between shots (sensei_scd41_sleep()); start()
 * wakes it.
 */

#define DT_DRV_COMPAT sensei_scd41

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>

#include <sensei/drivers/scd41.h>
#include <sensei/drivers/sensirion_hal.h>
#include <sensei/drivers/sensor_supply.h>

#include "scd4x_i2c.h"

LOG_MODULE_REGISTER(scd41, CONFIG_SENSOR_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1, "the Sensirion library supports one SCD4x");

/* The single shot takes 5 s. The deferred sleep of the library is what start() returns. */
#define SCD41_SHOT_DEFER_MIN_US (1000 * 1000)
#define SCD41_TIMEOUT_MS 7000

struct scd41_config {
  const struct device *bus;
  uint8_t addr;
  struct sensei_supply supply;
};

struct scd41_data {
  bool powered; /* rail on */
  bool asleep;  /* power-down command sent, rail on */
  bool probed;
  bool present;
  bool asc_enable;
  uint16_t asc_initial_h;
  uint16_t asc_standard_h;
  bool asc_set;
  bool asc_applied; /* the settings are in the sensor's RAM */
  uint32_t pressure_pa;
  uint16_t co2_ppm;
  int32_t temp_mdeg;
  int32_t rh_mpct;
};

/* Take the HAL lock, select the bus and the sensor address. Pair with sensirion_hal_release(). */
static void lib_enter(const struct scd41_config *cfg) {
  sensirion_hal_acquire(cfg->bus);
  scd4x_init(cfg->addr);
}

static int apply_asc(const struct scd41_config *cfg, struct scd41_data *d) {
  int16_t e;

  lib_enter(cfg);
  e = scd4x_set_automatic_self_calibration_enabled(d->asc_enable ? 1 : 0);
  if (e == 0 && d->asc_enable) {
    e = scd4x_set_automatic_self_calibration_initial_period(d->asc_initial_h);
    if (e == 0) {
      e = scd4x_set_automatic_self_calibration_standard_period(d->asc_standard_h);
    }
  }
  sensirion_hal_release();
  if (e == 0) {
    d->asc_applied = true;
  }
  return e ? -EIO : 0;
}

static int check_chip(const struct scd41_config *cfg) {
  uint16_t sn[3];
  scd4x_sensor_variant var = 0;
  int16_t e;

  lib_enter(cfg);
  scd4x_stop_periodic_measurement(); /* in case it was running; ignored when idle */
  e = scd4x_get_serial_number(sn, 3);
  if (e == 0) {
    e = scd4x_get_sensor_variant(&var);
  }
  sensirion_hal_release();
  if (e != 0 || (var & SCD4X_SENSOR_VARIANT_MASK) != SCD4X_SENSOR_VARIANT_SCD41) {
    return -ENODEV;
  }
  return 0;
}

static int scd41_power_up(const struct device *dev) {
  const struct scd41_config *cfg = dev->config;
  struct scd41_data *d = dev->data;
  int ret;

  if (d->powered) {
    return 0;
  }
  ret = sensei_supply_enable(&cfg->supply);
  if (ret < 0) {
    return ret;
  }
  d->powered = true;
  d->asleep = false;
  if (!d->probed) {
    d->present = check_chip(cfg) == 0;
    d->probed = d->present; /* a chip that did not answer is probed again on the next power-up */
  }
  if (!d->present) {
    (void)sensei_supply_disable(&cfg->supply);
    d->powered = false;
    return -ENODEV;
  }
  if (d->asc_set) {
    ret = apply_asc(cfg, d);
    if (ret < 0) {
      LOG_WRN("ASC settings failed (%d)", ret);
    }
  }
  return 0;
}

static int scd41_power_down(const struct device *dev) {
  const struct scd41_config *cfg = dev->config;
  struct scd41_data *d = dev->data;

  if (!d->powered) {
    return 0;
  }
  d->powered = false;
  d->asleep = false;
  d->asc_applied = false; /* the settings are lost with the power */
  return sensei_supply_disable(&cfg->supply);
}

static int scd41_pm_action(const struct device *dev, enum pm_device_action action) {
  switch (action) {
  case PM_DEVICE_ACTION_RESUME:
    return scd41_power_up(dev);
  case PM_DEVICE_ACTION_SUSPEND:
    return scd41_power_down(dev);
  default:
    return -ENOTSUP;
  }
}

int sensei_scd41_probe(const struct device *dev) {
  struct scd41_data *d = dev->data;
  int ret;

  if (!d->probed) {
    ret = pm_device_runtime_get(dev);
    if (ret < 0) {
      return ret;
    }
    (void)pm_device_runtime_put(dev);
  }
  return d->present ? 0 : -ENODEV;
}

int sensei_scd41_set_asc(const struct device *dev, bool enable, uint16_t initial_hours, uint16_t standard_hours) {
  const struct scd41_config *cfg = dev->config;
  struct scd41_data *d = dev->data;
  bool changed = !d->asc_set || d->asc_enable != enable || d->asc_initial_h != initial_hours ||
                 d->asc_standard_h != standard_hours;

  d->asc_enable = enable;
  d->asc_initial_h = initial_hours;
  d->asc_standard_h = standard_hours;
  d->asc_set = true;
  if (changed) {
    d->asc_applied = false;
    if (d->powered && !d->asleep) {
      return apply_asc(cfg, d);
    }
  }
  return 0;
}

void sensei_scd41_set_pressure(const struct device *dev, uint32_t pressure_pa) {
  struct scd41_data *d = dev->data;

  d->pressure_pa = pressure_pa;
}

int sensei_scd41_start(const struct device *dev) {
  const struct scd41_config *cfg = dev->config;
  struct scd41_data *d = dev->data;
  uint32_t us;
  int16_t e;

  if (!d->powered) {
    return -ENOTCONN;
  }
  lib_enter(cfg);
  if (d->asleep) {
    scd4x_wake_up(); /* answers with a NACK, ignored */
    d->asleep = false;
  }
  sensirion_hal_release();
  /* Settings changed while the sensor slept are written now. */
  if (d->asc_set && !d->asc_applied && apply_asc(cfg, d) < 0) {
    LOG_WRN("ASC settings failed");
  }
  lib_enter(cfg);
  if (d->pressure_pa >= 70000 && d->pressure_pa <= 120000) {
    scd4x_set_ambient_pressure(d->pressure_pa);
  }
  sensirion_hal_defer_begin(SCD41_SHOT_DEFER_MIN_US);
  e = scd4x_measure_single_shot();
  us = sensirion_hal_defer_end();
  sensirion_hal_release();
  if (e != 0) {
    return -EIO;
  }
  return (int)((us + 999) / 1000);
}

int sensei_scd41_fetch(const struct device *dev) {
  const struct scd41_config *cfg = dev->config;
  struct scd41_data *d = dev->data;
  bool ready = false;
  uint16_t co2;
  int32_t t;
  int32_t rh;

  if (!d->powered) {
    return -ENOTCONN;
  }
  lib_enter(cfg);
  if (scd4x_get_data_ready_status(&ready) != 0) {
    sensirion_hal_release();
    return -EIO;
  }
  if (!ready) {
    sensirion_hal_release();
    return -EBUSY;
  }
  if (scd4x_read_measurement(&co2, &t, &rh) != 0) {
    sensirion_hal_release();
    return -EIO;
  }
  sensirion_hal_release();
  d->co2_ppm = co2;
  d->temp_mdeg = t;
  d->rh_mpct = rh;
  return 0;
}

int sensei_scd41_sleep(const struct device *dev) {
  const struct scd41_config *cfg = dev->config;
  struct scd41_data *d = dev->data;
  int16_t e;

  if (!d->powered) {
    return 0;
  }
  lib_enter(cfg);
  e = scd4x_power_down();
  sensirion_hal_release();
  if (e == 0) {
    d->asleep = true;
  }
  return e ? -EIO : 0;
}

int sensei_scd41_forced_recalibration(const struct device *dev, uint16_t target_ppm, int16_t *correction) {
  const struct scd41_config *cfg = dev->config;
  struct scd41_data *d = dev->data;
  uint16_t frc = 0;
  int16_t e;

  if (!d->powered) {
    return -ENOTCONN;
  }
  lib_enter(cfg);
  if (d->asleep) {
    scd4x_wake_up();
    d->asleep = false;
  }
  e = scd4x_perform_forced_recalibration(target_ppm, &frc);
  sensirion_hal_release();
  if (e != 0 || frc == 0xFFFF) {
    return -EIO;
  }
  if (correction != NULL) {
    *correction = (int16_t)((int32_t)frc - 0x8000);
  }
  return 0;
}

static int scd41_sample_fetch(const struct device *dev, enum sensor_channel chan) {
  int64_t end;
  int ret;
  int ms;

  if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_CO2 && chan != SENSOR_CHAN_AMBIENT_TEMP &&
      chan != SENSOR_CHAN_HUMIDITY) {
    return -ENOTSUP;
  }
  ret = pm_device_runtime_get(dev);
  if (ret < 0) {
    return ret;
  }
  ms = sensei_scd41_start(dev);
  if (ms < 0) {
    ret = ms;
    goto out;
  }
  k_msleep(ms);
  end = k_uptime_get() + SCD41_TIMEOUT_MS;
  do {
    ret = sensei_scd41_fetch(dev);
    if (ret != -EBUSY) {
      break;
    }
    k_msleep(50);
  } while (k_uptime_get() < end);
  if (ret == -EBUSY) {
    ret = -ETIMEDOUT;
  }
out:
  (void)pm_device_runtime_put(dev);
  return ret;
}

static int scd41_channel_get(const struct device *dev, enum sensor_channel chan, struct sensor_value *val) {
  const struct scd41_data *d = dev->data;

  switch (chan) {
  case SENSOR_CHAN_CO2:
    val->val1 = d->co2_ppm;
    val->val2 = 0;
    return 0;
  case SENSOR_CHAN_AMBIENT_TEMP:
    val->val1 = d->temp_mdeg / 1000;
    val->val2 = (d->temp_mdeg % 1000) * 1000;
    return 0;
  case SENSOR_CHAN_HUMIDITY:
    val->val1 = d->rh_mpct / 1000;
    val->val2 = (d->rh_mpct % 1000) * 1000;
    return 0;
  default:
    return -ENOTSUP;
  }
}

static DEVICE_API(sensor, scd41_api) = {
    .sample_fetch = scd41_sample_fetch,
    .channel_get = scd41_channel_get,
};

static int scd41_init(const struct device *dev) {
  const struct scd41_config *cfg = dev->config;

  if (!device_is_ready(cfg->bus) || !sensei_supply_is_ready(&cfg->supply)) {
    return -ENODEV;
  }
  /* Rails off and no I2C traffic until the first resume. */
  return pm_device_runtime_enable(dev);
}

#define SCD41_INIT(inst)                                                                                               \
  static struct scd41_data scd41_data_##inst;                                                                          \
  static const struct scd41_config scd41_cfg_##inst = {                                                                \
      .bus = DEVICE_DT_GET(DT_INST_BUS(inst)),                                                                         \
      .addr = DT_INST_REG_ADDR(inst),                                                                                  \
      .supply = SENSEI_SUPPLY_DT_INST_GET(inst),                                                                       \
  };                                                                                                                   \
  PM_DEVICE_DT_INST_DEFINE(inst, scd41_pm_action);                                                                     \
  SENSOR_DEVICE_DT_INST_DEFINE(inst, scd41_init, PM_DEVICE_DT_INST_GET(inst), &scd41_data_##inst, &scd41_cfg_##inst,   \
                               POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &scd41_api);

DT_INST_FOREACH_STATUS_OKAY(SCD41_INIT)
