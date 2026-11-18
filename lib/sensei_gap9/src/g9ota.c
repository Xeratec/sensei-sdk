/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_gap9/g9ota.h"

#include <errno.h>
#include <string.h>

#include "sensei_gap9/gap9_slot.h"
#include "sensei_link/crc.h"
#include "sensei_ota_image.h"

#define CKPT_MAGIC 0x31544F47u /* "GOT1" */
#define TLV_KEYHASH 0x01u
#define TLV_ECDSA_SIG 0x22u

static uint32_t rd32(const uint8_t *p) { return sensei_image_le32(p); }

static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* ---- Pure helpers ---- */

bool g9ota_header_version(const uint8_t hdr[32], uint32_t *packed) {
  if (rd32(hdr) != SENSEI_IMAGE_MAGIC) {
    return false;
  }
  *packed = ((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) | sensei_image_le16(hdr + 22);
  return true;
}

/* One DER INTEGER at *pos: a positive number of up to 32 bytes (a leading zero byte is allowed),
 * right aligned. */
static bool der_int(const uint8_t *der, size_t len, size_t *pos, uint8_t out[32]) {
  size_t p = *pos, n;

  if (p + 2 > len || der[p] != 0x02) {
    return false;
  }
  n = der[p + 1];
  p += 2;
  if (n == 0 || p + n > len || (der[p] & 0x80u) != 0) { /* empty, too long for the buffer, or negative */
    return false;
  }
  while (n > 1 && der[p] == 0) {
    p++;
    n--;
  }
  if (n > 32) {
    return false;
  }
  memset(out, 0, 32);
  memcpy(out + 32 - n, der + p, n);
  *pos = p + n;
  return true;
}

bool g9ota_sig_der_to_raw(const uint8_t *der, size_t len, uint8_t out[64]) {
  size_t pos = 2;

  if (len < 8 || len > 80 || der[0] != 0x30 || der[1] != len - 2u) {
    return false;
  }
  if (!der_int(der, len, &pos, out) || !der_int(der, len, &pos, out + 32)) {
    return false;
  }
  return pos == len;
}

void g9ota_ckpt_encode(const struct g9ota_ckpt *c, uint8_t out[G9OTA_CKPT_LEN]) {
  memset(out, 0, G9OTA_CKPT_LEN);
  wr32(out, CKPT_MAGIC);
  wr32(out + 4, c->seq);
  out[8] = c->state;
  out[9] = c->target;
  wr32(out + 12, c->size);
  wr32(out + 16, c->offset);
  memcpy(out + 20, c->sha, 32);
  memcpy(out + 52, c->hdr0, 16);
  wr32(out + G9OTA_CKPT_LEN - 4, sensei_crc32(out, G9OTA_CKPT_LEN - 4));
}

bool g9ota_ckpt_decode(const uint8_t in[G9OTA_CKPT_LEN], struct g9ota_ckpt *c) {
  if (rd32(in) != CKPT_MAGIC || rd32(in + G9OTA_CKPT_LEN - 4) != sensei_crc32(in, G9OTA_CKPT_LEN - 4)) {
    return false;
  }
  c->valid = true;
  c->seq = rd32(in + 4);
  c->state = in[8];
  c->target = in[9];
  c->size = rd32(in + 12);
  c->offset = rd32(in + 16);
  memcpy(c->sha, in + 20, 32);
  memcpy(c->hdr0, in + 52, 16);
  return c->state >= G9OTA_CK_OPEN && c->state <= G9OTA_CK_CHECKED && c->target < 2;
}

/* ---- Guarded MRAM access: the one place that writes ---- */

bool g9ota_addr_ok(const struct g9ota *o, uint32_t addr, uint32_t len) {
  uint64_t end = (uint64_t)addr + len;
  uint32_t t = o->cfg.slot_base[o->target];

  if (len == 0) {
    return false;
  }
  /* the slot that runs is never allowed, whatever `target` says */
  if (o->target != o->booted && addr >= t && end <= (uint64_t)t + o->cfg.slot_size) {
    return true;
  }
  for (int i = 0; i < 2; i++) {
    if (addr >= o->cfg.ckpt_base[i] && end <= (uint64_t)o->cfg.ckpt_base[i] + G9OTA_SECTOR) {
      return true;
    }
  }
  return false;
}

static int g_erase(struct g9ota *o, uint32_t addr) {
  if ((addr % G9OTA_SECTOR) != 0 || !g9ota_addr_ok(o, addr, G9OTA_SECTOR)) {
    o->writes_outside++;
    return -EACCES;
  }
  return o->fl->erase(o->ctx, addr);
}

static int g_program(struct g9ota *o, uint32_t addr, const void *data, uint32_t len) {
  if ((addr % 16u) != 0 || (len % 16u) != 0 || !g9ota_addr_ok(o, addr, len)) {
    o->writes_outside++;
    return -EACCES;
  }
  return o->fl->program(o->ctx, addr, data, len);
}

static int g_read(struct g9ota *o, uint32_t addr, void *buf, uint32_t len) {
  return o->fl->read(o->ctx, addr, buf, len);
}

/* ---- Slot reads: the bytes of a slot as the image parsers want them ---- */

struct slot_rd {
  const struct g9ota_flash *fl;
  void *ctx;
  uint32_t base;
  uint32_t limit;
  const uint8_t *hdr0; /* the slot is the target of the open transfer: its first 16 bytes are
                          these (see below) */
};

static struct slot_rd slot_rd_of(const struct g9ota *o, uint8_t slot, bool target) {
  return (struct slot_rd){o->fl, o->ctx, o->cfg.slot_base[slot & 1u], o->cfg.slot_size, target ? o->hdr0 : NULL};
}

/* The first MRAM word of the file (16 bytes, the MCUboot magic is in it) is not programmed while
 * the image comes in: a slot that is not complete never parses, whatever else is in it. It is
 * kept in the checkpoint records and written by `seal` (seal_header), after the nRF5340 checked the
 * signature. The reads of `finish` see the file as it is: those 16 bytes come from the record. */
// cppcheck-suppress constParameterCallback ; the signature is fixed by sensei_g9_read_fn
static int slot_read(void *c, uint32_t off, void *buf, size_t len) {
  const struct slot_rd *r = c;
  int e;

  if ((uint64_t)off + len > r->limit) {
    return -EINVAL;
  }
  e = r->fl->read(r->ctx, r->base + off, buf, (uint32_t)len);
  if (e == 0 && r->hdr0 != NULL && off < 16) {
    size_t n = 16 - off;

    memcpy(buf, r->hdr0 + off, n < len ? n : len);
  }
  return e;
}

/* The kind TLV of the image is the board id of the configuration. */
static bool kind_ok(const struct g9ota *o, struct slot_rd *r) {
  return o->cfg.kind != NULL && sensei_image_kind_is(slot_read, r, o->cfg.kind);
}

/* ---- Checkpoint log: records in two sectors that take turns, newest has the highest seq ---- */

static bool rec_empty(const uint8_t *r) {
  for (unsigned i = 0; i < G9OTA_CKPT_LEN; i++) {
    if (r[i] != 0xFF) {
      return false;
    }
  }
  return true;
}

/* Read both sectors: the newest valid record, and where the next record goes. */
static void ck_load(struct g9ota *o) {
  uint8_t rec[G9OTA_CKPT_LEN];
  uint32_t next[2] = {0, 0};
  bool read_ok[2] = {true, true};
  int newest = -1;

  memset(&o->ck, 0, sizeof(o->ck));
  for (int s = 0; s < 2; s++) {
    for (uint32_t i = 0; i < G9OTA_CKPT_PER_SECTOR; i++) {
      struct g9ota_ckpt c;

      if (g_read(o, o->cfg.ckpt_base[s] + i * G9OTA_CKPT_LEN, rec, sizeof(rec)) != 0) {
        read_ok[s] = false;
        next[s] = G9OTA_CKPT_PER_SECTOR; /* unreadable: the next record erases it */
        break;
      }
      if (rec_empty(rec)) {
        continue; /* a torn record may be followed by free ones: only the last non-empty
                     counts */
      }
      next[s] = i + 1;
      memset(&c, 0, sizeof(c));
      if (g9ota_ckpt_decode(rec, &c) && (!o->ck.valid || (int32_t)(c.seq - o->ck.seq) > 0)) {
        o->ck = c;
        newest = s;
      }
    }
  }
  o->ck_seq = o->ck.valid ? o->ck.seq : 0;
  if (newest >= 0 && next[newest] < G9OTA_CKPT_PER_SECTOR) {
    o->ck_sector = newest;
    o->ck_next = next[newest];
  } else if (newest >= 0) { /* the current sector is full: the other one takes over (erased first) */
    o->ck_sector = 1 - newest;
    o->ck_next = G9OTA_CKPT_PER_SECTOR;
  } else { /* no valid record: use a sector with room, else the first */
    o->ck_sector = next[0] < G9OTA_CKPT_PER_SECTOR ? 0 : (next[1] < G9OTA_CKPT_PER_SECTOR ? 1 : 0);
    o->ck_next = next[o->ck_sector];
  }
  (void)read_ok;
  o->ck_loaded = true;
}

/* Append a record. A full sector: erase the other one first; the old records stay valid until the
 * new one lands. */
static int ck_put(struct g9ota *o, uint8_t state, uint32_t offset) {
  struct g9ota_ckpt c;
  uint8_t rec[G9OTA_CKPT_LEN];
  int e;

  if (!o->ck_loaded) {
    ck_load(o);
  }
  if (o->ck_next >= G9OTA_CKPT_PER_SECTOR) {
    e = g_erase(o, o->cfg.ckpt_base[o->ck_sector]);
    if (e != 0) {
      return e;
    }
    o->ck_next = 0;
  }
  memset(&c, 0, sizeof(c));
  c.valid = true;
  c.state = state;
  c.target = o->target;
  c.seq = o->ck_seq + 1;
  c.size = o->size;
  c.offset = offset;
  memcpy(c.sha, o->sha, 32);
  memcpy(c.hdr0, o->hdr0, 16);
  g9ota_ckpt_encode(&c, rec);
  e = g_program(o, o->cfg.ckpt_base[o->ck_sector] + o->ck_next * G9OTA_CKPT_LEN, rec, sizeof(rec));
  if (e != 0) {
    o->ck_loaded = false; /* a torn record may be there: look again before the next one */
    return e;
  }
  o->ck = c;
  o->ck_seq = c.seq;
  o->ck_next++;
  if (o->ck_next >= G9OTA_CKPT_PER_SECTOR) { /* the next record goes to the other sector */
    o->ck_sector = 1 - o->ck_sector;
  }
  return 0;
}

/* ---- Core ---- */

int g9ota_arm(const struct g9ota_flash *fl, void *ctx, const struct g9ota_cfg *cfg, int handoff) {
  struct slot_rd r;
  struct sensei_g9_slot_info si;

  if (handoff < 0 || handoff > 1) {
    return -ENOENT;
  }
  r = (struct slot_rd){fl, ctx, cfg->slot_base[handoff], cfg->slot_size, NULL};
  return sensei_g9_slot_parse(slot_read, &r, cfg->slot_size, &si) == 0 ? handoff : -ENOENT;
}

int g9ota_cfg_check(const struct g9ota_cfg *cfg, uint8_t booted_slot) {
  uint32_t base[4];
  uint64_t len[4];

  if (cfg == NULL || booted_slot > 1 || cfg->slot_size == 0 || (cfg->slot_size % G9OTA_SECTOR) != 0) {
    return -EINVAL;
  }
  if (cfg->kind != NULL) {
    size_t n = 0;

    while (n <= SENSEI_IMAGE_KIND_MAX && cfg->kind[n] != '\0') {
      n++;
    }
    if (n > SENSEI_IMAGE_KIND_MAX) {
      return -EINVAL; /* sensei_image_kind_is() would match no image */
    }
  }
  base[0] = cfg->slot_base[0];
  base[1] = cfg->slot_base[1];
  base[2] = cfg->ckpt_base[0];
  base[3] = cfg->ckpt_base[1];
  len[0] = len[1] = cfg->slot_size;
  len[2] = len[3] = G9OTA_SECTOR;
  for (int i = 0; i < 4; i++) {
    if ((base[i] % G9OTA_SECTOR) != 0) {
      return -EINVAL;
    }
    for (int j = 0; j < i; j++) {
      /* the booted slot, the target and the two checkpoint sectors must not share a byte */
      if ((uint64_t)base[i] < (uint64_t)base[j] + len[j] && (uint64_t)base[j] < (uint64_t)base[i] + len[i]) {
        return -EINVAL;
      }
    }
  }
  return 0;
}

int g9ota_select(const struct g9ota_flash *fl, void *ctx, const struct g9ota_cfg *real,
                 const struct g9ota_cfg *test_area, bool test_allowed, int handoff, struct g9ota_cfg *out) {
  int slot;

  if (test_area != NULL) {
    /* The test area is for a build that runs from the single-image layout, where no SSBL left a record. Any record
     * proves that the app runs from a slot of the A/B layout: the area could lie on that slot or on the checkpoint
     * sectors. */
    if (!test_allowed || handoff >= 0) {
      return -EPERM;
    }
    *out = *test_area;
    out->kind = real->kind;
    return g9ota_cfg_check(out, 0) == 0 ? 0 : -EINVAL;
  }
  slot = g9ota_arm(fl, ctx, real, handoff);
  if (slot < 0) {
    return slot;
  }
  *out = *real;
  return g9ota_cfg_check(out, (uint8_t)slot) == 0 ? slot : -EINVAL;
}

int g9ota_init(struct g9ota *o, const struct g9ota_flash *fl, void *ctx, const struct g9ota_cfg *cfg,
               uint8_t booted_slot) {
  memset(o, 0, sizeof(*o));
  if (fl == NULL || fl->read == NULL || fl->erase == NULL || fl->program == NULL ||
      g9ota_cfg_check(cfg, booted_slot) != 0) {
    return -EINVAL; /* `o` stays zero: the caller must not use it */
  }
  o->fl = fl;
  o->ctx = ctx;
  o->cfg = *cfg;
  o->booted = booted_slot;
  o->target = 1u - o->booted;
  o->stage = sensei_Gap9OtaStage_GAP9_OTA_STAGE_IDLE;
  o->ck_sector = -1;
  return 0;
}

void g9ota_touch(struct g9ota *o, uint32_t now_ms) {
  o->timed = true;
  o->now_ms = now_ms;
}

static void set_fail(struct g9ota *o, sensei_Gap9OtaFail fail, int detail) {
  o->stage = sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED;
  o->fail = fail;
  o->detail = detail;
}

/* A transfer into the target is open (in this session, or a checkpoint says so): the slot is not a
 * valid image. */
static bool target_busy(struct g9ota *o, uint8_t slot) {
  if (slot != o->target) {
    return false;
  }
  if (o->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING ||
      o->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED) {
    return true;
  }
  if (!o->ck_loaded) {
    ck_load(o);
  }
  /* OPEN: bytes are coming. CHECKED: complete and checked, but not sealed (the nRF5340 has not accepted it yet). */
  return o->ck.valid && (o->ck.state == G9OTA_CK_OPEN || o->ck.state == G9OTA_CK_CHECKED) && o->ck.target == slot;
}

bool g9ota_slot_info(struct g9ota *o, uint8_t slot, sensei_Gap9Slot *out) {
  struct slot_rd r = slot_rd_of(o, slot, false);
  struct sensei_g9_slot_info si;
  uint8_t sha[32];

  memset(out, 0, sizeof(*out));
  if (target_busy(o, slot & 1u)) {
    return false;
  }
  if (sensei_g9_slot_parse(slot_read, &r, o->cfg.slot_size, &si) != 0) {
    return false;
  }
  if (!kind_ok(o, &r)) {
    return false;
  }
  out->valid = true;
  out->has_version = true;
  out->version.major = si.ver_major;
  out->version.minor = si.ver_minor;
  out->version.patch = si.ver_revision;
  if (sensei_image_tlv(slot_read, &r, SENSEI_IMAGE_TLV_SHA256, false, sha, sizeof(sha)) == 32) {
    out->image_id = rd32(sha);
  }
  return true;
}

static void fill_state(const struct g9ota *o, sensei_Gap9OtaInfo *info, sensei_Gap9OtaResult res) {
  memset(info, 0, sizeof(*info));
  info->result = res;
  info->stage = o->stage;
  info->fail = o->fail;
  info->detail = o->detail;
  info->offset = o->offset;
  info->size = o->size;
  info->chunk_max = G9OTA_CHUNK_MAX;
  info->booted_slot = o->booted;
  info->slot_size = o->cfg.slot_size;
  info->sealed = o->sealed;
}

static void fill_slots(struct g9ota *o, sensei_Gap9OtaInfo *info) {
  info->has_slot_a = true;
  (void)g9ota_slot_info(o, 0, &info->slot_a);
  info->has_slot_b = true;
  (void)g9ota_slot_info(o, 1, &info->slot_b);
}

/* Refuse a request: the transfer stays as it is, the answer says why. */
static sensei_Gap9OtaResult reject(struct g9ota *o, sensei_Gap9OtaInfo *info, sensei_Gap9OtaFail why,
                                   sensei_Gap9OtaResult res, bool slots) {
  fill_state(o, info, res);
  info->fail = why;
  if (slots) {
    fill_slots(o, info);
  }
  return res;
}

static sensei_Gap9OtaResult done(struct g9ota *o, sensei_Gap9OtaInfo *info, bool slots) {
  sensei_Gap9OtaResult res = o->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_FAILED
                                 ? sensei_Gap9OtaResult_GAP9_OTA_FAILED
                                 : sensei_Gap9OtaResult_GAP9_OTA_OK;

  fill_state(o, info, res);
  if (slots) {
    fill_slots(o, info);
  }
  if (o->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED) {
    info->image_hash.size = 32;
    memcpy(info->image_hash.bytes, o->image_hash, 32);
    info->signature.size = 64;
    memcpy(info->signature.bytes, o->signature, 64);
    if (o->have_key_hash) {
      info->key_hash.size = 32;
      memcpy(info->key_hash.bytes, o->key_hash, 32);
    }
    info->has_version = true;
    info->version = o->image_version;
  }
  return res;
}

sensei_Gap9OtaResult g9ota_status(struct g9ota *o, sensei_Gap9OtaInfo *info) { return done(o, info, true); }

static bool same_transfer(const struct g9ota *o, const sensei_Gap9OtaBegin *r) {
  return (o->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING ||
          o->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED) &&
         o->size == r->size && memcmp(o->sha, r->sha256, 32) == 0;
}

sensei_Gap9OtaResult g9ota_begin(struct g9ota *o, const sensei_Gap9OtaBegin *req, sensei_Gap9OtaInfo *info) {
  uint32_t from = 0;
  bool resumed = false;

  if (req->size == 0 || req->size > o->cfg.slot_size) {
    return reject(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIZE, sensei_Gap9OtaResult_GAP9_OTA_INVALID, true);
  }
  if (req->resume && same_transfer(o, req)) {
    return done(o, info, true); /* the open transfer goes on where it is */
  }
  if (!o->ck_loaded) {
    ck_load(o);
  }
  if (req->resume && o->ck.valid && o->ck.state != G9OTA_CK_CLEARED && o->ck.target == o->target &&
      o->ck.size == req->size && memcmp(o->ck.sha, req->sha256, 32) == 0 && o->ck.offset <= req->size &&
      (o->ck.offset % G9OTA_SECTOR == 0 || o->ck.offset == req->size)) {
    from = o->ck.offset;
    resumed = true;
  }
  if (!resumed && o->timed && o->opened && (uint32_t)(o->now_ms - o->open_ms) < G9OTA_OPEN_MIN_MS) {
    return reject(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_RATE, sensei_Gap9OtaResult_GAP9_OTA_BUSY, true);
  }
  o->size = req->size;
  memcpy(o->sha, req->sha256, 32);
  o->offset = from;
  o->flushed = from;
  o->nbuf = 0;
  o->fail = sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE;
  o->detail = 0;
  o->have_key_hash = false;
  o->sealed = false;
  memset(o->hdr0, 0, sizeof(o->hdr0));
  if (resumed && from > 0) {
    memcpy(o->hdr0, o->ck.hdr0, sizeof(o->hdr0)); /* the first sector is in, without these bytes */
  }
  if (!resumed) {
    /* The record comes first: from here on the slot counts as a transfer in progress, not as an
     * image. */
    o->opened = true;
    o->open_ms = o->now_ms;
    int e = ck_put(o, G9OTA_CK_OPEN, 0);

    if (e != 0) {
      set_fail(o, sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH, e);
      return done(o, info, true);
    }
  }
  if (from == 0) {
    /* The header sector goes first: nothing of the old image in this slot can parse any more,
     * and the new image has no magic until `finish` (flush_sector, seal_header). */
    int e = g_erase(o, o->cfg.slot_base[o->target]);

    if (e != 0) {
      set_fail(o, sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH, e);
      return done(o, info, true);
    }
  }
  o->stage = sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING;
  if (resumed && from == o->size && (o->ck.state == G9OTA_CK_CHECKED || o->ck.state == G9OTA_CK_DONE)) {
    /* The transfer was finished before the restart: check it again and answer VERIFIED with the proof, so that the
     * node can check the signature and seal without a new transfer. */
    return g9ota_finish(o, info);
  }
  return done(o, info, true);
}

/* Write the buffer into the sector at `flushed`: erase, program, read back, then the checkpoint. */
static int flush_sector(struct g9ota *o) {
  uint32_t addr = o->cfg.slot_base[o->target] + o->flushed;
  uint8_t chk[256];
  int e;

  if (o->nbuf < G9OTA_SECTOR) {
    memset(o->buf + o->nbuf, 0xFF, G9OTA_SECTOR - o->nbuf);
  }
  if (o->flushed == 0) {
    memcpy(o->hdr0, o->buf, sizeof(o->hdr0)); /* the checkpoint below keeps it */
    memset(o->buf, 0xFF, sizeof(o->hdr0));    /* left out until the image is complete and checked */
  }
  e = g_erase(o, addr);
  if (e == 0) {
    e = g_program(o, addr, o->buf, G9OTA_SECTOR);
  }
  for (uint32_t pos = 0; e == 0 && pos < G9OTA_SECTOR; pos += sizeof(chk)) {
    e = g_read(o, addr + pos, chk, sizeof(chk));
    if (e == 0 && memcmp(chk, o->buf + pos, sizeof(chk)) != 0) {
      e = -EIO;
    }
  }
  if (e != 0) {
    return e;
  }
  o->flushed += o->nbuf;
  o->nbuf = 0;
  return ck_put(o, G9OTA_CK_OPEN, o->flushed);
}

sensei_Gap9OtaResult g9ota_chunk(struct g9ota *o, const sensei_Gap9OtaData *req, sensei_Gap9OtaInfo *info) {
  const uint8_t *p = req->data.bytes;
  size_t len = req->data.size;
  const uint32_t off = req->offset;

  if (o->stage != sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING) {
    return reject(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE, sensei_Gap9OtaResult_GAP9_OTA_INVALID, false);
  }
  if (len == 0 || len > G9OTA_CHUNK_MAX || (uint64_t)off + len > o->size) {
    return reject(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIZE, sensei_Gap9OtaResult_GAP9_OTA_INVALID, false);
  }
  if (off + len <= o->offset) {
    return done(o, info, false); /* a repeat (the answer was lost): already in place */
  }
  if (off < o->offset) { /* overlaps what is in: take the new part */
    size_t skip = o->offset - off;

    p += skip;
    len -= skip;
  } else if (off > o->offset) {
    return reject(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_OFFSET, sensei_Gap9OtaResult_GAP9_OTA_INVALID, false);
  }
  while (len > 0) {
    size_t n = G9OTA_SECTOR - o->nbuf;

    if (n > len) {
      n = len;
    }
    memcpy(o->buf + o->nbuf, p, n);
    o->nbuf += (uint32_t)n;
    o->offset += (uint32_t)n;
    p += n;
    len -= n;
    if (o->nbuf == G9OTA_SECTOR || o->offset == o->size) {
      int e = flush_sector(o);
      if (e != 0) {
        set_fail(o, sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH, e);
        o->offset = o->flushed; /* what is in the slot; the buffer is gone */
        o->nbuf = 0;
        break;
      }
    }
  }
  return done(o, info, false);
}

/* Take a rejected or aborted image out of use: its header sector goes first, then the checkpoint.
 */
static void invalidate_target(struct g9ota *o) {
  if (o->flushed > 0) {
    (void)g_erase(o, o->cfg.slot_base[o->target]);
  }
  (void)ck_put(o, G9OTA_CK_CLEARED, 0);
  o->flushed = 0;
  o->nbuf = 0;
  o->offset = 0;
}

static sensei_Gap9OtaResult image_fail(struct g9ota *o, sensei_Gap9OtaInfo *info, sensei_Gap9OtaFail why, int detail) {
  invalidate_target(o);
  set_fail(o, why, detail);
  return done(o, info, true);
}

/* Length of what the image hash covers (header, image, protected TLVs), from the MCUboot header;
 * 0 if there is no header or it does not fit `size`. */
static uint32_t hashed_len(const uint8_t hdr[32], uint32_t size) {
  uint64_t pl;

  if (rd32(hdr) != SENSEI_IMAGE_MAGIC) {
    return 0;
  }
  pl = (uint64_t)sensei_image_le16(hdr + 8) + rd32(hdr + 12) + sensei_image_le16(hdr + 10);
  return pl <= size ? (uint32_t)pl : 0;
}

/* Read [0, len) of the slot a sector at a time (o->buf): SHA-256 of all of it into `all` (if not
 * NULL) and of the first `prefix_len` bytes into `prefix`. */
static int hash_slot(struct g9ota *o, const struct slot_rd *r, uint32_t len, uint32_t prefix_len, sensei_sha256_t *all,
                     sensei_sha256_t *prefix) {
  for (uint32_t pos = 0; pos < len; pos += G9OTA_SECTOR) {
    uint32_t m = len - pos < G9OTA_SECTOR ? len - pos : G9OTA_SECTOR;
    int e = slot_read((void *)r, pos, o->buf, m);

    if (e != 0) {
      return e;
    }
    if (all != NULL) {
      sensei_sha256_update(all, o->buf, m);
    }
    if (pos < prefix_len) {
      sensei_sha256_update(prefix, o->buf, prefix_len - pos < m ? prefix_len - pos : m);
    }
  }
  return 0;
}

/* What the node needs to check the signature. */
struct proof {
  uint8_t hash[32];
  uint8_t sig[64];
  uint8_t key_hash[32];
  bool have_key_hash;
};

/* The checks that make a slot an image of this board (the part of `finish` after the file hash,
 * and all of `verify`): container (the parser the SSBL trusts), kind, image hash TLV against the
 * bytes (`prefix` is their SHA-256), the app descriptor (what the SSBL would load), a signature of
 * the right shape. Fills `pf` for the node, which owns the key. Returns 0, or the failure and its
 * detail. */
static int check_image(const struct g9ota *o, struct slot_rd *r, uint32_t size, sensei_sha256_t *prefix,
                       struct sensei_g9_slot_info *si, struct proof *pf, sensei_Gap9OtaFail *why, int *detail) {
  uint8_t sum[32], tlv_hash[32], sig_der[80];
  int n;

  *detail = 0;
  if (sensei_g9_slot_parse(slot_read, r, size, si) != 0 || si->total_size != size) {
    *why = sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE;
    return -EINVAL;
  }
  r->limit = size;
  if (!kind_ok(o, r)) {
    *why = sensei_Gap9OtaFail_GAP9_OTA_FAIL_KIND;
    return -EINVAL;
  }
  /* The image hash TLV is the hash of what is in the slot (the signature covers this value). */
  sensei_sha256_final(prefix, sum);
  if (sensei_image_tlv(slot_read, r, SENSEI_IMAGE_TLV_SHA256, false, tlv_hash, sizeof(tlv_hash)) != 32 ||
      memcmp(tlv_hash, sum, 32) != 0) {
    *why = sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE;
    *detail = 1;
    return -EINVAL;
  }
  /* What the SSBL would load: no section over its own area, the hand-off record or the end of L2,
   * XIP, too many sections. The signature does not make a bad descriptor harmless. */
  if (sensei_g9_app_check(slot_read, r, si->app_off, si->app_size) != 0) {
    *why = sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE;
    *detail = 2;
    return -EINVAL;
  }
  /* A signature of the right shape is there: the node holds the key and checks it. */
  n = sensei_image_tlv(slot_read, r, TLV_ECDSA_SIG, false, sig_der, sizeof(sig_der));
  if (n < 0 || (size_t)n > sizeof(sig_der) || !g9ota_sig_der_to_raw(sig_der, (size_t)n, pf->sig)) {
    *why = sensei_Gap9OtaFail_GAP9_OTA_FAIL_SIGNATURE;
    return -EINVAL;
  }
  memcpy(pf->hash, tlv_hash, 32);
  pf->have_key_hash = sensei_image_tlv(slot_read, r, TLV_KEYHASH, false, pf->key_hash, sizeof(pf->key_hash)) == 32;
  return 0;
}

/* The last write of an image: the first MRAM word of the file, with the magic. The slot holds
 * everything else, checked, and the nRF5340 accepted its signature. The word is erased (FF): one aligned program of 16
 * bytes. After a cut in that program it is torn: then the sector is written again whole (erase, program). A cut in
 * between leaves a slot without a header (it never parses); a resume finds that `finish` fails the
 * hash and the host sends the image again from 0. */
static int seal_header(struct g9ota *o) {
  uint32_t addr = o->cfg.slot_base[o->target];
  uint8_t w[16], ff[16];
  uint8_t chk[256];
  int e = g_read(o, addr, w, sizeof(w));

  memset(ff, 0xFF, sizeof(ff));
  if (e != 0 || memcmp(w, o->hdr0, sizeof(w)) == 0) {
    return e; /* sealed before a cut */
  }
  if (memcmp(w, ff, sizeof(w)) == 0) {
    e = g_program(o, addr, o->hdr0, sizeof(o->hdr0));
    if (e == 0) {
      e = g_read(o, addr, w, sizeof(w));
    }
    return e != 0 ? e : (memcmp(w, o->hdr0, sizeof(w)) == 0 ? 0 : -EIO);
  }
  e = g_read(o, addr, o->buf, G9OTA_SECTOR); /* torn word: the whole sector again, the word last */
  if (e == 0) {
    memcpy(o->buf, o->hdr0, sizeof(o->hdr0));
    e = g_erase(o, addr);
  }
  if (e == 0) {
    e = g_program(o, addr + sizeof(o->hdr0), o->buf + sizeof(o->hdr0), G9OTA_SECTOR - sizeof(o->hdr0));
  }
  if (e == 0) {
    e = g_program(o, addr, o->buf, sizeof(o->hdr0));
  }
  for (uint32_t pos = 0; e == 0 && pos < G9OTA_SECTOR; pos += sizeof(chk)) {
    e = g_read(o, addr + pos, chk, sizeof(chk));
    if (e == 0 && memcmp(chk, o->buf + pos, sizeof(chk)) != 0) {
      e = -EIO;
    }
  }
  return e;
}

sensei_Gap9OtaResult g9ota_finish(struct g9ota *o, sensei_Gap9OtaInfo *info) {
  struct slot_rd r = slot_rd_of(o, o->target, true);
  struct sensei_g9_slot_info si;
  sensei_Gap9OtaFail why = sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE;
  sensei_sha256_t all, prefix;
  struct proof pf;
  uint8_t hdr[32], sum[32];
  uint32_t new_ver, run_ver, hl;
  int e, detail;

  if (o->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED) {
    return done(o, info, true); /* a repeat: the answer was lost */
  }
  if (o->stage != sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING) {
    return reject(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE, sensei_Gap9OtaResult_GAP9_OTA_INVALID, true);
  }
  if (o->offset != o->size) {
    return reject(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_OFFSET, sensei_Gap9OtaResult_GAP9_OTA_INVALID, true);
  }

  /* 1. The bytes in the slot are the file of `begin`. The MCUboot hash (header, image, protected
   * TLVs) is made on the way: its length comes from the header, which is the first thing read. */
  e = slot_read(&r, 0, hdr, sizeof(hdr));
  hl = hashed_len(hdr, o->size);
  if (e == 0) {
    sensei_sha256_init(&all);
    sensei_sha256_init(&prefix);
    e = hash_slot(o, &r, o->size, hl, &all, &prefix);
  }
  if (e != 0) {
    set_fail(o, sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH, e);
    return done(o, info, true);
  }
  sensei_sha256_final(&all, sum);
  if (memcmp(sum, o->sha, 32) != 0) {
    return image_fail(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_HASH, 0);
  }
  /* 2. to 4. Header, container, kind, image hash, app descriptor, signature. */
  if (hl == 0) {
    return image_fail(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE, 0);
  }
  if (check_image(o, &r, o->size, &prefix, &si, &pf, &why, &detail) != 0) {
    return image_fail(o, info, why, detail);
  }
  memcpy(o->image_hash, pf.hash, 32);
  memcpy(o->signature, pf.sig, 64);
  memcpy(o->key_hash, pf.key_hash, 32);
  o->have_key_hash = pf.have_key_hash;
  o->image_version = (sensei_Version){si.ver_major, si.ver_minor, si.ver_revision, 0};

  /* 5. Not older than the image that runs (the same version is allowed). The running slot may
   * hold no image (a development flash): then there is nothing to compare with. */
  new_ver = ((uint32_t)si.ver_major << 24) | ((uint32_t)si.ver_minor << 16) | si.ver_revision;
  {
    uint8_t bh[32];

    if (g_read(o, o->cfg.slot_base[o->booted], bh, sizeof(bh)) == 0 && g9ota_header_version(bh, &run_ver) &&
        new_ver < run_ver) {
      return image_fail(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_DOWNGRADE, 0);
    }
  }
  /* 6. Complete and checked: the record says so. The slot gets its magic only from `seal`, after the nRF5340 checked
   * the signature. A resume of a transfer that is sealed already stays sealed. */
  if (o->ck.valid && o->ck.state == G9OTA_CK_DONE && o->ck.target == o->target && o->ck.size == o->size &&
      memcmp(o->ck.sha, o->sha, 32) == 0) {
    o->sealed = true;
  } else {
    e = ck_put(o, G9OTA_CK_CHECKED, o->size);
    if (e != 0) {
      set_fail(o, sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH, e);
      return done(o, info, true);
    }
  }
  o->stage = sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED;
  return done(o, info, true);
}

sensei_Gap9OtaResult g9ota_seal(struct g9ota *o, const sensei_Gap9OtaSeal *req, sensei_Gap9OtaInfo *info) {
  int e;

  if (o->stage != sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED) {
    return reject(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE, sensei_Gap9OtaResult_GAP9_OTA_INVALID, true);
  }
  if (memcmp(req->image_hash, o->image_hash, 32) != 0) {
    /* not the image that finish answered: the nRF5340 accepted something else */
    return reject(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE, sensei_Gap9OtaResult_GAP9_OTA_INVALID, true);
  }
  if (o->sealed) {
    return done(o, info, true); /* a repeat: the answer was lost */
  }
  e = seal_header(o);
  if (e == 0) {
    e = ck_put(o, G9OTA_CK_DONE, o->size);
  }
  if (e != 0) {
    set_fail(o, sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH, e);
    return done(o, info, true);
  }
  o->sealed = true;
  return done(o, info, true);
}

/* An answer to `verify` that is not a pass: the state of the transfer is not touched. */
static sensei_Gap9OtaResult verify_fail(struct g9ota *o, sensei_Gap9OtaInfo *info, sensei_Gap9OtaFail why, int detail) {
  fill_state(o, info, sensei_Gap9OtaResult_GAP9_OTA_FAILED);
  info->fail = why;
  info->detail = detail;
  fill_slots(o, info);
  return sensei_Gap9OtaResult_GAP9_OTA_FAILED;
}

sensei_Gap9OtaResult g9ota_verify(struct g9ota *o, uint8_t slot, sensei_Gap9OtaInfo *info) {
  struct slot_rd r = slot_rd_of(o, slot, false);
  struct sensei_g9_slot_info si;
  sensei_Gap9OtaFail why = sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE;
  sensei_sha256_t prefix;
  struct proof pf;
  uint8_t hdr[32];
  uint32_t hl;
  int e, detail = 0;

  /* A transfer is open (its buffer is o->buf), or one into this slot was cut: no image to look
   * at. */
  if (o->stage == sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING || target_busy(o, slot & 1u)) {
    return reject(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_STATE, sensei_Gap9OtaResult_GAP9_OTA_INVALID, true);
  }
  e = slot_read(&r, 0, hdr, sizeof(hdr));
  if (e != 0) {
    return verify_fail(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH, e);
  }
  if (sensei_g9_slot_parse(slot_read, &r, o->cfg.slot_size, &si) != 0) {
    return verify_fail(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE, 0);
  }
  hl = hashed_len(hdr, si.total_size);
  sensei_sha256_init(&prefix);
  e = hash_slot(o, &r, hl, hl, NULL, &prefix);
  if (e != 0) {
    return verify_fail(o, info, sensei_Gap9OtaFail_GAP9_OTA_FAIL_FLASH, e);
  }
  if (hl == 0 || check_image(o, &r, si.total_size, &prefix, &si, &pf, &why, &detail) != 0) {
    return verify_fail(o, info, hl == 0 ? sensei_Gap9OtaFail_GAP9_OTA_FAIL_IMAGE : why, detail);
  }
  fill_state(o, info, sensei_Gap9OtaResult_GAP9_OTA_OK);
  fill_slots(o, info);
  info->has_version = true;
  info->version = (sensei_Version){si.ver_major, si.ver_minor, si.ver_revision, 0};
  info->image_hash.size = 32;
  memcpy(info->image_hash.bytes, pf.hash, 32);
  info->signature.size = 64;
  memcpy(info->signature.bytes, pf.sig, 64);
  if (pf.have_key_hash) {
    info->key_hash.size = 32;
    memcpy(info->key_hash.bytes, pf.key_hash, 32);
  }
  return sensei_Gap9OtaResult_GAP9_OTA_OK;
}

void g9ota_boot_note(struct g9ota *o) {
  if (!o->ck_loaded) {
    ck_load(o);
  }
  if (o->ck.valid && (o->ck.state == G9OTA_CK_DONE || o->ck.state == G9OTA_CK_CHECKED) && o->ck.target == o->booted) {
    (void)ck_put(o, G9OTA_CK_CLEARED, 0); /* the image of that transfer runs: nothing is waiting */
  }
}

sensei_Gap9OtaResult g9ota_abort(struct g9ota *o, sensei_Gap9OtaInfo *info) {
  bool open = o->stage != sensei_Gap9OtaStage_GAP9_OTA_STAGE_IDLE;

  if (!o->ck_loaded) {
    ck_load(o);
  }
  /* Something is open: this session's transfer, or the record of one from a session that lost
   * power: OPEN (not complete), CHECKED (complete, not sealed: the node did not accept it, or did not get to it) or
   * DONE (sealed, not started). A record of an image that runs is consumed (g9ota_boot_note). */
  if (open || (o->ck.valid &&
               (o->ck.state == G9OTA_CK_OPEN || o->ck.state == G9OTA_CK_CHECKED || o->ck.state == G9OTA_CK_DONE) &&
               o->ck.target == o->target)) {
    if (!open) {
      o->flushed = o->ck.offset; /* sectors that were written */
    }
    invalidate_target(o);
  }
  o->stage = sensei_Gap9OtaStage_GAP9_OTA_STAGE_IDLE;
  o->size = 0;
  o->offset = 0;
  o->fail = sensei_Gap9OtaFail_GAP9_OTA_FAIL_NONE;
  o->detail = 0;
  o->have_key_hash = false;
  o->sealed = false;
  return done(o, info, true);
}

int g9ota_dispatch(struct g9ota *o, const struct g9ota_req *req, uint32_t now_ms, sensei_Gap9OtaInfo *info) {
  g9ota_touch(o, now_ms);
  switch (req->op) {
  case G9OTA_OP_BEGIN:
    (void)g9ota_begin(o, &req->u.begin, info);
    return 0;
  case G9OTA_OP_CHUNK:
    (void)g9ota_chunk(o, &req->u.chunk, info);
    return 0;
  case G9OTA_OP_STATUS:
    if (req->u.status.verify) {
      if (req->u.status.slot > 1) {
        return -EINVAL;
      }
      (void)g9ota_verify(o, (uint8_t)req->u.status.slot, info);
    } else {
      (void)g9ota_status(o, info);
    }
    return 0;
  case G9OTA_OP_FINISH:
    (void)g9ota_finish(o, info);
    return 0;
  case G9OTA_OP_ABORT:
    (void)g9ota_abort(o, info);
    return 0;
  case G9OTA_OP_SEAL:
    (void)g9ota_seal(o, &req->u.seal, info);
    return 0;
  default:
    return -EINVAL;
  }
}
