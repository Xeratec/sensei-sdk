/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * AS7331, command mode (one measurement per start), on the AMS register library.
 *
 * The shield has no rail switch for the sensor, only the I2C isolation switch (bus-switch-supply, behind the
 * expander). Resume closes the switch and clears the PD bit; suspend sets the PD bit and opens the switch.
 * Nothing touches the hardware before the first resume. The results are converted to uW/cm2 with the
 * datasheet responsivity (lib/sensei_sensor_math).
 */

#define DT_DRV_COMPAT ams_as7331

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>

#include <sensei/drivers/as7331.h>
#include <sensei/drivers/sensor_supply.h>
#include <sensei_sensor_math.h>

#include "as7331_reg.h"

LOG_MODULE_REGISTER(as7331, CONFIG_SENSOR_LOG_LEVEL);

#define AS7331_BREAK_TIME 255
#define AS7331_RESET_MS 5
#define AS7331_POWER_UP_MS 2
#define AS7331_TIMEOUT_MARGIN_MS 500

struct as7331_config {
  struct i2c_dt_spec i2c;
  struct sensei_supply supply;
  struct gpio_dt_spec ready;
  uint8_t gain;
  uint8_t time;
};

struct as7331_data {
  as7331_t as;
  bool powered;
  bool probed;
  bool present;
  uint8_t gain;
  uint8_t time;
  uint8_t meas_gain; /* settings of the measurement in the result registers */
  uint8_t meas_time;
  uint16_t raw[4]; /* temp, uva, uvb, uvc */
};

static int32_t as_write_reg(void *handle, uint8_t reg, const uint8_t *buf, uint16_t len) {
  return i2c_burst_write_dt(handle, reg, buf, len);
}

static int32_t as_read_reg(void *handle, uint8_t reg, uint8_t *buf, uint16_t len) {
  return i2c_burst_read_dt(handle, reg, buf, len);
}

static int check_chip(const struct as7331_config *cfg, struct as7331_data *d) {
  uint8_t id = 0;
  int ret;

  as7331_reset(&d->as);
  k_msleep(AS7331_RESET_MS);
  as7331_power_up(&d->as);
  as7331_set_configuration_mode(&d->as);
  ret = as7331_get_chip_id(&d->as, &id);
  if (ret != 0 || (id >> 4) != 0x2) {
    return -ENODEV;
  }
  if (cfg->ready.port != NULL && gpio_pin_configure_dt(&cfg->ready, GPIO_INPUT) < 0) {
    LOG_WRN("READY pin setup failed");
  }
  return 0;
}

static int as7331_power_up_dev(const struct device *dev) {
  const struct as7331_config *cfg = dev->config;
  struct as7331_data *d = dev->data;
  int ret;

  if (d->powered) {
    return 0;
  }
  ret = sensei_supply_enable(&cfg->supply);
  if (ret < 0) {
    return ret;
  }
  d->powered = true;
  if (!d->probed) {
    d->present = check_chip(cfg, d) == 0;
    d->probed = true;
  }
  if (!d->present) {
    (void)sensei_supply_disable(&cfg->supply);
    d->powered = false;
    return -ENODEV;
  }
  return as7331_power_up(&d->as) ? -EIO : 0;
}

static int as7331_power_down_dev(const struct device *dev) {
  const struct as7331_config *cfg = dev->config;
  struct as7331_data *d = dev->data;

  if (!d->powered) {
    return 0;
  }
  (void)as7331_power_down(&d->as);
  d->powered = false;
  return sensei_supply_disable(&cfg->supply);
}

static int as7331_pm_action(const struct device *dev, enum pm_device_action action) {
  switch (action) {
  case PM_DEVICE_ACTION_RESUME:
    return as7331_power_up_dev(dev);
  case PM_DEVICE_ACTION_SUSPEND:
    return as7331_power_down_dev(dev);
  default:
    return -ENOTSUP;
  }
}

int sensei_as7331_probe(const struct device *dev) {
  struct as7331_data *d = dev->data;
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

int sensei_as7331_configure(const struct device *dev, uint8_t gain, uint8_t time) {
  struct as7331_data *d = dev->data;

  if (gain > 11 || time > 14) {
    return -EINVAL;
  }
  d->gain = gain;
  d->time = time;
  return 0;
}

int sensei_as7331_start(const struct device *dev) {
  struct as7331_data *d = dev->data;

  if (!d->powered) {
    return -ENOTCONN;
  }
  if (as7331_power_up(&d->as)) {
    return -EIO;
  }
  k_msleep(AS7331_POWER_UP_MS);
  if (as7331_set_configuration_mode(&d->as) ||
      as7331_init(&d->as, AS7331_CMD_MODE, AS7331_1024, 0x00, AS7331_BREAK_TIME, d->gain, d->time) ||
      as7331_set_measurement_mode(&d->as) || as7331_start_measurement(&d->as)) {
    return -EIO;
  }
  d->meas_gain = d->gain;
  d->meas_time = d->time;
  return (1 << d->time) + 20;
}

int sensei_as7331_fetch(const struct device *dev) {
  struct as7331_data *d = dev->data;
  as7331_reg_osrstat_t st;

  if (!d->powered) {
    return -ENOTCONN;
  }
  if (as7331_get_status(&d->as, &st)) {
    return -EIO;
  }
  if (!st.ndata) {
    return -EBUSY;
  }
  if (as7331_read_all(&d->as, d->raw)) {
    return -EIO;
  }
  return 0;
}

int sensei_as7331_ready(const struct device *dev) {
  const struct as7331_config *cfg = dev->config;

  if (cfg->ready.port == NULL) {
    return -ENOTSUP;
  }
  return gpio_pin_get_dt(&cfg->ready);
}

static int as7331_sample_fetch(const struct device *dev, enum sensor_channel chan) {
  int64_t end;
  int ret;
  int ms;

  if (chan != SENSOR_CHAN_ALL && chan != SENSEI_AS7331_CHAN_UVA && chan != SENSEI_AS7331_CHAN_UVB &&
      chan != SENSEI_AS7331_CHAN_UVC && chan != SENSOR_CHAN_DIE_TEMP) {
    return -ENOTSUP;
  }
  ret = pm_device_runtime_get(dev);
  if (ret < 0) {
    return ret;
  }
  ms = sensei_as7331_start(dev);
  if (ms < 0) {
    ret = ms;
    goto out;
  }
  k_msleep(ms);
  end = k_uptime_get() + AS7331_TIMEOUT_MARGIN_MS;
  do {
    ret = sensei_as7331_fetch(dev);
    if (ret != -EBUSY) {
      break;
    }
    k_msleep(20);
  } while (k_uptime_get() < end);
  if (ret == -EBUSY) {
    ret = -ETIMEDOUT;
  }
out:
  (void)pm_device_runtime_put(dev);
  return ret;
}

static void float_to_val(float v, struct sensor_value *val) {
  val->val1 = (int32_t)v;
  val->val2 = (int32_t)((v - (float)val->val1) * 1000000.0f);
}

static int as7331_channel_get(const struct device *dev, enum sensor_channel chan, struct sensor_value *val) {
  const struct as7331_data *d = dev->data;

  switch ((int)chan) {
  case SENSOR_CHAN_DIE_TEMP:
    float_to_val(sensei_as7331_temp_c(d->raw[0]), val);
    return 0;
  case SENSEI_AS7331_CHAN_UVA:
    float_to_val(sensei_as7331_uw_cm2(SENSEI_AS7331_UVA, d->raw[1], d->meas_gain, d->meas_time), val);
    return 0;
  case SENSEI_AS7331_CHAN_UVB:
    float_to_val(sensei_as7331_uw_cm2(SENSEI_AS7331_UVB, d->raw[2], d->meas_gain, d->meas_time), val);
    return 0;
  case SENSEI_AS7331_CHAN_UVC:
    float_to_val(sensei_as7331_uw_cm2(SENSEI_AS7331_UVC, d->raw[3], d->meas_gain, d->meas_time), val);
    return 0;
  default:
    return -ENOTSUP;
  }
}

static DEVICE_API(sensor, as7331_api) = {
    .sample_fetch = as7331_sample_fetch,
    .channel_get = as7331_channel_get,
};

static int as7331_init_dev(const struct device *dev) {
  const struct as7331_config *cfg = dev->config;
  struct as7331_data *d = dev->data;

  if (!i2c_is_ready_dt(&cfg->i2c) || !sensei_supply_is_ready(&cfg->supply)) {
    return -ENODEV;
  }
  if (cfg->ready.port != NULL && !gpio_is_ready_dt(&cfg->ready)) {
    return -ENODEV;
  }
  d->as.ctx.write_reg = as_write_reg;
  d->as.ctx.read_reg = as_read_reg;
  d->as.ctx.handle = (void *)&cfg->i2c;
  d->gain = cfg->gain;
  d->time = cfg->time;
  d->meas_gain = cfg->gain;
  d->meas_time = cfg->time;
  /* Bus switch open and no I2C traffic until the first resume. */
  return pm_device_runtime_enable(dev);
}

#define AS7331_INIT(inst)                                                                                              \
  BUILD_ASSERT(DT_INST_PROP(inst, gain) <= 11 && DT_INST_PROP(inst, integration_time) <= 14,                           \
               "AS7331 gain is 0 to 11, integration-time 0 to 14");                                                    \
  static struct as7331_data as7331_data_##inst;                                                                        \
  static const struct as7331_config as7331_cfg_##inst = {                                                              \
      .i2c = I2C_DT_SPEC_INST_GET(inst),                                                                               \
      .supply = SENSEI_SUPPLY_DT_INST_GET(inst),                                                                       \
      .ready = GPIO_DT_SPEC_INST_GET_OR(inst, ready_gpios, {0}),                                                       \
      .gain = DT_INST_PROP(inst, gain),                                                                                \
      .time = DT_INST_PROP(inst, integration_time),                                                                    \
  };                                                                                                                   \
  PM_DEVICE_DT_INST_DEFINE(inst, as7331_pm_action);                                                                    \
  SENSOR_DEVICE_DT_INST_DEFINE(inst, as7331_init_dev, PM_DEVICE_DT_INST_GET(inst), &as7331_data_##inst,                \
                               &as7331_cfg_##inst, POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &as7331_api);

DT_INST_FOREACH_STATUS_OKAY(AS7331_INIT)
