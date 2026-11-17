/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#ifndef SENSEI_OTA_IMAGE_H_
#define SENSEI_OTA_IMAGE_H_

/**
 * @file sensei_ota_image.h
 * @brief Find a TLV in the trailer of a signed MCUboot image (no Zephyr, host-testable). Used for the image id (the
 *        SHA-256 TLV) of the nRF5340 node and of the LPWAN shield, and for the kind TLV that says which board an image
 *        is for.
 *
 * Image layout: 32 B header (magic 0x96f3b83d, header size at 8, protect TLV size at 10, image size at 12), the image,
 * the protected TLV area (magic 0x6908, covered by the signature), the TLV area (magic 0x6907, not covered). Each TLV
 * is a 16 bit type, a 16 bit length and the value. imgtool adds a custom TLV to the protected area:
 * `imgtool sign --custom-tlv 0xa0 <string>` (see sysbuild.cmake of the app).
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SENSEI_IMAGE_MAGIC 0x96f3b83dU
#define SENSEI_IMAGE_TLV_INFO_MAGIC 0x6907u
#define SENSEI_IMAGE_TLV_PROT_MAGIC 0x6908u
#define SENSEI_IMAGE_TLV_SHA256 0x10u
/** Protected custom TLV: the board the image is for (a string without terminator). */
#define SENSEI_IMAGE_TLV_KIND 0x00A0u

/** Read `len` bytes at `off` of the image (the flash area of the slot). Return 0 or a negative errno. */
typedef int (*sensei_image_read_fn)(void *ctx, uint32_t off, void *buf, size_t len);

static inline uint16_t sensei_image_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t sensei_image_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One TLV area at `pos` of `size` bytes (with its info header): look for `type`. */
static inline int sensei_image_area_find(sensei_image_read_fn rd, void *ctx, uint32_t pos, uint32_t size,
                                         uint16_t magic, uint16_t type, uint8_t *out, size_t max) {
  uint8_t h[4];
  uint32_t end = pos + size;

  if (size < 4 || end < pos || rd(ctx, pos, h, 4) != 0) {
    return -EIO;
  }
  if (sensei_image_le16(h) != magic || sensei_image_le16(h + 2) != size) {
    return -ENOENT;
  }
  for (uint32_t p = pos + 4; p + 4 <= end;) {
    uint16_t len;

    if (rd(ctx, p, h, 4) != 0) {
      return -EIO;
    }
    len = sensei_image_le16(h + 2);
    if ((uint64_t)p + 4 + len > end) {
      return -ENOENT; /* damaged */
    }
    if (sensei_image_le16(h) == type) {
      size_t n = len < max ? len : max;

      if (n > 0 && rd(ctx, p + 4, out, n) != 0) {
        return -EIO;
      }
      return len;
    }
    p += 4u + len;
  }
  return -ENOENT;
}

/**
 * Find the TLV `type` of the image whose first byte is at offset 0 of `rd`. Copies up to `max` bytes of the value to
 * `out` and returns the length of the value, or a negative errno (-ENOENT: no such TLV or no MCUboot image, -EIO).
 * `protected_only`: look only in the protected area, the one the signature covers (the kind TLV).
 */
static inline int sensei_image_tlv(sensei_image_read_fn rd, void *ctx, uint16_t type, bool protected_only, uint8_t *out,
                                   size_t max) {
  uint8_t h[32];
  uint32_t hdr, img, prot, pos;
  uint8_t info[4];
  int r = -ENOENT;

  if (rd(ctx, 0, h, sizeof(h)) != 0) {
    return -EIO;
  }
  if (sensei_image_le32(h) != SENSEI_IMAGE_MAGIC) {
    return -ENOENT;
  }
  hdr = sensei_image_le16(h + 8);
  prot = sensei_image_le16(h + 10);
  img = sensei_image_le32(h + 12);
  pos = hdr + img;
  if (pos < hdr) {
    return -ENOENT;
  }
  if (prot != 0) {
    r = sensei_image_area_find(rd, ctx, pos, prot, SENSEI_IMAGE_TLV_PROT_MAGIC, type, out, max);
    if (r != -ENOENT) {
      return r;
    }
  }
  if (protected_only) {
    return -ENOENT;
  }
  pos += prot;
  if (rd(ctx, pos, info, sizeof(info)) != 0) {
    return -EIO;
  }
  return sensei_image_area_find(rd, ctx, pos, sensei_image_le16(info + 2), SENSEI_IMAGE_TLV_INFO_MAGIC, type, out, max);
}

/** The image id: the first 4 bytes of the SHA-256 TLV (little endian), 0 if there is none. */
static inline uint32_t sensei_image_id(sensei_image_read_fn rd, void *ctx) {
  uint8_t sha[32];

  return sensei_image_tlv(rd, ctx, SENSEI_IMAGE_TLV_SHA256, false, sha, sizeof(sha)) == 32 ? sensei_image_le32(sha) : 0;
}

/** The image is for `kind`: the protected kind TLV holds exactly that string. */
static inline bool sensei_image_kind_is(sensei_image_read_fn rd, void *ctx, const char *kind) {
  uint8_t v[48];
  size_t n = 0;
  int r;

  while (kind[n] != '\0') {
    n++;
  }
  if (n > sizeof(v)) {
    return false;
  }
  r = sensei_image_tlv(rd, ctx, SENSEI_IMAGE_TLV_KIND, true, v, sizeof(v));
  if (r < 0 || (size_t)r != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (v[i] != (uint8_t)kind[i]) {
      return false;
    }
  }
  return true;
}

#endif /* SENSEI_OTA_IMAGE_H_ */
