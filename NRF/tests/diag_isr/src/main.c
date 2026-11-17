/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Fault capture of the SENSEI diagnostics module for faults that do not happen in a thread, four boots in one run:
 *   boot 1: no record. An undefined instruction in a timer interrupt: record, reset.
 *   boot 2: the record is there once (undefined instruction, thread "ISR", an exception number in the stacked xPSR, PC
 *           in the handler), the reset cause says FAULT. Then a thread overflows its stack (stack guard): reset.
 *   boot 3: the record says stack check failure, thread "overflow". Then k_panic() in the timer interrupt: reset.
 *   boot 4: the record says kernel panic, thread "ISR". Prints DIAG ISR TEST PASSED.
 * With CONFIG_TEST_ISR_STACK_OVERFLOW the first boot overflows the interrupt stack instead: the fault handler has no
 * stack left, the core locks up and no record is written. QEMU stops on a lockup; the nRF resets and the reset cause
 * says CPU_LOCKUP (counted as FAULT, see lib/sensei_diag). Prints DIAG ISR STACK OVERFLOW STARTED before.
 * Any other result prints DIAG ISR TEST FAILED and a reason. */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "diag/sensei_diag_rt.h"

#define K_ERR_USAGE_UNDEFINED 36 /* K_ERR_ARM_USAGE_UNDEFINED_INSTRUCTION */

static K_THREAD_STACK_DEFINE(overflow_stack, 1024);
static struct k_thread overflow_thread;
static struct k_timer timer;
static volatile int how;
static volatile int stop; /* never set: keeps the compiler from turning the recursion into a loop */

static void fail(const char *what) {
  printk("DIAG ISR TEST FAILED: %s\n", what);
  k_sleep(K_FOREVER);
}

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      fail(#cond);                                                                                                     \
    }                                                                                                                  \
  } while (0)

static __noinline int recurse(volatile int n) {
  volatile char pad[64];

  pad[0] = (char)n;
  if (stop) {
    return n;
  }
  return recurse(n + 1) + pad[0];
}

/* noinline: the PC of the record must lie in this function. */
static __noinline void timer_fault(struct k_timer *t) {
  ARG_UNUSED(t);
  switch (how) {
  case 1:
    __asm__ volatile("udf #0");
    break;
  case 3:
    k_panic();
    break;
  case 4:
    printk("DIAG ISR STACK OVERFLOW STARTED\n");
    (void)recurse(0);
    break;
  default:
    break;
  }
}

static void overflow_entry(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  (void)recurse(0);
}

static void start_timer(int what) {
  how = what;
  k_timer_init(&timer, timer_fault, NULL);
  k_timer_start(&timer, K_MSEC(10), K_NO_WAIT);
  k_sleep(K_FOREVER);
}

int main(void) {
  struct sensei_diag_fault f;
  uint32_t boot = sensei_diag_boot_count();
  uint32_t cause = sensei_diag_reset_cause();

  printk("DIAG ISR boot %u, reset cause 0x%x\n", (unsigned)boot, (unsigned)cause);
  CHECK(sensei_diag_retained());

  if (IS_ENABLED(CONFIG_TEST_ISR_STACK_OVERFLOW)) {
    CHECK(boot == 1);
    start_timer(4);
  } else if (boot == 1) {
    CHECK((cause & SENSEI_RESET_FAULT) == 0);
    CHECK(!sensei_diag_take_fault(&f));
    start_timer(1);
  } else if (boot == 2) {
    uintptr_t fn = (uintptr_t)timer_fault & ~1u;

    CHECK((cause & SENSEI_RESET_FAULT) != 0);
    CHECK(sensei_diag_take_fault(&f));
    printk("DIAG record: reason %u pc 0x%x xpsr 0x%x thread %s boot %u\n", f.reason, (unsigned)f.pc, (unsigned)f.status,
           f.thread, (unsigned)f.boot_count);
    CHECK(f.reason == K_ERR_USAGE_UNDEFINED);
    CHECK(strcmp(f.thread, "ISR") == 0);
    CHECK((f.status & 0x1FF) != 0); /* xPSR: the fault hit an interrupt handler */
    CHECK(f.pc >= fn && f.pc < fn + 32);
    CHECK(f.boot_count == 1);
    CHECK(!sensei_diag_take_fault(&f)); /* read once */
    k_thread_create(&overflow_thread, overflow_stack, K_THREAD_STACK_SIZEOF(overflow_stack), overflow_entry, NULL, NULL,
                    NULL, 5, 0, K_NO_WAIT);
    k_thread_name_set(&overflow_thread, "overflow");
    k_sleep(K_FOREVER);
  } else if (boot == 3) {
    CHECK((cause & SENSEI_RESET_FAULT) != 0);
    CHECK(sensei_diag_take_fault(&f));
    printk("DIAG record: reason %u (%s) thread %s boot %u\n", f.reason, sensei_diag_fault_reason_name(f.reason),
           f.thread, (unsigned)f.boot_count);
    CHECK(f.reason == SENSEI_FAULT_STACK_CHK_FAIL);
    CHECK(strcmp(f.thread, "overflow") == 0);
    CHECK(f.boot_count == 2);
    start_timer(3);
  } else if (boot == 4) {
    CHECK((cause & SENSEI_RESET_FAULT) != 0);
    CHECK(sensei_diag_take_fault(&f));
    printk("DIAG record: reason %u (%s) thread %s boot %u\n", f.reason, sensei_diag_fault_reason_name(f.reason),
           f.thread, (unsigned)f.boot_count);
    CHECK(f.reason == SENSEI_FAULT_KERNEL_PANIC);
    CHECK(strcmp(f.thread, "ISR") == 0);
    CHECK(f.boot_count == 3);
    printk("DIAG ISR TEST PASSED\n");
  } else {
    fail("more boots than expected");
  }
  k_sleep(K_FOREVER);
  return 0;
}
