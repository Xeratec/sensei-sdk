/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Fault capture of the SENSEI diagnostics module, three boots in one run:
 *   boot 1: no record. A thread named "crasher" sets the job id and calls k_oops(): record, reset.
 *   boot 2: the oops record is there once (reason, thread, job, image id, PC in the crashing function, boot count of
 *           the crash), the reset cause says FAULT. Then an undefined instruction (usage fault): record, reset.
 *   boot 3: the usage fault record is there, the oops record is not. The coredump of the fault is in the retained RAM
 *           (registers and stack top, Zephyr coredump header). Prints DIAG TEST PASSED.
 * Any other result prints DIAG TEST FAILED and a reason. */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "diag/sensei_diag_rt.h"

#define JOB_ID 42
#define IMAGE_ID 0xCAFEF00Du

static K_THREAD_STACK_DEFINE(crash_stack, 1024);
static struct k_thread crash_thread;

static void fail(const char *what) {
  printk("DIAG TEST FAILED: %s\n", what);
  k_sleep(K_FOREVER);
}

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      fail(#cond);                                                                                                     \
    }                                                                                                                  \
  } while (0)

/* noinline: the PC of the record must lie in this function. */
static __noinline void crash_oops(void) {
  sensei_diag_set_job(JOB_ID);
  k_oops();
}

static __noinline void crash_cpu(void) {
  sensei_diag_set_job(JOB_ID + 1);
  __asm__ volatile("udf #0"); /* undefined instruction: usage fault (QEMU does not fault on unmapped addresses) */
}

static void crash_entry(void *a, void *b, void *c) {
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  if (a == NULL) {
    crash_oops();
  } else {
    crash_cpu();
  }
}

static void crash_in_thread(void *how) {
  k_thread_create(&crash_thread, crash_stack, K_THREAD_STACK_SIZEOF(crash_stack), crash_entry, how, NULL, NULL, 5, 0,
                  K_NO_WAIT);
  k_thread_name_set(&crash_thread, "crasher");
  k_sleep(K_FOREVER);
}

int main(void) {
  struct sensei_diag_fault f;
  uint32_t boot = sensei_diag_boot_count();
  uint32_t cause = sensei_diag_reset_cause();

  printk("DIAG boot %u, reset cause 0x%x, retained %d\n", (unsigned)boot, (unsigned)cause, sensei_diag_retained());
  CHECK(sensei_diag_retained());
  sensei_diag_set_image_id(IMAGE_ID);

  if (boot == 1) {
    CHECK((cause & SENSEI_RESET_FAULT) == 0);
    CHECK(!sensei_diag_take_fault(&f));
    crash_in_thread(NULL); /* does not return: the node resets */
  } else if (boot == 2) {
    uintptr_t fn = (uintptr_t)crash_oops & ~1u;

    CHECK((cause & SENSEI_RESET_FAULT) != 0);
    CHECK(sensei_diag_take_fault(&f));
    printk("DIAG record: reason %u pc 0x%x thread %s job %u boot %u image %x\n", f.reason, (unsigned)f.pc, f.thread,
           f.last_job, (unsigned)f.boot_count, (unsigned)f.image_id);
    CHECK(f.reason == SENSEI_FAULT_KERNEL_OOPS);
    CHECK(strcmp(f.thread, "crasher") == 0);
    CHECK(f.last_job == JOB_ID);
    CHECK(f.image_id == IMAGE_ID);
    CHECK(f.boot_count == 1);
    CHECK(f.pc >= fn && f.pc < fn + 48); /* the instruction after the SVC of k_oops() */
    CHECK(f.lr != 0);
    CHECK(f.uptime_s < 60);
    CHECK((f.flags & SENSEI_FAULT_FLAG_COREDUMP) != 0);
    /* read once: gone now */
    CHECK(!sensei_diag_take_fault(&f));
    crash_in_thread((void *)1); /* the usage fault */
  } else if (boot == 3) {
    uint8_t dump[32];

    CHECK((cause & SENSEI_RESET_FAULT) != 0);
    CHECK(sensei_diag_take_fault(&f));
    printk("DIAG record: reason %u (%s) xpsr 0x%x thread %s job %u boot %u\n", f.reason,
           sensei_diag_fault_reason_name(f.reason), (unsigned)f.status, f.thread, f.last_job, (unsigned)f.boot_count);
    CHECK(f.reason == 36); /* K_ERR_ARM_USAGE_UNDEFINED_INSTRUCTION */
    CHECK(strcmp(sensei_diag_fault_reason_name(f.reason), "usage-undefined-instruction") == 0);
    CHECK((f.status & 0x1FF) == 0); /* xPSR: the fault hit a thread, no interrupt */
    CHECK(f.status & (1u << 24));   /* Thumb state */
    CHECK(f.pc >= ((uintptr_t)crash_cpu & ~1u) && f.pc < ((uintptr_t)crash_cpu & ~1u) + 16);
    CHECK(strcmp(f.thread, "crasher") == 0);
    CHECK(f.last_job == JOB_ID + 1);
    CHECK(f.boot_count == 2);
    CHECK(!sensei_diag_take_fault(&f));
    /* the coredump of this fault: Zephyr header "ZE", version, and the arch block after it */
    size_t size = sensei_diag_dump_size();

    printk("DIAG coredump: %u bytes\n", (unsigned)size);
    CHECK(size >= 32 && size <= 0x378);
    CHECK(sensei_diag_dump_read(0, dump, sizeof(dump)) == sizeof(dump));
    CHECK(dump[0] == 'Z' && dump[1] == 'E');
    sensei_diag_dump_clear();
    CHECK(sensei_diag_dump_size() == 0);
    printk("DIAG TEST PASSED\n");
  } else {
    fail("more boots than expected");
  }
  k_sleep(K_FOREVER);
  return 0;
}
