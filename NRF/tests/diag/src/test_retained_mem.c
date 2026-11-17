/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Retained memory driver on a RAM array, so that the diagnostics module and the retention subsystem run on
 * native_sim. A reset is simulated by calling sensei_diag_boot() again (the array stays). */

#define DT_DRV_COMPAT sensei_test_retained_mem

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/retained_mem.h>

struct cfg {
  uint8_t *mem;
  size_t size;
};

static ssize_t t_size(const struct device *dev) {
  const struct cfg *c = dev->config;

  return (ssize_t)c->size;
}

static int t_read(const struct device *dev, off_t off, uint8_t *buf, size_t len) {
  const struct cfg *c = dev->config;

  memcpy(buf, c->mem + off, len);
  return 0;
}

static int t_write(const struct device *dev, off_t off, const uint8_t *buf, size_t len) {
  const struct cfg *c = dev->config;

  memcpy(c->mem + off, buf, len);
  return 0;
}

static int t_clear(const struct device *dev) {
  const struct cfg *c = dev->config;

  memset(c->mem, 0, c->size);
  return 0;
}

static DEVICE_API(retained_mem, t_api) = {
    .size = t_size,
    .read = t_read,
    .write = t_write,
    .clear = t_clear,
};

#define TEST_RETAINED(inst)                                                                                            \
  static uint8_t mem_##inst[DT_INST_PROP(inst, size)];                                                                 \
  static const struct cfg cfg_##inst = {.mem = mem_##inst, .size = DT_INST_PROP(inst, size)};                          \
  DEVICE_DT_INST_DEFINE(inst, NULL, NULL, NULL, &cfg_##inst, PRE_KERNEL_1, CONFIG_RETAINED_MEM_INIT_PRIORITY, &t_api);

DT_INST_FOREACH_STATUS_OKAY(TEST_RETAINED)
