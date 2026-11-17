/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Host test of sensei_ota_image.h: the image id and the kind TLV of an MCUboot image, with and without a protected
 * TLV area, and damaged trailers. */

#include <stdio.h>
#include <string.h>

#include "sensei_ota_image.h"

static int failures;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                                                  \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static uint8_t flash[4096];
static size_t flash_len;

static int rd(void *ctx, uint32_t off, void *buf, size_t len) {
  (void)ctx;
  if (off + len > flash_len) {
    return -EIO;
  }
  memcpy(buf, flash + off, len);
  return 0;
}

static void put16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
  put16(p, v);
  put16(p + 2, v >> 16);
}

/* A TLV entry at p: type, length, value. Returns its size. */
static size_t tlv(uint8_t *p, uint16_t type, const void *v, uint16_t len) {
  put16(p, type);
  put16(p + 2, len);
  memcpy(p + 4, v, len);
  return 4u + len;
}

/* hdr 0x200 + 1000 B image, then (optional) the protected area with `kind`, then the TLV area with the hash. */
static void make(const char *kind, uint16_t kind_tag, bool hash_in_protected) {
  uint8_t sha[32];
  uint8_t *p;
  uint32_t prot = 0;

  for (int i = 0; i < 32; i++) {
    sha[i] = (uint8_t)(0xA0 + i);
  }
  memset(flash, 0xFF, sizeof(flash));
  memset(flash, 0, 32);
  put32(flash, SENSEI_IMAGE_MAGIC);
  put16(flash + 8, 0x200);
  put32(flash + 12, 1000);
  p = flash + 0x200 + 1000;
  if (kind != NULL || hash_in_protected) {
    uint8_t *a = p + 4;

    if (kind != NULL) {
      a += tlv(a, kind_tag, kind, (uint16_t)strlen(kind));
    }
    if (hash_in_protected) {
      a += tlv(a, SENSEI_IMAGE_TLV_SHA256, sha, 32);
    }
    prot = (uint32_t)(a - p);
    put16(p, SENSEI_IMAGE_TLV_PROT_MAGIC);
    put16(p + 2, prot);
    put16(flash + 10, prot);
    p = a;
  }
  {
    uint8_t *a = p + 4;

    if (!hash_in_protected) {
      a += tlv(a, SENSEI_IMAGE_TLV_SHA256, sha, 32);
    }
    a += tlv(a, 0x22, sha, 32); /* a signature, say */
    put16(p, SENSEI_IMAGE_TLV_INFO_MAGIC);
    put16(p + 2, (uint16_t)(a - p));
    flash_len = (size_t)(a - flash);
  }
}

static void test_id(void) {
  make(NULL, 0, false);
  CHECK(sensei_image_id(rd, NULL) == 0xA3A2A1A0u);
  make("sensei-lpwan-nrf9151", SENSEI_IMAGE_TLV_KIND, false);
  CHECK(sensei_image_id(rd, NULL) == 0xA3A2A1A0u); /* a protected area in front of the hash */
  make("x", SENSEI_IMAGE_TLV_KIND, true);
  CHECK(sensei_image_id(rd, NULL) == 0xA3A2A1A0u); /* a hash in the protected area is found too */
  /* a type with the same low byte is not the hash */
  make(NULL, 0, false);
  put16(flash + 0x200 + 1000 + 4, 0x0110);
  CHECK(sensei_image_id(rd, NULL) == 0);
  /* not an image */
  memset(flash, 0, 32);
  CHECK(sensei_image_id(rd, NULL) == 0);
}

static void test_kind(void) {
  make("sensei-lpwan-nrf9151", SENSEI_IMAGE_TLV_KIND, false);
  CHECK(sensei_image_kind_is(rd, NULL, "sensei-lpwan-nrf9151"));
  CHECK(!sensei_image_kind_is(rd, NULL, "sensei-ieq-nrf5340"));
  CHECK(!sensei_image_kind_is(rd, NULL, "sensei-lpwan-nrf915"));   /* prefix */
  CHECK(!sensei_image_kind_is(rd, NULL, "sensei-lpwan-nrf91511")); /* longer */
  make("sensei-ieq-nrf5340", SENSEI_IMAGE_TLV_KIND, false);
  CHECK(sensei_image_kind_is(rd, NULL, "sensei-ieq-nrf5340"));
  CHECK(!sensei_image_kind_is(rd, NULL, "sensei-lpwan-nrf9151"));
  /* no kind at all (an image made before the TLV existed) */
  make(NULL, 0, false);
  CHECK(!sensei_image_kind_is(rd, NULL, "sensei-lpwan-nrf9151"));
  /* another custom tag */
  make("sensei-lpwan-nrf9151", 0x00A1, false);
  CHECK(!sensei_image_kind_is(rd, NULL, "sensei-lpwan-nrf9151"));
  /* the signature does not cover the unprotected area: a kind put there does not count */
  make(NULL, 0, false);
  {
    uint8_t *a = flash + 0x200 + 1000 + 4;
    const char *k = "sensei-lpwan-nrf9151";

    a += 4 + 32; /* the hash */
    a += tlv(a, SENSEI_IMAGE_TLV_KIND, k, (uint16_t)strlen(k));
    put16(flash + 0x200 + 1000 + 2, (uint16_t)(a - (flash + 0x200 + 1000)));
    flash_len = (size_t)(a - flash);
  }
  CHECK(!sensei_image_kind_is(rd, NULL, "sensei-lpwan-nrf9151"));
  {
    uint8_t v[32];

    CHECK(sensei_image_tlv(rd, NULL, SENSEI_IMAGE_TLV_KIND, false, v, sizeof(v)) == 20); /* found, but not protected */
    CHECK(sensei_image_tlv(rd, NULL, SENSEI_IMAGE_TLV_KIND, true, v, sizeof(v)) == -ENOENT);
  }
}

static void test_damaged(void) {
  uint8_t v[8];

  /* TLV longer than its area */
  make("sensei-lpwan-nrf9151", SENSEI_IMAGE_TLV_KIND, false);
  put16(flash + 0x200 + 1000 + 4 + 2, 0xFFF0);
  CHECK(!sensei_image_kind_is(rd, NULL, "sensei-lpwan-nrf9151"));
  /* the area length in the header differs from the one in the TLV info */
  make("sensei-lpwan-nrf9151", SENSEI_IMAGE_TLV_KIND, false);
  put16(flash + 10, 12);
  CHECK(!sensei_image_kind_is(rd, NULL, "sensei-lpwan-nrf9151"));
  /* the file ends inside the trailer */
  make("sensei-lpwan-nrf9151", SENSEI_IMAGE_TLV_KIND, false);
  flash_len = 0x200 + 1000 + 10;
  CHECK(sensei_image_tlv(rd, NULL, SENSEI_IMAGE_TLV_KIND, true, v, sizeof(v)) < 0);
  CHECK(sensei_image_id(rd, NULL) == 0);
  flash_len = 16;
  CHECK(sensei_image_id(rd, NULL) == 0);
  /* image size that wraps */
  make(NULL, 0, false);
  put32(flash + 12, 0xFFFFFFF0u);
  CHECK(sensei_image_id(rd, NULL) == 0);
  /* a value is truncated to the buffer, the full length is returned */
  make("sensei-lpwan-nrf9151", SENSEI_IMAGE_TLV_KIND, false);
  CHECK(sensei_image_tlv(rd, NULL, SENSEI_IMAGE_TLV_KIND, true, v, sizeof(v)) == 20 && memcmp(v, "sensei-l", 8) == 0);
}

int main(void) {
  test_id();
  test_kind();
  test_damaged();
  printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
