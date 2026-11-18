/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file gap9_slot.h
 * @brief A/B firmware slots in the GAP9 MRAM: layout, slot format and the checks of the SSBL (GAP9/ssbl). Pure C, no
 *        OS, no GAP SDK: the SSBL, the GAP9 app (slot status, readfs offset), the host tools and the host tests use it.
 *
 * MRAM (2 MiB):
 *   0x000000 boot area: FSBL, meta table, SSBL, partition and volume table (written by the factory flash only)
 *   0x010000 checkpoint of the OTA writer (16 KiB)
 *   0x014000 slot A (0xF6000 B)
 *   0x10A000 slot B (0xF6000 B)
 *
 * A slot holds one MCUboot image (imgtool sign), so that the nRF5340 can verify it like its own images:
 *   32 B header (magic 0x96f3b83d, hdr_size, protected TLV size, img_size, version), padding up to hdr_size,
 *   the image (img_size bytes), the protected TLV area (magic 0x6908), the TLV area (magic 0x6907).
 * The image is a container:
 *   struct sensei_g9_container (32 B, little endian) at the start of the image, then the GAP9 app binary (the bytes of
 *   the gapy "app binary" section, first word 0xC001B001) and the readfs (the bytes of the gapy "readfs" section).
 *   Offsets in the container are relative to the start of the image. The tools pad both parts to 16 bytes.
 * The SSBL needs the header, the TLV areas (a slot that was not written to its end has none), the container and the
 * app descriptor. It loads the app with the CRC32 of its sections. The app opens its readfs at readfs_off.
 */

#ifndef SENSEI_GAP9_SLOT_H_
#define SENSEI_GAP9_SLOT_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSEI_G9_MRAM_SIZE 0x200000u
#define SENSEI_G9_BOOT_SIZE 0x010000u
#define SENSEI_G9_CKPT_OFF 0x010000u
#define SENSEI_G9_CKPT_SIZE 0x004000u
#define SENSEI_G9_SLOT_A_OFF 0x014000u
#define SENSEI_G9_SLOT_B_OFF 0x10A000u
#define SENSEI_G9_SLOT_SIZE 0x0F6000u

#define SENSEI_G9_SLOT_A 0u
#define SENSEI_G9_SLOT_B 1u

/** MRAM offset of a slot (0 = A, 1 = B), 0 for another value. */
static inline uint32_t sensei_g9_slot_offset(uint8_t slot) {
  return slot == SENSEI_G9_SLOT_A ? SENSEI_G9_SLOT_A_OFF : (slot == SENSEI_G9_SLOT_B ? SENSEI_G9_SLOT_B_OFF : 0u);
}

/** The slot that follows from the level of the slot line (GAP9 pad 56): low = A, high = B. */
static inline uint8_t sensei_g9_slot_from_level(int level) { return level ? SENSEI_G9_SLOT_B : SENSEI_G9_SLOT_A; }

#define SENSEI_G9_MCUBOOT_MAGIC 0x96f3b83dU
#define SENSEI_G9_TLV_INFO_MAGIC 0x6907u
#define SENSEI_G9_TLV_PROT_MAGIC 0x6908u
#define SENSEI_G9_CONTAINER_MAGIC 0x42413953U /* "S9AB" */
#define SENSEI_G9_CONTAINER_VERSION 1u
#define SENSEI_G9_CONTAINER_SIZE 32u
#define SENSEI_G9_ALIGN 16u
#define SENSEI_G9_HDR_MAX 0x1000u

/** Where the parts of a slot are, as offsets from the start of the slot. */
struct sensei_g9_slot_info {
  uint32_t hdr_size;   /**< MCUboot header size: the image starts here */
  uint32_t img_size;   /**< image size */
  uint32_t total_size; /**< header, image and TLV areas: the bytes an update writes */
  uint32_t app_off;    /**< GAP9 app binary */
  uint32_t app_size;
  uint32_t readfs_off; /**< readfs (size 0: none) */
  uint32_t readfs_size;
  uint8_t ver_major; /**< MCUboot image version */
  uint8_t ver_minor;
  uint16_t ver_revision;
  uint32_t ver_build;
};

/** Read `len` bytes at `off` of the slot. Return 0 or a negative errno. */
typedef int (*sensei_g9_read_fn)(void *ctx, uint32_t off, void *buf, size_t len);

static inline uint16_t sensei_g9_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t sensei_g9_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Parse and check the slot (header, TLV areas, container). `slot_size` bounds every offset.
 * Returns 0, -ENOENT (no image: wrong magic, an erased slot), -EINVAL (sizes or offsets do not fit),
 * -EBADMSG (TLV areas or container missing or damaged: a slot that was not written to its end) or -EIO.
 */
static inline int sensei_g9_slot_parse(sensei_g9_read_fn rd, void *ctx, uint32_t slot_size,
                                       struct sensei_g9_slot_info *out) {
  uint8_t h[SENSEI_G9_CONTAINER_SIZE];
  uint32_t hdr, img, prot, pos, tot;

  if (rd(ctx, 0, h, 32) != 0) {
    return -EIO;
  }
  if (sensei_g9_le32(h) != SENSEI_G9_MCUBOOT_MAGIC) {
    return -ENOENT;
  }
  hdr = sensei_g9_le16(h + 8);
  prot = sensei_g9_le16(h + 10);
  img = sensei_g9_le32(h + 12);
  if (hdr < 32 || hdr > SENSEI_G9_HDR_MAX || (hdr % SENSEI_G9_ALIGN) != 0 || img < SENSEI_G9_CONTAINER_SIZE ||
      slot_size < hdr || img > slot_size - hdr) {
    return -EINVAL;
  }
  out->hdr_size = hdr;
  out->img_size = img;
  out->ver_major = h[20];
  out->ver_minor = h[21];
  out->ver_revision = sensei_g9_le16(h + 22);
  out->ver_build = sensei_g9_le32(h + 24);

  /* TLV areas: a slot whose write did not reach its end has none of them. */
  pos = hdr + img;
  if (prot != 0) {
    if (prot < 4 || prot > slot_size - pos) {
      return -EINVAL;
    }
    if (rd(ctx, pos, h, 4) != 0) {
      return -EIO;
    }
    if (sensei_g9_le16(h) != SENSEI_G9_TLV_PROT_MAGIC || sensei_g9_le16(h + 2) != prot) {
      return -EBADMSG;
    }
    pos += prot;
  }
  if (slot_size < pos || slot_size - pos < 4) {
    return -EINVAL;
  }
  if (rd(ctx, pos, h, 4) != 0) {
    return -EIO;
  }
  tot = sensei_g9_le16(h + 2);
  if (sensei_g9_le16(h) != SENSEI_G9_TLV_INFO_MAGIC) {
    return -EBADMSG;
  }
  if (tot < 4 || tot > slot_size - pos) {
    return -EINVAL;
  }
  out->total_size = pos + tot;

  /* Container. */
  if (rd(ctx, hdr, h, SENSEI_G9_CONTAINER_SIZE) != 0) {
    return -EIO;
  }
  if (sensei_g9_le32(h) != SENSEI_G9_CONTAINER_MAGIC || sensei_g9_le32(h + 4) != SENSEI_G9_CONTAINER_VERSION) {
    return -EBADMSG;
  }
  {
    uint32_t a_off = sensei_g9_le32(h + 8), a_size = sensei_g9_le32(h + 12);
    uint32_t r_off = sensei_g9_le32(h + 16), r_size = sensei_g9_le32(h + 20);

    if ((a_off % SENSEI_G9_ALIGN) != 0 || (r_off % SENSEI_G9_ALIGN) != 0 || a_off < SENSEI_G9_CONTAINER_SIZE ||
        a_size == 0 || a_off > img || a_size > img - a_off || r_off > img || r_size > img - r_off) {
      return -EINVAL;
    }
    if (r_size != 0 && r_off < a_off + a_size && r_off + r_size > a_off) {
      return -EINVAL; /* readfs inside the app */
    }
    out->app_off = hdr + a_off;
    out->app_size = a_size;
    out->readfs_off = hdr + r_off;
    out->readfs_size = r_size;
  }
  return 0;
}

/* ---- GAP9 app binary (gapy "app binary" section, gap-sdk bsp/app_bin.h) ---- */

#define SENSEI_G9_APP_MAGIC 0xC001B001U
#define SENSEI_G9_APP_NB_AREAS_MAX 32u
#define SENSEI_G9_APP_HDR_SIZE 1332u /* sizeof(app_bin_desc_header_t) */
#define SENSEI_G9_APP_AREA_SIZE 52u  /* sizeof(app_bin_desc_mem_area_t) */
#define SENSEI_G9_APP_FLAG_OVERLAY 1u
#define SENSEI_G9_APP_FLAG_LZ4 2u /* flags >> 24: number of blocks */
/** The loader treats 0x1C000000 ... 0x1D000000 as L2 (pi_app_bin_load); the physical L2 ends at 0x1C190000. */
#define SENSEI_G9_L2_BASE 0x1C000000U
#define SENSEI_G9_L2_LOADER_END 0x1D000000U
#define SENSEI_G9_L2_END 0x1C190000U
/** The part of L2 that the SSBL uses itself (linker script gap_mem_upper.ld): sections must not land there. */
#define SENSEI_G9_SSBL_LO 0x1C100000U
#define SENSEI_G9_SSBL_HI 0x1C190000U

/* ---- Hand-off record: the slot the SSBL started ---- */

/**
 * The SSBL starts the other slot when the first one does not load, so the pad alone does not tell the app which slot it
 * runs from. Right before it launches the app, the SSBL writes this record to L2: 4 words (magic, slot, ~slot, 0).
 * Address: the top of L2 priv0 (0x1C000000..0x1C010000), 256 bytes below its end. Why it survives until the app reads
 * it:
 *   - L2 shared is not usable: the runtime allocator powers down every 128 KiB bank that holds no allocation (GVSoC
 *     shows it: the SSBL cannot read back a word above its own banks), so a word in the heap is lost. L2 priv0 is not
 *     power-managed, and the app (CONFIG_MALLOC_NO_L2_PRIV=y) has no heap there. Its sections (.data_fc, the IRQ stack)
 *     start at 0x1C000004 and use a few KiB;
 *   - sensei_g9_app_check() refuses an app section that covers the record, so the loader never writes it, and the SSBL
 *     writes the record after the load;
 *   - the app reads the record at the start of main().
 * Nothing else reads or clears it: a reset starts the SSBL again, which writes it again.
 */
#define SENSEI_G9_HANDOFF_ADDR 0x1C00FF00U
#define SENSEI_G9_HANDOFF_SIZE 16u          /* no app section may cover it (sensei_g9_app_check) */
#define SENSEI_G9_HANDOFF_MAGIC 0x4F483953U /* "S9HO" */

/* The record lies in L2 priv0 (0x1C000000 ... 0x1C010000), below its end. GAP9 apps check at build time that nothing of
 * theirs lands on it: CONFIG_MALLOC_NO_L2_PRIV=y and no section over it (firmware/gap9/tools/check_handoff.py). */
_Static_assert(SENSEI_G9_HANDOFF_ADDR >= SENSEI_G9_L2_BASE &&
                   SENSEI_G9_HANDOFF_ADDR + SENSEI_G9_HANDOFF_SIZE <= SENSEI_G9_L2_BASE + 0x10000u,
               "the hand-off record must lie in L2 priv0");

/** Fill the record for `slot`. */
static inline void sensei_g9_handoff_encode(volatile uint32_t rec[4], uint8_t slot) {
  rec[1] = slot;
  rec[2] = ~(uint32_t)slot;
  rec[3] = 0;
  rec[0] = SENSEI_G9_HANDOFF_MAGIC; /* the magic last */
}

/** The slot of a record, or -ENOENT if there is none (reset without the SSBL, JTAG load) or it is damaged. */
static inline int sensei_g9_handoff_decode(const volatile uint32_t rec[4]) {
  if (rec[0] != SENSEI_G9_HANDOFF_MAGIC || rec[1] > SENSEI_G9_SLOT_B || rec[2] != ~rec[1]) {
    return -ENOENT;
  }
  return (int)rec[1];
}

/**
 * Check the descriptor of the app at `app_off` (size `app_size`) before pi_app_bin_load() trusts it: magic, number of
 * areas, no XIP, every area (and every LZ4 block) inside the app, no L2 area beyond the physical L2, over the SSBL or
 * the hand-off record. The CRC of the section
 * data is not checked here. Returns 0, -EBADMSG (not an app binary), -EINVAL (damaged descriptor) or -EIO.
 */
static inline int sensei_g9_app_check(sensei_g9_read_fn rd, void *ctx, uint32_t app_off, uint32_t app_size) {
  uint8_t h[SENSEI_G9_APP_AREA_SIZE];
  uint32_t n;

  if (app_size < SENSEI_G9_APP_HDR_SIZE) {
    return -EBADMSG;
  }
  if (rd(ctx, app_off, h, 48) != 0) {
    return -EIO;
  }
  if (sensei_g9_le32(h) != SENSEI_G9_APP_MAGIC) {
    return -EBADMSG;
  }
  n = sensei_g9_le32(h + 4);
  if (n == 0 || n > SENSEI_G9_APP_NB_AREAS_MAX) {
    return -EINVAL;
  }
  if (sensei_g9_le32(h + 32) != 0) { /* xip_flash_nb_pages: the loader would map absolute flash addresses */
    return -EINVAL;
  }
  if (app_size < SENSEI_G9_APP_HDR_SIZE + n * SENSEI_G9_APP_AREA_SIZE) {
    return -EINVAL;
  }
  for (uint32_t i = 0; i < n; i++) {
    uint32_t start, ptr, size, flags;

    if (rd(ctx, app_off + SENSEI_G9_APP_HDR_SIZE + i * SENSEI_G9_APP_AREA_SIZE, h, SENSEI_G9_APP_AREA_SIZE) != 0) {
      return -EIO;
    }
    start = sensei_g9_le32(h);
    ptr = sensei_g9_le32(h + 4);
    size = sensei_g9_le32(h + 8);
    flags = sensei_g9_le32(h + 16);
    if (size == 0 || (flags & SENSEI_G9_APP_FLAG_OVERLAY) != 0) {
      continue; /* nothing is loaded */
    }
    if (start >= app_size) {
      return -EINVAL;
    }
    if ((flags & SENSEI_G9_APP_FLAG_LZ4) != 0) {
      /* Blocks: 2 x u16 (decompressed - 1, compressed - 1), then the compressed bytes. The loader trusts them. */
      uint32_t blocks = flags >> 24, cur = 0, total = 0;

      if (blocks == 0) {
        return -EINVAL;
      }
      for (uint32_t b = 0; b < blocks; b++) {
        uint8_t bh[4];
        uint32_t dec, comp;

        if (app_size - start < cur || app_size - start - cur < 4) {
          return -EINVAL;
        }
        if (rd(ctx, app_off + start + cur, bh, 4) != 0) {
          return -EIO;
        }
        dec = (uint32_t)sensei_g9_le16(bh) + 1u;
        comp = (uint32_t)sensei_g9_le16(bh + 2) + 1u;
        if (comp > app_size - start - cur - 4) {
          return -EINVAL;
        }
        cur += 4 + comp;
        total += dec;
      }
      if (total != size) {
        return -EINVAL;
      }
    } else if (size > app_size - start) {
      return -EINVAL;
    }
    if (ptr >= SENSEI_G9_L2_BASE && ptr < SENSEI_G9_L2_LOADER_END) {
      if (ptr >= SENSEI_G9_L2_END || size > SENSEI_G9_L2_END - ptr ||
          (ptr < SENSEI_G9_SSBL_HI && ptr + size > SENSEI_G9_SSBL_LO) ||
          (ptr < SENSEI_G9_HANDOFF_ADDR + SENSEI_G9_HANDOFF_SIZE && ptr + size > SENSEI_G9_HANDOFF_ADDR)) {
        return -EINVAL;
      }
    }
  }
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_GAP9_SLOT_H_ */
