/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * SGP41 raw ticks on the Sensirion SGP41 library. The driver has no state machine: conditioning, measuring
 * and the heater are separate calls, so a scheduler can spread the cycle over time. The gas index is not part
 * of the driver (lib/sensei_gas_index).
 *
 * Power: the rail and the bus switch come from the devicetree (vdd-supply, bus-switch-supply) and follow
 * runtime PM (resume: on, suspend: off). Nothing touches the hardware before the first resume.
 */

#define DT_DRV_COMPAT sensirion_sgp41

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>

#include <sensei/drivers/sensirion_hal.h>
#include <sensei/drivers/sensor_supply.h>
#include <sensei/drivers/sgp41.h>

#include "sgp41_i2c.h"

LOG_MODULE_REGISTER(sgp41, CONFIG_SENSOR_LOG_LEVEL);

/* The library has the address built in. */
#define SGP41_ADDR 0x59
#define SGP41_STEP_MS 1000

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1, "the Sensirion library supports one SGP41");

struct sgp41_config {
  const struct device *bus;
  struct sensei_supply supply;
};

struct sgp41_data {
  bool powered;
  bool probed;
  bool present;
  uint16_t rh_ticks;
  uint16_t t_ticks;
  uint16_t sraw_voc;
  uint16_t sraw_nox;
};

static int check_chip(const struct sgp41_config *cfg) {
  uint16_t sn[3];
  uint16_t res = 0;
  int16_t e;

  sensirion_hal_acquire(cfg->bus);
  e = sgp41_get_serial_number(sn);
  if (e == 0) {
    e = sgp41_execute_self_test(&res);
    if (e == 0 && (res & 0x0F) != 0) {
      LOG_WRN("self test 0x%04x", res);
    }
  }
  sensirion_hal_release();
  return e ? -ENODEV : 0;
}

static int sgp41_power_up(const struct device *dev) {
  const struct sgp41_config *cfg = dev->config;
  struct sgp41_data *d = dev->data;
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
    d->present = check_chip(cfg) == 0;
    d->probed = true;
  }
  if (!d->present) {
    (void)sensei_supply_disable(&cfg->supply);
    d->powered = false;
    return -ENODEV;
  }
  return 0;
}

static int sgp41_power_down(const struct device *dev) {
  const struct sgp41_config *cfg = dev->config;
  struct sgp41_data *d = dev->data;

  if (!d->powered) {
    return 0;
  }
  d->powered = false;
  return sensei_supply_disable(&cfg->supply);
}

static int sgp41_pm_action(const struct device *dev, enum pm_device_action action) {
  switch (action) {
  case PM_DEVICE_ACTION_RESUME:
    return sgp41_power_up(dev);
  case PM_DEVICE_ACTION_SUSPEND:
    return sgp41_power_down(dev);
  default:
    return -ENOTSUP;
  }
}

int sensei_sgp41_probe(const struct device *dev) {
  struct sgp41_data *d = dev->data;
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

int sensei_sgp41_condition(const struct device *dev, uint16_t rh_ticks, uint16_t t_ticks, uint16_t *sraw_voc) {
  const struct sgp41_config *cfg = dev->config;
  const struct sgp41_data *d = dev->data;
  uint16_t voc = 0;
  int16_t e;

  if (!d->powered) {
    return -ENOTCONN;
  }
  sensirion_hal_acquire(cfg->bus);
  e = sgp41_execute_conditioning(rh_ticks, t_ticks, &voc);
  sensirion_hal_release();
  if (e != 0) {
    return -EIO;
  }
  if (sraw_voc != NULL) {
    *sraw_voc = voc;
  }
  return 0;
}

int sensei_sgp41_measure(const struct device *dev, uint16_t rh_ticks, uint16_t t_ticks, uint16_t *sraw_voc,
                         uint16_t *sraw_nox) {
  const struct sgp41_config *cfg = dev->config;
  struct sgp41_data *d = dev->data;
  uint16_t voc = 0;
  uint16_t nox = 0;
  int16_t e;

  if (!d->powered) {
    return -ENOTCONN;
  }
  sensirion_hal_acquire(cfg->bus);
  e = sgp41_measure_raw_signals(rh_ticks, t_ticks, &voc, &nox);
  sensirion_hal_release();
  if (e != 0) {
    return -EIO;
  }
  d->sraw_voc = voc;
  d->sraw_nox = nox;
  if (sraw_voc != NULL) {
    *sraw_voc = voc;
  }
  if (sraw_nox != NULL) {
    *sraw_nox = nox;
  }
  return 0;
}

int sensei_sgp41_heater_off(const struct device *dev) {
  const struct sgp41_config *cfg = dev->config;
  const struct sgp41_data *d = dev->data;
  int16_t e;

  if (!d->powered) {
    return 0;
  }
  sensirion_hal_acquire(cfg->bus);
  e = sgp41_turn_heater_off();
  sensirion_hal_release();
  return e ? -EIO : 0;
}

void sensei_sgp41_set_compensation(const struct device *dev, uint16_t rh_ticks, uint16_t t_ticks) {
  struct sgp41_data *d = dev->data;

  d->rh_ticks = rh_ticks;
  d->t_ticks = t_ticks;
}

static int sgp41_sample_fetch(const struct device *dev, enum sensor_channel chan) {
  struct sgp41_data *d = dev->data;
  int64_t next;
  int ret;

  if (chan != SENSOR_CHAN_ALL && chan != SENSEI_SGP41_CHAN_VOC_RAW && chan != SENSEI_SGP41_CHAN_NOX_RAW) {
    return -ENOTSUP;
  }
  ret = pm_device_runtime_get(dev);
  if (ret < 0) {
    return ret;
  }
  /* Conditioning, one command per second, then the measurement. */
  next = k_uptime_get();
  for (int i = 0; i < CONFIG_SGP41_SENSEI_CONDITIONING_S && ret == 0; i++) {
    ret = sensei_sgp41_condition(dev, d->rh_ticks, d->t_ticks, NULL);
    next += SGP41_STEP_MS;
    k_msleep(MAX(0, next - k_uptime_get()));
  }
  if (ret == 0) {
    ret = sensei_sgp41_measure(dev, d->rh_ticks, d->t_ticks, NULL, NULL);
  }
  (void)pm_device_runtime_put(dev);
  return ret;
}

static int sgp41_channel_get(const struct device *dev, enum sensor_channel chan, struct sensor_value *val) {
  const struct sgp41_data *d = dev->data;

  switch ((int)chan) {
  case SENSEI_SGP41_CHAN_VOC_RAW:
    val->val1 = d->sraw_voc;
    val->val2 = 0;
    return 0;
  case SENSEI_SGP41_CHAN_NOX_RAW:
    val->val1 = d->sraw_nox;
    val->val2 = 0;
    return 0;
  default:
    return -ENOTSUP;
  }
}

static DEVICE_API(sensor, sgp41_api) = {
    .sample_fetch = sgp41_sample_fetch,
    .channel_get = sgp41_channel_get,
};

static int sgp41_init(const struct device *dev) {
  const struct sgp41_config *cfg = dev->config;
  struct sgp41_data *d = dev->data;

  if (!device_is_ready(cfg->bus) || !sensei_supply_is_ready(&cfg->supply)) {
    return -ENODEV;
  }
  d->rh_ticks = 0x8000; /* 50 %RH */
  d->t_ticks = 0x6666;  /* 25 degC */
  /* Rails off and no I2C traffic until the first resume. */
  return pm_device_runtime_enable(dev);
}

#define SGP41_INIT(inst)                                                                                               \
  BUILD_ASSERT(DT_INST_REG_ADDR(inst) == SGP41_ADDR, "the SGP41 library has the address 0x59 built in");               \
  static struct sgp41_data sgp41_data_##inst;                                                                          \
  static const struct sgp41_config sgp41_cfg_##inst = {                                                                \
      .bus = DEVICE_DT_GET(DT_INST_BUS(inst)),                                                                         \
      .supply = SENSEI_SUPPLY_DT_INST_GET(inst),                                                                       \
  };                                                                                                                   \
  PM_DEVICE_DT_INST_DEFINE(inst, sgp41_pm_action);                                                                     \
  SENSOR_DEVICE_DT_INST_DEFINE(inst, sgp41_init, PM_DEVICE_DT_INST_GET(inst), &sgp41_data_##inst, &sgp41_cfg_##inst,   \
                               POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &sgp41_api);

DT_INST_FOREACH_STATUS_OKAY(SGP41_INIT)
