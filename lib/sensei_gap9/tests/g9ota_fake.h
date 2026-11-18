/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* A RAM "MRAM" for the tests of the writer (g9ota.c), with a power cut at the n-th write and a synthetic signed
 * image. */

#ifndef G9OTA_FAKE_H_
#define G9OTA_FAKE_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sensei_gap9/g9ota.h"
#include "sensei_gap9/gap9_slot.h"
#include "sensei_link/sha256.h"

/* The board id of the test images (the kind TLV). */
#define FAKE_KIND "sensei-test-gap9"

#define MRAM_SIZE SENSEI_G9_MRAM_SIZE

struct fake_mram {
  uint8_t mem[MRAM_SIZE];
  long writes;       /* erase and program calls so far */
  long cut_at;       /* the write with this number is cut short (-1: never) */
  unsigned cut_seed; /* how much of the cut write lands */
  bool dead;         /* power is gone: every call fails */
  long reprogram;    /* programs into bytes that were not erased: the real part is not an SRAM */
  long bad_align;
};

static inline int fm_read(void *c, uint32_t addr, void *buf, uint32_t len) {
  struct fake_mram *m = c;

  if (m->dead || (uint64_t)addr + len > MRAM_SIZE) {
    return -EIO;
  }
  memcpy(buf, m->mem + addr, len);
  return 0;
}

static inline int fm_erase(void *c, uint32_t addr) {
  struct fake_mram *m = c;
  uint32_t n = G9OTA_SECTOR;

  if (m->dead) {
    return -EIO;
  }
  if (addr % G9OTA_SECTOR != 0 || (uint64_t)addr + G9OTA_SECTOR > MRAM_SIZE) {
    m->bad_align++;
    return -EINVAL;
  }
  if (m->writes++ == m->cut_at) { /* power goes while erasing: a part of the sector is erased */
    n = (m->cut_seed * 2654435761u) % G9OTA_SECTOR;
    m->dead = true;
  }
  memset(m->mem + addr, 0xFF, n);
  return m->dead ? -EIO : 0;
}

static inline int fm_program(void *c, uint32_t addr, const void *buf, uint32_t len) {
  struct fake_mram *m = c;
  uint32_t n = len;

  if (m->dead) {
    return -EIO;
  }
  if (addr % 16 != 0 || len % 16 != 0 || (uint64_t)addr + len > MRAM_SIZE) {
    m->bad_align++;
    return -EINVAL;
  }
  for (uint32_t i = 0; i < len; i++) {
    if (m->mem[addr + i] != 0xFF && m->mem[addr + i] != ((const uint8_t *)buf)[i]) {
      m->reprogram++;
      break;
    }
  }
  if (m->writes++ == m->cut_at) { /* power goes while programming: a part of the bytes lands (and
                                     may be wrong) */
    n = ((m->cut_seed * 2654435761u) % (len + 1)) & ~15u;
    m->dead = true;
  }
  memcpy(m->mem + addr, buf, n);
  if (m->dead && n < len) {
    m->mem[addr + n] = (uint8_t)(((const uint8_t *)buf)[n] ^ 0x5A); /* a torn word */
  }
  return m->dead ? -EIO : 0;
}

static const struct g9ota_flash fake_ops = {fm_read, fm_erase, fm_program};

static inline struct g9ota_cfg fake_layout(void) {
  struct g9ota_cfg l = {
      .slot_base = {SENSEI_G9_SLOT_A_OFF, SENSEI_G9_SLOT_B_OFF},
      .slot_size = SENSEI_G9_SLOT_SIZE,
      .ckpt_base = {SENSEI_G9_CKPT_OFF, SENSEI_G9_CKPT_OFF + G9OTA_SECTOR},
      .kind = FAKE_KIND,
  };
  return l;
}

/* ---- a signed image in the MCUboot format with the container of sensei_gap9/gap9_slot.h ---- */

struct img_opt {
  size_t payload; /* bytes of the app binary and the readfs together (rounded to 16) */
  bool bad_app;   /* the app binary has a descriptor with a section over the hand-off record */
  uint8_t major, minor;
  uint16_t revision;
  const char *kind; /* NULL: no kind TLV */
  bool sign;        /* signature TLV */
  bool bad_digest;  /* the hash TLV does not match the bytes */
  unsigned seed;
};

static inline void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static inline void put32(uint8_t *p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}

/* Returns the length of the file written to `out`. */
static inline size_t build_image(uint8_t *out, const struct img_opt *o) {
  const uint32_t hdr = 0x200;
  const uint32_t app_min = SENSEI_G9_APP_HDR_SIZE + SENSEI_G9_APP_AREA_SIZE + 16; /* a descriptor, one area */
  uint32_t app = (uint32_t)((o->payload / 2 + 15) & ~15u), rfs = (uint32_t)((o->payload / 2 + 15) & ~15u);

  if (app < app_min) {
    app = (app_min + 15) & ~15u;
  }
  uint32_t img = SENSEI_G9_CONTAINER_SIZE + app + rfs;
  uint8_t *p, *prot, *tlv;
  uint32_t prot_len = 0, tlv_len;
  uint8_t digest[32];
  sensei_sha256_t h;
  unsigned x = o->seed ? o->seed : 1;

  memset(out, 0, hdr);
  put32(out, SENSEI_G9_MCUBOOT_MAGIC);
  put16(out + 8, hdr);
  put32(out + 12, img);
  out[20] = o->major;
  out[21] = o->minor;
  put16(out + 22, o->revision);
  p = out + hdr;
  put32(p, SENSEI_G9_CONTAINER_MAGIC);
  put32(p + 4, SENSEI_G9_CONTAINER_VERSION);
  put32(p + 8, SENSEI_G9_CONTAINER_SIZE);
  put32(p + 12, app);
  put32(p + 16, SENSEI_G9_CONTAINER_SIZE + app);
  put32(p + 20, rfs);
  for (uint32_t i = SENSEI_G9_CONTAINER_SIZE; i < img; i++) { /* pseudo random payload */
    x = x * 1664525u + 1013904223u;
    p[i] = (uint8_t)(x >> 24);
  }
  {
    /* the app binary: the descriptor that the SSBL checks (gapy "app binary", one area) */
    uint8_t *a = p + SENSEI_G9_CONTAINER_SIZE;

    memset(a, 0, SENSEI_G9_APP_HDR_SIZE + SENSEI_G9_APP_AREA_SIZE);
    put32(a, SENSEI_G9_APP_MAGIC);
    put32(a + 4, 1);
    put32(a + SENSEI_G9_APP_HDR_SIZE, SENSEI_G9_APP_HDR_SIZE + SENSEI_G9_APP_AREA_SIZE);      /* start */
    put32(a + SENSEI_G9_APP_HDR_SIZE + 4, o->bad_app ? SENSEI_G9_HANDOFF_ADDR : 0x1C000100u); /* ptr */
    put32(a + SENSEI_G9_APP_HDR_SIZE + 8, 16);                                                /* size */
  }
  prot = out + hdr + img;
  if (o->kind != NULL) {
    size_t kl = strlen(o->kind);

    put16(prot, SENSEI_G9_TLV_PROT_MAGIC);
    put16(prot + 4, 0x00A0);
    put16(prot + 6, (uint16_t)kl);
    memcpy(prot + 8, o->kind, kl);
    prot_len = (uint32_t)(4 + 4 + kl);
  } else {
    put16(prot, SENSEI_G9_TLV_PROT_MAGIC);
    prot_len = 4;
  }
  put16(prot + 2, (uint16_t)prot_len);
  put16(out + 10, (uint16_t)prot_len);
  sensei_sha256_init(&h);
  sensei_sha256_update(&h, out, hdr + img + prot_len);
  sensei_sha256_final(&h, digest);
  if (o->bad_digest) {
    digest[0] ^= 0xFF;
  }
  tlv = prot + prot_len;
  put16(tlv, SENSEI_G9_TLV_INFO_MAGIC);
  tlv_len = 4;
  put16(tlv + tlv_len, 0x01); /* key hash */
  put16(tlv + tlv_len + 2, 32);
  memset(tlv + tlv_len + 4, 0x11, 32);
  tlv_len += 36;
  put16(tlv + tlv_len, 0x10); /* image hash */
  put16(tlv + tlv_len + 2, 32);
  memcpy(tlv + tlv_len + 4, digest, 32);
  tlv_len += 36;
  if (o->sign) { /* a signature of the right shape (not a valid one: the node checks it) */
    uint8_t *s = tlv + tlv_len;

    put16(s, 0x22);
    put16(s + 2, 70);
    s[4] = 0x30;
    s[5] = 68;
    s[6] = 0x02;
    s[7] = 32;
    for (int i = 0; i < 32; i++) {
      s[8 + i] = (uint8_t)(0x20 + i);
    }
    s[40] = 0x02;
    s[41] = 32;
    for (int i = 0; i < 32; i++) {
      s[42 + i] = (uint8_t)(0x40 + i);
    }
    tlv_len += 74;
  }
  put16(tlv + 2, (uint16_t)tlv_len);
  return hdr + img + prot_len + tlv_len;
}

static inline void file_sha(const uint8_t *d, size_t n, uint8_t out[32]) {
  sensei_sha256_t h;

  sensei_sha256_init(&h);
  sensei_sha256_update(&h, d, n);
  sensei_sha256_final(&h, out);
}

#endif
