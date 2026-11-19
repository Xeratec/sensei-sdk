/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The LPWAN client on a node without a shield. On the board the expander (I2C 0x20) does not answer; in the simulation
 * the target of the shield is empty. Both are the same case for the client: sensei_lpwan_init() fails with -ENODEV, the
 * state is ABSENT, the thread does not start, the shield is never powered and no bring-up is retried. The API is safe
 * to call anyway (an app may call it before it looks at the result of init). */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "lpwan_priv.h"
#include "sensei_lpwan/lpwan.h"

static int up_calls, power_calls, done_calls;

static void on_up(sensei_Radio r) {
  ARG_UNUSED(r);
  up_calls++;
}

static void on_power(bool on) {
  ARG_UNUSED(on);
  power_calls++;
}

static void send_done(int err, void *user) {
  ARG_UNUSED(err);
  ARG_UNUSED(user);
  done_calls++;
}

static void gnss_done(int err, const sensei_GnssFixResult *res, void *user) {
  ARG_UNUSED(err);
  ARG_UNUSED(res);
  ARG_UNUSED(user);
  done_calls++;
}

ZTEST(lpwan_absent, test_no_shield_means_absent_and_inert) {
  struct sensei_lpwan_hooks hooks = {.on_up = on_up, .on_power = on_power};
  struct sensei_lpwan_config cfg = {0};
  static const uint8_t data[4] = {1, 2, 3, 4};
  sensei_LpwanVersion v;
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LoraProvision keys = sensei_LoraProvision_init_zero;
  struct lpwan_info info;

  zassert_equal(sensei_lpwan_shield_state(), LPWAN_SHIELD_UNKNOWN, "not decided before init");
  zassert_equal(sensei_lpwan_init(&hooks, &cfg), -ENODEV);
  zassert_equal(sensei_lpwan_shield_state(), LPWAN_SHIELD_ABSENT);
  zassert_equal(strcmp(sensei_lpwan_state_str(), "no shield"), 0);

  /* nothing is ready, nothing is accepted, nothing wakes the shield */
  for (sensei_Radio r = sensei_Radio_RADIO_LORA; r <= sensei_Radio_RADIO_NBIOT; r++) {
    zassert_false(sensei_lpwan_radio_ready(r));
    zassert_equal(sensei_lpwan_radio_send(r, data, sizeof(data), send_done, NULL), -ENODEV);
  }
  zassert_equal(sensei_lpwan_gnss_fix(30, gnss_done, NULL), -ENODEV);
  zassert_false(sensei_lpwan_gnss_busy());
  zassert_equal(sensei_lpwan_gnss_supported(), 0, "no GNSS of the shield");
  zassert_equal(sensei_lpwan_query_version(&v, 100), -ENODEV);
  rq.which_body = sensei_LpwanRequest_dfu_begin_tag;
  zassert_equal(sensei_lpwan_dfu_submit(&rq, NULL, NULL), -ENODEV);
  zassert_equal(sensei_lpwan_cmd_power(true), -ENODEV);
  zassert_equal(sensei_lpwan_cmd_connect(sensei_Radio_RADIO_LTEM), -ENODEV);
  zassert_equal(sensei_lpwan_lte_link_check(), SENSEI_LPWAN_LINK_OK);
  /* LoRa keys are not kept in RAM with nothing to take them (the shell and the app call these) */
  zassert_equal(sensei_lpwan_lora_keys_queue(&keys), -ENODEV);
  zassert_equal(sensei_lpwan_lora_provision(&keys), -ENODEV);
  sensei_lpwan_get_info(&info);
  zassert_false(info.provisioned_pending, "no keys waiting");
  zassert_equal(sensei_lpwan_shield_version(&v), -ENODATA);
  sensei_lpwan_set_config(&cfg); /* harmless */

  /* a minute (simulated): no bring-up, no power change, no callback; the old retry would try every 15 s */
  k_msleep(60000);
  zassert_equal(power_calls, 0);
  zassert_equal(up_calls, 0);
  zassert_equal(done_calls, 0);
  zassert_equal(sensei_lpwan_version_reads(), 0);
  zassert_equal(sensei_lpwan_shield_state(), LPWAN_SHIELD_ABSENT);
}

ZTEST_SUITE(lpwan_absent, NULL, NULL, NULL, NULL, NULL);
