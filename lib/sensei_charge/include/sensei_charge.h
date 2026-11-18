/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_charge.h
 * @brief Software charge counter of a battery without a fuel gauge. Plain C, no Zephyr, no locking: the caller
 * serialises the calls and passes the uptime (ms, 32 bit, wraps after 49 days; every call must be less than 24 days
 * after the last one, the power thread ticks every 20 s).
 *
 * used = floor + sum(time in state x current above floor) + sum(events x charge per event) + self-discharge.
 *
 *  - States are ref-counted: sensei_charge_state(id, true) twice needs two calls with false. They cost current while
 *    at least one reference is held.
 *  - Events are jobs of known charge: sensei_charge_event(id, n) adds n times the charge of the table.
 *  - The table (const, from the application) holds uA and nC at 3.6 V. The rails sit behind a buck-boost, so the
 *    battery current is that value times 3600 / V_bat (sensei_charge_set_vbat()).
 *  - The raw counters (seconds per state, counts per event) are kept too (the ledger), so that a host can apply a
 *    better table to the history.
 *  - Units: 1 uA x 1 ms = 1 nC.
 */

#ifndef SENSEI_CHARGE_H
#define SENSEI_CHARGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSEI_CHARGE_MAX_STATES 12
#define SENSEI_CHARGE_MAX_EVENTS 24
#define SENSEI_CHARGE_MAX_CHECKS 6

/** Size of the record in the retained RAM and in the settings. */
#define SENSEI_CHARGE_RECORD_BYTES 32
/** Largest size of the ledger blob (counters per state and event, with header and CRC). */
#define SENSEI_CHARGE_LEDGER_MAX_BYTES (4 + 4 * SENSEI_CHARGE_MAX_STATES + 4 * SENSEI_CHARGE_MAX_EVENTS + 2)

/** Voltage at which the table values are given. */
#define SENSEI_CHARGE_REF_MV 3600u

/** Status flags (charge_flags). */
#define SENSEI_CHARGE_F_RESTORED                                                                                       \
  (1u << 0) /**< the counter came from the settings after a power loss (up to 6 h lost) */
#define SENSEI_CHARGE_F_UNCONFIRMED (1u << 1) /**< power was lost and nobody declared the battery: may be a new one */
#define SENSEI_CHARGE_F_AUTO_SWAP (1u << 2)   /**< a new battery was detected by its voltage and the counter reset */
#define SENSEI_CHARGE_F_FLOOR_HIGH                                                                                     \
  (1u << 3) /**< the idle current is above the floor: the measured current is counted */
#define SENSEI_CHARGE_F_CHECK_MISMATCH (1u << 4)   /**< the AMUX samples disagree with the table by more than 30 % */
#define SENSEI_CHARGE_F_VOLTAGE_EOL (1u << 5)      /**< voltage under load or brown-outs say the cell is at its end */
#define SENSEI_CHARGE_F_CAPACITY_DEFAULT (1u << 6) /**< the capacity is the build default, nobody declared it */
#define SENSEI_CHARGE_F_LIION_SYNCED (1u << 7)     /**< Li-ion: the counter was reset by "charge done" */
/** Flags that describe the battery and stay in the record. The others are worked out again at start. */
#define SENSEI_CHARGE_F_STICKY                                                                                         \
  (SENSEI_CHARGE_F_RESTORED | SENSEI_CHARGE_F_UNCONFIRMED | SENSEI_CHARGE_F_AUTO_SWAP | SENSEI_CHARGE_F_VOLTAGE_EOL |  \
   SENSEI_CHARGE_F_CAPACITY_DEFAULT | SENSEI_CHARGE_F_LIION_SYNCED)

/** Battery chemistry. */
enum sensei_charge_chem {
  SENSEI_CHARGE_LISOCL2, /**< primary cell: self-discharge counted, voltage never corrects the counter */
  SENSEI_CHARGE_LIION,   /**< rechargeable: charge current integrated, "charge done" resets the counter */
};

/** Consumption table of the application (arrays outlive the counter). Values at 3.6 V. */
struct sensei_charge_table {
  uint8_t n_states;         /**< entries of state_ua, at most SENSEI_CHARGE_MAX_STATES */
  uint8_t n_events;         /**< entries of event_nc, at most SENSEI_CHARGE_MAX_EVENTS */
  uint8_t n_checks;         /**< entries of check_ua, at most SENSEI_CHARGE_MAX_CHECKS (may be 0) */
  const uint32_t *state_ua; /**< current above the floor while the state is on (uA) */
  const uint32_t *event_nc; /**< charge of one count above the floor (nC) */
  const uint32_t *check_ua; /**< expected battery current above the floor at the AMUX sample point, 0: check off */
};

/** One class of AMUX sanity check: running mean of measured / expected. */
struct sensei_charge_check {
  uint16_t n;        /**< samples taken into the mean (saturates at 1024, then it is a moving average) */
  uint16_t mean_pm;  /**< mean of measured / expected in permille */
  uint32_t mean_q8;  /**< the same with 8 more bits: a moving average of 1024 samples needs the resolution */
  uint16_t rejected; /**< samples thrown away (the job was not running, or a bad reading) */
};

/** Counter. Fill with sensei_charge_init(), do not touch the fields. */
struct sensei_charge {
  const struct sensei_charge_table *table;
  enum sensei_charge_chem chem;

  /* settings */
  uint32_t capacity_mah;
  uint32_t default_capacity_mah;
  uint8_t usable_pct;
  uint32_t floor_ua;        /**< board floor above nothing, at 3.6 V */
  uint32_t addon_ua;        /**< constant extras of the settings, at 3.6 V */
  uint32_t floor_margin_ua; /**< floor watchdog: samples this far above the floor count as high */

  /* inputs */
  uint32_t vbat_mv;
  int16_t temp_c;
  bool ext_power;
  uint32_t last_ms;
  bool started;

  /* counter */
  uint64_t used_nc;         /**< consumed charge, voltage scaled, without self-discharge */
  uint64_t sd_nc;           /**< self-discharge */
  uint64_t restore_loss_uc; /**< estimate of the charge lost over restores (adds to the error) */
  uint32_t battery_id;
  uint16_t last_vbat_mv;
  uint16_t bo_h; /**< hour (wall time / 3600, low 16 bit) of the last brown-out, 0: none */
  uint16_t flags;
  bool chg_on;          /**< a Li-ion charge sample series is running */
  uint32_t chg_ms;      /**< time of the last Li-ion charge sample */
  uint32_t chg_prev_ua; /**< and its current */

  /* ledger (since the declaration, never voltage scaled) */
  uint8_t ref[SENSEI_CHARGE_MAX_STATES];
  uint64_t state_ms[SENSEI_CHARGE_MAX_STATES];
  uint32_t event_cnt[SENSEI_CHARGE_MAX_EVENTS];
  uint32_t underflow; /**< sensei_charge_state(false) without a matching on */

  /* average */
  uint64_t win_nc;
  uint32_t win_ms;
  uint32_t avg_ua;
  uint16_t avg_n;

  /* floor watchdog */
  uint32_t floor_extra_ua;
  uint8_t floor_high_n;
  uint8_t floor_ok_n;

  /* AMUX check */
  struct sensei_charge_check check[SENSEI_CHARGE_MAX_CHECKS];

  /* Li-ion */
  uint64_t liion_full_nc; /**< consumed charge when the last discharge reached the empty voltage */
};

/** Values for reporting. */
struct sensei_charge_report {
  uint32_t used_mah;
  uint32_t capacity_mah;
  uint8_t pct;        /**< left, 0..100 of the capacity; 255: no capacity known */
  uint32_t avg_ua;    /**< average battery current, 7-day moving average (0 until the first hour is over) */
  uint16_t days_left; /**< to the usable capacity at the average current; 0xFFFF: not known yet */
  uint32_t err_mah;   /**< 1 sigma of used_mah */
  uint16_t flags;
  uint32_t battery_id;
};

#define SENSEI_CHARGE_DAYS_UNKNOWN 0xFFFFu
#define SENSEI_CHARGE_PCT_UNKNOWN 255u

/** What a start found, see sensei_charge_boot(). */
enum sensei_charge_source {
  SENSEI_CHARGE_SRC_NONE,     /**< no record: first start of this node, or both were damaged */
  SENSEI_CHARGE_SRC_RETAINED, /**< the retained RAM record was valid */
  SENSEI_CHARGE_SRC_SETTINGS, /**< the retained RAM was lost, the settings record was used */
};

/** Start of a counter. Clears it. Chemistry and table are fixed. default_capacity_mah is used while no capacity is set.
 */
void sensei_charge_init(struct sensei_charge *ch, const struct sensei_charge_table *table, enum sensei_charge_chem chem,
                        uint32_t default_capacity_mah);

/** Settings: capacity (0: the default, flagged; a capacity that was declared or learned stays), usable percent of it,
 * floor current at 3.6 V (uA), add-on at 3.6 V. Call sensei_charge_tick() first: a new floor applies from the next
 * tick. */
void sensei_charge_set_params(struct sensei_charge *ch, uint32_t capacity_mah, uint8_t usable_pct, uint32_t floor_ua,
                              uint32_t addon_ua);

/** Floor watchdog margin (uA, default 2000). */
void sensei_charge_set_floor_margin(struct sensei_charge *ch, uint32_t margin_ua);

/** Account up to now: floor, states, self-discharge and the averages. Call at least every 24 days, every 20 s in the
 * field. */
void sensei_charge_tick(struct sensei_charge *ch, uint32_t now_ms);

/** Battery voltage (mV). Accounts up to now at the old voltage first. 0 is ignored. */
void sensei_charge_set_vbat(struct sensei_charge *ch, uint32_t now_ms, uint32_t mv);

/** Battery temperature in 0.1 degC for the self-discharge. Accounts up to now first. */
void sensei_charge_set_temp(struct sensei_charge *ch, uint32_t now_ms, int32_t t_c10);

/** External power (USB) present: the battery does not supply the node, consumption is not counted (the ledger is). */
void sensei_charge_set_ext_power(struct sensei_charge *ch, uint32_t now_ms, bool present);

/** A state turns on or off (ref-counted). Unknown ids are ignored. An off without an on is counted in underflow. */
void sensei_charge_state(struct sensei_charge *ch, uint32_t now_ms, unsigned id, bool on);

/** Number of references held by a state. */
unsigned sensei_charge_state_refs(const struct sensei_charge *ch, unsigned id);

/** True while any state is on. */
bool sensei_charge_any_state(const struct sensei_charge *ch);

/** An event happened n times (a job, a packet, n bytes). */
void sensei_charge_event(struct sensei_charge *ch, unsigned id, uint32_t n);

/** A new battery: reset to used_mah (0 for a fresh one), capacity_mah 0 keeps the default, clears the flags and the
 * ledger. */
void sensei_charge_new(struct sensei_charge *ch, uint32_t now_ms, uint32_t capacity_mah, uint32_t used_mah,
                       uint32_t battery_id);

/** Report. Ticks first. */
void sensei_charge_report(struct sensei_charge *ch, uint32_t now_ms, struct sensei_charge_report *r);

/** Total used charge in uC (counter and self-discharge). */
uint64_t sensei_charge_used_uc(const struct sensei_charge *ch);

/** Current flags. */
uint16_t sensei_charge_flags(const struct sensei_charge *ch);
/** Set and clear flags (SENSEI_CHARGE_F_*). */
void sensei_charge_flags_update(struct sensei_charge *ch, uint16_t set, uint16_t clear);

/* ---- sanity checks ---- */

/** Expected battery current in uA of a check class at the present voltage (floor, add-on and table), 0: check off. */
uint32_t sensei_charge_check_expected_ua(const struct sensei_charge *ch, unsigned cls);

/**
 * AMUX sample of the battery current taken while the job of the class runs. Samples that are under 25 % or over 400 %
 * of the expected current count as rejected. Once 200 samples are in, a mean outside +-30 % (or more rejected than
 * accepted samples) sets CHECK_MISMATCH. Only a sanity check: it never changes the counter.
 */
void sensei_charge_check_sample(struct sensei_charge *ch, unsigned cls, uint32_t measured_ua);

/**
 * Sample of the idle current, to be taken when no state is on. Three samples in a row more than the margin above the
 * floor set FLOOR_HIGH and the measured excess is counted from then on; three normal ones clear it again.
 * Ignored while a state is on or external power is present. Returns true if the sample was used.
 */
bool sensei_charge_floor_sample(struct sensei_charge *ch, uint32_t now_ms, uint32_t measured_ua);

/* ---- Li-ion ---- */

/** Charge current into the battery (uA) measured now; it reduces the used charge for the time since the last sample. */
void sensei_charge_liion_charge(struct sensei_charge *ch, uint32_t now_ms, uint32_t chg_ua);
/** The charge sample series ends (charger off or input gone): the next sample starts a new interval. */
void sensei_charge_liion_charge_end(struct sensei_charge *ch);
/** The charger reported "done": the battery is full, used = 0, LIION_SYNCED. Learns nothing. */
void sensei_charge_liion_done(struct sensei_charge *ch, uint32_t now_ms);
/** The battery reached the empty voltage (3.3 V) after a synced full charge: its charge becomes the capacity. */
void sensei_charge_liion_empty(struct sensei_charge *ch, uint32_t now_ms);
/**
 * Pull towards the open-circuit voltage: call after at least 30 minutes without load with the charge level that the
 * voltage says (ocv_pct). Only below 20 % or above 80 % it moves the counter, by 1/8 of the difference.
 */
void sensei_charge_liion_ocv(struct sensei_charge *ch, uint32_t now_ms, uint8_t ocv_pct);

/* ---- end of life and swap ---- */

/** Voltage under load or another reason says the cell is at its end (sets or clears VOLTAGE_EOL). */
void sensei_charge_set_eol(struct sensei_charge *ch, bool eol);
/** A brown-out reset at wall time wall_s (0: unknown): the second one within 24 h sets VOLTAGE_EOL. */
void sensei_charge_brownout(struct sensei_charge *ch, uint32_t wall_s);

/** Presses of the swap button: three within 3 s, only within 5 min after a power-on reset. */
struct sensei_charge_btn {
  uint32_t t[3];
  uint8_t n;
};
#define SENSEI_CHARGE_BTN_WINDOW_MS 3000u
#define SENSEI_CHARGE_BTN_BOOT_MS (5u * 60u * 1000u)
/** A short press at now_ms, since_boot_ms after a power-on reset. True on the third press: the caller declares a new
 * battery. */
bool sensei_charge_btn_press(struct sensei_charge_btn *b, uint32_t now_ms, uint32_t since_boot_ms);

/* ---- record ---- */

/** Fixed fields of the record. */
struct sensei_charge_record {
  uint64_t used_uc;
  uint32_t sd_mc;
  uint32_t battery_id;
  uint32_t capacity_mah;
  uint16_t last_vbat_mv;
  uint16_t flags;
  uint32_t wall_s;
  uint16_t bo_h;
  bool liion;
};

/** CRC-16/CCITT-FALSE. */
uint16_t sensei_charge_crc16(const uint8_t *data, size_t len);

/** Take the counter into out[SENSEI_CHARGE_RECORD_BYTES] (little endian, magic, version, CRC-16). wall_s: unix time, 0
 * unknown. */
void sensei_charge_pack(const struct sensei_charge *ch, uint32_t wall_s, uint8_t *out);
/** Read a record. 0, -EINVAL (NULL), -ENOMSG (no magic or another version), -EBADMSG (CRC). */
int sensei_charge_unpack(const uint8_t *in, struct sensei_charge_record *rec);

/** Ledger blob of the counters (states, events). Returns the length. */
size_t sensei_charge_ledger_pack(const struct sensei_charge *ch, uint8_t *out, size_t max);
/** Read a ledger blob into the counter. 0, -EINVAL, -ENOMSG (another layout), -EBADMSG (CRC). */
int sensei_charge_ledger_unpack(struct sensei_charge *ch, const uint8_t *in, size_t len);

/**
 * Start of the node. Takes the counter from the retained RAM record, else from the settings record (flag RESTORED, the
 * time since the last save is a loss: error grows), else starts at zero. Then, after a power-on reset:
 *  - a rested battery voltage at least 150 mV above the last one is a new battery: reset, flag AUTO_SWAP;
 *  - else, if the retained record was lost, flag UNCONFIRMED (the battery may have been replaced).
 * A first start without any record is UNCONFIRMED too. Brown-out resets are counted by sensei_charge_brownout().
 * ret and set may be NULL. Set the parameters (sensei_charge_set_params()) before it.
 */
enum sensei_charge_source sensei_charge_boot(struct sensei_charge *ch, const uint8_t *ret, const uint8_t *set,
                                             bool power_on_reset, uint32_t rested_vbat_mv, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_CHARGE_H */
