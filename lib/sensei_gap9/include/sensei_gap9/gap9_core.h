/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file gap9_core.h
 * @brief Protocol state machine of the GAP9 link on the nRF5340: job queue, boot, frames,
 *        retries, timeouts, bus lock and idle power-off. No OS, no drivers: the platform
 *        (zephyr/gap9_link.c) and the host tests provide the operations below. All calls must come
 *        from one context (the link thread).
 *
 * The core moves opaque job and result messages. The app describes them with a struct g9_codec
 * (encoding, sequence numbers, status), so any message schema works. Jobs and results live in
 * app-provided structs of codec->job_size and codec->result_size bytes.
 */

#ifndef SENSEI_GAP9_CORE_H_
#define SENSEI_GAP9_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei_gap9/gap9_link.h"
#include "sensei_link/pb.h"

#ifndef G9_QUEUE_DEPTH
#define G9_QUEUE_DEPTH 3
#endif

/**
 * Result of gap9_link_reboot(): the first HELLO of the new boot, or an error.
 * err: 0, -ETIMEDOUT (no HELLO in boot_ms), -EPROTO (the HELLO has another major version, hello is
 * set when it decoded), -ECANCELED (power off or button), -EBUSY (SPI_A or LTE busy, platform).
 * hello is NULL when none came.
 */
typedef void (*g9_reboot_fn)(int err, const sensei_Version *hello, void *user);

/**
 * Result callback, once per result frame. err < 0: timeout, link or GAP9 error (res may be NULL).
 * final: the last call of this job (a job with MORE frames calls it with final = false first).
 */
typedef void (*g9_done_fn)(int err, const void *res, bool final, void *user);

/** How the app's job and result messages are coded. All functions are called on the link thread. */
struct g9_codec {
  size_t job_size;    /**< sizeof the app's job struct */
  size_t result_size; /**< sizeof the app's result struct */
  /** Encode the job into buf (cap bytes). Returns the length or a negative value. */
  int32_t (*encode_job)(uint8_t *buf, size_t cap, const void *job);
  /** Decode a result into res (zeroed before the call). False if malformed. */
  bool (*decode_result)(const uint8_t *buf, size_t len, void *res);
  uint32_t (*job_seq)(const void *job);
  void (*job_set_seq)(void *job, uint32_t seq);
  uint32_t (*result_seq)(const void *res);
  /** errno of the result's status: 0 for success, a negative errno otherwise. */
  int (*result_err)(const void *res);
  /** The job needs the shared camera bus: the bus lock is held until GAP9 sends BUSFREE. */
  bool (*job_uses_bus)(const void *job);
  /**
   * Major version that GAP9 reports in its hello (Version.major) and that these job and result
   * messages need. Another major: the jobs fail with -EPROTO and GAP9 is powered off.
   */
  uint32_t gap9_major;
};

struct g9_ops {
  /** Rail and select lines on (boot starts) or off. */
  void (*power)(bool on);
  /**
   * Drive the trigger line: true = GAP9 should start a transaction now. The frame to send is
   * struct g9.tx (tx_len bytes); the SPIS reads that buffer, so it needs no call to arm it.
   */
  void (*trigger)(bool high);
  void (*bus_lock)(void);
  void (*bus_unlock)(void);
  /** One-shot timeout, 0 stops it. Calls g9_timer_expired(). */
  void (*timer)(uint32_t ms);
  /**
   * Give a result frame to the user, once per frame. A job may answer with several frames (flag
   * MORE on all but the last, for example a detect result and JPEG chunks); the last one ends
   * the job (final). res is valid during the call only. A final frame must never be dropped
   * (the job would never end); a frame that is not final may be.
   */
  void (*deliver)(g9_done_fn fn, void *user, int err, const void *res, bool final);
  void (*error)(void);
  void (*marker)(bool on);
  /**
   * Report the end of g9_reboot(), once. May be NULL. The platform runs fn outside the link
   * thread (fn may call back into the link).
   */
  void (*reboot_done)(g9_reboot_fn fn, void *user, int err, const sensei_Version *hello);
};

struct g9_cfg {
  uint32_t boot_ms, busfree_ms, job_ms, idle_off_ms, restart_off_ms;
  /** Time GAP9 stays on after a reboot's HELLO when no job comes (0: idle_off_ms). */
  uint32_t reboot_grace_ms;
};

enum g9_state { G9_OFF, G9_BOOT, G9_RUN, G9_CYCLE };

struct g9_entry {
  enum { G9E_FREE, G9E_PENDING, G9E_SENT } st;
  bool rec;     /* a recovery job (the recovery codec) */
  bool locked;  /* this entry holds the bus lock */
  bool busfree; /* bus free received */
  bool partial; /* a result frame with MORE was delivered */
  void *job;    /* codec->job_size bytes of the app's pool */
  g9_done_fn done;
  void *user;
};

struct g9 {
  const struct g9_ops *ops;
  const struct g9_codec *codec;
  struct g9_cfg cfg;
  enum g9_state state;
  enum { G9T_NONE, G9T_BOOT, G9T_BUSFREE, G9T_JOB, G9T_IDLE, G9T_CYCLE, G9T_FETCH } timer;
  bool err_flag; /* last GAP9 frame was bad: set ERROR in our next header */
  bool boot_retry;
  bool hold;              /* g9_hold(): stay powered with no jobs */
  bool grace;             /* first idle after a reboot: wait reboot_grace_ms */
  g9_reboot_fn reboot_fn; /* g9_reboot() is waiting for the HELLO */
  void *reboot_user;
  const struct g9_codec *rec_codec; /* the recovery jobs, NULL: no recovery support */
  bool recovery;                    /* the last HELLO was the boot loader's: only recovery jobs run */
  uint32_t rec_reason;              /* its Version.patch (G9L_RECOVERY_*) */
  int armed;                        /* index of the job frame in tx, -1 for NONE */
  size_t tx_len;
  uint32_t seq;
  uint32_t errors;
  sensei_Version version;
  struct g9_entry q[G9_QUEUE_DEPTH];
  void *res; /* codec->result_size bytes of the app: decode buffer */
  uint8_t tx[G9L_TX_CAP];
};

/**
 * Set up the core. jobs: G9_QUEUE_DEPTH jobs of codec->job_size bytes, res: one result of
 * codec->result_size bytes, both aligned for the app's structs and valid for the life of the core.
 */
void g9_init(struct g9 *g, const struct g9_ops *ops, const struct g9_codec *codec, const struct g9_cfg *cfg, void *jobs,
             void *res);

/** Queue a job (copied). -ENOMEM if the queue is full, -ENOEXEC while GAP9 is in recovery (nothing is powered for it).
 * Starts a boot if GAP9 is off. */
int g9_submit(struct g9 *g, const void *job, g9_done_fn done, void *user);

/**
 * Recovery (the SSBL of GAP9/ssbl serves the update writer when no slot boots; PROTOCOL.md, "Recovery HELLO"). Register
 * the codec of its jobs: job_size and result_size must not exceed the app's (the queue and the decode buffer are the
 * app's), gap9_major is the recovery protocol that this nRF5340 speaks (G9L_RECOVERY_PROTO). Without it a recovery
 * HELLO is a version mismatch (-EPROTO, GAP9 off). Returns 0 or -EINVAL.
 *
 * After a HELLO with major G9L_RECOVERY_MAJOR (and that protocol as minor) the core latches "recovery": the app's jobs
 * fail with -ENOEXEC, at once for new ones, and GAP9 stays as it is; jobs of g9_submit_recovery() run. A reboot
 * callback gets -ENOEXEC with the HELLO. A HELLO of the app clears the latch.
 */
int g9_set_recovery(struct g9 *g, const struct g9_codec *rec);

/** Queue a recovery job (copied). -ENOTSUP without g9_set_recovery(), -ENOEXEC while GAP9 runs its app, -ENOMEM if the
 * queue is full. Starts a boot if GAP9 is off (its HELLO says what it is: a recovery job fails with -ENOEXEC when the
 * app came up). */
int g9_submit_recovery(struct g9 *g, const void *job, g9_done_fn done, void *user);

/** True while the latch is set; `reason` (may be NULL) gets G9L_RECOVERY_BOTH_BAD or G9L_RECOVERY_FORCED. */
bool g9_recovery(const struct g9 *g, uint32_t *reason);

/** Look again: clear the latch and power GAP9 once if it is off (the next HELLO sets it again if it is still in
 * recovery; the idle timer powers GAP9 off after it). The platform calls it every few hours. */
void g9_reprobe(struct g9 *g);

/** True if g9_submit() would fail with -ENOMEM. */
bool g9_full(const struct g9 *g);

/**
 * A SPI transaction ended. rx holds the bytes the master clocked in (rx_len of them). The SPIS
 * receive buffer is as long as the longest armed frame, so rx_len is also the number of bytes of
 * the armed frame that the master clocked out.
 */
void g9_spi_done(struct g9 *g, const uint8_t *rx, size_t rx_len);

void g9_timer_expired(struct g9 *g);

/**
 * Power cycle GAP9 and report the HELLO of the new boot through ops->reboot_done (fn, once). The
 * platform has set the boot selection (slot line) already. The jobs queued or in flight fail
 * with -ECANCELED. Jobs submitted afterwards run on the new boot. No retry: a boot without HELLO
 * ends with -ETIMEDOUT and GAP9 off. -EBUSY if a reboot is pending already.
 */
int g9_reboot(struct g9 *g, g9_reboot_fn fn, void *user);

/**
 * Keep GAP9 on without jobs (an update session): on powers it up if it is off and blocks the idle
 * power-off; off lets the idle timer run again. A boot that fails twice clears the hold.
 */
void g9_hold(struct g9 *g, bool on);

/** True while GAP9 is powered, booting or restarting, jobs are pending, or it is held. */
bool g9_busy(const struct g9 *g);

/** Power off now and fail all jobs (-ECANCELED). */
void g9_shutdown(struct g9 *g);

/**
 * Reset GAP9 (the button): power cycle it, fail the job in flight (-ECANCELED) and run the
 * waiting ones on the new boot. Does nothing while GAP9 is off.
 */
void g9_reset(struct g9 *g);

#endif /* SENSEI_GAP9_CORE_H_ */
