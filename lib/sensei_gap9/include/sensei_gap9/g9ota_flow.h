/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file g9ota_flow.h
 * @brief Update of the GAP9 application on the nRF5340: the session, the check of the signature, the trial boot of
 * the new slot and the revert. No OS: the job link to GAP9, the signature check, the storage, the clock and the
 * trial checks of the app are behind `struct g9ota_ops`, and the host tests run the flow on fakes of them. The Zephyr
 * glue is `gap9_ota_zephyr.h`.
 *
 * The host sends the image in pieces (begin, chunk, command: gap9_ota.proto). The flow relays each piece as one job
 * to GAP9 (`struct g9ota_req`, answered by a Gap9OtaInfo) and keeps none of the image; GAP9 writes the slot it does
 * not run from (g9ota.h). The rules, as OTA.md describes them:
 *
 * - A session starts with `begin` (or `revert`): the nRF5340 holds GAP9 powered and SPI_A (gap9_link_hold) until
 * `apply`, `abort`, 120 s without a request or 45 min after the start. The transfer itself is checkpointed on GAP9
 * and survives the end of the session (resume).
 * - `finish`: GAP9 reads the slot back and checks it, then the nRF5340 checks the signature (ECDSA P-256 over the
 * image hash that GAP9 matched against the bytes) with the key the app was built with. Only then it sends the seal
 * job: GAP9 writes the MCUboot magic, and not before, so a slot of an image that the nRF5340 did not accept never
 * parses (deferred sealing). A bad signature aborts the transfer (the abort is repeated if it gets no answer); if it
 * is lost, the unsealed image is still no slot to the SSBL. The same check and seal are made on a `begin` with `resume`
 * that GAP9 answers VERIFIED (the host came back after a finish).
 * - The flow keeps, in the app's settings, the image id of each slot that it verified (signature) or confirmed. A
 * slot is started on trial only with such an id: `revert` and the lost-settings probe ask GAP9 to verify a slot
 * (status with verify: the same checks as `finish`, hash and signature returned) when the id is not known, and check
 * the signature themselves.
 * - `apply` needs a finished transfer whose signature was checked in this session. The flow stores the trial, power
 * cycles GAP9 with the slot line set to the new slot, and decides (gap9_trial.h): HELLO of the right protocol, then a
 * status job (booted slot and image id are the new ones) and the checks of the app (`trial_check`: self-test, detect
 * job ...). Passed: the slot is confirmed and stored. Anything else, or a reset of the nRF5340 during the trial: the
 * confirmed slot boots again. `revert` is `apply` of the other slot (a verified one).
 * - Recovery (CONFIG_SENSEI_GAP9_RECOVERY, OTA.md): when GAP9's boot loader runs the writer, the same requests work:
 * the flow checks that the slot it keeps (the slot line, the confirmed slot) is the confirmed one (FAIL_KEEP) before a
 * transfer starts, and writes the other slot. `begin` with `force_recovery` powers GAP9 with the strap first (once per
 * session, never during a transfer). An image older than the highest confirmed version is refused (FAIL_ROLLBACK).
 * - A GAP9 that runs the other slot than the confirmed one (the confirmed slot did not start, or an image of a
 * recovery started without its trial) has a confirmed slot that is bad: a transfer goes into it, and `apply` of that
 * slot makes the slot that runs the one to go back to (g9ota_flow.c, start_trial). `revert` then starts the slot that
 * runs on trial, so that it is checked and confirmed like any image. A forced recovery is refused in this state: the
 * writer would overwrite the only image that works.
 * - An app older than G9OTA_APP_MAJOR_MIN (job protocol) runs the normal jobs; the update requests fail with
 * -EPROTONOSUPPORT (the link, gap9_core.h job_min_major). A forced recovery is refused for it too.
 * - New transfers (not a resume, and every forced recovery) are at least G9OTA_OPEN_MIN_MS apart (MRAM wear; GAP9
 * forgets its own count when it loses power).
 * - Settings that cannot be stored are retried every few seconds (g9ota_flow_poll).
 * - The reply to `apply` comes at once (stage APPLYING). The host asks `status` until the trial is over.
 */

#ifndef SENSEI_GAP9_G9OTA_FLOW_H_
#define SENSEI_GAP9_G9OTA_FLOW_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei/gap9_ota.pb.h"
#include "sensei_gap9/g9ota.h"
#include "sensei_gap9/gap9_trial.h"

/** Largest Gap9OtaChunk.data: what fits a host message of 212 B (the receive buffer of the link is 256 B). */
#define G9OTA_HOST_CHUNK_MAX 188u
/** The session ends after this long without a request, ms. */
#define G9OTA_FLOW_IDLE_MS 120000u
/** The longest session, ms. */
#define G9OTA_FLOW_SESSION_MS (45u * 60u * 1000u)
/** A reboot of GAP9 that finds SPI_A busy is tried again at every poll, this many times. */
#define G9OTA_FLOW_BOOT_WAITS 12u
/** A transfer that failed the signature is aborted again this many times if the abort gets no answer. */
#define G9OTA_FLOW_ABORT_RETRIES 2u

/** What the flow keeps in the settings. */
struct g9ota_store {
  struct sensei_gap9_trial trial; /* confirmed slot and the trial */
  uint32_t known[2];              /* image id per slot that the flow verified or confirmed, 0: none */
  /* The highest image version this node ever confirmed (major << 24 | minor << 16 | patch, as MCUboot compares), 0:
   * none. A transfer of an older image is refused (GAP9_OTA_FAIL_ROLLBACK): GAP9's own check compares with the slot
   * that runs, which is empty or bad in recovery. A revert to the old slot is not a transfer and is not checked. */
  uint32_t min_version;
};

/** The record of the settings (the Zephyr glue stores it under "sensei_gap9/trial"). Version 3 added `min_version`. */
#define G9OTA_RECORD_VERSION 3u
struct g9ota_record {
  uint8_t version;
  struct g9ota_store store;
};

/** Read a stored record of `len` bytes: version 3 as it is, version 2 (before `min_version`: trial and image ids only)
 * with `min_version` 0, so that an update of the nRF5340 firmware does not send the node into the lost-settings probe.
 * False for anything else (another length, version or slot numbers that are no slots). */
bool g9ota_record_decode(const void *buf, size_t len, struct g9ota_store *out);

/** Result of a job: `info` is GAP9's answer when `err` is 0. Errors: -ENOTSUP (a GAP9 build without the update, one
 * that is not armed, or one that does not know the job: the host must not retry), -EPROTO (an answer that is no
 * Gap9OtaInfo), -EIO (the job failed), -ETIMEDOUT, -ECANCELED, -EBUSY ... */
typedef void (*g9ota_job_cb)(int err, const sensei_Gap9OtaInfo *info, void *user);
/** Result of the trial checks of the app: 0 passed. */
typedef void (*g9ota_check_cb)(int err, void *user);
/** Result of a reboot: 0 with the HELLO, -EPROTO with a HELLO of another protocol, -ETIMEDOUT without, -EBUSY when
 * SPI_A went to someone else. */
typedef void (*g9ota_reboot_cb)(int err, const sensei_Version *hello, void *user);
/** The reply to a host request: `err` 0 with the reply (valid during the call; the app copies it into its own host
 * message), or a negative errno with no reply (-EBUSY, -ENOTSUP, -EPROTO, -ETIMEDOUT, -EIO ...): the app sends an
 * error. The reply is the verdict, GAP9's view of the transfer, and the slots: the nRF5340 adds the slot it confirmed.
 */
typedef void (*g9ota_reply_fn)(void *rctx, int err, const sensei_Gap9OtaReply *reply);

/** The world of the flow. The callbacks run on the same thread as the requests (the app work queue); the app keeps its
 * state in globals: the operations have no context pointer, so an app runs one flow (the Zephyr glue keeps one).
 * `job`, `hold`, `reboot`, `set_slot`, `verify`, `persist` and `now_ms` are required (g9ota_flow_init() returns
 * -EINVAL without them); `trial_check` and `wake` may be NULL. */
struct g9ota_ops {
  /** Queue a job (copied; the request is valid during the call only); `cb` gets the answer or an error. */
  int (*job)(const struct g9ota_req *req, g9ota_job_cb cb, void *user);
  /** Keep GAP9 powered and SPI_A claimed (gap9_link_hold). -EBUSY: SPI_A is in use, try later. */
  int (*hold)(bool on);
  /** Power cycle GAP9 with the slot line at `slot` and report the HELLO (gap9_link_reboot). */
  int (*reboot)(uint8_t slot, g9ota_reboot_cb cb, void *user);
  /** Slot line for later power-ups (gap9_link_set_slot). */
  int (*set_slot)(uint8_t slot);
  /** Check the signature (raw r || s) over the hash with the app's key. 0: valid. -EACCES: the image names another
   * key (key hash differs). -EBADMSG: the signature is wrong. */
  int (*verify)(const uint8_t hash[32], const uint8_t sig[64], const uint8_t *key_hash, size_t key_hash_len);
  /** Store the settings of the flow (before the flow acts on them). Returns 0 or a negative errno. */
  int (*persist)(const struct g9ota_store *st);
  uint32_t (*now_ms)(void);
  /** The app's own checks of the new image on trial, after the status job (self-test, a detect job ...). Start them
   * and call `cb` once with 0 (passed) or an error. Returns 0, or a negative errno if they cannot start (counts as
   * failed). NULL: no checks. */
  int (*trial_check)(g9ota_check_cb cb, void *user);
  /** Called after a request that leaves the flow wanting g9ota_flow_poll() (see g9ota_flow_needs_poll): start the
   * housekeeping timer (a few seconds). The timer calls g9ota_flow_poll() and starts itself again while the flow
   * wants it. NULL: the app calls g9ota_flow_poll() by itself while g9ota_flow_needs_poll(). */
  void (*wake)(void);
  /** GAP9 is in recovery (its boot loader runs the writer, gap9_core.h g9_recovery()); `reason` may be NULL. The job
   * callback then sends the recovery jobs. NULL: no recovery support (a build without CONFIG_SENSEI_GAP9_RECOVERY). */
  bool (*recovery)(uint32_t *reason);
  /** `on`: power the next boot with the recovery strap (Gap9OtaBegin.force_recovery), 0 or -ENOTSUP; `off`: disarm it
   * (the reboot did not happen or ended; the flow calls it on every way out). NULL: no strap. */
  int (*force_recovery)(bool on);
  /** The job protocol major of the app that said hello last: true with the major, false if none did or the last HELLO
   * was the boot loader's. NULL: not known. A forced recovery is refused for an app older than G9OTA_APP_MAJOR_MIN. */
  bool (*app_major)(uint32_t *major);
};

enum g9ota_state {
  G9OTA_F_IDLE,    /* no session */
  G9OTA_F_SESSION, /* holding GAP9: a transfer may be open */
  G9OTA_F_TRIAL,   /* the new slot is booting or being checked */
};

struct g9ota_flow {
  const struct g9ota_ops *ops;

  struct sensei_gap9_trial trial; /* confirmed slot and the trial; stored */
  uint32_t known[2];              /* image id per slot that the flow verified or confirmed; stored */
  bool dirty;                     /* the settings could not be stored: poll tries again */
  enum g9ota_state state;
  bool held;
  uint32_t start_ms, last_ms;

  sensei_Gap9OtaReply view; /* the last answer of GAP9, for the replies */
  bool verified;            /* the transfer was checked, signature too, in this session */
  uint8_t verified_slot;
  uint32_t verified_id;         /* image id of what was verified */
  sensei_Gap9OtaFail last_fail; /* of an update that went wrong, until the next begin or abort */
  uint8_t booted;               /* the slot GAP9 runs from, as of the last answer */
  uint32_t image_id;            /* of the running image, 0 if not known */
  bool booted_known; /* `booted` is what the last answer of the app said (false after a revert: the outcome is open) */

  /* the request in progress */
  bool busy;
  int step;
  g9ota_reply_fn reply;
  void *rctx;
  int expect_slot;
  uint32_t expect_id;
  sensei_Version hello; /* of the boot on trial */
  bool probe_pending;   /* lost settings: the slots are looked at as soon as GAP9 can be held */
  bool begin_fresh;     /* the begin in flight starts a new transfer */
  bool have_fresh;
  uint32_t fresh_ms; /* when the last new transfer started */
  unsigned abort_tries;
  bool boot_wait; /* a reboot of GAP9 found SPI_A busy: poll tries it again */
  uint8_t boot_wait_slot;
  unsigned boot_waits;
  uint8_t verify_slot; /* slot that a verify job looks at (revert, probe) */
  uint8_t seal_slot;   /* slot and image id that the seal job in flight is for */
  uint32_t seal_id;
  uint32_t min_version;          /* the highest version confirmed; stored */
  uint32_t trial_version;        /* version of the slot on trial, as the status job showed it */
  bool forced;                   /* a begin forced recovery in this session */
  bool rec_retry;                /* the job in flight was sent again the other way (app job or recovery job) */
  sensei_Gap9OtaBegin rec_begin; /* the begin that waits for the check of the keep slot */

  struct g9ota_req req; /* about 1 KB: the job in flight */
};

/**
 * Start the flow. `stored` is from the settings, NULL if there is none (first start or lost settings: GAP9 is asked
 * which slot answers, slot A first, and the slot is verified). A trial that was open at the reset ends with the
 * confirmed slot. Sets the slot line. Returns 0, or -EINVAL when a required operation is missing (`f` is then not
 * usable).
 */
int g9ota_flow_init(struct g9ota_flow *f, const struct g9ota_ops *ops, const struct g9ota_store *stored);

/**
 * The three kinds of host request. The reply goes through `reply`, at once or later. Each returns 0 if it was
 * taken, or a negative errno for a request that cannot be right (-EINVAL: empty file, empty or too long chunk) or that
 * comes while another is in progress (-EBUSY); the caller then sends the error.
 */
int g9ota_flow_begin(struct g9ota_flow *f, const sensei_Gap9OtaBegin *req, g9ota_reply_fn reply, void *rctx);
int g9ota_flow_chunk(struct g9ota_flow *f, const sensei_Gap9OtaChunk *req, g9ota_reply_fn reply, void *rctx);
int g9ota_flow_command(struct g9ota_flow *f, sensei_Gap9OtaAction action, g9ota_reply_fn reply, void *rctx);

/** Housekeeping, every few seconds: ends a session that nobody uses, retries a probe, a reboot or a store of the
 * settings that waited. */
void g9ota_flow_poll(struct g9ota_flow *f);

/** True while the flow wants g9ota_flow_poll(): a session, a trial, a probe or settings to store. */
bool g9ota_flow_needs_poll(const struct g9ota_flow *f);

/** True while a session or a trial keeps GAP9 (the app's scheduler must not expect it for jobs of its own). */
bool g9ota_flow_active(const struct g9ota_flow *f);

/** Slot GAP9 boots from now (the confirmed one, or the one on trial), and what is known of the running image. */
uint8_t g9ota_flow_slot(const struct g9ota_flow *f);

#endif /* SENSEI_GAP9_G9OTA_FLOW_H_ */
