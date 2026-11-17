/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * ILPS28QSW pressure and temperature sensor, I2C, one-shot mode.
 * sample_fetch triggers one conversion and waits for it. In between the sensor is powered down.
 * Untested on hardware.
 */

#define DT_DRV_COMPAT st_ilps28qsw

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include "ilps28qsw_reg.h"

LOG_MODULE_REGISTER(ilps28qsw, CONFIG_SENSOR_LOG_LEVEL);

/* Time to wait for one conversion at most (16 averaged samples take well below 20 ms). */
#define ILPS_CONVERSION_TIMEOUT_MS 100

struct ilps_config {
  struct i2c_dt_spec i2c;
  ilps28qsw_avg_t avg;
  ilps28qsw_fs_t fs;
};

struct ilps_data {
  stmdev_ctx_t ctx;
  ilps28qsw_md_t md;
  float press_hpa;
  float temp_c;
};

static int32_t ilps_write_reg(void *handle, uint8_t reg, const uint8_t *buf, uint16_t len) {
  const struct i2c_dt_spec *i2c = handle;

  return i2c_burst_write_dt(i2c, reg, buf, len);
}

static int32_t ilps_read_reg(void *handle, uint8_t reg, uint8_t *buf, uint16_t len) {
  const struct i2c_dt_spec *i2c = handle;

  return i2c_burst_read_dt(i2c, reg, buf, len);
}

static int ilps_sample_fetch(const struct device *dev, enum sensor_channel chan) {
  struct ilps_data *data = dev->data;
  ilps28qsw_all_sources_t src = {0};
  ilps28qsw_data_t d;
  int64_t end = k_uptime_get() + ILPS_CONVERSION_TIMEOUT_MS;

  if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_PRESS && chan != SENSOR_CHAN_AMBIENT_TEMP) {
    return -ENOTSUP;
  }
  if (ilps28qsw_trigger_sw(&data->ctx, &data->md) != 0) {
    return -EIO;
  }
  do {
    k_msleep(2);
    if (ilps28qsw_all_sources_get(&data->ctx, &src) != 0) {
      return -EIO;
    }
  } while (!src.drdy_pres && k_uptime_get() < end);
  if (!src.drdy_pres) {
    return -ETIMEDOUT;
  }
  if (ilps28qsw_data_get(&data->ctx, &data->md, &d) != 0) {
    return -EIO;
  }
  data->press_hpa = d.pressure.hpa;
  data->temp_c = d.heat.deg_c;
  return 0;
}

static void float_to_val(float v, struct sensor_value *val) {
  val->val1 = (int32_t)v;
  val->val2 = (int32_t)((v - (float)val->val1) * 1000000.0f);
}

static int ilps_channel_get(const struct device *dev, enum sensor_channel chan, struct sensor_value *val) {
  const struct ilps_data *data = dev->data;

  switch (chan) {
  case SENSOR_CHAN_PRESS:
    float_to_val(data->press_hpa / 10.0f, val); /* hPa to kPa */
    return 0;
  case SENSOR_CHAN_AMBIENT_TEMP:
    float_to_val(data->temp_c, val);
    return 0;
  default:
    return -ENOTSUP;
  }
}

static DEVICE_API(sensor, ilps_api) = {
    .sample_fetch = ilps_sample_fetch,
    .channel_get = ilps_channel_get,
};

static int __maybe_unused ilps_pm_action(const struct device *dev, enum pm_device_action action) {
  ARG_UNUSED(dev);

  /* One-shot mode powers down after every conversion, so there is nothing to switch. */
  switch (action) {
  case PM_DEVICE_ACTION_SUSPEND:
  case PM_DEVICE_ACTION_RESUME:
    return 0;
  default:
    return -ENOTSUP;
  }
}

static int ilps_init(const struct device *dev) {
  const struct ilps_config *cfg = dev->config;
  struct ilps_data *data = dev->data;
  ilps28qsw_id_t id = {0};
  ilps28qsw_stat_t st = {0};
  ilps28qsw_bus_mode_t bm = {.filter = ILPS28QSW_AUTO};

  if (!i2c_is_ready_dt(&cfg->i2c)) {
    return -ENODEV;
  }
  data->ctx.write_reg = ilps_write_reg;
  data->ctx.read_reg = ilps_read_reg;
  data->ctx.handle = (void *)&cfg->i2c;
  data->md = (ilps28qsw_md_t){
      .fs = cfg->fs,
      .odr = ILPS28QSW_ONE_SHOT,
      .avg = cfg->avg,
      .lpf = ILPS28QSW_LPF_DISABLE,
  };

  if (ilps28qsw_id_get(&data->ctx, &id) != 0 || id.whoami != ILPS28QSW_ID) {
    LOG_ERR("ILPS28QSW not found (id 0x%02x)", id.whoami);
    return -ENODEV;
  }
  if (ilps28qsw_init_set(&data->ctx, ILPS28QSW_RESET) != 0) {
    return -EIO;
  }
  for (int i = 0; i < 20; i++) {
    if (ilps28qsw_status_get(&data->ctx, &st) != 0) {
      return -EIO;
    }
    if (!st.sw_reset) {
      break;
    }
    k_msleep(1);
  }
  if (st.sw_reset) {
    return -ETIMEDOUT;
  }
  /* QVAR off keeps the power-down current at about 1 uA. */
  ilps28qsw_ah_qvar_en_set(&data->ctx, PROPERTY_DISABLE);
  ilps28qsw_init_set(&data->ctx, ILPS28QSW_DRV_RDY);
  ilps28qsw_bus_mode_set(&data->ctx, &bm);
  return ilps28qsw_mode_set(&data->ctx, &data->md) ? -EIO : 0;
}

#define ILPS_AVG(n)                                                                                                    \
  ((n) == 4     ? ILPS28QSW_4_AVG                                                                                      \
   : (n) == 8   ? ILPS28QSW_8_AVG                                                                                      \
   : (n) == 16  ? ILPS28QSW_16_AVG                                                                                     \
   : (n) == 32  ? ILPS28QSW_32_AVG                                                                                     \
   : (n) == 64  ? ILPS28QSW_64_AVG                                                                                     \
   : (n) == 128 ? ILPS28QSW_128_AVG                                                                                    \
   : (n) == 256 ? ILPS28QSW_256_AVG                                                                                    \
                : ILPS28QSW_512_AVG)

#define ILPS_INIT(inst)                                                                                                \
  static struct ilps_data ilps_data_##inst;                                                                            \
  static const struct ilps_config ilps_cfg_##inst = {                                                                  \
      .i2c = I2C_DT_SPEC_INST_GET(inst),                                                                               \
      .avg = ILPS_AVG(DT_INST_PROP(inst, avg)),                                                                        \
      .fs = DT_INST_PROP(inst, full_scale) == 4060 ? ILPS28QSW_4060hPa : ILPS28QSW_1260hPa,                            \
  };                                                                                                                   \
  PM_DEVICE_DT_INST_DEFINE(inst, ilps_pm_action);                                                                      \
  SENSOR_DEVICE_DT_INST_DEFINE(inst, ilps_init, PM_DEVICE_DT_INST_GET(inst), &ilps_data_##inst, &ilps_cfg_##inst,      \
                               POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &ilps_api);

DT_INST_FOREACH_STATUS_OKAY(ILPS_INIT)
