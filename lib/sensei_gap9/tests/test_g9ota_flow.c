/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The update flow of the nRF5340 (src/g9ota_flow.c) against a fake of the GAP9 link: the
 * fake GAP9 runs the real writer (g9ota.c) on a RAM MRAM, boots the slot the slot line names
 * (and falls back to the other one when it is not an image, as the SSBL does), loses its RAM state
 * at every power cycle, and answers HELLO, status and self-test as told. Replies and callbacks
 * arrive late (pump()), as they do from the link thread. */

#include <stdio.h>

#include "check.h"
#include "g9ota_fake.h"
#include "sensei_gap9/g9ota_flow.h"
#include "sensei_ota_image.h"

/* ---- the fake GAP9 and its link ---- */

static struct {
  struct fake_mram mram;
  struct g9ota ota;
  uint8_t slot_line;
  uint8_t booted;
  bool on;
  /* knobs */
  int no_hello;   /* boots that give no HELLO */
  bool hello_bad; /* the HELLO has another major version */
  bool selftest_ok;
  int selftest_fails; /* the next self-tests fail, then they pass */
  bool lie_slot;      /* the app reports the slot of the slot line, not the one the SSBL started (no
                         hand-off) */
  bool job_timeout;   /* the next job times out */
  bool not_built;     /* a GAP9 build without the update, or one that is not armed: GAP9_NOT_BUILT */
  bool bad_job;       /* an older GAP9 firmware that does not know the job: GAP9_BAD_JOB */
  int job_timeouts;   /* the next jobs time out (GAP9 loses power) */
  int abort_timeouts; /* the next abort jobs time out */
  int persist_skip;   /* this many stores work before persist_fails starts */
  bool detect_fail;   /* the detect job of the trial does not run */
  bool detect_not_built;
  int reboot_busy;    /* the next reboots find SPI_A busy (-EBUSY) */
  int reboot_busy_cb; /* the next reboots are accepted, the callback says -EBUSY */
  uint32_t bad_id;    /* the signature of the image with this id does not check */
  /* in flight */
  g9ota_job_cb job_cb;
  void *job_user;
  sensei_Gap9OtaInfo job_info;
  int job_err;
  g9ota_check_cb check_cb;
  void *check_user;
  int check_err;
  g9ota_reboot_cb rb_cb;
  void *rb_user;
  int rb_err;
  sensei_Version rb_hello;
  /* what the flow did */
  int holds, releases, reboots, jobs, detects;
  uint8_t reboot_slots[16];
  bool held;
  int hold_rc;
  int persists;
  struct g9ota_store persisted;
  bool persist_fail;
  int persist_fails; /* the next stores fail */
  int verify_rc;
  int verifies;
  uint8_t line_log[16];
  int line_n;
  uint32_t clock_ms;
  bool image_ok[2];
} g;

static struct g9ota_cfg lay;

static void gboot(uint8_t slot) {
  struct g9ota tmp;
  sensei_Gap9Slot si;
  uint8_t first = slot;

  g.on = true;
  g.booted = first;
  g9ota_init(&tmp, &fake_ops, &g.mram, &lay, 0);
  if (!g9ota_slot_info(&tmp, first, &si)) { /* the SSBL falls back to the other slot */
    g.booted = 1 - first;
  }
  g9ota_init(&g.ota, &fake_ops, &g.mram, &lay, g.booted); /* the RAM state of GAP9 starts again */
  g9ota_boot_note(&g.ota);
}

/* Common start of a job of the fake GAP9: a job powers GAP9 as the link does (boot the slot line); a job that
 * times out loses GAP9's power. Returns 0 or -ETIMEDOUT. */
static int job_start(bool is_abort) {
  g.jobs++;
  if (!g.on) {
    gboot(g.slot_line);
  }
  if (g.job_timeout || g.job_timeouts > 0 || (g.abort_timeouts > 0 && is_abort)) {
    if (g.job_timeouts > 0) {
      g.job_timeouts--;
    } else if (g.job_timeout) {
      g.job_timeout = false;
    } else {
      g.abort_timeouts--;
    }
    g.job_timeout = false;
    g.on = false;
    return -ETIMEDOUT;
  }
  return 0;
}

static int f_job(const struct g9ota_req *req, g9ota_job_cb cb, void *user) {
  sensei_Gap9OtaInfo *r = &g.job_info;

  if (g.job_cb != NULL) {
    return -EBUSY;
  }
  memset(r, 0, sizeof(*r));
  g.job_err = job_start(req->op == G9OTA_OP_ABORT);
  if (g.job_err == 0 && (g.not_built || g.bad_job)) {
    /* a GAP9 build without the update, one that is not armed (NOT_BUILT), or an older firmware that does
     * not know the job (BAD_JOB): the app's glue maps both to -ENOTSUP */
    g.job_err = -ENOTSUP;
  } else if (g.job_err == 0) {
    switch (req->op) {
    case G9OTA_OP_BEGIN:
      (void)g9ota_begin(&g.ota, &req->u.begin, r);
      break;
    case G9OTA_OP_CHUNK:
      (void)g9ota_chunk(&g.ota, &req->u.chunk, r);
      break;
    case G9OTA_OP_STATUS:
      if (req->u.status.verify) {
        (void)g9ota_verify(&g.ota, (uint8_t)req->u.status.slot, r);
        break;
      }
      (void)g9ota_status(&g.ota, r);
      if (g.lie_slot) {
        r->booted_slot = g.slot_line;
      }
      break;
    case G9OTA_OP_FINISH:
      (void)g9ota_finish(&g.ota, r);
      break;
    case G9OTA_OP_ABORT:
      (void)g9ota_abort(&g.ota, r);
      break;
    }
  }
  g.job_cb = cb;
  g.job_user = user;
  return 0;
}

/* The checks of the app on a new image on trial: a self-test job and, for a build with a detector, a detect job
 * (the node's trial_check). The fake runs both when the call is made and answers later. */
static bool with_detector;

static int f_check(g9ota_check_cb cb, void *user) {
  if (g.check_cb != NULL) {
    return -EBUSY;
  }
  g.check_err = job_start(false);
  if (g.check_err == 0) { /* the self-test */
    bool passed = g.selftest_ok && g.selftest_fails == 0;

    if (g.selftest_fails > 0) {
      g.selftest_fails--;
    }
    g.check_err = passed ? 0 : -EIO;
  }
  if (g.check_err == 0 && with_detector) { /* the detect job; a build without detector answers NOT_BUILT: fine */
    g.check_err = job_start(false);
    if (g.check_err == 0) {
      g.detects++;
      g.check_err = g.detect_not_built ? 0 : (g.detect_fail ? -EIO : 0);
    }
  }
  g.check_cb = cb;
  g.check_user = user;
  return 0;
}

static int f_hold(bool on) {
  if (on) {
    g.holds++;
    if (g.hold_rc != 0) {
      return g.hold_rc;
    }
    g.held = true;
    if (!g.on) {
      gboot(g.slot_line);
    }
  } else {
    g.releases++;
    g.held = false;
  }
  return 0;
}

static int f_reboot(uint8_t slot, g9ota_reboot_cb cb, void *user) {
  if (g.rb_cb != NULL) {
    return -EBUSY;
  }
  if (g.reboot_busy > 0) {
    g.reboot_busy--;
    return -EBUSY;
  }
  g.reboot_slots[g.reboots++ % 16] = slot;
  g.slot_line = slot;
  g.on = false;
  if (g.no_hello > 0) {
    g.no_hello--;
    g.rb_err = -ETIMEDOUT;
  } else if (g.hello_bad) {
    g.rb_err = -EPROTO;
  } else {
    gboot(slot);
    g.rb_err = 0;
  }
  if (g.reboot_busy_cb > 0) { /* the link took it, then SPI_A was gone */
    g.reboot_busy_cb--;
    g.rb_err = -EBUSY;
    g.slot_line = slot;
  }
  memset(&g.rb_hello, 0, sizeof(g.rb_hello));
  {
    struct g9ota tmp;
    sensei_Gap9Slot si;

    g9ota_init(&tmp, &fake_ops, &g.mram, &lay, 0);
    g9ota_slot_info(&tmp, g.booted, &si); /* the firmware reports its own version */
    g.rb_hello.major = si.has_version ? si.version.major : 1;
    g.rb_hello.minor = si.version.minor;
    g.rb_hello.patch = si.version.patch;
  }
  g.rb_cb = cb;
  g.rb_user = user;
  return 0;
}

static int f_set_slot(uint8_t slot) {
  g.slot_line = slot;
  g.line_log[g.line_n++ % 16] = slot;
  return 0;
}

static int f_verify(const uint8_t hash[32], const uint8_t sig[64], const uint8_t *kh, size_t kl) {
  (void)hash;
  (void)sig;
  (void)kh;
  (void)kl;
  g.verifies++;
  if (g.bad_id != 0 && sensei_image_le32(hash) == g.bad_id) {
    return -EBADMSG;
  }
  return g.verify_rc;
}

static int f_persist(const struct g9ota_store *st) {
  g.persists++;
  if (g.persist_skip > 0) {
    g.persist_skip--;
  } else if (g.persist_fails > 0) {
    g.persist_fails--;
    return -EIO;
  }
  if (g.persist_fail) {
    return -EIO;
  }
  g.persisted = *st;
  return 0;
}

static uint32_t f_now(void) { return g.clock_ms; }

static int wakes; /* the flow asked for its housekeeping timer */

static void f_wake(void) { wakes++; }

static const struct g9ota_ops ops = {f_job, f_hold, f_reboot, f_set_slot, f_verify, f_persist, f_now, f_check, f_wake};
/* an app without checks of its own: the check answers at once */
static int f_nocheck(g9ota_check_cb cb, void *user) {
  cb(0, user);
  return 0;
}

static const struct g9ota_ops ops_nocheck = {f_job,     f_hold, f_reboot,  f_set_slot, f_verify,
                                             f_persist, f_now,  f_nocheck, f_wake};
static const struct g9ota_ops *cur_ops = &ops;

static struct g9ota_flow flow;

/* Deliver what is waiting (one callback at a time, like the work queue). */
static bool pump(void) {
  if (g.rb_cb != NULL) {
    g9ota_reboot_cb cb = g.rb_cb;

    g.rb_cb = NULL;
    cb(g.rb_err, g.rb_err == 0 || g.rb_err == -EPROTO ? &g.rb_hello : NULL, g.rb_user);
    return true;
  }
  if (g.job_cb != NULL) {
    g9ota_job_cb cb = g.job_cb;

    g.job_cb = NULL;
    cb(g.job_err, g.job_err ? NULL : &g.job_info, g.job_user);
    return true;
  }
  if (g.check_cb != NULL) {
    g9ota_check_cb cb = g.check_cb;

    g.check_cb = NULL;
    cb(g.check_err, g.check_user);
    return true;
  }
  return false;
}

static void pump_all(void) {
  for (int i = 0; i < 100 && pump(); i++) {
  }
}

/* the replies the host sees */
static struct {
  int err;
  sensei_Gap9OtaReply reply;
} replies[64];
static int nreplies;

static void on_reply(void *ctx, int err, const sensei_Gap9OtaReply *v) {
  (void)ctx;
  replies[nreplies % 64].err = err;
  memset(&replies[nreplies % 64].reply, 0, sizeof(sensei_Gap9OtaReply));
  if (err == 0) {
    replies[nreplies % 64].reply = *v;
  }
  nreplies++;
}

/* The error of the last reply (0: a view), and the view as the host sees it. */
static int last_err(void) { return replies[(nreplies - 1) % 64].err; }

static const sensei_Gap9OtaReply *R(void) { return &replies[(nreplies - 1) % 64].reply; }

static sensei_Gap9OtaBegin B;
static sensei_Gap9OtaChunk C;
static sensei_Gap9OtaAction A;

static int ask_begin(void) {
  int rc = g9ota_flow_begin(&flow, &B, on_reply, NULL);

  if (rc == 0) {
    pump_all();
  }
  return rc;
}

static int ask_chunk(void) {
  int rc = g9ota_flow_chunk(&flow, &C, on_reply, NULL);

  if (rc == 0) {
    pump_all();
  }
  return rc;
}

static int ask_cmd(void) {
  int rc = g9ota_flow_command(&flow, A, on_reply, NULL);

  if (rc == 0) {
    pump_all();
  }
  return rc;
}

static int do_begin(const uint8_t *img, size_t n, bool resume) {
  memset(&B, 0, sizeof(B));
  B.size = (uint32_t)n;
  file_sha(img, n, B.sha256);
  B.resume = resume;
  return ask_begin();
}

static int do_chunk(const uint8_t *img, uint32_t off, size_t n) {
  memset(&C, 0, sizeof(C));
  C.offset = off;
  C.data.size = (pb_size_t)n;
  memcpy(C.data.bytes, img + off, n);
  return ask_chunk();
}

static int do_cmd(sensei_Gap9OtaAction a) {
  A = a;
  return ask_cmd();
}

/* The whole transfer with 188 B chunks; returns the last reply's offset. */
static uint32_t send_all(const uint8_t *img, size_t n, uint32_t from) {
  uint32_t off = from;

  while (off < n) {
    size_t m = n - off < 188 ? n - off : 188;

    if (do_chunk(img, off, m) != 0 || last_err() != 0 || R()->result != sensei_Gap9OtaResult_GAP9_OTA_OK) {
      break;
    }
    off = R()->offset;
  }
  return off;
}

/* GAP9 loses power (its RAM state is gone, the MRAM stays) and the node's session ends. */
static void session_gone(void) {
  g.on = false;
  if (g.held) {
    g.held = false;
    g.releases++;
  }
  flow.state = G9OTA_F_IDLE;
  flow.held = false;
  flow.verified = false;
}

static uint8_t image[SENSEI_G9_SLOT_SIZE];
static size_t img_n;

static void setup(const struct sensei_gap9_trial *stored, uint8_t running_slot, uint8_t major, uint8_t minor) {
  static struct img_opt o;
  static uint8_t cur[SENSEI_G9_SLOT_SIZE];
  size_t n;

  memset(&g, 0, sizeof(g));
  wakes = 0;
  memset(g.mram.mem, 0xFF, sizeof(g.mram.mem));
  g.mram.cut_at = -1;
  g.selftest_ok = true;
  g.verify_rc = 0;
  lay = fake_layout();
  nreplies = 0;
  /* the image that runs, in the running slot */
  o = (struct img_opt){.payload = 6000, .major = major, .minor = minor, .kind = FAKE_KIND, .sign = true, .seed = 5};
  n = build_image(cur, &o);
  memcpy(g.mram.mem + lay.slot_base[running_slot], cur, n);
  g.slot_line = running_slot;
  if (stored != NULL) {
    struct g9ota_store st = {.trial = *stored, .known = {0, 0}};

    g9ota_flow_init(&flow, cur_ops, &st);
  } else {
    g9ota_flow_init(&flow, cur_ops, NULL);
  }
}

static void make_new(uint8_t major, uint8_t minor, size_t payload) {
  struct img_opt o = {.payload = payload, .major = major, .minor = minor, .kind = FAKE_KIND, .sign = true, .seed = 77};

  img_n = build_image(image, &o);
}

static uint32_t id_of(const uint8_t *img, size_t n) {
  uint32_t hdr = sensei_image_le16(img + 8), len = sensei_image_le32(img + 12), prot = sensei_image_le16(img + 10);
  uint8_t d[32];

  file_sha(img, hdr + len + prot, d);
  (void)n;
  return sensei_image_le32(d);
}

static struct sensei_gap9_trial closed(uint8_t confirmed) {
  struct sensei_gap9_trial t;

  sensei_gap9_trial_init(&t, confirmed);
  return t;
}

/* uploads and verifies a new image: leaves the flow in a session with `verified` set */
static void upload(void) {
  make_new(1, 4, 30000);
  CHECK(do_begin(image, img_n, false) == 0 && R()->result == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING && R()->offset == 0 && R()->chunk_max == 188);
  CHECK(wakes > 0); /* a session wants the housekeeping timer */
  CHECK(send_all(image, img_n, 0) == img_n);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_FINISH) == 0);
}

/* ---- tests ---- */

static void test_update_and_trial(uint8_t running) {
  struct sensei_gap9_trial t = closed(running);
  uint8_t other = 1 - running;

  setup(&t, running, 1, 3);
  CHECK(g.line_n == 1 && g.line_log[0] == running); /* the slot line follows the stored slot */
  CHECK(!g9ota_flow_active(&flow));

  /* status without a session: a job, GAP9 powers on for it, no hold */
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0);
  CHECK(R()->booted_slot == running && R()->confirmed_slot == running && g.holds == 0);
  CHECK((running ? R()->slot_b : R()->slot_a).valid && !(running ? R()->slot_a : R()->slot_b).valid);

  /* chunk, finish and apply need the session of a begin */
  CHECK(do_chunk(image, 0, 16) == 0 && R()->result == sensei_Gap9OtaResult_GAP9_OTA_INVALID &&
        R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_FINISH) == 0 &&
        R()->result == sensei_Gap9OtaResult_GAP9_OTA_INVALID);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0 &&
        R()->result == sensei_Gap9OtaResult_GAP9_OTA_INVALID);

  upload();
  CHECK(R()->result == sensei_Gap9OtaResult_GAP9_OTA_OK && R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED);
  CHECK(g.verifies == 1 && g.holds == 1 && g.held && g9ota_flow_active(&flow));
  CHECK(flow.verified && flow.verified_slot == other);

  /* apply: the reply comes at once and says APPLYING; the trial then runs */
  g.persists = 0;
  A = sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY;
  CHECK(g9ota_flow_command(&flow, A, on_reply, NULL) == 0);
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_APPLYING && R()->result == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(R()->booted_slot == other && R()->confirmed_slot == running);
  CHECK(g.persists == 1 && g.persisted.trial.open && g.persisted.trial.trial == other &&
        g.persisted.trial.confirmed == running);
  CHECK(g.reboots == 1 && g.reboot_slots[0] == other && g.held); /* power cycled, the hold stays during the trial */
  /* while the trial runs the node answers from what it knows and refuses a new transfer */
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0); /* answered from the cache, then the trial runs on */
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_APPLYING && R()->booted_slot == other);
  pump_all();
  CHECK(!flow.trial.open && flow.trial.confirmed == other); /* confirmed */
  CHECK(g.persisted.trial.confirmed == other && !g.persisted.trial.open);
  CHECK(g.slot_line == other && !g.held && g.releases == 1);
  CHECK(!g9ota_flow_active(&flow));
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0);
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_IDLE && R()->booted_slot == other &&
        R()->confirmed_slot == other);
  CHECK((other ? R()->slot_b : R()->slot_a).image_id == id_of(image, img_n));
  CHECK((other ? R()->slot_b : R()->slot_a).version.minor == 4 && (running ? R()->slot_b : R()->slot_a).valid);

  /* the next update goes into the slot that ran before (the one that is not running now) */
  g.clock_ms += G9OTA_OPEN_MIN_MS;
  make_new(1, 5, 12000);
  CHECK(do_begin(image, img_n, false) == 0 && R()->result == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(send_all(image, img_n, 0) == img_n);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_FINISH) == 0 &&
        R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED);
  CHECK(flow.verified_slot == running);
}

static void run_trial_failure(const char *what, void (*knob)(void), sensei_Gap9OtaFail want_fail, int want_reboots) {
  struct sensei_gap9_trial t = closed(0);

  setup(&t, 0, 1, 3);
  upload();
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED);
  knob();
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_APPLYING);
  pump_all();
  if (flow.trial.confirmed != 0 || flow.trial.open) {
    printf("FAIL: %s: not reverted (confirmed %u open %d)\n", what, flow.trial.confirmed, flow.trial.open);
    fails++;
  }
  CHECK(flow.trial.confirmed == 0 && !flow.trial.open);
  CHECK(g.persisted.trial.confirmed == 0 && !g.persisted.trial.open);
  CHECK(g.slot_line == 0);
  CHECK(g.reboots == want_reboots); /* the boots of the new slot, then the confirmed one */
  CHECK(g.reboot_slots[want_reboots - 1] == 0);
  CHECK(!g.held && !g9ota_flow_active(&flow));
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0);
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED && R()->fail == want_fail);
  CHECK(R()->confirmed_slot == 0 && R()->booted_slot == 0);
  /* a new begin clears the failure */
  CHECK(do_begin(image, img_n, true) == 0 && R()->stage != sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED);
}

static void k_nohello(void) { g.no_hello = 2; }
static void k_hellobad(void) { g.hello_bad = true; }
static void k_selftest(void) { g.selftest_ok = false; }
static void k_corrupt(void) { /* the new slot does not load: the SSBL falls back to the old one */
  memset(g.mram.mem + SENSEI_G9_SLOT_B_OFF + 0x200, 0, 16); /* the container header is gone */
}
static void k_corrupt_lie(void) { /* the same, and the app says the slot of the slot line (no hand-off from the SSBL) */
  k_corrupt();
  g.lie_slot = true;
}

static void test_trial_failures(void) {
  run_trial_failure("no HELLO", k_nohello, sensei_Gap9OtaFail_GAP9_OTA_FAIL_BOOT, 3);
  run_trial_failure("HELLO of another protocol", k_hellobad, sensei_Gap9OtaFail_GAP9_OTA_FAIL_BOOT, 2);
  run_trial_failure("self-test fails", k_selftest, sensei_Gap9OtaFail_GAP9_OTA_FAIL_BOOT, 2);
  run_trial_failure("the new slot does not load", k_corrupt, sensei_Gap9OtaFail_GAP9_OTA_FAIL_BOOT, 2);
  /* the version in the HELLO is the firmware's own: it shows that the old image runs */
  run_trial_failure("the new slot does not load, slot reported wrong", k_corrupt_lie,
                    sensei_Gap9OtaFail_GAP9_OTA_FAIL_BOOT, 2);
}

static void test_one_lost_hello(void) {
  struct sensei_gap9_trial t = closed(0);

  setup(&t, 0, 1, 3);
  upload();
  g.no_hello = 1; /* a power glitch: the second boot works */
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 1 && !flow.trial.open && g.reboots == 2);
}

static void test_signature_and_version(void) {
  struct sensei_gap9_trial t = closed(0);

  /* another key */
  setup(&t, 0, 1, 3);
  g.verify_rc = -EACCES;
  upload();
  CHECK(R()->result == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
        R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIGNATURE && R()->detail == 1);
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED && !flow.verified);
  CHECK(!g.held && g.releases == 1);              /* the session ended */
  CHECK(!(R()->has_slot_b && R()->slot_b.valid)); /* GAP9 dropped the image: its slot is not one */
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0 &&
        R()->result == sensei_Gap9OtaResult_GAP9_OTA_INVALID);
  CHECK(g.reboots == 0);

  /* a signature that does not check */
  setup(&t, 0, 1, 3);
  g.verify_rc = -EBADMSG;
  upload();
  CHECK(R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIGNATURE && R()->detail == 2);

  /* older than the image that runs: GAP9 compares the headers of the two slots (the signature
   * covers them) */
  setup(&t, 0, 1, 9);
  upload(); /* 1.4.0 against 1.9.0 */
  CHECK(R()->result == sensei_Gap9OtaResult_GAP9_OTA_FAILED && R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_DOWNGRADE);
  CHECK(g.verifies == 0 && !g.held); /* refused before the signature is looked at; the session ended */
  setup(&t, 0, 1, 4);
  upload(); /* the same version is allowed */
  CHECK(R()->result == sensei_Gap9OtaResult_GAP9_OTA_OK && R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED);
}

static void test_nrf_reset_during_trial(void) {
  struct sensei_gap9_trial t = closed(0);
  struct sensei_gap9_trial open = {.confirmed = 0, .trial = 1, .boots = 1, .open = true, .probing = false};

  setup(&open, 0, 1, 3);
  CHECK(!flow.trial.open && flow.trial.confirmed == 0);
  CHECK(g.persisted.trial.confirmed == 0 && !g.persisted.trial.open && g.persists == 1);
  CHECK(g.slot_line == 0 && g.reboots == 0);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0);
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED && R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_BOOT);
  CHECK(do_begin(image, 100, false) == 0); /* a new update clears it */
  (void)t;
}

static void test_lost_settings(void) {
  /* slot A holds an image: it answers, A is confirmed */
  setup(NULL, 0, 1, 3);
  CHECK(flow.probe_pending && g.line_n == 1 && g.line_log[0] == 0);
  g9ota_flow_poll(&flow);
  CHECK(g.holds == 1 && g.held && g.reboots == 1 && g.reboot_slots[0] == 0);
  pump_all();
  CHECK(!flow.trial.open && flow.trial.confirmed == 0 && g.persisted.trial.confirmed == 0 && !g.held &&
        !flow.probe_pending);

  /* slot A is empty, B holds the image: A does not answer, B is confirmed */
  setup(NULL, 1, 1, 3);
  g.no_hello = 1; /* the boot of the empty slot A gives no HELLO */
  g9ota_flow_poll(&flow);
  pump_all();
  CHECK(flow.trial.confirmed == 1 && !flow.trial.open && g.slot_line == 1);
  CHECK(g.reboots == 2 && g.reboot_slots[0] == 0 && g.reboot_slots[1] == 1);

  /* neither answers: slot A stays selected */
  setup(NULL, 0, 1, 3);
  g.no_hello = 4;
  g9ota_flow_poll(&flow);
  pump_all();
  CHECK(flow.trial.confirmed == 0 && !flow.trial.open && g.slot_line == 0 && !g.held);

  /* SPI_A is busy: the probe waits and tries again */
  setup(NULL, 0, 1, 3);
  g.hold_rc = -EBUSY;
  g9ota_flow_poll(&flow);
  CHECK(flow.probe_pending && g.reboots == 0);
  g.hold_rc = 0;
  g9ota_flow_poll(&flow);
  pump_all();
  CHECK(!flow.probe_pending && flow.trial.confirmed == 0 && g.reboots == 1);
}

static void test_session_rules(void) {
  struct sensei_gap9_trial t = closed(0);

  setup(&t, 0, 1, 3);
  make_new(1, 4, 40000);
  /* SPI_A busy: the begin is not taken, the app sends an error */
  g.hold_rc = -EBUSY;
  memset(&B, 0, sizeof(B));
  B.size = (uint32_t)img_n;
  file_sha(image, img_n, B.sha256);
  CHECK(g9ota_flow_begin(&flow, &B, on_reply, NULL) == -EBUSY);
  CHECK(!flow.busy && !g.held);
  g.hold_rc = 0;

  /* an invalid request does not take the hold */
  memset(&B, 0, sizeof(B));
  B.size = 0;
  CHECK(g9ota_flow_begin(&flow, &B, on_reply, NULL) == -EINVAL && !g.held);

  /* a second request while one is in flight */
  CHECK(do_begin(image, img_n, false) == 0);
  A = sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS;
  CHECK(g9ota_flow_command(&flow, A, on_reply, NULL) == 0); /* taken: the job is in flight */
  CHECK(g9ota_flow_command(&flow, A, on_reply, NULL) == -EBUSY);
  pump_all();

  /* half the image, then the host goes quiet: the session ends after 120 s, the transfer stays on
   * GAP9 */
  g.clock_ms = 1000;
  CHECK(do_begin(image, img_n, true) == 0);
  {
    uint32_t off = 0;

    while (off < 3 * G9OTA_SECTOR + 100) {
      CHECK(do_chunk(image, off, 188) == 0);
      off = R()->offset;
    }
    CHECK(off > 3 * G9OTA_SECTOR);
  }
  g.clock_ms += G9OTA_FLOW_IDLE_MS - 1000;
  g9ota_flow_poll(&flow);
  CHECK(g.held); /* not yet */
  g.clock_ms += 1001;
  g9ota_flow_poll(&flow);
  CHECK(!g.held && !g9ota_flow_active(&flow));
  CHECK(do_chunk(image, 4 * G9OTA_SECTOR, 188) == 0 &&
        R()->result == sensei_Gap9OtaResult_GAP9_OTA_INVALID); /* no session */
  /* resume: begin finds the checkpoint at the last whole sector */
  g.on = false; /* GAP9 was powered off meanwhile */
  CHECK(do_begin(image, img_n, true) == 0);
  CHECK(R()->result == sensei_Gap9OtaResult_GAP9_OTA_OK && R()->offset == 3 * G9OTA_SECTOR && g.held);
  CHECK(send_all(image, img_n, R()->offset) == img_n);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_FINISH) == 0 &&
        R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED);

  /* the longest session: renewed for ever by requests, it still ends */
  g.clock_ms += 1;
  for (int i = 0; i < 50; i++) {
    g.clock_ms += 60000;
    CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0);
    g9ota_flow_poll(&flow);
    if (!g.held) {
      break;
    }
  }
  CHECK(!g.held && g.clock_ms - 1000 >= G9OTA_FLOW_SESSION_MS - 60000);

  /* abort: GAP9 drops the transfer, the hold goes */
  setup(&t, 0, 1, 3);
  CHECK(do_begin(image, img_n, false) == 0 && g.held);
  CHECK(send_all(image, img_n, 0) > 0);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_ABORT) == 0);
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_IDLE && !g.held && !R()->slot_b.valid);
  /* abort without a session still asks GAP9 (an open checkpoint is dropped) */
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_ABORT) == 0 &&
        R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_IDLE);

  /* a begin that GAP9 refuses (an image that is too large) does not keep GAP9 */
  g.clock_ms += G9OTA_OPEN_MIN_MS;
  memset(&B, 0, sizeof(B));
  B.size = SENSEI_G9_SLOT_SIZE + 1;
  CHECK(ask_begin() == 0 && R()->result == sensei_Gap9OtaResult_GAP9_OTA_INVALID &&
        R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIZE);
  CHECK(!g.held);

  /* GAP9 does not answer: an error, the session is still there for a retry */
  setup(&t, 0, 1, 3);
  make_new(1, 4, 20000);
  CHECK(do_begin(image, img_n, false) == 0 && g.held);
  g.job_timeout = true;
  CHECK(do_chunk(image, 0, 188) == 0);
  CHECK(last_err() == -ETIMEDOUT);
  CHECK(g.held);
  /* GAP9 lost its RAM state: begin with resume starts at the checkpoint (0), the chunk goes
   * through */
  CHECK(do_begin(image, img_n, true) == 0 && R()->offset == 0);
  CHECK(do_chunk(image, 0, 188) == 0 && R()->result == sensei_Gap9OtaResult_GAP9_OTA_OK && R()->offset == 188);
}

static void test_not_built(void) {
  struct sensei_gap9_trial t = closed(0);

  setup(&t, 0, 1, 3);
  g.not_built = true;
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0);
  CHECK(last_err() == -ENOTSUP);
  CHECK(do_begin(image, 1000, false) == 0 && last_err() == -ENOTSUP);
  CHECK(!g.held); /* the session does not stay on a GAP9 that cannot be updated */
}

static void test_revert(void) {
  struct sensei_gap9_trial t = closed(0);

  /* update 0 -> 1, then revert to 0 */
  setup(&t, 0, 1, 3);
  upload();
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 1);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_REVERT) == 0);
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_APPLYING && R()->booted_slot == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 0 && !flow.trial.open && g.slot_line == 0 && !g.held);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0 && R()->booted_slot == 0 && R()->confirmed_slot == 0 &&
        R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_IDLE);

  /* nothing to go back to */
  setup(&t, 0, 1, 3);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_REVERT) == 0);
  CHECK(R()->result == sensei_Gap9OtaResult_GAP9_OTA_FAILED && R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_NO_SLOT);
  CHECK(!g.held && g.reboots == 0);

  /* the other image does not start: back on the confirmed one */
  setup(&t, 0, 1, 3);
  upload();
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 1);
  g.hello_bad = true;
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_REVERT) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 1 && !flow.trial.open);
  g.hello_bad = false;
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0 && R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_BOOT);
}

static void test_during_trial(void) {
  struct sensei_gap9_trial t = closed(0);

  setup(&t, 0, 1, 3);
  upload();
  A = sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY;
  CHECK(g9ota_flow_command(&flow, A, on_reply, NULL) == 0); /* nothing is pumped: the trial is open */
  A = sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS;
  g.jobs = 0;
  CHECK(g9ota_flow_command(&flow, A, on_reply, NULL) == 0);
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_APPLYING &&
        g.jobs == 0); /* answered from what the node knows */
  {
    /* refused or answered from the cache at once: nothing is pumped in between */
    static const struct {
      sensei_Gap9OtaAction a;
      bool refused;
    } cmds[] = {{sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY, true},
                {sensei_Gap9OtaAction_GAP9_OTA_ACTION_REVERT, true},
                {sensei_Gap9OtaAction_GAP9_OTA_ACTION_ABORT, false}};

    memset(&B, 0, sizeof(B));
    B.size = (uint32_t)img_n;
    file_sha(image, img_n, B.sha256);
    CHECK(g9ota_flow_begin(&flow, &B, on_reply, NULL) == 0 && R()->result == sensei_Gap9OtaResult_GAP9_OTA_INVALID);
    for (unsigned i = 0; i < 3; i++) {
      A = cmds[i].a;
      CHECK(g9ota_flow_command(&flow, A, on_reply, NULL) == 0);
      CHECK(cmds[i].refused ? R()->result == sensei_Gap9OtaResult_GAP9_OTA_INVALID
                            : R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_APPLYING);
    }
    CHECK(g.jobs == 0);
  }
  pump_all();
  CHECK(flow.trial.confirmed == 1);
}

static void test_persist_failure(void) {
  struct sensei_gap9_trial t = closed(0);

  setup(&t, 0, 1, 3);
  upload();
  g.persist_fail = true; /* the trial cannot be stored: it is not started */
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  CHECK(R()->result == sensei_Gap9OtaResult_GAP9_OTA_FAILED && R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH);
  CHECK(g.reboots == 0 && !flow.trial.open);
}

/* ---- review round 9 ---- */

/* The image that upload() sends: its id (what GAP9 reports for the slot). */
static uint32_t new_id(void) { return id_of(image, img_n); }

/* Q2 (q_flow.c of the review): the signature of an image fails, the abort that follows gets no
 * answer, and the slot still holds the image. REVERT must not start it, and the lost-settings probe
 * must not confirm it. */
static void test_unverified_slot(void) {
  struct sensei_gap9_trial t = closed(0);

  /* the abort is repeated and gets through the second time */
  setup(&t, 0, 1, 3);
  g.verify_rc = -EBADMSG;
  g.job_timeouts = 0;
  make_new(1, 4, 30000);
  CHECK(do_begin(image, img_n, false) == 0);
  CHECK(send_all(image, img_n, 0) == img_n);
  g.abort_timeouts = 1; /* the first abort gets no answer */
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_FINISH) == 0);
  CHECK(R()->result == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
        R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIGNATURE && R()->detail == 2);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0);
  CHECK(!R()->slot_b.valid); /* the retry dropped the image */

  /* the abort never gets through (three times): the slot is an image for GAP9 */
  setup(&t, 0, 1, 3);
  g.verify_rc = -EBADMSG;
  make_new(1, 4, 30000);
  CHECK(do_begin(image, img_n, false) == 0);
  CHECK(send_all(image, img_n, 0) == img_n);
  g.abort_timeouts = 1 + G9OTA_FLOW_ABORT_RETRIES;
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_FINISH) == 0);
  CHECK(R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIGNATURE && g.abort_timeouts == 0 && !g.held);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0);
  CHECK(R()->slot_b.valid); /* GAP9 says it is an image: only the node knows better */
  g.bad_id = new_id();      /* the signature of exactly this image does not check */
  g.verify_rc = 0;
  g.verifies = 0;
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_REVERT) == 0);
  pump_all();
  CHECK(g.verifies == 1); /* GAP9 read the slot, the node looked at the signature */
  CHECK(R()->result == sensei_Gap9OtaResult_GAP9_OTA_FAILED && R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIGNATURE);
  CHECK(flow.trial.confirmed == 0 && !flow.trial.open && g.reboots == 0 && !g.held);
  CHECK(flow.known[1] == 0);

  /* the same image after a restart of the nRF5340: nothing known either, same answer */
  {
    struct g9ota_store st = {.trial = closed(0), .known = {0, 0}};

    g.reboots = 0;
    g9ota_flow_init(&flow, cur_ops, &st);
    CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_REVERT) == 0);
    pump_all();
    CHECK(R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIGNATURE && g.reboots == 0);
  }

  /* lost settings, slot A holds the image that does not verify, slot B a good one */
  setup(NULL, 1, 1, 3); /* B runs the good image */
  {
    static struct img_opt o;
    static uint8_t bad[SENSEI_G9_SLOT_SIZE];
    size_t n;

    o = (struct img_opt){.payload = 7000, .major = 1, .minor = 3, .kind = FAKE_KIND, .sign = true, .seed = 99};
    n = build_image(bad, &o);
    memcpy(g.mram.mem + lay.slot_base[0], bad, n);
    g.bad_id = id_of(bad, n);
  }
  g9ota_flow_poll(&flow);
  pump_all();
  CHECK(g.reboots == 2 && g.reboot_slots[0] == 0 && g.reboot_slots[1] == 1);
  CHECK(flow.trial.confirmed == 1 && !flow.trial.open && g.slot_line == 1);
  CHECK(flow.known[1] != 0 && flow.known[0] == 0 && g.persisted.known[1] == flow.known[1]);

  /* lost settings, both slots verify, the self-test of A fails: B is confirmed with its own id */
  setup(NULL, 0, 1, 3);
  {
    static struct img_opt o;
    static uint8_t other[SENSEI_G9_SLOT_SIZE];
    size_t n;

    o = (struct img_opt){.payload = 7000, .major = 1, .minor = 3, .kind = FAKE_KIND, .sign = true, .seed = 98};
    n = build_image(other, &o);
    memcpy(g.mram.mem + lay.slot_base[1], other, n);
  }
  g.selftest_fails = 1;
  g9ota_flow_poll(&flow);
  pump_all();
  CHECK(flow.trial.confirmed == 1 && !flow.trial.open && g.slot_line == 1 && g.reboots == 2);
  CHECK(flow.known[0] != 0 && flow.known[1] != 0 && flow.known[0] != flow.known[1]);
  CHECK(flow.image_id == flow.known[1]);

  /* lost settings, only slot A, which verifies: A is confirmed, its id kept */
  setup(NULL, 0, 1, 3);
  g9ota_flow_poll(&flow);
  pump_all();
  CHECK(flow.trial.confirmed == 0 && g.verifies == 1 && flow.known[0] != 0);
  CHECK(g.persisted.known[0] == flow.known[0]);
}

/* Q2: ids that the node knows: update, then revert goes without a second verification. */
static void test_known_ids(void) {
  struct sensei_gap9_trial t = closed(0);

  setup(&t, 0, 1, 3);
  upload();
  CHECK(flow.known[1] == new_id() && g.persisted.known[1] == new_id());
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 1 && g.persisted.known[1] == new_id());
  /* revert to slot A: its image was never verified by this node (it came with the factory
   * flash): GAP9 reads it, the node checks it, the id is kept */
  g.verifies = 0;
  CHECK(flow.known[0] == 0);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_REVERT) == 0);
  pump_all();
  CHECK(g.verifies == 1 && flow.trial.confirmed == 0 && flow.known[0] != 0 && g.persisted.known[0] == flow.known[0] &&
        g.persisted.known[1] == new_id());
  /* and forward again: both are known now, no verification */
  g.verifies = 0;
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_REVERT) == 0);
  pump_all();
  CHECK(g.verifies == 0 && flow.trial.confirmed == 1);
  /* a slot with another image than the known id is verified again */
  flow.known[0] = flow.known[0] ^ 1u;
  g.verifies = 0;
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_REVERT) == 0);
  pump_all();
  CHECK(g.verifies == 1 && flow.trial.confirmed == 0);
  /* a verified image that cannot be stored is not started */
  setup(&t, 0, 1, 3);
  g.persist_fail = true;
  upload();
  CHECK(R()->result == sensei_Gap9OtaResult_GAP9_OTA_FAILED && R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH &&
        !flow.verified && flow.known[1] == 0);
  g.persist_fail = false;
  CHECK(do_begin(image, img_n, true) == 0); /* the host comes back: GAP9 answers VERIFIED, node checks again */
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED && flow.verified && flow.known[1] == new_id());
}

/* Q3 (q_resume.c of the review): begin(resume) after a finished transfer keeps the verification, or
 * makes it again from the answer. */
static void test_resume_after_verified(void) {
  struct sensei_gap9_trial t = closed(0);

  setup(&t, 0, 1, 3);
  upload();
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED && flow.verified);
  g.verifies = 0;
  CHECK(do_begin(image, img_n, true) == 0);
  CHECK(R()->result == sensei_Gap9OtaResult_GAP9_OTA_OK && R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED &&
        flow.verified && g.verifies == 1);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0 &&
        R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_APPLYING);
  pump_all();
  CHECK(flow.trial.confirmed == 1);

  /* a restart of the nRF5340 after the finish (the GAP9 keeps the transfer, it was held on) */
  setup(&t, 0, 1, 3);
  upload();
  {
    struct g9ota_store st = {.trial = closed(0), .known = {0, 0}};

    g9ota_flow_init(&flow, cur_ops, &st);
  }
  CHECK(do_begin(image, img_n, true) == 0);
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED && flow.verified);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 1);

  /* GAP9 lost power after the finish: its record says the image is complete, begin answers the
   * end of the transfer, finish checks it again */
  setup(&t, 0, 1, 3);
  upload();
  session_gone();
  CHECK(do_begin(image, img_n, true) == 0);
  CHECK(R()->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING && R()->offset == img_n);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_FINISH) == 0 && flow.verified);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 1);

  /* a finished image whose signature does not check: the resume does not let it through */
  setup(&t, 0, 1, 3);
  upload();
  g.verify_rc = -EBADMSG;
  CHECK(do_begin(image, img_n, true) == 0);
  CHECK(R()->result == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
        R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIGNATURE && !flow.verified);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0 &&
        R()->result == sensei_Gap9OtaResult_GAP9_OTA_INVALID);
}

/* Q5: an older GAP9 firmware answers BAD_JOB: a clean refusal, not ten retries with the bus held.
 */
static void test_bad_job(void) {
  struct sensei_gap9_trial t = closed(0);

  setup(&t, 0, 1, 3);
  g.bad_job = true;
  make_new(1, 4, 20000);
  CHECK(do_begin(image, img_n, false) == 0);
  CHECK(last_err() == -ENOTSUP);
  CHECK(g.jobs == 1 && !g.held);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0 && last_err() == -ENOTSUP);
}

/* Q6: a build with a detector runs a detect job in the trial. */
static void test_trial_detect(void) {
  struct sensei_gap9_trial t = closed(0);

  with_detector = true;
  setup(&t, 0, 1, 3);
  upload();
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 1 && g.detects == 1);

  setup(&t, 0, 1, 3); /* the detector of the new image does not run: back on the old one */
  g.detect_fail = true;
  upload();
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 0 && !flow.trial.open && g.detects == 1 && g.slot_line == 0);
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_STATUS) == 0 && R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_BOOT);

  setup(&t, 0, 1, 3); /* an image without a detector (NOT_BUILT) is fine */
  g.detect_not_built = true;
  upload();
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 1);

  setup(&t, 0, 1, 3); /* the self-test fails first: no detect job */
  g.selftest_ok = false;
  upload();
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 0 && g.detects == 0);
  with_detector = false;
}

/* An app without checks of its own (no trial_check): the status job is enough, no self-test runs. */
static void test_no_trial_check(void) {
  struct sensei_gap9_trial t = closed(0);

  cur_ops = &ops_nocheck;
  setup(&t, 0, 1, 3);
  g.selftest_ok = false; /* would fail a self-test, but nothing asks for it */
  upload();
  g.jobs = 0;
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 1 && !flow.trial.open && g.jobs == 1); /* the status job only */
  cur_ops = &ops;
}

/* Q12: the LPWAN client has SPI_A when the trial wants to reboot GAP9: wait, do not revert. */
static void test_reboot_busy(void) {
  struct sensei_gap9_trial t = closed(0);

  setup(&t, 0, 1, 3);
  upload();
  g.reboot_busy = 3; /* the first boot is refused three times */
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(g.reboots == 0 && flow.trial.open && flow.boot_wait && g9ota_flow_needs_poll(&flow));
  for (int i = 0; i < 3; i++) {
    g9ota_flow_poll(&flow);
    pump_all();
  }
  CHECK(g.reboots == 1 && flow.trial.confirmed == 1 && !flow.trial.open && !flow.boot_wait);

  /* the link took the reboot and lost the bus before the boot: the callback says -EBUSY */
  setup(&t, 0, 1, 3);
  upload();
  g.reboot_busy_cb = 1;
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.open && flow.boot_wait && flow.trial.boots == 1); /* not counted as a boot */
  g9ota_flow_poll(&flow);
  pump_all();
  CHECK(flow.trial.confirmed == 1);

  /* SPI_A never comes back: after G9OTA_FLOW_BOOT_WAITS tries it counts as no HELLO */
  setup(&t, 0, 1, 3);
  upload();
  g.reboot_busy = 1000;
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  for (unsigned i = 0; i < 4 * G9OTA_FLOW_BOOT_WAITS && flow.trial.open; i++) {
    g9ota_flow_poll(&flow);
    pump_all();
  }
  g.reboot_busy = 0;
  CHECK(!flow.trial.open && flow.trial.confirmed == 0);
}

/* Q13: a confirm that cannot be stored is tried again by the poll. */
static void test_persist_retry(void) {
  struct sensei_gap9_trial t = closed(0);

  setup(&t, 0, 1, 3);
  upload();
  g.persist_skip = 1;  /* the trial is stored */
  g.persist_fails = 3; /* the three tries of the confirm fail */
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 1 && flow.dirty && g.persisted.trial.open); /* the old record stays */
  CHECK(g9ota_flow_needs_poll(&flow));
  g9ota_flow_poll(&flow);
  CHECK(!flow.dirty && g.persisted.trial.confirmed == 1 && !g.persisted.trial.open && !g9ota_flow_needs_poll(&flow));

  /* a store that works at the second try is not an error */
  setup(&t, 0, 1, 3);
  upload();
  g.persist_skip = 1;
  g.persist_fails = 1;
  CHECK(do_cmd(sensei_Gap9OtaAction_GAP9_OTA_ACTION_APPLY) == 0);
  pump_all();
  CHECK(flow.trial.confirmed == 1 && !flow.dirty && g.persisted.trial.confirmed == 1);
}

/* Q15: new transfers are rate limited on the node (GAP9 forgets its count when it loses power). */
static void test_node_rate_limit(void) {
  struct sensei_gap9_trial t = closed(0);

  setup(&t, 0, 1, 3);
  make_new(1, 4, 20000);
  g.clock_ms = 100000;
  CHECK(do_begin(image, img_n, false) == 0 && R()->result == sensei_Gap9OtaResult_GAP9_OTA_OK);
  session_gone();
  g.clock_ms += G9OTA_OPEN_MIN_MS - 1;
  CHECK(do_begin(image, img_n, false) == 0 && R()->result == sensei_Gap9OtaResult_GAP9_OTA_BUSY &&
        R()->fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_RATE);
  CHECK(!g.held); /* refused before GAP9 is held */
  CHECK(do_begin(image, img_n, true) == 0 && R()->result == sensei_Gap9OtaResult_GAP9_OTA_OK); /* a resume is not new */
  g.clock_ms += 2;
  CHECK(do_begin(image, img_n, false) == 0 && R()->result == sensei_Gap9OtaResult_GAP9_OTA_OK);
}

int main(void) {
  test_update_and_trial(0);
  test_update_and_trial(1);
  test_trial_failures();
  test_one_lost_hello();
  test_signature_and_version();
  test_nrf_reset_during_trial();
  test_lost_settings();
  test_session_rules();
  test_revert();
  test_not_built();
  test_during_trial();
  test_persist_failure();
  test_unverified_slot();
  test_known_ids();
  test_resume_after_verified();
  test_bad_job();
  test_trial_detect();
  test_no_trial_check();
  test_reboot_busy();
  test_persist_retry();
  test_node_rate_limit();
  DONE();
}
