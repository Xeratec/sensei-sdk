/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>

#include "marker/sensei_marker.h"

static const struct gpio_dt_spec pin[SENSEI_MARKER_PINS] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(marker0), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(marker1), gpios),
};
static atomic_t active[SENSEI_MARKER_PINS]; /* one bit per source */

void sensei_marker_init(void) {
  for (int i = 0; i < SENSEI_MARKER_PINS; i++) {
    if (gpio_is_ready_dt(&pin[i])) {
      gpio_pin_configure_dt(&pin[i], GPIO_OUTPUT_INACTIVE);
    }
  }
}

void sensei_marker_set(unsigned p, unsigned source, bool on) {
  if (p >= SENSEI_MARKER_PINS || source >= SENSEI_MARKER_SOURCES) {
    return;
  }
  atomic_val_t bit = BIT(source);
  atomic_val_t v = on ? atomic_or(&active[p], bit) | bit : atomic_and(&active[p], ~bit) & ~bit;

  if (gpio_is_ready_dt(&pin[p])) {
    gpio_pin_set_dt(&pin[p], v != 0);
  }
}
