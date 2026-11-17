/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The SMP reset hook of NRF/system/ota (sensei_ota_reset.c) against the mcumgr callback chain: a reset into the MCUboot
 * recovery (boot mode 1) that arrives without USB power, as it does over Bluetooth, is refused; a normal reset and a
 * reset with USB power go through. mgmt_callback_notify() is what the os_mgmt reset command calls. */

#include <zcbor_common.h>

#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/ztest.h>

#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>

static bool usb;

/* The test's USB power (the default of the module reads the nRF5340 and says no on native_sim). */
bool sensei_ota_usb_power(void) { return usb; }

static enum mgmt_cb_return reset(uint8_t boot_mode, int32_t *rc) {
  struct os_mgmt_reset_data d = {.force = false, .boot_mode = boot_mode};
  uint16_t group = 0;

  *rc = 0;
  return mgmt_callback_notify(MGMT_EVT_OP_OS_MGMT_RESET, &d, sizeof(d), rc, &group);
}

ZTEST(ota_reset, test_boot_mode_over_ble_refused) {
  int32_t rc;

  usb = false;
  zassert_equal(reset(1, &rc), MGMT_CB_ERROR_RC);
  zassert_equal(rc, MGMT_ERR_EACCESSDENIED);
  zassert_equal(reset(2, &rc), MGMT_CB_ERROR_RC);
  zassert_equal(reset(255, &rc), MGMT_CB_ERROR_RC);
}

ZTEST(ota_reset, test_normal_reset_always_goes_through) {
  int32_t rc;

  usb = false;
  zassert_equal(reset(0, &rc), MGMT_CB_OK);
  usb = true;
  zassert_equal(reset(0, &rc), MGMT_CB_OK);
}

ZTEST(ota_reset, test_boot_mode_with_usb_power) {
  int32_t rc;

  usb = true;
  zassert_equal(reset(1, &rc), MGMT_CB_OK);
  usb = false;
  zassert_equal(reset(1, &rc), MGMT_CB_ERROR_RC); /* unplugged again */
}

ZTEST_SUITE(ota_reset, NULL, NULL, NULL, NULL, NULL);
