/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file app.h
 * @brief Shared declarations of the time sync example.
 */

#ifndef APP_H_
#define APP_H_

#include <stdint.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/drivers/gpio.h>

#include "sensei_timesync/timesync.h"

/* Report service of this example (not part of the library): the follower notifies the global time of every edge. */
#define APP_REPORT_SVC_UUID_VAL BT_UUID_128_ENCODE(0xc0f5a4f0, 0x7d1b, 0x4c8e, 0x9a51, 0x3e2f6b7d8a10)
#define APP_REPORT_CHR_UUID_VAL BT_UUID_128_ENCODE(0xc0f5a4f1, 0x7d1b, 0x4c8e, 0x9a51, 0x3e2f6b7d8a10)

/** Size of the report on the air: seq u32, global_ns u64, uncert_ns u32, state u8 (little endian). */
#define APP_REPORT_LEN 17

/** An edge seen by a follower, in global time. */
struct app_report {
  uint32_t seq;       /**< counts every captured edge of the follower, also those without global time */
  uint64_t global_ns; /**< global time of the edge */
  uint32_t uncert_ns; /**< uncertainty of the global time */
  uint8_t state;      /**< enum sensei_ts_state of the follower */
};

void app_report_pack(const struct app_report *r, uint8_t buf[APP_REPORT_LEN]);

/** @retval 0, or -EINVAL if len is wrong */
int app_report_unpack(const uint8_t *buf, uint16_t len, struct app_report *r);

const char *app_state_str(enum sensei_ts_state state);

/* Pins from the devicetree aliases ts-pps, ts-capture, ts-test-out. */
extern const struct gpio_dt_spec app_pin_pps;
extern const struct gpio_dt_spec app_pin_capture;
extern const struct gpio_dt_spec app_pin_test_out;

/** Role entry: starts the role and never returns (it runs in the main thread). */
void app_role_run(void);

/* Master only (measure.c). */

/** The master captured an edge at global time ns (interrupt context). */
void measure_capture_add(uint64_t global_ns);

/** A follower reported an edge (Bluetooth thread). idx: connection index of the follower. */
void measure_report(uint8_t idx, const struct app_report *r);

void measure_follower_add(uint8_t idx, const bt_addr_le_t *addr);
void measure_follower_remove(uint8_t idx);

/** Log the statistics of all followers. */
void measure_print_all(void);

/** Restart the statistics of all followers (the connections stay). */
void measure_reset_all(void);

#endif /* APP_H_ */
