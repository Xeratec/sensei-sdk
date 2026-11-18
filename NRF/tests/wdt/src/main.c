/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* NRF/system/wdt (sensei_wdt.c) against a test watchdog whose calls are FFF fakes: what is installed, that setup
 * and the feed only follow a good installation, and that the feed stops while the application is not healthy. Time
 * is the simulated clock of native_sim.
 *
 * sensei_wdt_start() keeps a delayable work item in static memory and initialises it on every call. A second
 * start while the first one still runs would corrupt the timer list, so each test gets its own work queue and
 * stops it afterwards (the pending timer then finds a stopped queue and the item is idle again). The last test
 * uses the system work queue (wq = NULL) and leaves it running. */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/fff.h>
#include <zephyr/ztest.h>

#include "wdt/sensei_wdt.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, fake_install, const struct device *, const struct wdt_timeout_cfg *);
FAKE_VALUE_FUNC(int, fake_setup, const struct device *, uint8_t);
FAKE_VALUE_FUNC(int, fake_feed, const struct device *, int);

#define FEED_MS CONFIG_SENSEI_WDT_FEED_PERIOD_MS

BUILD_ASSERT(CONFIG_SENSEI_WDT_TIMEOUT_S * 1000 > CONFIG_SENSEI_WDT_FEED_PERIOD_MS,
             "the default timeout must be longer than the feed period");

/* ---- test watchdog ---- */

static struct wdt_timeout_cfg installed; /* copy of the last configuration (the caller's one is on its stack) */
static int64_t feed_at[16];
static int feeds_seen;

static int install_copy(const struct device *dev, const struct wdt_timeout_cfg *cfg) {
  ARG_UNUSED(dev);
  installed = *cfg;
  return 3; /* the channel */
}

static int feed_log(const struct device *dev, int channel) {
  ARG_UNUSED(dev);
  ARG_UNUSED(channel);
  if (feeds_seen < ARRAY_SIZE(feed_at)) {
    feed_at[feeds_seen] = k_uptime_get();
  }
  feeds_seen++;
  return 0;
}

static int fake_disable(const struct device *dev) {
  ARG_UNUSED(dev);
  return 0;
}

static DEVICE_API(wdt, wdt_api) = {
    .setup = fake_setup,
    .disable = fake_disable,
    .install_timeout = fake_install,
    .feed = fake_feed,
};

static int wdt_init(const struct device *dev) {
  ARG_UNUSED(dev);
  return 0;
}

DEVICE_DT_DEFINE(DT_NODELABEL(test_wdt), wdt_init, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
                 &wdt_api);

static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(test_wdt));

/* ---- test fixture ---- */

static K_THREAD_STACK_DEFINE(wq_stack, 2048);
static struct k_work_q wq;
static int health_calls;
static bool health;

static bool healthy_cb(void) {
  health_calls++;
  return health;
}

static void start_wq(void) {
  k_work_queue_init(&wq);
  k_work_queue_start(&wq, wq_stack, K_THREAD_STACK_SIZEOF(wq_stack), K_PRIO_PREEMPT(5), NULL);
}

static void before(void *f) {
  ARG_UNUSED(f);
  RESET_FAKE(fake_install);
  RESET_FAKE(fake_setup);
  RESET_FAKE(fake_feed);
  FFF_RESET_HISTORY();
  fake_install_fake.custom_fake = install_copy;
  fake_feed_fake.custom_fake = feed_log;
  memset(&installed, 0, sizeof(installed));
  feeds_seen = 0;
  health_calls = 0;
  health = true;
  start_wq();
}

static void after(void *f) {
  ARG_UNUSED(f);
  /* Stop the queue, then wait for the pending timer: it finds the stopped queue and the work item is idle. */
  if (k_work_queue_drain(&wq, true) == 0) {
    (void)k_work_queue_stop(&wq, K_SECONDS(1));
  }
  k_sleep(K_MSEC(FEED_MS + 100));
}

ZTEST_SUITE(wdt, NULL, NULL, before, after, NULL);

/* ---- start-up failures: nothing is fed afterwards ---- */

ZTEST(wdt, test_01_device_not_ready) {
  struct device_state *st = wdt->state;

  st->initialized = false;
  zassert_equal(sensei_wdt_start(10, healthy_cb, &wq), -ENODEV);
  st->initialized = true;
  zassert_equal(fake_install_fake.call_count, 0, "nothing is installed on a device that is not ready");
  zassert_equal(fake_setup_fake.call_count, 0);
  k_sleep(K_MSEC(FEED_MS * 2));
  zassert_equal(fake_feed_fake.call_count, 0);
  zassert_equal(health_calls, 0);
}

ZTEST(wdt, test_02_install_fails) {
  fake_install_fake.custom_fake = NULL;
  fake_install_fake.return_val = -EINVAL;
  zassert_equal(sensei_wdt_start(10, healthy_cb, &wq), -EIO);
  zassert_equal(fake_setup_fake.call_count, 0, "no setup after a failed install");
  k_sleep(K_MSEC(FEED_MS * 2));
  zassert_equal(fake_feed_fake.call_count, 0);
}

ZTEST(wdt, test_03_setup_fails) {
  fake_setup_fake.return_val = -EBUSY;
  zassert_equal(sensei_wdt_start(10, healthy_cb, &wq), -EIO);
  zassert_equal(fake_install_fake.call_count, 1);
  zassert_equal(fake_setup_fake.call_count, 1);
  k_sleep(K_MSEC(FEED_MS * 2));
  zassert_equal(fake_feed_fake.call_count, 0, "a watchdog that did not start is not fed");
}

/* ---- running ---- */

ZTEST(wdt, test_04_timeout_and_options) {
  zassert_ok(sensei_wdt_start(42, healthy_cb, &wq));
  zassert_equal(fake_install_fake.call_count, 1);
  zassert_equal(fake_setup_fake.call_count, 1);
  zassert_equal(installed.window.min, 0);
  zassert_equal(installed.window.max, 42000, "timeout in ms");
  zassert_equal(installed.flags, WDT_FLAG_RESET_SOC, "the SoC resets when the feed stops");
  zassert_equal(fake_setup_fake.arg1_val, WDT_OPT_PAUSE_HALTED_BY_DBG);
  zassert_equal(fake_setup_fake.arg0_val, wdt);
  /* install, then setup, then feed */
  k_sleep(K_MSEC(10));
  zassert_equal(fake_feed_fake.call_count, 1);
  zassert_true(fff.call_history[0] == (void *)fake_install);
  zassert_true(fff.call_history[1] == (void *)fake_setup);
  zassert_true(fff.call_history[2] == (void *)fake_feed);
}

ZTEST(wdt, test_05_feed_period_and_channel) {
  zassert_ok(sensei_wdt_start(CONFIG_SENSEI_WDT_TIMEOUT_S, healthy_cb, &wq));
  k_sleep(K_MSEC(100));
  zassert_equal(fake_feed_fake.call_count, 1, "the first feed comes at once");
  zassert_equal(fake_feed_fake.arg1_val, 3, "the channel that install returned");
  k_sleep(K_MSEC(FEED_MS - 200));
  zassert_equal(fake_feed_fake.call_count, 1);
  k_sleep(K_MSEC(300));
  zassert_equal(fake_feed_fake.call_count, 2);
  k_sleep(K_MSEC(FEED_MS));
  zassert_equal(fake_feed_fake.call_count, 3);
  /* every gap is one period, far below the timeout */
  for (int i = 1; i < 3; i++) {
    int64_t gap = feed_at[i] - feed_at[i - 1];

    zassert_true(gap >= FEED_MS && gap < FEED_MS + 50, "gap %lld ms", gap);
    zassert_true(gap < CONFIG_SENSEI_WDT_TIMEOUT_S * 1000);
  }
}

ZTEST(wdt, test_06_not_healthy_is_not_fed) {
  health = false;
  zassert_ok(sensei_wdt_start(CONFIG_SENSEI_WDT_TIMEOUT_S, healthy_cb, &wq));
  k_sleep(K_MSEC(FEED_MS * 2 + 100));
  zassert_equal(health_calls, 3, "the check runs every period");
  zassert_equal(fake_feed_fake.call_count, 0, "no feed while the application is not healthy");

  /* it recovers: the next period feeds again, with no restart */
  health = true;
  k_sleep(K_MSEC(FEED_MS));
  zassert_equal(fake_feed_fake.call_count, 1);
  /* and stops again */
  health = false;
  k_sleep(K_MSEC(FEED_MS * 2));
  zassert_equal(fake_feed_fake.call_count, 1);
}

ZTEST(wdt, test_07_feed_error_does_not_stop_the_loop) {
  fake_feed_fake.custom_fake = NULL;
  fake_feed_fake.return_val = -EAGAIN;
  zassert_ok(sensei_wdt_start(CONFIG_SENSEI_WDT_TIMEOUT_S, healthy_cb, &wq));
  k_sleep(K_MSEC(FEED_MS * 2 + 100));
  zassert_equal(fake_feed_fake.call_count, 3, "it tries again every period");
}

/* The system work queue and no check: always fed. Last test, it cannot be stopped. */
ZTEST(wdt, test_99_system_work_queue_always_feeds) {
  zassert_ok(sensei_wdt_start(CONFIG_SENSEI_WDT_TIMEOUT_S, NULL, NULL));
  k_sleep(K_MSEC(FEED_MS * 2 + 100));
  zassert_equal(fake_feed_fake.call_count, 3);
  zassert_equal(health_calls, 0);
}
