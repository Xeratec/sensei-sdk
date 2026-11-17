/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Randomized check of lpwan_dfu.c. Host-like sender (next_chunk with 0xFF skip and gap),
 * random lost replies, shield restarts (RAM lost, flash kept), power loss in the middle of a data write, a sector
 * erase or a log write (partial NOR state), random chunk sizes. Every run must end VERIFIED with slot == image. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sensei_lpwan/lpwan_dfu.h"

#define SLOT (448u * 1024u)
static uint8_t slot[SLOT], logf[LPWAN_DFU_SECTOR], running[32];
static int violations;
static int crash_in; /* >0: count down flash ops, crash (partial op) at 0 */
static int crashed;
static unsigned rng = 1;
static unsigned rnd(void) {
  rng = rng * 1103515245u + 12345u;
  return rng >> 8;
}
static int tick(void) {
  if (crash_in > 0 && --crash_in == 0) {
    crashed = 1;
    return 1;
  }
  return crashed;
}
static uint32_t cap(void *c, sensei_DfuTarget t) {
  (void)c;
  return t == sensei_DfuTarget_DFU_TARGET_APP ? SLOT : 0;
}
static int er(void *c, uint32_t off, uint32_t len) {
  (void)c;
  if (crashed)
    return -EIO;
  if (tick()) { /* partial erase: some bytes 0xFF, others random */
    for (uint32_t i = 0; i < len; i++)
      slot[off + i] = (rnd() & 1) ? 0xFF : (uint8_t)rnd();
    return -EIO;
  }
  memset(slot + off, 0xFF, len);
  return 0;
}
static int wr(void *c, uint32_t off, const uint8_t *d, size_t n) {
  (void)c;
  if (crashed)
    return -EIO;
  if (off % 4 || n % 4)
    violations++;
  size_t m = n;
  int cr = tick();
  if (cr)
    m = (rnd() % (n / 4 + 1)) * 4; /* partial write: a prefix of whole words */
  for (size_t i = 0; i < m; i++) {
    if (slot[off + i] != 0xFF)
      violations++;
    slot[off + i] &= d[i];
  }
  return cr ? -EIO : 0;
}
static int rd(void *c, uint32_t off, uint8_t *d, size_t n) {
  (void)c;
  memcpy(d, slot + off, n);
  return 0;
}
static int lrd(void *c, uint32_t off, uint8_t *d, size_t n) {
  (void)c;
  memcpy(d, logf + off, n);
  return 0;
}
static int lwr(void *c, uint32_t off, const uint8_t *d, size_t n) {
  (void)c;
  if (crashed)
    return -EIO;
  size_t m = n;
  int cr = tick();
  if (cr)
    m = rnd() % n;
  for (size_t i = 0; i < m; i++) {
    if (logf[off + i] != 0xFF)
      violations++;
    logf[off + i] &= d[i];
  }
  return cr ? -EIO : 0;
}
static int ler(void *c) {
  (void)c;
  if (crashed)
    return -EIO;
  if (tick()) {
    for (unsigned i = 0; i < sizeof(logf); i++)
      logf[i] = (rnd() & 1) ? 0xFF : logf[i];
    return -EIO;
  }
  memset(logf, 0xFF, sizeof(logf));
  return 0;
}
static int hdrs(void *c, uint8_t nh[32], uint8_t rh[32]) {
  (void)c;
  memcpy(nh, slot, 32);
  memcpy(rh, running, 32);
  return 0;
}
static int apply(void *c, sensei_DfuTarget t) {
  (void)c;
  (void)t;
  return 0;
}
static const struct lpwan_dfu_ops ops = {.capacity = cap,
                                         .erase = er,
                                         .write = wr,
                                         .read = rd,
                                         .log_read = lrd,
                                         .log_write = lwr,
                                         .log_erase = ler,
                                         .read_headers = hdrs,
                                         .apply = apply};

static void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}

/* host next_chunk */
static int next_chunk(const uint8_t *img, uint32_t n, uint32_t pos, uint32_t cmax, uint32_t *off, uint32_t *len,
                      int *gap) {
  uint32_t o = pos;
  *gap = 0;
  while (o < n) {
    uint32_t end = o + cmax < n ? o + cmax : n;
    int ff = 1;
    for (uint32_t i = o; i < end && ff; i++)
      ff = img[i] == 0xFF;
    if (end < n && o % 4 == 0 && ff) {
      o = end;
      *gap = 1;
      continue;
    }
    *off = o;
    *len = end - o;
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  int runs = argc > 1 ? atoi(argv[1]) : 60;
  int bad = 0, restarts_total = 0, crashes_total = 0;

  for (int run = 0; run < runs; run++) {
    rng = (unsigned)run * 7919u + 1u;
    uint32_t size = 20000 + rnd() % (SLOT - 20000 - 8192);
    uint8_t *img = malloc(size);
    for (uint32_t i = 0; i < size; i++)
      img[i] = (uint8_t)rnd();
    /* big padding and small 0xFF runs */
    uint32_t a = 512 + rnd() % (size / 2), b = a + rnd() % (size / 3);
    for (uint32_t i = a; i < b && i < size; i++)
      img[i] = 0xFF;
    for (int k = 0; k < 20; k++) {
      uint32_t s = rnd() % size, l = rnd() % 600;
      for (uint32_t i = s; i < s + l && i < size; i++)
        img[i] = 0xFF;
    }
    memset(img, 0, 32);
    put32(img, 0x96f3b83du);
    img[9] = 2;
    put32(img + 12, size - 0x300);
    img[21] = 2;
    uint8_t sha[32];
    sensei_sha256_t h;
    sensei_sha256_init(&h);
    sensei_sha256_update(&h, img, size);
    sensei_sha256_final(&h, sha);
    memset(running, 0, 32);
    put32(running, 0x96f3b83du);
    running[21] = 1;
    memset(slot, 0x55, sizeof(slot));
    if (run % 3 == 0)
      memset(logf, 0xFF, sizeof(logf));

    struct lpwan_dfu d;
    sensei_DfuStatus st;
    sensei_DfuBegin bg = sensei_DfuBegin_init_zero;
    bg.target = sensei_DfuTarget_DFU_TARGET_APP;
    bg.size = size;
    memcpy(bg.sha256, sha, 32);
    bg.resume = true;
    lpwan_dfu_init(&d, &ops, NULL, NULL);
    crashed = 0;
    crash_in = 0;
    lpwan_dfu_begin(&d, &bg, &st);
    uint32_t pos = st.offset;
    int steps = 0, done = 0;
    while (!done && steps++ < 200000) {
      uint32_t cmax = (rnd() % 4 == 0) ? 4 * (1 + rnd() % 128) : 188;
      uint32_t off, len;
      int gap;
      if (!next_chunk(img, size, pos, cmax, &off, &len, &gap)) {
        sensei_LpwanResult r = lpwan_dfu_finish(&d, &st);
        if (r != sensei_LpwanResult_LPWAN_OK || st.state != sensei_DfuState_DFU_STATE_VERIFIED) {
          printf("run %d: finish failed r=%d state=%d fail=%d off=%u size=%u\n", run, r, st.state, st.fail,
                 (unsigned)st.offset, size);
          bad++;
        } else if (memcmp(slot, img, size) != 0) {
          printf("run %d: slot differs\n", run);
          bad++;
        }
        done = 1;
        break;
      }
      /* sometimes resend an older chunk (repeat) */
      if (rnd() % 50 == 0 && pos > 400) {
        uint32_t o = pos - 1 - rnd() % 400;
        o &= ~3u;
        uint32_t l = pos - o + rnd() % 64;
        if (o + l > size)
          l = size - o;
        if (l > 512)
          l = 512;
        sensei_DfuChunk c = sensei_DfuChunk_init_zero;
        c.offset = o;
        c.data.size = (pb_size_t)l;
        memcpy(c.data.bytes, img + o, l);
        lpwan_dfu_chunk(&d, &c, &st);
      }
      if (rnd() % 400 == 0)
        crash_in = 1 + rnd() % 3; /* power loss within the next flash ops */
      sensei_DfuChunk c = sensei_DfuChunk_init_zero;
      c.offset = off;
      c.gap = gap;
      c.data.size = (pb_size_t)len;
      memcpy(c.data.bytes, img + off, len);
      sensei_LpwanResult r = lpwan_dfu_chunk(&d, &c, &st);
      int lost = rnd() % 30 == 0;
      if (crashed || rnd() % 300 == 0) { /* shield restart: RAM gone */
        crashes_total += crashed;
        restarts_total++;
        crashed = 0;
        crash_in = 0;
        lpwan_dfu_init(&d, &ops, NULL, NULL);
        lost = 1;
      }
      if (!lost && r == sensei_LpwanResult_LPWAN_OK) {
        pos = off + len;
        continue;
      }
      /* resync like the host */
      lpwan_dfu_status(&d, &st);
      if (st.state == sensei_DfuState_DFU_STATE_RECEIVING) {
        pos = st.offset;
      } else if (st.state == sensei_DfuState_DFU_STATE_FAILED) {
        printf("run %d: FAILED fail=%d detail=%d\n", run, st.fail, st.detail);
        bad++;
        done = 1;
      } else {
        lpwan_dfu_begin(&d, &bg, &st);
        pos = st.offset;
      }
    }
    if (!done) {
      printf("run %d: no end\n", run);
      bad++;
    }
    free(img);
  }
  printf("runs %d bad %d violations %d restarts %d crashes %d\n", runs, bad, violations, restarts_total, crashes_total);
  return bad || violations;
}
