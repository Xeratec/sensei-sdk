/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* NRF/system/diag on native_sim with a retained memory test driver: boot counter, reset cause, the fault record
 * written by sensei_diag_capture() and taken after the next boot, damaged and foreign RAM contents. */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/ztest.h>

#include "diag/sensei_diag_rt.h"
#include "sensei_diag.h"

#if DT_NODE_HAS_COMPAT(DT_CHOSEN(sensei_diag_fault), zephyr_retention)
#include <zephyr/retention/retention.h>
#endif

static const struct device *const fault_dev = DEVICE_DT_GET(DT_CHOSEN(sensei_diag_fault));
static const struct device *const state_dev = DEVICE_DT_GET(DT_CHOSEN(sensei_diag_state));

static void *setup(void) {
  /* a clean start: the RAM was lost */
  retention_clear(fault_dev);
  retention_clear(state_dev);
  return NULL;
}

ZTEST_SUITE(diag, NULL, setup, NULL, NULL, NULL);

ZTEST(diag, test_01_boot_counter_and_cause) {
  struct sensei_diag_fault f;

  /* the first boot after power-up: the retained RAM was lost */
  zassert_ok(sensei_diag_boot());
  zassert_equal(sensei_diag_boot_count(), 1);
  zassert_true(sensei_diag_retained());
  /* native_sim reports no reset flag: POR, no fault */
  zassert_equal(sensei_diag_reset_cause(), SENSEI_RESET_POR);
  zassert_false(sensei_diag_take_fault(&f));

  /* a warm reset: the count goes on */
  zassert_ok(sensei_diag_boot());
  zassert_equal(sensei_diag_boot_count(), 2);
  zassert_ok(sensei_diag_boot());
  zassert_equal(sensei_diag_boot_count(), 3);
}

ZTEST(diag, test_02_fault_record_once) {
  struct sensei_diag_fault f;
  uint32_t boots;

  zassert_ok(sensei_diag_boot());
  boots = sensei_diag_boot_count();

  sensei_diag_set_job(7);
  sensei_diag_set_image_id(0x1234ABCD);
  sensei_diag_capture(K_ERR_KERNEL_OOPS, NULL);

  /* "reset": the next boot finds the record */
  zassert_ok(sensei_diag_boot());
  zassert_equal(sensei_diag_boot_count(), boots + 1);
  zassert_true((sensei_diag_reset_cause() & SENSEI_RESET_FAULT) != 0);
  zassert_true(sensei_diag_peek_fault(&f));
  zassert_true(sensei_diag_take_fault(&f));
  zassert_equal(f.reason, SENSEI_FAULT_KERNEL_OOPS);
  zassert_equal(f.last_job, 7);
  zassert_equal(f.image_id, 0x1234ABCD);
  zassert_equal(f.boot_count, boots);
  zassert_true(strlen(f.thread) > 0);
  zassert_equal(f.pc, 0); /* no exception frame */
  /* once */
  zassert_false(sensei_diag_take_fault(&f));
  zassert_false(sensei_diag_peek_fault(&f));

  /* and not again after another reset */
  zassert_ok(sensei_diag_boot());
  zassert_equal(sensei_diag_reset_cause() & SENSEI_RESET_FAULT, 0);
  zassert_false(sensei_diag_take_fault(&f));
}

ZTEST(diag, test_03_peek_does_not_clear) {
  struct sensei_diag_fault a, b;

  sensei_diag_capture(K_ERR_STACK_CHK_FAIL, NULL);
  zassert_ok(sensei_diag_boot());
  zassert_true(sensei_diag_peek_fault(&a));
  zassert_true(sensei_diag_peek_fault(&b));
  zassert_equal(a.reason, SENSEI_FAULT_STACK_CHK_FAIL);
  zassert_mem_equal(&a, &b, sizeof(a));
  zassert_true(sensei_diag_take_fault(&a));
}

ZTEST(diag, test_04_damaged_record_is_dropped) {
  struct sensei_diag_fault f;
  uint8_t raw[SENSEI_DIAG_FAULT_BYTES];

  sensei_diag_capture(K_ERR_CPU_EXCEPTION, NULL);
  zassert_ok(retention_read(fault_dev, 0, raw, sizeof(raw)));
  raw[10] ^= 0x01; /* one flipped bit */
  zassert_ok(retention_write(fault_dev, 0, raw, sizeof(raw)));
  zassert_ok(sensei_diag_boot());
  zassert_false(sensei_diag_take_fault(&f));
  zassert_equal(sensei_diag_reset_cause() & SENSEI_RESET_FAULT, 0);
  /* and it is gone from the RAM */
  zassert_ok(retention_read(fault_dev, 0, raw, sizeof(raw)));
  zassert_equal(raw[0], 0);
}

ZTEST(diag, test_05_foreign_ram_is_ignored) {
  struct sensei_diag_fault f;
  uint8_t junk[SENSEI_DIAG_FAULT_BYTES];

  /* random contents after a power-up */
  memset(junk, 0xA5, sizeof(junk));
  zassert_ok(retention_write(fault_dev, 0, junk, sizeof(junk)));
  zassert_ok(retention_write(state_dev, 0, junk, SENSEI_DIAG_STATE_BYTES));
  zassert_ok(sensei_diag_boot());
  zassert_equal(sensei_diag_boot_count(), 1);
  zassert_false(sensei_diag_take_fault(&f));
  zassert_equal(sensei_diag_reset_cause(), SENSEI_RESET_POR);
}

ZTEST(diag, test_06_job_and_image_are_taken_at_capture) {
  struct sensei_diag_fault f;

  sensei_diag_set_job(1);
  sensei_diag_set_job(3);
  sensei_diag_set_image_id(0xAAAA5555);
  sensei_diag_capture(K_ERR_KERNEL_PANIC, NULL);
  sensei_diag_set_job(9); /* later changes do not matter */
  zassert_ok(sensei_diag_boot());
  zassert_true(sensei_diag_take_fault(&f));
  zassert_equal(f.last_job, 3);
  zassert_equal(f.image_id, 0xAAAA5555);
  zassert_equal(f.reason, SENSEI_FAULT_KERNEL_PANIC);
}
