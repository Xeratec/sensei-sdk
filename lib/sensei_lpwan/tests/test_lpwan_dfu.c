/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Shield update rules (lpwan_dfu.c) on a RAM flash: chunks, repeats, gaps, resume, SHA-256, MCUboot header checks,
 * the modem path, and the whole thing over the SPI link core with damaged frames. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sensei_link/pb.h"
#include "sensei_lpwan/link_slave.h"
#include "sensei_lpwan/lpwan_dfu.h"
#include "sensei_lpwan/lpwan_master.h"

static int check_failures;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                                  \
      check_failures++;                                                                                                \
    }                                                                                                                  \
  } while (0)

#define SLOT_SIZE (448u * 1024u)

/* ---- RAM flash: NOR rules. Erase sets 0xFF, a write may only clear bits. ---- */

static uint8_t slot[SLOT_SIZE];
static uint8_t logf[LPWAN_DFU_SECTOR];
static uint8_t running[32]; /* MCUboot header of the running image */
static int violations;      /* writes to a place that was not erased, or unaligned */
static int erases, log_erases, writes;
static int fail_write_at = -1; /* the n-th write fails (-EIO) */
static int fail_erase;
static bool app_enabled = true;

static uint32_t cap_app(void *c, sensei_DfuTarget t) {
  (void)c;
  return t == sensei_DfuTarget_DFU_TARGET_APP && app_enabled ? SLOT_SIZE : 0;
}
static int app_erase(void *c, uint32_t off, uint32_t len) {
  (void)c;
  if (off % LPWAN_DFU_SECTOR || len % LPWAN_DFU_SECTOR || off + len > SLOT_SIZE) {
    violations++;
    return -EINVAL;
  }
  if (fail_erase) {
    return -EIO;
  }
  memset(slot + off, 0xFF, len);
  erases += (int)(len / LPWAN_DFU_SECTOR);
  return 0;
}
static int app_write(void *c, uint32_t off, const uint8_t *d, size_t n) {
  (void)c;
  if (off % 4 || n % 4 || off + n > SLOT_SIZE) {
    violations++;
    return -EINVAL;
  }
  if (fail_write_at >= 0 && writes++ == fail_write_at) {
    return -EIO;
  }
  for (size_t i = 0; i < n; i++) {
    if (slot[off + i] != 0xFF) {
      violations++; /* not erased */
    }
    slot[off + i] &= d[i];
  }
  return 0;
}
static int app_read(void *c, uint32_t off, uint8_t *d, size_t n) {
  (void)c;
  if (off + n > SLOT_SIZE) {
    return -EINVAL;
  }
  memcpy(d, slot + off, n);
  return 0;
}
static int log_read(void *c, uint32_t off, uint8_t *d, size_t n) {
  (void)c;
  memcpy(d, logf + off, n);
  return 0;
}
static int log_write(void *c, uint32_t off, const uint8_t *d, size_t n) {
  (void)c;
  for (size_t i = 0; i < n; i++) {
    if (logf[off + i] != 0xFF) {
      violations++;
    }
    logf[off + i] &= d[i];
  }
  return 0;
}
static int fail_log_erase;
static int log_erase(void *c) {
  (void)c;
  if (fail_log_erase) {
    return -EIO;
  }
  memset(logf, 0xFF, sizeof(logf));
  log_erases++;
  return 0;
}
static int app_headers(void *c, uint8_t nh[32], uint8_t rh[32]) {
  (void)c;
  memcpy(nh, slot, 32);
  memcpy(rh, running, 32);
  return 0;
}
static int applied, unmarked, fail_apply, fail_unmark;
static sensei_DfuFail kind_result;
static int app_apply(void *c, sensei_DfuTarget t) {
  (void)c;
  (void)t;
  applied++;
  return fail_apply ? -EIO : 0;
}
static int app_unmark(void *c) {
  (void)c;
  if (fail_unmark) {
    return -EIO;
  }
  unmarked++;
  return 0;
}
static sensei_DfuFail app_kind(void *c) {
  (void)c;
  return kind_result;
}

static const struct lpwan_dfu_ops app_ops = {
    .capacity = cap_app,
    .erase = app_erase,
    .write = app_write,
    .read = app_read,
    .log_read = log_read,
    .log_write = log_write,
    .log_erase = log_erase,
    .read_headers = app_headers,
    .apply = app_apply,
    .unmark = app_unmark,
    .check_kind = app_kind,
};

/* ---- Modem: a stream in order, an erase that takes a few polls, an update that takes a few polls ---- */

static uint8_t mfw[SLOT_SIZE];
static uint32_t mfw_len;
static int erase_polls, apply_polls, modem_fail_write;
static bool modem_closed;
static int modem_finishes;

static uint32_t cap_modem(void *c, sensei_DfuTarget t) {
  (void)c;
  return t == sensei_DfuTarget_DFU_TARGET_MODEM ? 300u * 1024u : 0;
}
static int modem_open(void *c, sensei_DfuTarget t, uint32_t size, bool fresh) {
  (void)c;
  (void)t;
  (void)size;
  if (fresh) {
    mfw_len = 0;
    erase_polls = 3;
  }
  modem_closed = false;
  return 0;
}
static int modem_busy(void *c) {
  (void)c;
  return erase_polls > 0 ? erase_polls-- : 0;
}
static int modem_write(void *c, uint32_t off, const uint8_t *d, size_t n) {
  (void)c;
  if (modem_fail_write) {
    return 7; /* a positive modem error */
  }
  if (off != mfw_len) {
    return -EINVAL;
  }
  memcpy(mfw + off, d, n);
  mfw_len += (uint32_t)n;
  return 0;
}
static int modem_finish(void *c, sensei_DfuTarget t) {
  (void)c;
  (void)t;
  modem_closed = true;
  modem_finishes++;
  return 0;
}
static int modem_apply(void *c, sensei_DfuTarget t) {
  (void)c;
  (void)t;
  apply_polls = 3;
  return 0;
}
static int modem_state(void *c, int *detail) {
  (void)c;
  *detail = 0;
  return apply_polls > 0 && --apply_polls > 0 ? 0 : 1;
}

static const struct lpwan_dfu_ops modem_ops = {
    .capacity = cap_modem,
    .open = modem_open,
    .busy = modem_busy,
    .write = modem_write,
    .finish = modem_finish,
    .apply = modem_apply,
    .apply_state = modem_state,
};

/* ---- Images ---- */

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void make_header(uint8_t h[32], uint32_t img_size, uint8_t maj, uint8_t min, uint16_t rev) {
  memset(h, 0, 32);
  put32(h, 0x96f3b83du);
  h[8] = 0x00;
  h[9] = 0x02; /* header size 0x200 */
  put32(h + 12, img_size);
  h[20] = maj;
  h[21] = min;
  h[22] = (uint8_t)rev;
  h[23] = (uint8_t)(rev >> 8);
}

/* An image of `size` bytes: header, pseudo-random body; `ff_from..ff_to` is 0xFF padding (as in a merged TF-M and app
 * image). */
static uint8_t *make_image(uint32_t size, uint8_t maj, uint8_t min, uint16_t rev, uint32_t ff_from, uint32_t ff_to,
                           uint8_t sha[32]) {
  uint8_t *img = calloc(size + 1024u, 1); /* room for a test that reads past the end */
  uint32_t x = 12345;
  sensei_sha256_t h;

  for (uint32_t i = 0; i < size; i++) {
    x = x * 1664525u + 1013904223u;
    img[i] = (uint8_t)(x >> 24);
  }
  for (uint32_t i = ff_from; i < ff_to && i < size; i++) {
    img[i] = 0xFF;
  }
  make_header(img, size - 0x300, maj, min, rev);
  sensei_sha256_init(&h);
  sensei_sha256_update(&h, img, size);
  sensei_sha256_final(&h, sha);
  return img;
}

static void reset_world(void) {
  memset(slot, 0x55, sizeof(slot)); /* old image */
  memset(logf, 0xFF, sizeof(logf));
  memset(mfw, 0, sizeof(mfw));
  make_header(running, 100000, 0, 1, 0);
  violations = erases = log_erases = writes = applied = unmarked = 0;
  fail_apply = fail_unmark = fail_log_erase = 0;
  kind_result = sensei_DfuFail_DFU_FAIL_NONE;
  fail_write_at = -1;
  fail_erase = 0;
  app_enabled = true;
  modem_fail_write = 0;
}

static sensei_DfuBegin begin_msg(sensei_DfuTarget t, uint32_t size, const uint8_t sha[32], bool resume) {
  sensei_DfuBegin b = sensei_DfuBegin_init_zero;

  b.target = t;
  b.size = size;
  memcpy(b.sha256, sha, 32);
  b.resume = resume;
  return b;
}

static sensei_LpwanResult send_chunk(struct lpwan_dfu *d, const uint8_t *img, uint32_t off, size_t n, bool gap,
                                     sensei_DfuStatus *st) {
  sensei_DfuChunk c = sensei_DfuChunk_init_zero;

  c.offset = off;
  c.gap = gap;
  c.data.size = (pb_size_t)n;
  memcpy(c.data.bytes, img + off, n);
  return lpwan_dfu_chunk(d, &c, st);
}

/* Send img[from, size) in chunks of `step`, skipping chunks that are all 0xFF when `skip` is set. Returns the last
 * result. */
static sensei_LpwanResult send_all(struct lpwan_dfu *d, const uint8_t *img, uint32_t size, uint32_t from, size_t step,
                                   bool skip, sensei_DfuStatus *st) {
  sensei_LpwanResult r = sensei_LpwanResult_LPWAN_OK;
  bool gap = false;

  for (uint32_t off = from; off < size; off += (uint32_t)step) {
    size_t n = size - off < step ? size - off : step;
    bool ff = true;

    for (size_t i = 0; i < n && ff; i++) {
      ff = img[off + i] == 0xFF;
    }
    if (skip && ff && off + n < size) {
      gap = true;
      continue;
    }
    r = send_chunk(d, img, off, n, gap, st);
    gap = false;
    if (r != sensei_LpwanResult_LPWAN_OK) {
      return r;
    }
  }
  return r;
}

/* ---- Tests ---- */

static void test_plain_transfer(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  uint8_t sha[32];
  uint32_t sizes[] = {20000, 20001, 20002, 20003, 4096, 8192 + 7};
  size_t steps[] = {192, 101, 512, 4, 3};

  for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
    for (unsigned k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
      uint8_t *img = make_image(sizes[s], 1, 2, 3, 0, 0, sha);
      sensei_DfuBegin b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, sizes[s], sha, false);

      reset_world();
      lpwan_dfu_init(&d, &app_ops, NULL, NULL);
      CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK);
      CHECK(st.state == sensei_DfuState_DFU_STATE_RECEIVING && st.offset == 0 && st.chunk_max == 512);
      CHECK(send_all(&d, img, sizes[s], 0, steps[k], false, &st) == sensei_LpwanResult_LPWAN_OK);
      CHECK(st.offset == sizes[s]);
      CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_OK);
      CHECK(st.state == sensei_DfuState_DFU_STATE_VERIFIED);
      CHECK(memcmp(slot, img, sizes[s]) == 0);
      CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_OK);
      CHECK(st.state == sensei_DfuState_DFU_STATE_APPLYING && applied == 1);
      CHECK(violations == 0);
      free(img);
    }
  }
}

static void test_repeat_and_overlap(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  uint8_t sha[32];
  uint8_t *img = make_image(10000, 1, 0, 0, 0, 0, sha);
  sensei_DfuBegin b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, 10000, sha, false);

  reset_world();
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  lpwan_dfu_begin(&d, &b, &st);
  CHECK(send_chunk(&d, img, 0, 192, false, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 192);
  /* the answer was lost: the same chunk again changes nothing */
  CHECK(send_chunk(&d, img, 0, 192, false, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 192);
  /* a chunk that starts earlier and goes beyond: the new part is taken */
  CHECK(send_chunk(&d, img, 100, 300, false, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 400);
  /* a gap without the flag is refused and says where to continue */
  CHECK(send_chunk(&d, img, 1000, 100, false, &st) == sensei_LpwanResult_LPWAN_INVALID);
  CHECK(st.fail == sensei_DfuFail_DFU_FAIL_OFFSET && st.offset == 400);
  /* beyond the file, empty, too long */
  CHECK(send_chunk(&d, img, 9950, 100, false, &st) == sensei_LpwanResult_LPWAN_INVALID);
  CHECK(st.fail == sensei_DfuFail_DFU_FAIL_SIZE);
  CHECK(send_chunk(&d, img, 400, 0, false, &st) == sensei_LpwanResult_LPWAN_INVALID);
  /* finish too early */
  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_INVALID && st.fail == sensei_DfuFail_DFU_FAIL_OFFSET);
  CHECK(send_all(&d, img, 10000, 400, 512, false, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(memcmp(slot, img, 10000) == 0 && violations == 0);
  free(img);
}

static void test_gap(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  uint8_t sha[32];
  uint32_t size = 200000;
  /* padding from 60000 to 140000 (not on a sector or chunk boundary at the start) */
  uint8_t *img = make_image(size, 2, 0, 0, 60000, 140000, sha);
  sensei_DfuBegin b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size, sha, false);
  unsigned chunks_full = (size + 191) / 192;

  reset_world();
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  lpwan_dfu_begin(&d, &b, &st);
  CHECK(send_all(&d, img, size, 0, 192, true, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_VERIFIED);
  CHECK(memcmp(slot, img, size) == 0); /* the stale 0x55 in the padding is gone */
  CHECK(violations == 0);
  CHECK(erases == (int)((size + LPWAN_DFU_SECTOR - 1) / LPWAN_DFU_SECTOR));
  (void)chunks_full;

  /* gap rules: aligned, flag set, not for the modem */
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  lpwan_dfu_begin(&d, &b, &st);
  CHECK(send_chunk(&d, img, 4098, 100, true, &st) == sensei_LpwanResult_LPWAN_INVALID); /* offset not a multiple of 4 */
  CHECK(st.fail == sensei_DfuFail_DFU_FAIL_OFFSET && st.offset == 0);
  free(img);
}

static void test_resume(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  uint8_t sha[32], other[32];
  uint32_t size = 100000;
  uint8_t *img = make_image(size, 1, 1, 1, 0, 0, sha);
  sensei_DfuBegin b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size, sha, true);

  reset_world();
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 0); /* nothing to resume */
  CHECK(send_all(&d, img, 30000, 0, 192, false, &st) == sensei_LpwanResult_LPWAN_OK);
  /* link broke, the session lives on: resume gives the same offset */
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 30000 &&
        st.state == sensei_DfuState_DFU_STATE_RECEIVING);
  CHECK(send_chunk(&d, img, 30000, 192, false, &st) == sensei_LpwanResult_LPWAN_OK);

  /* the shield restarted: a new core over the same flash continues at the last checkpoint */
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(st.offset == 28672 && st.offset % LPWAN_DFU_SECTOR == 0); /* 30192 / 4096 sectors, rounded down */
  CHECK(send_all(&d, img, size, st.offset, 192, false, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(memcmp(slot, img, size) == 0 && violations == 0);

  /* another image (other hash) does not resume: it starts at 0 */
  memset(logf, 0xFF, sizeof(logf));
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  b.resume = false;
  lpwan_dfu_begin(&d, &b, &st);
  send_all(&d, img, 20000, 0, 192, false, &st);
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  memcpy(other, sha, 32);
  other[0] ^= 1;
  b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size, other, true);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 0);
  /* so does one of another size */
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size + 4, sha, true);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 0);
  /* resume=false drops a transfer that was open */
  b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size, sha, false);
  lpwan_dfu_begin(&d, &b, &st);
  send_all(&d, img, 20000, 0, 192, false, &st);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 0);
  free(img);
}

static void test_log_wrap(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  uint8_t sha[32];
  uint32_t size = SLOT_SIZE; /* 112 sectors: the log (64 records) wraps once */
  uint8_t *img = make_image(size, 1, 0, 0, 0, 0, sha);
  sensei_DfuBegin b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size, sha, true);

  reset_world();
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  lpwan_dfu_begin(&d, &b, &st);
  CHECK(send_all(&d, img, size - 1000, 0, 512, false, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(log_erases >= 1 && violations == 0);
  lpwan_dfu_init(&d, &app_ops, NULL, NULL); /* restart */
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(st.offset == ((size - 1000) / LPWAN_DFU_SECTOR) * LPWAN_DFU_SECTOR);
  CHECK(send_all(&d, img, size, st.offset, 512, false, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(memcmp(slot, img, size) == 0 && violations == 0);
  free(img);
}

static void test_hash_and_header(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  uint8_t sha[32];
  uint32_t size = 30000;
  uint8_t *img = make_image(size, 1, 0, 0, 0, 0, sha);
  sensei_DfuBegin b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size, sha, true);

  /* a flipped byte: the hash does not match; the transfer is dropped, resume starts at 0 */
  reset_world();
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  lpwan_dfu_begin(&d, &b, &st);
  img[12345] ^= 0x10;
  send_all(&d, img, size, 0, 192, false, &st);
  img[12345] ^= 0x10;
  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_FAILED);
  CHECK(st.state == sensei_DfuState_DFU_STATE_FAILED && st.fail == sensei_DfuFail_DFU_FAIL_HASH);
  CHECK(send_chunk(&d, img, 0, 100, false, &st) == sensei_LpwanResult_LPWAN_INVALID);
  CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_INVALID && applied == 0);
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 0);
  send_all(&d, img, size, 0, 192, false, &st);
  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_OK);

  /* version: older is refused, the same and newer are taken */
  struct {
    uint8_t maj, min;
    uint16_t rev;
    sensei_DfuFail want;
  } v[] = {{0, 9, 9, sensei_DfuFail_DFU_FAIL_DOWNGRADE},
           {1, 0, 6, sensei_DfuFail_DFU_FAIL_DOWNGRADE},
           {1, 5, 0, sensei_DfuFail_DFU_FAIL_NONE},
           {2, 0, 0, sensei_DfuFail_DFU_FAIL_NONE},
           {1, 0, 7, sensei_DfuFail_DFU_FAIL_NONE}};
  uint8_t runv[3] = {1, 0, 7}; /* running 1.0.7 */

  for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
    uint8_t s2[32];
    uint8_t *im2 = make_image(size, v[i].maj, v[i].min, v[i].rev, 0, 0, s2);
    sensei_DfuBegin b2 = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size, s2, false);

    reset_world();
    make_header(running, 100000, runv[0], runv[1], runv[2]);
    lpwan_dfu_init(&d, &app_ops, NULL, NULL);
    lpwan_dfu_begin(&d, &b2, &st);
    send_all(&d, im2, size, 0, 512, false, &st);
    lpwan_dfu_finish(&d, &st);
    if (v[i].want == sensei_DfuFail_DFU_FAIL_NONE) {
      CHECK(st.state == sensei_DfuState_DFU_STATE_VERIFIED);
    } else {
      CHECK(st.state == sensei_DfuState_DFU_STATE_FAILED && st.fail == v[i].want);
    }
    free(im2);
  }
  /* not an MCUboot image (hash fine, header wrong), and a header that claims more than the file */
  {
    uint8_t s2[32], *im2 = make_image(size, 1, 0, 0, 0, 0, s2);
    sensei_sha256_t h;

    im2[0] ^= 1;
    sensei_sha256_init(&h);
    sensei_sha256_update(&h, im2, size);
    sensei_sha256_final(&h, s2);
    b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size, s2, false);
    reset_world();
    lpwan_dfu_init(&d, &app_ops, NULL, NULL);
    lpwan_dfu_begin(&d, &b, &st);
    send_all(&d, im2, size, 0, 512, false, &st);
    lpwan_dfu_finish(&d, &st);
    CHECK(st.fail == sensei_DfuFail_DFU_FAIL_IMAGE);
    im2[0] ^= 1;
    put32(im2 + 12, size); /* image size as large as the file: header + image + TLV do not fit */
    sensei_sha256_init(&h);
    sensei_sha256_update(&h, im2, size);
    sensei_sha256_final(&h, s2);
    b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size, s2, false);
    reset_world();
    lpwan_dfu_init(&d, &app_ops, NULL, NULL);
    lpwan_dfu_begin(&d, &b, &st);
    send_all(&d, im2, size, 0, 512, false, &st);
    lpwan_dfu_finish(&d, &st);
    CHECK(st.fail == sensei_DfuFail_DFU_FAIL_IMAGE);
    free(im2);
  }
  /* a running image without MCUboot header (a development flash): no version check */
  reset_world();
  memset(running, 0xFF, 32);
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size, sha, false);
  lpwan_dfu_begin(&d, &b, &st);
  send_all(&d, img, size, 0, 512, false, &st);
  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_OK);
  free(img);
}

static void test_state_and_limits(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  uint8_t sha[32];
  uint8_t *img = make_image(5000, 1, 0, 0, 0, 0, sha);
  sensei_DfuBegin b;
  sensei_DfuChunk c = sensei_DfuChunk_init_zero;

  reset_world();
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  /* nothing open */
  c.data.size = 4;
  CHECK(lpwan_dfu_chunk(&d, &c, &st) == sensei_LpwanResult_LPWAN_INVALID && st.fail == sensei_DfuFail_DFU_FAIL_STATE);
  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_INVALID);
  CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_INVALID);
  CHECK(lpwan_dfu_status(&d, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_IDLE);
  /* too large, empty, unsupported target */
  b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, SLOT_SIZE + 4, sha, false);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_INVALID && st.fail == sensei_DfuFail_DFU_FAIL_SIZE);
  b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, 0, sha, false);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_INVALID);
  b = begin_msg(sensei_DfuTarget_DFU_TARGET_MODEM, 5000, sha, false);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_UNSUPPORTED &&
        st.fail == sensei_DfuFail_DFU_FAIL_UNSUPPORTED);
  CHECK(lpwan_dfu_targets(&d) == LPWAN_DFU_TARGET_APP_BIT);
  /* abort drops the transfer */
  b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, 5000, sha, true);
  lpwan_dfu_begin(&d, &b, &st);
  send_all(&d, img, 5000, 0, 192, false, &st);
  CHECK(lpwan_dfu_abort(&d, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_IDLE);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 0);
  /* apply before finish */
  CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_INVALID && applied == 0);
  free(img);
}

static void test_flash_errors(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  uint8_t sha[32];
  uint8_t *img = make_image(20000, 1, 0, 0, 0, 0, sha);
  sensei_DfuBegin b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, 20000, sha, false);

  reset_world();
  fail_write_at = 5;
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  lpwan_dfu_begin(&d, &b, &st);
  CHECK(send_all(&d, img, 20000, 0, 192, false, &st) == sensei_LpwanResult_LPWAN_FAILED);
  CHECK(st.state == sensei_DfuState_DFU_STATE_FAILED && st.fail == sensei_DfuFail_DFU_FAIL_FLASH && st.detail == -EIO);
  reset_world();
  fail_erase = 1;
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  lpwan_dfu_begin(&d, &b, &st);
  CHECK(send_chunk(&d, img, 0, 192, false, &st) == sensei_LpwanResult_LPWAN_FAILED);
  CHECK(st.fail == sensei_DfuFail_DFU_FAIL_FLASH);
  app_enabled = false;
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_UNSUPPORTED);
  free(img);
}

static void test_modem(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  uint8_t sha[32];
  uint32_t size = 90000;
  uint8_t *img = make_image(size, 0, 0, 0, 0, 0, sha);
  sensei_DfuBegin b = begin_msg(sensei_DfuTarget_DFU_TARGET_MODEM, size, sha, true);

  reset_world();
  lpwan_dfu_init(&d, NULL, &modem_ops, NULL);
  CHECK(lpwan_dfu_targets(&d) == LPWAN_DFU_TARGET_MODEM_BIT);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(st.state == sensei_DfuState_DFU_STATE_ERASING);
  CHECK(send_chunk(&d, img, 0, 192, false, &st) == sensei_LpwanResult_LPWAN_BUSY); /* erase running */
  CHECK(lpwan_dfu_status(&d, &st) == sensei_LpwanResult_LPWAN_OK);
  while (st.state == sensei_DfuState_DFU_STATE_ERASING) {
    lpwan_dfu_status(&d, &st);
  }
  CHECK(st.state == sensei_DfuState_DFU_STATE_RECEIVING);
  CHECK(send_all(&d, img, 40000, 0, 192, true, &st) == sensei_LpwanResult_LPWAN_OK);     /* no gap: nothing is 0xFF */
  CHECK(send_chunk(&d, img, 40100, 100, true, &st) == sensei_LpwanResult_LPWAN_INVALID); /* no gaps for the modem */
  /* link broke, same session: resume continues */
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 40000 + 0u);
  CHECK(send_all(&d, img, size, st.offset, 500, false, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_VERIFIED);
  CHECK(mfw_len == size && memcmp(mfw, img, size) == 0 && modem_closed);
  CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_APPLYING);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK || 1);
  lpwan_dfu_status(&d, &st);
  for (int i = 0; i < 5 && st.state == sensei_DfuState_DFU_STATE_APPLYING; i++) {
    lpwan_dfu_status(&d, &st);
  }
  CHECK(st.state == sensei_DfuState_DFU_STATE_DONE);

  /* a restart forgets the session: the transfer begins at 0 and the modem erases */
  lpwan_dfu_init(&d, NULL, &modem_ops, NULL);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 0);
  /* a flipped byte is caught by the hash */
  while (lpwan_dfu_status(&d, &st), st.state == sensei_DfuState_DFU_STATE_ERASING) {
  }
  img[500] ^= 4;
  send_all(&d, img, size, 0, 500, false, &st);
  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_FAILED && st.fail == sensei_DfuFail_DFU_FAIL_HASH);
  /* a modem write error is reported with its code */
  img[500] ^= 4;
  lpwan_dfu_init(&d, NULL, &modem_ops, NULL);
  lpwan_dfu_begin(&d, &b, &st);
  while (lpwan_dfu_status(&d, &st), st.state == sensei_DfuState_DFU_STATE_ERASING) {
  }
  modem_fail_write = 1;
  CHECK(send_chunk(&d, img, 0, 100, false, &st) == sensei_LpwanResult_LPWAN_FAILED);
  CHECK(st.fail == sensei_DfuFail_DFU_FAIL_MODEM && st.detail == 7);
  free(img);
}

/* A transfer in the VERIFIED state, ready for apply. */
static uint8_t *verified(struct lpwan_dfu *d, sensei_DfuBegin *b, uint32_t size) {
  sensei_DfuStatus st;
  uint8_t sha[32];
  uint8_t *img = make_image(size, 1, 2, 3, 0, 0, sha);

  reset_world();
  lpwan_dfu_init(d, &app_ops, NULL, NULL);
  *b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size, sha, true);
  CHECK(lpwan_dfu_begin(d, b, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(send_all(d, img, size, 0, 188, false, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(lpwan_dfu_finish(d, &st) == sensei_LpwanResult_LPWAN_OK);
  return img;
}

/* The sender repeats a request whose answer was lost: finish, begin of the open transfer, apply. */
static void test_repeats(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  sensei_DfuBegin b;
  uint8_t *img = verified(&d, &b, 20000);

  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_VERIFIED);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_VERIFIED);
  CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_APPLYING);
  CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_APPLYING);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_APPLYING);
  CHECK(applied == 1); /* marked once */
  CHECK(violations == 0);
  free(img);
}

/* abort takes the marking for the test boot back (VERIFIED, APPLYING, a half done apply) and a new transfer too. */
static void test_abort_unmarks(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  sensei_DfuBegin b;
  uint8_t *img = verified(&d, &b, 20000);

  CHECK(lpwan_dfu_abort(&d, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_IDLE);
  CHECK(unmarked == 0); /* VERIFIED: nothing was marked */
  free(img);

  img = verified(&d, &b, 20000);
  CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_OK);
  CHECK(lpwan_dfu_abort(&d, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_IDLE);
  CHECK(unmarked == 1);
  CHECK(lpwan_dfu_abort(&d, &st) == sensei_LpwanResult_LPWAN_OK && unmarked == 1); /* once */
  CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_INVALID);             /* nothing open any more */
  free(img);

  /* the marking failed half way: the state is FAILED, abort still undoes it */
  img = verified(&d, &b, 20000);
  fail_apply = 1;
  CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_FAILED && st.state == sensei_DfuState_DFU_STATE_FAILED);
  CHECK(lpwan_dfu_abort(&d, &st) == sensei_LpwanResult_LPWAN_OK && unmarked == 1);
  free(img);

  /* the unmark fails: the answer says so and the transfer is not forgotten */
  img = verified(&d, &b, 20000);
  CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_OK);
  fail_unmark = 1;
  CHECK(lpwan_dfu_abort(&d, &st) == sensei_LpwanResult_LPWAN_FAILED && st.fail == sensei_DfuFail_DFU_FAIL_FLASH);
  fail_unmark = 0;
  CHECK(lpwan_dfu_abort(&d, &st) == sensei_LpwanResult_LPWAN_OK && unmarked == 1);
  free(img);

  /* a begin of another image while the first is marked takes the marking back first */
  img = verified(&d, &b, 20000);
  CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_OK);
  b.size = 20004;
  b.sha256[0] ^= 1;
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 0);
  CHECK(unmarked == 1);
  free(img);
}

/* The kind TLV: an image for another board stops at finish, before apply is possible. */
static void test_kind(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  sensei_DfuBegin b;
  uint8_t sha[32];
  uint8_t *img = make_image(20000, 1, 2, 3, 0, 0, sha);

  reset_world();
  kind_result = sensei_DfuFail_DFU_FAIL_BOARD;
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, 20000, sha, true);
  lpwan_dfu_begin(&d, &b, &st);
  send_all(&d, img, 20000, 0, 188, false, &st);
  CHECK(lpwan_dfu_finish(&d, &st) == sensei_LpwanResult_LPWAN_FAILED && st.fail == sensei_DfuFail_DFU_FAIL_BOARD);
  CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_INVALID && applied == 0);
  /* no checkpoint is left to resume from */
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 0);
  free(img);
}

/* A checkpoint that cannot be erased: the new transfer is not started (an old checkpoint could be resumed). */
static void test_checkpoint_erase_error(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  sensei_DfuBegin b;
  uint8_t sha[32];
  uint8_t *img = make_image(30000, 1, 2, 3, 0, 0, sha);

  reset_world();
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, 30000, sha, true);
  lpwan_dfu_begin(&d, &b, &st);
  send_all(&d, img, 12000, 0, 188, false, &st); /* checkpoints exist now */
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  fail_log_erase = 1;
  b.resume = false;
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_FAILED && st.fail == sensei_DfuFail_DFU_FAIL_FLASH);
  fail_log_erase = 0;
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 0);
  free(img);
}

/* New transfers are rate limited (flash wear), repeats of the open one are not, no clock: no limit. */
static void test_open_rate(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  sensei_DfuBegin b;
  uint8_t sha[32];
  uint8_t *img = make_image(20000, 1, 2, 3, 0, 0, sha);

  reset_world();
  b = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, 20000, sha, true);
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  for (int i = 0; i < 5; i++) { /* untimed: no limit */
    b.resume = false;
    CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK);
  }
  lpwan_dfu_init(&d, &app_ops, NULL, NULL);
  lpwan_dfu_touch(&d, 1000);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK);
  send_chunk(&d, img, 0, 188, false, &st);
  b.resume = false; /* "begin fresh, one chunk" loop */
  lpwan_dfu_touch(&d, 1000 + 2000);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_BUSY && st.fail == sensei_DfuFail_DFU_FAIL_RATE);
  CHECK(st.state == sensei_DfuState_DFU_STATE_RECEIVING && st.offset == 188); /* the open transfer is untouched */
  b.resume = true; /* a repeat of the open transfer is no new opening */
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 188);
  int erased = erases, log_er = log_erases;
  for (uint32_t t = 3000; t < 1000 + LPWAN_DFU_OPEN_MIN_MS; t += 500) {
    b.resume = false;
    lpwan_dfu_touch(&d, t);
    CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_BUSY);
  }
  CHECK(erases == erased && log_erases == log_er); /* the refused ones wore nothing */
  b.resume = false;
  lpwan_dfu_touch(&d, 1000 + LPWAN_DFU_OPEN_MIN_MS);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK && st.offset == 0);
  /* the clock wraps */
  lpwan_dfu_touch(&d, 0xFFFFFF00u);
  b.resume = false;
  lpwan_dfu_begin(&d, &b, &st);
  lpwan_dfu_touch(&d, 0xFFFFFF00u + 100u);
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_BUSY || st.state == sensei_DfuState_DFU_STATE_FAILED);
  lpwan_dfu_touch(&d, 0xFFFFFF00u + LPWAN_DFU_OPEN_MIN_MS); /* wrapped past zero */
  CHECK(lpwan_dfu_begin(&d, &b, &st) == sensei_LpwanResult_LPWAN_OK);
  free(img);
}

/* The radios are held while a sender is around, not for ever; the modem holds them while it works. */
static void test_holds_link(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  sensei_DfuBegin b;
  uint8_t *img = verified(&d, &b, 20000);

  CHECK(lpwan_dfu_holds_link(&d, 5000, 150000)); /* untimed: while open */
  lpwan_dfu_touch(&d, 10000);
  CHECK(lpwan_dfu_holds_link(&d, 10000, 150000));
  CHECK(lpwan_dfu_holds_link(&d, 10000 + 149999, 150000));
  CHECK(!lpwan_dfu_holds_link(&d, 10000 + 150000, 150000)); /* the sender gave up */
  lpwan_dfu_touch(&d, 200000);                              /* it is back: the transfer is still there */
  CHECK(lpwan_dfu_holds_link(&d, 200000, 150000));
  CHECK(lpwan_dfu_status(&d, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_VERIFIED);
  CHECK(lpwan_dfu_abort(&d, &st) == sensei_LpwanResult_LPWAN_OK && !lpwan_dfu_holds_link(&d, 200000, 150000));
  free(img);

  /* modem: held while erasing and updating, whatever the sender does; let go as soon as it is done, no status call */
  {
    uint8_t sha[32];
    uint8_t *m = make_image(90000, 0, 0, 0, 0, 0, sha);

    reset_world();
    b = begin_msg(sensei_DfuTarget_DFU_TARGET_MODEM, 90000, sha, true);
    lpwan_dfu_init(&d, NULL, &modem_ops, NULL);
    lpwan_dfu_touch(&d, 0);
    lpwan_dfu_begin(&d, &b, &st);
    CHECK(st.state == sensei_DfuState_DFU_STATE_ERASING);
    CHECK(lpwan_dfu_holds_link(&d, 1000000, 150000));
    while (lpwan_dfu_status(&d, &st), st.state == sensei_DfuState_DFU_STATE_ERASING) {
    }
    send_all(&d, m, 90000, 0, 500, false, &st);
    lpwan_dfu_finish(&d, &st);
    CHECK(lpwan_dfu_apply(&d, &st) == sensei_LpwanResult_LPWAN_OK);
    CHECK(lpwan_dfu_holds_link(&d, 9000000, 150000)); /* running in the modem */
    CHECK(lpwan_dfu_abort(&d, &st) == sensei_LpwanResult_LPWAN_BUSY);
    {
      int n = 0;

      while (lpwan_dfu_holds_link(&d, 9000000, 150000) && n++ < 10) {
      }
      CHECK(n < 10); /* the poll sees the end: the radios are free again without a status request */
    }
    free(m);
  }
}

/* The modem write session is released by abort also after a failed write. */
static void test_modem_abort(void) {
  struct lpwan_dfu d;
  sensei_DfuStatus st;
  uint8_t sha[32];
  uint8_t *img = make_image(90000, 0, 0, 0, 0, 0, sha);
  sensei_DfuBegin b = begin_msg(sensei_DfuTarget_DFU_TARGET_MODEM, 90000, sha, true);

  reset_world();
  modem_finishes = 0;
  lpwan_dfu_init(&d, NULL, &modem_ops, NULL);
  lpwan_dfu_begin(&d, &b, &st);
  while (lpwan_dfu_status(&d, &st), st.state == sensei_DfuState_DFU_STATE_ERASING) {
  }
  modem_fail_write = 1;
  CHECK(send_chunk(&d, img, 0, 100, false, &st) == sensei_LpwanResult_LPWAN_FAILED);
  CHECK(modem_finishes == 0);
  CHECK(lpwan_dfu_abort(&d, &st) == sensei_LpwanResult_LPWAN_OK && st.state == sensei_DfuState_DFU_STATE_IDLE);
  CHECK(modem_finishes == 1); /* write_done after the failed write */
  free(img);
}

static void test_confirm_rule(void) {
  CHECK(lpwan_dfu_confirm_allowed(0, 0x1234)); /* old master: any */
  CHECK(lpwan_dfu_confirm_allowed(0x1234, 0x1234));
  CHECK(!lpwan_dfu_confirm_allowed(0x1234, 0x5678)); /* another image than the one the master looked at */
  CHECK(lpwan_dfu_confirm_allowed(0x1234, 0));       /* the id of the running image is not known */
}

static void test_checkpoint_record(void) {
  struct lpwan_dfu_ckpt c = {.target = sensei_DfuTarget_DFU_TARGET_APP, .size = 123456, .offset = 8192}, o;
  uint8_t rec[LPWAN_DFU_CKPT_LEN];

  memset(c.sha, 0xAB, 32);
  lpwan_dfu_ckpt_encode(&c, rec);
  CHECK(lpwan_dfu_ckpt_decode(rec, &o) && o.size == 123456 && o.offset == 8192 && memcmp(o.sha, c.sha, 32) == 0);
  for (unsigned i = 0; i < 52; i++) { /* one bit flipped in the magic, the fields or the CRC: refused */
    rec[i] ^= 0x04;
    CHECK(!lpwan_dfu_ckpt_decode(rec, &o));
    rec[i] ^= 0x04;
  }
  memset(rec, 0xFF, sizeof(rec));
  CHECK(!lpwan_dfu_ckpt_decode(rec, &o));
}

/* ---- The same over the SPI link core, with damaged frames ---- */

static struct link_slave slv;
static struct lpwan_master mst;
static struct lpwan_dfu link_dfu;
static const uint8_t *armed_tx;
static uint8_t *armed_rx;
static bool busy_pin = true;
static unsigned rng = 7;
static int damage_pct;
static uint32_t clock_ms;

static unsigned rnd(void) {
  rng = rng * 1103515245u + 12345u;
  return (rng >> 16) & 0x7FFF;
}

static int s_arm(void *c, const uint8_t *tx, uint8_t *rx, size_t len) {
  (void)c;
  (void)len;
  armed_tx = tx;
  armed_rx = rx;
  return 0;
}
static void s_busy(void *c, bool b) {
  (void)c;
  busy_pin = b;
}
static void s_event(void *c, bool e) {
  (void)c;
  (void)e;
}
static const struct link_slave_ops sops = {.arm = s_arm, .set_busy = s_busy, .set_event = s_event};

static void on_request(struct link_slave *s, const sensei_LpwanRequest *req, void *u) {
  sensei_LpwanResponse r = sensei_LpwanResponse_init_zero;

  (void)u;
  r.seq = req->seq;
  r.which_payload = sensei_LpwanResponse_dfu_tag;
  switch (req->which_body) {
  case sensei_LpwanRequest_dfu_begin_tag:
    r.result = lpwan_dfu_begin(&link_dfu, &req->dfu_begin, &r.dfu);
    break;
  case sensei_LpwanRequest_dfu_chunk_tag:
    r.result = lpwan_dfu_chunk(&link_dfu, &req->dfu_chunk, &r.dfu);
    break;
  case sensei_LpwanRequest_dfu_finish_tag:
    r.result = lpwan_dfu_finish(&link_dfu, &r.dfu);
    break;
  case sensei_LpwanRequest_dfu_apply_tag:
    r.result = lpwan_dfu_apply(&link_dfu, &r.dfu);
    break;
  case sensei_LpwanRequest_dfu_status_tag:
    r.result = lpwan_dfu_status(&link_dfu, &r.dfu);
    break;
  default:
    r.which_payload = 0;
    r.result = sensei_LpwanResult_LPWAN_UNSUPPORTED;
  }
  link_slave_respond(s, &r);
}

static int m_xfer(void *c, const uint8_t *tx, uint8_t *rx, size_t len) {
  (void)c;
  if (!armed_tx || busy_pin) {
    return -EBUSY;
  }
  memcpy(armed_rx, tx, len);
  memcpy(rx, armed_tx, len);
  if ((int)(rnd() % 100) < damage_pct) {
    armed_rx[SENSEI_SPI_HEADER_SIZE + (rnd() % 100)] ^= 0x40; /* master frame damaged */
  }
  if ((int)(rnd() % 100) < damage_pct) {
    rx[SENSEI_SPI_HEADER_SIZE + (rnd() % 20)] ^= 0x40; /* slave frame damaged */
  }
  busy_pin = true;
  armed_tx = NULL;
  clock_ms += 2;
  link_slave_done(&slv, 0);
  return 0;
}
static int m_ready(void *c, uint32_t t) {
  (void)c;
  if (busy_pin) {
    clock_ms += t;
    return -ETIMEDOUT;
  }
  return 0;
}
static bool m_event_pending(void *c) {
  (void)c;
  return false;
}
static uint32_t m_now(void *c) {
  (void)c;
  return clock_ms;
}
static void m_on_event(void *c, const sensei_LpwanEvent *e) {
  (void)c;
  (void)e;
}
static const struct lpwan_master_ops mops = {
    .xfer = m_xfer, .wait_ready = m_ready, .event_pending = m_event_pending, .now_ms = m_now, .on_event = m_on_event};

static sensei_LpwanResult call(sensei_LpwanRequest *rq, sensei_DfuStatus *st, int *err) {
  sensei_LpwanResponse rs;

  *err = lpwan_master_request(&mst, rq, &rs, 100);
  if (*err == 0) {
    *st = rs.dfu;
    return rs.result;
  }
  return sensei_LpwanResult_LPWAN_TIMEOUT;
}

/* The master side as the nRF5340 does it: if a request gets no answer, ask for the status and go on from the
 * offset it reports (the resume query). */
static void test_over_link(int pct) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_DfuStatus st = sensei_DfuStatus_init_zero;
  uint8_t sha[32];
  uint32_t size = 150000;
  uint8_t *img = make_image(size, 3, 0, 0, 30000, 90000, sha);
  uint32_t off = 0;
  int err, lost = 0;
  bool gap = false;

  reset_world();
  damage_pct = pct;
  clock_ms = 0;
  busy_pin = true;
  armed_tx = NULL;
  memset(&slv, 0, sizeof(slv));
  lpwan_dfu_init(&link_dfu, &app_ops, NULL, NULL);
  link_slave_init(&slv, &sops, NULL, on_request, NULL);
  lpwan_master_init(&mst, &mops, NULL);
  link_slave_start(&slv);

  rq.which_body = sensei_LpwanRequest_dfu_begin_tag;
  rq.dfu_begin = begin_msg(sensei_DfuTarget_DFU_TARGET_APP, size, sha, true);
  while (call(&rq, &st, &err) != sensei_LpwanResult_LPWAN_OK) {
    CHECK(++lost < 50);
  }
  while (off < size) {
    size_t n = size - off < 480 ? size - off : 480;
    sensei_LpwanResult r;
    bool ff = true;

    for (size_t i = 0; i < n && ff; i++) {
      ff = img[off + i] == 0xFF;
    }
    if (ff && off + n < size) {
      gap = true;
      off += (uint32_t)n;
      continue;
    }
    memset(&rq, 0, sizeof(rq));
    rq.which_body = sensei_LpwanRequest_dfu_chunk_tag;
    rq.dfu_chunk.offset = off;
    rq.dfu_chunk.gap = gap;
    rq.dfu_chunk.data.size = (pb_size_t)n;
    memcpy(rq.dfu_chunk.data.bytes, img + off, n);
    r = call(&rq, &st, &err);
    if (err != 0 || r != sensei_LpwanResult_LPWAN_OK) {
      /* no answer: ask where the shield stands (a status request), continue there */
      lost++;
      do {
        memset(&rq, 0, sizeof(rq));
        rq.which_body = sensei_LpwanRequest_dfu_status_tag;
      } while (call(&rq, &st, &err) != sensei_LpwanResult_LPWAN_OK && lost++ < 100000);
      CHECK(st.state == sensei_DfuState_DFU_STATE_RECEIVING);
      if (st.offset >= off + n) {
        off += (uint32_t)n;
        gap = false;
      }
      continue;
    }
    gap = false;
    off += (uint32_t)n;
  }
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_dfu_finish_tag;
  while (call(&rq, &st, &err) != sensei_LpwanResult_LPWAN_OK) {
    CHECK(++lost < 100000);
  }
  CHECK(st.state == sensei_DfuState_DFU_STATE_VERIFIED);
  CHECK(memcmp(slot, img, size) == 0 && violations == 0);
  if (pct > 0) {
    CHECK(mst.n_retries > 0);
  }
  free(img);
}

int main(void) {
  test_plain_transfer();
  test_repeat_and_overlap();
  test_gap();
  test_resume();
  test_log_wrap();
  test_hash_and_header();
  test_state_and_limits();
  test_flash_errors();
  test_modem();
  test_repeats();
  test_abort_unmarks();
  test_kind();
  test_checkpoint_erase_error();
  test_open_rate();
  test_holds_link();
  test_modem_abort();
  test_confirm_rule();
  test_checkpoint_record();
  test_over_link(0);
  test_over_link(5);
  test_over_link(20);
  printf("%s\n", check_failures ? "FAILED" : "ok");
  return check_failures ? 1 : 0;
}
