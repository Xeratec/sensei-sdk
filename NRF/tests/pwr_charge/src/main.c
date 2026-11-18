/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The charge counter of NRF/system/pwr (pwr_charge.c, lib/sensei_charge) with the real MAX77654 driver and BSP on the
 * I2C emulator of the PMIC of the pwr test. The ADC emulator answers what the AMUX would show: the battery voltage, the
 * discharge NULL and DISCHG readings of a chosen load current, the charge current. Time is the simulated clock of
 * native_sim (hours pass at once). The retained RAM is a RAM array, the settings are on the flash simulator.
 *
 * Scenarios (Kconfig of this folder): the counter starts once per boot, so what it finds at the start has its own
 * build. NORMAL: no record (first start), then the counter runs: accounting, voltage scaling, floor watchdog, external
 * power, AMUX checks (order of the conversions, the uA result), the save policy, the retained record, a new battery,
 * the button (no declaration without a power-on reset); with the Li-ion choice also charge integration, "charger done"
 * and the learned capacity. RETAINED, SETTINGS, SWAP: restore precedence and the swap rules; SWAP also declares a new
 * battery with the button. The tests of a scenario continue each other: keep the numbers. */

#include <string.h>

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/fff.h>
#include <zephyr/retention/retention.h>
#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

#include "bsp/pwr_bsp.h"
#include "emul_max77654.h"
#include "max77654.h"
#include "max77654_reg.h"
#include "pwr/pwr.h"
#include "pwr/pwr_charge.h"
#include "pwr/pwr_priv.h"
#include "sensei_charge.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, pwr_bsp_init);
FAKE_VALUE_FUNC(int, pwr_bsp_start);

#define PIN_IRQ 5
#define PIN_SOFT_NRST 6
#define THREAD_PASS_MS 500
#define PASS_S 21 /* a pass of the power thread every 20 s */

static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *const adc0 = DEVICE_DT_GET(DT_NODELABEL(adc0));
static const struct device *const ret_dev = DEVICE_DT_GET(DT_CHOSEN(sensei_charge));

/* ---- the table of the test: big numbers, so that whole mAh show ---- */

enum { ST_A, ST_B, ST_N };
enum { EV_X, EV_Y, EV_N };

static const uint32_t state_ua[ST_N] = {3600000, 1000};   /* A: 3.6 A = 1 mAh per second */
static const uint32_t event_nc[EV_N] = {3600000000u, 50}; /* X: 1 mAh per count */
static const uint32_t check_ua[2] = {19000, 0};           /* + floor 1000 uA: 20 mA expected */
static const struct sensei_charge_table table = {ST_N, EV_N, 2, state_ua, event_nc, check_ua};

#define FLOOR_UA 1000

/* the table is at 3.6 V: at this voltage the counter does not scale */
#define FULL_MV 3600
#define LEARN_MV 3250

/* ---- the AMUX as the ADC emulator sees it ---- */

static uint32_t vbat_mv = FULL_MV;
static uint32_t load_ua; /* battery discharge current */
static uint32_t chg_pct; /* charge current, percent of the fast charge current */
static const uint32_t null_mv = 20;
static uint8_t seq[16];
static int seq_n;

static uint32_t imon_scale_ua(void) {
  static const uint32_t scale[] = {8200, 40500, 72300, 103400, 134100, 164100, 193700, 222700, 251200, 279300};
  uint8_t sel = pmic_emul_reg(MAX77654__REG_CNFG_CHG_I) >> 4;

  return sel < ARRAY_SIZE(scale) ? scale[sel] : 300000;
}

static int adc_fn(const struct device *dev, unsigned int chan, void *param, uint32_t *result) {
  uint8_t mux = (uint8_t)pmic_emul_amux();

  ARG_UNUSED(dev);
  ARG_UNUSED(chan);
  ARG_UNUSED(param);
  if (seq_n < (int)sizeof(seq)) {
    seq[seq_n++] = mux;
  }
  switch (mux) {
  case MAX77654_BATT_V:
    *result = vbat_mv * 272 / 1000; /* the AMUX divides the battery voltage by 0.272 */
    break;
  case MAX77654_BATT_I_CHG:
    *result = chg_pct * 1250 / 100;
    break;
  case MAX77654_BATT_I_NULL:
    *result = null_mv;
    break;
  case 5: /* BATT_DISCHG_I */
    *result = null_mv + (uint32_t)((uint64_t)load_ua * (1250 - null_mv) / imon_scale_ua());
    break;
  default:
    *result = 0;
  }
  return 0;
}

static void __maybe_unused set_battery_mv(uint32_t mv) { vbat_mv = mv; }

static void __maybe_unused irq_falling_edge(void) {
  zassert_ok(gpio_emul_input_set(gpio0, PIN_IRQ, 0));
  zassert_ok(gpio_emul_input_set(gpio0, PIN_IRQ, 1));
}

static void __maybe_unused soft_nrst_release(void) {
  zassert_ok(gpio_emul_input_set(gpio0, PIN_SOFT_NRST, 0));
  zassert_ok(gpio_emul_input_set(gpio0, PIN_SOFT_NRST, 1));
}

/* ---- records ---- */

#define KEY_REC "sensei_charge/rec"
#define KEY_LEDGER "sensei_charge/ledger"

static void __maybe_unused make_rec(uint32_t used_mah, uint32_t last_vbat, uint32_t id, uint8_t *out) {
  struct sensei_charge c;

  sensei_charge_init(&c, &table,
                     IS_ENABLED(CONFIG_SENSEI_PWR_BATTERY_LIION) ? SENSEI_CHARGE_LIION : SENSEI_CHARGE_LISOCL2, 17000);
  sensei_charge_set_params(&c, 17000, 90, FLOOR_UA, 0);
  sensei_charge_tick(&c, 0);
  sensei_charge_set_vbat(&c, 0, last_vbat);
  sensei_charge_new(&c, 0, 17000, used_mah, id);
  sensei_charge_pack(&c, 1780000000u, out);
}

struct stored {
  bool rec_ok;
  struct sensei_charge_record rec;
  size_t ledger_len;
};

static int stored_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param) {
  struct stored *s = param;
  uint8_t buf[SENSEI_CHARGE_LEDGER_MAX_BYTES];

  if (len > sizeof(buf) || read_cb(cb_arg, buf, len) != (ssize_t)len) {
    return 0;
  }
  if (strcmp(key, "rec") == 0) {
    s->rec_ok = len == SENSEI_CHARGE_RECORD_BYTES && sensei_charge_unpack(buf, &s->rec) == 0;
  } else if (strcmp(key, "ledger") == 0) {
    s->ledger_len = len;
  }
  return 0;
}

static struct stored read_stored(void) {
  struct stored s = {0};

  zassert_ok(settings_load_subtree_direct("sensei_charge", stored_cb, &s));
  return s;
}

static uint32_t wall(void) { return 1780000000u + (uint32_t)(k_uptime_get() / 1000); }

/* ---- scenarios: what is in the retained RAM and the settings at the start ---- */

static bool por_reset;

static void *setup(void) {
  uint8_t rec[SENSEI_CHARGE_RECORD_BYTES] __maybe_unused;

  zassert_ok(adc_emul_value_func_set(adc0, 0, adc_fn, NULL));
  zassert_ok(settings_subsys_init());
  /* the flash simulator keeps its file between runs */
  (void)settings_delete(KEY_REC);
  (void)settings_delete(KEY_LEDGER);
  zassert_ok(retention_clear(ret_dev));

#if defined(CONFIG_SENSEI_TEST_CHARGE_RETAINED)
  make_rec(100, FULL_MV, 11, rec);
  zassert_ok(retention_write(ret_dev, 0, rec, sizeof(rec)));
  make_rec(50, FULL_MV, 22, rec);
  zassert_ok(settings_save_one(KEY_REC, rec, sizeof(rec)));
#elif defined(CONFIG_SENSEI_TEST_CHARGE_SETTINGS)
  make_rec(100, FULL_MV, 11, rec);
  rec[10] ^= 0xFF; /* damaged: the CRC does not fit */
  zassert_ok(retention_write(ret_dev, 0, rec, sizeof(rec)));
  make_rec(50, FULL_MV, 22, rec);
  zassert_ok(settings_save_one(KEY_REC, rec, sizeof(rec)));
  por_reset = true;
#elif defined(CONFIG_SENSEI_TEST_CHARGE_SWAP)
  make_rec(900, 3400, 22, rec); /* the last voltage was 3400 mV ... */
  zassert_ok(settings_save_one(KEY_REC, rec, sizeof(rec)));
  vbat_mv = 3400 + 160; /* ... the new cell rests at 3560 */
  por_reset = true;
#endif
  return NULL;
}

ZTEST_SUITE(charge, NULL, setup, NULL, NULL, NULL);

static struct pwr_charge_cfg cfg(void) {
  return (struct pwr_charge_cfg){.table = &table,
                                 .capacity_mah = 0,
                                 .usable_pct = 0,
                                 .floor_ua = FLOOR_UA,
                                 .addon_ua = 0,
                                 .power_on_reset = por_reset,
                                 .brownout_reset = false,
                                 .wall_s = wall};
}

static struct sensei_charge_report report(void) {
  struct sensei_charge_report r;

  zassert_ok(pwr_charge_report(&r));
  return r;
}

static void __maybe_unused passes(int n) {
  for (int i = 0; i < n; i++) {
    k_sleep(K_SECONDS(PASS_S));
  }
}

/* ---- start ---- */

ZTEST(charge, test_01_nothing_counts_before_the_start) {
  struct sensei_charge_report r;
  uint32_t s[ST_N], e[EV_N];

  pwr_bsp_init_fake.return_val = 0;
  pwr_bsp_start_fake.return_val = 0;
  zassert_ok(pwr_init());
  zassert_ok(pwr_start());
  k_sleep(K_MSEC(THREAD_PASS_MS));
  pwr_charge_state(ST_A, true); /* ignored, no crash */
  pwr_charge_event(EV_X, 5);
  pwr_charge_set_temp(250);
  zassert_equal(pwr_charge_report(&r), -EAGAIN);
  zassert_equal(pwr_charge_ledger(s, e), -EAGAIN);
  zassert_equal(pwr_charge_save(), -EAGAIN);
  zassert_equal(pwr_charge_check(0), -EAGAIN);
  zassert_equal(pwr_charge_new(1, 1, 1), -EAGAIN);
}

ZTEST(charge, test_02_start) {
  struct pwr_charge_cfg c = cfg();
  struct sensei_charge_report r;
  struct stored st;
  uint8_t ret[SENSEI_CHARGE_RECORD_BYTES];
  struct sensei_charge_record rec;

  zassert_equal(pwr_charge_init(NULL), -EINVAL);
  zassert_ok(pwr_charge_init(&c));
  zassert_equal(pwr_charge_init(&c), -EALREADY);
  r = report();

#if defined(CONFIG_SENSEI_TEST_CHARGE_RETAINED)
  /* the retained record is newer than the settings record */
  zassert_equal(r.used_mah, 100);
  zassert_equal(r.battery_id, 11);
  zassert_equal(r.flags & ~SENSEI_CHARGE_F_CAPACITY_DEFAULT, 0, "no flag: flags 0x%x", r.flags);
  zassert_equal(r.capacity_mah, 17000);
#elif defined(CONFIG_SENSEI_TEST_CHARGE_SETTINGS)
  /* the retained RAM is damaged: the settings record, with RESTORED; the power-on reset adds UNCONFIRMED */
  zassert_equal(r.used_mah, 50);
  zassert_equal(r.battery_id, 22);
  zassert_equal(r.flags & (SENSEI_CHARGE_F_RESTORED | SENSEI_CHARGE_F_UNCONFIRMED),
                SENSEI_CHARGE_F_RESTORED | SENSEI_CHARGE_F_UNCONFIRMED, "flags 0x%x", r.flags);
  zassert_false(r.flags & SENSEI_CHARGE_F_AUTO_SWAP);
  /* up to 6 h at the floor (1 mA in this test, 6 mAh) may be lost: that is in the error (1 sigma 7 mAh with 7 % of 50)
   */
  zassert_within(r.err_mah, 7, 1, "err %u mAh", r.err_mah);
#elif defined(CONFIG_SENSEI_TEST_CHARGE_SWAP)
  /* 160 mV above the last voltage after a power-on reset: a new battery, the old count is gone */
  zassert_equal(r.used_mah, 0);
  zassert_equal(r.battery_id, 0);
  zassert_true(r.flags & SENSEI_CHARGE_F_AUTO_SWAP, "flags 0x%x", r.flags);
  zassert_false(r.flags & (SENSEI_CHARGE_F_UNCONFIRMED | SENSEI_CHARGE_F_RESTORED));
#else
  /* first start: no record at all */
  zassert_equal(r.used_mah, 0);
  zassert_true(r.flags & SENSEI_CHARGE_F_UNCONFIRMED);
  zassert_true(r.flags & SENSEI_CHARGE_F_CAPACITY_DEFAULT);
#ifdef CONFIG_SENSEI_PWR_BATTERY_LIION
  zassert_equal(r.capacity_mah, 0);
  zassert_equal(r.pct, SENSEI_CHARGE_PCT_UNKNOWN);
#else
  zassert_equal(r.capacity_mah, 17000);
  zassert_equal(r.pct, 100);
#endif
#endif

  /* the start writes both records: the retained RAM and the settings */
  st = read_stored();
  zassert_true(st.rec_ok, "settings record");
  zassert_true(st.ledger_len > 0, "ledger");
  zassert_equal(st.rec.used_uc / 3600000u, r.used_mah);
  zassert_ok(retention_read(ret_dev, 0, ret, sizeof(ret)));
  zassert_ok(sensei_charge_unpack(ret, &rec), "retained record");
  zassert_equal(rec.used_uc / 3600000u, r.used_mah);
}

#ifdef CONFIG_SENSEI_TEST_CHARGE_SWAP

/* ---- the swap button: three short presses within 3 s, in the first 5 min after a power-on reset ---- */

static int swaps;
static void swap_cb(void) { swaps++; }

static void press(int hold_ms) {
  pmic_emul_raise(MAX77654__REG_INT_GLBL0__FIELD_nEN_F, 0, 0);
  irq_falling_edge();
  k_sleep(K_MSEC(hold_ms));
  soft_nrst_release();
  k_sleep(K_MSEC(THREAD_PASS_MS));
}

ZTEST(charge, test_10_button_three_presses_declare_a_new_battery) {
  struct sensei_charge_report r;

  pwr_charge_set_swap_cb(swap_cb);
  pwr_charge_event(EV_X, 30); /* something is counted */
  zassert_equal(report().used_mah, 30);

  /* two presses are not enough */
  press(100);
  press(100);
  zassert_equal(swaps, 0);
  zassert_equal(report().used_mah, 30);
  /* too slow: the first press falls out of the 3 s window */
  k_sleep(K_SECONDS(4));
  press(100);
  zassert_equal(swaps, 0, "the third press came more than 3 s after the first of the three");
  /* the third within 3 s of the other two */
  press(100);
  press(100);
  zassert_equal(swaps, 1, "swap callback");
  r = report();
  zassert_equal(r.used_mah, 0, "reset");
  zassert_equal(r.flags & SENSEI_CHARGE_F_AUTO_SWAP, 0, "a declaration clears the flags");
  zassert_equal(r.flags & SENSEI_CHARGE_F_UNCONFIRMED, 0);
  zassert_true(r.battery_id >= 1780000000u, "the id is the wall time");
  /* saved to the settings (by the work queue) */
  k_sleep(K_MSEC(200));
  zassert_equal(read_stored().rec.battery_id, r.battery_id);
}

#endif /* SWAP */

#if !defined(CONFIG_SENSEI_TEST_CHARGE_RETAINED) && !defined(CONFIG_SENSEI_TEST_CHARGE_SETTINGS) &&                    \
    !defined(CONFIG_SENSEI_TEST_CHARGE_SWAP)

/* ---- accounting ---- */

ZTEST(charge, test_03_states_events_and_ledger) {
  uint32_t s[ST_N], e[EV_N];
  uint32_t before = report().used_mah;

  pwr_charge_event(EV_X, 3);
  pwr_charge_state(ST_A, true);
  k_sleep(K_SECONDS(10));
  pwr_charge_state(ST_A, true); /* a second user: stays on */
  pwr_charge_state(ST_A, false);
  k_sleep(K_SECONDS(10));
  pwr_charge_state(ST_A, false);
  /* 3 mAh of events and 20 s of 3.6 A = 20 mAh; the floor adds a few mC */
  zassert_within(report().used_mah - before, 23, 1);
  zassert_ok(pwr_charge_ledger(s, e));
  zassert_within(s[ST_A], 20, 1);
  zassert_equal(s[ST_B], 0);
  zassert_equal(e[EV_X], 3);
}

ZTEST(charge, test_04_the_battery_voltage_scales_the_charge) {
  uint32_t before;

  /* 3.0 V: 1 mAh at 3.6 V costs 1.2 mAh. The voltage reaches the counter at the next pass of the power thread. */
  set_battery_mv(3000);
  pwr_refresh();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  before = report().used_mah;
  pwr_charge_event(EV_X, 5);
  zassert_within(report().used_mah - before, 6, 1);
  set_battery_mv(FULL_MV);
  pwr_refresh();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  before = report().used_mah;
  pwr_charge_event(EV_X, 5);
  zassert_within(report().used_mah - before, 5, 1);
}

ZTEST(charge, test_05_floor_watchdog_counts_an_idle_current_that_is_too_high) {
  uint32_t before;
  struct sensei_charge_report r;

  /* idle at 5 mA, the floor is 1 mA and the margin 2 mA: high. Three samples in a row set the flag. */
  load_ua = 5000;
  passes(2);
  zassert_false(report().flags & SENSEI_CHARGE_F_FLOOR_HIGH, "two samples are not enough");
  passes(2);
  zassert_true(report().flags & SENSEI_CHARGE_F_FLOOR_HIGH);
  /* the measured current is counted: one hour at 5 mA is 5 mAh instead of 1 */
  before = report().used_mah;
  k_sleep(K_HOURS(1));
  zassert_within(report().used_mah - before, 5, 1);
  /* the load is normal again: the flag clears after three samples and the floor counts again */
  load_ua = 1000;
  passes(4);
  zassert_false(report().flags & SENSEI_CHARGE_F_FLOOR_HIGH);
  before = report().used_mah;
  k_sleep(K_HOURS(1));
  r = report();
  zassert_within(r.used_mah - before, 1, 1);
  /* a state that is on stops the sample: nothing is measured, nothing changes */
  load_ua = 8000;
  pwr_charge_state(ST_B, true);
  passes(5);
  zassert_false(report().flags & SENSEI_CHARGE_F_FLOOR_HIGH);
  pwr_charge_state(ST_B, false);
  load_ua = 1000;
}

ZTEST(charge, test_06_external_power_stops_the_count) {
  struct sensei_charge_report r;
  uint32_t before, s[ST_N], e[EV_N];

  /* USB is in (CHGIN valid, the primary cell is not charged): the battery does not supply the node */
  pmic_emul_set_charger(MAX77654_CHG_DTLS_OFF, MAX77654_CHGIN_DTLS_OK, false);
  pwr_refresh();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  before = report().used_mah;
  zassert_ok(pwr_charge_ledger(s, e));
  uint32_t ev_before = e[EV_X];
  pwr_charge_event(EV_X, 4);
  pwr_charge_state(ST_A, true);
  k_sleep(K_SECONDS(5));
  pwr_charge_state(ST_A, false);
  k_sleep(K_HOURS(2));
  r = report();
  /* only the self-discharge of a primary cell (19 uA), a Li-ion cell nothing */
  zassert_within(r.used_mah, before, 0, "used %u, before %u", r.used_mah, before);
  zassert_ok(pwr_charge_ledger(s, e));
  zassert_equal(e[EV_X], ev_before + 4, "the ledger counts");
  /* the AMUX check does not run (the battery does not supply the node) */
  zassert_equal(pwr_charge_check(0), -EBUSY);
  /* USB out: counting again */
  pmic_emul_set_charger(MAX77654_CHG_DTLS_OFF, MAX77654_CHGIN_DTLS_UVLO, false);
  pwr_refresh();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  before = report().used_mah;
  pwr_charge_event(EV_X, 2);
  zassert_within(report().used_mah - before, 2, 1);
}

/* ---- AMUX samples ---- */

ZTEST(charge, test_07_check_converts_null_first_then_dischg_and_reads_microamps) {
  uint32_t ua = 0;
  int64_t t0;

  /* the driver on its own: 20 mA at the 40.5 mA scale, ADC step of the 12 bit SAADC: 211 uA */
  load_ua = 20000;
  seq_n = 0;
  t0 = k_uptime_get();
  zassert_equal(max77654_measure_dischg_ua(&pmic_h, MAX77654_BATT_I_40MA5, &ua), E_MAX77654_SUCCESS);
  zassert_within(ua, 20000, 250);
  zassert_equal(seq_n, 2, "two conversions");
  zassert_equal(seq[0], MAX77654_BATT_I_NULL, "NULL first");
  zassert_equal(seq[1], 5, "then DISCHG");
  zassert_equal(pmic_emul_amux(), 0, "AMUX off after the measurement");
  zassert_true(k_uptime_get() - t0 < 1, "no 1 ms wait between the conversions");
  /* full resolution at the small scale: 1.5 mA is 1.5 mA, not 0 % */
  load_ua = 1500;
  zassert_equal(max77654_measure_dischg_ua(&pmic_h, MAX77654_BATT_I_8MA2, &ua), E_MAX77654_SUCCESS);
  zassert_within(ua, 1500, 12);
  /* the integer percent of the old function: 8.2 mA scale, 1500 uA is 18 % */
  uint32_t pct = 0;
  zassert_equal(max77654_measure(&pmic_h, MAX77654_BATT_I_8MA2, &pct), E_MAX77654_SUCCESS);
  zassert_within(pct, 18, 1);
  zassert_equal(pmic_emul_amux(), 0);
  /* below the null reading: 0 */
  load_ua = 0;
  zassert_equal(max77654_measure_dischg_ua(&pmic_h, MAX77654_BATT_I_8MA2, &ua), E_MAX77654_SUCCESS);
  zassert_equal(ua, 0);

  /* the call of the application: the order and the AMUX off again */
  load_ua = 20000;
  seq_n = 0;
  zassert_ok(pwr_charge_check(0));
  zassert_equal(seq_n, 2);
  zassert_equal(seq[0], MAX77654_BATT_I_NULL);
  zassert_equal(seq[1], 5);
  zassert_equal(pmic_emul_amux(), 0);
  /* the PMIC mutex is free again */
  zassert_ok(k_mutex_lock(&pwr_mutex, K_NO_WAIT));
  k_mutex_unlock(&pwr_mutex);
  zassert_equal(pwr_charge_check(1), -ENOTSUP, "class 1 has no expected current");
  zassert_equal(pwr_charge_check(9), -ENOTSUP);
}

ZTEST(charge, test_08_check_flags_a_table_that_is_off) {
  /* 40 % too much, over many samples: CHECK_MISMATCH */
  load_ua = 28000;
  for (int i = 0; i < 250; i++) {
    zassert_ok(pwr_charge_check(0));
  }
  zassert_true(report().flags & SENSEI_CHARGE_F_CHECK_MISMATCH);
  /* the table fits again */
  load_ua = 20000;
  for (int i = 0; i < 1500; i++) {
    zassert_ok(pwr_charge_check(0));
  }
  zassert_false(report().flags & SENSEI_CHARGE_F_CHECK_MISMATCH);
  load_ua = 1000; /* the idle current again: the floor watchdog must not see 20 mA while nothing runs */
  passes(4);
  zassert_false(report().flags & SENSEI_CHARGE_F_FLOOR_HIGH);
}

/* ---- records ---- */

ZTEST(charge, test_09_the_retained_record_follows_every_pass) {
  uint8_t ret[SENSEI_CHARGE_RECORD_BYTES];
  struct sensei_charge_record rec;
  struct sensei_charge_report r;

  pwr_charge_event(EV_X, 7);
  passes(1);
  r = report();
  zassert_ok(retention_read(ret_dev, 0, ret, sizeof(ret)));
  zassert_ok(sensei_charge_unpack(ret, &rec));
  zassert_within(rec.used_uc / 3600000u, r.used_mah, 1);
  zassert_equal(rec.capacity_mah, r.capacity_mah);
}

ZTEST(charge, test_10_settings_are_saved_every_six_hours_and_on_request) {
  struct stored a, b;
  struct sensei_charge_report r;

  zassert_ok(pwr_charge_save());
  a = read_stored();
  zassert_true(a.rec_ok);
  zassert_within(a.rec.used_uc / 3600000u, report().used_mah, 1);

  /* five hours: not saved yet (the floor adds up in RAM) */
  pwr_charge_event(EV_X, 7);
  k_sleep(K_HOURS(5));
  b = read_stored();
  zassert_equal(b.rec.used_uc, a.rec.used_uc, "not before CONFIG_SENSEI_PWR_CHARGE_SAVE_H hours");
  /* after the sixth: saved, with the 7 mAh of the event */
  k_sleep(K_MINUTES(61));
  b = read_stored();
  r = report();
  zassert_true(b.rec.used_uc >= a.rec.used_uc + 7ull * 3600000ull, "saved after 6 h");
  zassert_within(b.rec.used_uc / 3600000u, r.used_mah, 1);
  zassert_true(b.ledger_len > 0);
}

ZTEST(charge, test_11_a_new_battery_restarts_the_counter_and_is_saved_at_once) {
  struct sensei_charge_report r;
  struct stored st;

  zassert_ok(pwr_charge_new(8000, 1000, 77));
  r = report();
  zassert_equal(r.capacity_mah, 8000);
  zassert_within(r.used_mah, 1000, 1);
  zassert_equal(r.battery_id, 77);
  zassert_equal(r.flags & (SENSEI_CHARGE_F_UNCONFIRMED | SENSEI_CHARGE_F_CAPACITY_DEFAULT | SENSEI_CHARGE_F_RESTORED),
                0, "flags 0x%x", r.flags);
  st = read_stored();
  zassert_equal(st.rec.battery_id, 77);
  zassert_equal(st.rec.capacity_mah, 8000);
  zassert_within(st.rec.used_uc / 3600000u, 1000, 1);
  /* the capacity setting of 0 does not take the declared capacity back */
  pwr_charge_configure(0, 80, FLOOR_UA, 0);
  zassert_equal(report().capacity_mah, 8000);
  pwr_charge_configure(9000, 80, FLOOR_UA, 0);
  zassert_equal(report().capacity_mah, 9000);
}

ZTEST(charge, test_12_the_button_declares_no_battery_without_a_power_on_reset) {
  pwr_charge_set_swap_cb(NULL);
  for (int i = 0; i < 3; i++) {
    pmic_emul_raise(MAX77654__REG_INT_GLBL0__FIELD_nEN_F, 0, 0);
    irq_falling_edge();
    k_sleep(K_MSEC(100));
    soft_nrst_release();
    k_sleep(K_MSEC(THREAD_PASS_MS));
  }
  zassert_equal(report().battery_id, 77, "the battery was not replaced");
}

#ifndef CONFIG_SENSEI_PWR_BATTERY_LIION

ZTEST(charge, test_13_a_voltage_under_the_critical_level_marks_the_end_of_life) {
  struct sensei_charge_report r;

  /* a primary cell under 2.9 V (the voltage of the pass, under its load) for three passes: VOLTAGE_EOL */
  zassert_ok(pwr_charge_new(1000, 100, 8));
  set_battery_mv(2850);
  passes(2);
  zassert_false(report().flags & SENSEI_CHARGE_F_VOLTAGE_EOL, "two passes are no verdict");
  set_battery_mv(FULL_MV);
  passes(2);
  set_battery_mv(2850);
  passes(4);
  r = report();
  zassert_true(r.flags & SENSEI_CHARGE_F_VOLTAGE_EOL, "flags 0x%x", r.flags);
  zassert_true(r.pct <= 5 && r.days_left == 0, "the percentage cannot be trusted any more: %u %%, %u days", r.pct,
               r.days_left);
  /* it stays after the voltage comes back, until a new battery */
  set_battery_mv(FULL_MV);
  passes(2);
  zassert_true(report().flags & SENSEI_CHARGE_F_VOLTAGE_EOL);
  zassert_ok(pwr_charge_new(1000, 0, 9));
  zassert_false(report().flags & SENSEI_CHARGE_F_VOLTAGE_EOL);
}

#endif

#ifdef CONFIG_SENSEI_PWR_BATTERY_LIION

/* ---- Li-ion: charge current, charger done, learned capacity ---- */

ZTEST(charge, test_20_charge_current_reduces_the_used_charge) {
  uint32_t before;

  zassert_ok(pwr_charge_new(1000, 500, 5));
  /* charging at 50 % of the fast-charge current (15 mA): 7.5 mA */
  chg_pct = 50;
  pmic_emul_set_charger(MAX77654_CHG_DTLS_CC, MAX77654_CHGIN_DTLS_OK, true);
  pwr_refresh();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  before = report().used_mah;
  k_sleep(K_HOURS(1));
  /* the battery pays nothing while the input supplies the node: only the charge counts, 7.5 mAh in an hour */
  zassert_within(before - report().used_mah, 7, 1);
}

ZTEST(charge, test_21_charger_done_resets_the_counter) {
  struct sensei_charge_report r;

  zassert_true(report().used_mah > 0);
  pmic_emul_set_charger(MAX77654_CHG_DTLS_DONE, MAX77654_CHGIN_DTLS_OK, false);
  chg_pct = 0;
  pwr_refresh();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  r = report();
  zassert_equal(r.used_mah, 0);
  zassert_true(r.flags & SENSEI_CHARGE_F_LIION_SYNCED);
  zassert_equal(r.pct, 100);
  k_sleep(K_MSEC(200));
  zassert_equal(read_stored().rec.used_uc, 0, "saved");
}

ZTEST(charge, test_22_the_capacity_is_learned_from_a_full_discharge) {
  struct sensei_charge_report r;

  /* USB out, the cell is used up to 800 mAh and reaches 3.3 V: that is its capacity (800 of 1000 known). Some activity
   * first: after 30 minutes without any, the counter would be pulled to the open-circuit voltage (3.6 V says 19 %). */
  pwr_charge_state(ST_B, true);
  pwr_charge_state(ST_B, false);
  pmic_emul_set_charger(MAX77654_CHG_DTLS_OFF, MAX77654_CHGIN_DTLS_UVLO, false);
  pwr_refresh();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  pwr_charge_event(EV_X, 800);
  set_battery_mv(LEARN_MV);
  pwr_refresh();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  r = report();
  zassert_within(r.capacity_mah, 800, 2, "capacity %u mAh, used %u mAh", r.capacity_mah, r.used_mah);
  zassert_false(r.flags & SENSEI_CHARGE_F_LIION_SYNCED, "learned once per full charge");
}

ZTEST(charge, test_23_the_voltage_pulls_the_counter_after_half_an_hour_without_load) {
  struct sensei_charge_report r;
  uint32_t before;

  /* the cell is at 3.6 V (19 % by the Li-ion table), the counter says 500 of 1000 mAh used: after 30 minutes without a
   * state or an event the next pass moves the counter towards 81 % used, by 1/8 of the difference */
  zassert_ok(pwr_charge_new(1000, 500, 6));
  set_battery_mv(3600);
  pwr_refresh();
  k_sleep(K_MSEC(THREAD_PASS_MS));
  before = report().used_mah;
  k_sleep(K_MINUTES(40));
  r = report();
  zassert_true(r.used_mah > before + 30, "pulled up: %u after %u mAh", r.used_mah, before);
  zassert_true(r.used_mah < before + 80, "a step of 1/8 of the difference (810 - 500 = 310 mAh): %u", r.used_mah);
}

#endif /* LIION */

#endif /* NORMAL */
