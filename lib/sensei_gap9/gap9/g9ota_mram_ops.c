/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_gap9/g9ota_mram_ops.h"

#include <errno.h>
#include <string.h>

#include "pmsis.h"

#include "bsp/bsp.h"

static struct pi_device ota_flash_dev;
static struct pi_mram_conf ota_conf;
static bool ota_flash_open;

int g9ota_mram_open(void) {
  if (ota_flash_open) {
    return 0;
  }
  pi_mram_conf_init(&ota_conf);
  pi_open_from_conf(&ota_flash_dev, &ota_conf);
  if (pi_flash_open(&ota_flash_dev)) {
    return -EIO;
  }
  ota_flash_open = true;
  return 0;
}

/* ---- MRAM operations (blocking) ---- */

/* Reads go through a small buffer in L2 on a 16 byte window: the uDMA wants aligned L2 addresses
 * and the parsers read 4 and 32 bytes at any offset. Whole aligned blocks go straight to the
 * caller's buffer. */
static PI_L2 uint8_t bounce[256 + 32];

static int mram_read(void *ctx, uint32_t addr, void *buf, uint32_t len) {
  uint8_t *out = buf;

  (void)ctx;
  if (!ota_flash_open) {
    return -ENODEV;
  }
  if ((addr & 15u) == 0 && (len & 15u) == 0 && (((uintptr_t)buf) & 3u) == 0) {
    pi_flash_read(&ota_flash_dev, addr, buf, len);
    return 0;
  }
  while (len > 0) {
    uint32_t a0 = addr & ~15u;
    uint32_t head = addr - a0;
    uint32_t n = len < sizeof(bounce) - 32u ? len : (uint32_t)sizeof(bounce) - 32u;
    uint32_t span = (head + n + 15u) & ~15u;

    pi_flash_read(&ota_flash_dev, a0, bounce, span);
    memcpy(out, bounce + head, n);
    out += n;
    addr += n;
    len -= n;
  }
  return 0;
}

static int mram_erase(void *ctx, uint32_t addr) {
  (void)ctx;
  if (!ota_flash_open) {
    return -ENODEV;
  }
  pi_flash_erase_sector(&ota_flash_dev, addr);
  return 0;
}

static int mram_program(void *ctx, uint32_t addr, const void *buf, uint32_t len) {
  (void)ctx;
  if (!ota_flash_open) {
    return -ENODEV;
  }
  return pi_flash_program(&ota_flash_dev, addr, buf, len) == 0 ? 0 : -EIO;
}

static const struct g9ota_flash mram_ops = {mram_read, mram_erase, mram_program};

const struct g9ota_flash *g9ota_mram_flash(void) { return &mram_ops; }
