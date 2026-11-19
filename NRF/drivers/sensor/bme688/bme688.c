/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * BME688 with the Bosch BME68x SensorAPI, forced mode with one heater step.
 * start: set forced mode, return the measurement time (T/P/H plus heater).
 * fetch: read the fresh data set. The sensor sleeps between forced measurements.
 * The chip is set up at the first call (probe), not at boot, so the driver does not depend on the board rails
 * being up when Zephyr initialises it. BSEC is not used.
 */

#define DT_DRV_COMPAT bosch_bme688

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <sensei/drivers/bme688.h>

#include "bme68x.h"

LOG_MODULE_REGISTER(bme688, CONFIG_SENSOR_LOG_LEVEL);

#define BME688_TIMEOUT_MS 1000

struct bme688_config {
  struct i2c_dt_spec i2c;
  uint16_t heater_temp_c;
  uint16_t heater_dur_ms;
  uint8_t os_temp;
  uint8_t os_pres;
  uint8_t os_hum;
};

struct bme688_data {
  struct bme68x_dev dev;
  struct bme68x_conf conf;
  struct bme68x_heatr_conf heatr;
  bool probed;
  bool present;
  bool heater_dirty;
  bool heater_off; /* kept across the probe, which loads the devicetree defaults */
  uint32_t meas_ms;
  struct bme68x_data sample;
};

static BME68X_INTF_RET_TYPE bme_read(uint8_t reg, uint8_t *data, uint32_t len, void *intf) {
  const struct i2c_dt_spec *i2c = intf;

  return i2c_burst_read_dt(i2c, reg, data, len) ? BME68X_E_COM_FAIL : BME68X_INTF_RET_SUCCESS;
}

static BME68X_INTF_RET_TYPE bme_write(uint8_t reg, const uint8_t *data, uint32_t len, void *intf) {
  const struct i2c_dt_spec *i2c = intf;

  return i2c_burst_write_dt(i2c, reg, data, len) ? BME68X_E_COM_FAIL : BME68X_INTF_RET_SUCCESS;
}

static void bme_delay_us(uint32_t us, void *intf) {
  ARG_UNUSED(intf);
  k_usleep(us);
}

static uint8_t os_code(uint8_t n) {
  switch (n) {
  case 1:
    return BME68X_OS_1X;
  case 2:
    return BME68X_OS_2X;
  case 4:
    return BME68X_OS_4X;
  case 8:
    return BME68X_OS_8X;
  case 16:
    return BME68X_OS_16X;
  default:
    return BME68X_OS_NONE;
  }
}

static int apply_heater(const struct device *dev) {
  struct bme688_data *d = dev->data;

  if (bme68x_set_heatr_conf(BME68X_FORCED_MODE, &d->heatr, &d->dev) != BME68X_OK) {
    return -EIO;
  }
  d->meas_ms = bme68x_get_meas_dur(BME68X_FORCED_MODE, &d->conf, &d->dev) / 1000 +
               (d->heatr.enable == BME68X_ENABLE ? d->heatr.heatr_dur : 0) + 2;
  d->heater_dirty = false;
  return 0;
}

int sensei_bme688_probe(const struct device *dev) {
  const struct bme688_config *cfg = dev->config;
  struct bme688_data *d = dev->data;

  if (d->probed) {
    return d->present ? 0 : -ENODEV;
  }
  d->dev.intf = BME68X_I2C_INTF;
  d->dev.read = bme_read;
  d->dev.write = bme_write;
  d->dev.delay_us = bme_delay_us;
  d->dev.intf_ptr = (void *)&cfg->i2c;
  d->dev.amb_temp = 25;
  if (bme68x_init(&d->dev) != BME68X_OK) {
    return -ENODEV; /* try again at the next call */
  }
  d->conf.os_hum = os_code(cfg->os_hum);
  d->conf.os_temp = os_code(cfg->os_temp);
  d->conf.os_pres = os_code(cfg->os_pres);
  d->conf.filter = BME68X_FILTER_OFF;
  d->conf.odr = BME68X_ODR_NONE;
  if (bme68x_set_conf(&d->conf, &d->dev) != BME68X_OK) {
    return -EIO;
  }
  d->heatr.enable = d->heater_off ? BME68X_DISABLE : BME68X_ENABLE;
  d->heatr.heatr_temp = cfg->heater_temp_c;
  d->heatr.heatr_dur = cfg->heater_dur_ms;
  if (apply_heater(dev) < 0) {
    return -EIO;
  }
  d->probed = true;
  d->present = true;
  return 0;
}

int sensei_bme688_set_heater(const struct device *dev, uint16_t temp_c, uint16_t duration_ms) {
  struct bme688_data *d = dev->data;

  if (temp_c < 200 || temp_c > 400 || duration_ms < 1 || duration_ms > 4032) {
    return -EINVAL;
  }
  d->heatr.heatr_temp = temp_c;
  d->heatr.heatr_dur = duration_ms;
  d->heater_dirty = true; /* written at the next start, once the chip is set up */
  return 0;
}

int sensei_bme688_set_heater_enabled(const struct device *dev, bool enabled) {
  struct bme688_data *d = dev->data;

  d->heater_off = !enabled;
  d->heatr.enable = enabled ? BME68X_ENABLE : BME68X_DISABLE;
  d->heater_dirty = true; /* written at the next start, once the chip is set up */
  return 0;
}

int sensei_bme688_start(const struct device *dev) {
  struct bme688_data *d = dev->data;
  int ret = sensei_bme688_probe(dev);

  if (ret < 0) {
    return ret;
  }
  if (d->heater_dirty && apply_heater(dev) < 0) {
    return -EIO;
  }
  if (bme68x_set_op_mode(BME68X_FORCED_MODE, &d->dev) != BME68X_OK) {
    return -EIO;
  }
  return (int)d->meas_ms;
}

int sensei_bme688_fetch(const struct device *dev) {
  struct bme688_data *d = dev->data;
  struct bme68x_data s;
  uint8_t n = 0;
  int8_t r;

  if (!d->present) {
    return -ENODEV;
  }
  r = bme68x_get_data(BME68X_FORCED_MODE, &s, &n, &d->dev);
  if (r == BME68X_W_NO_NEW_DATA || (r == BME68X_OK && n == 0)) {
    return -EBUSY;
  }
  if (r != BME68X_OK) {
    return -EIO;
  }
  d->sample = s;
  return 0;
}

static int bme688_sample_fetch(const struct device *dev, enum sensor_channel chan) {
  int64_t end;
  int ret;

  if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_AMBIENT_TEMP && chan != SENSOR_CHAN_PRESS &&
      chan != SENSOR_CHAN_HUMIDITY && chan != SENSOR_CHAN_GAS_RES) {
    return -ENOTSUP;
  }
  ret = sensei_bme688_start(dev);
  if (ret < 0) {
    return ret;
  }
  k_msleep(ret);
  end = k_uptime_get() + BME688_TIMEOUT_MS;
  do {
    ret = sensei_bme688_fetch(dev);
    if (ret != -EBUSY) {
      return ret;
    }
    k_msleep(5);
  } while (k_uptime_get() < end);
  return -ETIMEDOUT;
}

static void float_to_val(float v, struct sensor_value *val) {
  val->val1 = (int32_t)v;
  val->val2 = (int32_t)((v - (float)val->val1) * 1000000.0f);
}

static int bme688_channel_get(const struct device *dev, enum sensor_channel chan, struct sensor_value *val) {
  const struct bme688_data *d = dev->data;

  switch (chan) {
  case SENSOR_CHAN_AMBIENT_TEMP:
    float_to_val(d->sample.temperature, val);
    return 0;
  case SENSOR_CHAN_PRESS:
    float_to_val(d->sample.pressure / 1000.0f, val); /* Pa to kPa */
    return 0;
  case SENSOR_CHAN_HUMIDITY:
    float_to_val(d->sample.humidity, val);
    return 0;
  case SENSOR_CHAN_GAS_RES: {
    bool ok = (d->sample.status & BME68X_GASM_VALID_MSK) && (d->sample.status & BME68X_HEAT_STAB_MSK);

    float_to_val(ok ? d->sample.gas_resistance : 0.0f, val);
    return 0;
  }
  default:
    return -ENOTSUP;
  }
}

static DEVICE_API(sensor, bme688_api) = {
    .sample_fetch = bme688_sample_fetch,
    .channel_get = bme688_channel_get,
};

static int bme688_init(const struct device *dev) {
  const struct bme688_config *cfg = dev->config;

  /* No traffic until the first call: the board rails may not be up yet. */
  return i2c_is_ready_dt(&cfg->i2c) ? 0 : -ENODEV;
}

#define BME688_INIT(inst)                                                                                              \
  static struct bme688_data bme688_data_##inst;                                                                        \
  static const struct bme688_config bme688_cfg_##inst = {                                                              \
      .i2c = I2C_DT_SPEC_INST_GET(inst),                                                                               \
      .heater_temp_c = DT_INST_PROP(inst, heater_temp_c),                                                              \
      .heater_dur_ms = DT_INST_PROP(inst, heater_duration_ms),                                                         \
      .os_temp = DT_INST_PROP(inst, oversampling_temperature),                                                         \
      .os_pres = DT_INST_PROP(inst, oversampling_pressure),                                                            \
      .os_hum = DT_INST_PROP(inst, oversampling_humidity),                                                             \
  };                                                                                                                   \
  SENSOR_DEVICE_DT_INST_DEFINE(inst, bme688_init, NULL, &bme688_data_##inst, &bme688_cfg_##inst, POST_KERNEL,          \
                               CONFIG_SENSOR_INIT_PRIORITY, &bme688_api);

DT_INST_FOREACH_STATUS_OKAY(BME688_INIT)
