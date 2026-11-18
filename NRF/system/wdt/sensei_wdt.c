/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <errno.h>

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
static atomic_t feed_errors;

uint32_t sensei_wdt_feed_errors(void) { return (uint32_t)atomic_get(&feed_errors); }

static void feed_handler(struct k_work *w) {
  ARG_UNUSED(w);
  if (healthy_cb == NULL || healthy_cb()) {
    int err = wdt_feed(wdt, channel);

    if (err < 0) {
      /* Keep the loop going: the next try may work, and if not the watchdog resets the SoC, as it should. */
      atomic_inc(&feed_errors);
      LOG_ERR("watchdog feed failed (%d)", err);
    }
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

  /* The timeout must outlast the feed period, or the watchdog would reset a healthy node. The product is checked
   * for overflow of the millisecond value. */
  if (timeout_s == 0 || timeout_s > UINT32_MAX / 1000 || timeout_s * 1000 <= CONFIG_SENSEI_WDT_FEED_PERIOD_MS) {
    LOG_ERR("timeout %u s must be longer than the feed period of %d ms", (unsigned)timeout_s,
            CONFIG_SENSEI_WDT_FEED_PERIOD_MS);
    return -EINVAL;
  }
  /* A running feed loop owns the work item: initialising it again would corrupt the timer list. */
  if (k_work_delayable_busy_get(&feed_work) != 0) {
    LOG_ERR("watchdog already started");
    return -EALREADY;
  }
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
