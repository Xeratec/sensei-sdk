/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * Forked from utils/ssbl/src (boot_app.c, ssbl.c) of the GAP SDK 5.22.4 (GreenWaves Technologies, Apache-2.0): the
 * loading of an app binary into L2 and its launch are the same calls.
 */

/*
 * SENSEI second stage bootloader. Starts the firmware of slot A or B of the MRAM (lib/sensei_gap9/include/sensei_gap9/
 * gap9_slot.h). The nRF5340 selects the slot with the level of GAP9 pad 56 (P1.04): low A, high B. A slot that is not
 * a complete image or whose app does not load (header, descriptor, CRC of the sections) is skipped: the other slot
 * boots. If both fail, GAP9 waits (the nRF5340 powers it off after its boot timeout; JTAG recovers it), or, in a build
 * with SENSEI_SSBL_RECOVERY, runs the update writer for the nRF5340 (recovery.c). Before the launch it writes a
 * hand-off record with the slot that started (the app cannot tell after a fallback).
 *
 * Build flags: SENSEI_SSBL_FORCE_SLOT=0|1 uses that slot instead of the pad (GVSoC does not model it),
 * SENSEI_SSBL_BOOT_SAFETY_US is the watchdog the app disarms in its start-up (runtime system init),
 * SENSEI_SSBL_RECOVERY the recovery with SENSEI_SSBL_KIND (board id of the images), SENSEI_SSBL_STRAP (pad 57 forces
 * it, untested on hardware), SENSEI_SSBL_RECOVERY_TEST (the GVSoC test: the jobs come from the keep slot, recovery
 * returns).
 */

#include <pmsis.h>

#include <bsp/app_bin.h>
#include <bsp/app_boot.h>
#include <bsp/bsp.h>

#include "sensei_gap9/gap9_slot.h"

#ifdef SENSEI_SSBL_RECOVERY
#include "recovery.h"
#include "sensei_gap9/gap9_link.h"
#endif

#define SLOT_PAD PI_PAD_056
#define PAD_SAMPLES 5

#ifndef SENSEI_SSBL_BOOT_SAFETY_US
#define SENSEI_SSBL_BOOT_SAFETY_US 200000
#endif

static app_bin_description_t app;

struct mram_ctx {
  pi_device_t *dev;
  uint32_t base; /* MRAM offset of the slot */
};

static int mram_read(void *ctx, uint32_t off, void *buf, size_t len) {
  struct mram_ctx *c = ctx;

  pi_flash_read(c->dev, c->base + off, buf, (uint32_t)len); /* blocking, no status */
  return 0;
}

/* The slot line with the internal pull-down: a line that nobody drives selects slot A. Majority of a few samples. */
static uint8_t read_slot_line(void) {
  uint32_t high = 0;

  pi_pad_function_set(SLOT_PAD, PI_PAD_FUNC1);
  pi_gpio_pin_configure(SLOT_PAD, PI_GPIO_INPUT | PI_GPIO_PULL_DOWN | PI_GPIO_DRIVE_STRENGTH_LOW);
  pi_time_wait_us(100);
  for (int i = 0; i < PAD_SAMPLES; i++) {
    uint32_t v = 0;

    pi_gpio_pin_read(SLOT_PAD, &v);
    high += v ? 1 : 0;
    pi_time_wait_us(10);
  }
  return sensei_g9_slot_from_level(high > PAD_SAMPLES / 2);
}

/* Check the slot and load its app into L2. Returns 0 (ready to launch) or a negative value. */
static int load_slot(uint8_t slot) {
  const uint32_t base = sensei_g9_slot_offset(slot);
  struct sensei_g9_slot_info info;
  struct mram_ctx c = {.dev = NULL, .base = base};
  int rc;

  if (pi_open(PI_FLASH_MRAM, &c.dev) != PI_OK) {
    return -ENODEV;
  }
  rc = sensei_g9_slot_parse(mram_read, &c, SENSEI_G9_SLOT_SIZE, &info);
  if (rc == 0) {
    c.base = 0; /* absolute from here */
    rc = sensei_g9_app_check(mram_read, &c, base + info.app_off, info.app_size);
  }
  pi_close(PI_FLASH_MRAM);
  if (rc != 0) {
    printf("slot %c: not bootable (%d)\n", 'A' + slot, rc);
    return rc;
  }
  if (pi_app_bin_description_get(PI_FLASH_MRAM, base + info.app_off, &app) != PI_OK) {
    printf("slot %c: app header\n", 'A' + slot);
    return -EIO;
  }
  /* The CRC32 of every section is checked while it is loaded. */
  if (pi_app_bin_load(PI_FLASH_MRAM, &app, 1) != PI_OK) {
    printf("slot %c: app load failed\n", 'A' + slot);
    return -EBADMSG;
  }
  return 0;
}

/* Try `first`, then the other slot. Returns only if neither boots. */
static void boot_slots(uint8_t first) {
  for (int i = 0; i < 2; i++) {
    uint8_t slot = i == 0 ? first : (uint8_t)(first ^ 1u);

    printf("try slot %c\n", 'A' + slot);
    if (load_slot(slot) == 0) {
      printf("boot slot %c\n", 'A' + slot);
      hal_write32(&(FC_ICACHE->ENABLE), 0x0);
      /* the app disarms it in its runtime start-up */
      if (pi_app_boot_safety_arm(SENSEI_SSBL_BOOT_SAFETY_US) != PI_OK) {
        printf("watchdog failed\n");
      }
      /* The slot that started, for the app (gap9_slot.h, SENSEI_G9_HANDOFF_ADDR). */
      sensei_g9_handoff_encode((volatile uint32_t *)SENSEI_G9_HANDOFF_ADDR, slot);
      printf_deinit();
      pi_app_bin_launch(&app);
    }
  }
}

int main(void) {
  uint8_t first;

  printf("SENSEI SSBL\n");
#ifdef SENSEI_SSBL_FORCE_SLOT
  first = SENSEI_SSBL_FORCE_SLOT ? SENSEI_G9_SLOT_B : SENSEI_G9_SLOT_A;
#else
  first = read_slot_line();
#endif
#ifdef SENSEI_SSBL_RECOVERY
  if (recovery_strap()) { /* a slot may boot but hang before its HELLO: the nRF5340 asked for recovery */
    printf("recovery forced\n");
    (void)recovery_run(first, G9L_RECOVERY_FORCED);
  }
#endif
  boot_slots(first);
  printf("no bootable slot\n");
#ifdef SENSEI_SSBL_RECOVERY
  /* Serve the update writer. The production build does not return from here (the nRF5340 powers GAP9 off or on again);
   * the test build comes back after its feed and tries the slots once more. */
  printf("recovery: both slots failed\n");
  (void)recovery_run(first, G9L_RECOVERY_BOTH_BAD);
  boot_slots(first);
  printf("no bootable slot\n");
#endif
#ifdef SENSEI_SSBL_FORCE_SLOT
  pmsis_exit(-1); /* test build: end the simulation */
#endif
  for (;;) {
    pi_time_wait_us(1000000);
  }
  return 0;
}
