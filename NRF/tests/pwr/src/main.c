/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* NRF/system/pwr with the real MAX77654 driver and BSP (pmic_bsp.c) on an I2C emulator of the PMIC
 * (emul_max77654.c), the GPIO emulator for the board lines and the ADC emulator for the battery. The board hooks of the
 * application (pwr_bsp_init/start) are FFF fakes. Time is the simulated clock of native_sim.
 *
 * Scenarios (Kconfig of this folder, testcase.yaml): the boot hook runs once, before main, so each way it can go has
 * its own build:
 *  - NORMAL: the PMIC answers. Boot order and rails, init and start, charger and battery thresholds of the chemistry
 *    (CONFIG_SENSEI_PWR_BATTERY_*), events, battery levels with hysteresis, errors of the PMIC, the periodic work, the
 *    button, power-off.
 *  - BOOT_FAIL: the PMIC does not answer at boot; pwr_init() tries again.
 *  - NO_RAILS: SENSEI_PWR_BOOT_RAILS=n.
 *
 * The tests of a scenario continue each other (init, then start, then the thread runs): keep the numbers. Power-off
 * resets the node: the reset is recorded and parks the calling thread, as the real one does not return. The last test
 * lets the power thread do it (long button press). */

#include <string.h>

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/fff.h>
#include <zephyr/init.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

#include "bsp/pwr_bsp.h"
#include "config/tasks.h"
#include "emul_max77654.h"
#include "max77654.h"
#include "max77654_reg.h"
#include "pwr/pwr.h"
#include "pwr/pwr_board.h"
#include "pwr/pwr_priv.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, pwr_bsp_init);
FAKE_VALUE_FUNC(int, pwr_bsp_start);

extern atomic_t pwr_is_initialised;
extern atomic_t pwr_is_running;

void __wrap_sys_reboot(int type);

/* ---- board ---- */

#define PIN_IRQ 5       /* PMIC nIRQ, active low */
#define PIN_SOFT_NRST 6 /* Soft_nRST, active low */

/* Time for one pass of the power thread (PMIC flags, status, three battery readings); also the wait for "no event". */
#define THREAD_PASS_MS 500
/* After a key-up that would be a long press: the pass, then 500 ms before the reset. Wait this long to say "no reset".
 */
#define NO_REBOOT_MS 1500

static const struct device *const gpio0 __maybe_unused = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *const adc0 = DEVICE_DT_GET(DT_NODELABEL(adc0));
static const struct gpio_dt_spec gap9_sel __maybe_unused = GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_gap9_pwr), gpios);
static const struct gpio_dt_spec led_pwr = GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_led_pwr), gpios);
static const struct gpio_dt_spec gap9_lines[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_gap9_pwr), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_gap9_i2c_ctrl), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_gap9_spi_ctrl), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_gap9_shared_ctrl), gpios),
};
static const struct gpio_dt_spec irq_pin __maybe_unused = GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_pmic_irq), gpios);
static const struct gpio_dt_spec soft_nrst_pin __maybe_unused = GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_soft_nrst), gpios);

static bool is_output(const struct gpio_dt_spec *s) {
  gpio_flags_t f = 0;

  return gpio_emul_flags_get(s->port, s->pin, &f) == 0 && (f & GPIO_OUTPUT) != 0;
}

static bool all_gap9_lines_inactive(void) {
  for (size_t i = 0; i < ARRAY_SIZE(gap9_lines); i++) {
    if (!is_output(&gap9_lines[i]) || gpio_emul_output_get_dt(&gap9_lines[i]) != 0) {
      return false;
    }
  }
  return true;
}

static bool any_gap9_line_output(void) {
  for (size_t i = 0; i < ARRAY_SIZE(gap9_lines); i++) {
    if (is_output(&gap9_lines[i])) {
      return true;
    }
  }
  return false;
}

/* The battery as the SAADC sees it: the AMUX divides the battery voltage by 0.272 (driver: gain 272/1000). */
static void __maybe_unused set_battery_mv(uint32_t mv) {
  zassert_ok(adc_emul_const_value_set(adc0, 0, mv * 272 / 1000));
}

static int set_battery_mv_fn_value __maybe_unused;
static int fluke_calls __maybe_unused;

static int __maybe_unused fluke_first_sample(const struct device *dev, unsigned int chan, void *param,
                                             uint32_t *result) {
  ARG_UNUSED(dev);
  ARG_UNUSED(chan);
  ARG_UNUSED(param);
  *result = fluke_calls++ == 0 ? 5 : (uint32_t)set_battery_mv_fn_value * 272 / 1000; /* 5 mV: the first-reading fluke */
  return 0;
}

/* ---- reset: recorded, does not return ---- */

static int reboots;
static int reboot_type;
static K_SEM_DEFINE(reboot_sem, 0, 10);

void __wrap_sys_reboot(int type) {
  reboots++;
  reboot_type = type;
  (void)k_mutex_unlock(&pwr_mutex); /* only if this thread holds it: the real call never comes back, nobody would */
  k_sem_give(&reboot_sem);
  for (;;) {
    k_sleep(K_FOREVER);
  }
}

/* ---- what the boot hook did, seen from hooks around it (the hook is SYS_INIT POST_KERNEL 55) ---- */

static struct {
  int64_t t_before, t_after;
  bool outputs_before;
  int transfers_before;
  bool first_access_seen;
  bool first_access_all_inactive;
  int sbb_mv[3], ldo_mv[2];
  bool led_on_after, led_output_after, lines_inactive_after;
  int resets_after;
  int64_t last_write_after;
} snap;

static void first_access(void) {
  snap.first_access_seen = true;
  snap.first_access_all_inactive = all_gap9_lines_inactive();
}

static int snap_before_hook(void) {
  struct pmic_emul *e = pmic_emul_get();

  snap.t_before = k_uptime_get();
  snap.outputs_before = any_gap9_line_output();
  snap.transfers_before = e->transfers;
  e->first_access_cb = first_access;
  if (IS_ENABLED(CONFIG_SENSEI_TEST_BOOT_FAIL)) {
    e->present = false;
  }
  return 0;
}

static int snap_after_hook(void) {
  struct pmic_emul *e = pmic_emul_get();

  snap.t_after = k_uptime_get();
  for (int i = 0; i < 3; i++) {
    snap.sbb_mv[i] = pmic_emul_sbb_mv(i);
  }
  for (int i = 0; i < 2; i++) {
    snap.ldo_mv[i] = pmic_emul_ldo_mv(i);
  }
  snap.led_output_after = is_output(&led_pwr);
  snap.led_on_after = snap.led_output_after && gpio_emul_output_get_dt(&led_pwr) == 1;
  snap.lines_inactive_after = all_gap9_lines_inactive();
  snap.resets_after = e->resets;
  snap.last_write_after = e->last_write_ms;
  return 0;
}

SYS_INIT(snap_before_hook, POST_KERNEL, 54);
SYS_INIT(snap_after_hook, POST_KERNEL, 56);

ZTEST_SUITE(pwr, NULL, NULL, NULL, NULL, NULL);

/* Battery levels of the chemistry (pwr/Kconfig): the table in README.md */
#ifdef CONFIG_SENSEI_PWR_BATTERY_LIION
#define FULL_MV 4200
#define EMPTY_MV 3300
#define CRIT_MV 3100
#define LOW_MV 3300
#define OK_MV 3500
#else
#define FULL_MV 3650
#define EMPTY_MV 3000
#define CRIT_MV 2900
#define LOW_MV 3100
#define OK_MV 3300
#endif

#if defined(CONFIG_SENSEI_TEST_NORMAL) || defined(CONFIG_SENSEI_TEST_BOOT_FAIL)

/* The power module sets up its two input lines itself (input with the pull of the devicetree, idle high, no edge from
 * the setup). */
static void check_irq_pins_are_inputs(void) {
  gpio_flags_t f = 0;

  zassert_ok(gpio_emul_flags_get(irq_pin.port, irq_pin.pin, &f));
  zassert_true((f & GPIO_INPUT) != 0 && (f & GPIO_PULL_UP) != 0, "nIRQ: input with pull-up, flags 0x%x", f);
  zassert_equal(gpio_pin_get_raw(irq_pin.port, irq_pin.pin), 1, "nIRQ idles high");
  zassert_ok(gpio_emul_flags_get(soft_nrst_pin.port, soft_nrst_pin.pin, &f));
  zassert_true((f & GPIO_INPUT) != 0 && (f & GPIO_PULL_UP) != 0, "Soft_nRST: input with pull-up, flags 0x%x", f);
  zassert_equal(gpio_pin_get_raw(soft_nrst_pin.port, soft_nrst_pin.pin), 1, "Soft_nRST idles high");
}

/* ---- the PMIC at boot: the first PMIC access and its effects ---- */

static void check_rails_on(const int sbb[3], const int ldo[2]) {
  zassert_equal(sbb[0], 3300, "SBB0 3.3 V");
  zassert_equal(sbb[1], 2800, "SBB1 2.8 V");
  zassert_equal(sbb[2], 1200, "SBB2 1.2 V");
  zassert_equal(ldo[0], 3300, "LDO0 3.3 V");
  zassert_equal(ldo[1], 0, "LDO1 stays off");
}

#endif

#if defined(CONFIG_SENSEI_TEST_NORMAL)

ZTEST(pwr, test_01_boot_hook_resets_the_pmic_first_and_waits) {
  struct pmic_emul *e = pmic_emul_get();

  zassert_true(snap.first_access_seen);
  zassert_equal(e->resets, 1, "the hook resets the PMIC once");
  zassert_equal(snap.transfers_before, 0, "the PMIC was not touched before the hook");
  zassert_equal(e->wlog[0].reg, MAX77654__REG_CNFG_GLBL);
  zassert_equal(e->wlog[0].val & MAX77654__REG_CNFG_GLBL__FIELD_SFT_CTRL,
                MAX77654__REG_CNFG_GLBL__FIELD_SFT_CTRL__CONST_SFT_CRST);
  zassert_equal(e->early, 0, "no access while the PMIC power-cycles after the reset");
  zassert_true(e->wlog[1].t_ms - e->wlog[0].t_ms >= PMIC_EMUL_RESET_BUSY_MS, "waits after the reset");
  zassert_true(e->wlog[0].t_ms >= snap.t_before);
}

ZTEST(pwr, test_02_gap9_lines_inactive_before_the_rails) {
  zassert_false(snap.outputs_before, "not configured before the hook");
  zassert_true(snap.first_access_all_inactive,
               "GAP9 supply select and the I2C, SPI and GPIO muxes are inactive before the first PMIC access");
  zassert_true(snap.lines_inactive_after);
}

ZTEST(pwr, test_03_rails_are_on_when_the_hook_ends_and_settled) {
  struct pmic_emul *e = pmic_emul_get();

  /* seen at priority 56, before the expander and the shield drivers (60 and later) start */
  check_rails_on(snap.sbb_mv, snap.ldo_mv);
  zassert_true(snap.led_on_after, "LED supply switched on");
  zassert_true(snap.t_after - snap.last_write_after >= 50, "50 ms for the rails to settle, then the drivers start");
  zassert_equal(e->early, 0);
  zassert_equal(e->nacks, 0);
}

ZTEST(pwr, test_04_charger_follows_the_battery_choice) {
  zassert_equal(pmic_emul_charger_enabled(), IS_ENABLED(CONFIG_SENSEI_PWR_BATTERY_LIION),
                "a Li-SOCl2 primary cell must never be charged");
  if (IS_ENABLED(CONFIG_SENSEI_PWR_BATTERY_LIION)) {
    zassert_equal(pmic_emul_charge_cv_mv(), 4200);
    zassert_equal(pmic_emul_charge_cc_x10ma(), 150, "15 mA");
  } else {
    zassert_equal(pmic_emul_charge_cv_mv(), 3600, "the lowest the charger can do");
  }
}

ZTEST(pwr, test_05_battery_thresholds_follow_the_battery_choice) {
  zassert_equal(PWR_BAT_CRITICAL_MV, CRIT_MV);
  zassert_equal(PWR_BAT_LOW_MV, LOW_MV);
  zassert_equal(PWR_BAT_OK_MV, OK_MV);
  zassert_true(PWR_BAT_CRITICAL_MV + 100 < PWR_BAT_LOW_MV, "room for the hysteresis between critical and low");
}

/* ---- pwr_init() ---- */

ZTEST(pwr, test_10_init_fails_when_the_board_hook_fails) {
  set_battery_mv(OK_MV + 100);
  pwr_bsp_init_fake.return_val = -1;
  zassert_equal(pwr_init(), -1);
  zassert_equal(pwr_bsp_init_fake.call_count, 1);
  zassert_equal(atomic_get(&pwr_is_initialised), 0);
  /* the power mutex is not left locked */
  zassert_ok(k_mutex_lock(&pwr_mutex, K_NO_WAIT));
  k_mutex_unlock(&pwr_mutex);
  /* the GAP9 supply select was set inactive on the way */
  zassert_true(is_output(&gap9_sel));
  zassert_equal(gpio_emul_output_get_dt(&gap9_sel), 0);
}

ZTEST(pwr, test_11_init_reads_the_battery_and_does_not_cycle_the_rails_again) {
  struct pmic_emul *e = pmic_emul_get();
  int resets = e->resets;

  pwr_bsp_init_fake.return_val = 0;
  zassert_ok(pwr_init());
  zassert_equal(pwr_bsp_init_fake.call_count, 2);
  zassert_equal(atomic_get(&pwr_is_initialised), 1);
  zassert_equal(e->resets, resets, "the boot hook did the reset; pwr_init() does not cycle the rails");
  check_irq_pins_are_inputs();
  zassert_within(pwr_bat_mV(), OK_MV + 100, 15);
  zassert_equal(pwr_get_status(&(struct pwr_status){0}), -EAGAIN, "the power thread has not read the PMIC yet");
}

ZTEST(pwr, test_12_start_fails_when_the_board_hook_fails_and_runs_no_thread) {
  struct pwr_status s;

  pwr_bsp_start_fake.return_val = -1;
  zassert_equal(pwr_start(), -1);
  zassert_equal(atomic_get(&pwr_is_running), 0);
  pwr_refresh();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  zassert_equal(pwr_get_status(&s), -EAGAIN, "no power thread");
}

ZTEST(pwr, test_13_start_runs_the_power_thread) {
  struct pwr_status s;

  pwr_bsp_start_fake.return_val = 0;
  zassert_ok(pwr_start());
  /* a pass of the thread takes 100 ms and more: the battery is three ADC readings, each a few ticks */
  zassert_equal(atomic_get(&pwr_is_running), 1);
  zassert_equal(pwr_bsp_start_fake.call_count, 2);
  zassert_true(is_output(&led_pwr));
  zassert_equal(gpio_emul_output_get_dt(&led_pwr), 1, "LED supply on");
  k_sleep(K_MSEC(THREAD_PASS_MS));
  zassert_ok(pwr_get_status(&s));
  zassert_true(s.valid);
  zassert_within(s.bat_mv, OK_MV + 100, 15);
}

/* ---- the power thread: status, events ---- */

static K_SEM_DEFINE(evt_sem, 0, 100);
static uint32_t evt_bits;
static struct pwr_status evt_status;

static void evt_cb(uint32_t events, const struct pwr_status *status, void *user) {
  ARG_UNUSED(user);
  evt_bits |= events;
  evt_status = *status;
  k_sem_give(&evt_sem);
}

/* The events of the next PMIC read; 0 when none comes. */
static uint32_t take_events(int timeout_ms) {
  uint32_t ev;

  if (k_sem_take(&evt_sem, K_MSEC(timeout_ms)) != 0) {
    return 0;
  }
  k_sleep(K_MSEC(10));
  while (k_sem_take(&evt_sem, K_NO_WAIT) == 0) {
  }
  ev = evt_bits;
  evt_bits = 0;
  return ev;
}

static uint32_t refresh_and_take(int timeout_ms) {
  evt_bits = 0;
  while (k_sem_take(&evt_sem, K_NO_WAIT) == 0) {
  }
  pwr_refresh();
  return take_events(timeout_ms);
}

static void irq_falling_edge(void) {
  zassert_ok(gpio_emul_input_set(gpio0, PIN_IRQ, 0));
  zassert_ok(gpio_emul_input_set(gpio0, PIN_IRQ, 1));
}

/* Key-up: Soft_nRST rises. */
static void soft_nrst_release(void) {
  zassert_ok(gpio_emul_input_set(gpio0, PIN_SOFT_NRST, 0));
  zassert_ok(gpio_emul_input_set(gpio0, PIN_SOFT_NRST, 1));
}

ZTEST(pwr, test_20_charger_and_input_states) {
  static const struct {
    uint8_t dtls;
    enum pwr_charger_state state;
  } map[] = {
      {MAX77654_CHG_DTLS_OFF, PWR_CHG_OFF},
      {MAX77654_CHG_DTLS_PREQUAL, PWR_CHG_PREQUAL},
      {MAX77654_CHG_DTLS_CC, PWR_CHG_CC},
      {MAX77654_CHG_DTLS_CC_JEITA, PWR_CHG_CC},
      {MAX77654_CHG_DTLS_CV, PWR_CHG_CV},
      {MAX77654_CHG_DTLS_CV_JEITA, PWR_CHG_CV},
      {MAX77654_CHG_DTLS_TOP_OFF, PWR_CHG_TOP_OFF},
      {MAX77654_CHG_DTLS_TOP_OFF_JEITA, PWR_CHG_TOP_OFF},
      {MAX77654_CHG_DTLS_DONE, PWR_CHG_DONE},
      {MAX77654_CHG_DTLS_DONE_JEITA, PWR_CHG_DONE},
      {MAX77654_CHG_DTLS_PREQUAL_TIMER_FAULT, PWR_CHG_FAULT},
      {MAX77654_CHG_DTLS_FASTCHARGE_TIMER_FAULT, PWR_CHG_FAULT},
      {MAX77654_CHG_DTLS_BAT_TEMPERATURE_FAULT, PWR_CHG_FAULT},
  };
  struct pwr_status s;

  pwr_set_event_cb(evt_cb, NULL);
  for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
    pmic_emul_set_charger(map[i].dtls, MAX77654_CHGIN_DTLS_OK, map[i].dtls != 0);
    refresh_and_take(THREAD_PASS_MS);
    zassert_ok(pwr_get_status(&s));
    zassert_equal(s.charger, map[i].state, "CHG_DTLS %u", map[i].dtls);
    zassert_equal(s.chgin, PWR_CHGIN_OK);
    zassert_equal(s.charging, map[i].dtls != 0);
  }
  pmic_emul_set_charger(MAX77654_CHG_DTLS_OFF, MAX77654_CHGIN_DTLS_UVLO, false);
  refresh_and_take(THREAD_PASS_MS);
  pwr_get_status(&s);
  zassert_equal(s.chgin, PWR_CHGIN_UVLO);
  pmic_emul_set_charger(MAX77654_CHG_DTLS_OFF, MAX77654_CHGIN_DTLS_OVP, false);
  refresh_and_take(THREAD_PASS_MS);
  pwr_get_status(&s);
  zassert_equal(s.chgin, PWR_CHGIN_OVP);
}

ZTEST(pwr, test_21_interrupt_flags_become_events) {
  static const struct {
    uint8_t glbl0, glbl1, chg;
    uint32_t event;
  } map[] = {
      {0, 0, MAX77654__REG_INT_CHG__FIELD_CHGIN_I, PWR_EVT_CHGIN_CHANGE},
      {0, 0, MAX77654__REG_INT_CHG__FIELD_CHG_I, PWR_EVT_CHG_CHANGE},
      {0, 0, MAX77654__REG_INT_CHG__FIELD_CHGIN_CTRL_I, PWR_EVT_CHGIN_CTRL},
      {0, 0, MAX77654__REG_INT_CHG__FIELD_TJ_REG_I, PWR_EVT_THERMAL},
      {MAX77654__REG_INT_GLBL0__FIELD_TJAL1_R, 0, 0, PWR_EVT_THERMAL},
      {MAX77654__REG_INT_GLBL0__FIELD_TJAL2_R, 0, 0, PWR_EVT_THERMAL},
      {MAX77654__REG_INT_GLBL0__FIELD_nEN_F, 0, 0, PWR_EVT_BUTTON},
  };

  zassert_equal(refresh_and_take(THREAD_PASS_MS), 0, "no flag, no event");
  for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
    pmic_emul_raise(map[i].glbl0, map[i].glbl1, map[i].chg);
    if (map[i].event == PWR_EVT_BUTTON) {
      irq_falling_edge(); /* nEN pulls nIRQ low, and the time of the press is the time of that interrupt */
    }
    zassert_equal(refresh_and_take(THREAD_PASS_MS), map[i].event, "flag %zu", i);
    zassert_false(pmic_emul_pending(), "the flags are read, so the PMIC clears them");
    zassert_equal(refresh_and_take(THREAD_PASS_MS), 0, "an event comes once");
  }
  /* two flags, two events in one callback */
  pmic_emul_raise(0, 0, MAX77654__REG_INT_CHG__FIELD_CHGIN_I | MAX77654__REG_INT_CHG__FIELD_CHG_I);
  zassert_equal(refresh_and_take(THREAD_PASS_MS), PWR_EVT_CHGIN_CHANGE | PWR_EVT_CHG_CHANGE);
  /* the last flag was the button: end that press (a short one) */
  soft_nrst_release();
  k_sleep(K_MSEC(50));
  zassert_equal(reboots, 0);
}

/* Battery: one step per reading. */
static void battery_step(uint32_t mv, uint32_t expect, const char *msg) {
  struct pwr_status s;

  set_battery_mv(mv);
  zassert_equal(refresh_and_take(THREAD_PASS_MS), expect, "%s: events", msg);
  zassert_ok(pwr_get_status(&s));
  zassert_within(s.bat_mv, mv, 15, "%s: mV", msg);
  zassert_within(pwr_bat_mV(), mv, 15);
}

ZTEST(pwr, test_22_battery_levels_have_hysteresis) {
  /* The first reading only sets the level (test_13 read OK + 100). */
  battery_step(OK_MV + 100, 0, "ok, no event");
  battery_step(LOW_MV - 50, PWR_EVT_BAT_LOW, "below low");
  battery_step(CRIT_MV - 50, PWR_EVT_BAT_CRITICAL, "below critical");
  battery_step(CRIT_MV + 50, 0, "still critical: left only above critical + 100 mV");
  battery_step(CRIT_MV + 150, PWR_EVT_BAT_LOW, "back to low");
  battery_step((LOW_MV + OK_MV) / 2, 0, "between low and ok: stays low");
  battery_step(OK_MV + 50, PWR_EVT_BAT_OK, "ok again");
  battery_step(OK_MV + 100, 0, "ok stays ok");
  battery_step(CRIT_MV - 50, PWR_EVT_BAT_CRITICAL, "ok to critical at once");
  battery_step(OK_MV + 50, PWR_EVT_BAT_OK, "critical to ok at once");
}

ZTEST(pwr, test_23_battery_percent_is_capped) {
  set_battery_mv(FULL_MV + 50);
  refresh_and_take(THREAD_PASS_MS);
  zassert_equal(pwr_bat_perc(), 100);
  set_battery_mv(EMPTY_MV - 50);
  refresh_and_take(THREAD_PASS_MS);
  zassert_equal(pwr_bat_perc(), 0);
  set_battery_mv(OK_MV + 100);
  refresh_and_take(THREAD_PASS_MS);
  zassert_true(pwr_bat_perc() > 0 && pwr_bat_perc() < 100);
}

ZTEST(pwr, test_24_a_reading_below_100_mv_is_ignored) {
  struct pwr_status s;

  /* The first ADC sample after power-up is often a few mV: it must not pull the average down or raise an alarm. */
  set_battery_mv_fn_value = OK_MV + 100;
  fluke_calls = 0;
  zassert_ok(adc_emul_value_func_set(adc0, 0, fluke_first_sample, NULL));
  zassert_equal(refresh_and_take(THREAD_PASS_MS), 0);
  pwr_get_status(&s);
  zassert_within(s.bat_mv, OK_MV + 100, 15, "average of the two good samples, not of three");

  /* All three samples invalid: no new value, no event, the thread goes on. */
  set_battery_mv(0);
  zassert_equal(refresh_and_take(THREAD_PASS_MS), 0);
  zassert_within(pwr_bat_mV(), OK_MV + 100, 15, "the last good value stays");
  set_battery_mv(OK_MV + 100);
  zassert_equal(refresh_and_take(THREAD_PASS_MS), 0);
  zassert_within(pwr_bat_mV(), OK_MV + 100, 15);
}

ZTEST(pwr, test_25_a_failed_status_read_keeps_the_last_state_but_not_the_events) {
  struct pwr_status s;
  struct pmic_emul *e = pmic_emul_get();

  pmic_emul_set_charger(MAX77654_CHG_DTLS_CC, MAX77654_CHGIN_DTLS_OK, true);
  refresh_and_take(THREAD_PASS_MS);
  pmic_emul_set_charger(MAX77654_CHG_DTLS_CV, MAX77654_CHGIN_DTLS_OK, true);
  e->fail_read_reg = MAX77654__REG_STAT_CHG_B;
  pmic_emul_raise(0, 0, MAX77654__REG_INT_CHG__FIELD_CHG_I);
  /* the flags were read (and cleared in the PMIC): the event is not lost, the charger state is still the old one */
  zassert_equal(refresh_and_take(THREAD_PASS_MS), PWR_EVT_CHG_CHANGE);
  zassert_equal(evt_status.charger, PWR_CHG_CC, "stale: the status read failed");
  e->fail_read_reg = -1;
  refresh_and_take(THREAD_PASS_MS);
  pwr_get_status(&s);
  zassert_equal(s.charger, PWR_CHG_CV);
}

ZTEST(pwr, test_26_a_failed_flag_read_does_not_drop_the_flags) {
  struct pmic_emul *e = pmic_emul_get();

  e->fail_read_reg = MAX77654__REG_INT_GLBL0;
  pmic_emul_raise(0, 0, MAX77654__REG_INT_CHG__FIELD_CHGIN_I);
  zassert_equal(refresh_and_take(THREAD_PASS_MS), 0, "flags of a failed read are not events");
  zassert_true(pmic_emul_pending(), "and they stay in the PMIC");
  e->fail_read_reg = -1;
  zassert_equal(refresh_and_take(THREAD_PASS_MS), PWR_EVT_CHGIN_CHANGE, "delivered when the read works again");
  zassert_equal(refresh_and_take(THREAD_PASS_MS), 0);
}

ZTEST(pwr, test_27_thread_wakes_by_itself_and_reapplies_the_charger_config) {
  struct pmic_emul *e = pmic_emul_get();
  int chg_writes = e->wcount[MAX77654__REG_CNFG_CHG_B];
  int nacks = e->nacks;

  /* every 20 s without an interrupt */
  set_battery_mv(OK_MV + 200);
  k_sleep(K_SECONDS(21));
  zassert_within(pwr_bat_mV(), OK_MV + 200, 15, "read by the 20 s period");

  /* every 25 minutes the charger and global config are written again (datasheet recommendation) */
  k_sleep(K_SECONDS(26 * 60));
  zassert_equal(e->wcount[MAX77654__REG_CNFG_CHG_B] - chg_writes, 1, "once in 26 minutes");
  zassert_equal(pmic_emul_charger_enabled(), IS_ENABLED(CONFIG_SENSEI_PWR_BATTERY_LIION), "same config as before");
  zassert_equal(e->nacks, nacks, "no failed access");
  evt_bits = 0;
  while (k_sem_take(&evt_sem, K_NO_WAIT) == 0) {
  }
}

/* Time until the pin has this logical level, or -1. */
static int wait_pin(const struct gpio_dt_spec *s, int level, int timeout_ms) {
  int64_t t0 = k_uptime_get();

  while (k_uptime_get() - t0 < timeout_ms) {
    if (gpio_emul_output_get_dt(s) == level) {
      return (int)(k_uptime_get() - t0);
    }
    k_sleep(K_MSEC(10));
  }
  return -1;
}

ZTEST(pwr, test_28_button_resets_gap9_only_when_it_is_on) {
  int64_t low_at;

  gap9_pwr(true);
  zassert_equal(gpio_emul_output_get_dt(&gap9_sel), 1);
  pmic_emul_raise(MAX77654__REG_INT_GLBL0__FIELD_nEN_F, 0, 0);
  irq_falling_edge();
  zassert_true(wait_pin(&gap9_sel, 0, THREAD_PASS_MS) >= 0, "GAP9 is held in reset");
  low_at = k_uptime_get();
  zassert_true(wait_pin(&gap9_sel, 1, THREAD_PASS_MS) >= 0, "and released again");
  zassert_true(k_uptime_get() - low_at >= 100, "for 100 ms");
  soft_nrst_release();
  k_sleep(K_MSEC(THREAD_PASS_MS));

  gap9_pwr(false);
  pmic_emul_raise(MAX77654__REG_INT_GLBL0__FIELD_nEN_F, 0, 0);
  irq_falling_edge();
  zassert_equal(wait_pin(&gap9_sel, 1, THREAD_PASS_MS * 2), -1, "GAP9 is off: a reset pulse would switch it on");
  soft_nrst_release();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  zassert_equal(reboots, 0);
}

ZTEST(pwr, test_29_log_voltages_takes_the_mutex_and_switches_the_amux_off) {
  k_sleep(K_MSEC(THREAD_PASS_MS)); /* the power thread holds the mutex during a pass; the call waits only 50 ms */
  zassert_ok(pwr_log_voltages());
  zassert_equal(pmic_emul_amux(), 0, "AMUX off after a measurement, to save power");
}

static struct k_thread helper;
static K_THREAD_STACK_DEFINE(helper_stack, 2048);
static int helper_ret;

static void log_voltages_entry(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  helper_ret = pwr_log_voltages();
}

ZTEST(pwr, test_30_log_voltages_does_not_wait_for_ever) {
  helper_ret = 0;
  zassert_ok(k_mutex_lock(&pwr_mutex, K_FOREVER));
  k_thread_create(&helper, helper_stack, K_THREAD_STACK_SIZEOF(helper_stack), log_voltages_entry, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
  zassert_ok(k_thread_join(&helper, K_SECONDS(1)));
  k_mutex_unlock(&pwr_mutex);
  zassert_equal(helper_ret, -EBUSY);
}

/* ---- power off ---- */

static void off_entry(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  pwr_off_and_reboot();
}

static void run_power_off(void) {
  k_sem_reset(&reboot_sem);
  k_thread_create(&helper, helper_stack, K_THREAD_STACK_SIZEOF(helper_stack), off_entry, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
  zassert_ok(k_sem_take(&reboot_sem, K_SECONDS(5)), "the node reboots");
  k_thread_abort(&helper);
}

static void rails_back_on(void) {
  struct max77654_h *h = pwr_pmic();

  for (int i = 0; i < 3; i++) {
    h->conf.sbb_conf[i].en = MAX77654_REG_ON;
  }
  h->conf.ldo_conf[0].en = MAX77654_REG_ON;
  zassert_equal(max77654_config(h), E_MAX77654_SUCCESS);
}

ZTEST(pwr, test_40_power_off_retries_and_reboots_anyway) {
  struct pmic_emul *e = pmic_emul_get();
  int nacks = e->nacks;
  int64_t t0 = k_uptime_get();

  e->present = false;
  run_power_off();
  e->present = true;
  zassert_equal(reboots, 1);
  zassert_equal(reboot_type, SYS_REBOOT_COLD);
  zassert_equal(e->nacks - nacks, 10, "ten tries");
  zassert_true(k_uptime_get() - t0 >= 500, "waits 500 ms before the reset");
  zassert_equal(pmic_emul_sbb_mv(0), 3300, "the PMIC could not be written");
}

ZTEST(pwr, test_41_power_off_switches_the_rails_and_the_led_off) {
  zassert_ok(gpio_pin_set_dt(&led_pwr, 1)); /* test_40 switched the LED supply off; the rails are still on */
  zassert_equal(pmic_emul_sbb_mv(0), 3300);
  run_power_off();
  zassert_equal(reboots, 2);
  zassert_equal(reboot_type, SYS_REBOOT_COLD);
  zassert_equal(gpio_emul_output_get_dt(&led_pwr), 0, "LED supply off");
  for (int i = 0; i < 3; i++) {
    zassert_equal(pmic_emul_sbb_mv(i), 0, "SBB%d off", i);
  }
  zassert_equal(pmic_emul_ldo_mv(0), 0);
  zassert_equal(pmic_emul_ldo_mv(1), 0);
  zassert_false(pmic_emul_reg(MAX77654__REG_CNFG_GLBL) & MAX77654__REG_CNFG_GLBL__FIELD_SBIA_EN, "main bias off");
}

ZTEST(pwr, test_42_power_off_goes_on_when_the_power_mutex_is_stuck) {
  rails_back_on();
  zassert_equal(pmic_emul_sbb_mv(1), 2800);
  zassert_ok(k_mutex_lock(&pwr_mutex, K_FOREVER)); /* held by this thread: the helper cannot get it */
  run_power_off();
  k_mutex_unlock(&pwr_mutex);
  zassert_equal(reboots, 3);
  zassert_equal(pmic_emul_sbb_mv(1), 0, "forced shut-down");
}

/* ---- the button ---- */

ZTEST(pwr, test_50_button_a_short_press_does_nothing_and_a_lost_key_up_is_replaced) {
  rails_back_on();
  /* key-up without key-down: ignored */
  soft_nrst_release();
  k_sleep(K_MSEC(NO_REBOOT_MS));
  zassert_equal(reboots, 3);

  /* a short press */
  pmic_emul_raise(MAX77654__REG_INT_GLBL0__FIELD_nEN_F, 0, 0);
  irq_falling_edge();
  k_sleep(K_MSEC(500));
  soft_nrst_release();
  k_sleep(K_MSEC(NO_REBOOT_MS));
  zassert_equal(reboots, 3, "a press of 0.5 s");

  /* a press whose key-up was missed, then a new one: only the new one counts */
  pmic_emul_raise(MAX77654__REG_INT_GLBL0__FIELD_nEN_F, 0, 0);
  irq_falling_edge();
  k_sleep(K_SECONDS(5));
  pmic_emul_raise(MAX77654__REG_INT_GLBL0__FIELD_nEN_F, 0, 0);
  irq_falling_edge();
  k_sleep(K_MSEC(300));
  soft_nrst_release();
  k_sleep(K_MSEC(NO_REBOOT_MS));
  zassert_equal(reboots, 3, "the new press was 0.3 s");
}

ZTEST(pwr, test_51_key_up_is_handled_only_after_a_good_flag_read) {
  struct pmic_emul *e = pmic_emul_get();

  /* key-down and key-up arrive while the flags cannot be read: the key-up waits for the flags that carry the key-down
   */
  e->fail_read_reg = MAX77654__REG_INT_GLBL0;
  pmic_emul_raise(MAX77654__REG_INT_GLBL0__FIELD_nEN_F, 0, 0);
  irq_falling_edge();
  k_sleep(K_MSEC(300));
  soft_nrst_release();
  k_sleep(K_MSEC(300));
  e->fail_read_reg = -1;
  pwr_refresh();
  k_sleep(K_MSEC(NO_REBOOT_MS));
  zassert_equal(reboots, 3, "a press of 0.3 s, seen late");
  /* The press is over. If the key-up had been used up before its key-down, the next bare key-up would count as a long
   * press. */
  k_sleep(K_SECONDS(5));
  soft_nrst_release();
  k_sleep(K_MSEC(NO_REBOOT_MS));
  zassert_equal(reboots, 3, "a key-up with no key-down is ignored");
}

ZTEST(pwr, test_52_key_down_flag_read_without_a_new_interrupt_takes_the_time_of_the_read) {
  /* An unrelated PMIC interrupt: it sets the interrupt time. */
  irq_falling_edge();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  k_sleep(K_SECONDS(5));
  /* The nEN flag is read on a refresh, with no interrupt of its own (as after pwr_refresh() or on the 20 s pass).
   * It must not take the 5 s old interrupt as the time of the key-down. */
  pmic_emul_raise(MAX77654__REG_INT_GLBL0__FIELD_nEN_F, 0, 0);
  pwr_refresh();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  zassert_false(pmic_emul_pending(), "the flag was read");
  soft_nrst_release(); /* about 0.5 s after the flag was read */
  k_sleep(K_MSEC(NO_REBOOT_MS));
  zassert_equal(reboots, 3, "a short press, not 5 s from a stale interrupt time");
}

ZTEST(pwr, test_53_key_up_of_a_press_older_than_any_real_one_is_ignored) {
  /* A key-down whose key-up was lost, then a bare edge on Soft_nRST long after. The PMIC resets itself after 16 s of
   * nEN, so this was no press. */
  pmic_emul_raise(MAX77654__REG_INT_GLBL0__FIELD_nEN_F, 0, 0);
  irq_falling_edge();
  k_sleep(K_SECONDS(THREAD_PWR_BUTTON_MAX_MS / 1000 + 5));
  soft_nrst_release();
  k_sleep(K_MSEC(NO_REBOOT_MS));
  zassert_equal(reboots, 3, "no power-off for a lost key-up");
}

/* ---- third-party driver (NRF/system/third_party/max77654): register fields ---- */

ZTEST(pwr, test_60_charger_thermistor_thresholds_go_to_their_fields) {
  struct max77654_h *h = pwr_pmic();
  struct max77654_conf saved = h->conf;

  /* Distinct values in all four fields (the defaults are not: cool and cold are both 0b11). */
  h->conf.th_hot = MAX77654_THM_HOT_0V367;   /* 0b01 */
  h->conf.th_warm = MAX77654_THM_WARM_0V411; /* 0b10 */
  h->conf.th_cool = MAX77654_THM_COOL_0V923; /* 0b00 */
  h->conf.th_cold = MAX77654_THM_COLD_0V867; /* 0b11 */
  zassert_ok(k_mutex_lock(&pwr_mutex, K_SECONDS(1)));
  zassert_equal(max77654_config_charger(h), E_MAX77654_SUCCESS);
  k_mutex_unlock(&pwr_mutex);
  /* CNFG_CHG_A: THM_HOT[7:6] THM_WARM[5:4] THM_COOL[3:2] THM_COLD[1:0] (data sheet, register 0x20) */
  zassert_equal(pmic_emul_reg(MAX77654__REG_CNFG_CHG_A), 0x63, "CNFG_CHG_A 0x%02x",
                pmic_emul_reg(MAX77654__REG_CNFG_CHG_A));

  h->conf = saved;
  zassert_ok(k_mutex_lock(&pwr_mutex, K_SECONDS(1)));
  zassert_equal(max77654_config_charger(h), E_MAX77654_SUCCESS);
  k_mutex_unlock(&pwr_mutex);
}

ZTEST(pwr, test_61_erc_flags_wdt_rst_and_wdt_off_are_separate) {
  struct max77654_erc f;
  const struct {
    uint8_t reg;
    bool rst, off;
  } cases[] = {
      {0x00, false, false},
      {MAX77654__REG_ERC_FLAG__FIELD_WDT_RST, true, false},
      {MAX77654__REG_ERC_FLAG__FIELD_WDT_OFF, false, true},
      {MAX77654__REG_ERC_FLAG__FIELD_WDT_RST | MAX77654__REG_ERC_FLAG__FIELD_WDT_OFF, true, true},
  };

  zassert_ok(k_mutex_lock(&pwr_mutex, K_SECONDS(1)));
  for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
    pmic_emul_set_reg(MAX77654__REG_ERC_FLAG, cases[i].reg);
    memset(&f, 0, sizeof(f));
    zassert_equal(max77654_get_erc_flags(pwr_pmic(), &f), E_MAX77654_SUCCESS);
    zassert_equal(f.wdt_rst, cases[i].rst, "ERCFLAG 0x%02x: wdt_rst", cases[i].reg);
    zassert_equal(f.wdt_off, cases[i].off, "ERCFLAG 0x%02x: wdt_off", cases[i].reg);
  }
  /* the other flags keep their bits */
  pmic_emul_set_reg(MAX77654__REG_ERC_FLAG,
                    MAX77654__REG_ERC_FLAG__FIELD_SFT_CRST_F | MAX77654__REG_ERC_FLAG__FIELD_TOVLD);
  zassert_equal(max77654_get_erc_flags(pwr_pmic(), &f), E_MAX77654_SUCCESS);
  zassert_true(f.sft_crst_f && f.tovld && !f.wdt_rst && !f.wdt_off && !f.mrst);
  k_mutex_unlock(&pwr_mutex);
}

/* Last: the power thread parks in the reset. */
ZTEST(pwr, test_99_long_press_powers_off_and_reboots) {
  zassert_ok(gpio_pin_set_dt(&led_pwr, 1)); /* the earlier power-off tests switched it off */
  zassert_equal(pmic_emul_sbb_mv(0), 3300);
  k_sem_reset(&reboot_sem);
  pmic_emul_raise(MAX77654__REG_INT_GLBL0__FIELD_nEN_F, 0, 0);
  irq_falling_edge();
  k_sleep(K_MSEC(2500));
  zassert_equal(reboots, 3, "not before the key-up");
  soft_nrst_release();
  zassert_ok(k_sem_take(&reboot_sem, K_SECONDS(2)), "a press of more than 2 s");
  zassert_equal(reboots, 4);
  zassert_equal(reboot_type, SYS_REBOOT_COLD);
  zassert_equal(gpio_emul_output_get_dt(&led_pwr), 0);
  for (int i = 0; i < 3; i++) {
    zassert_equal(pmic_emul_sbb_mv(i), 0);
  }
  zassert_equal(pmic_emul_ldo_mv(0), 0);
}

#endif /* NORMAL */

#if defined(CONFIG_SENSEI_TEST_BOOT_FAIL)

ZTEST(pwr, test_01_hook_failed_but_the_gap9_lines_are_inactive) {
  struct pmic_emul *e = pmic_emul_get();

  zassert_true(snap.first_access_seen);
  zassert_true(snap.first_access_all_inactive, "before the PMIC is touched");
  zassert_true(snap.lines_inactive_after);
  zassert_equal(snap.resets_after, 0);
  zassert_true(e->nacks > 0);
  for (int i = 0; i < 3; i++) {
    zassert_equal(snap.sbb_mv[i], 0);
  }
  zassert_false(snap.led_output_after, "no LED supply without the PMIC");
}

ZTEST(pwr, test_02_init_tries_again_and_fails_while_the_pmic_is_gone) {
  int64_t t0 = k_uptime_get();

  zassert_equal(pwr_init(), -1);
  zassert_true(k_uptime_get() - t0 >= 300, "the retry waits for the reset like the hook");
  zassert_equal(pwr_bsp_init_fake.call_count, 0, "no board hook without the PMIC");
  zassert_equal(atomic_get(&pwr_is_initialised), 0);
}

ZTEST(pwr, test_03_init_refuses_another_chip) {
  struct pmic_emul *e = pmic_emul_get();

  e->present = true;
  e->cid = 0x01; /* the driver wants variant 0x02 */
  zassert_equal(pwr_init(), -1);
  zassert_equal(e->resets, 1);
  for (int i = 0; i < 3; i++) {
    zassert_equal(pmic_emul_sbb_mv(i), 0, "the rails stay off");
  }
  zassert_equal(e->wcount[MAX77654__REG_CNFG_SBB0_B], 0, "nothing configured");
  zassert_equal(atomic_get(&pwr_is_initialised), 0);
}

ZTEST(pwr, test_04_init_brings_the_pmic_up_when_it_answers) {
  struct pmic_emul *e = pmic_emul_get();

  e->cid = 0x02;
  pwr_bsp_init_fake.return_val = 0;
  set_battery_mv(OK_MV + 100);
  zassert_ok(pwr_init());
  check_irq_pins_are_inputs();
  zassert_equal(e->resets, 2);
  check_rails_on((int[]){pmic_emul_sbb_mv(0), pmic_emul_sbb_mv(1), pmic_emul_sbb_mv(2)},
                 (int[]){pmic_emul_ldo_mv(0), pmic_emul_ldo_mv(1)});
  zassert_true(is_output(&led_pwr));
  zassert_equal(gpio_emul_output_get_dt(&led_pwr), 1);
  zassert_equal(pwr_bsp_init_fake.call_count, 1);
  zassert_equal(atomic_get(&pwr_is_initialised), 1);
  zassert_within(pwr_bat_mV(), OK_MV + 100, 15);
}

#endif /* BOOT_FAIL */

#if defined(CONFIG_SENSEI_TEST_NO_RAILS)

ZTEST(pwr, test_01_hook_resets_the_pmic_but_leaves_the_rails_off) {
  struct pmic_emul *e = pmic_emul_get();

  zassert_equal(snap.resets_after, 1);
  for (int i = 0; i < 3; i++) {
    zassert_equal(snap.sbb_mv[i], 0, "SBB%d", i);
    zassert_equal(pmic_emul_sbb_mv(i), 0);
  }
  zassert_equal(snap.ldo_mv[0], 0);
  zassert_equal(e->early, 0);
  zassert_true(snap.first_access_all_inactive);
  zassert_true(snap.led_on_after, "the LED supply is switched on in any case");
  zassert_true(snap.t_after - snap.last_write_after < 50, "no settle time without the rails");
}

#endif /* NO_RAILS */
