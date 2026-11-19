/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file timesync.h
 * @brief Precise time synchronization between SENSEI nodes (nRF5340 application core, Zephyr).
 *
 * Clocks:
 * - Local time: a mirror of the Bluetooth controller clock on the application core (RTC started by the
 *   network core when the controller starts its clock, plus a TIMER for the part below one RTC tick).
 *   Nanoseconds since the controller clock started. Anchor point reports of the controller are on this clock.
 * - Global time: the local time of the master (the BLE central that sends time samples). On the master,
 *   global == local. A follower maps local to global with a clock model (ts_model.h) fed by connection anchor
 *   points (ts_pair.h): no message timing is involved, only the instants both radios already share.
 *
 * Hardware paths (no interrupt latency in the timing):
 * - Capture: an edge on a pin is timestamped by GPIOTE -> DPPI -> RTC and TIMER capture.
 * - PPS: a pin toggles at multiples of a period in global time (RTC and TIMER compare -> DPPI -> GPIOTE),
 *   for a logic analyzer or to tell another chip (GAP9) where the global seconds are.
 *
 * Needs in the network core image (sysbuild ipc_radio.conf):
 *   CONFIG_BT_CTLR_SDC_CONN_ANCHOR_POINT_REPORT=y
 *   CONFIG_MPSL_TRIGGER_IPC_TASK_ON_RTC_START=y
 *   CONFIG_MPSL_TRIGGER_IPC_TASK_ON_RTC_START_CHANNEL=<CONFIG_SENSEI_TIMESYNC_IPC_CHANNEL>
 *
 * Threading: all functions are thread safe. sensei_ts_local_ns(), sensei_ts_local_to_global() and
 * sensei_ts_global_to_local() may also be called from interrupts (capture callbacks).
 */

#ifndef SENSEI_TIMESYNC_H_
#define SENSEI_TIMESYNC_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>

#include "sensei_timesync/ts_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Time sync service (on the follower): c0f5a4e0-7d1b-4c8e-9a51-3e2f6b7d8a10 */
#define SENSEI_TS_SVC_UUID_VAL BT_UUID_128_ENCODE(0xc0f5a4e0, 0x7d1b, 0x4c8e, 0x9a51, 0x3e2f6b7d8a10)
/** Sample characteristic (write without response, ts_msg.h): c0f5a4e1-7d1b-4c8e-9a51-3e2f6b7d8a10 */
#define SENSEI_TS_SAMPLE_UUID_VAL BT_UUID_128_ENCODE(0xc0f5a4e1, 0x7d1b, 0x4c8e, 0x9a51, 0x3e2f6b7d8a10)

enum sensei_ts_state {
  SENSEI_TS_UNSYNCED, /**< follower without a valid model: no global time */
  SENSEI_TS_SYNCED,   /**< follower with a valid model and recent samples */
  SENSEI_TS_HOLDOVER, /**< follower: model valid but no sample for a while (link down); uncertainty grows */
  SENSEI_TS_MASTER,   /**< master: global time is the local time */
};

struct sensei_ts_time {
  uint64_t ns;        /**< global time */
  uint32_t uncert_ns; /**< estimated error (0 on the master) */
  enum sensei_ts_state state;
};

struct sensei_ts_status {
  enum sensei_ts_state state;
  uint8_t points;               /**< points in the fit */
  double rate_ppm;              /**< master clock rate relative to the local clock, minus 1, in ppm */
  uint32_t resid_ns;            /**< RMS residual of the fit */
  uint64_t last_point_local_ns; /**< local time of the newest point */
  uint32_t master_epoch;        /**< epoch of the master in use (0: none) */
  uint32_t resets;              /**< model resets since boot */
  uint32_t samples_rx;          /**< master samples received */
  uint32_t points_added;        /**< points given to the model (accepted, outliers and resets) */
  uint32_t outliers;            /**< points rejected as outliers */
};

/* ---- Clocks ---- */

/** Local time: the mirror of the controller clock (ns). Valid once the network core has started its clock. */
uint64_t sensei_ts_local_ns(void);

/** True once the network core has started the controller clock (the mirror runs). */
bool sensei_ts_local_running(void);

/**
 * Global time for a local time. On a follower in HOLDOVER the uncertainty grows with the time since the last point.
 * @retval 0, -EAGAIN if there is no global time (UNSYNCED).
 */
int sensei_ts_local_to_global(uint64_t local_ns, struct sensei_ts_time *out);

/** Local time for a global time. @retval 0, or -EAGAIN (UNSYNCED). */
int sensei_ts_global_to_local(uint64_t global_ns, uint64_t *local_ns);

/** Global time now. @retval 0, or -EAGAIN (UNSYNCED, or the local clock does not run yet). */
int sensei_ts_now(struct sensei_ts_time *out);

enum sensei_ts_state sensei_ts_state_get(void);

void sensei_ts_status_get(struct sensei_ts_status *st);

/** Called on every state change, on the thread that caused it (Bluetooth RX or system work queue). */
typedef void (*sensei_ts_state_cb_t)(enum sensei_ts_state state);
void sensei_ts_state_cb_set(sensei_ts_state_cb_t cb);

/* ---- Hardware capture ---- */

/** Called from the GPIOTE interrupt with the local time of the edge. Keep it short. */
typedef void (*sensei_ts_capture_cb_t)(uint8_t channel, uint64_t local_ns, void *user);

/**
 * Timestamp edges on pin in hardware. channel: 0 to CONFIG_SENSEI_TIMESYNC_CAPTURE_CHANNELS - 1.
 * edge: GPIO_INT_EDGE_RISING, GPIO_INT_EDGE_FALLING or GPIO_INT_EDGE_BOTH. The pin is configured as input
 * (its devicetree flags, for example a pull, are kept). Edges closer together than the callback takes are lost
 * (the capture register is overwritten): fine for data-ready lines up to a few kHz.
 * @retval 0, -EINVAL, -EBUSY (channel in use), -ENODEV (no GPIOTE channel or DPPI channel left).
 */
int sensei_ts_capture_start(uint8_t channel, const struct gpio_dt_spec *pin, gpio_flags_t edge,
                            sensei_ts_capture_cb_t cb, void *user);

/**
 * Timestamp any peripheral event in hardware (same channel space, callback and hardware path as
 * sensei_ts_capture_start(): event -> DPPI -> RTC and TIMER capture + EGU). event_address is the address of the
 * event register, for example nrf_saadc_event_address_get(NRF_SAADC, NRF_SAADC_EVENT_END).
 * The library connects the event to its DPPI channel by setting the PUBLISH register of the event (nrfx_gppi).
 * One channel serves one event source. The caller must not publish the same event to another DPPI channel:
 * a PUBLISH register holds one channel, so the last writer wins and the capture would stop (or the other
 * subscriber would). Subscribing other tasks to the library's channel is not supported either.
 * The PUBLISH register is cleared again by sensei_ts_capture_stop().
 * The callback runs from the EGU interrupt with the local time of the event.
 * @retval 0, -EINVAL, -EBUSY (channel in use), -ENODEV (no DPPI channel left).
 */
int sensei_ts_capture_event_start(uint8_t channel, uint32_t event_address, sensei_ts_capture_cb_t cb, void *user);

/** Stop a capture started with sensei_ts_capture_start() or sensei_ts_capture_event_start(). */
int sensei_ts_capture_stop(uint8_t channel);

/* ---- Timed output ---- */

/**
 * Toggle pin at every global time k * period_ns. The pin is high during [2m * period, (2m + 1) * period), so
 * with period_ns = 500000000 the rising edge is on every full global second (1 PPS, 50 % duty).
 * Runs while there is a global time (SYNCED, HOLDOVER or MASTER); in UNSYNCED the pin stays low and the output
 * resumes on its own. Each edge is armed from the newest model, so rate corrections are followed.
 * period_ns: 1 ms to 256 s (the RTC compare is a 24 bit match, 512 s).
 * @retval 0, -EINVAL, -EBUSY (already running), -ENODEV (no GPIOTE or DPPI channel left).
 */
int sensei_ts_pps_start(const struct gpio_dt_spec *pin, uint64_t period_ns);

/**
 * Pulse variant: rising edge at every global time k * period_ns, falling edge width_ns later. The pin is low
 * otherwise, in particular in UNSYNCED and before the first edge (a pin that must not be high at boot of another
 * chip). width_ns: 100 us to period_ns / 2 (the falling edge is armed by the interrupt of the rising edge with the
 * single compare of the module, so very short pulses cannot be armed in time; the request is rejected). Other
 * rules as sensei_ts_pps_start(). If a falling edge cannot be armed in time the pulse is ended at once and logged.
 * @retval 0, -EINVAL, -EBUSY (already running), -ENODEV (no GPIOTE or DPPI channel left).
 */
int sensei_ts_pps_start_pulse(const struct gpio_dt_spec *pin, uint64_t period_ns, uint64_t width_ns);

/**
 * Stop the PPS output (pin low). A watchdog re-arms the output if an edge did not happen within
 * 100 ms of its due time (a missed compare); it is stopped here too.
 */
int sensei_ts_pps_stop(void);

/* ---- Bluetooth ---- */

/**
 * Enable the anchor point reports of the controller and register the vendor event handler.
 * Call once after bt_enable(). Note: Zephyr allows one vendor event callback; this module takes it.
 * @retval 0, or a negative error (the controller does not support the reports: check the ipc_radio config).
 */
int sensei_ts_ble_init(void);

/**
 * Master (CONFIG_SENSEI_TIMESYNC_MASTER): use conn as a follower. Discovers the time sync service on conn and
 * then sends a sample every CONFIG_SENSEI_TIMESYNC_SAMPLE_PERIOD_MS. Detached automatically on disconnection.
 * @retval 0 (discovery started), -ENOMEM (CONFIG_SENSEI_TIMESYNC_MAX_FOLLOWERS reached), -EALREADY.
 */
int sensei_ts_master_attach(struct bt_conn *conn);

void sensei_ts_master_detach(struct bt_conn *conn);

/**
 * Follower (CONFIG_SENSEI_TIMESYNC_FOLLOWER): the GATT service is registered at build time. The connection that
 * writes samples is the master. Its disconnection moves the state to HOLDOVER.
 * Drop the model and go to UNSYNCED (for example after the network core was restarted).
 */
void sensei_ts_follower_reset(void);

/**
 * Snapshot of the current fit, to convert events of a disconnected phase later with ts_fit_bridge():
 * keep the fit taken when the link went down, and convert the events of the gap once SYNCED again.
 */
void sensei_ts_fit_get(struct ts_fit *fit);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_TIMESYNC_H_ */
