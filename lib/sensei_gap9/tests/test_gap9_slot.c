/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Slot format of the SSBL: MCUboot header, TLV areas, container and app descriptor, on a slot in RAM. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "sensei_gap9/gap9_slot.h"

static int fails, checks;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    checks++;                                                                                                          \
    if (!(c)) {                                                                                                        \
      fails++;                                                                                                         \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                                              \
    }                                                                                                                  \
  } while (0)

#define SLOT_SIZE 0x8000u /* a small slot is enough for the checks */
#define HDR 0x200u
#define PROT 12u
#define TLVS 40u
#define APP_OFF 0x20u
#define NAREAS 2u
#define APP_SIZE (SENSEI_G9_APP_HDR_SIZE + NAREAS * SENSEI_G9_APP_AREA_SIZE + 0x400u)
#define RFS_OFF 0x1000u
#define RFS_SIZE 0x800u
#define IMG (RFS_OFF + RFS_SIZE)

static uint8_t slot[SLOT_SIZE];

static void put16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
  put16(p, v);
  put16(p + 2, v >> 16);
}

static int rd(void *ctx, uint32_t off, void *buf, size_t len) {
  (void)ctx;
  if ((uint64_t)off + len > SLOT_SIZE) {
    return -EIO;
  }
  memcpy(buf, slot + off, len);
  return 0;
}

static void area(unsigned i, uint32_t start, uint32_t ptr, uint32_t size, uint32_t flags) {
  uint8_t *a = slot + HDR + APP_OFF + SENSEI_G9_APP_HDR_SIZE + i * SENSEI_G9_APP_AREA_SIZE;

  put32(a, start);
  put32(a + 4, ptr);
  put32(a + 8, size);
  put32(a + 16, flags);
}

/* A complete slot, like tools/make_gap9_ota.py builds it. */
static void build(void) {
  memset(slot, 0xff, sizeof(slot));
  put32(slot, SENSEI_G9_MCUBOOT_MAGIC);
  put16(slot + 8, HDR);
  put16(slot + 10, PROT);
  put32(slot + 12, IMG);
  slot[20] = 1; /* version 1.2.3+4 */
  slot[21] = 2;
  put16(slot + 22, 3);
  put32(slot + 24, 4);
  memset(slot + 32, 0, HDR - 32);

  uint8_t *c = slot + HDR;

  memset(c, 0, IMG);
  put32(c, SENSEI_G9_CONTAINER_MAGIC);
  put32(c + 4, SENSEI_G9_CONTAINER_VERSION);
  put32(c + 8, APP_OFF);
  put32(c + 12, APP_SIZE);
  put32(c + 16, RFS_OFF);
  put32(c + 20, RFS_SIZE);

  uint8_t *app = c + APP_OFF;

  put32(app, SENSEI_G9_APP_MAGIC);
  put32(app + 4, NAREAS);
  put32(app + 8, 0x1c000100);
  area(0, 0x600, 0x1c000100, 0x300, 0); /* text */
  area(1, 0x900, 0x1c010000, 0x100, 0); /* data */

  uint8_t *p = slot + HDR + IMG;

  memset(p, 0, PROT + TLVS);
  put16(p, SENSEI_G9_TLV_PROT_MAGIC);
  put16(p + 2, PROT);
  put16(p + PROT, SENSEI_G9_TLV_INFO_MAGIC);
  put16(p + PROT + 2, TLVS);
}

static void test_layout(void) {
  CHECK(sensei_g9_slot_offset(0) == 0x14000 && sensei_g9_slot_offset(1) == 0x10A000 && sensei_g9_slot_offset(2) == 0);
  CHECK(SENSEI_G9_SLOT_A_OFF + SENSEI_G9_SLOT_SIZE == SENSEI_G9_SLOT_B_OFF);
  CHECK(SENSEI_G9_SLOT_B_OFF + SENSEI_G9_SLOT_SIZE == SENSEI_G9_MRAM_SIZE);
  CHECK(SENSEI_G9_CKPT_OFF + SENSEI_G9_CKPT_SIZE == SENSEI_G9_SLOT_A_OFF);
  CHECK(sensei_g9_slot_from_level(0) == SENSEI_G9_SLOT_A && sensei_g9_slot_from_level(1) == SENSEI_G9_SLOT_B);
}

static void test_good(void) {
  struct sensei_g9_slot_info i;

  build();
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == 0);
  CHECK(i.hdr_size == HDR && i.img_size == IMG && i.total_size == HDR + IMG + PROT + TLVS);
  CHECK(i.app_off == HDR + APP_OFF && i.app_size == APP_SIZE && i.readfs_off == HDR + RFS_OFF &&
        i.readfs_size == RFS_SIZE);
  CHECK(i.ver_major == 1 && i.ver_minor == 2 && i.ver_revision == 3 && i.ver_build == 4);
  CHECK(sensei_g9_app_check(rd, NULL, i.app_off, i.app_size) == 0);

  /* no protected TLV area, no readfs */
  build();
  put16(slot + 10, 0);
  memmove(slot + HDR + IMG, slot + HDR + IMG + PROT, TLVS);
  put32(slot + HDR + 16, 0);
  put32(slot + HDR + 20, 0);
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == 0 && i.readfs_size == 0 && i.total_size == HDR + IMG + TLVS);
}

static void test_bad_slot(void) {
  struct sensei_g9_slot_info i;

  /* erased and zero slots */
  memset(slot, 0xff, sizeof(slot));
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -ENOENT);
  memset(slot, 0, sizeof(slot));
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -ENOENT);

  /* header sizes */
  build();
  put16(slot + 8, 0x10);
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EINVAL);
  build();
  put16(slot + 8, HDR + 4); /* not 16 aligned */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EINVAL);
  build();
  put32(slot + 12, SLOT_SIZE); /* image does not fit */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EINVAL);
  build();
  put32(slot + 12, 0xfffffff0u); /* overflow */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EINVAL);

  /* a write that stopped before the TLV areas */
  build();
  memset(slot + HDR + IMG, 0xff, PROT + TLVS);
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EBADMSG);
  build();
  memset(slot + HDR + IMG + PROT, 0xff, TLVS); /* protected area there, the TLV area is not */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EBADMSG);
  build();
  put16(slot + HDR + IMG + 2, PROT + 4); /* protected size differs from the header */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EBADMSG);
  build();
  put16(slot + HDR + IMG + PROT + 2, 0x7ff0); /* TLV area larger than the slot */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EINVAL);

  /* container */
  build();
  put32(slot + HDR, 0);
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EBADMSG);
  build();
  put32(slot + HDR + 4, 2); /* unknown version */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EBADMSG);
  build();
  put32(slot + HDR + 8, APP_OFF + 4); /* not 16 aligned */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EINVAL);
  build();
  put32(slot + HDR + 8, 0x10); /* app over the container */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EINVAL);
  build();
  put32(slot + HDR + 12, IMG); /* app past the image */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EINVAL);
  build();
  put32(slot + HDR + 12, 0);
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EINVAL);
  build();
  put32(slot + HDR + 16, RFS_OFF + RFS_SIZE); /* readfs past the image */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EINVAL);
  build();
  put32(slot + HDR + 16, APP_OFF + 0x10); /* readfs inside the app */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EINVAL);
  build();
  put32(slot + HDR + 8, 0xfffffff0u); /* overflow */
  CHECK(sensei_g9_slot_parse(rd, NULL, SLOT_SIZE, &i) == -EINVAL);
}

static void test_app_check(void) {
  uint32_t a = HDR + APP_OFF;

  build();
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == 0);
  CHECK(sensei_g9_app_check(rd, NULL, a, 100) == -EBADMSG); /* too small for a descriptor */

  build();
  put32(slot + a, 0);
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == -EBADMSG);
  build();
  put32(slot + a + 4, 0);
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == -EINVAL);
  build();
  put32(slot + a + 4, 33);
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == -EINVAL);
  build();
  put32(slot + a + 32, 4); /* XIP */
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == -EINVAL);
  build();
  put32(slot + a + 4, 30); /* more areas than the app holds */
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == -EINVAL);

  /* section data outside the app */
  build();
  area(0, APP_SIZE, 0x1c000100, 0x300, 0);
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == -EINVAL);
  /* over the SSBL in the upper L2 */
  build();
  area(1, 0x900, SENSEI_G9_SSBL_LO - 0x80, 0x100, 0);
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == -EINVAL);
  build();
  area(1, 0x900, SENSEI_G9_SSBL_HI - 0x10, 0x100, 0);
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == -EINVAL);
  build();
  area(1, 0x900, SENSEI_G9_SSBL_HI, 0x100, 0); /* just above it is fine */
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == 0);
  build();
  area(1, 0x900, SENSEI_G9_L2_END - 0x10, 0x100, 0); /* past the L2 */
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == -EINVAL);
  /* sections that are not loaded: L1, overlay, empty */
  build();
  area(1, 0x900, 0x10000004, 0x100, 0);
  area(0, 0x600, SENSEI_G9_SSBL_LO, 0x100, SENSEI_G9_APP_FLAG_OVERLAY);
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == 0);
  build();
  area(1, 0x900, SENSEI_G9_SSBL_LO, 0, 0);
  CHECK(sensei_g9_app_check(rd, NULL, a, APP_SIZE) == 0);
}

int main(void) {
  test_layout();
  test_good();
  test_bad_slot();
  test_app_check();
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
