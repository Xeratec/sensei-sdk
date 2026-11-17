/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_lpwan/lpwan_dfu.h"

#include <string.h>

#include "sensei_link/crc.h"

#define CKPT_MAGIC 0x31554644u /* "DFU1" */
#define LOG_SLOTS (LPWAN_DFU_SECTOR / LPWAN_DFU_CKPT_LEN)
#define MCUBOOT_MAGIC 0x96f3b83du
#define READ_BLOCK 128u

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* ---- Pure helpers ---- */

bool lpwan_dfu_header_version(const uint8_t hdr[32], uint8_t *major, uint8_t *minor, uint16_t *revision) {
  if (rd32(hdr) != MCUBOOT_MAGIC) {
    return false;
  }
  *major = hdr[20];
  *minor = hdr[21];
  *revision = (uint16_t)(hdr[22] | (hdr[23] << 8));
  return true;
}

sensei_DfuFail lpwan_dfu_check_header(const uint8_t new_hdr[32], uint32_t size, const uint8_t running_hdr[32]) {
  uint8_t nmaj, nmin, rmaj, rmin;
  uint16_t nrev, rrev;
  uint32_t hdr_size, protect, img_size;

  if (!lpwan_dfu_header_version(new_hdr, &nmaj, &nmin, &nrev)) {
    return sensei_DfuFail_DFU_FAIL_IMAGE;
  }
  hdr_size = (uint32_t)(new_hdr[8] | (new_hdr[9] << 8));
  protect = (uint32_t)(new_hdr[10] | (new_hdr[11] << 8));
  img_size = rd32(new_hdr + 12);
  /* header, image, protected TLVs and at least the TLV info header (4 B) must fit the file */
  if (hdr_size == 0 || img_size == 0 || (uint64_t)hdr_size + img_size + protect + 4u > size) {
    return sensei_DfuFail_DFU_FAIL_IMAGE;
  }
  if (lpwan_dfu_header_version(running_hdr, &rmaj, &rmin, &rrev)) {
    uint32_t n = ((uint32_t)nmaj << 24) | ((uint32_t)nmin << 16) | nrev; /* revision is 16 bit: fits */
    uint32_t r = ((uint32_t)rmaj << 24) | ((uint32_t)rmin << 16) | rrev;

    if (n < r) {
      return sensei_DfuFail_DFU_FAIL_DOWNGRADE;
    }
  }
  return sensei_DfuFail_DFU_FAIL_NONE;
}

void lpwan_dfu_ckpt_encode(const struct lpwan_dfu_ckpt *c, uint8_t out[LPWAN_DFU_CKPT_LEN]) {
  memset(out, 0, LPWAN_DFU_CKPT_LEN);
  wr32(out, CKPT_MAGIC);
  out[4] = (uint8_t)c->target;
  wr32(out + 8, c->size);
  wr32(out + 12, c->offset);
  memcpy(out + 16, c->sha, 32);
  wr32(out + 48, sensei_crc32(out, 48));
}

bool lpwan_dfu_ckpt_decode(const uint8_t in[LPWAN_DFU_CKPT_LEN], struct lpwan_dfu_ckpt *c) {
  if (rd32(in) != CKPT_MAGIC || rd32(in + 48) != sensei_crc32(in, 48)) {
    return false;
  }
  c->target = (sensei_DfuTarget)in[4];
  c->size = rd32(in + 8);
  c->offset = rd32(in + 12);
  memcpy(c->sha, in + 16, 32);
  return true;
}

/* ---- Checkpoint log: records appended in order, newest last, one erase unit ---- */

static bool slot_empty(const uint8_t *r) {
  for (unsigned i = 0; i < LPWAN_DFU_CKPT_LEN; i++) {
    if (r[i] != 0xFF) {
      return false;
    }
  }
  return true;
}

/* Find the newest valid record (if any) and the first free slot. */
static bool log_scan(struct lpwan_dfu *d, struct lpwan_dfu_ckpt *out) {
  const struct lpwan_dfu_ops *o = d->ops;
  uint8_t rec[LPWAN_DFU_CKPT_LEN];
  bool have = false;
  int next = LOG_SLOTS;

  if (o->log_read == NULL) {
    d->log_next = LOG_SLOTS;
    return false;
  }
  for (int i = 0; i < (int)LOG_SLOTS; i++) {
    if (o->log_read(d->ctx, (uint32_t)i * LPWAN_DFU_CKPT_LEN, rec, sizeof(rec)) != 0) {
      break; /* unreadable: treat the log as full, the next save erases it */
    }
    if (slot_empty(rec)) {
      next = i;
      break;
    }
    if (lpwan_dfu_ckpt_decode(rec, out)) {
      have = true; /* later records replace earlier ones */
    }
  }
  d->log_next = next;
  return have;
}

static void ckpt_clear(struct lpwan_dfu *d) {
  if (d->ops == NULL || d->ops->log_erase == NULL) {
    return;
  }
  if (d->log_next < 0) {
    struct lpwan_dfu_ckpt dummy;

    (void)log_scan(d, &dummy);
  }
  if (d->log_next != 0) {
    (void)d->ops->log_erase(d->ctx);
    d->log_next = 0;
  }
}

static void ckpt_save(struct lpwan_dfu *d, uint32_t offset) {
  struct lpwan_dfu_ckpt c;
  uint8_t rec[LPWAN_DFU_CKPT_LEN];
  const struct lpwan_dfu_ops *o = d->ops;

  if (o->log_write == NULL || o->log_erase == NULL) {
    return;
  }
  if (d->log_next < 0) {
    (void)log_scan(d, &c);
  }
  if (d->log_next >= (int)LOG_SLOTS) {
    if (o->log_erase(d->ctx) != 0) {
      return;
    }
    d->log_next = 0;
  }
  c.target = d->target;
  c.size = d->size;
  c.offset = offset;
  memcpy(c.sha, d->sha, 32);
  lpwan_dfu_ckpt_encode(&c, rec);
  if (o->log_write(d->ctx, (uint32_t)d->log_next * LPWAN_DFU_CKPT_LEN, rec, sizeof(rec)) == 0) {
    d->log_next++;
    d->ckpt_off = offset;
  }
}

/* ---- Core ---- */

void lpwan_dfu_init(struct lpwan_dfu *d, const struct lpwan_dfu_ops *app, const struct lpwan_dfu_ops *modem,
                    void *ctx) {
  memset(d, 0, sizeof(*d));
  d->app = app;
  d->modem = modem;
  d->ctx = ctx;
  d->state = sensei_DfuState_DFU_STATE_IDLE;
  d->log_next = -1;
}

uint32_t lpwan_dfu_targets(struct lpwan_dfu *d) {
  uint32_t t = 0;

  if (d->app != NULL && d->app->capacity != NULL && d->app->capacity(d->ctx, sensei_DfuTarget_DFU_TARGET_APP) != 0) {
    t |= LPWAN_DFU_TARGET_APP_BIT;
  }
  if (d->modem != NULL && d->modem->capacity != NULL &&
      d->modem->capacity(d->ctx, sensei_DfuTarget_DFU_TARGET_MODEM) != 0) {
    t |= LPWAN_DFU_TARGET_MODEM_BIT;
  }
  return t;
}

static void set_fail(struct lpwan_dfu *d, sensei_DfuFail fail, int detail) {
  d->state = sensei_DfuState_DFU_STATE_FAILED;
  d->fail = fail;
  d->detail = detail;
}

/* Modem: the erase may have finished, the update may have ended. */
static void refresh(struct lpwan_dfu *d) {
  const struct lpwan_dfu_ops *o = d->ops;

  if (o == NULL) {
    return; /* nothing was opened yet */
  }
  if (d->state == sensei_DfuState_DFU_STATE_ERASING) {
    int b = o->busy != NULL ? o->busy(d->ctx) : 0;

    if (b < 0) {
      set_fail(d, sensei_DfuFail_DFU_FAIL_MODEM, b);
    } else if (b == 0) {
      d->state = sensei_DfuState_DFU_STATE_RECEIVING;
    }
  } else if (d->state == sensei_DfuState_DFU_STATE_APPLYING && d->target == sensei_DfuTarget_DFU_TARGET_MODEM &&
             o->apply_state != NULL) {
    int detail = 0;
    int r = o->apply_state(d->ctx, &detail);

    if (r > 0) {
      d->state = sensei_DfuState_DFU_STATE_DONE;
    } else if (r < 0) {
      set_fail(d, sensei_DfuFail_DFU_FAIL_MODEM, detail);
    }
  }
}

static void fill(const struct lpwan_dfu *d, sensei_DfuStatus *st) {
  memset(st, 0, sizeof(*st));
  st->state = d->state;
  st->target = d->target;
  st->size = d->size;
  st->offset = d->offset;
  st->chunk_max = LPWAN_DFU_CHUNK_MAX;
  st->fail = d->fail;
  st->detail = d->detail;
}

/* Refuse a request: the transfer stays as it is, the status tells why (fail) and where it stands. */
static sensei_LpwanResult reject(const struct lpwan_dfu *d, sensei_DfuStatus *st, sensei_DfuFail why,
                                 sensei_LpwanResult res) {
  fill(d, st);
  st->fail = why;
  return res;
}

static sensei_LpwanResult done(const struct lpwan_dfu *d, sensei_DfuStatus *st) {
  fill(d, st);
  return d->state == sensei_DfuState_DFU_STATE_FAILED ? sensei_LpwanResult_LPWAN_FAILED : sensei_LpwanResult_LPWAN_OK;
}

static bool same_transfer(const struct lpwan_dfu *d, const sensei_DfuBegin *r) {
  return d->state != sensei_DfuState_DFU_STATE_IDLE && d->state != sensei_DfuState_DFU_STATE_FAILED &&
         d->target == r->target && d->size == r->size && memcmp(d->sha, r->sha256, 32) == 0;
}

sensei_LpwanResult lpwan_dfu_begin(struct lpwan_dfu *d, const sensei_DfuBegin *req, sensei_DfuStatus *st) {
  const struct lpwan_dfu_ops *o = req->target == sensei_DfuTarget_DFU_TARGET_MODEM ? d->modem : d->app;
  uint32_t cap = o != NULL && o->capacity != NULL ? o->capacity(d->ctx, req->target) : 0;
  uint32_t from = 0;
  bool resumed = false;
  int e;

  refresh(d);
  if (cap == 0) {
    return reject(d, st, sensei_DfuFail_DFU_FAIL_UNSUPPORTED, sensei_LpwanResult_LPWAN_UNSUPPORTED);
  }
  if (req->size == 0 || req->size > cap) {
    return reject(d, st, sensei_DfuFail_DFU_FAIL_SIZE, sensei_LpwanResult_LPWAN_INVALID);
  }
  if (d->state == sensei_DfuState_DFU_STATE_APPLYING && d->target == sensei_DfuTarget_DFU_TARGET_MODEM) {
    return reject(d, st, sensei_DfuFail_DFU_FAIL_STATE, sensei_LpwanResult_LPWAN_BUSY); /* the modem is updating */
  }
  if (req->resume && same_transfer(d, req)) {
    return done(d, st); /* the open transfer goes on where it is */
  }
  d->ops = o;
  if (req->resume && req->target == sensei_DfuTarget_DFU_TARGET_APP) {
    struct lpwan_dfu_ckpt c;

    if (log_scan(d, &c) && c.target == req->target && c.size == req->size && memcmp(c.sha, req->sha256, 32) == 0 &&
        c.offset < req->size && (c.offset % LPWAN_DFU_SECTOR) == 0) {
      from = c.offset;
      resumed = true;
    }
  }
  /* A new transfer, or one continued from its checkpoint. */
  d->target = req->target;
  d->size = req->size;
  memcpy(d->sha, req->sha256, 32);
  d->offset = from;
  d->erased_to = from;
  d->ntail = 0;
  d->ckpt_off = from;
  d->fail = sensei_DfuFail_DFU_FAIL_NONE;
  d->detail = 0;
  sensei_sha256_init(&d->hash);
  if (!resumed) {
    ckpt_clear(d);
  }
  e = o->open != NULL ? o->open(d->ctx, req->target, req->size, !resumed) : 0;
  if (e < 0) {
    set_fail(d,
             req->target == sensei_DfuTarget_DFU_TARGET_APP ? sensei_DfuFail_DFU_FAIL_FLASH
                                                            : sensei_DfuFail_DFU_FAIL_MODEM,
             e);
    return done(d, st);
  }
  d->state = sensei_DfuState_DFU_STATE_RECEIVING;
  if (o->busy != NULL && o->busy(d->ctx) > 0) {
    d->state = sensei_DfuState_DFU_STATE_ERASING;
  }
  return done(d, st);
}

/* Erase the sectors up to the one that holds byte `end - 1`, one at a time. */
static int erase_to(struct lpwan_dfu *d, uint32_t end, uint32_t cap) {
  uint32_t need = (end + LPWAN_DFU_SECTOR - 1) / LPWAN_DFU_SECTOR * LPWAN_DFU_SECTOR;

  if (need > cap) {
    need = cap;
  }
  while (d->erased_to < need) {
    int e = d->ops->erase(d->ctx, d->erased_to, LPWAN_DFU_SECTOR);

    if (e != 0) {
      return e;
    }
    d->erased_to += LPWAN_DFU_SECTOR;
    if (d->ops->kick != NULL) {
      d->ops->kick(d->ctx);
    }
  }
  return 0;
}

/* Take `len` bytes that belong at the offset `off` (>= the wanted one; the bytes in between are 0xFF). */
static int store(struct lpwan_dfu *d, uint32_t off, const uint8_t *p, size_t len) {
  const struct lpwan_dfu_ops *o = d->ops;
  size_t i = 0;
  int e;

  if (o->erase == NULL) { /* modem: the stream goes in as it comes */
    sensei_sha256_update(&d->hash, p, len);
    e = o->write(d->ctx, off, p, len);
    if (e == 0) {
      d->offset = off + (uint32_t)len;
    }
    return e;
  }
  e = erase_to(d, off + (uint32_t)len, o->capacity(d->ctx, d->target));
  if (e != 0) {
    return e;
  }
  if (off > d->offset) {
    d->offset = off; /* skipped bytes are erased, so they are in place */
  }
  while (d->ntail > 0 && i < len) {
    d->tail[d->ntail++] = p[i++];
    if (d->ntail == 4) {
      e = o->write(d->ctx, d->offset - 3u, d->tail, 4);
      if (e != 0) {
        return e;
      }
      d->ntail = 0;
    }
    d->offset++;
  }
  if (d->ntail == 0) {
    size_t whole = (len - i) & ~(size_t)3u;

    if (whole > 0) {
      e = o->write(d->ctx, d->offset, p + i, whole);
      if (e != 0) {
        return e;
      }
      i += whole;
      d->offset += (uint32_t)whole;
    }
    while (i < len) {
      d->tail[d->ntail++] = p[i++];
      d->offset++;
    }
  }
  {
    uint32_t flushed = (d->offset - d->ntail) / LPWAN_DFU_SECTOR * LPWAN_DFU_SECTOR; /* sectors that are final */

    if (flushed > d->ckpt_off) {
      ckpt_save(d, flushed);
    }
  }
  return 0;
}

sensei_LpwanResult lpwan_dfu_chunk(struct lpwan_dfu *d, const sensei_DfuChunk *req, sensei_DfuStatus *st) {
  const uint8_t *p = req->data.bytes;
  size_t len = req->data.size;
  uint32_t off = req->offset;
  int e;

  refresh(d);
  if (d->state == sensei_DfuState_DFU_STATE_ERASING) {
    return reject(d, st, sensei_DfuFail_DFU_FAIL_STATE, sensei_LpwanResult_LPWAN_BUSY);
  }
  if (d->state != sensei_DfuState_DFU_STATE_RECEIVING) {
    return reject(d, st, sensei_DfuFail_DFU_FAIL_STATE, sensei_LpwanResult_LPWAN_INVALID);
  }
  if (len == 0 || len > LPWAN_DFU_CHUNK_MAX || (uint64_t)off + len > d->size) {
    return reject(d, st, sensei_DfuFail_DFU_FAIL_SIZE, sensei_LpwanResult_LPWAN_INVALID);
  }
  if (off + len <= d->offset) {
    return done(d, st); /* a repeat (the answer was lost): already in place */
  }
  if (off < d->offset) { /* overlaps what is in: take the new part */
    size_t skip = d->offset - off;

    p += skip;
    len -= skip;
    off = d->offset;
  } else if (off > d->offset) {
    if (!req->gap || d->ops->erase == NULL || (off % 4u) != 0 || (d->offset % 4u) != 0) {
      return reject(d, st, sensei_DfuFail_DFU_FAIL_OFFSET, sensei_LpwanResult_LPWAN_INVALID);
    }
  }
  e = store(d, off, p, len);
  if (e != 0) {
    set_fail(
        d, d->target == sensei_DfuTarget_DFU_TARGET_APP ? sensei_DfuFail_DFU_FAIL_FLASH : sensei_DfuFail_DFU_FAIL_MODEM,
        e);
  }
  return done(d, st);
}

/* App: hash what is in the flash. */
static int hash_flash(struct lpwan_dfu *d, uint8_t out[32]) {
  uint8_t buf[READ_BLOCK];
  sensei_sha256_t h;

  sensei_sha256_init(&h);
  for (uint32_t pos = 0; pos < d->size; pos += READ_BLOCK) {
    size_t n = d->size - pos < READ_BLOCK ? d->size - pos : READ_BLOCK;
    int e = d->ops->read(d->ctx, pos, buf, n);

    if (e != 0) {
      return e;
    }
    sensei_sha256_update(&h, buf, n);
    if (d->ops->kick != NULL && (pos % (READ_BLOCK * 64u)) == 0) {
      d->ops->kick(d->ctx);
    }
  }
  sensei_sha256_final(&h, out);
  return 0;
}

sensei_LpwanResult lpwan_dfu_finish(struct lpwan_dfu *d, sensei_DfuStatus *st) {
  const struct lpwan_dfu_ops *o = d->ops;
  uint8_t sum[32];
  int e;

  refresh(d);
  if (d->state != sensei_DfuState_DFU_STATE_RECEIVING) {
    return reject(d, st, sensei_DfuFail_DFU_FAIL_STATE, sensei_LpwanResult_LPWAN_INVALID);
  }
  if (d->offset != d->size) {
    return reject(d, st, sensei_DfuFail_DFU_FAIL_OFFSET, sensei_LpwanResult_LPWAN_INVALID);
  }
  if (o->erase != NULL && d->ntail > 0) { /* the last bytes do not fill a word: pad with 0xFF */
    uint8_t w[4] = {0xFF, 0xFF, 0xFF, 0xFF};

    memcpy(w, d->tail, d->ntail);
    e = o->write(d->ctx, d->offset - d->ntail, w, 4);
    if (e != 0) {
      set_fail(d, sensei_DfuFail_DFU_FAIL_FLASH, e);
      return done(d, st);
    }
    d->ntail = 0;
  }
  if (o->read != NULL) {
    e = hash_flash(d, sum);
    if (e != 0) {
      set_fail(d, sensei_DfuFail_DFU_FAIL_FLASH, e);
      return done(d, st);
    }
  } else {
    sensei_sha256_t h = d->hash;

    sensei_sha256_final(&h, sum);
  }
  if (memcmp(sum, d->sha, 32) != 0) {
    ckpt_clear(d); /* what is in the flash is wrong: the next transfer starts at 0 */
    set_fail(d, sensei_DfuFail_DFU_FAIL_HASH, 0);
    return done(d, st);
  }
  if (d->target == sensei_DfuTarget_DFU_TARGET_APP && o->read_headers != NULL) {
    uint8_t nh[32], rh[32];
    sensei_DfuFail f;

    e = o->read_headers(d->ctx, nh, rh);
    if (e != 0) {
      set_fail(d, sensei_DfuFail_DFU_FAIL_FLASH, e);
      return done(d, st);
    }
    f = lpwan_dfu_check_header(nh, d->size, rh);
    if (f != sensei_DfuFail_DFU_FAIL_NONE) {
      ckpt_clear(d);
      set_fail(d, f, 0);
      return done(d, st);
    }
  }
  e = o->finish != NULL ? o->finish(d->ctx, d->target) : 0;
  if (e != 0) {
    set_fail(
        d, d->target == sensei_DfuTarget_DFU_TARGET_APP ? sensei_DfuFail_DFU_FAIL_FLASH : sensei_DfuFail_DFU_FAIL_MODEM,
        e);
    return done(d, st);
  }
  d->state = sensei_DfuState_DFU_STATE_VERIFIED;
  return done(d, st);
}

sensei_LpwanResult lpwan_dfu_apply(struct lpwan_dfu *d, sensei_DfuStatus *st) {
  int e;

  refresh(d);
  if (d->state != sensei_DfuState_DFU_STATE_VERIFIED) {
    return reject(d, st, sensei_DfuFail_DFU_FAIL_STATE, sensei_LpwanResult_LPWAN_INVALID);
  }
  e = d->ops->apply != NULL ? d->ops->apply(d->ctx, d->target) : -1;
  if (e != 0) {
    set_fail(
        d, d->target == sensei_DfuTarget_DFU_TARGET_APP ? sensei_DfuFail_DFU_FAIL_FLASH : sensei_DfuFail_DFU_FAIL_MODEM,
        e);
    return done(d, st);
  }
  d->state = sensei_DfuState_DFU_STATE_APPLYING;
  if (d->target == sensei_DfuTarget_DFU_TARGET_APP) {
    ckpt_clear(d); /* the image is marked: a later transfer must not resume it */
  }
  return done(d, st);
}

sensei_LpwanResult lpwan_dfu_abort(struct lpwan_dfu *d, sensei_DfuStatus *st) {
  if (d->state == sensei_DfuState_DFU_STATE_APPLYING && d->target == sensei_DfuTarget_DFU_TARGET_MODEM) {
    return reject(d, st, sensei_DfuFail_DFU_FAIL_STATE, sensei_LpwanResult_LPWAN_BUSY); /* running in the modem */
  }
  if (d->ops != NULL && d->ops->finish != NULL && d->target == sensei_DfuTarget_DFU_TARGET_MODEM &&
      d->state == sensei_DfuState_DFU_STATE_RECEIVING) {
    (void)d->ops->finish(d->ctx, d->target); /* modem: release its write session */
  }
  ckpt_clear(d);
  d->state = sensei_DfuState_DFU_STATE_IDLE;
  d->offset = 0;
  d->size = 0;
  d->erased_to = 0;
  d->ntail = 0;
  d->fail = sensei_DfuFail_DFU_FAIL_NONE;
  d->detail = 0;
  fill(d, st);
  return sensei_LpwanResult_LPWAN_OK;
}

sensei_LpwanResult lpwan_dfu_status(struct lpwan_dfu *d, sensei_DfuStatus *st) {
  refresh(d);
  fill(d, st);
  return sensei_LpwanResult_LPWAN_OK;
}
