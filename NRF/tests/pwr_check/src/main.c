/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* sensei_pwr_check_idle() (NRF/system/pwr/pwr_check.c) on devices of the test in every power state: it must report
 * what keeps the system from its sleep floor and nothing else. */

#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/ztest.h>

#include "pwr/pwr_check.h"

static int pm_action(const struct device *dev, enum pm_device_action action) {
  ARG_UNUSED(dev);
  switch (action) {
  case PM_DEVICE_ACTION_SUSPEND:
  case PM_DEVICE_ACTION_RESUME:
  case PM_DEVICE_ACTION_TURN_ON:
  case PM_DEVICE_ACTION_TURN_OFF:
    return 0;
  default:
    return -ENOTSUP;
  }
}

static int init_ok(const struct device *dev) {
  ARG_UNUSED(dev);
  return 0;
}

static int init_fails(const struct device *dev) {
  ARG_UNUSED(dev);
  return -EIO;
}

PM_DEVICE_DEFINE(pm_active, pm_action);
PM_DEVICE_DEFINE(pm_runtime_idle, pm_action);
PM_DEVICE_DEFINE(pm_runtime_held, pm_action);
PM_DEVICE_DEFINE(pm_suspended, pm_action);
PM_DEVICE_DEFINE(pm_off, pm_action);
PM_DEVICE_DEFINE(pm_not_ready, pm_action);

#define TEST_DEVICE(name, init)                                                                                        \
  DEVICE_DEFINE(name, #name, init, PM_DEVICE_GET(pm_##name), NULL, NULL, POST_KERNEL,                                  \
                CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL)

TEST_DEVICE(active, init_ok);       /* PM, no runtime PM, never suspended: stays active */
TEST_DEVICE(runtime_idle, init_ok); /* runtime PM, nobody uses it */
TEST_DEVICE(runtime_held, init_ok); /* runtime PM, a user holds a reference */
TEST_DEVICE(suspended, init_ok);    /* suspended by hand */
TEST_DEVICE(off, init_ok);          /* switched off */
TEST_DEVICE(not_ready, init_fails); /* its init failed */
DEVICE_DEFINE(no_pm, "no_pm", init_ok, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);

#define DEV(name) (&DEVICE_NAME_GET(name))

struct seen {
  const struct device *dev;
  struct sensei_pwr_dev_info info;
};

static struct seen seen[16];
static int seen_n;

static void collect(const struct sensei_pwr_dev_info *info, void *user) {
  ARG_UNUSED(user);
  if (seen_n < ARRAY_SIZE(seen)) {
    seen[seen_n].dev = info->dev;
    seen[seen_n].info = *info;
  }
  seen_n++;
}

static const struct sensei_pwr_dev_info *find(const struct device *dev) {
  for (int i = 0; i < seen_n && i < ARRAY_SIZE(seen); i++) {
    if (seen[i].dev == dev) {
      return &seen[i].info;
    }
  }
  return NULL;
}

static void *setup(void) {
  zassert_ok(pm_device_action_run(DEV(suspended), PM_DEVICE_ACTION_SUSPEND));
  zassert_ok(pm_device_action_run(DEV(off), PM_DEVICE_ACTION_SUSPEND));
  zassert_ok(pm_device_action_run(DEV(off), PM_DEVICE_ACTION_TURN_OFF));
  zassert_ok(pm_device_runtime_enable(DEV(runtime_idle)));
  zassert_ok(pm_device_runtime_enable(DEV(runtime_held)));
  zassert_ok(pm_device_runtime_get(DEV(runtime_held)));
  return NULL;
}

ZTEST_SUITE(pwr_check, NULL, setup, NULL, NULL, NULL);

ZTEST(pwr_check, test_01_reports_only_what_is_not_suspended) {
  const struct sensei_pwr_dev_info *i;
  int n;

  seen_n = 0;
  n = sensei_pwr_check_idle(collect, NULL);
  zassert_equal(n, seen_n, "the return value counts the callbacks");
  zassert_equal(n, 2, "only the active device and the one with a reference");

  i = find(DEV(active));
  zassert_not_null(i, "an active device is reported");
  zassert_equal(i->state, PM_DEVICE_STATE_ACTIVE);
  zassert_equal(i->usage, 0);
  zassert_false(i->runtime, "no runtime PM: it will not suspend by itself");

  i = find(DEV(runtime_held));
  zassert_not_null(i, "a device that is in use is reported");
  zassert_equal(i->state, PM_DEVICE_STATE_ACTIVE);
  zassert_equal(i->usage, 1);
  zassert_true(i->runtime, "runtime PM: it suspends once the user lets go");

  zassert_is_null(find(DEV(runtime_idle)), "runtime PM, unused: suspended");
  zassert_is_null(find(DEV(suspended)));
  zassert_is_null(find(DEV(off)), "off is not a power draw");
  zassert_is_null(find(DEV(not_ready)), "a device that did not start is skipped");
  zassert_is_null(find(DEV(no_pm)), "no device PM: not looked at");
}

ZTEST(pwr_check, test_02_counting_without_a_callback) { zassert_equal(sensei_pwr_check_idle(NULL, NULL), 2); }

ZTEST(pwr_check, test_03_a_released_device_drops_out) {
  zassert_ok(pm_device_runtime_put(DEV(runtime_held)));
  seen_n = 0;
  zassert_equal(sensei_pwr_check_idle(collect, NULL), 1);
  zassert_is_null(find(DEV(runtime_held)));
  zassert_not_null(find(DEV(active)));

  /* and one that wakes up shows again */
  zassert_ok(pm_device_runtime_get(DEV(runtime_idle)));
  seen_n = 0;
  zassert_equal(sensei_pwr_check_idle(collect, NULL), 2);
  zassert_not_null(find(DEV(runtime_idle)));
  zassert_ok(pm_device_runtime_put(DEV(runtime_idle)));

  /* an active device that is suspended is not reported any more */
  zassert_ok(pm_device_action_run(DEV(active), PM_DEVICE_ACTION_SUSPEND));
  zassert_equal(sensei_pwr_check_idle(NULL, NULL), 0, "all quiet: the sleep floor");
}
