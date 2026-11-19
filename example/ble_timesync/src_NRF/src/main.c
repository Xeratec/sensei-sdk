/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

BUILD_ASSERT(IS_ENABLED(CONFIG_SENSEI_TIMESYNC_MASTER) != IS_ENABLED(CONFIG_SENSEI_TIMESYNC_FOLLOWER),
             "Select exactly one role: master.conf or follower.conf");

const struct gpio_dt_spec app_pin_pps = GPIO_DT_SPEC_GET(DT_ALIAS(ts_pps), gpios);
const struct gpio_dt_spec app_pin_capture = GPIO_DT_SPEC_GET(DT_ALIAS(ts_capture), gpios);
const struct gpio_dt_spec app_pin_test_out = GPIO_DT_SPEC_GET(DT_ALIAS(ts_test_out), gpios);

#define PPS_PERIOD_NS 500000000ULL

static struct k_work_delayable status_work;

static void ppm_str(char *buf, size_t len, double ppm) {
  long milli = lround(ppm * 1000.0);

  snprintf(buf, len, "%s%ld.%03ld", milli < 0 ? "-" : "", labs(milli) / 1000, labs(milli) % 1000);
}

static void on_state(enum sensei_ts_state state) { LOG_INF("Time sync state: %s", app_state_str(state)); }

static void status_fn(struct k_work *work) {
  struct sensei_ts_status st;
  struct sensei_ts_time now;
  char ppm[16];

  sensei_ts_status_get(&st);
  ppm_str(ppm, sizeof(ppm), st.rate_ppm);
  if (sensei_ts_now(&now) != 0) {
    now.uncert_ns = 0;
  }

  LOG_INF("Status: %s, points %u, rate %s ppm, resid %u ns, uncert %u ns", app_state_str(st.state), st.points, ppm,
          st.resid_ns, now.uncert_ns);
  LOG_INF("        samples %u, points added %u, outliers %u, resets %u", st.samples_rx, st.points_added, st.outliers,
          st.resets);

  k_work_reschedule(&status_work, K_SECONDS(CONFIG_APP_STATUS_PERIOD_S));
}

int main(void) {
  int err;

  LOG_INF("BLE time sync example, role %s", IS_ENABLED(CONFIG_SENSEI_TIMESYNC_MASTER) ? "master" : "follower");

  err = bt_enable(NULL);
  if (err) {
    LOG_ERR("bt_enable failed (%d)", err);
    return err;
  }

  err = sensei_ts_ble_init();
  if (err) {
    LOG_ERR("sensei_ts_ble_init failed (%d): check sysbuild/ipc_radio.conf", err);
    return err;
  }
  sensei_ts_state_cb_set(on_state);

  /* The PPS pin toggles at multiples of 500 ms in global time (or, with APP_PPS_PULSE_WIDTH_US, pulses high for that
   * long at multiples of 500 ms), whenever there is a global time. */
  if (CONFIG_APP_PPS_PULSE_WIDTH_US > 0) {
    err = sensei_ts_pps_start_pulse(&app_pin_pps, PPS_PERIOD_NS, (uint64_t)CONFIG_APP_PPS_PULSE_WIDTH_US * 1000ULL);
  } else {
    err = sensei_ts_pps_start(&app_pin_pps, PPS_PERIOD_NS);
  }
  if (err) {
    LOG_ERR("PPS start failed (%d)", err);
  }

  k_work_init_delayable(&status_work, status_fn);
  k_work_schedule(&status_work, K_SECONDS(CONFIG_APP_STATUS_PERIOD_S));

  app_role_run();
  return 0;
}
