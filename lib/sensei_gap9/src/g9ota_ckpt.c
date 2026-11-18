/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_gap9/g9ota_ckpt.h"

#include <string.h>

#include "sensei_link/crc.h"

#define CKPT_MAGIC 0x31544F47u /* "GOT1" */

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
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

bool g9ota_ckpt_newest(g9ota_ckpt_read_fn rd, void *ctx, const uint32_t ckpt_base[2], struct g9ota_ckpt *out) {
  uint8_t rec[G9OTA_CKPT_LEN];

  memset(out, 0, sizeof(*out));
  for (int s = 0; s < 2; s++) {
    for (uint32_t i = 0; i < G9OTA_CKPT_PER_SECTOR; i++) {
      struct g9ota_ckpt c;

      if (rd(ctx, ckpt_base[s] + i * G9OTA_CKPT_LEN, rec, sizeof(rec)) != 0) {
        memset(out, 0, sizeof(*out));
        return false;
      }
      memset(&c, 0, sizeof(c));
      if (g9ota_ckpt_decode(rec, &c) && (!out->valid || (int32_t)(c.seq - out->seq) > 0)) {
        *out = c;
      }
    }
  }
  return out->valid;
}

bool g9ota_ckpt_sealed_waiting(g9ota_ckpt_read_fn rd, void *ctx, const uint32_t ckpt_base[2], uint8_t slot) {
  struct g9ota_ckpt c;

  return g9ota_ckpt_newest(rd, ctx, ckpt_base, &c) && c.state == G9OTA_CK_DONE && c.target == slot;
}
