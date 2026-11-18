/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_gap9/g9ota_mram.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "pmsis.h"

#include "bsp/bsp.h"
#include "bsp/fs.h"
#include "sensei_gap9/gap9_side.h"
#include "sensei_gap9/gap9_slot.h"

#define LOG_ERR(...) printf("[GAP9-ERR] " __VA_ARGS__)
#if G9L_LOG_LEVEL >= 2
#define LOG_INF(...) printf("[GAP9-INF] " __VA_ARGS__)
#else
#define LOG_INF(...)
#endif

/* The slot line (GAP9 pad 56, driven by the nRF5340 P1.04): low A, high B. */
#define SLOT_PAD PI_PAD_056

static struct pi_device ota_flash_dev;
static struct pi_mram_conf ota_conf;
static bool ota_flash_open;
static struct g9ota g_ota; /* 8 KiB buffer: in .bss, not in the image */
static bool ready;
static uint8_t booted;
static bool from_slot;
static uint32_t readfs_off = 0xFFFFFFFFu;

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

/* ---- Which slot ---- */

/* The slot line (pad 56) is read for the log only: it is what the node asked for, not what runs.
 * The SSBL starts the other slot when the first one does not load, and a reset or a JTAG load
 * starts without it. */
static uint8_t read_slot_line(void) {
  uint32_t high = 0;

  pi_pad_function_set(SLOT_PAD, PI_PAD_FUNC1);
  pi_gpio_pin_configure(SLOT_PAD, PI_GPIO_INPUT | PI_GPIO_PULL_DOWN | PI_GPIO_DRIVE_STRENGTH_LOW);
  pi_time_wait_us(100);
  for (int i = 0; i < 5; i++) {
    uint32_t v = 0;

    pi_gpio_pin_read(SLOT_PAD, &v);
    high += v ? 1 : 0;
    pi_time_wait_us(10);
  }
  return sensei_g9_slot_from_level(high > 2);
}

struct slot_ctx {
  uint32_t base;
};

static int slot_rd(void *c, uint32_t off, void *buf, size_t len) {
  return mram_read(NULL, ((const struct slot_ctx *)c)->base + off, buf, (uint32_t)len);
}

/* The slot is a complete image with a container: this application was started from it. */
static bool slot_is_image(uint8_t slot, struct sensei_g9_slot_info *si) {
  struct slot_ctx c = {sensei_g9_slot_offset(slot)};

  return sensei_g9_slot_parse(slot_rd, &c, SENSEI_G9_SLOT_SIZE, si) == 0;
}

int g9ota_mram_init(const char *kind, const struct g9ota_cfg *test_area) {
  struct g9ota_cfg cfg = {
      .slot_base = {SENSEI_G9_SLOT_A_OFF, SENSEI_G9_SLOT_B_OFF},
      .slot_size = SENSEI_G9_SLOT_SIZE,
      .ckpt_base = {SENSEI_G9_CKPT_OFF, SENSEI_G9_CKPT_OFF + G9OTA_SECTOR},
      .kind = kind,
  };
  struct sensei_g9_slot_info si;
  int h;

  pi_mram_conf_init(&ota_conf);
  pi_open_from_conf(&ota_flash_dev, &ota_conf);
  if (pi_flash_open(&ota_flash_dev)) {
    LOG_ERR("MRAM does not open\n");
    return -EIO;
  }
  ota_flash_open = true;
  /* The slot this application runs from is the one in the hand-off record of the SSBL, and only
   * that. The writer is armed with a record that names a slot holding a complete image: there
   * the other slot is free to write. Without one (single-image layout from `make flash`, JTAG
   * load, a reset without the SSBL) the layout may be the old one, where the app and its readfs
   * lie where the checkpoint and slot A are: nothing is written and the update jobs answer
   * NOT_BUILT. */
  h = sensei_g9_handoff_decode((const volatile uint32_t *)SENSEI_G9_HANDOFF_ADDR);
  from_slot = g9ota_arm(&mram_ops, NULL, &cfg, h) >= 0;
  if (from_slot) {
    booted = (uint8_t)h;
    (void)slot_is_image(booted, &si);
  }
  if (from_slot && si.readfs_size != 0) {
    readfs_off = sensei_g9_slot_offset(booted) + si.readfs_off;
  }
  if (test_area != NULL) {
    /* Local mode runs from the single-image layout, whose readfs and app lie where the checkpoint
     * and slot A would be: the test uses its own area and writes only there. */
    cfg = *test_area;
    cfg.kind = kind;
    booted = 0;
    from_slot = true;
  }
  if (from_slot) {
    g9ota_init(&g_ota, &mram_ops, NULL, &cfg, booted);
    g9ota_boot_note(&g_ota);
    ready = true;
  }
  LOG_INF("running from %s (%s), slot line %s\n", booted ? "slot B" : "slot A",
          from_slot ? "update writer armed"
                    : (h >= 0 ? "record names a slot without an image: updates refused"
                              : "no hand-off record: updates refused"),
          read_slot_line() ? "high" : "low");
  return 0;
}

struct g9ota *g9ota_mram_get(void) { return ready ? &g_ota : NULL; }

uint8_t g9ota_mram_booted_slot(void) { return booted; }

bool g9ota_mram_from_slot(void) { return from_slot; }

uint32_t g9ota_mram_readfs_offset(void) { return readfs_off; }

/* ---- readfs of the slot: pi_fs_conf_init is wrapped at link time (-Wl,--wrap=pi_fs_conf_init)
 * ---- */

void __real_pi_fs_conf_init(struct pi_fs_conf *conf);

void __wrap_pi_fs_conf_init(struct pi_fs_conf *conf) {
  __real_pi_fs_conf_init(conf);
  if (readfs_off != 0xFFFFFFFFu) {
    /* The mount still looks up a readfs partition in the partition table (it checks its type),
     * in the volume G9OTA_READFS_VOLUME: the layout has a small placeholder there. Only its
     * existence counts: the data is read at the offset of the slot. The "active" volume is not
     * used: the SSBL does not mark one. */
    conf->volume_name = (char *)G9OTA_READFS_VOLUME;
    conf->partition_offset = readfs_off;
  }
}
