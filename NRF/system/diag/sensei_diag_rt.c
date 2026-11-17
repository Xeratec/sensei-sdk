/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

#include "diag/sensei_diag_rt.h"

#if DT_HAS_CHOSEN(sensei_diag_fault) && DT_HAS_CHOSEN(sensei_diag_state)
#include <zephyr/retention/retention.h>
#define DIAG_RETENTION 1
#endif

LOG_MODULE_REGISTER(sensei_diag, CONFIG_SENSEI_DIAG_LOG_LEVEL);

extern void sys_arch_reboot(int type);

/* The reset cause bits are the hwinfo flags (sensei_diag.h). */
BUILD_ASSERT(SENSEI_RESET_PIN == RESET_PIN && SENSEI_RESET_SOFTWARE == RESET_SOFTWARE &&
                 SENSEI_RESET_BROWNOUT == RESET_BROWNOUT && SENSEI_RESET_POR == RESET_POR &&
                 SENSEI_RESET_WATCHDOG == RESET_WATCHDOG && SENSEI_RESET_DEBUG == RESET_DEBUG &&
                 SENSEI_RESET_SECURITY == RESET_SECURITY && SENSEI_RESET_LOW_POWER_WAKE == RESET_LOW_POWER_WAKE &&
                 SENSEI_RESET_CPU_LOCKUP == RESET_CPU_LOCKUP && SENSEI_RESET_PARITY == RESET_PARITY &&
                 SENSEI_RESET_PLL == RESET_PLL && SENSEI_RESET_CLOCK == RESET_CLOCK &&
                 SENSEI_RESET_HARDWARE == RESET_HARDWARE && SENSEI_RESET_USER == RESET_USER &&
                 SENSEI_RESET_TEMPERATURE == RESET_TEMPERATURE && SENSEI_RESET_BOOTLOADER == RESET_BOOTLOADER &&
                 SENSEI_RESET_FLASH == RESET_FLASH,
             "SENSEI_RESET_* differ from the hwinfo RESET_* flags");

#ifdef DIAG_RETENTION
/* The fatal handler writes the record from any context, also from an interrupt or while a lock is held: the retention
 * and the retained memory drivers must not take a mutex. SENSEI_DIAG only implies the options, a configuration that
 * switches them off again must not build. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_RETENTION_MUTEXES) && !IS_ENABLED(CONFIG_RETAINED_MEM_MUTEXES),
             "the fatal handler writes the fault record from interrupts: set RETENTION_MUTEX_FORCE_DISABLE=y and "
             "RETAINED_MEM_MUTEX_FORCE_DISABLE=y");

static const struct device *const fault_dev = DEVICE_DT_GET(DT_CHOSEN(sensei_diag_fault));
static const struct device *const state_dev = DEVICE_DT_GET(DT_CHOSEN(sensei_diag_state));

static int store_read(const struct device *dev, uint8_t *buf, size_t len) { return retention_read(dev, 0, buf, len); }

static int store_write(const struct device *dev, const uint8_t *buf, size_t len) {
  return retention_write(dev, 0, buf, len);
}

static void store_clear(const struct device *dev) { (void)retention_clear(dev); }

#define FAULT_STORE fault_dev
#define STATE_STORE state_dev
#else
/* No retained RAM in the devicetree: noinit RAM. It usually survives a warm reset, but the bootloader or a
 * linker change may overwrite it. */
static __noinit uint8_t fault_ram[SENSEI_DIAG_FAULT_BYTES];
static __noinit uint8_t state_ram[SENSEI_DIAG_STATE_BYTES];

static int store_read(const uint8_t *dev, uint8_t *buf, size_t len) {
  memcpy(buf, dev, len);
  return 0;
}

static int store_write(uint8_t *dev, const uint8_t *buf, size_t len) {
  memcpy(dev, buf, len);
  return 0;
}

static void store_clear(uint8_t *dev) { memset(dev, 0, sizeof(fault_ram)); }

#define FAULT_STORE fault_ram
#define STATE_STORE state_ram
#endif

static uint32_t reset_cause;
static uint32_t reset_cause_hw;
static uint32_t boots;
static struct sensei_diag_fault boot_fault;
static bool boot_fault_valid;
static volatile uint16_t last_job;
static volatile uint32_t image_id;
static atomic_t capturing; /* a capture is running: a fault inside it (the uptime read) does not start another */

int sensei_diag_boot(void) {
  uint32_t hw = 0;
  bool hw_ok = hwinfo_get_reset_cause(&hw) == 0;
  uint8_t fb[SENSEI_DIAG_FAULT_BYTES];
  uint8_t st[SENSEI_DIAG_STATE_BYTES];
  bool had_state = false;
  int ret;

  if (hw_ok) {
    (void)hwinfo_clear_reset_cause();
  }

  /* The record of the last fatal error, once. */
  boot_fault_valid = false;
  ret = store_read(FAULT_STORE, fb, sizeof(fb));
  if (ret == 0) {
    int rc = sensei_diag_fault_unpack(fb, sizeof(fb), &boot_fault);

    boot_fault_valid = rc == 0;
    if (rc != -ENOMSG) {
      store_clear(FAULT_STORE);
    }
    if (rc == -EBADMSG) {
      LOG_WRN("fault record damaged, dropped");
    }
  }

  ret = store_read(STATE_STORE, st, sizeof(st));
  if (ret != 0) {
    memset(st, 0, sizeof(st));
  }
  boots = sensei_diag_boot_count_next(st, &had_state);
  (void)store_write(STATE_STORE, st, sizeof(st));

  reset_cause_hw = hw_ok ? hw : 0;
  reset_cause = sensei_diag_reset_map(hw, hw_ok, boot_fault_valid);
  LOG_INF("boot %u, reset cause 0x%x%s%s", (unsigned)boots, (unsigned)reset_cause,
          had_state ? "" : " (retained RAM was lost)", boot_fault_valid ? ", fault record found" : "");
  return 0;
}

SYS_INIT(sensei_diag_boot, POST_KERNEL, 99);

uint32_t sensei_diag_reset_cause(void) { return reset_cause; }

uint32_t sensei_diag_reset_cause_hw(void) { return reset_cause_hw; }

uint32_t sensei_diag_boot_count(void) { return boots; }

bool sensei_diag_retained(void) { return IS_ENABLED(DIAG_RETENTION); }

bool sensei_diag_peek_fault(struct sensei_diag_fault *out) {
  if (!boot_fault_valid) {
    return false;
  }
  *out = boot_fault;
  return true;
}

bool sensei_diag_take_fault(struct sensei_diag_fault *out) {
  if (!sensei_diag_peek_fault(out)) {
    return false;
  }
  boot_fault_valid = false;
  return true;
}

void sensei_diag_set_job(uint16_t id) { last_job = id; }

void sensei_diag_set_image_id(uint32_t id) { image_id = id; }

void sensei_diag_capture_ext(uint8_t reason, uint32_t pc, uint32_t lr, uint32_t status, const char *thread) {
  struct sensei_diag_fault f = {
      .reason = reason,
      .last_job = last_job,
      .pc = pc,
      .lr = lr,
      .status = status,
      .image_id = image_id,
      .boot_count = boots,
  };
  uint8_t b[SENSEI_DIAG_FAULT_BYTES];

  /* The handler may run with the timer lock held (a fault in the timer code): k_uptime_get() then asserts or hangs. A
   * nested fault must not replace the record of the first one, so the record is written first, with uptime 0, and
   * the uptime is added by a second write. A fault in the second step finds `capturing` set and returns. */
  if (!atomic_cas(&capturing, 0, 1)) {
    return;
  }

  strncpy(f.thread, thread != NULL && thread[0] != '\0' ? thread : "?", sizeof(f.thread) - 1);
#ifdef CONFIG_SENSEI_DIAG_COREDUMP
  if (sensei_diag_dump_valid()) {
    f.flags |= SENSEI_FAULT_FLAG_COREDUMP;
  }
#endif
  sensei_diag_fault_pack(&f, b);
  (void)store_write(FAULT_STORE, b, sizeof(b));
  f.uptime_s = (uint32_t)(k_uptime_get() / 1000);
  sensei_diag_fault_pack(&f, b);
  (void)store_write(FAULT_STORE, b, sizeof(b));
  atomic_set(&capturing, 0);
}

void sensei_diag_capture(unsigned int reason, const struct arch_esf *esf) {
  uint32_t pc = 0, lr = 0, status = 0;
  const char *name = NULL;

#if defined(CONFIG_ARM)
  if (esf != NULL) {
    pc = esf->basic.pc;
    lr = esf->basic.lr;
    status = esf->basic.xpsr;
  }
#endif
#if defined(CONFIG_MULTITHREADING)
  bool in_isr = false;

#if defined(CONFIG_CPU_CORTEX_M)
  /* The stacked xPSR holds the exception number of the code that was interrupted: 0 in a thread. */
  in_isr = esf != NULL && (esf->basic.xpsr & 0x1FFu) != 0;
#else
  in_isr = k_is_in_isr();
#endif
  if (!in_isr) {
    name = k_thread_name_get(k_current_get());
  }
#endif
  sensei_diag_capture_ext(reason > 0xFE ? SENSEI_FAULT_UNKNOWN : (uint8_t)reason, pc, lr, status,
                          name != NULL && name[0] != '\0' ? name : "ISR");
}

#ifdef CONFIG_SENSEI_DIAG_FATAL_HANDLER
/* The same behaviour as RESET_ON_FATAL_ERROR of NCS (nrf/lib/fatal_error), after the record is written. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
  sensei_diag_capture(reason, esf);
  LOG_PANIC();
#ifdef CONFIG_SENSEI_DIAG_RESET_ON_FATAL
  LOG_ERR("Resetting system");
  sys_arch_reboot(0);
#else
  LOG_ERR("Halting system");
  k_fatal_halt(reason);
#endif
  CODE_UNREACHABLE;
}
#endif
