/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>

#include "wdt/sensei_wdt.h"

LOG_MODULE_REGISTER(sensei_wdt, CONFIG_SENSEI_WDT_LOG_LEVEL);

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int channel;
static bool (*healthy_cb)(void);
static struct k_work_q *feed_wq;
static struct k_work_delayable feed_work;

static void feed_handler(struct k_work *w) {
  ARG_UNUSED(w);
  if (healthy_cb == NULL || healthy_cb()) {
    wdt_feed(wdt, channel);
  } else {
    LOG_ERR("application not healthy, not feeding the watchdog");
  }
  if (feed_wq != NULL) {
    k_work_reschedule_for_queue(feed_wq, &feed_work, K_MSEC(CONFIG_SENSEI_WDT_FEED_PERIOD_MS));
  } else {
    k_work_reschedule(&feed_work, K_MSEC(CONFIG_SENSEI_WDT_FEED_PERIOD_MS));
  }
}

int sensei_wdt_start(uint32_t timeout_s, bool (*healthy)(void), struct k_work_q *wq) {
  struct wdt_timeout_cfg cfg = {
      .flags = WDT_FLAG_RESET_SOC,
      .window.min = 0,
      .window.max = timeout_s * 1000,
  };

  if (!device_is_ready(wdt)) {
    LOG_ERR("watchdog not ready");
    return -ENODEV;
  }
  channel = wdt_install_timeout(wdt, &cfg);
  if (channel < 0 || wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) < 0) {
    LOG_ERR("watchdog setup failed");
    return -EIO;
  }
  healthy_cb = healthy;
  feed_wq = wq;
  k_work_init_delayable(&feed_work, feed_handler);
  if (wq != NULL) {
    k_work_reschedule_for_queue(wq, &feed_work, K_NO_WAIT);
  } else {
    k_work_reschedule(&feed_work, K_NO_WAIT);
  }
  return 0;
}
