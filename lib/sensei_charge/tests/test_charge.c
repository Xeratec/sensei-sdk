/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "sensei_charge.h"

static int fails;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                              \
      fails++;                                                                                                         \
    }                                                                                                                  \
  } while (0)
/* a and b equal within pct percent */
#define NEAR(a, b, pct)                                                                                                \
  CHECK((double)(a) >= (double)(b) * (1.0 - (pct) / 100.0) && (double)(a) <= (double)(b) * (1.0 + (pct) / 100.0))

enum { ST_A, ST_B, ST_N };
enum { EV_X, EV_Y, EV_N };

static const uint32_t state_ua[ST_N] = {10000, 2000};
static const uint32_t event_nc[EV_N] = {1000000, 50};
static const uint32_t check_ua[2] = {20000, 0};
static const struct sensei_charge_table table = {ST_N, EV_N, 2, state_ua, event_nc, check_ua};

static struct sensei_charge ch;

static void fresh(enum sensei_charge_chem chem, uint32_t floor_ua) {
  sensei_charge_init(&ch, &table, chem, 17000);
  sensei_charge_set_params(&ch, 0, 90, floor_ua, 0);
  sensei_charge_tick(&ch, 0);
}

static double used_ua_s(void) { return (double)sensei_charge_used_uc(&ch); }

static void test_states(void) {
  fresh(SENSEI_CHARGE_LIION, 100); /* no self-discharge */
  /* floor only: 100 uA for 1000 s = 100000 uC */
  sensei_charge_tick(&ch, 1000000);
  CHECK(sensei_charge_used_uc(&ch) == 100000);
  /* state A on for 10 s: 10000 uA x 10 s = 100000 uC more */
  sensei_charge_state(&ch, 1000000, ST_A, true);
  sensei_charge_state(&ch, 1010000, ST_A, false);
  /* the floor of those 10 s: 1000 uC */
  CHECK(sensei_charge_used_uc(&ch) == 100000 + 100000 + 1000);
  CHECK(ch.state_ms[ST_A] == 10000);

  /* ref count: two users, the state stays on until both are off */
  fresh(SENSEI_CHARGE_LIION, 0);
  sensei_charge_state(&ch, 0, ST_B, true);
  sensei_charge_state(&ch, 1000, ST_B, true);
  sensei_charge_state(&ch, 2000, ST_B, false);
  CHECK(sensei_charge_state_refs(&ch, ST_B) == 1);
  CHECK(sensei_charge_any_state(&ch));
  sensei_charge_state(&ch, 4000, ST_B, false);
  CHECK(!sensei_charge_any_state(&ch));
  sensei_charge_tick(&ch, 10000);
  /* 4 s x 2000 uA */
  CHECK(sensei_charge_used_uc(&ch) == 8000);
  CHECK(ch.state_ms[ST_B] == 4000);

  /* an off without an on is counted and does nothing */
  sensei_charge_state(&ch, 10000, ST_A, false);
  CHECK(ch.underflow == 1);
  CHECK(sensei_charge_state_refs(&ch, ST_A) == 0);
  /* unknown ids are ignored */
  sensei_charge_state(&ch, 10000, 7, true);
  sensei_charge_event(&ch, 9, 1);
  CHECK(!sensei_charge_any_state(&ch));
}

static void test_events(void) {
  fresh(SENSEI_CHARGE_LIION, 0);
  sensei_charge_event(&ch, EV_X, 3);       /* 3 x 1 mC */
  sensei_charge_event(&ch, EV_Y, 1000000); /* 1e6 x 50 nC = 50 mC */
  CHECK(sensei_charge_used_uc(&ch) == 3000 + 50000);
  CHECK(ch.event_cnt[EV_X] == 3 && ch.event_cnt[EV_Y] == 1000000);
  /* the counter saturates */
  sensei_charge_event(&ch, EV_Y, 0xFFFFFFFFu);
  CHECK(ch.event_cnt[EV_Y] == 0xFFFFFFFFu);
}

static void test_uptime_wrap(void) {
  uint32_t t = 0xFFFFF000u;

  sensei_charge_init(&ch, &table, SENSEI_CHARGE_LIION, 17000);
  sensei_charge_set_params(&ch, 0, 90, 1000, 0);
  sensei_charge_tick(&ch, t);
  sensei_charge_state(&ch, t, ST_A, true);
  sensei_charge_state(&ch, t + 0x2000u, ST_A, false); /* crosses 2^32 */
  CHECK(ch.state_ms[ST_A] == 0x2000);
  /* (10000 + 1000) uA x 8.192 s */
  NEAR(sensei_charge_used_uc(&ch), 11000 * 8.192, 0.1);
  /* a clock that steps back does not add or subtract anything */
  uint64_t before = sensei_charge_used_uc(&ch);
  sensei_charge_tick(&ch, t + 0x1000u);
  CHECK(sensei_charge_used_uc(&ch) == before);
}

static void test_vbat_scaling(void) {
  fresh(SENSEI_CHARGE_LIION, 0);
  sensei_charge_set_vbat(&ch, 0, 3000);
  sensei_charge_event(&ch, EV_X, 1); /* 1 mC at 3.6 V costs 1.2 mC at 3.0 V */
  CHECK(sensei_charge_used_uc(&ch) == 1200);
  sensei_charge_state(&ch, 0, ST_A, true);
  sensei_charge_state(&ch, 10000, ST_A, false);
  /* 10 mA x 10 s x 3.6/3.0 = 120000 uC */
  CHECK(sensei_charge_used_uc(&ch) == 1200 + 120000);
  /* the change takes effect after the time before it was accounted at the old voltage */
  fresh(SENSEI_CHARGE_LIION, 1000);
  sensei_charge_set_vbat(&ch, 100000, 1800); /* the first 100 s at 3.6 V */
  CHECK(sensei_charge_used_uc(&ch) == 100000);
  sensei_charge_tick(&ch, 200000); /* the next 100 s at 1.8 V: twice */
  CHECK(sensei_charge_used_uc(&ch) == 100000 + 200000);
  /* 0 mV is not a reading */
  sensei_charge_set_vbat(&ch, 200000, 0);
  CHECK(ch.vbat_mv == 1800);
}

static void test_self_discharge(void) {
  const uint32_t year_s = 8766u * 3600u;

  fresh(SENSEI_CHARGE_LISOCL2, 0);
  /* 1 % per year of 17000 mAh at 20 degC: 170 mAh */
  for (uint32_t s = 0; s < year_s; s += 3600) {
    sensei_charge_tick(&ch, (s + 3600) * 1000u);
  }
  NEAR(sensei_charge_used_uc(&ch) / 3.6e6, 170.0, 1.0);

  /* +10 degC doubles it, -10 degC halves it */
  fresh(SENSEI_CHARGE_LISOCL2, 0);
  sensei_charge_set_temp(&ch, 0, 300);
  sensei_charge_tick(&ch, 3600u * 1000u * 24u);
  double hot = used_ua_s();
  fresh(SENSEI_CHARGE_LISOCL2, 0);
  sensei_charge_set_temp(&ch, 0, 200);
  sensei_charge_tick(&ch, 3600u * 1000u * 24u);
  double mid = used_ua_s();
  fresh(SENSEI_CHARGE_LISOCL2, 0);
  sensei_charge_set_temp(&ch, 0, 100);
  sensei_charge_tick(&ch, 3600u * 1000u * 24u);
  double cold = used_ua_s();
  NEAR(hot / mid, 2.0, 1.0);
  NEAR(mid / cold, 2.0, 1.0);
  /* 19.4 uA for a day: 1.68 C */
  NEAR(mid, 19.39 * 86400, 1.0);
  /* between the steps it is smooth (+5 degC about 1.41) */
  fresh(SENSEI_CHARGE_LISOCL2, 0);
  sensei_charge_set_temp(&ch, 0, 250);
  sensei_charge_tick(&ch, 3600u * 1000u * 24u);
  NEAR(used_ua_s() / mid, 1.414, 3.0);
  /* the extremes are held */
  fresh(SENSEI_CHARGE_LISOCL2, 0);
  sensei_charge_set_temp(&ch, 0, 1500);
  CHECK(ch.temp_c == 150);
  /* Li-ion has none */
  fresh(SENSEI_CHARGE_LIION, 0);
  sensei_charge_tick(&ch, year_s * 1000u / 2);
  CHECK(sensei_charge_used_uc(&ch) == 0);
}

static void test_report(void) {
  struct sensei_charge_report r;

  /* 1000 uA floor, 17000 mAh Li-SOCl2: the average over days, the days left */
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  sensei_charge_report(&ch, 0, &r);
  CHECK(r.days_left == SENSEI_CHARGE_DAYS_UNKNOWN); /* no hour yet */
  CHECK(r.pct == 100);
  CHECK(r.flags & SENSEI_CHARGE_F_CAPACITY_DEFAULT);
  for (uint32_t h = 1; h <= 24 * 10; h++) {
    sensei_charge_tick(&ch, h * 3600000u);
  }
  sensei_charge_report(&ch, 240u * 3600000u, &r);
  NEAR(r.avg_ua, 1019.4, 1.0); /* floor + 19.4 uA self-discharge */
  /* 240 h x 1019 uA = 245 mAh used */
  NEAR(r.used_mah, 245, 1.0);
  CHECK(r.capacity_mah == 17000);
  CHECK(r.pct == 99);
  /* usable 90 % = 15300 mAh; left 15055 mAh at 1.0194 mA = 615 days */
  NEAR(r.days_left, 15055.0 / 1.0194 / 24.0, 1.5);
  /* error: 7 % of 245 mAh */
  NEAR(r.err_mah, 17, 10.0);

  /* a battery change of the usable share */
  sensei_charge_set_params(&ch, 17000, 50, 1000, 0);
  sensei_charge_report(&ch, 240u * 3600000u, &r);
  NEAR(r.days_left, (8500.0 - 245.0) / 1.0194 / 24.0, 1.5);
  CHECK(!(r.flags & SENSEI_CHARGE_F_CAPACITY_DEFAULT));

  /* the moving average follows a change of the load, with a seven day memory */
  for (uint32_t h = 241; h <= 240 + 24 * 28; h++) {
    sensei_charge_tick(&ch, h * 3600000u);
  }
  sensei_charge_set_params(&ch, 17000, 50, 5000, 0);
  for (uint32_t h = 240 + 24 * 28 + 1; h <= 240 + 24 * 28 + 24 * 7; h++) {
    sensei_charge_tick(&ch, h * 3600000u);
  }
  sensei_charge_report(&ch, (240u + 24u * 35u) * 3600000u, &r);
  /* a time constant of 7 days: after 7 days 63 % of the way from 1019 to 5019 uA */
  NEAR(r.avg_ua, 1019 + 0.63 * 4000, 3.0);

  /* empty: no days left */
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  sensei_charge_set_params(&ch, 1, 90, 1000, 0); /* 1 mAh = 3600 C: gone in an hour */
  for (uint32_t m = 1; m <= 130; m++) {
    sensei_charge_tick(&ch, m * 60000u);
  }
  sensei_charge_report(&ch, 130u * 60000u, &r);
  CHECK(r.days_left == 0);
  CHECK(r.pct == 0 || r.pct == 100); /* 1000 uA x 2 h is 2 mAh */
  CHECK(r.pct == 0);

  /* no capacity (Li-ion without a default): the percentage is not known */
  sensei_charge_init(&ch, &table, SENSEI_CHARGE_LIION, 0);
  sensei_charge_set_params(&ch, 0, 90, 100, 0);
  sensei_charge_report(&ch, 0, &r);
  CHECK(r.pct == SENSEI_CHARGE_PCT_UNKNOWN);
  CHECK(r.days_left == SENSEI_CHARGE_DAYS_UNKNOWN);
}

static void test_ext_power(void) {
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  sensei_charge_set_ext_power(&ch, 0, true);
  sensei_charge_state(&ch, 0, ST_A, true);
  sensei_charge_event(&ch, EV_X, 1);
  sensei_charge_tick(&ch, 3600000u);
  sensei_charge_state(&ch, 3600000u, ST_A, false);
  /* only the cell's own self-discharge: 19.4 uA x 1 h; the ledger counts the state and the event */
  NEAR(sensei_charge_used_uc(&ch), 19.39 * 3600, 2.0);
  CHECK(ch.state_ms[ST_A] == 3600000u);
  CHECK(ch.event_cnt[EV_X] == 1);
  sensei_charge_set_ext_power(&ch, 3600000u, false);
  sensei_charge_tick(&ch, 3600000u + 1000);
  NEAR(sensei_charge_used_uc(&ch), 19.39 * 3601 + 1000 * 1.0, 2.0);
}

static void test_check_and_floor(void) {
  /* class 0: expected (20 mA + floor 1 mA); at 3.6 V */
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  CHECK(sensei_charge_check_expected_ua(&ch, 0) == 21000);
  CHECK(sensei_charge_check_expected_ua(&ch, 1) == 0); /* off */
  CHECK(sensei_charge_check_expected_ua(&ch, 5) == 0);
  /* at 3.0 V the battery current is 1.2 times */
  sensei_charge_set_vbat(&ch, 0, 3000);
  CHECK(sensei_charge_check_expected_ua(&ch, 0) == 25200);
  sensei_charge_set_vbat(&ch, 0, 3600);

  /* samples within the limits do not flag */
  for (int i = 0; i < 300; i++) {
    sensei_charge_check_sample(&ch, 0, 21000 + (i % 5) * 400 - 800);
  }
  CHECK(!(ch.flags & SENSEI_CHARGE_F_CHECK_MISMATCH));
  CHECK(ch.check[0].n == 300);
  NEAR(ch.check[0].mean_pm, 1000, 2.0);
  /* a job that did not run (near zero) is thrown away, it does not drag the mean */
  sensei_charge_check_sample(&ch, 0, 1000);
  CHECK(ch.check[0].rejected == 1 && ch.check[0].n == 300);
  /* class 1 has no expected value: ignored */
  sensei_charge_check_sample(&ch, 1, 5000);
  CHECK(ch.check[1].n == 0);

  /* 40 % too high: flagged once 200 samples are in */
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  for (int i = 0; i < 199; i++) {
    sensei_charge_check_sample(&ch, 0, 29400);
  }
  CHECK(!(ch.flags & SENSEI_CHARGE_F_CHECK_MISMATCH));
  sensei_charge_check_sample(&ch, 0, 29400);
  CHECK(ch.flags & SENSEI_CHARGE_F_CHECK_MISMATCH);
  /* and it clears when the mean comes back */
  for (int i = 0; i < 1500; i++) {
    sensei_charge_check_sample(&ch, 0, 21000);
  }
  CHECK(!(ch.flags & SENSEI_CHARGE_F_CHECK_MISMATCH));
  /* a long run of bad samples (the average is saturated at 1024) comes back too: 800 bad, then 3000 good */
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  for (int i = 0; i < 800; i++) {
    sensei_charge_check_sample(&ch, 0, 29400);
  }
  CHECK(ch.flags & SENSEI_CHARGE_F_CHECK_MISMATCH);
  for (int i = 0; i < 3000; i++) {
    sensei_charge_check_sample(&ch, 0, 20000);
  }
  CHECK(!(ch.flags & SENSEI_CHARGE_F_CHECK_MISMATCH));
  NEAR(ch.check[0].mean_pm, 1000, 3.0); /* a moving average of 1024 samples is not at 1000 yet */
  /* 20 % off is fine */
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  for (int i = 0; i < 400; i++) {
    sensei_charge_check_sample(&ch, 0, 25000);
  }
  CHECK(!(ch.flags & SENSEI_CHARGE_F_CHECK_MISMATCH));
  /* the job is mostly not running: more rejected than accepted */
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  for (int i = 0; i < 100; i++) {
    sensei_charge_check_sample(&ch, 0, 21000);
  }
  for (int i = 0; i < 201; i++) {
    sensei_charge_check_sample(&ch, 0, 500);
  }
  CHECK(ch.flags & SENSEI_CHARGE_F_CHECK_MISMATCH);

  /* floor watchdog: normal samples do nothing */
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  for (int i = 0; i < 10; i++) {
    CHECK(sensei_charge_floor_sample(&ch, (uint32_t)(i + 1) * 20000u, 1500));
  }
  CHECK(!(ch.flags & SENSEI_CHARGE_F_FLOOR_HIGH));
  /* a sample while a state is on is not used */
  sensei_charge_state(&ch, 200000, ST_B, true);
  CHECK(!sensei_charge_floor_sample(&ch, 200000, 30000));
  sensei_charge_state(&ch, 200000, ST_B, false);
  /* two high ones, then a normal one: nothing */
  CHECK(sensei_charge_floor_sample(&ch, 220000, 4000));
  CHECK(sensei_charge_floor_sample(&ch, 240000, 4000));
  CHECK(sensei_charge_floor_sample(&ch, 260000, 1000));
  CHECK(!(ch.flags & SENSEI_CHARGE_F_FLOOR_HIGH));
  /* three in a row: FLOOR_HIGH, the excess (3 mA over 1 mA) is counted */
  for (int i = 0; i < 3; i++) {
    CHECK(sensei_charge_floor_sample(&ch, 280000u + (uint32_t)i * 20000u, 4000));
  }
  CHECK(ch.flags & SENSEI_CHARGE_F_FLOOR_HIGH);
  CHECK(ch.floor_extra_ua == 3000);
  uint64_t before = sensei_charge_used_uc(&ch);
  sensei_charge_tick(&ch, 420000u); /* 100 s at 4 mA after the sample at 320 s */
  NEAR(sensei_charge_used_uc(&ch) - before, 4000.0 * 100, 1.0);
  /* back to normal after three good samples */
  for (int i = 0; i < 3; i++) {
    sensei_charge_floor_sample(&ch, 440000u + (uint32_t)i * 20000u, 1100);
  }
  CHECK(!(ch.flags & SENSEI_CHARGE_F_FLOOR_HIGH));
  CHECK(ch.floor_extra_ua == 0);
  /* with the margin set: 500 uA over is high */
  sensei_charge_set_floor_margin(&ch, 400);
  for (int i = 0; i < 3; i++) {
    sensei_charge_floor_sample(&ch, 520000u + (uint32_t)i * 20000u, 1500);
  }
  CHECK(ch.flags & SENSEI_CHARGE_F_FLOOR_HIGH);
}

static void test_record(void) {
  uint8_t rec[SENSEI_CHARGE_RECORD_BYTES];
  struct sensei_charge_record r;

  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  sensei_charge_new(&ch, 0, 19000, 1234, 0xDEADBEEFu);
  sensei_charge_set_vbat(&ch, 0, 3400);
  sensei_charge_flags_update(&ch, SENSEI_CHARGE_F_RESTORED | SENSEI_CHARGE_F_FLOOR_HIGH, 0);
  ch.sd_nc = 5ull * 1000000ull * 1000ull; /* 5 C */
  ch.bo_h = 777;
  sensei_charge_pack(&ch, 1780000000u, rec);
  CHECK(rec[0] == 0xC5 && rec[1] == 1);
  CHECK(sensei_charge_unpack(rec, &r) == 0);
  CHECK(r.used_uc == 1234ull * 3600000ull);
  CHECK(r.sd_mc == 5000);
  CHECK(r.battery_id == 0xDEADBEEFu);
  CHECK(r.capacity_mah == 19000);
  CHECK(r.last_vbat_mv == 3400);
  CHECK(r.wall_s == 1780000000u);
  CHECK(r.bo_h == 777);
  CHECK(r.flags == SENSEI_CHARGE_F_RESTORED); /* the dynamic flag is not stored */
  CHECK(!r.liion);

  /* the layout is fixed (little endian), a host tool reads the same bytes */
  CHECK(rec[4] == (uint8_t)(r.used_uc) && rec[5] == (uint8_t)(r.used_uc >> 8));
  CHECK(rec[16] == 0xEF && rec[19] == 0xDE);

  /* errors */
  CHECK(sensei_charge_unpack(NULL, &r) == -EINVAL);
  uint8_t bad[SENSEI_CHARGE_RECORD_BYTES];
  memcpy(bad, rec, sizeof(bad));
  bad[9] ^= 0x01;
  CHECK(sensei_charge_unpack(bad, &r) == -EBADMSG);
  memcpy(bad, rec, sizeof(bad));
  bad[0] = 0;
  CHECK(sensei_charge_unpack(bad, &r) == -ENOMSG);
  memcpy(bad, rec, sizeof(bad));
  bad[1] = 2; /* another version */
  CHECK(sensei_charge_unpack(bad, &r) == -ENOMSG);
  memset(bad, 0, sizeof(bad));
  CHECK(sensei_charge_unpack(bad, &r) == -ENOMSG);
  memset(bad, 0xFF, sizeof(bad));
  CHECK(sensei_charge_unpack(bad, &r) == -ENOMSG);

  /* the CRC is CRC-16/CCITT-FALSE */
  CHECK(sensei_charge_crc16((const uint8_t *)"123456789", 9) == 0x29B1);

  /* Li-ion marker, and big values */
  fresh(SENSEI_CHARGE_LIION, 0);
  ch.used_nc = (uint64_t)61200 * 1000000ull * 1000ull * 1000ull / 1000ull; /* 61.2 kC... */
  sensei_charge_pack(&ch, 0, rec);
  CHECK(sensei_charge_unpack(rec, &r) == 0 && r.liion);
  CHECK(r.used_uc == ch.used_nc / 1000);

  /* ledger */
  fresh(SENSEI_CHARGE_LISOCL2, 0);
  sensei_charge_state(&ch, 0, ST_A, true);
  sensei_charge_state(&ch, 5000, ST_A, false);
  sensei_charge_event(&ch, EV_X, 7);
  uint8_t led[SENSEI_CHARGE_LEDGER_MAX_BYTES];
  size_t n = sensei_charge_ledger_pack(&ch, led, sizeof(led));
  CHECK(n == 4 + 4 * ST_N + 4 * EV_N + 2);
  CHECK(sensei_charge_ledger_pack(&ch, led, n - 1) == 0);
  struct sensei_charge other;
  sensei_charge_init(&other, &table, SENSEI_CHARGE_LISOCL2, 17000);
  CHECK(sensei_charge_ledger_unpack(&other, led, n) == 0);
  CHECK(other.state_ms[ST_A] == 5000 && other.event_cnt[EV_X] == 7);
  led[6] ^= 1;
  CHECK(sensei_charge_ledger_unpack(&other, led, n) == -EBADMSG);
  led[6] ^= 1;
  led[2] = 5; /* another layout */
  CHECK(sensei_charge_ledger_unpack(&other, led, n) == -ENOMSG);
}

static void pack_with(uint32_t used_mah, uint32_t vbat, uint32_t id, uint8_t *out) {
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  sensei_charge_new(&ch, 0, 17000, used_mah, id);
  sensei_charge_set_vbat(&ch, 0, vbat);
  ch.last_vbat_mv = (uint16_t)vbat;
  sensei_charge_pack(&ch, 1780000000u, out);
}

static void test_boot(void) {
  uint8_t ret[32], set[32], old[32];
  struct sensei_charge_report r;

  /* retained wins over the settings (it is newer) */
  pack_with(100, 3500, 11, ret);
  pack_with(50, 3500, 11, set);
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  CHECK(sensei_charge_boot(&ch, ret, set, false, 3500, 0) == SENSEI_CHARGE_SRC_RETAINED);
  sensei_charge_report(&ch, 0, &r);
  CHECK(r.used_mah == 100 && r.battery_id == 11);
  CHECK(!(r.flags & (SENSEI_CHARGE_F_RESTORED | SENSEI_CHARGE_F_UNCONFIRMED | SENSEI_CHARGE_F_AUTO_SWAP)));

  /* a damaged retained record: the settings record, RESTORED, the loss of up to 6 h is in the error */
  ret[10] ^= 0xFF;
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  CHECK(sensei_charge_boot(&ch, ret, set, false, 3500, 0) == SENSEI_CHARGE_SRC_SETTINGS);
  sensei_charge_report(&ch, 0, &r);
  CHECK(r.used_mah == 50);
  CHECK(r.flags & SENSEI_CHARGE_F_RESTORED);
  CHECK(!(r.flags & SENSEI_CHARGE_F_UNCONFIRMED)); /* not a power-on reset */
  CHECK(ch.restore_loss_uc == 1000ull * 21600);

  /* both damaged or none: zero, UNCONFIRMED, even without a power-on reset */
  set[3] ^= 1;
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  CHECK(sensei_charge_boot(&ch, ret, set, false, 3500, 0) == SENSEI_CHARGE_SRC_NONE);
  sensei_charge_report(&ch, 0, &r);
  CHECK(r.used_mah == 0 && (r.flags & SENSEI_CHARGE_F_UNCONFIRMED));
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  CHECK(sensei_charge_boot(&ch, NULL, NULL, true, 3600, 0) == SENSEI_CHARGE_SRC_NONE);
  CHECK(ch.flags & SENSEI_CHARGE_F_UNCONFIRMED);

  /* power-on reset, retained lost, settings record, same voltage: UNCONFIRMED and RESTORED */
  pack_with(900, 3400, 22, old);
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  CHECK(sensei_charge_boot(&ch, NULL, old, true, 3420, 0) == SENSEI_CHARGE_SRC_SETTINGS);
  sensei_charge_report(&ch, 0, &r);
  CHECK(r.used_mah == 900);
  CHECK((r.flags & (SENSEI_CHARGE_F_UNCONFIRMED | SENSEI_CHARGE_F_RESTORED)) ==
        (SENSEI_CHARGE_F_UNCONFIRMED | SENSEI_CHARGE_F_RESTORED));
  CHECK(ch.last_vbat_mv == 3420); /* the new rested value is the reference for the next time */

  /* 150 mV up: a new battery. Reset, AUTO_SWAP, the old id is gone */
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  CHECK(sensei_charge_boot(&ch, NULL, old, true, 3550, 0) == SENSEI_CHARGE_SRC_SETTINGS);
  sensei_charge_report(&ch, 0, &r);
  CHECK(r.used_mah == 0 && r.battery_id == 0);
  CHECK(r.flags & SENSEI_CHARGE_F_AUTO_SWAP);
  CHECK(!(r.flags & (SENSEI_CHARGE_F_UNCONFIRMED | SENSEI_CHARGE_F_RESTORED)));
  CHECK(ch.restore_loss_uc == 0);
  /* 149 mV up is not */
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  CHECK(sensei_charge_boot(&ch, NULL, old, true, 3549, 0) == SENSEI_CHARGE_SRC_SETTINGS);
  CHECK(ch.flags & SENSEI_CHARGE_F_UNCONFIRMED);
  CHECK(!(ch.flags & SENSEI_CHARGE_F_AUTO_SWAP));
  /* a rested voltage that is not known (0) never means a swap */
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  sensei_charge_boot(&ch, NULL, old, true, 0, 0);
  CHECK(!(ch.flags & SENSEI_CHARGE_F_AUTO_SWAP));
  /* a valid retained record after a power-on reset: the battery was never off */
  pack_with(100, 3500, 11, ret);
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  CHECK(sensei_charge_boot(&ch, ret, old, true, 3800, 0) == SENSEI_CHARGE_SRC_RETAINED);
  CHECK(!(ch.flags & (SENSEI_CHARGE_F_AUTO_SWAP | SENSEI_CHARGE_F_UNCONFIRMED)));

  /* capacity: the setting wins; without one the record's declared capacity; a default stays flagged */
  pack_with(100, 3500, 11, ret);
  sensei_charge_init(&ch, &table, SENSEI_CHARGE_LISOCL2, 17000);
  sensei_charge_set_params(&ch, 8000, 90, 1000, 0);
  sensei_charge_boot(&ch, ret, NULL, false, 3500, 0);
  CHECK(ch.capacity_mah == 8000 && !(ch.flags & SENSEI_CHARGE_F_CAPACITY_DEFAULT));
  sensei_charge_init(&ch, &table, SENSEI_CHARGE_LISOCL2, 5000);
  sensei_charge_set_params(&ch, 0, 90, 1000, 0);
  sensei_charge_boot(&ch, ret, NULL, false, 3500, 0);
  CHECK(ch.capacity_mah == 17000 && !(ch.flags & SENSEI_CHARGE_F_CAPACITY_DEFAULT));
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  sensei_charge_new(&ch, 0, 0, 0, 5); /* no capacity declared */
  sensei_charge_pack(&ch, 0, ret);
  sensei_charge_init(&ch, &table, SENSEI_CHARGE_LISOCL2, 17000);
  sensei_charge_set_params(&ch, 0, 90, 1000, 0);
  sensei_charge_boot(&ch, ret, NULL, false, 3500, 0);
  CHECK(ch.capacity_mah == 17000 && (ch.flags & SENSEI_CHARGE_F_CAPACITY_DEFAULT));

  /* new: resets everything the ledger and the flags */
  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  sensei_charge_flags_update(&ch, SENSEI_CHARGE_F_UNCONFIRMED | SENSEI_CHARGE_F_VOLTAGE_EOL | SENSEI_CHARGE_F_AUTO_SWAP,
                             0);
  sensei_charge_event(&ch, EV_X, 2);
  sensei_charge_new(&ch, 0, 17000, 0, 99);
  CHECK(ch.flags == 0 && ch.event_cnt[EV_X] == 0 && ch.battery_id == 99);
  sensei_charge_new(&ch, 0, 17000, 1000, 100);
  CHECK(sensei_charge_used_uc(&ch) == 1000ull * 3600000ull);
}

static void test_liion(void) {
  struct sensei_charge_report r;

  /* the charge current is integrated over the time between samples (mean of two) */
  fresh(SENSEI_CHARGE_LIION, 100);
  sensei_charge_new(&ch, 0, 1000, 500, 1); /* 500 of 1000 mAh used */
  sensei_charge_set_ext_power(&ch, 0, true);
  sensei_charge_liion_charge(&ch, 0, 15000);
  sensei_charge_liion_charge(&ch, 20000, 15000); /* 15 mA x 20 s = 300 mC */
  sensei_charge_liion_charge(&ch, 40000, 5000);  /* (15 + 5)/2 mA x 20 s = 200 mC */
  CHECK(sensei_charge_used_uc(&ch) == 500ull * 3600000ull - 500000);
  /* the series ends: the gap is not counted */
  sensei_charge_liion_charge_end(&ch);
  sensei_charge_liion_charge(&ch, 100000, 15000);
  CHECK(sensei_charge_used_uc(&ch) == 500ull * 3600000ull - 500000);
  /* it never goes below zero */
  sensei_charge_new(&ch, 100000, 1000, 0, 2);
  sensei_charge_liion_charge(&ch, 100000, 15000);
  sensei_charge_liion_charge(&ch, 200000, 15000);
  CHECK(sensei_charge_used_uc(&ch) == 0);
  /* a primary cell ignores it */
  fresh(SENSEI_CHARGE_LISOCL2, 0);
  sensei_charge_new(&ch, 0, 1000, 500, 1);
  sensei_charge_liion_charge(&ch, 0, 15000);
  sensei_charge_liion_charge(&ch, 20000, 15000);
  CHECK(ch.used_nc == 500ull * 3600000ull * 1000ull);

  /* charge done: zero, synced, the flags of a lost battery history are gone */
  fresh(SENSEI_CHARGE_LIION, 100);
  sensei_charge_new(&ch, 0, 1000, 400, 1);
  sensei_charge_flags_update(&ch, SENSEI_CHARGE_F_UNCONFIRMED | SENSEI_CHARGE_F_RESTORED, 0);
  sensei_charge_liion_done(&ch, 0);
  sensei_charge_report(&ch, 0, &r);
  CHECK(r.used_mah == 0 && r.pct == 100);
  CHECK(r.flags == SENSEI_CHARGE_F_LIION_SYNCED);

  /* empty after a synced full: the charge it took becomes the capacity (here 800 of 1000 mAh) */
  sensei_charge_set_ext_power(&ch, 0, false);
  sensei_charge_event(&ch, EV_X, 1);
  ch.used_nc = 800ull * 3600000ull * 1000ull;
  sensei_charge_liion_empty(&ch, 0);
  CHECK(ch.capacity_mah == 800);
  CHECK(!(ch.flags & SENSEI_CHARGE_F_LIION_SYNCED));
  /* without a synced full charge it learns nothing */
  ch.used_nc = 500ull * 3600000ull * 1000ull;
  sensei_charge_liion_empty(&ch, 0);
  CHECK(ch.capacity_mah == 800);
  /* an insane value (10 % of the capacity) is not taken */
  sensei_charge_liion_done(&ch, 0);
  ch.used_nc = 80ull * 3600000ull * 1000ull;
  sensei_charge_liion_empty(&ch, 0);
  CHECK(ch.capacity_mah == 800);
  /* no capacity before: any */
  sensei_charge_init(&ch, &table, SENSEI_CHARGE_LIION, 0);
  sensei_charge_set_params(&ch, 0, 90, 100, 0);
  sensei_charge_liion_done(&ch, 0);
  ch.used_nc = 1500ull * 3600000ull * 1000ull;
  sensei_charge_liion_empty(&ch, 0);
  CHECK(ch.capacity_mah == 1500 && !(ch.flags & SENSEI_CHARGE_F_CAPACITY_DEFAULT));

  /* voltage hint: only below 20 % and above 80 % */
  fresh(SENSEI_CHARGE_LIION, 0);
  sensei_charge_new(&ch, 0, 1000, 500, 1); /* the counter says 50 % */
  sensei_charge_liion_ocv(&ch, 0, 50);
  CHECK(sensei_charge_used_uc(&ch) == 500ull * 3600000ull);
  sensei_charge_liion_ocv(&ch, 0, 80);
  CHECK(sensei_charge_used_uc(&ch) == 500ull * 3600000ull);
  sensei_charge_liion_ocv(&ch, 0, 10); /* the voltage says 10 % left: used 900, a step of 1/8 of 400 */
  CHECK(sensei_charge_used_uc(&ch) == 550ull * 3600000ull);
  fresh(SENSEI_CHARGE_LIION, 0);
  sensei_charge_new(&ch, 0, 1000, 500, 1);
  sensei_charge_liion_ocv(&ch, 0, 100); /* full: used 0, a step of 1/8 of 500 */
  CHECK(sensei_charge_used_uc(&ch) == (500ull * 3600000ull) - (500ull * 3600000ull) / 8);
  /* a primary cell is never pulled */
  fresh(SENSEI_CHARGE_LISOCL2, 0);
  sensei_charge_new(&ch, 0, 1000, 500, 1);
  sensei_charge_liion_ocv(&ch, 0, 10);
  CHECK(sensei_charge_used_uc(&ch) == 500ull * 3600000ull);
}

static void test_eol_button(void) {
  struct sensei_charge_report r;

  fresh(SENSEI_CHARGE_LISOCL2, 1000);
  sensei_charge_new(&ch, 0, 1000, 100, 1);
  sensei_charge_report(&ch, 0, &r);
  CHECK(r.pct == 90 && r.days_left == SENSEI_CHARGE_DAYS_UNKNOWN);
  sensei_charge_set_eol(&ch, true);
  sensei_charge_report(&ch, 0, &r);
  CHECK(r.pct == 5 && r.days_left == 0 && (r.flags & SENSEI_CHARGE_F_VOLTAGE_EOL));
  sensei_charge_set_eol(&ch, false);
  CHECK(!(ch.flags & SENSEI_CHARGE_F_VOLTAGE_EOL));

  /* two brown-outs within 24 h: end of life; one a day and a half later is not */
  sensei_charge_brownout(&ch, 1780000000u);
  CHECK(!(ch.flags & SENSEI_CHARGE_F_VOLTAGE_EOL));
  sensei_charge_brownout(&ch, 1780000000u + 36 * 3600);
  CHECK(!(ch.flags & SENSEI_CHARGE_F_VOLTAGE_EOL));
  sensei_charge_brownout(&ch, 1780000000u + 36 * 3600 + 3600 * 20);
  CHECK(ch.flags & SENSEI_CHARGE_F_VOLTAGE_EOL);
  /* no wall clock: nothing can be said */
  sensei_charge_set_eol(&ch, false);
  sensei_charge_brownout(&ch, 0);
  sensei_charge_brownout(&ch, 0);
  CHECK(!(ch.flags & SENSEI_CHARGE_F_VOLTAGE_EOL));
  /* it is kept across a restart (a sticky flag) */
  uint8_t rec[32];
  sensei_charge_set_eol(&ch, true);
  sensei_charge_pack(&ch, 0, rec);
  sensei_charge_init(&ch, &table, SENSEI_CHARGE_LISOCL2, 17000);
  sensei_charge_boot(&ch, rec, NULL, false, 3500, 0);
  CHECK(ch.flags & SENSEI_CHARGE_F_VOLTAGE_EOL);

  /* swap button: three presses within 3 s, only in the first 5 min */
  struct sensei_charge_btn b;
  memset(&b, 0, sizeof(b));
  CHECK(!sensei_charge_btn_press(&b, 1000, 10000));
  CHECK(!sensei_charge_btn_press(&b, 2000, 11000));
  CHECK(sensei_charge_btn_press(&b, 3500, 12500));
  /* it starts again */
  CHECK(!sensei_charge_btn_press(&b, 4000, 13000));
  CHECK(!sensei_charge_btn_press(&b, 5000, 14000));
  /* too slow: the first falls out of the window */
  CHECK(!sensei_charge_btn_press(&b, 7500, 16500));
  CHECK(sensei_charge_btn_press(&b, 8000, 17000)); /* 5000, 7500, 8000 */
  /* late: refused and the count starts again */
  memset(&b, 0, sizeof(b));
  CHECK(!sensei_charge_btn_press(&b, 1, SENSEI_CHARGE_BTN_BOOT_MS + 1));
  CHECK(!sensei_charge_btn_press(&b, 2, SENSEI_CHARGE_BTN_BOOT_MS + 2));
  CHECK(!sensei_charge_btn_press(&b, 3, SENSEI_CHARGE_BTN_BOOT_MS + 3));
  /* the uptime wraps */
  memset(&b, 0, sizeof(b));
  CHECK(!sensei_charge_btn_press(&b, 0xFFFFFF00u, 1000));
  CHECK(!sensei_charge_btn_press(&b, 0xFFFFFF80u, 1100));
  CHECK(sensei_charge_btn_press(&b, 0x00000040u, 1200));
}

/* A year of the C4 LoRa budget: 1.51 mW load at 3.6 V (419 uA) plus the self-discharge is 22.6 % of 17 Ah. */
static void test_year(void) {
  struct sensei_charge_report r;
  const uint32_t day_s = 86400;

  fresh(SENSEI_CHARGE_LISOCL2, 419);
  for (uint32_t d = 1; d <= 365; d++) {
    for (uint32_t h = 1; h <= 24; h++) {
      sensei_charge_tick(&ch, ((d - 1) * day_s + h * 3600u) * 1000u);
    }
  }
  sensei_charge_report(&ch, 365u * day_s * 1000u, &r);
  /* 419 uA for a year is 3670 mAh, the self-discharge adds 170 mAh: 3.84 Ah as in the design */
  NEAR(r.used_mah, 3670.0 + 170.0, 1.0);
  CHECK(r.pct >= 76 && r.pct <= 78);
  NEAR(r.avg_ua, 419 + 19.4, 1.0);
  /* days left at the end of year one: 15300 - 3840 mAh at 0.4384 mA */
  NEAR(r.days_left, (15300.0 - r.used_mah) / 0.4384 / 24.0, 1.5);
}

int main(void) {
  test_states();
  test_events();
  test_uptime_wrap();
  test_vbat_scaling();
  test_self_discharge();
  test_report();
  test_ext_power();
  test_check_and_floor();
  test_record();
  test_boot();
  test_liion();
  test_eol_button();
  test_year();
  if (fails != 0) {
    printf("%d checks failed\n", fails);
    return 1;
  }
  printf("charge: all tests passed\n");
  return 0;
}
