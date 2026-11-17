/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <errno.h>
#include <zephyr/kernel.h>

#include <sensei/drivers/lis2duxs12_extra.h>
#include <sensei_sensor_math.h>

#include "lis2duxs12_reg.h"

static int32_t x_write(void *handle, uint8_t reg, const uint8_t *buf, uint16_t len) {
  return i2c_burst_write_dt(handle, reg, buf, len);
}

static int32_t x_read(void *handle, uint8_t reg, uint8_t *buf, uint16_t len) {
  return i2c_burst_read_dt(handle, reg, buf, len);
}

static void make_ctx(stmdev_ctx_t *ctx, const struct i2c_dt_spec *i2c) {
  *ctx = (stmdev_ctx_t){.write_reg = x_write, .read_reg = x_read, .handle = (void *)i2c};
}

int sensei_lis2duxs12_wake_enable(const struct i2c_dt_spec *i2c, const struct sensei_lis2duxs12_wake_cfg *cfg) {
  static const lis2duxs12_wake_dur_t dur[] = {LIS2DUXS12_0_ODR, LIS2DUXS12_1_ODR, LIS2DUXS12_2_ODR, LIS2DUXS12_3_ODR};
  stmdev_ctx_t ctx;
  lis2duxs12_pin_int_route_t route = {0};
  lis2duxs12_int_config_t icfg = {.int_cfg = LIS2DUXS12_INT_LATCHED};
  lis2duxs12_wakeup_config_t wk = {0};

  if (cfg == NULL || cfg->duration > 3 || cfg->threshold_mg == 0 ||
      (cfg->fs_g != 2 && cfg->fs_g != 4 && cfg->fs_g != 8 && cfg->fs_g != 16)) {
    return -EINVAL;
  }
  make_ctx(&ctx, i2c);

  /* The threshold field has 6 bits: fine step (FS/256) up to 63, then the coarse step (FS/64), capped. */
  uint8_t weight, ths;

  (void)sensei_lis2duxs12_wake_ths(cfg->fs_g, cfg->threshold_mg, &weight, &ths);
  wk.wake_ths_weight = weight;
  wk.wake_ths = ths;
  wk.wake_dur = dur[cfg->duration];
  wk.sleep_dur = 1;
  wk.wake_enable = LIS2DUXS12_SLEEP_ON;
  wk.inact_odr = LIS2DUXS12_ODR_1_6_HZ;
  route.wake_up = 1;

  if (lis2duxs12_wakeup_config_set(&ctx, wk) != 0 || lis2duxs12_int_config_set(&ctx, &icfg) != 0 ||
      lis2duxs12_pin_int1_route_set(&ctx, &route) != 0) {
    return -EIO;
  }
  return 0;
}

int sensei_lis2duxs12_wake_disable(const struct i2c_dt_spec *i2c) {
  stmdev_ctx_t ctx;
  lis2duxs12_pin_int_route_t route = {0};
  lis2duxs12_wakeup_config_t wk = {.wake_enable = LIS2DUXS12_SLEEP_OFF, .inact_odr = LIS2DUXS12_ODR_NO_CHANGE};

  make_ctx(&ctx, i2c);
  if (lis2duxs12_pin_int1_route_set(&ctx, &route) != 0 || lis2duxs12_wakeup_config_set(&ctx, wk) != 0) {
    return -EIO;
  }
  return 0;
}

int sensei_lis2duxs12_wake_event(const struct i2c_dt_spec *i2c, bool *woke) {
  stmdev_ctx_t ctx;
  lis2duxs12_all_sources_t src = {0};

  make_ctx(&ctx, i2c);
  if (lis2duxs12_all_sources_get(&ctx, &src) != 0) {
    return -EIO;
  }
  *woke = src.wake_up;
  return 0;
}

int sensei_lis2duxs12_deep_power_down(const struct i2c_dt_spec *i2c) {
  stmdev_ctx_t ctx;
  lis2duxs12_md_t md = {.odr = 0};

  make_ctx(&ctx, i2c);
  /* Output data rate off first, then deep power-down. */
  if (lis2duxs12_mode_set(&ctx, &md) != 0 || lis2duxs12_enter_deep_power_down(&ctx, 1) != 0) {
    return -EIO;
  }
  return 0;
}

int sensei_lis2duxs12_wake_from_deep_power_down(const struct i2c_dt_spec *i2c) {
  stmdev_ctx_t ctx;

  make_ctx(&ctx, i2c);
  if (lis2duxs12_exit_deep_power_down(&ctx) != 0) {
    return -EIO;
  }
  k_msleep(25);
  return 0;
}
