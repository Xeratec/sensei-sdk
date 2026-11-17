/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "pwr/pwr_check.h"

#include <zephyr/pm/device_runtime.h>

int sensei_pwr_check_idle(sensei_pwr_idle_cb cb, void *user) {
  const struct device *devs;
  size_t n = z_device_get_all_static(&devs);
  int found = 0;

  for (size_t i = 0; i < n; i++) {
    const struct device *dev = &devs[i];
    enum pm_device_state state;

    if (!device_is_ready(dev) || pm_device_state_get(dev, &state) != 0) {
      continue; /* not ready, or no device PM */
    }
    uint32_t usage = 0;
#ifdef CONFIG_PM_DEVICE_RUNTIME
    usage = dev->pm_base->usage;
#endif
    if ((state == PM_DEVICE_STATE_SUSPENDED || state == PM_DEVICE_STATE_OFF) && usage == 0) {
      continue;
    }
    found++;
    if (cb != NULL) {
      struct sensei_pwr_dev_info info = {
          .dev = dev,
          .state = state,
          .usage = usage,
          .runtime = pm_device_runtime_is_enabled(dev),
      };
      cb(&info, user);
    }
  }
  return found;
}
