/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Is a shield fitted (the start-up check: present, absent, fault) and the levels of the nRF21540 lines
 * (lpwan_shield.h). */

#include <stdio.h>
#include <string.h>

#include "sensei_lpwan/lpwan_shield.h"

static int check_failures;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                                  \
      check_failures++;                                                                                                \
    }                                                                                                                  \
  } while (0)

/* A scripted start: what the hardware does, and what the check did about it. */
struct hw {
  bool answers;  /* the expander answers at 0x20 as a TCAL6408 */
  bool ready;    /* its driver and the other devices came up */
  int pins_fail; /* the first n pin writes fail (-1: all of them) */
  int irq_err;   /* result of the interrupt setup */
  int answers_calls, pins_calls, irq_calls, cut_calls, release_calls, pause_calls;
};

static bool hw_answers(void *c) {
  struct hw *h = c;

  h->answers_calls++;
  return h->answers;
}

static bool hw_ready(void *c) { return ((struct hw *)c)->ready; }

static int hw_pins(void *c) {
  struct hw *h = c;

  h->pins_calls++;
  return h->pins_fail < 0 || h->pins_calls <= h->pins_fail ? -5 : 0;
}

static int hw_irq(void *c) {
  struct hw *h = c;

  h->irq_calls++;
  return h->irq_err;
}

static void hw_cut(void *c) { ((struct hw *)c)->cut_calls++; }
static void hw_release(void *c) { ((struct hw *)c)->release_calls++; }
static void hw_pause(void *c) { ((struct hw *)c)->pause_calls++; }

static const struct lpwan_shield_ops ops = {
    .expander_answers = hw_answers,
    .devices_ready = hw_ready,
    .pins_init = hw_pins,
    .irq_setup = hw_irq,
    .power_cut = hw_cut,
    .release = hw_release,
    .pause = hw_pause,
};

static enum lpwan_shield_state start(struct hw *h) { return lpwan_shield_start(&ops, h); }

static void test_start_present(void) {
  struct hw h = {.answers = true, .ready = true};

  CHECK(start(&h) == LPWAN_SHIELD_PRESENT);
  CHECK(h.pins_calls == 1 && h.irq_calls == 1 && h.pause_calls == 0);
  CHECK(h.cut_calls == 0 && h.release_calls == 0);
}

static void test_start_absent(void) {
  /* nothing at 0x20 (the driver is down too): no shield; the pins that the failed driver left are given back, nothing
   * is driven and nothing is tried again */
  struct hw h = {.answers = false, .ready = false};

  CHECK(start(&h) == LPWAN_SHIELD_ABSENT);
  CHECK(h.release_calls == 1 && h.cut_calls == 0 && h.pins_calls == 0 && h.irq_calls == 0);
  /* another chip at 0x20 whose driver came up all the same (it is not the TCAL6408): still no shield */
  struct hw other = {.answers = false, .ready = true};

  CHECK(start(&other) == LPWAN_SHIELD_ABSENT);
  CHECK(other.release_calls == 1 && other.cut_calls == 0 && other.pins_calls == 0);
}

static void test_start_fault_driver_down(void) {
  /* the expander answers, its driver did not come up (a one-off I2C error at boot): a shield that may be powered */
  struct hw h = {.answers = true, .ready = false};

  CHECK(start(&h) == LPWAN_SHIELD_FAULT);
  CHECK(h.cut_calls == 1 && h.release_calls == 0 && h.pins_calls == 0 && h.irq_calls == 0);
}

static void test_start_pins_retry(void) {
  /* one-off failures of the pin writes are tried again: the shield is present */
  struct hw h = {.answers = true, .ready = true, .pins_fail = LPWAN_SHIELD_PIN_TRIES - 1};

  CHECK(start(&h) == LPWAN_SHIELD_PRESENT);
  CHECK(h.pins_calls == LPWAN_SHIELD_PIN_TRIES && h.pause_calls == LPWAN_SHIELD_PIN_TRIES - 1);
  CHECK(h.cut_calls == 0 && h.irq_calls == 1);
}

static void test_start_fault_pins(void) {
  /* the pins keep failing: a fault, EN_3V3 is driven low, the interrupt is not set up, no more than the tries */
  struct hw h = {.answers = true, .ready = true, .pins_fail = -1};

  CHECK(start(&h) == LPWAN_SHIELD_FAULT);
  CHECK(h.pins_calls == LPWAN_SHIELD_PIN_TRIES);
  CHECK(h.cut_calls == 1 && h.irq_calls == 0 && h.release_calls == 0);
}

static void test_start_fault_irq(void) {
  /* the pins are fine, the interrupt cannot be set up: not "present" with a client that never starts */
  struct hw h = {.answers = true, .ready = true, .irq_err = -5};

  CHECK(start(&h) == LPWAN_SHIELD_FAULT);
  CHECK(h.pins_calls == 1 && h.irq_calls == 1 && h.cut_calls == 1);
}

static void test_start_without_pause(void) {
  struct hw h = {.answers = true, .ready = true, .pins_fail = 1};
  struct lpwan_shield_ops o = ops;

  o.pause = NULL;
  CHECK(lpwan_shield_start(&o, &h) == LPWAN_SHIELD_PRESENT && h.pins_calls == 2);
}

static void test_start_all_cases(void) {
  /* every combination: never "unknown"; "present" only when everything worked; no expander, no power cut; an expander
   * that is not usable always gets EN_3V3 driven low */
  for (int m = 0; m < 16; m++) {
    bool answers = m & 1, ready = m & 2, pins_bad = m & 4, irq_bad = m & 8;
    struct hw h = {.answers = answers, .ready = ready, .pins_fail = pins_bad ? -1 : 0, .irq_err = irq_bad ? -5 : 0};
    enum lpwan_shield_state st = start(&h);

    CHECK(st != LPWAN_SHIELD_UNKNOWN);
    CHECK((st == LPWAN_SHIELD_PRESENT) == (answers && ready && !pins_bad && !irq_bad));
    CHECK((st == LPWAN_SHIELD_ABSENT) == !answers);
    CHECK(h.cut_calls == (st == LPWAN_SHIELD_FAULT ? 1 : 0));
  }
}

static void test_names(void) {
  CHECK(strcmp(lpwan_shield_name(LPWAN_SHIELD_UNKNOWN), "unknown") == 0);
  CHECK(strcmp(lpwan_shield_name(LPWAN_SHIELD_PRESENT), "present") == 0);
  CHECK(strcmp(lpwan_shield_name(LPWAN_SHIELD_ABSENT), "absent") == 0);
  CHECK(strcmp(lpwan_shield_name(LPWAN_SHIELD_FAULT), "fault") == 0);
}

static void test_fem_off(void) {
  /* extender not in use: both lines low, with the shield on or off, whatever MODE is configured to */
  for (int on = 0; on < 2; on++) {
    for (int mode = 0; mode < 2; mode++) {
      struct lpwan_fem_levels l = lpwan_fem_levels(false, on, mode);

      CHECK(!l.npd_high);
      CHECK(!l.mode_high);
    }
  }
}

static void test_fem_on(void) {
  struct lpwan_fem_levels l = lpwan_fem_levels(true, true, false);

  CHECK(l.npd_high && !l.mode_high);
  l = lpwan_fem_levels(true, true, true);
  CHECK(l.npd_high && l.mode_high);
  /* the shield's 3.3 V is off: power the extender down too */
  l = lpwan_fem_levels(true, false, true);
  CHECK(!l.npd_high);
}

int main(void) {
  test_start_present();
  test_start_absent();
  test_start_fault_driver_down();
  test_start_pins_retry();
  test_start_fault_pins();
  test_start_fault_irq();
  test_start_without_pause();
  test_start_all_cases();
  test_names();
  test_fem_off();
  test_fem_on();
  if (check_failures != 0) {
    printf("test_lpwan_shield: %d failed\n", check_failures);
    return 1;
  }
  printf("test_lpwan_shield: ok\n");
  return 0;
}
