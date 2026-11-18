/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_gap9/g9ota_flow.h"

#include <errno.h>
#include <string.h>

enum step {
  ST_NONE,
  ST_BEGIN,
  ST_CHUNK,
  ST_STATUS,
  ST_FINISH,
  ST_ABORT,
  ST_SEAL,          /* the signature checked: GAP9 writes the magic of the slot */
  ST_VERIFY_ABORT,  /* abort after a signature that did not check: the reply says why */
  ST_REVERT_STATUS, /* revert: look at the other slot first */
  ST_REVERT_VERIFY, /* revert: GAP9 checks the other slot, the node its signature */
  ST_TRIAL_STATUS,  /* trial: booted slot and image id */
  ST_TRIAL_VERIFY,  /* probe: the slot that answers is checked like a received image */
  ST_REC_KEEP,      /* recovery: the status job shows the slot that GAP9 keeps, before the begin */
};

static void trial_event(struct g9ota_flow *f, enum sensei_gap9_trial_event ev);
static void on_job(int err, const sensei_Gap9OtaInfo *info, void *user);
static void on_reboot(int err, const sensei_Version *hello, void *user);
static void view_from_info(sensei_Gap9OtaReply *v, const sensei_Gap9OtaInfo *info);

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t now(const struct g9ota_flow *f) { return f->ops->now_ms(); }

/* The version as MCUboot compares it (the writer packs the same way). */
static uint32_t pack_version(const sensei_Version *v) {
  return ((v->major & 0xFFu) << 24) | ((v->minor & 0xFFu) << 16) | (v->patch & 0xFFFFu);
}

static bool in_recovery(const struct g9ota_flow *f) { return f->ops->recovery != NULL && f->ops->recovery(NULL); }

/* ---- settings ---- */

static int persist_try(struct g9ota_flow *f) {
  struct g9ota_store st = {.trial = f->trial, .known = {f->known[0], f->known[1]}, .min_version = f->min_version};
  int rc = -EIO;

  for (int i = 0; i < 3 && rc != 0; i++) {
    rc = f->ops->persist(&st);
  }
  return rc;
}

/* Store, and keep trying at every poll if it does not work: a confirm that is not stored would be
 * undone by the next reset of the nRF5340. */
static void persist_keep(struct g9ota_flow *f) { f->dirty = persist_try(f) != 0; }

static bool slot_known(const struct g9ota_flow *f, uint8_t slot, const sensei_Gap9Slot *s) {
  return s->valid && s->image_id != 0 && f->known[slot & 1u] == s->image_id;
}

/* ---- replies ---- */

/* The view as the host should see it now: the trial and the last failure override what GAP9 said.
 */
static void cur_view(struct g9ota_flow *f) {
  f->view.confirmed_slot = f->trial.confirmed;
  if (f->state == G9OTA_F_TRIAL) {
    f->view.stage = sensei_Gap9OtaStage_GAP9_OTA_STAGE_APPLYING;
    f->view.booted_slot = f->trial.trial;
    f->view.fail = sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE;
  } else if (f->last_fail != sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE &&
             f->view.stage != sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING) {
    f->view.stage = sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED;
    f->view.fail = f->last_fail;
  }
  if (f->view.chunk_max == 0 || f->view.chunk_max > G9OTA_HOST_CHUNK_MAX) {
    f->view.chunk_max = G9OTA_HOST_CHUNK_MAX; /* what the host link takes */
  }
  f->view.recovery = false;
  f->view.recovery_reason = 0;
  if (f->ops->recovery != NULL) {
    uint32_t reason = 0;

    f->view.recovery = f->ops->recovery(&reason);
    f->view.recovery_reason = f->view.recovery ? reason : 0;
  }
}

static void send_view(struct g9ota_flow *f) {
  g9ota_reply_fn reply = f->reply;
  void *rctx = f->rctx;

  cur_view(f);
  f->busy = false;
  reply(rctx, 0, &f->view);
}

static void send_error(struct g9ota_flow *f, int err) {
  g9ota_reply_fn reply = f->reply;
  void *rctx = f->rctx;

  f->busy = false;
  reply(rctx, err, NULL);
}

/* Refuse a request: the state does not change, the reply says why. */
static void refuse(struct g9ota_flow *f, sensei_Gap9OtaResult res, sensei_Gap9OtaFail why) {
  f->view.result = res;
  f->view.fail = why;
  cur_view(f);
  f->view.fail = why; /* cur_view may have put the trial's or the last failure there */
  f->busy = false;
  f->reply(f->rctx, 0, &f->view);
  f->view.fail = sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE;
}

/* ---- session ---- */

static int session_start(struct g9ota_flow *f) {
  int rc;

  if (f->held) {
    f->state = f->state == G9OTA_F_TRIAL ? G9OTA_F_TRIAL : G9OTA_F_SESSION;
    return 0;
  }
  rc = f->ops->hold(true);
  if (rc != 0) {
    return rc;
  }
  f->held = true;
  f->state = G9OTA_F_SESSION;
  f->start_ms = f->last_ms = now(f);
  return 0;
}

static void session_end(struct g9ota_flow *f) {
  if (f->held) {
    f->held = false;
    (void)f->ops->hold(false);
  }
  if (f->state == G9OTA_F_SESSION) {
    f->state = G9OTA_F_IDLE;
  }
  f->verified = false;
  f->forced = false;
}

static void ingest(struct g9ota_flow *f, const sensei_Gap9OtaInfo *info) {
  view_from_info(&f->view, info);
  f->booted = (uint8_t)(info->booted_slot & 1u);
  f->booted_known = true;
  if (info->has_slot_a && info->has_slot_b) {
    const sensei_Gap9Slot *s = f->booted ? &info->slot_b : &info->slot_a;

    f->image_id = s->valid ? s->image_id : 0;
    /* No version confirmed yet (a node that confirmed before min_version existed, or settings that were migrated): the
     * confirmed slot answers with an image that this node verified: its version is the floor. */
    if (f->min_version == 0 && !f->trial.open && f->booted == f->trial.confirmed && s->has_version &&
        slot_known(f, f->booted, s)) {
      f->min_version = pack_version(&s->version);
      persist_keep(f);
    }
  }
}

/* ---- jobs ---- */

static void submit(struct g9ota_flow *f, enum step step) {
  int rc;

  f->step = step;
  rc = f->ops->job(&f->req, on_job, f);
  if (rc != 0) {
    on_job(rc, NULL, f);
  }
}

static void req_clear(struct g9ota_flow *f, enum g9ota_op op) {
  memset(&f->req, 0, sizeof(f->req));
  f->req.op = op;
}

static void job_status(struct g9ota_flow *f) { req_clear(f, G9OTA_OP_STATUS); }

/* ---- trial ---- */

/* SPI_A is busy (the LPWAN client has it): not a boot without HELLO. Try again at the next poll;
 * after G9OTA_FLOW_BOOT_WAITS tries it counts as a timeout. */
static void boot_later(struct g9ota_flow *f, uint8_t slot) {
  if (f->boot_waits++ < G9OTA_FLOW_BOOT_WAITS) {
    f->boot_wait = true;
    f->boot_wait_slot = slot;
    return;
  }
  trial_event(f, SENSEI_GAP9_TRIAL_TIMEOUT);
}

static void boot_slot(struct g9ota_flow *f, uint8_t slot) {
  int rc = f->ops->reboot(slot, on_reboot, f);

  if (rc == -EBUSY) {
    boot_later(f, slot);
  } else if (rc != 0) { /* the link would not start it: as a boot without HELLO */
    trial_event(f, SENSEI_GAP9_TRIAL_TIMEOUT);
  }
}

static void on_revert_boot(int err, const sensei_Version *hello, void *user) {
  struct g9ota_flow *f = user;

  (void)err;
  (void)hello;
  session_end(f); /* the confirmed slot runs again (or GAP9 is off: the next power-up boots it) */
}

static void trial_end(struct g9ota_flow *f) {
  f->boot_wait = false;
  f->state = G9OTA_F_IDLE;
  f->verified = false;
  f->view.offset = 0;
  f->view.size = 0;
}

static void trial_act(struct g9ota_flow *f, struct sensei_gap9_trial_out out) {
  switch (out.action) {
  case SENSEI_GAP9_TRIAL_BOOT:
    if (f->trial.probing) {
      f->expect_id = 0; /* a probe takes the id of the slot that answers, not of the one before */
    }
    persist_keep(f);
    boot_slot(f, out.slot);
    break;
  case SENSEI_GAP9_TRIAL_CONFIRM:
    if (f->expect_id != 0) {
      f->known[out.slot & 1u] = f->expect_id;
    }
    if (f->trial_version > f->min_version) { /* no transfer of an older image from now on */
      f->min_version = f->trial_version;
    }
    persist_keep(f);
    (void)f->ops->set_slot(out.slot);
    f->booted = out.slot;
    f->booted_known = true;
    f->image_id = f->expect_id;
    f->last_fail = sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE;
    trial_end(f);
    session_end(f);
    break;
  case SENSEI_GAP9_TRIAL_REVERT:
    persist_keep(f);
    (void)f->ops->set_slot(out.slot);
    f->booted_known = false; /* the SSBL may start the other slot when this one does not load */
    f->last_fail = sensei_Gap9OtaFail_GAP9_OTA_FAIL_BOOT;
    trial_end(f);
    if (f->held && f->ops->reboot(out.slot, on_revert_boot, f) == 0) {
      break; /* the hold goes when GAP9 is back */
    }
    session_end(f);
    break;
  case SENSEI_GAP9_TRIAL_GIVE_UP:
    persist_keep(f);
    (void)f->ops->set_slot(out.slot);
    f->booted_known = false;
    trial_end(f);
    session_end(f);
    break;
  default:
    break;
  }
}

static void trial_event(struct g9ota_flow *f, enum sensei_gap9_trial_event ev) {
  trial_act(f, sensei_gap9_trial_step(&f->trial, ev, f->trial.trial));
}

static void on_reboot(int err, const sensei_Version *hello, void *user) {
  struct g9ota_flow *f = user;

  memset(&f->hello, 0, sizeof(f->hello));
  if (hello != NULL) {
    f->hello = *hello;
  }
  if (err == -EBUSY) { /* SPI_A went to someone else: the boot did not happen */
    boot_later(f, f->trial.trial);
  } else if (err == 0) {
    (void)sensei_gap9_trial_step(&f->trial, SENSEI_GAP9_TRIAL_HELLO_OK, f->trial.trial);
    /* A boot that timed out cleared the hold of the link: take it again, so that GAP9 stays on
     * for the checks. */
    f->held = f->ops->hold(true) == 0 || f->held;
    job_status(f);
    submit(f, ST_TRIAL_STATUS);
  } else {
    /* -ENOEXEC: GAP9 landed in recovery: the image did not boot, as with another protocol */
    trial_event(f, err == -EPROTO || err == -ENOEXEC ? SENSEI_GAP9_TRIAL_HELLO_BAD : SENSEI_GAP9_TRIAL_TIMEOUT);
  }
}

/* Start the trial of `slot` (an apply or a revert). Replies at once. */
static void start_trial(struct g9ota_flow *f, uint8_t slot, uint32_t image_id) {
  struct sensei_gap9_trial_out out;
  uint8_t was_confirmed;

  /* A slot is started only with an image id that this node verified. */
  if (image_id == 0 || f->known[slot & 1u] != image_id) {
    refuse(f, sensei_Gap9OtaResult_GAP9_OTA_INVALID, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
    return;
  }
  /* GAP9 runs the other slot than the confirmed one (a fallback, or a recovery image without its trial): the confirmed
   * slot is bad, and the transfer went into it. The slot that runs becomes the one to go back to. */
  was_confirmed = f->trial.confirmed;
  if (!f->trial.open && slot == was_confirmed && f->booted_known && (f->booted & 1u) != was_confirmed) {
    f->trial.confirmed = f->booted & 1u;
  }
  out = sensei_gap9_trial_step(&f->trial, SENSEI_GAP9_TRIAL_APPLY, slot);
  if (out.action != SENSEI_GAP9_TRIAL_BOOT) { /* a trial is open, or the slot is the confirmed one */
    f->trial.confirmed = was_confirmed;
    refuse(f, sensei_Gap9OtaResult_GAP9_OTA_INVALID, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
    return;
  }
  if (persist_try(f) != 0) {
    f->trial.confirmed = was_confirmed;
    f->trial.open = false;
    f->view.detail = -EIO;
    refuse(f, sensei_Gap9OtaResult_GAP9_OTA_FAILED, sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH);
    return;
  }
  f->state = G9OTA_F_TRIAL;
  f->boot_waits = 0;
  f->boot_wait = false;
  f->expect_slot = slot;
  f->expect_id = image_id;
  f->trial_version = 0;
  f->verified = false;
  f->last_fail = sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE;
  f->view.result = sensei_Gap9OtaResult_GAP9_OTA_OK;
  send_view(f); /* stage APPLYING */
  boot_slot(f, slot);
}

/* ---- the answers of GAP9 ---- */

static bool trial_status_ok(struct g9ota_flow *f, const sensei_Gap9OtaInfo *info) {
  const sensei_Gap9Slot *s;

  if (info->booted_slot != (uint32_t)f->trial.trial) {
    return false;
  }
  s = f->trial.trial ? &info->slot_b : &info->slot_a;
  f->trial_version = s->valid && s->has_version ? pack_version(&s->version) : 0;
  if (f->expect_id == 0) {
    return s->valid; /* a probe: the slot that answers holds an image; on_job checks it */
  }
  /* The image id and the version of the slot must be what runs. GAP9 reports the slot that the
   * SSBL started (hand-off record), which is not the slot of the slot line after a fallback. The
   * version in the HELLO (the firmware's own) must agree with the slot's header too. */
  return s->valid && s->image_id == f->expect_id && s->has_version && s->version.major == f->hello.major &&
         s->version.minor == f->hello.minor && s->version.patch == f->hello.patch;
}

/* The proof that GAP9 gives for an image it checked (finish, a resumed begin that answers VERIFIED,
 * verify): the node checks the signature with its key. Returns 0 and the image id, or the failure.
 */
static int check_proof(struct g9ota_flow *f, const sensei_Gap9OtaInfo *info, uint32_t *id, sensei_Gap9OtaFail *why,
                       int *detail) {
  int rc;

  *detail = 0;
  if (info->result != sensei_Gap9OtaResult_GAP9_OTA_OK) {
    *why = info->fail != sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE ? info->fail : sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE;
    *detail = info->detail;
    return -EBADMSG;
  }
  if (info->image_hash.size != 32 || info->signature.size != 64) { /* a GAP9 that cannot verify */
    *why = sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE;
    *detail = 3;
    return -EBADMSG;
  }
  rc = f->ops->verify(info->image_hash.bytes, info->signature.bytes, info->key_hash.size ? info->key_hash.bytes : NULL,
                      info->key_hash.size);
  if (rc != 0) {
    *why = sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIGNATURE;
    *detail = rc == -EACCES ? 1 : 2; /* 1: another key, 2: the signature does not check */
    return rc;
  }
  *id = le32(info->image_hash.bytes);
  return 0;
}

static void job_verify(struct g9ota_flow *f, uint8_t slot) {
  req_clear(f, G9OTA_OP_STATUS);
  f->req.u.status.verify = true;
  f->req.u.status.slot = slot;
}

static void job_abort(struct g9ota_flow *f) { req_clear(f, G9OTA_OP_ABORT); }

static void job_seal(struct g9ota_flow *f, const uint8_t hash[32]) {
  req_clear(f, G9OTA_OP_SEAL);
  memcpy(f->req.u.seal.image_hash, hash, 32);
}

/* The answer to a finish, or to a begin with resume that finds the transfer finished. */
static void on_finish(struct g9ota_flow *f, const sensei_Gap9OtaInfo *info) {
  sensei_Gap9OtaFail why = sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE;
  uint32_t id = 0;
  int detail = 0;

  if (info->result != sensei_Gap9OtaResult_GAP9_OTA_OK || info->stage != sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED) {
    bool resumable = info->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH ||
                     info->stage != sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED;

    send_view(f);
    if (!resumable) { /* GAP9 dropped the image: the host starts again, nothing to hold GAP9 for
                       */
      session_end(f);
    }
    return;
  }
  int rc = check_proof(f, info, &id, &why, &detail);

  if (rc == 0 && f->min_version != 0 && (!info->has_version || pack_version(&info->version) < f->min_version)) {
    rc = -EPERM; /* an older image than this node ever confirmed: the signature does not make it an update */
    why = sensei_Gap9OtaFail_GAP9_OTA_FAIL_ROLLBACK;
    detail = 0;
  }
  if (rc == 0) {
    /* The signature is good: GAP9 writes the magic, so that the slot can start. Until then the slot is no image to the
     * SSBL, whatever happens to this session (a reset, a lost abort). The version rule (not older than what runs) is
     * GAP9's: it compares the headers of the two slots, which the signature covers. */
    f->seal_slot = (uint8_t)(1u - (info->booted_slot & 1u));
    f->seal_id = id;
    job_seal(f, info->image_hash.bytes);
    submit(f, ST_SEAL);
    return;
  }
  /* Not an image that this node takes: drop it on GAP9 (its slot is invalidated), then say why.
   */
  f->last_fail = why;
  f->view.detail = detail;
  f->view.result = sensei_Gap9OtaResult_GAP9_OTA_FAILED;
  f->view.stage = sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED;
  f->view.fail = why;
  f->abort_tries = 0;
  job_abort(f);
  submit(f, ST_VERIFY_ABORT);
}

static void on_check(int err, void *user) {
  struct g9ota_flow *f = user;

  trial_event(f, err == 0 ? SENSEI_GAP9_TRIAL_STATUS_OK : SENSEI_GAP9_TRIAL_STATUS_BAD);
}

/* The checks of the app on the new image (none: the status job was enough). */
static void next_check(struct g9ota_flow *f) {
  int rc = 0;

  if (f->ops->trial_check != NULL) {
    rc = f->ops->trial_check(on_check, f);
    if (rc == 0) {
      return;
    }
  }
  on_check(rc, f);
}

static void on_job(int err, const sensei_Gap9OtaInfo *info, void *user) {
  struct g9ota_flow *f = user;

  if (info == NULL && err == 0) {
    err = -EIO;
  }
  if (err == -EPROTONOSUPPORT && f->step != ST_TRIAL_STATUS && f->step != ST_TRIAL_VERIFY) {
    /* The app of GAP9 is older than G9OTA_APP_MAJOR_MIN: it runs the normal jobs, not the update. A trial of such an
     * image fails as any trial that gets no status. */
    session_end(f);
    send_error(f, err);
    return;
  }
  if (err == -ENOEXEC && !f->rec_retry) {
    /* The job was meant for the other program of GAP9: an app job and the boot loader runs (or the first job of a
     * session came before the HELLO), or a recovery job and the app came up. The same request goes again, once, the
     * other way (the glue routes by the state of the link). */
    bool rec = in_recovery(f);

    switch (f->step) {
    case ST_BEGIN:
      f->rec_retry = true;
      if (rec) { /* the keep slot is checked first */
        f->rec_begin = f->req.u.begin;
        job_status(f);
        submit(f, ST_REC_KEEP);
      } else {
        submit(f, f->step);
      }
      return;
    case ST_REC_KEEP:
      if (!rec) { /* the app came up: no keep slot to check, the begin goes as an app job */
        f->rec_retry = true;
        req_clear(f, G9OTA_OP_BEGIN);
        f->req.u.begin = f->rec_begin;
        submit(f, ST_BEGIN);
        return;
      }
      break;
    case ST_CHUNK:
    case ST_STATUS:
    case ST_FINISH:
    case ST_ABORT:
    case ST_SEAL:
    case ST_REVERT_STATUS:
    case ST_REVERT_VERIFY:
      f->rec_retry = true;
      submit(f, f->step);
      return;
    default:
      break;
    }
  }

  switch (f->step) {
  case ST_REC_KEEP:
    if (err != 0) {
      session_end(f);
      send_error(f, err);
      break;
    }
    ingest(f, info);
    if (info->booted_slot != (uint32_t)f->trial.confirmed) {
      /* The slot line says one slot, the node trusts the other: nothing is written (the keep slot is the one on the
       * slot line, the transfer would go into the slot that the node holds good). */
      f->view.detail = (int)info->booted_slot;
      session_end(f);
      refuse(f, sensei_Gap9OtaResult_GAP9_OTA_FAILED, sensei_Gap9OtaFail_GAP9_OTA_FAIL_KEEP);
      break;
    }
    req_clear(f, G9OTA_OP_BEGIN);
    f->req.u.begin = f->rec_begin;
    submit(f, ST_BEGIN);
    break;
  case ST_BEGIN:
  case ST_CHUNK:
  case ST_STATUS:
    if (err != 0) {
      if (err == -ENOTSUP || err == -EPROTO) {
        session_end(f); /* a GAP9 that cannot be updated: nothing to hold it for */
      }
      send_error(f, err);
      break;
    }
    ingest(f, info);
    if (f->step == ST_BEGIN && info->stage != sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING &&
        info->stage != sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED) {
      session_end(f); /* nothing open: do not keep GAP9 for nothing */
      send_view(f);
      break;
    }
    if (f->step == ST_BEGIN) {
      f->last_fail = sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE;
      f->verified = false;
      if (f->begin_fresh && info->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING) {
        f->have_fresh = true;
        f->fresh_ms = now(f);
      }
      if (info->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED) {
        /* the host came back after a finish: GAP9 holds a finished image. The node checks
         * the signature from the answer, as it does after a finish. */
        on_finish(f, info);
        break;
      }
    }
    send_view(f);
    break;
  case ST_FINISH:
    if (err != 0) {
      send_error(f, err);
      break;
    }
    ingest(f, info);
    on_finish(f, info);
    break;
  case ST_SEAL:
    if (err != 0) {
      send_error(f, err); /* the host asks again: finish is repeated, GAP9 still holds the checked image */
      break;
    }
    ingest(f, info);
    if (info->result != sensei_Gap9OtaResult_GAP9_OTA_OK || !info->sealed) {
      if (info->result == sensei_Gap9OtaResult_GAP9_OTA_OK) { /* an answer that is not a seal */
        f->view.result = sensei_Gap9OtaResult_GAP9_OTA_FAILED;
        f->view.fail = sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE;
      }
      send_view(f);
      break;
    }
    f->known[f->seal_slot] = f->seal_id;
    if (persist_try(f) != 0) { /* not stored: not started later; the host asks again */
      f->known[f->seal_slot] = 0;
      f->view.detail = -EIO;
      f->view.result = sensei_Gap9OtaResult_GAP9_OTA_FAILED;
      f->view.fail = sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH;
      send_view(f);
      break;
    }
    f->verified = true;
    f->verified_slot = f->seal_slot;
    f->verified_id = f->seal_id;
    send_view(f);
    break;
  case ST_ABORT:
    if (err != 0) {
      session_end(f);
      send_error(f, err);
      break;
    }
    ingest(f, info);
    f->last_fail = sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE;
    session_end(f);
    send_view(f);
    break;
  case ST_VERIFY_ABORT:
    if (err != 0 && f->abort_tries < G9OTA_FLOW_ABORT_RETRIES) {
      /* The slot may still hold the image that failed: ask again. A GAP9 that lost power
       * answers an abort for a finished image as well (its record says so). */
      f->abort_tries++;
      job_abort(f);
      submit(f, ST_VERIFY_ABORT);
      break;
    }
    if (err == 0) {
      sensei_Gap9OtaFail why = f->last_fail;
      int detail = f->view.detail;

      ingest(f, info);
      f->view.result = sensei_Gap9OtaResult_GAP9_OTA_FAILED;
      f->view.stage = sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED;
      f->view.fail = why;
      f->view.detail = detail;
    }
    session_end(f);
    send_view(f); /* the reply says why, also when the abort itself got no answer */
    break;
  case ST_REVERT_STATUS:
    if (err != 0) {
      session_end(f);
      send_error(f, err);
      break;
    }
    ingest(f, info);
    {
      uint8_t other = (uint8_t)(1u - f->trial.confirmed);
      const sensei_Gap9Slot *s = other ? &info->slot_b : &info->slot_a;

      if (!s->valid) {
        f->view.detail = 0;
        refuse(f, sensei_Gap9OtaResult_GAP9_OTA_FAILED, sensei_Gap9OtaFail_GAP9_OTA_FAIL_NO_SLOT);
        session_end(f);
      } else if (slot_known(f, other, s)) {
        start_trial(f, other, s->image_id);
      } else { /* an image that this node never checked: GAP9 reads it, the node checks it */
        f->verify_slot = other;
        job_verify(f, other);
        submit(f, ST_REVERT_VERIFY);
      }
    }
    break;
  case ST_REVERT_VERIFY:
    if (err != 0) {
      session_end(f);
      send_error(f, err);
      break;
    }
    ingest(f, info);
    {
      sensei_Gap9OtaFail why;
      uint32_t id = 0;
      int detail;

      if (check_proof(f, info, &id, &why, &detail) != 0) {
        f->view.detail = detail;
        refuse(f, sensei_Gap9OtaResult_GAP9_OTA_FAILED, why);
        session_end(f);
      } else {
        f->known[f->verify_slot] = id;
        start_trial(f, f->verify_slot, id); /* stores the id with the trial */
      }
    }
    break;
  case ST_TRIAL_STATUS:
    if (err == 0 && trial_status_ok(f, info)) {
      ingest(f, info);
      f->view.booted_slot = f->trial.trial;
      if (f->trial.probing) { /* lost settings: nothing is known of this slot */
        const sensei_Gap9Slot *s = f->trial.trial ? &info->slot_b : &info->slot_a;

        if (slot_known(f, f->trial.trial, s)) {
          f->expect_id = s->image_id;
          next_check(f);
        } else {
          f->verify_slot = f->trial.trial;
          job_verify(f, f->trial.trial);
          submit(f, ST_TRIAL_VERIFY);
        }
      } else {
        next_check(f);
      }
    } else {
      trial_event(f, SENSEI_GAP9_TRIAL_STATUS_BAD);
    }
    break;
  case ST_TRIAL_VERIFY: {
    sensei_Gap9OtaFail why;
    uint32_t id = 0;
    int detail;

    if (err == 0 && check_proof(f, info, &id, &why, &detail) == 0 && info->has_slot_a && info->has_slot_b) {
      f->known[f->verify_slot] = id;
      f->expect_id = id;
      persist_keep(f);
      next_check(f);
    } else {
      trial_event(f, SENSEI_GAP9_TRIAL_STATUS_BAD);
    }
    break;
  }
  default:
    break;
  }
}

/* ---- requests ---- */

/* The reboot with the strap ended: the boot loader is there, or the strap did nothing. */
static void on_force_boot(int err, const sensei_Version *hello, void *user) {
  struct g9ota_flow *f = user;

  (void)hello;
  (void)f->ops->force_recovery(false); /* the power-up used the strap, or it did not happen: nothing stays armed */
  if (err == -ENOEXEC && in_recovery(f)) {
    job_status(f); /* the begin waits in rec_begin */
    submit(f, ST_REC_KEEP);
    return;
  }
  /* The app came up (the strap is not wired or not read), or nothing answered. */
  f->view.detail = err == 0 ? 1 : 2;
  session_end(f);
  refuse(f, sensei_Gap9OtaResult_GAP9_OTA_FAILED, sensei_Gap9OtaFail_GAP9_OTA_FAIL_BOOT);
}

/* Gap9OtaBegin.force_recovery: power GAP9 with the strap, so that the boot loader runs although a slot boots. Once per
 * session, never while a transfer is open. The write itself starts only after the boot loader answered and the keep
 * slot was checked. */
static int begin_forced(struct g9ota_flow *f, const sensei_Gap9OtaBegin *b) {
  int rc;

  uint32_t major = 0;

  if (f->ops->force_recovery == NULL || f->ops->recovery == NULL || f->forced ||
      (f->state == G9OTA_F_SESSION && f->view.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING)) {
    refuse(f, sensei_Gap9OtaResult_GAP9_OTA_INVALID, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
    return 0;
  }
  if (f->ops->app_major != NULL && f->ops->app_major(&major) && major < G9OTA_APP_MAJOR_MIN) {
    return -EPROTONOSUPPORT; /* an app that takes no update requests */
  }
  if (f->booted_known && f->booted != f->trial.confirmed) {
    /* The app answered from the slot that is not the confirmed one: the confirmed slot is damaged and this image is
     * the only one that works. The boot loader would keep the confirmed slot and write the other: this one. */
    f->view.detail = f->booted;
    refuse(f, sensei_Gap9OtaResult_GAP9_OTA_INVALID, sensei_Gap9OtaFail_GAP9_OTA_FAIL_KEEP);
    return 0;
  }
  if (f->have_fresh && (uint32_t)(now(f) - f->fresh_ms) < G9OTA_OPEN_MIN_MS) {
    /* a forced begin resets GAP9 whether it resumes or not: the same rule as for a new transfer */
    f->view.detail = 0;
    refuse(f, sensei_Gap9OtaResult_GAP9_OTA_BUSY, sensei_Gap9OtaFail_GAP9_OTA_FAIL_RATE);
    return 0;
  }
  rc = session_start(f);
  if (rc != 0) {
    return rc;
  }
  f->forced = true;
  f->rec_begin = *b; /* waits for the boot loader and the check of the keep slot */
  if (f->ops->force_recovery(true) != 0) {
    session_end(f);
    refuse(f, sensei_Gap9OtaResult_GAP9_OTA_INVALID, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
    return 0;
  }
  rc = f->ops->reboot(f->trial.confirmed, on_force_boot, f);
  if (rc != 0) {
    (void)f->ops->force_recovery(false); /* the strap must not wait for another power-up */
    session_end(f);
    return rc;
  }
  /* Every forced begin resets GAP9 and ends in a session end: the rule of the new transfers holds for them too. */
  f->have_fresh = true;
  f->fresh_ms = now(f);
  return 0;
}

static int begin_request(struct g9ota_flow *f, const sensei_Gap9OtaBegin *b) {
  int rc;

  if (f->state == G9OTA_F_TRIAL) {
    refuse(f, sensei_Gap9OtaResult_GAP9_OTA_INVALID, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
    return 0;
  }
  if (b->size == 0) {
    return -EINVAL;
  }
  f->begin_fresh = !b->resume;
  if (f->begin_fresh && f->have_fresh && (uint32_t)(now(f) - f->fresh_ms) < G9OTA_OPEN_MIN_MS) {
    /* The slot's sectors wear out: GAP9 keeps this rule too, but forgets it when it loses
     * power. */
    f->view.detail = 0;
    refuse(f, sensei_Gap9OtaResult_GAP9_OTA_BUSY, sensei_Gap9OtaFail_GAP9_OTA_FAIL_RATE);
    return 0;
  }
  if (b->force_recovery && !in_recovery(f)) {
    return begin_forced(f, b);
  }
  rc = session_start(f);
  if (rc != 0) {
    return rc;
  }
  if (in_recovery(f)) { /* the keep slot is checked first */
    f->rec_begin = *b;
    job_status(f);
    submit(f, ST_REC_KEEP);
    return 0;
  }
  req_clear(f, G9OTA_OP_BEGIN);
  f->req.u.begin = *b;
  submit(f, ST_BEGIN);
  return 0;
}

static int chunk_request(struct g9ota_flow *f, const sensei_Gap9OtaChunk *c) {
  if (f->state != G9OTA_F_SESSION) {
    refuse(f, sensei_Gap9OtaResult_GAP9_OTA_INVALID, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
    return 0;
  }
  if (c->data.size == 0 || c->data.size > G9OTA_HOST_CHUNK_MAX) {
    return -EINVAL;
  }
  req_clear(f, G9OTA_OP_CHUNK);
  f->req.u.chunk.offset = c->offset;
  f->req.u.chunk.data.size = c->data.size;
  memcpy(f->req.u.chunk.data.bytes, c->data.bytes, c->data.size);
  submit(f, ST_CHUNK);
  return 0;
}

static int command_request(struct g9ota_flow *f, sensei_Gap9OtaAction action) {
  int rc;

  switch (action) {
  case sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS:
    if (f->state == G9OTA_F_TRIAL) {
      send_view(f); /* GAP9 is booting: the answer is what the flow knows */
      return 0;
    }
    job_status(f);
    submit(f, ST_STATUS);
    return 0;
  case sensei_Gap9OtaAction_GAP9_OTA_ACTION_FINISH:
    if (f->state != G9OTA_F_SESSION) {
      refuse(f, sensei_Gap9OtaResult_GAP9_OTA_INVALID, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
      return 0;
    }
    req_clear(f, G9OTA_OP_FINISH);
    submit(f, ST_FINISH);
    return 0;
  case sensei_Gap9OtaAction_GAP9_OTA_ACTION_ABORT:
    if (f->state == G9OTA_F_TRIAL) {
      send_view(f); /* a trial that runs is not stopped from outside */
      return 0;
    }
    job_abort(f);
    submit(f, ST_ABORT);
    return 0;
  case sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY:
    if (f->state != G9OTA_F_SESSION || !f->verified) {
      refuse(f, sensei_Gap9OtaResult_GAP9_OTA_INVALID, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
      return 0;
    }
    start_trial(f, f->verified_slot, f->verified_id);
    return 0;
  case sensei_Gap9OtaAction_GAP9_OTA_ACTION_REVERT:
    if (f->state == G9OTA_F_TRIAL) {
      refuse(f, sensei_Gap9OtaResult_GAP9_OTA_INVALID, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
      return 0;
    }
    rc = session_start(f);
    if (rc != 0) {
      return rc;
    }
    job_status(f);
    submit(f, ST_REVERT_STATUS);
    return 0;
  default:
    return -EINVAL;
  }
}

/* The three requests share their start: one at a time. `msg` is the Gap9OtaBegin or the Gap9OtaChunk, `action` the
 * command. */
static int request(struct g9ota_flow *f, char kind, const void *msg, sensei_Gap9OtaAction action, g9ota_reply_fn reply,
                   void *rctx) {
  int rc;

  if (f->busy) {
    return -EBUSY;
  }
  g9ota_flow_poll(f);
  f->busy = true;
  f->reply = reply;
  f->rctx = rctx;
  f->rec_retry = false;
  f->view.result = sensei_Gap9OtaResult_GAP9_OTA_OK;
  if (f->state == G9OTA_F_SESSION) {
    f->last_ms = now(f);
  }
  rc = kind == 'b' ? begin_request(f, msg) : kind == 'c' ? chunk_request(f, msg) : command_request(f, action);
  if (rc != 0) {
    f->busy = false;
  }
  if (g9ota_flow_needs_poll(f) && f->ops->wake != NULL) {
    f->ops->wake();
  }
  return rc;
}

int g9ota_flow_begin(struct g9ota_flow *f, const sensei_Gap9OtaBegin *req, g9ota_reply_fn reply, void *rctx) {
  return request(f, 'b', req, 0, reply, rctx);
}

int g9ota_flow_chunk(struct g9ota_flow *f, const sensei_Gap9OtaChunk *req, g9ota_reply_fn reply, void *rctx) {
  return request(f, 'c', req, 0, reply, rctx);
}

int g9ota_flow_command(struct g9ota_flow *f, sensei_Gap9OtaAction action, g9ota_reply_fn reply, void *rctx) {
  return request(f, 'a', NULL, action, reply, rctx);
}

/* ---- start and housekeeping ---- */

int g9ota_flow_init(struct g9ota_flow *f, const struct g9ota_ops *ops, const struct g9ota_store *stored) {
  memset(f, 0, sizeof(*f));
  if (ops == NULL || ops->job == NULL || ops->hold == NULL || ops->reboot == NULL || ops->set_slot == NULL ||
      ops->verify == NULL || ops->persist == NULL || ops->now_ms == NULL) {
    return -EINVAL; /* a required operation is missing: `f` is not usable */
  }
  f->ops = ops;
  f->state = G9OTA_F_IDLE;
  f->view.chunk_max = G9OTA_HOST_CHUNK_MAX;
  if (stored == NULL) {
    sensei_gap9_trial_init(&f->trial, 0);
    f->probe_pending = true; /* settings lost: ask GAP9 which slot answers, when it can be held */
    (void)ops->set_slot(0);
    return 0;
  }
  f->trial = stored->trial;
  f->known[0] = stored->known[0];
  f->known[1] = stored->known[1];
  f->min_version = stored->min_version;
  if (f->trial.open) { /* the nRF5340 reset in the middle of a trial: the confirmed slot boots */
    struct sensei_gap9_trial_out out = sensei_gap9_trial_step(&f->trial, SENSEI_GAP9_TRIAL_NRF_RESET, f->trial.trial);

    if (out.action == SENSEI_GAP9_TRIAL_BOOT) { /* a probe that was open: start it again */
      f->probe_pending = true;
      f->trial.open = false;
      f->trial.probing = false;
    } else {
      persist_keep(f);
      (void)ops->set_slot(out.slot);
      f->last_fail = out.action == SENSEI_GAP9_TRIAL_REVERT ? sensei_Gap9OtaFail_GAP9_OTA_FAIL_BOOT
                                                            : sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE;
    }
    return 0;
  }
  (void)ops->set_slot(f->trial.confirmed);
  return 0;
}

static void probe_start(struct g9ota_flow *f) {
  struct sensei_gap9_trial_out out;

  if (f->ops->hold(true) != 0) {
    return; /* SPI_A is busy: next time */
  }
  f->held = true;
  f->probe_pending = false;
  f->boot_waits = 0;
  f->boot_wait = false;
  f->state = G9OTA_F_TRIAL;
  f->expect_slot = -1;
  f->expect_id = 0; /* the slot that answers and passes its signature check is confirmed */
  out = sensei_gap9_trial_lost(&f->trial);
  trial_act(f, out);
}

void g9ota_flow_poll(struct g9ota_flow *f) {
  uint32_t t = now(f);

  if (f->dirty) {
    persist_keep(f);
  }
  if (f->boot_wait) { /* a reboot waited for SPI_A */
    f->boot_wait = false;
    boot_slot(f, f->boot_wait_slot);
  }
  if (f->probe_pending && !f->busy && f->state == G9OTA_F_IDLE) {
    probe_start(f);
    return;
  }
  if (f->state == G9OTA_F_SESSION && !f->busy &&
      ((uint32_t)(t - f->last_ms) >= G9OTA_FLOW_IDLE_MS || (uint32_t)(t - f->start_ms) >= G9OTA_FLOW_SESSION_MS)) {
    session_end(f); /* the transfer stays on GAP9 as a checkpoint and can be resumed */
  }
}

bool g9ota_flow_needs_poll(const struct g9ota_flow *f) {
  return g9ota_flow_active(f) || f->probe_pending || f->dirty || f->boot_wait;
}

bool g9ota_flow_active(const struct g9ota_flow *f) { return f->held || f->state == G9OTA_F_TRIAL; }

uint8_t g9ota_flow_slot(const struct g9ota_flow *f) { return f->trial.open ? f->trial.trial : f->trial.confirmed; }

/* ---- the view of the transfer ---- */

/* Take GAP9's answer into the view: state, offsets, and the slots when it sent them (else the slots of the last answer
 * stay). The flow adds the slot it confirmed itself. */
static void view_from_info(sensei_Gap9OtaReply *v, const sensei_Gap9OtaInfo *info) {
  v->result = info->result;
  v->stage = info->stage;
  v->fail = info->fail;
  v->detail = info->detail;
  v->offset = info->offset;
  v->size = info->size;
  v->chunk_max = info->chunk_max;
  v->slot_size = info->slot_size;
  v->booted_slot = info->booted_slot & 1u;
  if (info->has_slot_a && info->has_slot_b) {
    v->has_slot_a = v->has_slot_b = true;
    v->slot_a = info->slot_a;
    v->slot_b = info->slot_b;
  }
}
