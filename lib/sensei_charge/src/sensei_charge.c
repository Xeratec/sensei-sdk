/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_charge.h"

#include <errno.h>
#include <string.h>

#define REC_MAGIC 0xC5u
#define REC_VERSION 1u
#define LEDGER_MAGIC 0xC6u
#define LEDGER_VERSION 1u

#define NC_PER_UC 1000ull
#define UC_PER_MAH 3600000ull
#define NC_PER_MC 1000000ull
#define HOUR_MS 3600000u
#define AVG_HOURS 168u /* 7 days */
#define LOSS_RESTORE_S 21600ull
#define SWAP_STEP_MV 150u
#define CHECK_MIN_N 200u
#define CHECK_N_MAX 1024u
#define SD_PCT_PER_YEAR_X10 10u /* 1 % per year at 20 degC, in 0.1 % */
#define HOURS_PER_YEAR 8766u

/* 2^(k/10) in Q10: self-discharge doubles every +10 degC */
static const uint16_t pow2_tenth_q10[10] = {1024, 1097, 1176, 1261, 1351, 1448, 1552, 1664, 1783, 1911};

static uint32_t vbat_or_ref(const struct sensei_charge *ch) {
  return ch->vbat_mv != 0 ? ch->vbat_mv : SENSEI_CHARGE_REF_MV;
}

/* A charge at 3.6 V as the battery pays it. */
static uint64_t scaled(const struct sensei_charge *ch, uint64_t nc_ref) {
  return nc_ref * SENSEI_CHARGE_REF_MV / vbat_or_ref(ch);
}

static uint32_t sd_factor_q10(int16_t temp_c) {
  int d = (temp_c < -20 ? -20 : temp_c > 70 ? 70 : temp_c) - 20;
  int n = d >= 0 ? d / 10 : -((-d + 9) / 10);
  int k = d - 10 * n;
  uint32_t f = pow2_tenth_q10[k];

  return n >= 0 ? f << n : f >> -n;
}

static uint64_t isqrt64(uint64_t x) {
  uint64_t r = 0;
  uint64_t bit = 1ull << 62;

  while (bit > x) {
    bit >>= 2;
  }
  while (bit != 0) {
    if (x >= r + bit) {
      x -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return r;
}

static uint64_t cap_uc(const struct sensei_charge *ch) { return (uint64_t)ch->capacity_mah * UC_PER_MAH; }

/* ---- time ---- */

static void add_used(struct sensei_charge *ch, uint64_t nc_ref) {
  uint64_t nc = scaled(ch, nc_ref);

  ch->used_nc += nc;
  ch->win_nc += nc;
}

void sensei_charge_tick(struct sensei_charge *ch, uint32_t now_ms) {
  int32_t dt;

  if (!ch->started) {
    ch->last_ms = now_ms;
    ch->started = true;
    return;
  }
  dt = (int32_t)(now_ms - ch->last_ms);
  if (dt <= 0) {
    ch->last_ms = now_ms; /* the clock stepped back: take it as the new origin */
    return;
  }
  ch->last_ms = now_ms;

  for (unsigned i = 0; i < ch->table->n_states; i++) {
    if (ch->ref[i] != 0) {
      ch->state_ms[i] += (uint32_t)dt;
      if (!ch->ext_power) {
        add_used(ch, (uint64_t)ch->table->state_ua[i] * (uint32_t)dt);
      }
    }
  }
  if (!ch->ext_power) {
    add_used(ch, (uint64_t)(ch->floor_ua + ch->addon_ua + ch->floor_extra_ua) * (uint32_t)dt);
  }
  if (ch->chem == SENSEI_CHARGE_LISOCL2) {
    uint64_t sd = (uint64_t)ch->capacity_mah * SD_PCT_PER_YEAR_X10 * (uint32_t)dt * sd_factor_q10(ch->temp_c) /
                  ((uint64_t)HOURS_PER_YEAR * 1024u);

    ch->sd_nc += sd;
    ch->win_nc += sd;
  }
  if (!ch->ext_power) {
    ch->win_ms += (uint32_t)dt;
    if (ch->win_ms >= HOUR_MS) {
      int64_t x = (int64_t)(ch->win_nc / ch->win_ms); /* nC per ms = uA */

      if (ch->avg_n < AVG_HOURS) {
        ch->avg_n++;
      }
      ch->avg_ua = (uint32_t)((int64_t)ch->avg_ua + (x - (int64_t)ch->avg_ua) / ch->avg_n);
      ch->win_nc = 0;
      ch->win_ms = 0;
    }
  }
}

void sensei_charge_init(struct sensei_charge *ch, const struct sensei_charge_table *table, enum sensei_charge_chem chem,
                        uint32_t default_capacity_mah) {
  memset(ch, 0, sizeof(*ch));
  ch->table = table;
  ch->chem = chem;
  ch->default_capacity_mah = default_capacity_mah;
  ch->capacity_mah = default_capacity_mah;
  ch->usable_pct = 90;
  ch->floor_margin_ua = 2000;
  ch->vbat_mv = SENSEI_CHARGE_REF_MV;
  ch->temp_c = 20;
  ch->flags = SENSEI_CHARGE_F_CAPACITY_DEFAULT;
}

void sensei_charge_set_params(struct sensei_charge *ch, uint32_t capacity_mah, uint8_t usable_pct, uint32_t floor_ua,
                              uint32_t addon_ua) {
  if (capacity_mah != 0) {
    ch->capacity_mah = capacity_mah;
    ch->flags &= (uint16_t)~SENSEI_CHARGE_F_CAPACITY_DEFAULT;
  } else if ((ch->flags & SENSEI_CHARGE_F_CAPACITY_DEFAULT) != 0) {
    ch->capacity_mah = ch->default_capacity_mah;
  } /* else: a declared or learned capacity stays */
  ch->usable_pct = usable_pct == 0 || usable_pct > 100 ? 90 : usable_pct;
  ch->floor_ua = floor_ua;
  ch->addon_ua = addon_ua;
}

void sensei_charge_set_floor_margin(struct sensei_charge *ch, uint32_t margin_ua) { ch->floor_margin_ua = margin_ua; }

void sensei_charge_set_vbat(struct sensei_charge *ch, uint32_t now_ms, uint32_t mv) {
  if (mv == 0) {
    return;
  }
  sensei_charge_tick(ch, now_ms);
  ch->vbat_mv = mv;
  ch->last_vbat_mv = (uint16_t)(mv > 0xFFFF ? 0xFFFF : mv);
}

void sensei_charge_set_temp(struct sensei_charge *ch, uint32_t now_ms, int32_t t_c10) {
  sensei_charge_tick(ch, now_ms);
  ch->temp_c = (int16_t)((t_c10 + (t_c10 >= 0 ? 5 : -5)) / 10);
}

void sensei_charge_set_ext_power(struct sensei_charge *ch, uint32_t now_ms, bool present) {
  sensei_charge_tick(ch, now_ms);
  ch->ext_power = present;
}

void sensei_charge_state(struct sensei_charge *ch, uint32_t now_ms, unsigned id, bool on) {
  if (id >= ch->table->n_states) {
    return;
  }
  sensei_charge_tick(ch, now_ms);
  if (on) {
    if (ch->ref[id] < 255) {
      ch->ref[id]++;
    }
  } else if (ch->ref[id] != 0) {
    ch->ref[id]--;
  } else {
    ch->underflow++;
  }
}

unsigned sensei_charge_state_refs(const struct sensei_charge *ch, unsigned id) {
  return id < ch->table->n_states ? ch->ref[id] : 0;
}

bool sensei_charge_any_state(const struct sensei_charge *ch) {
  for (unsigned i = 0; i < ch->table->n_states; i++) {
    if (ch->ref[i] != 0) {
      return true;
    }
  }
  return false;
}

void sensei_charge_event(struct sensei_charge *ch, unsigned id, uint32_t n) {
  if (id >= ch->table->n_events) {
    return;
  }
  uint32_t sum = ch->event_cnt[id] + n;

  ch->event_cnt[id] = sum < n ? UINT32_MAX : sum;
  if (!ch->ext_power) {
    add_used(ch, (uint64_t)ch->table->event_nc[id] * n);
  }
}

/* ---- battery ---- */

void sensei_charge_new(struct sensei_charge *ch, uint32_t now_ms, uint32_t capacity_mah, uint32_t used_mah,
                       uint32_t battery_id) {
  sensei_charge_tick(ch, now_ms);
  ch->used_nc = (uint64_t)used_mah * UC_PER_MAH * NC_PER_UC;
  ch->sd_nc = 0;
  ch->restore_loss_uc = 0;
  ch->battery_id = battery_id;
  ch->flags = (uint16_t)(ch->flags & SENSEI_CHARGE_F_CAPACITY_DEFAULT);
  if (capacity_mah != 0) {
    ch->capacity_mah = capacity_mah;
    ch->flags &= (uint16_t)~SENSEI_CHARGE_F_CAPACITY_DEFAULT;
  }
  ch->last_vbat_mv = (uint16_t)(ch->vbat_mv > 0xFFFF ? 0xFFFF : ch->vbat_mv);
  memset(ch->state_ms, 0, sizeof(ch->state_ms));
  memset(ch->event_cnt, 0, sizeof(ch->event_cnt));
  memset(ch->check, 0, sizeof(ch->check));
  ch->underflow = 0;
  ch->win_nc = 0;
  ch->win_ms = 0;
  ch->avg_ua = 0;
  ch->avg_n = 0;
  ch->floor_extra_ua = 0;
  ch->floor_high_n = 0;
  ch->floor_ok_n = 0;
  ch->bo_h = 0;
  ch->chg_on = false;
}

uint64_t sensei_charge_used_uc(const struct sensei_charge *ch) { return (ch->used_nc + ch->sd_nc) / NC_PER_UC; }

uint16_t sensei_charge_flags(const struct sensei_charge *ch) { return ch->flags; }

void sensei_charge_flags_update(struct sensei_charge *ch, uint16_t set, uint16_t clear) {
  ch->flags = (uint16_t)((ch->flags | set) & ~clear);
}

void sensei_charge_report(struct sensei_charge *ch, uint32_t now_ms, struct sensei_charge_report *r) {
  uint64_t used = 0;
  uint64_t cap;
  bool eol;

  sensei_charge_tick(ch, now_ms);
  used = sensei_charge_used_uc(ch);
  cap = cap_uc(ch);
  eol = (ch->flags & SENSEI_CHARGE_F_VOLTAGE_EOL) != 0;

  r->used_mah = (uint32_t)((used + UC_PER_MAH / 2) / UC_PER_MAH);
  r->capacity_mah = ch->capacity_mah;
  r->flags = ch->flags;
  r->battery_id = ch->battery_id;
  r->avg_ua = ch->avg_ua;

  if (cap == 0) {
    r->pct = SENSEI_CHARGE_PCT_UNKNOWN;
  } else if (used >= cap) {
    r->pct = 0;
  } else {
    r->pct = (uint8_t)(((cap - used) * 100 + cap / 2) / cap);
    if (eol && r->pct > 5) {
      r->pct = 5;
    }
  }

  if (eol) {
    r->days_left = 0;
  } else if (ch->avg_n == 0 || ch->avg_ua == 0 || cap == 0) {
    r->days_left = SENSEI_CHARGE_DAYS_UNKNOWN;
  } else {
    uint64_t usable = cap * ch->usable_pct / 100;

    if (used >= usable) {
      r->days_left = 0;
    } else {
      uint64_t days = (usable - used) / ch->avg_ua / 86400u;

      r->days_left = days >= SENSEI_CHARGE_DAYS_UNKNOWN ? SENSEI_CHARGE_DAYS_UNKNOWN - 1 : (uint16_t)days;
    }
  }

  /* 1 sigma: table +-7 % of the counted charge, self-discharge +-20 %, plus what the restores may have lost */
  uint64_t a = ch->used_nc / NC_PER_UC * 7 / 100;
  uint64_t b = ch->sd_nc / NC_PER_UC * 20 / 100;
  uint64_t c = ch->restore_loss_uc;
  uint64_t err = isqrt64(a * a + b * b + c * c);

  r->err_mah = (uint32_t)((err + UC_PER_MAH / 2) / UC_PER_MAH);
}

/* ---- sanity checks ---- */

uint32_t sensei_charge_check_expected_ua(const struct sensei_charge *ch, unsigned cls) {
  if (cls >= ch->table->n_checks || ch->table->check_ua == NULL || ch->table->check_ua[cls] == 0) {
    return 0;
  }
  return (uint32_t)((uint64_t)(ch->table->check_ua[cls] + ch->floor_ua + ch->addon_ua) * SENSEI_CHARGE_REF_MV /
                    vbat_or_ref(ch));
}

static bool check_off_ratio(const struct sensei_charge_check *c) {
  unsigned dev = c->mean_pm > 1000 ? c->mean_pm - 1000u : 1000u - c->mean_pm;

  return (c->n >= CHECK_MIN_N && dev > 300) || (c->rejected > CHECK_MIN_N && c->rejected > c->n);
}

void sensei_charge_check_sample(struct sensei_charge *ch, unsigned cls, uint32_t measured_ua) {
  uint32_t exp = sensei_charge_check_expected_ua(ch, cls);
  struct sensei_charge_check *c;
  uint64_t ratio;

  if (exp == 0) {
    return;
  }
  c = &ch->check[cls];
  ratio = (uint64_t)measured_ua * 1000u / exp;
  if (ratio < 250 || ratio > 4000) {
    if (c->rejected < UINT16_MAX) {
      c->rejected++;
    }
  } else {
    if (c->n < CHECK_N_MAX) {
      c->n++;
    }
    if (c->n == 1) {
      c->mean_q8 = (uint32_t)ratio << 8;
    } else {
      c->mean_q8 = (uint32_t)((int64_t)c->mean_q8 + (((int64_t)ratio << 8) - (int64_t)c->mean_q8) / (int64_t)c->n);
    }
    c->mean_pm = (uint16_t)(c->mean_q8 >> 8);
  }
  bool bad = false;

  for (unsigned i = 0; i < ch->table->n_checks; i++) {
    bad = bad || check_off_ratio(&ch->check[i]);
  }
  if (bad) {
    ch->flags |= SENSEI_CHARGE_F_CHECK_MISMATCH;
  } else {
    ch->flags &= (uint16_t)~SENSEI_CHARGE_F_CHECK_MISMATCH;
  }
}

bool sensei_charge_floor_sample(struct sensei_charge *ch, uint32_t now_ms, uint32_t measured_ua) {
  uint32_t exp;

  sensei_charge_tick(ch, now_ms);
  if (sensei_charge_any_state(ch) || ch->ext_power) {
    return false;
  }
  exp = (uint32_t)((uint64_t)(ch->floor_ua + ch->addon_ua) * SENSEI_CHARGE_REF_MV / vbat_or_ref(ch));
  if (measured_ua > exp + ch->floor_margin_ua) {
    ch->floor_ok_n = 0;
    if (ch->floor_high_n < 255) {
      ch->floor_high_n++;
    }
    if (ch->floor_high_n >= 3) {
      ch->flags |= SENSEI_CHARGE_F_FLOOR_HIGH;
      ch->floor_extra_ua = (uint32_t)((uint64_t)(measured_ua - exp) * vbat_or_ref(ch) / SENSEI_CHARGE_REF_MV);
    }
  } else {
    ch->floor_high_n = 0;
    if (ch->floor_ok_n < 255) {
      ch->floor_ok_n++;
    }
    if (ch->floor_ok_n >= 3) {
      ch->flags &= (uint16_t)~SENSEI_CHARGE_F_FLOOR_HIGH;
      ch->floor_extra_ua = 0;
    }
  }
  return true;
}

/* ---- Li-ion ---- */

void sensei_charge_liion_charge(struct sensei_charge *ch, uint32_t now_ms, uint32_t chg_ua) {
  sensei_charge_tick(ch, now_ms);
  if (ch->chem != SENSEI_CHARGE_LIION) {
    return;
  }
  if (ch->chg_on) {
    int32_t dt = (int32_t)(now_ms - ch->chg_ms);

    if (dt > 0) {
      uint64_t nc = (uint64_t)((ch->chg_prev_ua + (uint64_t)chg_ua) / 2) * (uint32_t)dt;

      ch->used_nc = nc >= ch->used_nc ? 0 : ch->used_nc - nc;
    }
  }
  ch->chg_on = true;
  ch->chg_ms = now_ms;
  ch->chg_prev_ua = chg_ua;
}

void sensei_charge_liion_charge_end(struct sensei_charge *ch) { ch->chg_on = false; }

void sensei_charge_liion_done(struct sensei_charge *ch, uint32_t now_ms) {
  sensei_charge_tick(ch, now_ms);
  if (ch->chem != SENSEI_CHARGE_LIION) {
    return;
  }
  ch->used_nc = 0;
  ch->sd_nc = 0;
  ch->restore_loss_uc = 0;
  ch->chg_on = false;
  ch->flags = (uint16_t)((ch->flags & ~(SENSEI_CHARGE_F_RESTORED | SENSEI_CHARGE_F_UNCONFIRMED |
                                        SENSEI_CHARGE_F_AUTO_SWAP | SENSEI_CHARGE_F_VOLTAGE_EOL)) |
                         SENSEI_CHARGE_F_LIION_SYNCED);
}

void sensei_charge_liion_empty(struct sensei_charge *ch, uint32_t now_ms) {
  uint64_t used_mah;

  sensei_charge_tick(ch, now_ms);
  if (ch->chem != SENSEI_CHARGE_LIION || (ch->flags & SENSEI_CHARGE_F_LIION_SYNCED) == 0) {
    return;
  }
  used_mah = sensei_charge_used_uc(ch) / UC_PER_MAH;
  /* a sane capacity only: between 30 % and 200 % of what was known (any value when none was) */
  if (used_mah == 0 || used_mah > 0xFFFFFFFFull ||
      (ch->capacity_mah != 0 &&
       (used_mah * 100 < (uint64_t)ch->capacity_mah * 30 || used_mah * 100 > (uint64_t)ch->capacity_mah * 200))) {
    return;
  }
  ch->capacity_mah = (uint32_t)used_mah;
  ch->liion_full_nc = ch->used_nc;
  ch->flags &= (uint16_t)~(SENSEI_CHARGE_F_LIION_SYNCED | SENSEI_CHARGE_F_CAPACITY_DEFAULT);
}

void sensei_charge_liion_ocv(struct sensei_charge *ch, uint32_t now_ms, uint8_t ocv_pct) {
  uint64_t cap = cap_uc(ch);

  sensei_charge_tick(ch, now_ms);
  if (ch->chem != SENSEI_CHARGE_LIION || cap == 0 || (ocv_pct >= 20 && ocv_pct <= 80) || ocv_pct > 100) {
    return;
  }
  int64_t target = (int64_t)(cap * (100u - ocv_pct) / 100u * NC_PER_UC);
  int64_t now_nc = (int64_t)ch->used_nc;
  int64_t next = now_nc + (target - now_nc) / 8;

  ch->used_nc = next < 0 ? 0 : (uint64_t)next;
}

/* ---- end of life and swap ---- */

void sensei_charge_set_eol(struct sensei_charge *ch, bool eol) {
  if (eol) {
    ch->flags |= SENSEI_CHARGE_F_VOLTAGE_EOL;
  } else {
    ch->flags &= (uint16_t)~SENSEI_CHARGE_F_VOLTAGE_EOL;
  }
}

void sensei_charge_brownout(struct sensei_charge *ch, uint32_t wall_s) {
  uint16_t h;

  if (wall_s == 0) {
    return; /* no wall clock: two brown-outs within 24 h cannot be told */
  }
  h = (uint16_t)(wall_s / 3600u);
  if (h == 0) {
    h = 1;
  }
  if (ch->bo_h != 0 && (uint16_t)(h - ch->bo_h) <= 24) {
    ch->flags |= SENSEI_CHARGE_F_VOLTAGE_EOL;
  }
  ch->bo_h = h;
}

bool sensei_charge_btn_press(struct sensei_charge_btn *b, uint32_t now_ms, uint32_t since_boot_ms) {
  if (since_boot_ms > SENSEI_CHARGE_BTN_BOOT_MS) {
    b->n = 0;
    return false;
  }
  if (b->n < 3) {
    b->t[b->n++] = now_ms;
  } else {
    b->t[0] = b->t[1];
    b->t[1] = b->t[2];
    b->t[2] = now_ms;
  }
  if (b->n == 3 && (int32_t)(b->t[2] - b->t[0]) <= (int32_t)SENSEI_CHARGE_BTN_WINDOW_MS &&
      (int32_t)(b->t[2] - b->t[0]) >= 0) {
    b->n = 0;
    return true;
  }
  return false;
}

/* ---- record ---- */

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}

static void put64(uint8_t *p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16); }

static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

uint16_t sensei_charge_crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;

  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)((uint16_t)data[i] << 8);
    for (int b = 0; b < 8; b++) {
      crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
  }
  return crc;
}

/*
 *  0  u8   magic 0xC5
 *  1  u8   version 1
 *  2  u16  flags (bits 0..7 SENSEI_CHARGE_F_*, bit 8 Li-ion)
 *  4  u64  used charge in uC (counter, without self-discharge)
 * 12  u32  self-discharge in mC
 * 16  u32  battery id
 * 20  u16  capacity in mAh
 * 22  u16  last battery voltage in mV
 * 24  u32  wall time of the save (unix s, 0 unknown)
 * 28  u16  hour of the last brown-out
 * 30  u16  CRC-16 of bytes 0..29
 */
void sensei_charge_pack(const struct sensei_charge *ch, uint32_t wall_s, uint8_t *out) {
  uint32_t cap = ch->capacity_mah > 0xFFFF ? 0xFFFF : ch->capacity_mah;
  uint16_t flags = (uint16_t)(ch->flags & SENSEI_CHARGE_F_STICKY);
  uint64_t sd_mc = ch->sd_nc / NC_PER_MC;

  memset(out, 0, SENSEI_CHARGE_RECORD_BYTES);
  out[0] = REC_MAGIC;
  out[1] = REC_VERSION;
  if (ch->chem == SENSEI_CHARGE_LIION) {
    flags |= 0x100;
  }
  put16(out + 2, flags);
  put64(out + 4, ch->used_nc / NC_PER_UC);
  put32(out + 12, sd_mc > UINT32_MAX ? UINT32_MAX : (uint32_t)sd_mc);
  put32(out + 16, ch->battery_id);
  put16(out + 20, (uint16_t)cap);
  put16(out + 22, ch->last_vbat_mv);
  put32(out + 24, wall_s);
  put16(out + 28, ch->bo_h);
  put16(out + 30, sensei_charge_crc16(out, 30));
}

int sensei_charge_unpack(const uint8_t *in, struct sensei_charge_record *rec) {
  if (in == NULL || rec == NULL) {
    return -EINVAL;
  }
  if (in[0] != REC_MAGIC || in[1] != REC_VERSION) {
    return -ENOMSG;
  }
  if (get16(in + 30) != sensei_charge_crc16(in, 30)) {
    return -EBADMSG;
  }
  uint16_t flags = get16(in + 2);

  rec->flags = (uint16_t)(flags & 0xFF);
  rec->liion = (flags & 0x100) != 0;
  rec->used_uc = get64(in + 4);
  rec->sd_mc = get32(in + 12);
  rec->battery_id = get32(in + 16);
  rec->capacity_mah = get16(in + 20);
  rec->last_vbat_mv = get16(in + 22);
  rec->wall_s = get32(in + 24);
  rec->bo_h = get16(in + 28);
  return 0;
}

/*
 *  0  u8   magic 0xC6
 *  1  u8   version 1
 *  2  u8   number of states n
 *  3  u8   number of events m
 *  4  u32 x n   seconds per state
 *  .  u32 x m   counts per event
 *  .  u16  CRC-16 of all before
 */
size_t sensei_charge_ledger_pack(const struct sensei_charge *ch, uint8_t *out, size_t max) {
  size_t n = ch->table->n_states;
  size_t m = ch->table->n_events;
  size_t len = 4 + 4 * n + 4 * m + 2;
  size_t o = 4;

  if (max < len) {
    return 0;
  }
  out[0] = LEDGER_MAGIC;
  out[1] = LEDGER_VERSION;
  out[2] = (uint8_t)n;
  out[3] = (uint8_t)m;
  for (size_t i = 0; i < n; i++, o += 4) {
    uint64_t s = ch->state_ms[i] / 1000u;

    put32(out + o, s > UINT32_MAX ? UINT32_MAX : (uint32_t)s);
  }
  for (size_t i = 0; i < m; i++, o += 4) {
    put32(out + o, ch->event_cnt[i]);
  }
  put16(out + o, sensei_charge_crc16(out, o));
  return len;
}

int sensei_charge_ledger_unpack(struct sensei_charge *ch, const uint8_t *in, size_t len) {
  size_t n = ch->table->n_states;
  size_t m = ch->table->n_events;
  size_t o = 4;

  if (in == NULL || len < 6) {
    return -EINVAL;
  }
  if (in[0] != LEDGER_MAGIC || in[1] != LEDGER_VERSION || in[2] != n || in[3] != m || len != 4 + 4 * n + 4 * m + 2) {
    return -ENOMSG;
  }
  if (get16(in + len - 2) != sensei_charge_crc16(in, len - 2)) {
    return -EBADMSG;
  }
  for (size_t i = 0; i < n; i++, o += 4) {
    ch->state_ms[i] = (uint64_t)get32(in + o) * 1000u;
  }
  for (size_t i = 0; i < m; i++, o += 4) {
    ch->event_cnt[i] = get32(in + o);
  }
  return 0;
}

enum sensei_charge_source sensei_charge_boot(struct sensei_charge *ch, const uint8_t *ret, const uint8_t *set,
                                             bool power_on_reset, uint32_t rested_vbat_mv, uint32_t now_ms) {
  struct sensei_charge_record rec;
  enum sensei_charge_source src = SENSEI_CHARGE_SRC_NONE;
  bool cap_set = (ch->flags & SENSEI_CHARGE_F_CAPACITY_DEFAULT) == 0; /* a capacity setting is in force */

  if (ret != NULL && sensei_charge_unpack(ret, &rec) == 0) {
    src = SENSEI_CHARGE_SRC_RETAINED;
  } else if (set != NULL && sensei_charge_unpack(set, &rec) == 0) {
    src = SENSEI_CHARGE_SRC_SETTINGS;
  }

  ch->started = false;
  sensei_charge_tick(ch, now_ms);
  if (rested_vbat_mv != 0) {
    ch->vbat_mv = rested_vbat_mv;
  }

  if (src != SENSEI_CHARGE_SRC_NONE) {
    ch->used_nc = rec.used_uc * NC_PER_UC;
    ch->sd_nc = (uint64_t)rec.sd_mc * NC_PER_MC;
    ch->battery_id = rec.battery_id;
    ch->last_vbat_mv = rec.last_vbat_mv;
    ch->bo_h = rec.bo_h;
    ch->flags = (uint16_t)((ch->flags & ~SENSEI_CHARGE_F_STICKY) | (rec.flags & SENSEI_CHARGE_F_STICKY));
    if (!cap_set) {
      if (rec.capacity_mah != 0 && (rec.flags & SENSEI_CHARGE_F_CAPACITY_DEFAULT) == 0) {
        ch->capacity_mah = rec.capacity_mah;
        ch->flags &= (uint16_t)~SENSEI_CHARGE_F_CAPACITY_DEFAULT;
      } else {
        ch->flags |= SENSEI_CHARGE_F_CAPACITY_DEFAULT;
      }
    } else {
      ch->flags &= (uint16_t)~SENSEI_CHARGE_F_CAPACITY_DEFAULT;
    }
    if (src == SENSEI_CHARGE_SRC_SETTINGS) {
      ch->flags |= SENSEI_CHARGE_F_RESTORED;
      ch->restore_loss_uc += (uint64_t)(ch->floor_ua + ch->addon_ua) * LOSS_RESTORE_S;
    }
  } else {
    ch->flags |= SENSEI_CHARGE_F_UNCONFIRMED;
  }

  if (power_on_reset && src != SENSEI_CHARGE_SRC_RETAINED) {
    if (src != SENSEI_CHARGE_SRC_NONE && ch->last_vbat_mv != 0 && rested_vbat_mv >= ch->last_vbat_mv + SWAP_STEP_MV) {
      uint16_t keep = (uint16_t)(ch->flags & SENSEI_CHARGE_F_CAPACITY_DEFAULT);

      ch->used_nc = 0;
      ch->sd_nc = 0;
      ch->restore_loss_uc = 0;
      ch->battery_id = 0;
      ch->bo_h = 0;
      ch->flags = (uint16_t)(keep | SENSEI_CHARGE_F_AUTO_SWAP);
      memset(ch->state_ms, 0, sizeof(ch->state_ms));
      memset(ch->event_cnt, 0, sizeof(ch->event_cnt));
    } else {
      ch->flags |= SENSEI_CHARGE_F_UNCONFIRMED;
    }
  }
  if (rested_vbat_mv != 0) {
    ch->last_vbat_mv = (uint16_t)(rested_vbat_mv > 0xFFFF ? 0xFFFF : rested_vbat_mv);
  }
  return src;
}
