/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The on_power hook of the LPWAN client: it is called once per real change of the shield's 3.3 V rail. The shield is
 * a TCP port that nobody listens on (CONFIG_SENSEI_LPWAN_SIM_TARGET), so a bring-up powers the shield, gets no answer
 * within the boot timeout and powers it down again: on, then off. The first bring-up after a start waits 180 s (a
 * shield swap may be running), the next ones the boot timeout. A power command while the client waits to try again is
 * refused and calls nothing; after the wait the bring-up goes on once more. */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "lpwan_priv.h"
#include "sensei_lpwan/lpwan.h"

#define BOOT_MS CONFIG_SENSEI_LPWAN_BOOT_TIMEOUT_MS
#define RETRY_MS 15000      /* SHIELD_RETRY_MS of the client */
#define SWAP_WAIT_MS 180000 /* LPWAN_UPD_SWAP_TIMEOUT_MS: the first bring-up after a start (a swap may be running) */

static struct {
  int on, off;
  int64_t t_on, t_off;
  int order; /* 1: the last call was on, 2: off */
  int bad_order;
  bool in_wq;
} ev;

static void on_power(bool on) {
  if (on) {
    ev.on++;
    ev.t_on = k_uptime_get();
    ev.bad_order += ev.order == 1;
    ev.order = 1;
  } else {
    ev.off++;
    ev.t_off = k_uptime_get();
    ev.bad_order += ev.order != 1;
    ev.order = 2;
  }
}

/* Wait until the counter reaches n (the bring-up needs the boot timeout and the requests that fail). */
static bool wait_count(const int *counter, int n, int timeout_ms) {
  for (int t = 0; t < timeout_ms && *counter < n; t += 20) {
    k_msleep(20);
  }
  return *counter >= n;
}

static int up_calls;
static void on_up(sensei_Radio r) {
  ARG_UNUSED(r);
  up_calls++;
}

ZTEST(lpwan_power, test_hook_once_per_change) {
  struct sensei_lpwan_hooks hooks = {.on_up = on_up, .on_power = on_power};
  struct sensei_lpwan_config cfg = {0};

  zassert_ok(sensei_lpwan_init(&hooks, &cfg));
  k_msleep(200);
  zassert_equal(ev.on + ev.off, 0, "the shield starts off, no call");

  /* first bring-up: rail on at once, off after the boot timeout */
  zassert_ok(sensei_lpwan_cmd_power(true));
  k_msleep(150);
  zassert_equal(ev.on, 1, "on");
  zassert_equal(ev.off, 0, "still waiting for the shield");
  int64_t t_on = ev.t_on;

  zassert_true(wait_count(&ev.off, 1, SWAP_WAIT_MS + 20000), "no answer: power cut");
  zassert_equal(ev.on, 1);
  zassert_true(ev.t_off - t_on >= SWAP_WAIT_MS, "after the long first wait, not before (%d ms)",
               (int)(ev.t_off - t_on));
  zassert_equal(ev.bad_order, 0);
  zassert_equal(up_calls, 0);

  /* off when it is off: nothing */
  zassert_ok(sensei_lpwan_cmd_power(false));
  k_msleep(200);
  zassert_equal(ev.off, 1, "an off of a shield that is off is not a change");

  /* inside the retry wait of a failed bring-up: refused, no hook */
  zassert_ok(sensei_lpwan_cmd_power(true));
  k_msleep(300);
  zassert_equal(ev.on, 1, "the rail stays off while the client waits");

  /* after it: one more on and one more off */
  k_msleep(RETRY_MS);
  zassert_ok(sensei_lpwan_cmd_power(true));
  k_msleep(150);
  zassert_equal(ev.on, 2);
  /* power(true) again while the bring-up runs or the rail is on: no second on */
  zassert_ok(sensei_lpwan_cmd_power(true));
  k_msleep(100);
  zassert_equal(ev.on, 2);
  zassert_true(wait_count(&ev.off, 2, 20000));
  zassert_equal(ev.on, 2);
  zassert_equal(ev.off, 2);
  zassert_true(ev.t_off - ev.t_on >= BOOT_MS && ev.t_off - ev.t_on < SWAP_WAIT_MS, "the boot timeout (%d ms)",
               (int)(ev.t_off - ev.t_on));
  zassert_equal(ev.bad_order, 0);
}

ZTEST_SUITE(lpwan_power, NULL, NULL, NULL, NULL, NULL);
