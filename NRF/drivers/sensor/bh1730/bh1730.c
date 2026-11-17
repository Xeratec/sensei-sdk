/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * BH1730FVC, one-time measurement (ONE_TIME bit), on the ROHM register library, which has no one-shot call,
 * so the control register is written here. Gain and integration time are written at each start.
 * The shield has no rail switch for this sensor: suspend clears the POWER bit (software power-down), resume
 * sets it. The part ID is read at the first call, not at boot.
 */

#define DT_DRV_COMPAT sensei_bh1730

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>

#include <sensei/drivers/bh1730.h>
#include <sensei_sensor_math.h>

#include "bh1730fvc_reg.h"

LOG_MODULE_REGISTER(bh1730, CONFIG_SENSOR_LOG_LEVEL);

#define BH1730_TIMEOUT_MARGIN_MS 200

struct bh1730_config {
  struct i2c_dt_spec i2c;
  uint8_t gain;
  uint8_t itime;
};

struct bh1730_data {
  bh1730_t bh;
  bool powered;
  bool probed;
  bool present;
  uint16_t vis;
  uint16_t ir;
  uint32_t lux;
};

static int32_t bh_write_reg(void *handle, uint8_t reg, const uint8_t *buf, uint16_t len) {
  return i2c_burst_write_dt(handle, reg, buf, len);
}

static int32_t bh_read_reg(void *handle, uint8_t reg, uint8_t *buf, uint16_t len) {
  return i2c_burst_read_dt(handle, reg, buf, len);
}

static uint32_t gain_x(uint8_t gain_reg) {
  static const uint8_t x[4] = {1, 2, 64, 128};

  return x[gain_reg & 3];
}

/* The part ID can be read in power-down. */
static int check_chip(struct bh1730_data *d) {
  uint8_t id = 0;

  if (bh1730_read_reg(&d->bh.ctx, BH1730_REG_ID, &id, 1) != 0 || (id & 0xF0) != 0x70) {
    return -ENODEV;
  }
  (void)bh1730_power_down(&d->bh);
  return 0;
}

static int bh_probe_once(struct bh1730_data *d) {
  if (!d->probed) {
    d->present = check_chip(d) == 0;
    d->probed = d->present; /* a chip that did not answer is probed again on the next call */
  }
  return d->present ? 0 : -ENODEV;
}

static int bh_pm_action(const struct device *dev, enum pm_device_action action) {
  struct bh1730_data *d = dev->data;
  int ret;

  switch (action) {
  case PM_DEVICE_ACTION_RESUME:
    ret = bh_probe_once(d);
    if (ret < 0) {
      return ret;
    }
    if (bh1730_power_on(&d->bh)) {
      return -EIO;
    }
    d->powered = true;
    return 0;
  case PM_DEVICE_ACTION_SUSPEND:
    if (d->powered) {
      d->powered = false;
      (void)bh1730_power_down(&d->bh);
    }
    return 0;
  default:
    return -ENOTSUP;
  }
}

int sensei_bh1730_probe(const struct device *dev) {
  struct bh1730_data *d = dev->data;

  return bh_probe_once(d);
}

int sensei_bh1730_start(const struct device *dev) {
  const struct bh1730_config *cfg = dev->config;
  struct bh1730_data *d = dev->data;
  uint8_t itime = cfg->itime;
  uint8_t gain = cfg->gain;
  uint8_t ctl = BIT_POWER | BIT_ADC_EN | BIT_ONE_TIME;

  if (!d->powered) {
    return -ENOTCONN;
  }
  if (bh1730_write_reg(&d->bh.ctx, BH1730_REG_TIMING, &itime, 1) ||
      bh1730_write_reg(&d->bh.ctx, BH1730_REG_GAIN, &gain, 1)) {
    return -EIO;
  }
  d->bh.integration_time_us = (256u - itime) * 2700u; /* as in bh1730_init() */
  d->bh.gain = gain_x(gain);
  if (bh1730_write_reg(&d->bh.ctx, BH1730_REG_CONTROL, &ctl, 1)) {
    return -EIO;
  }
  return (int)sensei_bh1730_meas_ms(itime);
}

int sensei_bh1730_fetch(const struct device *dev) {
  struct bh1730_data *d = dev->data;
  uint8_t valid = 0;
  uint16_t vis;
  uint16_t ir;
  uint32_t lux;

  if (!d->powered) {
    return -ENOTCONN;
  }
  if (bh1730_valid(&d->bh, &valid) != 0) {
    return -EIO;
  }
  if (!valid) {
    return -EBUSY;
  }
  if (bh1730_read_visible(&d->bh, &vis) || bh1730_read_ir(&d->bh, &ir) || bh1730_read_lux(&d->bh, &lux)) {
    return -EIO;
  }
  d->vis = vis;
  d->ir = ir;
  d->lux = lux;
  return 0;
}

static int bh_sample_fetch(const struct device *dev, enum sensor_channel chan) {
  const struct bh1730_config *cfg = dev->config;
  int64_t end;
  int ret;
  int ms;

  if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_LIGHT && chan != SENSEI_BH1730_CHAN_VIS_RAW &&
      chan != SENSEI_BH1730_CHAN_IR_RAW) {
    return -ENOTSUP;
  }
  ret = pm_device_runtime_get(dev);
  if (ret < 0) {
    return ret;
  }
  ms = sensei_bh1730_start(dev);
  if (ms < 0) {
    ret = ms;
    goto out;
  }
  k_msleep(ms);
  end = k_uptime_get() + BH1730_TIMEOUT_MARGIN_MS + sensei_bh1730_meas_ms(cfg->itime);
  do {
    ret = sensei_bh1730_fetch(dev);
    if (ret != -EBUSY) {
      break;
    }
    k_msleep(10);
  } while (k_uptime_get() < end);
  if (ret == -EBUSY) {
    ret = -ETIMEDOUT;
  }
out:
  (void)pm_device_runtime_put(dev);
  return ret;
}

static int bh_channel_get(const struct device *dev, enum sensor_channel chan, struct sensor_value *val) {
  const struct bh1730_data *d = dev->data;

  val->val2 = 0;
  switch ((int)chan) {
  case SENSOR_CHAN_LIGHT:
    val->val1 = (int32_t)d->lux;
    return 0;
  case SENSEI_BH1730_CHAN_VIS_RAW:
    val->val1 = d->vis;
    return 0;
  case SENSEI_BH1730_CHAN_IR_RAW:
    val->val1 = d->ir;
    return 0;
  default:
    return -ENOTSUP;
  }
}

static DEVICE_API(sensor, bh1730_api) = {
    .sample_fetch = bh_sample_fetch,
    .channel_get = bh_channel_get,
};

static int bh_init(const struct device *dev) {
  const struct bh1730_config *cfg = dev->config;
  struct bh1730_data *d = dev->data;

  if (!i2c_is_ready_dt(&cfg->i2c)) {
    return -ENODEV;
  }
  d->bh.ctx.write_reg = bh_write_reg;
  d->bh.ctx.read_reg = bh_read_reg;
  d->bh.ctx.handle = (void *)&cfg->i2c;
  d->bh.integration_time_us = (256u - cfg->itime) * 2700u;
  d->bh.gain = gain_x(cfg->gain);
  /* No I2C traffic until the first call. */
  return pm_device_runtime_enable(dev);
}

#define BH1730_INIT(inst)                                                                                              \
  static struct bh1730_data bh1730_data_##inst;                                                                        \
  static const struct bh1730_config bh1730_cfg_##inst = {                                                              \
      .i2c = I2C_DT_SPEC_INST_GET(inst),                                                                               \
      .gain = DT_INST_PROP(inst, gain),                                                                                \
      .itime = DT_INST_PROP(inst, itime),                                                                              \
  };                                                                                                                   \
  PM_DEVICE_DT_INST_DEFINE(inst, bh_pm_action);                                                                        \
  SENSOR_DEVICE_DT_INST_DEFINE(inst, bh_init, PM_DEVICE_DT_INST_GET(inst), &bh1730_data_##inst, &bh1730_cfg_##inst,    \
                               POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &bh1730_api);

DT_INST_FOREACH_STATUS_OKAY(BH1730_INIT)
