/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Battery charge counter of the power module: lib/sensei_charge on Zephyr. Retained RAM record (chosen
 * sensei,charge, else noinit RAM) written on every pass of the power thread, settings record and ledger
 * ("sensei_charge/rec", "sensei_charge/ledger") every CONFIG_SENSEI_PWR_CHARGE_SAVE_H hours and on request. */

#include "pwr/pwr_charge.h"

#include "pwr/pwr.h"
#include "pwr/pwr_priv.h"

#include "max77654.h"
#include "sensei_pwr_math.h"

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#if DT_HAS_CHOSEN(sensei_charge)
#include <zephyr/retention/retention.h>
static const struct device *const ret_dev = DEVICE_DT_GET(DT_CHOSEN(sensei_charge));
#else
/* No retained RAM in the devicetree: noinit RAM, which usually survives a warm reset. The record has a CRC. */
static __noinit uint8_t ret_ram[SENSEI_CHARGE_RECORD_BYTES];
#endif

LOG_MODULE_REGISTER(pwr_charge);

#define KEY_REC "sensei_charge/rec"
#define KEY_LEDGER "sensei_charge/ledger"
#define SUBTREE "sensei_charge"

#define FLOOR_PERIOD_MS 15000
#define FLOOR_SCALE MAX77654_BATT_I_8MA2
#define OCV_IDLE_MS (30u * 60u * 1000u)
#define OCV_PERIOD_MS (60u * 60u * 1000u)
#define SAVE_PERIOD_MS ((uint32_t)CONFIG_SENSEI_PWR_CHARGE_SAVE_H * 3600u * 1000u)

static struct sensei_charge ch;
static struct k_spinlock lk;
static bool inited;
static struct pwr_charge_cfg cfg;
static struct sensei_charge_btn btn;
static pwr_charge_swap_cb_t swap_cb;

/* settings records found by the load */
static uint8_t nv_rec[SENSEI_CHARGE_RECORD_BYTES];
static bool nv_rec_ok;
static uint8_t nv_led[SENSEI_CHARGE_LEDGER_MAX_BYTES];
static size_t nv_led_len;

/* only used from the power thread (pwr_charge_pmic_tick) */
static uint32_t last_floor_ms;
static uint32_t last_ocv_ms;
static uint32_t last_save_ms;
static enum pwr_charger_state last_charger = (enum pwr_charger_state) - 1;
static uint8_t critical_n; /* passes in a row under the critical voltage */
/* written by the hooks, read by the power thread */
static atomic_t last_active_ms;

static void save_work_fn(struct k_work *w);
static K_WORK_DEFINE(save_work, save_work_fn);

/* ---- settings and retained RAM ---- */

static int nv_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
  if (strcmp(name, "rec") == 0) {
    nv_rec_ok = len == sizeof(nv_rec) && read_cb(cb_arg, nv_rec, sizeof(nv_rec)) == (ssize_t)sizeof(nv_rec);
  } else if (strcmp(name, "ledger") == 0) {
    nv_led_len = 0;
    if (len <= sizeof(nv_led) && read_cb(cb_arg, nv_led, len) == (ssize_t)len) {
      nv_led_len = len;
    }
  }
  return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(sensei_charge, SUBTREE, NULL, nv_set, NULL, NULL);

static uint32_t wall_now(void) { return cfg.wall_s != NULL ? cfg.wall_s() : 0; }

static bool ret_read(uint8_t *rec) {
#if DT_HAS_CHOSEN(sensei_charge)
  return device_is_ready(ret_dev) && retention_read(ret_dev, 0, rec, SENSEI_CHARGE_RECORD_BYTES) == 0;
#else
  memcpy(rec, ret_ram, SENSEI_CHARGE_RECORD_BYTES);
  return true;
#endif
}

static void ret_write(const uint8_t *rec) {
#if DT_HAS_CHOSEN(sensei_charge)
  if (device_is_ready(ret_dev)) {
    (void)retention_write(ret_dev, 0, rec, SENSEI_CHARGE_RECORD_BYTES);
  }
#else
  memcpy(ret_ram, rec, SENSEI_CHARGE_RECORD_BYTES);
#endif
}

/* The counter into the retained RAM. Cheap: no flash, no mutex. */
static void ret_update(void) {
  uint8_t rec[SENSEI_CHARGE_RECORD_BYTES];

  K_SPINLOCK(&lk) { sensei_charge_pack(&ch, wall_now(), rec); }
  ret_write(rec);
}

static int save_all(void) {
  uint8_t rec[SENSEI_CHARGE_RECORD_BYTES];
  uint8_t led[SENSEI_CHARGE_LEDGER_MAX_BYTES];
  size_t n = 0;
  int err;
  int err2;

  K_SPINLOCK(&lk) {
    sensei_charge_pack(&ch, wall_now(), rec);
    n = sensei_charge_ledger_pack(&ch, led, sizeof(led));
  }
  err = settings_save_one(KEY_REC, rec, sizeof(rec));
  err2 = n > 0 ? settings_save_one(KEY_LEDGER, led, n) : 0;
  if (err < 0 || err2 < 0) {
    LOG_ERR("charge counter save failed (%d, %d)", err, err2);
    return err < 0 ? err : err2;
  }
  return 0;
}

static void save_work_fn(struct k_work *w) {
  ARG_UNUSED(w);
  (void)save_all();
}

int pwr_charge_save(void) {
  if (!inited) {
    return -EAGAIN;
  }
  ret_update();
  last_save_ms = k_uptime_get_32();
  return save_all();
}

/* ---- start ---- */

int pwr_charge_init(const struct pwr_charge_cfg *c) {
  uint8_t ret[SENSEI_CHARGE_RECORD_BYTES];
  bool ret_ok;
  enum sensei_charge_source src;
  uint32_t now = k_uptime_get_32();
  struct pwr_status st;

  if (inited) {
    return -EALREADY;
  }
  if (c == NULL || c->table == NULL) {
    return -EINVAL;
  }
  cfg = *c;

  sensei_charge_init(&ch, c->table,
                     IS_ENABLED(CONFIG_SENSEI_PWR_BATTERY_LIION) ? SENSEI_CHARGE_LIION : SENSEI_CHARGE_LISOCL2,
                     CONFIG_SENSEI_PWR_CHARGE_DEFAULT_CAPACITY_MAH);
  sensei_charge_set_params(&ch, c->capacity_mah, (uint8_t)c->usable_pct, c->floor_ua, c->addon_ua);
  sensei_charge_set_floor_margin(&ch, CONFIG_SENSEI_PWR_CHARGE_FLOOR_MARGIN_UA);

  ret_ok = ret_read(ret);
  nv_rec_ok = false;
  nv_led_len = 0;
  (void)settings_load_subtree(SUBTREE);

  src = sensei_charge_boot(&ch, ret_ok ? ret : NULL, nv_rec_ok ? nv_rec : NULL, c->power_on_reset,
                           (uint32_t)atomic_get(&battery_mV), now);
  if (src != SENSEI_CHARGE_SRC_NONE && nv_led_len > 0 && (ch.flags & SENSEI_CHARGE_F_AUTO_SWAP) == 0) {
    (void)sensei_charge_ledger_unpack(&ch, nv_led, nv_led_len);
  }
  if (c->brownout_reset) {
    sensei_charge_brownout(&ch, wall_now());
  }
  if (pwr_get_status(&st) == 0) {
    sensei_charge_set_ext_power(&ch, now, st.chgin == PWR_CHGIN_OK);
  }
  last_save_ms = now;
  atomic_set(&last_active_ms, (atomic_val_t)now);
  inited = true;

  ret_update();
  (void)save_all();
  LOG_INF("charge counter: %s, flags 0x%x",
          src == SENSEI_CHARGE_SRC_RETAINED   ? "retained RAM"
          : src == SENSEI_CHARGE_SRC_SETTINGS ? "settings"
                                              : "new",
          (unsigned)ch.flags);
  return 0;
}

void pwr_charge_configure(uint32_t capacity_mah, uint32_t usable_pct, uint32_t floor_ua, uint32_t addon_ua) {
  if (!inited) {
    return;
  }
  K_SPINLOCK(&lk) {
    sensei_charge_tick(&ch, k_uptime_get_32());
    sensei_charge_set_params(&ch, capacity_mah, (uint8_t)usable_pct, floor_ua, addon_ua);
  }
}

/* ---- hooks ---- */

void pwr_charge_state(unsigned id, bool on) {
  uint32_t now = k_uptime_get_32();

  if (!inited) {
    return;
  }
  atomic_set(&last_active_ms, (atomic_val_t)now);
  K_SPINLOCK(&lk) { sensei_charge_state(&ch, now, id, on); }
}

void pwr_charge_event(unsigned id, uint32_t n) {
  if (!inited) {
    return;
  }
  atomic_set(&last_active_ms, (atomic_val_t)k_uptime_get_32());
  K_SPINLOCK(&lk) { sensei_charge_event(&ch, id, n); }
}

void pwr_charge_set_temp(int32_t t_c10) {
  if (!inited) {
    return;
  }
  K_SPINLOCK(&lk) { sensei_charge_set_temp(&ch, k_uptime_get_32(), t_c10); }
}

int pwr_charge_report(struct sensei_charge_report *r) {
  if (!inited) {
    return -EAGAIN;
  }
  K_SPINLOCK(&lk) { sensei_charge_report(&ch, k_uptime_get_32(), r); }
  return 0;
}

int pwr_charge_ledger(uint32_t *state_s, uint32_t *event_n) {
  if (!inited) {
    return -EAGAIN;
  }
  K_SPINLOCK(&lk) {
    sensei_charge_tick(&ch, k_uptime_get_32());
    for (unsigned i = 0; i < ch.table->n_states; i++) {
      state_s[i] = (uint32_t)(ch.state_ms[i] / 1000u);
    }
    for (unsigned i = 0; i < ch.table->n_events; i++) {
      event_n[i] = ch.event_cnt[i];
    }
  }
  return 0;
}

static void new_battery(uint32_t capacity_mah, uint32_t used_mah, uint32_t battery_id) {
  K_SPINLOCK(&lk) { sensei_charge_new(&ch, k_uptime_get_32(), capacity_mah, used_mah, battery_id); }
  ret_update();
  LOG_INF("new battery: capacity %u mAh, used %u mAh", (unsigned)ch.capacity_mah, (unsigned)used_mah);
}

int pwr_charge_new(uint32_t capacity_mah, uint32_t used_mah, uint32_t battery_id) {
  if (!inited) {
    return -EAGAIN;
  }
  new_battery(capacity_mah, used_mah, battery_id);
  last_save_ms = k_uptime_get_32();
  return save_all();
}

void pwr_charge_set_swap_cb(pwr_charge_swap_cb_t cb) { swap_cb = cb; }

void pwr_charge_button_short(uint32_t now_ms) {
  /* the uptime is the time since the power-on reset; after any other reset the button does not declare a battery */
  uint32_t since_boot = cfg.power_on_reset ? now_ms : UINT32_MAX;
  bool declare;

  if (!inited) {
    return;
  }
  declare = sensei_charge_btn_press(&btn, now_ms, since_boot);
  if (declare) {
    LOG_INF("swap button: new battery");
    new_battery(0, 0, wall_now());
    k_work_submit(&save_work); /* flash: not in the power thread, which holds the power mutex */
    if (swap_cb != NULL) {
      swap_cb();
    }
  }
}

/* ---- AMUX ---- */

/* Smallest full scale of the discharge monitor that is at least twice the expected current. */
static max77654_measure_t scale_for(uint32_t ua) {
  static const struct {
    uint32_t full_ua;
    max77654_measure_t sel;
  } scales[] = {
      {8200, MAX77654_BATT_I_8MA2},     {40500, MAX77654_BATT_I_40MA5},   {72300, MAX77654_BATT_I_72MA3},
      {103400, MAX77654_BATT_I_103MA4}, {134100, MAX77654_BATT_I_134MA1}, {164100, MAX77654_BATT_I_164MA1},
      {193700, MAX77654_BATT_I_193MA7}, {222700, MAX77654_BATT_I_222MA7}, {251200, MAX77654_BATT_I_251MA2},
      {279300, MAX77654_BATT_I_279MA3},
  };

  for (size_t i = 0; i < ARRAY_SIZE(scales); i++) {
    if (scales[i].full_ua >= 2u * ua) {
      return scales[i].sel;
    }
  }
  return MAX77654_BATT_I_300MA;
}

int pwr_charge_check(unsigned cls) {
  uint32_t exp = 0;
  uint32_t ua = 0;
  bool ext = false;
  int err;

  if (!inited) {
    return -EAGAIN;
  }
  K_SPINLOCK(&lk) {
    exp = sensei_charge_check_expected_ua(&ch, cls);
    ext = ch.ext_power;
  }
  if (exp == 0) {
    return -ENOTSUP;
  }
  if (ext) {
    return -EBUSY; /* the battery does not supply the node: the sample would read the wrong thing */
  }
  if (k_mutex_lock(&pwr_mutex, PWR_MUTEX_TIMEOUT) < 0) {
    return -EBUSY;
  }
  err = max77654_measure_dischg_ua(&pmic_h, scale_for(exp), &ua);
  k_mutex_unlock(&pwr_mutex);
  if (err != E_MAX77654_SUCCESS) {
    return -EIO;
  }
  K_SPINLOCK(&lk) { sensei_charge_check_sample(&ch, cls, ua); }
  return 0;
}

/* ---- power thread ---- */

void pwr_charge_pmic_tick(uint32_t bat_mv, bool chgin_ok, bool charging, enum pwr_charger_state charger_state) {
  uint32_t now = k_uptime_get_32();
  bool any = false;
  bool liion = IS_ENABLED(CONFIG_SENSEI_PWR_BATTERY_LIION);
  uint32_t ua = 0;

  if (!inited) {
    return;
  }
  K_SPINLOCK(&lk) {
    if (bat_mv != 0) {
      sensei_charge_set_vbat(&ch, now, bat_mv);
    }
    sensei_charge_set_ext_power(&ch, now, chgin_ok);
    any = sensei_charge_any_state(&ch);
  }

  if (liion) {
    if (chgin_ok && charging) {
      uint32_t pct = 0;

      if (max77654_measure(&pmic_h, MAX77654_BATT_I_CHG, &pct) == E_MAX77654_SUCCESS) {
        ua = pct * CONFIG_SENSEI_PWR_CHARGE_FAST_UA / 100u;
        K_SPINLOCK(&lk) { sensei_charge_liion_charge(&ch, now, ua); }
      }
    } else {
      K_SPINLOCK(&lk) { sensei_charge_liion_charge_end(&ch); }
    }
    /* "done" resets the counter: the battery is full. The first pass counts when the node starts on a full battery. */
    if (charger_state == PWR_CHG_DONE && chgin_ok && last_charger != PWR_CHG_DONE) {
      K_SPINLOCK(&lk) { sensei_charge_liion_done(&ch, now); }
      LOG_INF("charger done: counter reset");
      k_work_submit(&save_work);
    }
    last_charger = charger_state;
    if (!chgin_ok && bat_mv != 0 && bat_mv <= PWR_BAT_LOW_MV) {
      K_SPINLOCK(&lk) { sensei_charge_liion_empty(&ch, now); }
    }
  }

  /* Li-SOCl2 end of life: the voltage under the load of the pass stays under the critical level (three passes in a row,
   * so that a cold spell or a peak is no verdict). The flag stays until a new battery. The counter is never corrected
   * by the voltage; this only says that its percentage cannot be trusted any more (reported 5 % at most, 0 days left).
   */
  if (!liion) {
    if (!chgin_ok && bat_mv != 0 && bat_mv <= PWR_BAT_CRITICAL_MV) {
      if (++critical_n >= 3) {
        K_SPINLOCK(&lk) { sensei_charge_set_eol(&ch, true); }
        critical_n = 3;
      }
    } else {
      critical_n = 0;
    }
  }

  /* floor watchdog: the node is idle, the battery supplies it */
  if (!chgin_ok && !any && (uint32_t)(now - last_floor_ms) >= FLOOR_PERIOD_MS) {
    last_floor_ms = now;
    if (max77654_measure_dischg_ua(&pmic_h, FLOOR_SCALE, &ua) == E_MAX77654_SUCCESS) {
      K_SPINLOCK(&lk) { (void)sensei_charge_floor_sample(&ch, now, ua); }
    }
  }

  K_SPINLOCK(&lk) { sensei_charge_tick(&ch, now); }
  ret_update();

  /* Li-ion: after 30 minutes without any state or event the voltage is close to the open-circuit voltage */
  if (liion && !chgin_ok && !any && bat_mv != 0 &&
      (uint32_t)(now - (uint32_t)atomic_get(&last_active_ms)) >= OCV_IDLE_MS &&
      (uint32_t)(now - last_ocv_ms) >= OCV_PERIOD_MS) {
    last_ocv_ms = now;
    K_SPINLOCK(&lk) { sensei_charge_liion_ocv(&ch, now, sensei_battery_perc(SENSEI_BATTERY_LIION, bat_mv)); }
  }

  if ((uint32_t)(now - last_save_ms) >= SAVE_PERIOD_MS) {
    last_save_ms = now;
    k_work_submit(&save_work);
  }
}
