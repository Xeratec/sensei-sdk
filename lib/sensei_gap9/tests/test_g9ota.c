/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The GAP9 update writer (src/g9ota.c) on a RAM "MRAM": transfer, resume, a power cut at
 * every write (also a second cut after the first), the slot that runs is never written, the checks
 * of `finish`, the checkpoint log across its two sectors, the address guard. With an argument the
 * program feeds that signed image file through the writer instead (the cross-check with imgtool,
 * see GAP9/ssbl/tools/test_pack_slot.py). Arguments: image, booted slot, kind. */

#include <stdio.h>

#include "check.h"
#include "g9ota_fake.h"

static struct fake_mram mram;
static struct g9ota ota;
static uint8_t image[SENSEI_G9_SLOT_SIZE];
static uint8_t booted_copy[SENSEI_G9_SLOT_SIZE];
static uint8_t pre_copy[SENSEI_G9_SLOT_SIZE];

static const uint8_t *slot_mem(uint8_t slot) { return mram.mem + (slot ? SENSEI_G9_SLOT_B_OFF : SENSEI_G9_SLOT_A_OFF); }

static void fresh_mram(void) {
  memset(&mram, 0xFF, sizeof(mram.mem));
  mram.writes = 0;
  mram.cut_at = -1;
  mram.dead = false;
  mram.reprogram = mram.bad_align = 0;
}

static int raw_read(void *c, uint32_t off, void *buf, size_t len) {
  memcpy(buf, (const uint8_t *)c + off, len);
  return 0;
}

/* The slot parses as an image (what the SSBL checks first). */
static bool slot_parses(uint8_t slot) {
  struct sensei_g9_slot_info si;

  return sensei_g9_slot_parse(raw_read, (void *)slot_mem(slot), SENSEI_G9_SLOT_SIZE, &si) == 0;
}

static const char *file_kind; /* the kind of an image file given on the command line */

static void boot(uint8_t booted) {
  struct g9ota_cfg l = fake_layout();

  if (file_kind != NULL) {
    l.kind = file_kind;
  }
  g9ota_init(&ota, &fake_ops, &mram, &l, booted);
}

/* Put a valid image into the slot that "runs" (so that the downgrade check has something to compare
 * with). */
static size_t install_running(uint8_t slot, uint8_t major, uint8_t minor, uint16_t rev) {
  static uint8_t tmp[SENSEI_G9_SLOT_SIZE];
  struct img_opt o = {
      .payload = 4096, .major = major, .minor = minor, .revision = rev, .kind = FAKE_KIND, .sign = true, .seed = 7};
  size_t n = build_image(tmp, &o);

  memcpy(mram.mem + (slot ? SENSEI_G9_SLOT_B_OFF : SENSEI_G9_SLOT_A_OFF), tmp, n);
  return n;
}

static sensei_Gap9OtaBegin begin_req(const uint8_t *img, size_t n, bool resume) {
  sensei_Gap9OtaBegin b = sensei_Gap9OtaBegin_init_zero;

  b.size = (uint32_t)n;
  file_sha(img, n, b.sha256);
  b.resume = resume;
  return b;
}

static sensei_Gap9OtaResult send_chunk(const uint8_t *img, uint32_t off, size_t len, sensei_Gap9OtaInfo *info) {
  sensei_Gap9OtaData d = sensei_Gap9OtaData_init_zero;

  d.offset = off;
  d.data.size = (pb_size_t)len;
  memcpy(d.data.bytes, img + off, len);
  return g9ota_chunk(&ota, &d, info);
}

/* Send img[from, n) in chunks of `step`; stops at the first answer that is not OK. */
static sensei_Gap9OtaResult send_from(const uint8_t *img, size_t n, uint32_t from, size_t step,
                                      sensei_Gap9OtaInfo *info) {
  sensei_Gap9OtaResult r = sensei_Gap9OtaResult_GAP9_OTA_OK;

  for (uint32_t off = from; off < n && r == sensei_Gap9OtaResult_GAP9_OTA_OK;) {
    size_t m = n - off < step ? n - off : step;

    r = send_chunk(img, off, m, info);
    off = info->offset > off ? info->offset : off + (uint32_t)m;
  }
  return r;
}

/* ---- pure helpers ---- */

static void test_helpers(void) {
  uint8_t raw[64], hdr[32] = {0};
  uint32_t v;
  /* r = 0x80... (needs a zero byte), s short */
  uint8_t der[] = {0x30, 0x26, 0x02, 0x21, 0x00, 0x80, 1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,   13,   14,
                   15,   16,   17,   18,   19,   20,   21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 0x02, 0x01, 0x05};

  CHECK(g9ota_sig_der_to_raw(der, sizeof(der), raw));
  CHECK(raw[0] == 0x80 && raw[1] == 1 && raw[31] == 31 && raw[63] == 5 && raw[32] == 0 && raw[62] == 0);
  der[1] = 0x27; /* length wrong */
  CHECK(!g9ota_sig_der_to_raw(der, sizeof(der), raw));
  der[1] = 0x26;
  der[2] = 0x03;
  CHECK(!g9ota_sig_der_to_raw(der, sizeof(der), raw));
  CHECK(!g9ota_sig_der_to_raw(der, 5, raw));
  {
    uint8_t neg[] = {0x30, 0x08, 0x02, 0x01, 0x80, 0x02, 0x01, 0x01, 0, 0}; /* negative r */
    CHECK(!g9ota_sig_der_to_raw(neg, 8, raw));
  }
  {
    uint8_t big[2 + 2 + 33 + 2 + 1];
    memset(big, 0x11, sizeof(big));
    big[0] = 0x30;
    big[1] = sizeof(big) - 2;
    big[2] = 0x02;
    big[3] = 33;
    big[4] = 0x11; /* 33 bytes without a leading zero: too big for P-256 */
    big[2 + 2 + 33] = 0x02;
    big[2 + 2 + 33 + 1] = 1;
    CHECK(!g9ota_sig_der_to_raw(big, sizeof(big), raw));
  }
  CHECK(!g9ota_header_version(hdr, &v));
  put32(hdr, SENSEI_G9_MCUBOOT_MAGIC);
  hdr[20] = 1;
  hdr[21] = 2;
  put16(hdr + 22, 3);
  CHECK(g9ota_header_version(hdr, &v) && v == ((1u << 24) | (2u << 16) | 3u));

  struct g9ota_ckpt c = {.valid = true, .state = G9OTA_CK_OPEN, .target = 1, .seq = 9, .size = 1234, .offset = 8192};
  uint8_t rec[G9OTA_CKPT_LEN];
  struct g9ota_ckpt d;

  memset(c.sha, 0x33, 32);
  g9ota_ckpt_encode(&c, rec);
  CHECK(g9ota_ckpt_decode(rec, &d) && d.seq == 9 && d.size == 1234 && d.offset == 8192 && d.target == 1 &&
        d.sha[5] == 0x33);
  rec[20] ^= 1;
  CHECK(!g9ota_ckpt_decode(rec, &d));
  memset(rec, 0xFF, sizeof(rec));
  CHECK(!g9ota_ckpt_decode(rec, &d));
}

/* ---- address guard ---- */

static void test_guard(void) {
  fresh_mram();
  boot(0);                                                         /* runs from A: the target is B */
  CHECK(!g9ota_addr_ok(&ota, 0, G9OTA_SECTOR));                    /* boot area */
  CHECK(!g9ota_addr_ok(&ota, SENSEI_G9_SLOT_A_OFF, G9OTA_SECTOR)); /* the slot that runs */
  CHECK(!g9ota_addr_ok(&ota, SENSEI_G9_SLOT_A_OFF + SENSEI_G9_SLOT_SIZE - 16, 16));
  CHECK(g9ota_addr_ok(&ota, SENSEI_G9_SLOT_B_OFF, G9OTA_SECTOR));
  CHECK(g9ota_addr_ok(&ota, SENSEI_G9_SLOT_B_OFF + SENSEI_G9_SLOT_SIZE - 16, 16));
  CHECK(!g9ota_addr_ok(&ota, SENSEI_G9_SLOT_B_OFF + SENSEI_G9_SLOT_SIZE - 16, 32)); /* over the end */
  CHECK(!g9ota_addr_ok(&ota, SENSEI_G9_SLOT_B_OFF - 16, 32));                       /* over the start */
  CHECK(g9ota_addr_ok(&ota, SENSEI_G9_CKPT_OFF, G9OTA_SECTOR));
  CHECK(g9ota_addr_ok(&ota, SENSEI_G9_CKPT_OFF + G9OTA_SECTOR, G9OTA_SECTOR));
  CHECK(!g9ota_addr_ok(&ota, SENSEI_G9_CKPT_OFF + 2 * G9OTA_SECTOR, 16)); /* after the checkpoint area */
  CHECK(!g9ota_addr_ok(&ota, SENSEI_G9_CKPT_OFF + G9OTA_SECTOR, G9OTA_SECTOR + 16));
  CHECK(!g9ota_addr_ok(&ota, 0, 0));
  CHECK(!g9ota_addr_ok(&ota, 0xFFFFFFF0u, 64)); /* wraps */
  boot(1);
  CHECK(g9ota_addr_ok(&ota, SENSEI_G9_SLOT_A_OFF, G9OTA_SECTOR));
  CHECK(!g9ota_addr_ok(&ota, SENSEI_G9_SLOT_B_OFF, G9OTA_SECTOR));

  /* A damaged state (target = the running slot) writes nothing: the guard does not trust `target`
   * alone. */
  struct img_opt o = {.payload = 20000, .major = 1, .kind = FAKE_KIND, .sign = true, .seed = 3};
  size_t n = build_image(image, &o);
  sensei_Gap9OtaInfo info;
  sensei_Gap9OtaBegin b = begin_req(image, n, false);

  fresh_mram();
  boot(0);
  ota.target = 0;
  CHECK(g9ota_begin(&ota, &b, &info) ==
        sensei_Gap9OtaResult_GAP9_OTA_FAILED); /* the erase of the header sector is refused */
  CHECK(info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH && info.detail == -EACCES);
  CHECK(ota.writes_outside > 0);
  {
    static const uint8_t blank[64] = {[0 ... 63] = 0xFF};
    CHECK(memcmp(slot_mem(0), blank, sizeof(blank)) == 0);
  }
}

/* ---- a transfer without a cut ---- */

static void test_transfer(uint8_t booted, size_t step) {
  struct img_opt o = {
      .payload = 100000, .major = 1, .minor = 2, .revision = 3, .kind = FAKE_KIND, .sign = true, .seed = 11};
  size_t n = build_image(image, &o);
  sensei_Gap9OtaInfo info;
  sensei_Gap9OtaBegin b = begin_req(image, n, false);
  uint8_t tgt = 1 - booted;

  fresh_mram();
  boot(booted);
  install_running(booted, 1, 2, 0);
  memcpy(booted_copy, slot_mem(booted), SENSEI_G9_SLOT_SIZE);

  CHECK(g9ota_status(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(info.booted_slot == booted && info.chunk_max == G9OTA_CHUNK_MAX && info.slot_size == SENSEI_G9_SLOT_SIZE);
  CHECK((booted ? info.slot_b.valid : info.slot_a.valid) && !(booted ? info.slot_a.valid : info.slot_b.valid));
  CHECK((booted ? info.slot_b : info.slot_a).version.minor == 2 && (booted ? info.slot_b : info.slot_a).image_id != 0);

  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING && info.offset == 0 && info.size == n);
  CHECK(send_from(image, n, 0, step, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(info.offset == n);
  CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED);
  CHECK(info.image_hash.size == 32 && info.signature.size == 64 && info.key_hash.size == 32);
  CHECK(info.signature.bytes[0] == 0x20 && info.signature.bytes[32] == 0x40 && info.key_hash.bytes[0] == 0x11);
  CHECK(memcmp(slot_mem(tgt), image, n) == 0);
  CHECK(memcmp(slot_mem(booted), booted_copy, SENSEI_G9_SLOT_SIZE) == 0); /* the slot that runs: not touched */
  CHECK((booted ? info.slot_a : info.slot_b).valid);                      /* done: the slot is an image */
  CHECK(ota.writes_outside == 0 && mram.reprogram == 0 && mram.bad_align == 0);

  /* a repeated finish and a repeated begin(resume) are answered, nothing changes */
  CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK &&
        info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED);
  b.resume = true;
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK &&
        info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED);
  /* a chunk after the end is refused */
  CHECK(send_chunk(image, 0, 16, &info) == sensei_Gap9OtaResult_GAP9_OTA_INVALID);
  CHECK(info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
}

/* ---- request rules ---- */

static void test_rules(void) {
  struct img_opt o = {.payload = 30000, .major = 1, .minor = 0, .kind = FAKE_KIND, .sign = true, .seed = 5};
  size_t n = build_image(image, &o);
  sensei_Gap9OtaInfo info;
  sensei_Gap9OtaBegin b = begin_req(image, n, false);

  fresh_mram();
  boot(0);
  CHECK(send_chunk(image, 0, 16, &info) == sensei_Gap9OtaResult_GAP9_OTA_INVALID &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
  CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_INVALID &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
  b.size = 0;
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_INVALID &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIZE);
  b.size = SENSEI_G9_SLOT_SIZE + 1;
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_INVALID &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIZE);
  b = begin_req(image, n, false);
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(mram.writes > 0 && ota.writes_outside == 0);
  /* offset above the wanted one: refused, nothing moves */
  CHECK(send_chunk(image, 100, 16, &info) == sensei_Gap9OtaResult_GAP9_OTA_INVALID &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_OFFSET);
  CHECK(info.offset == 0);
  /* too long, empty, past the end */
  {
    sensei_Gap9OtaData d = sensei_Gap9OtaData_init_zero;

    d.offset = 0;
    d.data.size = 0;
    CHECK(g9ota_chunk(&ota, &d, &info) == sensei_Gap9OtaResult_GAP9_OTA_INVALID &&
          info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIZE);
    d.offset = (uint32_t)n;
    d.data.size = 16;
    CHECK(g9ota_chunk(&ota, &d, &info) == sensei_Gap9OtaResult_GAP9_OTA_INVALID &&
          info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIZE);
  }
  /* chunk, repeat of it, overlapping chunk */
  CHECK(send_chunk(image, 0, 1000, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK && info.offset == 1000);
  CHECK(send_chunk(image, 0, 1000, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK && info.offset == 1000);
  CHECK(send_chunk(image, 500, 1000, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK && info.offset == 1500);
  /* finish too early */
  CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_INVALID &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_OFFSET);
  CHECK(ota.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING);
  /* resume of the open transfer answers its offset */
  b.resume = true;
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK && info.offset == 1500);
  CHECK(send_from(image, n, 1500, 188, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(mram.reprogram == 0);

  /* abort drops it: the slot is not an image any more, the next begin starts at 0 */
  CHECK(g9ota_abort(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK &&
        info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_IDLE);
  CHECK(!info.slot_b.valid);
  b.resume = true;
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK && info.offset == 0);

  /* a fresh begin soon after another one waits (the rate rule), a resume does not */
  boot(0);
  g9ota_touch(&ota, 1000);
  b.resume = false;
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  g9ota_touch(&ota, 2000);
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_BUSY &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_RATE);
  b.resume = true;
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  b.resume = false;
  g9ota_touch(&ota, 2000 + G9OTA_OPEN_MIN_MS);
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
}

/* ---- what finish refuses ---- */

static sensei_Gap9OtaResult run_all(const struct img_opt *o, sensei_Gap9OtaInfo *info, bool corrupt_after) {
  size_t n = build_image(image, o);
  sensei_Gap9OtaBegin b = begin_req(image, n, false);

  boot(0);
  if (g9ota_begin(&ota, &b, info) != sensei_Gap9OtaResult_GAP9_OTA_OK) {
    return sensei_Gap9OtaResult_GAP9_OTA_INVALID;
  }
  if (send_from(image, n, 0, 1024, info) != sensei_Gap9OtaResult_GAP9_OTA_OK) {
    return sensei_Gap9OtaResult_GAP9_OTA_INVALID;
  }
  if (corrupt_after) {
    mram.mem[SENSEI_G9_SLOT_B_OFF + 5000] ^= 0x01; /* a bit that MRAM did not keep */
  }
  return g9ota_finish(&ota, info);
}

static void test_finish_checks(void) {
  sensei_Gap9OtaInfo info;
  struct img_opt ok = {
      .payload = 40000, .major = 1, .minor = 5, .revision = 0, .kind = FAKE_KIND, .sign = true, .seed = 9};
  struct img_opt o;

  fresh_mram();
  install_running(0, 1, 5, 0);
  CHECK(run_all(&ok, &info, false) == sensei_Gap9OtaResult_GAP9_OTA_OK); /* the same version is allowed */

  fresh_mram();
  install_running(0, 1, 5, 0);
  CHECK(run_all(&ok, &info, true) == sensei_Gap9OtaResult_GAP9_OTA_FAILED);
  CHECK(info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_HASH && info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED);
  CHECK(!info.slot_b.valid && info.slot_b.image_id == 0);
  /* the checkpoint is gone: a resume starts at 0 */
  sensei_Gap9OtaBegin b = begin_req(image, 0x10000, true);
  boot(0);
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK && info.offset == 0);

  o = ok;
  o.minor = 4; /* older than the one that runs */
  fresh_mram();
  install_running(0, 1, 5, 0);
  CHECK(run_all(&o, &info, false) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_DOWNGRADE);
  CHECK(!info.slot_b.valid);

  o = ok;
  o.revision = 1; /* newer */
  fresh_mram();
  install_running(0, 1, 5, 0);
  CHECK(run_all(&o, &info, false) == sensei_Gap9OtaResult_GAP9_OTA_OK);

  o = ok;
  o.kind = "sensei-ieq-nrf5340";
  fresh_mram();
  CHECK(run_all(&o, &info, false) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_KIND);
  o.kind = NULL;
  fresh_mram();
  CHECK(run_all(&o, &info, false) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_KIND);

  o = ok;
  o.sign = false;
  fresh_mram();
  CHECK(run_all(&o, &info, false) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIGNATURE);

  o = ok;
  o.bad_digest = true;
  fresh_mram();
  CHECK(run_all(&o, &info, false) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE);

  /* not an MCUboot image at all (the hash is right: it is the file of `begin`) */
  {
    static uint8_t junk[20000];
    sensei_Gap9OtaBegin jb;

    for (size_t i = 0; i < sizeof(junk); i++) {
      junk[i] = (uint8_t)(i * 7);
    }
    jb = begin_req(junk, sizeof(junk), false);
    fresh_mram();
    boot(0);
    CHECK(g9ota_begin(&ota, &jb, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    CHECK(send_from(junk, sizeof(junk), 0, 1024, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
          info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE);
  }
  /* a file with bytes after the TLV area: the container sizes do not add up */
  {
    static uint8_t big[100000];
    size_t n = build_image(big, &ok);
    sensei_Gap9OtaBegin bb;

    memset(big + n, 0xAA, 64);
    bb = begin_req(big, n + 64, false);
    fresh_mram();
    boot(0);
    CHECK(g9ota_begin(&ota, &bb, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    CHECK(send_from(big, n + 64, 0, 1024, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
          info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE);
  }
  /* the hash of `begin` is not the file: the host sent other bytes */
  {
    size_t n = build_image(image, &ok);
    sensei_Gap9OtaBegin bb = begin_req(image, n, false);

    bb.sha256[3] ^= 1;
    fresh_mram();
    boot(0);
    CHECK(g9ota_begin(&ota, &bb, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    CHECK(send_from(image, n, 0, 1024, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
          info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_HASH);
  }
  /* a flash that fails: the transfer ends FLASH and the checkpoint stays, so a resume can go on
   */
  {
    size_t n = build_image(image, &ok);
    sensei_Gap9OtaBegin bb = begin_req(image, n, false);

    fresh_mram();
    boot(0);
    CHECK(g9ota_begin(&ota, &bb, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    mram.cut_at = mram.writes + 5; /* power goes in the middle */
    CHECK(send_from(image, n, 0, 1024, &info) == sensei_Gap9OtaResult_GAP9_OTA_FAILED);
    CHECK(info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH &&
          info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED);
    CHECK(send_chunk(image, info.offset, 100, &info) == sensei_Gap9OtaResult_GAP9_OTA_INVALID);
  }
}

/* ---- Q4: a slot that is not complete never parses ---- */

static void test_header_last(void) {
  struct img_opt old_o = {.payload = 60000, .major = 1, .kind = FAKE_KIND, .sign = true, .seed = 31};
  struct img_opt new_o = {.payload = 60000, .major = 1, .minor = 1, .kind = FAKE_KIND, .sign = true, .seed = 32};
  static uint8_t old_img[SENSEI_G9_SLOT_SIZE];
  size_t on = build_image(old_img, &old_o);
  size_t n = build_image(image, &new_o);
  sensei_Gap9OtaInfo info;
  sensei_Gap9OtaBegin b = begin_req(image, n, false);
  uint32_t last = (uint32_t)((n - 1) / G9OTA_SECTOR * G9OTA_SECTOR);

  /* the inactive slot B holds a complete old image of the same size class: its tail is where the
   * new image's tail goes */
  fresh_mram();
  memcpy(mram.mem + SENSEI_G9_SLOT_B_OFF, old_img, on);
  boot(0);
  CHECK(slot_parses(1));
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(!slot_parses(1)); /* the header sector is erased at the start */
  for (uint32_t off = 0; off < n; off += 1024) {
    size_t m = n - off < 1024 ? n - off : 1024;

    CHECK(send_chunk(image, off, m, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    if (!slot_parses(1)) {
      continue;
    }
    CHECK(0); /* never: not before finish */
    break;
  }
  /* every byte but the first MRAM word is in the slot: still no parse, from the old tail or not
   */
  CHECK(memcmp(slot_mem(1) + 16, image + 16, n - 16) == 0 && !slot_parses(1));
  CHECK(last > 0);
  CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(slot_parses(1) && memcmp(slot_mem(1), image, n) == 0);
  CHECK(mram.reprogram == 0 && ota.writes_outside == 0);

  /* the same with a restart after the header sector: the word comes back from the checkpoint */
  fresh_mram();
  memcpy(mram.mem + SENSEI_G9_SLOT_B_OFF, old_img, on);
  boot(0);
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(send_from(image, n, 0, 1024, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  boot(0); /* power cut after the last byte, before finish */
  b.resume = true;
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK && info.offset == n);
  CHECK(!slot_parses(1));
  CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(slot_parses(1) && memcmp(slot_mem(1), image, n) == 0);

  /* an abort leaves a slot that does not parse */
  fresh_mram();
  memcpy(mram.mem + SENSEI_G9_SLOT_B_OFF, old_img, on);
  boot(0);
  b.resume = false;
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(send_from(image, n, 0, 1024, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(g9ota_abort(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK && !slot_parses(1));
}

/* ---- the app descriptor is checked as the SSBL checks it ---- */

static void test_app_check(void) {
  struct img_opt o = {.payload = 40000, .major = 1, .kind = FAKE_KIND, .sign = true, .seed = 41};
  sensei_Gap9OtaInfo info;

  fresh_mram();
  CHECK(run_all(&o, &info, false) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  o.bad_app = true; /* a section over the hand-off record */
  fresh_mram();
  CHECK(run_all(&o, &info, false) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
        info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE && info.detail == 2);
  CHECK(!info.slot_b.valid);
}

/* ---- verify: finish-style check of a slot without a transfer ---- */

static void test_verify(void) {
  struct img_opt good = {.payload = 40000, .major = 1, .minor = 2, .kind = FAKE_KIND, .sign = true, .seed = 51};
  struct img_opt bad = good;
  size_t n = build_image(image, &good);
  sensei_Gap9OtaInfo info, v;
  sensei_Gap9OtaBegin b = begin_req(image, n, false);

  fresh_mram();
  boot(0);
  install_running(0, 1, 2, 0);
  /* the running slot can be looked at too */
  CHECK(g9ota_verify(&ota, 0, &v) == sensei_Gap9OtaResult_GAP9_OTA_OK && v.image_hash.size == 32 &&
        v.signature.size == 64 && v.key_hash.size == 32 && v.signature.bytes[0] == 0x20);
  CHECK(g9ota_verify(&ota, 1, &v) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
        v.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE && v.image_hash.size == 0); /* empty */
  /* an unsigned image, a wrong kind, a bad digest, a bad app, in slot B: refused with the reason
   */
  {
    static uint8_t tmp[SENSEI_G9_SLOT_SIZE];
    size_t m;

    bad.sign = false;
    m = build_image(tmp, &bad);
    memcpy(mram.mem + SENSEI_G9_SLOT_B_OFF, tmp, m);
    CHECK(g9ota_verify(&ota, 1, &v) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
          v.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIGNATURE);
    bad = good;
    bad.kind = "sensei-ieq-nrf5340";
    m = build_image(tmp, &bad);
    memcpy(mram.mem + SENSEI_G9_SLOT_B_OFF, tmp, m);
    CHECK(g9ota_verify(&ota, 1, &v) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
          v.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_KIND);
    bad = good;
    bad.bad_digest = true;
    m = build_image(tmp, &bad);
    memcpy(mram.mem + SENSEI_G9_SLOT_B_OFF, tmp, m);
    CHECK(g9ota_verify(&ota, 1, &v) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
          v.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE);
    bad = good;
    bad.bad_app = true;
    m = build_image(tmp, &bad);
    memcpy(mram.mem + SENSEI_G9_SLOT_B_OFF, tmp, m);
    CHECK(g9ota_verify(&ota, 1, &v) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
          v.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE && v.detail == 2);
    m = build_image(tmp, &good);
    memcpy(mram.mem + SENSEI_G9_SLOT_B_OFF, tmp, m);
    mram.mem[SENSEI_G9_SLOT_B_OFF + 5000] ^= 1; /* a bit gone in the image: the hash TLV does not fit */
    CHECK(g9ota_verify(&ota, 1, &v) == sensei_Gap9OtaResult_GAP9_OTA_FAILED &&
          v.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE);
  }
  /* during a transfer: refused, and a verified transfer keeps its answer */
  fresh_mram();
  boot(0);
  install_running(0, 1, 2, 0);
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(send_from(image, n, 0, 1024, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(g9ota_verify(&ota, 0, &v) == sensei_Gap9OtaResult_GAP9_OTA_INVALID &&
        v.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE);
  CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(g9ota_verify(&ota, 0, &v) == sensei_Gap9OtaResult_GAP9_OTA_OK); /* the running slot, while VERIFIED */
  CHECK(g9ota_verify(&ota, 1, &v) == sensei_Gap9OtaResult_GAP9_OTA_OK &&
        memcmp(v.image_hash.bytes, info.image_hash.bytes, 32) == 0);
  CHECK(g9ota_status(&ota, &v) == sensei_Gap9OtaResult_GAP9_OTA_OK &&
        memcmp(v.image_hash.bytes, info.image_hash.bytes, 32) == 0 &&
        v.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED);
  CHECK(mram.reprogram == 0 && ota.writes_outside == 0);
}

/* ---- Q1: the writer is armed only for a record that names a slot with an image ---- */

/* The single-image MRAM of `make flash` (SDK SSBL, no slots): boot area, readfs at 0x9360 and the
 * app at 0x10910 with its descriptor. Sizes as the layout of a Release build with the stub
 * detector. The first checkpoint sector (0x10000) and slot A (0x14000) lie inside the app. */
static void single_image_mram(void) {
  uint32_t x = 12345;

  fresh_mram();
  for (uint32_t a = 0; a < 0x10910 + 0x69390; a += 4) {
    x = x * 1664525u + 1013904223u;
    put32(mram.mem + a, x);
  }
  put32(mram.mem + 0x10910, SENSEI_G9_APP_MAGIC); /* app descriptor: no MCUboot header anywhere */
}

static void test_arming(void) {
  struct g9ota_cfg l = fake_layout();
  uint8_t before[0x80000], after[0x80000];

  single_image_mram();
  memcpy(before, mram.mem, sizeof(before));
  /* without a record, and with a record for either slot: not armed, and nothing was written */
  for (int rec = -2; rec <= 2; rec++) {
    CHECK(g9ota_arm(&fake_ops, &mram, &l, rec) < 0);
  }
  memcpy(after, mram.mem, sizeof(after));
  CHECK(memcmp(before, after, sizeof(before)) == 0 && mram.writes == 0);

  /* the A/B layout: slot A holds an image */
  fresh_mram();
  install_running(0, 1, 0, 0);
  CHECK(g9ota_arm(&fake_ops, &mram, &l, 0) == 0);
  CHECK(g9ota_arm(&fake_ops, &mram, &l, 1) < 0); /* a record for the empty slot */
  CHECK(g9ota_arm(&fake_ops, &mram, &l, -1) < 0);
  /* the SSBL fell back from B to A: the record says A, whatever the pad says */
  install_running(1, 1, 0, 0);
  mram.mem[SENSEI_G9_SLOT_B_OFF + 3] ^= 0x80; /* B is damaged: not an image */
  CHECK(g9ota_arm(&fake_ops, &mram, &l, 0) == 0 && g9ota_arm(&fake_ops, &mram, &l, 1) < 0);
  /* a slot that is being written is not an image */
  fresh_mram();
  boot(0);
  {
    struct img_opt o = {.payload = 30000, .major = 1, .kind = FAKE_KIND, .sign = true, .seed = 61};
    size_t n = build_image(image, &o);
    sensei_Gap9OtaInfo info;
    sensei_Gap9OtaBegin b = begin_req(image, n, false);

    install_running(0, 1, 0, 0);
    CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    CHECK(send_from(image, n, 0, 1024, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    CHECK(g9ota_arm(&fake_ops, &mram, &l, 1) < 0);
  }
}

/* ---- the layout: g9ota_init refuses a wrong one, and the test area is allowed in few cases only ---- */

static void test_layout(void) {
  struct g9ota_cfg l = fake_layout();
  struct g9ota_cfg bad, out;
  struct g9ota o;
  /* the node's local test area: the end of the MRAM, away from the boot area */
  struct g9ota_cfg area = {
      .slot_base = {0x100000, 0x17C000}, .slot_size = 0x7C000, .ckpt_base = {0x1FC000, 0x1FE000}, .kind = NULL};

  CHECK(g9ota_cfg_check(&l, 0) == 0 && g9ota_cfg_check(&l, 1) == 0);
  CHECK(g9ota_cfg_check(&l, 2) != 0 && g9ota_cfg_check(NULL, 0) != 0);
  CHECK(g9ota_cfg_check(&area, 0) == 0);
  CHECK(g9ota_init(&o, &fake_ops, &mram, &l, 0) == 0 && o.target == 1);
  /* sector alignment: target, booted slot, checkpoints, size */
  for (int i = 0; i < 4; i++) {
    bad = l;
    if (i < 2) {
      bad.slot_base[i] += 16;
    } else {
      bad.ckpt_base[i - 2] += 16;
    }
    CHECK(g9ota_cfg_check(&bad, 0) != 0);
    CHECK(g9ota_init(&o, &fake_ops, &mram, &bad, 0) != 0 && o.fl == NULL);
  }
  bad = l;
  bad.slot_size -= 16;
  CHECK(g9ota_init(&o, &fake_ops, &mram, &bad, 0) != 0);
  bad.slot_size = 0;
  CHECK(g9ota_init(&o, &fake_ops, &mram, &bad, 0) != 0);
  /* overlap: a checkpoint sector in the booted slot, in the target, the slots over each other, the sectors over each
   * other */
  for (uint8_t booted = 0; booted < 2; booted++) {
    for (int c = 0; c < 2; c++) {
      bad = l;
      bad.ckpt_base[c] = l.slot_base[booted] + 5 * G9OTA_SECTOR; /* in the running slot */
      CHECK(g9ota_init(&o, &fake_ops, &mram, &bad, booted) != 0);
      bad.ckpt_base[c] = l.slot_base[1 - booted] + 5 * G9OTA_SECTOR; /* in the target */
      CHECK(g9ota_init(&o, &fake_ops, &mram, &bad, booted) != 0);
    }
  }
  bad = l;
  bad.slot_base[1] = l.slot_base[0] + G9OTA_SECTOR;
  CHECK(g9ota_init(&o, &fake_ops, &mram, &bad, 0) != 0);
  bad = l;
  bad.ckpt_base[1] = bad.ckpt_base[0];
  CHECK(g9ota_init(&o, &fake_ops, &mram, &bad, 0) != 0);
  /* the kind: up to 48 bytes (what the kind TLV reader takes), NULL refuses every image */
  bad = l;
  bad.kind = "0123456789012345678901234567890123456789012345678"; /* 49 */
  CHECK(g9ota_init(&o, &fake_ops, &mram, &bad, 0) != 0);
  bad.kind = "012345678901234567890123456789012345678901234567"; /* 48 */
  CHECK(g9ota_init(&o, &fake_ops, &mram, &bad, 0) == 0);
  bad.kind = NULL;
  CHECK(g9ota_init(&o, &fake_ops, &mram, &bad, 0) == 0);
  /* the operations are required */
  {
    struct g9ota_flash nofn = {fm_read, fm_erase, NULL};

    CHECK(g9ota_init(&o, &nofn, &mram, &l, 0) != 0);
    CHECK(g9ota_init(&o, NULL, &mram, &l, 0) != 0);
  }

  /* g9ota_select: the real layout needs a record whose slot holds an image */
  fresh_mram();
  install_running(1, 1, 0, 0);
  CHECK(g9ota_select(&fake_ops, &mram, &l, NULL, false, 1, &out) == 1 && out.slot_base[1] == l.slot_base[1]);
  CHECK(g9ota_select(&fake_ops, &mram, &l, NULL, false, 0, &out) < 0); /* record for the empty slot */
  CHECK(g9ota_select(&fake_ops, &mram, &l, NULL, false, -1, &out) < 0);
  bad = l;
  bad.ckpt_base[0] = l.slot_base[0] + G9OTA_SECTOR; /* a layout whose checkpoint lies in the target */
  CHECK(g9ota_select(&fake_ops, &mram, &bad, NULL, false, 1, &out) == -EINVAL);

  /* the test area: not in a build without the local test, not with a record (the area lies on the slot B of the real
   * layout: started from slot B it would write the running slot), only without a record */
  fresh_mram();
  install_running(1, 1, 0, 0);
  CHECK(g9ota_select(&fake_ops, &mram, &l, &area, false, -1, &out) == -EPERM);
  CHECK(g9ota_select(&fake_ops, &mram, &l, &area, true, 1, &out) == -EPERM);
  CHECK(g9ota_select(&fake_ops, &mram, &l, &area, true, 0, &out) == -EPERM);
  CHECK(g9ota_select(&fake_ops, &mram, &l, &area, true, -1, &out) == 0);
  CHECK(out.slot_base[0] == area.slot_base[0] && out.ckpt_base[1] == area.ckpt_base[1] && out.kind == l.kind);
  CHECK(mram.writes == 0);
  /* a test area that is not a valid layout */
  bad = area;
  bad.ckpt_base[0] = area.slot_base[1] + G9OTA_SECTOR;
  CHECK(g9ota_select(&fake_ops, &mram, &l, &bad, true, -1, &out) == -EINVAL);
  bad = area;
  bad.slot_base[1] += 4;
  CHECK(g9ota_select(&fake_ops, &mram, &l, &bad, true, -1, &out) == -EINVAL);
}

/* ---- the record of a finished transfer: an abort drops it until the image runs ---- */

static void test_done_record(void) {
  struct img_opt o = {.payload = 30000, .major = 1, .minor = 1, .kind = FAKE_KIND, .sign = true, .seed = 71};
  size_t n = build_image(image, &o);
  sensei_Gap9OtaInfo info;
  sensei_Gap9OtaBegin b = begin_req(image, n, false);

  fresh_mram();
  install_running(0, 1, 0, 0);
  boot(0);
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(send_from(image, n, 0, 1024, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);

  /* GAP9 loses power, the node did not accept the image: abort drops it */
  boot(0);
  g9ota_boot_note(&ota);
  CHECK(slot_parses(1));
  CHECK(g9ota_abort(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK && !slot_parses(1) && !info.slot_b.valid);

  /* the same, but the new image was started (apply): its record is consumed, an abort that
   * comes after a revert leaves it alone */
  fresh_mram();
  install_running(0, 1, 0, 0);
  boot(0);
  b.resume = false;
  CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(send_from(image, n, 0, 1024, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
  boot(1); /* the new image runs */
  g9ota_boot_note(&ota);
  boot(0); /* revert: the old image runs again */
  g9ota_boot_note(&ota);
  CHECK(slot_parses(1) && memcmp(slot_mem(1), image, n) == 0);
  CHECK(g9ota_abort(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK && slot_parses(1));
  CHECK(info.slot_b.valid && memcmp(slot_mem(1), image, n) == 0 && mram.reprogram == 0);
}

/* ---- power cut at every write ---- */

struct cutrun {
  long total_writes;
  long cuts_tested;
  long resumed_nonzero;
};

/* The state a restart finds: new RAM state over the same MRAM. */
static void restart(uint8_t booted) {
  mram.dead = false;
  mram.cut_at = -1;
  boot(booted);
}

/* Drive the transfer to the end, cutting power at the `cut`-th write (and at `cut2` writes after
 * the first restart, -1: no second cut). Returns false if the result is wrong. */
static bool cut_run(const uint8_t *img, size_t n, uint8_t booted, long cut, long cut2, unsigned seed, size_t step,
                    struct cutrun *stats) {
  sensei_Gap9OtaInfo info;
  sensei_Gap9OtaBegin b = begin_req(img, n, false);
  uint8_t tgt = 1 - booted;
  int cuts_left = cut2 >= 0 ? 2 : 1;
  bool ok = true;

  memcpy(pre_copy, slot_mem(tgt), SENSEI_G9_SLOT_SIZE);
  mram.writes = 0;
  mram.cut_at = cut;
  mram.cut_seed = seed;
  boot(booted);
  for (int guard = 0; guard < 8; guard++) {
    sensei_Gap9OtaResult r = g9ota_begin(&ota, &b, &info);
    uint32_t from;

    if (guard > 0 && r == sensei_Gap9OtaResult_GAP9_OTA_OK) {
      /* a resume after a restart: the offset is a sector boundary, or the end */
      ok &= (info.offset % G9OTA_SECTOR == 0 || info.offset == n);
    }
    if (r == sensei_Gap9OtaResult_GAP9_OTA_OK) {
      from = info.offset;
      ok &= from <= (uint32_t)n;
      r = send_from(img, n, from, step, &info);
      if (r == sensei_Gap9OtaResult_GAP9_OTA_OK) {
        r = g9ota_finish(&ota, &info);
      }
    }
    if (r == sensei_Gap9OtaResult_GAP9_OTA_OK && !mram.dead) {
      ok &= info.stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED;
      break;
    }
    if (r == sensei_Gap9OtaResult_GAP9_OTA_FAILED && !mram.dead && info.fail == sensei_Gap9OtaFail_GAP9_OTA_FAIL_HASH &&
        b.resume) {
      /* The cut hit the write of the magic sector (the last one): the slot has no header, the
       * hash does not match, the transfer starts again at 0. This is the host's rule. */
      b.resume = false;
      continue;
    }
    /* the power went: restart, resume */
    ok &= mram.dead;
    stats->cuts_tested++;
    {
      sensei_Gap9Slot sl;

      restart(booted);
      /* A slot that reports a valid image holds the old one (the cut came before the transfer
       * was recorded) or the whole new one (the cut hit the last record after everything was
       * written). A half written slot is never one. */
      if (g9ota_slot_info(&ota, tgt, &sl)) {
        ok &= memcmp(slot_mem(tgt), pre_copy, SENSEI_G9_SLOT_SIZE) == 0 || memcmp(slot_mem(tgt), img, n) == 0;
      }
      /* What the SSBL sees (it knows nothing of the checkpoint): the old image, the complete
       * new one, or a slot that does not parse. Never a mix. */
      if (slot_parses(tgt)) {
        ok &= memcmp(slot_mem(tgt), pre_copy, SENSEI_G9_SLOT_SIZE) == 0 || memcmp(slot_mem(tgt), img, n) == 0;
      }
    }
    cuts_left--;
    if (cuts_left == 1 && cut2 >= 0) {
      mram.cut_at = mram.writes + cut2;
      mram.cut_seed = seed + 1;
    }
    b.resume = true;
  }
  ok &= !mram.dead;
  if (getenv("G9OTA_DEBUG")) {
    printf("dbg: dead %d writes_outside %u reprogram %ld bad_align %ld tgt_match %d "
           "booted_match %d\n",
           mram.dead, (unsigned)ota.writes_outside, mram.reprogram, mram.bad_align, memcmp(slot_mem(tgt), img, n) == 0,
           memcmp(slot_mem(booted), booted_copy, SENSEI_G9_SLOT_SIZE) == 0);
  }
  /* the end state: the image is in place, the slot that runs untouched, no write outside the
   * guard */
  ok &= memcmp(slot_mem(tgt), img, n) == 0;
  ok &= memcmp(slot_mem(booted), booted_copy, SENSEI_G9_SLOT_SIZE) == 0;
  ok &= ota.writes_outside == 0 && mram.reprogram == 0 && mram.bad_align == 0;
  {
    sensei_Gap9Slot sl;

    restart(booted);
    ok &= g9ota_slot_info(&ota, tgt, &sl) && sl.valid; /* done: a valid slot after the next boot */
  }
  return ok;
}

/* Small updates until the next checkpoint record is at the end of its sector: the cuts then hit the
 * log's turn to the other sector (erase of the other sector, first record in it). */
static void prefill_log(uint8_t booted, unsigned room) {
  struct img_opt o = {.payload = 100, .major = 1, .kind = FAKE_KIND, .sign = true, .seed = 1};

  boot(booted);
  for (int i = 0; i < 400; i++) {
    sensei_Gap9OtaInfo info;
    size_t n = build_image(image, &o);
    sensei_Gap9OtaBegin b = begin_req(image, n, false);

    if (ota.ck_loaded && ota.ck_next >= G9OTA_CKPT_PER_SECTOR - room && ota.ck_next < G9OTA_CKPT_PER_SECTOR - 1) {
      return;
    }
    o.seed++;
    (void)g9ota_begin(&ota, &b, &info);
    (void)send_from(image, n, 0, 1024, &info);
    (void)g9ota_finish(&ota, &info);
  }
}

static void test_power_cuts(uint8_t booted, size_t payload, size_t step, unsigned seeds, unsigned log_room) {
  struct img_opt o = {.payload = payload, .major = 1, .minor = 1, .kind = FAKE_KIND, .sign = true, .seed = 21};
  size_t n = build_image(image, &o);
  struct cutrun st = {0};
  long total;

  /* a clean run counts the writes */
  fresh_mram();
  install_running(booted, 1, 0, 0);
  memcpy(booted_copy, slot_mem(booted), SENSEI_G9_SLOT_SIZE);
  if (log_room > 0) {
    prefill_log(booted, log_room);
    n = build_image(image, &o); /* the log runs through the same buffer */
    mram.writes = 0;
    memcpy(booted_copy, slot_mem(booted), SENSEI_G9_SLOT_SIZE);
  }
  {
    static uint8_t snapshot[MRAM_SIZE];
    memcpy(snapshot, mram.mem, MRAM_SIZE);
    CHECK(cut_run(image, n, booted, -1, -1, 0, step, &st));
    total = mram.writes;
    st.total_writes = total;
    CHECK(total > (long)(n / G9OTA_SECTOR)); /* sectors and records */
    for (long cut = 0; cut < total; cut++) {
      for (unsigned s = 0; s < seeds; s++) {
        memcpy(mram.mem, snapshot, MRAM_SIZE);
        bool ok = cut_run(image, n, booted, cut, -1, s * 977 + 13, step, &st);

        if (!ok) {
          printf("power cut at write %ld seed %u: wrong result\n", cut, s);
        }
        CHECK(ok);
        if (!ok) {
          return;
        }
      }
    }
    /* a second cut during the resume, at a few places */
    for (long cut = 0; cut < total; cut += (total / 23) + 1) {
      for (long second = 0; second < 9; second++) {
        memcpy(mram.mem, snapshot, MRAM_SIZE);
        bool ok = cut_run(image, n, booted, cut, second, (unsigned)(cut * 31 + second), step, &st);

        if (!ok) {
          printf("power cuts at write %ld and %ld later: wrong result\n", cut, second);
        }
        CHECK(ok);
        if (!ok) {
          return;
        }
      }
    }
  }
  printf("  booted %u, %zu B, %zu B chunks, log %s: %ld writes per update, %ld cuts tested\n", booted, n, step,
         log_room ? "near its turn" : "at the start", total, st.cuts_tested);
}

/* ---- the checkpoint log runs through both of its sectors ---- */

static void test_log_wrap(void) {
  struct img_opt o = {.payload = 3 * G9OTA_SECTOR + 100, .major = 1, .kind = FAKE_KIND, .sign = true, .seed = 2};
  size_t n = build_image(image, &o);
  sensei_Gap9OtaInfo info;
  long wrap_checks = 0;

  fresh_mram();
  boot(0);
  /* one record per sector, begin and done: about 6 per update; 100 updates write 600 records and
   * cross each of the two log sectors (102 records each) several times */
  for (int i = 0; i < 100; i++) {
    sensei_Gap9OtaBegin b;

    o.seed = (unsigned)(2 + i);
    n = build_image(image, &o);
    b = begin_req(image, n, false);
    boot(0); /* a restart in between: the log is read again */
    CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    CHECK(send_from(image, n, 0, 1024, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    if (i % 3 == 0) { /* a restart in the middle: resume */
      uint32_t at = info.offset;

      boot(0);
      b.resume = true;
      CHECK(g9ota_begin(&ota, &b, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK && info.offset == at);
    }
    CHECK(g9ota_finish(&ota, &info) == sensei_Gap9OtaResult_GAP9_OTA_OK);
    CHECK(memcmp(slot_mem(1), image, n) == 0);
    wrap_checks++;
  }
  CHECK(ota.ck_seq > 2 * G9OTA_CKPT_PER_SECTOR);
  CHECK(mram.reprogram == 0 && ota.writes_outside == 0);
  printf("  %ld updates, checkpoint log at record %u\n", wrap_checks, (unsigned)ota.ck_seq);
}

/* ---- a file from outside (imgtool): feed it through the writer ---- */

static int feed_file(const char *path, uint8_t booted) {
  FILE *f = fopen(path, "rb");
  static uint8_t buf[SENSEI_G9_SLOT_SIZE];
  size_t n;
  sensei_Gap9OtaInfo info;
  sensei_Gap9OtaBegin b;

  if (f == NULL) {
    printf("cannot open %s\n", path);
    return 2;
  }
  n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  fresh_mram();
  boot(booted);
  b = begin_req(buf, n, false);
  if (g9ota_begin(&ota, &b, &info) != sensei_Gap9OtaResult_GAP9_OTA_OK || send_from(buf, n, 0, 1024, &info) != 0 ||
      g9ota_finish(&ota, &info) != sensei_Gap9OtaResult_GAP9_OTA_OK) {
    printf("%s: refused: stage %d fail %d\n", path, (int)info.stage, (int)info.fail);
    return 1;
  }
  {
    sensei_Gap9Slot s;

    (void)g9ota_slot_info(&ota, 1 - booted, &s);
    printf("%s: verified, slot %u: version %u.%u.%u image_id %08x hash ", path, 1 - booted, (unsigned)s.version.major,
           (unsigned)s.version.minor, (unsigned)s.version.patch, (unsigned)s.image_id);
  }
  for (pb_size_t i = 0; i < info.image_hash.size; i++) {
    printf("%02x", info.image_hash.bytes[i]);
  }
  printf(" sig ");
  for (pb_size_t i = 0; i < info.signature.size; i++) {
    printf("%02x", info.signature.bytes[i]);
  }
  printf("\n");
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1) {
    file_kind = argc > 3 ? argv[3] : NULL;
    return feed_file(argv[1], argc > 2 ? (uint8_t)atoi(argv[2]) : 0);
  }
  test_helpers();
  test_guard();
  test_transfer(0, 1024);
  test_transfer(1, 188);
  test_rules();
  test_finish_checks();
  test_header_last();
  test_app_check();
  test_verify();
  test_arming();
  test_layout();
  test_done_record();
  test_log_wrap();
  test_power_cuts(0, 3 * G9OTA_SECTOR + 4000, 188, 3, 0);
  test_power_cuts(1, 2 * G9OTA_SECTOR + 77, 1024, 3, 0);
  test_power_cuts(0, 3 * G9OTA_SECTOR + 123, 1024, 3, 6); /* the log turns to its other sector during the update */
  test_power_cuts(1, 40 * G9OTA_SECTOR, 1024, 2, 0);      /* a long image */
  DONE();
}
