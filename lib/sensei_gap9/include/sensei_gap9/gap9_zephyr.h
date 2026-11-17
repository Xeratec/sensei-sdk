/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file gap9_zephyr.h
 * @brief GAP9 link on the nRF5340 (Zephyr): power, boot, SPI job exchange (GAP9 is SPI master, the
 *        nRF5340 SPIS3), trigger line, the camera bus handover and the shared SPI_A bus.
 *
 * GAP9 is powered only while jobs are pending: the first submitted job powers it, it boots from
 * MRAM, fetches the queued jobs, returns the results, and is powered off when the queue is empty
 * (after CONFIG_SENSEI_GAP9_IDLE_OFF_MS). The protocol is in lib/sensei_gap9/PROTOCOL.md.
 *
 * The link moves opaque job and result messages, described by a struct g9_codec. The app wraps
 * the calls with its own typed API and defines the storage with GAP9_LINK_STORAGE_DEFINE().
 * The devicetree needs lib/sensei_gap9/zephyr/gap9_link.overlay (aliases gap9-trigger,
 * gap9-spi-sel, gap9-i2c-sel, gap9-spis).
 */

#ifndef SENSEI_GAP9_ZEPHYR_H_
#define SENSEI_GAP9_ZEPHYR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "sensei_gap9/gap9_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What the link needs from the app. All hooks may be NULL except where noted. */
struct gap9_link_hooks {
  /** Work queue that runs the result callbacks. NULL: the system work queue. */
  struct k_work_q *deliver_wq;
  /** Lock of the bus that the camera shares with the sensors (for jobs with job_uses_bus). */
  void (*bus_lock)(void);
  void (*bus_unlock)(void);
  /** Gate the camera clock: true before GAP9 powers up, false after power-off. Takes the bus lock itself. */
  void (*camera_clock)(bool on);
  /** Count a link error (bad frame, timeout, GAP9 error, dropped frame). */
  void (*error)(void);
  /** Energy marker: high while GAP9 is on. */
  void (*marker)(bool on);
  /** Submit a ping job and wait (the app knows its job format). Used by the shell only. */
  int (*ping)(uint32_t timeout_ms, uint32_t *duration_us, sensei_Version *ver);
};

/** Memory of the link, sized for the app's job and result structs. Use GAP9_LINK_STORAGE_DEFINE(). */
struct gap9_link_storage {
  void *jobs;   /* CONFIG_SENSEI_GAP9_QUEUE_DEPTH jobs */
  void *stage;  /* CONFIG_SENSEI_GAP9_STAGE_DEPTH jobs */
  void *ring;   /* CONFIG_SENSEI_GAP9_RESULT_RING results */
  void *result; /* one result */
};

/** Defines static storage `name` for job_t and result_t, sized by Kconfig. */
#define GAP9_LINK_STORAGE_DEFINE(name, job_t, result_t)                                                                \
  static job_t name##_jobs[CONFIG_SENSEI_GAP9_QUEUE_DEPTH];                                                            \
  static job_t name##_stage[CONFIG_SENSEI_GAP9_STAGE_DEPTH];                                                           \
  static result_t name##_ring[CONFIG_SENSEI_GAP9_RESULT_RING];                                                         \
  static result_t name##_result[1];                                                                                    \
  static const struct gap9_link_storage name = {name##_jobs, name##_stage, name##_ring, name##_result}

struct gap9_link_cfg {
  const struct g9_codec *codec;
  const struct gap9_link_hooks *hooks;
  struct gap9_link_storage storage;
};

/** Set up GPIOs, SPIS and the link thread; leaves GAP9 off. Needs the config for the life of the link. */
int gap9_link_init(const struct gap9_link_cfg *cfg);

/**
 * Queue a job (copied). Jobs that use the camera bus take the bus lock hook before GAP9 sees them.
 * done runs on hooks->deliver_wq, once per result frame, and with final set for the last one.
 *
 * Returns 0 or -ENOMEM: all CONFIG_SENSEI_GAP9_STAGE_DEPTH staging slots are taken (try again
 * later). A staged job waits for SPI_A or LTE (up to CONFIG_SENSEI_GAP9_BUS_WAIT_S, then done gets
 * -EBUSY) and for a free queue entry. Other errors come through done: -ETIMEDOUT, -EPROTO (GAP9
 * firmware with other messages), -ECANCELED (power off or button), -ECONNRESET (GAP9 restarted
 * after it sent part of the result), or the status of the job.
 */
int gap9_link_submit(const void *job, g9_done_fn done, void *user);

/** True while GAP9 is powered or jobs are pending. */
bool gap9_link_busy(void);

/** Power GAP9 off now and fail the queued and staged jobs (-ECANCELED). Safe from any thread. */
void gap9_link_power_off_now(void);

/**
 * Reset GAP9 (the user pressed the button): power cycle it through the link, so that the link
 * state stays right. The job in flight fails (-ECANCELED), waiting jobs run on the new boot. Does
 * nothing while GAP9 is off. Safe from any thread.
 */
void gap9_link_reset(void);

struct gap9_link_status {
  enum g9_state state;
  uint32_t errors;
  sensei_Version version; /* reported by GAP9 in the last hello, zero before */
  bool busy;
};

void gap9_link_status_get(struct gap9_link_status *st);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_GAP9_ZEPHYR_H_ */
