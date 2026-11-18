/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The recovery job server (src/g9rec.c) on a RAM "MRAM": both slots bad, the keep slot is the one on the slot line.
 * A transfer through encoded Gap9RecoveryJob frames into the other slot: finish, seal, the keep slot and the boot area
 * never written, a repeated seq answered from a copy, a job without a body refused, a power cut at every write
 * (the seal included) and a resume. */

#include <stdio.h>

#include "check.h"
#include "g9ota_fake.h"
#include "sensei_gap9/g9rec.h"

static struct fake_mram mram;
static struct g9ota ota;
static struct g9rec rec;
static uint8_t image[SENSEI_G9_SLOT_SIZE];
static uint32_t seq;

#define BOOT_AREA SENSEI_G9_CKPT_OFF /* everything before the checkpoint sectors is the boot area */

static void fresh(void) {
  memset(&mram, 0xFF, sizeof(mram.mem));
  mram.writes = 0;
  mram.cut_at = -1;
  mram.dead = false;
  mram.reprogram = mram.bad_align = 0;
  for (uint32_t i = 0; i < BOOT_AREA; i++) { /* FSBL, SSBL, volume table */
    mram.mem[i] = (uint8_t)(i * 31u + 7u);
  }
}

static int raw_read(void *c, uint32_t off, void *buf, size_t len) {
  memcpy(buf, (const uint8_t *)c + off, len);
  return 0;
}

static bool slot_parses(uint8_t slot) {
  struct sensei_g9_slot_info si;

  return sensei_g9_slot_parse(raw_read, mram.mem + (slot ? SENSEI_G9_SLOT_B_OFF : SENSEI_G9_SLOT_A_OFF),
                              SENSEI_G9_SLOT_SIZE, &si) == 0;
}

/* The SSBL after a power-up: the slot line says `keep`, the writer keeps it. */
static void ssbl_start(uint8_t keep) {
  struct g9ota_cfg l = fake_layout();

  mram.dead = false;
  mram.cut_at = -1;
  CHECK(g9ota_init(&ota, &fake_ops, &mram, &l, keep) == 0);
  g9rec_init(&rec, &ota);
  seq = 0;
}

/* One job through the wire: encode, decode (as g9l_fetch does), run, encode, decode the answer. */
static int wire(const sensei_Gap9RecoveryJob *j, sensei_Gap9RecoveryResult *out) {
  uint8_t buf[sensei_Gap9RecoveryJob_size + 16], rb[sensei_Gap9RecoveryResult_size + 16];
  sensei_Gap9RecoveryJob dj;
  sensei_Gap9RecoveryResult res;
  int32_t n = sensei_encode_Gap9RecoveryJob(buf, sizeof(buf), j);
  int rc;

  CHECK(n > 0);
  CHECK(g9rec_decode(buf, (size_t)n, &dj));
  rc = g9rec_run(&rec, &dj, 1000u + (uint32_t)mram.writes, &res);
  n = g9rec_encode(&res, rb, sizeof(rb));
  CHECK(n > 0);
  CHECK(sensei_decode_Gap9RecoveryResult(rb, (size_t)n, out));
  CHECK(out->seq == j->seq);
  return rc;
}

static sensei_Gap9RecoveryJob job(pb_size_t tag) {
  sensei_Gap9RecoveryJob j = sensei_Gap9RecoveryJob_init_zero;

  j.seq = ++seq;
  j.which_body = tag;
  return j;
}

static int do_begin(const uint8_t *img, size_t n, bool resume, sensei_Gap9RecoveryResult *r) {
  sensei_Gap9RecoveryJob j = job(sensei_Gap9RecoveryJob_begin_tag);

  j.begin.size = (uint32_t)n;
  file_sha(img, n, j.begin.sha256);
  j.begin.resume = resume;
  return wire(&j, r);
}

static int do_chunk(const uint8_t *img, uint32_t off, size_t n, sensei_Gap9RecoveryResult *r) {
  sensei_Gap9RecoveryJob j = job(sensei_Gap9RecoveryJob_chunk_tag);

  j.chunk.offset = off;
  j.chunk.data.size = (pb_size_t)n;
  memcpy(j.chunk.data.bytes, img + off, n);
  return wire(&j, r);
}

static int do_send(const uint8_t *img, size_t n, uint32_t from, sensei_Gap9RecoveryResult *r) {
  for (uint32_t off = from; off < n;) {
    size_t m = n - off < G9OTA_CHUNK_MAX ? n - off : G9OTA_CHUNK_MAX;
    int rc = do_chunk(img, off, m, r);

    if (rc != 0 || r->info.result != sensei_Gap9OtaResult_GAP9_OTA_OK) {
      return -1;
    }
    off = r->info.offset > off ? r->info.offset : off + (uint32_t)m;
  }
  return 0;
}

static int do_simple(pb_size_t tag, sensei_Gap9RecoveryResult *r) {
  sensei_Gap9RecoveryJob j = job(tag);

  return wire(&j, r);
}

static long seal_jobs; /* seal jobs sent so far (the power-cut runner counts them) */

static int do_seal(const uint8_t hash[32], sensei_Gap9RecoveryResult *r) {
  sensei_Gap9RecoveryJob j = job(sensei_Gap9RecoveryJob_seal_tag);

  seal_jobs++;
  memcpy(j.seal.image_hash, hash, 32);
  return wire(&j, r);
}

static const uint8_t *slot_mem(uint8_t s) { return mram.mem + (s ? SENSEI_G9_SLOT_B_OFF : SENSEI_G9_SLOT_A_OFF); }

/* Both slots hold damaged images (slot A is the one on the slot line: keep). Bytes stay: a keep slot is never erased.
 */
static void both_bad(void) {
  struct img_opt o = {.payload = 20000, .major = 1, .minor = 2, .kind = FAKE_KIND, .sign = true, .seed = 3};
  static uint8_t tmp[SENSEI_G9_SLOT_SIZE];
  size_t n = build_image(tmp, &o);

  fresh();
  memcpy(mram.mem + SENSEI_G9_SLOT_A_OFF, tmp, n);
  memcpy(mram.mem + SENSEI_G9_SLOT_B_OFF, tmp, n);
  mram.mem[SENSEI_G9_SLOT_A_OFF + 3] ^= 0x80; /* no magic */
  mram.mem[SENSEI_G9_SLOT_B_OFF + 3] ^= 0x40;
}

static void test_transfer(uint8_t keep) {
  struct img_opt o = {.payload = 60000, .major = 1, .minor = 3, .kind = FAKE_KIND, .sign = true, .seed = 17};
  size_t n = build_image(image, &o);
  uint8_t tgt = 1 - keep, boot_copy[BOOT_AREA], keep_copy[SENSEI_G9_SLOT_SIZE];
  sensei_Gap9RecoveryResult r, fin;

  both_bad();
  if (keep == 1) { /* the other way round */
    static uint8_t t[SENSEI_G9_SLOT_SIZE];

    memcpy(t, slot_mem(0), SENSEI_G9_SLOT_SIZE);
    memcpy(mram.mem + SENSEI_G9_SLOT_A_OFF, slot_mem(1), SENSEI_G9_SLOT_SIZE);
    memcpy(mram.mem + SENSEI_G9_SLOT_B_OFF, t, SENSEI_G9_SLOT_SIZE);
  }
  memcpy(boot_copy, mram.mem, BOOT_AREA);
  memcpy(keep_copy, slot_mem(keep), SENSEI_G9_SLOT_SIZE);
  CHECK(!slot_parses(0) && !slot_parses(1));
  ssbl_start(keep);

  CHECK(do_simple(sensei_Gap9RecoveryJob_status_tag, &r) == 0 &&
        r.status == sensei_Gap9RecoveryStatus_GAP9_RECOVERY_OK);
  CHECK(r.info.booted_slot == keep && r.info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_IDLE);
  CHECK(!r.info.slot_a.valid && !r.info.slot_b.valid);
  CHECK(do_begin(image, n, false, &r) == 0 && r.info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING);
  CHECK(do_send(image, n, 0, &r) == 0 && r.info.offset == n);
  CHECK(do_simple(sensei_Gap9RecoveryJob_finish_tag, &fin) == 0);
  CHECK(fin.info.result == sensei_Gap9OtaResult_GAP9_OTA_OK &&
        fin.info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED);
  CHECK(fin.info.image_hash.size == 32 && fin.info.signature.size == 64 && fin.info.has_version &&
        fin.info.version.major == 1 && fin.info.version.minor == 3);
  CHECK(!slot_parses(tgt) && !fin.info.sealed); /* checked, not sealed: not bootable */
  {
    uint8_t wrong[32];

    memcpy(wrong, fin.info.image_hash.bytes, 32);
    wrong[0] ^= 1;
    CHECK(do_seal(wrong, &r) == 0 && r.info.result == sensei_Gap9OtaResult_GAP9_OTA_INVALID && !slot_parses(tgt));
  }
  CHECK(do_seal(fin.info.image_hash.bytes, &r) == 0 && r.info.sealed && slot_parses(tgt));
  CHECK(memcmp(slot_mem(tgt), image, n) == 0);
  CHECK(memcmp(slot_mem(keep), keep_copy, sizeof(keep_copy)) == 0); /* the keep slot is never written */
  CHECK(memcmp(mram.mem, boot_copy, BOOT_AREA) == 0);               /* nor the boot area */
  CHECK(ota.writes_outside == 0 && mram.reprogram == 0 && mram.bad_align == 0);
  CHECK(!g9ota_addr_ok(&ota, SENSEI_G9_SLOT_A_OFF + (keep ? SENSEI_G9_SLOT_B_OFF - SENSEI_G9_SLOT_A_OFF : 0), 16));
}

static void test_wire_rules(void) {
  struct img_opt o = {.payload = 30000, .major = 1, .minor = 1, .kind = FAKE_KIND, .sign = true, .seed = 5};
  size_t n = build_image(image, &o);
  sensei_Gap9RecoveryResult r, r2;
  sensei_Gap9RecoveryJob j;
  long w;

  both_bad();
  ssbl_start(0);
  /* a job without a body */
  j = job(0);
  CHECK(wire(&j, &r) == -EINVAL && r.status == sensei_Gap9RecoveryStatus_GAP9_RECOVERY_BAD_JOB);
  /* a status job with a slot that does not exist */
  j = job(sensei_Gap9RecoveryJob_status_tag);
  j.status.verify = true;
  j.status.slot = 5;
  CHECK(wire(&j, &r) == -EINVAL && r.status == sensei_Gap9RecoveryStatus_GAP9_RECOVERY_BAD_JOB);
  /* garbage is no job */
  {
    uint8_t junk[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    sensei_Gap9RecoveryJob dj;

    CHECK(!g9rec_decode(junk, sizeof(junk), &dj));
  }
  CHECK(do_begin(image, n, false, &r) == 0);
  /* a repeated seq is answered again and not run again: the same chunk with other data changes nothing */
  j = job(sensei_Gap9RecoveryJob_chunk_tag);
  j.chunk.offset = 0;
  j.chunk.data.size = 64;
  memset(j.chunk.data.bytes, 0x11, 64);
  CHECK(wire(&j, &r) == 0 && r.info.offset == 64);
  w = mram.writes;
  memset(j.chunk.data.bytes, 0x22, 64);
  CHECK(wire(&j, &r2) == 0 && r2.info.offset == 64 && mram.writes == w);
  /* a new seq with the same offset is a repeat of the chunk for the writer: nothing moves either */
  j.seq = ++seq;
  CHECK(wire(&j, &r2) == 0 && r2.info.offset == 64);
}

/* The state a restart finds: the SSBL runs again, the nRF5340 resumes. */
static bool cut_run(const uint8_t *img, size_t n, uint8_t keep, long cut, long cut2, unsigned seedv,
                    const uint8_t *boot_copy, const uint8_t *keep_copy) {
  sensei_Gap9RecoveryResult r;
  uint8_t tgt = 1 - keep;
  bool resume = false, ok = true;
  int cuts_left = cut2 >= 0 ? 2 : 1;

  seal_jobs = 0;
  mram.writes = 0;
  mram.cut_at = cut;
  mram.cut_seed = seedv;
  ssbl_start(keep);
  mram.cut_at = cut;
  for (int guard = 0; guard < 8; guard++) {
    int rc = do_begin(img, n, resume, &r);
    bool done = false;

    if (!mram.dead && r.info.result == sensei_Gap9OtaResult_GAP9_OTA_OK) {
      if (r.info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING) {
        rc = do_send(img, n, r.info.offset, &r);
        if (rc == 0 && !mram.dead) {
          rc = do_simple(sensei_Gap9RecoveryJob_finish_tag, &r);
        }
      }
      if (!mram.dead && r.info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED && r.info.result == 0) {
        uint8_t h[32];

        memcpy(h, r.info.image_hash.bytes, 32);
        rc = do_seal(h, &r);
        done = !mram.dead && r.info.sealed;
      }
    } else if (!mram.dead && r.info.result == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
               r.info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_HASH && resume) {
      resume = false; /* the cut hit the rewrite of the magic sector: the host sends the image again from 0 */
      continue;
    }
    (void)rc;
    if (done) {
      break;
    }
    ok &= mram.dead;
    /* power gone: what the SSBL would see is the new image whole, or no slot at the target */
    if (slot_parses(tgt)) {
      ok &= memcmp(slot_mem(tgt), img, n) == 0 && seal_jobs > 0; /* the magic comes from a seal job and nothing else */
    }
    ok &= memcmp(slot_mem(keep), keep_copy, SENSEI_G9_SLOT_SIZE) == 0 && memcmp(mram.mem, boot_copy, BOOT_AREA) == 0;
    cuts_left--;
    ssbl_start(keep);
    if (cuts_left == 1 && cut2 >= 0) {
      mram.cut_at = mram.writes + cut2;
      mram.cut_seed = seedv + 1;
    }
    resume = true;
  }
  ok &= !mram.dead && slot_parses(tgt) && memcmp(slot_mem(tgt), img, n) == 0 && seal_jobs > 0;
  ok &= memcmp(slot_mem(keep), keep_copy, SENSEI_G9_SLOT_SIZE) == 0 && memcmp(mram.mem, boot_copy, BOOT_AREA) == 0;
  ok &= ota.writes_outside == 0 && mram.reprogram == 0 && mram.bad_align == 0;
  return ok;
}

static void test_power_cuts(uint8_t keep) {
  struct img_opt o = {
      .payload = 2 * G9OTA_SECTOR + 777, .major = 1, .minor = 3, .kind = FAKE_KIND, .sign = true, .seed = 23};
  size_t n = build_image(image, &o);
  static uint8_t snapshot[MRAM_SIZE], boot_copy[BOOT_AREA], keep_copy[SENSEI_G9_SLOT_SIZE];
  long total, cuts = 0;

  both_bad();
  if (keep == 1) {
    static uint8_t t[SENSEI_G9_SLOT_SIZE];

    memcpy(t, slot_mem(0), SENSEI_G9_SLOT_SIZE);
    memcpy(mram.mem + SENSEI_G9_SLOT_A_OFF, slot_mem(1), SENSEI_G9_SLOT_SIZE);
    memcpy(mram.mem + SENSEI_G9_SLOT_B_OFF, t, SENSEI_G9_SLOT_SIZE);
  }
  memcpy(snapshot, mram.mem, MRAM_SIZE);
  memcpy(boot_copy, mram.mem, BOOT_AREA);
  memcpy(keep_copy, slot_mem(keep), SENSEI_G9_SLOT_SIZE);
  CHECK(cut_run(image, n, keep, -1, -1, 0, boot_copy, keep_copy));
  total = mram.writes;
  for (long cut = 0; cut < total; cut++) {
    for (unsigned s = 0; s < 2; s++) {
      bool ok;

      memcpy(mram.mem, snapshot, MRAM_SIZE);
      ok = cut_run(image, n, keep, cut, -1, s * 977 + 13, boot_copy, keep_copy);
      if (!ok) {
        printf("recovery: power cut at write %ld seed %u: wrong result\n", cut, s);
      }
      CHECK(ok);
      if (!ok) {
        return;
      }
      cuts++;
    }
  }
  for (long cut = 0; cut < total; cut += (total / 11) + 1) {
    for (long second = 0; second < 7; second++) {
      memcpy(mram.mem, snapshot, MRAM_SIZE);
      CHECK(cut_run(image, n, keep, cut, second, (unsigned)(cut * 31 + second), boot_copy, keep_copy));
      cuts++;
    }
  }
  printf("  recovery, keep %u: %ld writes per transfer, %ld cuts tested\n", keep, total, cuts);
}

int main(void) {
  test_transfer(0);
  test_transfer(1);
  test_wire_rules();
  test_power_cuts(0);
  test_power_cuts(1);
  DONE();
}
