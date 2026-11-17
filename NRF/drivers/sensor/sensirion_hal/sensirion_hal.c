/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Sensirion I2C HAL on the Zephyr I2C API. */

#include <errno.h>
#include <stdbool.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include "sensirion_i2c_hal.h"
#include <sensei/drivers/sensirion_hal.h>

static K_MUTEX_DEFINE(hal_lock);
static const struct device *hal_bus;
static bool defer_on;
static uint32_t defer_min_us;
static uint32_t defer_sum_us;

void sensirion_hal_acquire(const struct device *bus) {
  k_mutex_lock(&hal_lock, K_FOREVER);
  hal_bus = bus;
  defer_on = false;
}

void sensirion_hal_release(void) {
  defer_on = false;
  k_mutex_unlock(&hal_lock);
}

void sensirion_hal_defer_begin(uint32_t min_us) {
  defer_min_us = min_us;
  defer_sum_us = 0;
  defer_on = true;
}

uint32_t sensirion_hal_defer_end(void) {
  defer_on = false;
  return defer_sum_us;
}

int16_t sensirion_i2c_hal_select_bus(uint8_t bus_idx) {
  /* One bus at a time, chosen with sensirion_hal_acquire(). */
  (void)bus_idx;
  return 0;
}

void sensirion_i2c_hal_init(void) {}

void sensirion_i2c_hal_free(void) {}

int8_t sensirion_i2c_hal_read(uint8_t address, uint8_t *data, uint8_t count) {
  if (hal_bus == NULL) {
    return -ENODEV;
  }
  int ret = i2c_read(hal_bus, data, count, address);

  return ret == 0 ? 0 : (int8_t)(ret < -127 ? -EIO : ret);
}

int8_t sensirion_i2c_hal_write(uint8_t address, const uint8_t *data, uint8_t count) {
  if (hal_bus == NULL) {
    return -ENODEV;
  }
  int ret = i2c_write(hal_bus, data, count, address);

  return ret == 0 ? 0 : (int8_t)(ret < -127 ? -EIO : ret);
}

void sensirion_i2c_hal_sleep_usec(uint32_t useconds) {
  if (defer_on && useconds >= defer_min_us) {
    defer_sum_us += useconds;
    return;
  }
  if (useconds >= 1000) {
    k_msleep((useconds + 999) / 1000);
  } else if (useconds > 0) {
    k_usleep(useconds);
  }
}
