/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* GVSoC test build of the recovery (SENSEI_SSBL_RECOVERY_TEST): GVSoC has no nRF5340 on the SPI link. The jobs come
 * from a packed image that the test script put into the keep slot, far behind its own image (a slot holds 984 KiB), so
 * that the keep slot stays "damaged" for the SSBL and is never written by the writer:
 *
 *   keep slot + FEED_OFF:  u32 "FEED" (0x44454546), u32 length, 8 B reserved, then the file of the other slot
 *
 * The feed does what the nRF5340 does: begin, chunks of 1024 B, finish, and the seal with the hash of the answer to
 * finish (it checks no signature: the writer's shape check is the test's; the nRF5340's signature check has its host
 * tests). With SENSEI_SSBL_RECOVERY_TEST_NOSEAL it stops before the seal: the slot must then stay unbootable. It prints
 * what it did, the test script looks for the lines. Returns 0 when it is done, and the SSBL tries the slots again. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pmsis.h>

#include "../src/recovery.h"
#include "sensei_gap9/gap9_slot.h"
#include "sensei_link/sha256.h"

#define FEED_OFF 0x80000u
#define FEED_MAGIC 0x44454546u

static sensei_Gap9RecoveryJob job;
static sensei_Gap9RecoveryResult res;
static uint8_t blk[G9OTA_CHUNK_MAX] __attribute__((aligned(16)));
static uint32_t seq;

struct feed_rd {
  const struct g9ota *o;
  uint32_t base;
};

static int feed_read(void *c, uint32_t off, void *buf, size_t len) {
  const struct feed_rd *r = c;

  return r->o->fl->read(NULL, r->base + off, buf, (uint32_t)len);
}

static const sensei_Gap9OtaInfo *run(struct g9rec *r) {
  job.seq = ++seq;
  if (g9rec_run(r, &job, pi_time_get_us() / 1000u, &res) != 0) {
    return NULL;
  }
  return &res.info;
}

int recovery_serve(struct g9rec *r, uint32_t reason) {
  const struct g9ota *o = r->o;
  uint32_t base = o->cfg.slot_base[o->booted] + FEED_OFF;
  uint8_t head[16] __attribute__((aligned(16)));
  uint32_t len, off;
  uint8_t digest[32];
  sensei_sha256_t h;
  const sensei_Gap9OtaInfo *i;

  (void)reason;
  if (o->fl->read(NULL, base, head, sizeof(head)) != 0) {
    printf("recovery feed: no read\n");
    return -EIO;
  }
  len = (uint32_t)head[4] | ((uint32_t)head[5] << 8) | ((uint32_t)head[6] << 16) | ((uint32_t)head[7] << 24);
  if ((head[0] | (head[1] << 8) | (head[2] << 16) | ((uint32_t)head[3] << 24)) != FEED_MAGIC || len == 0 ||
      len > o->cfg.slot_size) {
    printf("recovery feed: none\n");
    return -ENOENT;
  }
  printf("recovery feed: %u B\n", (unsigned)len);
  sensei_sha256_init(&h);
  for (off = 0; off < len; off += G9OTA_CHUNK_MAX) {
    uint32_t n = len - off < G9OTA_CHUNK_MAX ? len - off : G9OTA_CHUNK_MAX;

    o->fl->read(NULL, base + 16 + off, blk, n);
    sensei_sha256_update(&h, blk, n);
  }
  sensei_sha256_final(&h, digest);

  memset(&job, 0, sizeof(job));
  job.which_body = sensei_Gap9RecoveryJob_begin_tag;
  job.begin.size = len;
  memcpy(job.begin.sha256, digest, 32);
  i = run(r);
  if (i == NULL || i->result != sensei_Gap9OtaResult_GAP9_OTA_OK ||
      i->stage != sensei_Gap9OtaStage_GAP9_OTA_STAGE_RECEIVING) {
    printf("recovery feed: begin refused\n");
    return -EPERM;
  }
  printf("recovery feed: begin ok\n");
  for (off = 0; off < len;) {
    uint32_t n = len - off < G9OTA_CHUNK_MAX ? len - off : G9OTA_CHUNK_MAX;

    o->fl->read(NULL, base + 16 + off, blk, n);
    memset(&job, 0, sizeof(job));
    job.which_body = sensei_Gap9RecoveryJob_chunk_tag;
    job.chunk.offset = off;
    job.chunk.data.size = (pb_size_t)n;
    memcpy(job.chunk.data.bytes, blk, n);
    i = run(r);
    if (i == NULL || i->result != sensei_Gap9OtaResult_GAP9_OTA_OK) {
      printf("recovery feed: chunk refused at %u\n", (unsigned)off);
      return -EPERM;
    }
    off = i->offset > off ? i->offset : off + n;
  }
  memset(&job, 0, sizeof(job));
  job.which_body = sensei_Gap9RecoveryJob_finish_tag;
  i = run(r);
  if (i == NULL || i->result != sensei_Gap9OtaResult_GAP9_OTA_OK ||
      i->stage != sensei_Gap9OtaStage_GAP9_OTA_STAGE_VERIFIED || i->image_hash.size != 32) {
    printf("recovery feed: finish failed (%d)\n", i != NULL ? (int)i->fail : -1);
    return -EPERM;
  }
  printf("recovery feed: finish ok, not sealed\n");
#ifdef SENSEI_SSBL_RECOVERY_TEST_NOSEAL
  return 0; /* the nRF5340 did not accept the image, or never came back: no magic */
#else
  memcpy(digest, i->image_hash.bytes, 32);
  memset(&job, 0, sizeof(job));
  job.which_body = sensei_Gap9RecoveryJob_seal_tag;
  memcpy(job.seal.image_hash, digest, 32);
  i = run(r);
  if (i == NULL || i->result != sensei_Gap9OtaResult_GAP9_OTA_OK || !i->sealed) {
    printf("recovery feed: seal failed\n");
    return -EPERM;
  }
  printf("recovery feed: sealed slot %c\n", 'A' + o->target);
  {
    /* what the SSBL reads next: the other slot parses (the keep slot does not have to) */
    struct sensei_g9_slot_info si;
    struct feed_rd c = {o, o->cfg.slot_base[o->target]};

    printf("recovery feed: slot %c %s\n", 'A' + o->target,
           sensei_g9_slot_parse(feed_read, &c, o->cfg.slot_size, &si) == 0 ? "parses" : "does not parse");
  }
  return 0;
#endif
}
