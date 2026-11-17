/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * Power switches of a shield sensor (vdd-supply and bus-switch-supply, see sensei-sensor-supply.yaml). Used by the
 * shield sensor drivers.
 */

#ifndef SENSEI_DRIVERS_SENSOR_SUPPLY_H_
#define SENSEI_DRIVERS_SENSOR_SUPPLY_H_

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

struct sensei_supply {
  const struct device *vdd;
  const struct device *bus_switch;
  uint32_t vdd_startup_us; /* startup-delay-us of the regulators, waited here (see sensei_supply_enable) */
  uint32_t bus_startup_us;
};

#define SENSEI_SUPPLY_PHANDLE(inst, prop)                                                                              \
  COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, prop), (DEVICE_DT_GET(DT_INST_PHANDLE(inst, prop))), (NULL))

#define SENSEI_SUPPLY_STARTUP_US(inst, prop)                                                                           \
  COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, prop), (DT_PROP_OR(DT_INST_PHANDLE(inst, prop), startup_delay_us, 0)), (0))

#define SENSEI_SUPPLY_DT_INST_GET(inst)                                                                                \
  {                                                                                                                    \
      .vdd = SENSEI_SUPPLY_PHANDLE(inst, vdd_supply),                                                                  \
      .bus_switch = SENSEI_SUPPLY_PHANDLE(inst, bus_switch_supply),                                                    \
      .vdd_startup_us = SENSEI_SUPPLY_STARTUP_US(inst, vdd_supply),                                                    \
      .bus_startup_us = SENSEI_SUPPLY_STARTUP_US(inst, bus_switch_supply),                                             \
  }

static inline bool sensei_supply_is_ready(const struct sensei_supply *s) {
  return (s->vdd == NULL || device_is_ready(s->vdd)) && (s->bus_switch == NULL || device_is_ready(s->bus_switch));
}

/**
 * Rail first, then the bus switch, each followed by its startup-delay-us. The wait is done here: in NCS v3.4.1
 * regulator_enable() waits off-on-delay-us after an enable and startup-delay-us only at the boot of the regulator.
 */
static inline int sensei_supply_enable(const struct sensei_supply *s) {
  int ret = 0;

  if (s->vdd != NULL) {
    ret = regulator_enable(s->vdd);
    if (ret < 0) {
      return ret;
    }
    if (s->vdd_startup_us > 0) {
      k_usleep(s->vdd_startup_us);
    }
  }
  if (s->bus_switch != NULL) {
    ret = regulator_enable(s->bus_switch);
    if (ret < 0 && s->vdd != NULL) {
      (void)regulator_disable(s->vdd);
    } else if (ret >= 0 && s->bus_startup_us > 0) {
      k_usleep(s->bus_startup_us);
    }
  }
  return ret;
}

/** Bus switch first, then the rail. */
static inline int sensei_supply_disable(const struct sensei_supply *s) {
  int ret = 0;

  if (s->bus_switch != NULL) {
    ret = regulator_disable(s->bus_switch);
  }
  if (s->vdd != NULL) {
    int r2 = regulator_disable(s->vdd);

    ret = ret < 0 ? ret : r2;
  }
  return ret;
}

#endif /* SENSEI_DRIVERS_SENSOR_SUPPLY_H_ */
