/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The writer of the update in the SSBL: g9ota.c behind the job server g9rec.c, the MRAM operations of g9ota_mram_ops.c.
 * The keep slot is the one on the slot line (the nRF5340 drives its confirmed slot there); the guard of the writer
 * never writes it, the boot area or anything but the other slot and the two checkpoint sectors. No signature is checked
 * here: the nRF5340 does and then seals. */

#include <errno.h>
#include <stdio.h>

#include <pmsis.h>

#include "recovery.h"
#include "sensei_gap9/g9ota.h"
#include "sensei_gap9/g9ota_mram_ops.h"
#include "sensei_gap9/gap9_link.h"
#include "sensei_gap9/gap9_slot.h"

#ifndef SENSEI_SSBL_KIND
#error "SENSEI_SSBL_KIND: the board id of the images (the kind TLV), for example sensei-ieq-gap9"
#endif

#define STRAP_PAD PI_PAD_057
#define PAD_SAMPLES 5

static struct g9ota writer; /* 8 KiB buffer: .bss */
static struct g9rec rec;

bool recovery_strap(void) {
#ifdef SENSEI_SSBL_RECOVERY_TEST_STRAP
  return true; /* GVSoC does not model pad 57: the test case says the strap is high */
#elif defined(SENSEI_SSBL_STRAP)
  uint32_t high = 0;

  pi_pad_function_set(STRAP_PAD, PI_PAD_FUNC1);
  pi_gpio_pin_configure(STRAP_PAD, PI_GPIO_INPUT | PI_GPIO_PULL_DOWN | PI_GPIO_DRIVE_STRENGTH_LOW);
  pi_time_wait_us(100);
  for (int i = 0; i < PAD_SAMPLES; i++) {
    uint32_t v = 0;

    pi_gpio_pin_read(STRAP_PAD, &v);
    high += v ? 1 : 0;
    pi_time_wait_us(10);
  }
  return high > PAD_SAMPLES / 2; /* a pad that nobody drives reads low */
#else
  return false;
#endif
}

int recovery_run(uint8_t keep, uint32_t reason) {
  const struct g9ota_cfg cfg = {
      .slot_base = {SENSEI_G9_SLOT_A_OFF, SENSEI_G9_SLOT_B_OFF},
      .slot_size = SENSEI_G9_SLOT_SIZE,
      .ckpt_base = {SENSEI_G9_CKPT_OFF, SENSEI_G9_CKPT_OFF + G9OTA_SECTOR},
      .kind = SENSEI_SSBL_KIND,
  };
  int rc = g9ota_mram_open();

  if (rc != 0) {
    printf("recovery: MRAM does not open\n");
    return rc;
  }
  rc = g9ota_init(&writer, g9ota_mram_flash(), NULL, &cfg, keep);
  if (rc != 0) {
    printf("recovery: layout refused (%d)\n", rc);
    return rc;
  }
  g9rec_init(&rec, &writer);
  printf("recovery: keep slot %c, reason %u\n", 'A' + keep, (unsigned)reason);
  return recovery_serve(&rec, reason);
}
