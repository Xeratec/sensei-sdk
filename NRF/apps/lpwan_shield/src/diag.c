/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <string.h>

#include <zephyr/logging/log.h>

#include "diag.h"
#include "diag/sensei_diag_rt.h"

LOG_MODULE_DECLARE(main, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

static struct sensei_diag_fault fault;
static volatile bool have_fault; /* until the node acks it (diag_ack): a lost status answer loses nothing */

void diag_init(void) {
  have_fault = sensei_diag_take_fault(&fault);
  if (have_fault) {
    LOG_WRN("last run ended with a fatal error: %s, pc 0x%08x, thread %s", sensei_diag_fault_reason_name(fault.reason),
            (unsigned)fault.pc, fault.thread);
  }
}

void diag_modem_fault(uint32_t reason, uint32_t pc) {
  sensei_diag_capture_ext(SENSEI_FAULT_MODEM, pc, 0, reason, "modem");
}

void diag_ack(void) {
  if (have_fault) {
    LOG_INF("fault record taken over by the node");
  }
  have_fault = false;
}

void diag_fill_status(sensei_LpwanStatus *st) {
  st->reset_cause = sensei_diag_reset_cause();
  st->boot_count = sensei_diag_boot_count();
  if (have_fault) {
    st->has_fault = true; /* every status, until diag_ack() */
    st->fault.reason = fault.reason;
    st->fault.pc = fault.pc;
    st->fault.lr = fault.lr;
    strncpy(st->fault.thread, fault.thread, sizeof(st->fault.thread) - 1);
    st->fault.uptime_s = fault.uptime_s;
    st->fault.last_job = fault.last_job;
    st->fault.image_id = fault.image_id;
    st->fault.boot_count = fault.boot_count;
    st->fault.status = fault.status;
    st->fault.coredump = (fault.flags & SENSEI_FAULT_FLAG_COREDUMP) != 0;
  }
}
