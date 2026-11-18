/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file g9ota_ckpt.h
 * @brief The checkpoint records of the update writer (g9ota.h): the record format, and the look at the newest record
 * that the SSBL needs (gap9_slot.h: a slot that was sealed and never started does not boot as a fallback). No OS, no
 * nanopb: plain C with sensei_link/crc.h.
 */

#ifndef SENSEI_GAP9_G9OTA_CKPT_H_
#define SENSEI_GAP9_G9OTA_CKPT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** MRAM erase unit, and the size of the write buffer of the writer. */
#define G9OTA_SECTOR 8192u
/** Bytes of one checkpoint record, and the records in one checkpoint sector. */
#define G9OTA_CKPT_LEN 80u
#define G9OTA_CKPT_PER_SECTOR (G9OTA_SECTOR / G9OTA_CKPT_LEN)

/** What the newest valid checkpoint record says. */
struct g9ota_ckpt {
  bool valid;
  uint8_t state; /* G9OTA_CK_* */
  uint8_t target;
  uint32_t seq;
  uint32_t size;
  uint32_t offset;
  uint8_t sha[32];
  uint8_t hdr0[16]; /* the first 16 bytes of the file, once its first sector is in (see g9ota.c) */
};

/* OPEN: bytes are coming. CHECKED: every byte is in and finish checked it, the magic is not written (the slot is not an
 * image yet; a resume re-checks and answers VERIFIED). DONE: sealed, and not started yet (the boot of the slot clears
 * it: CLEARED). CLEARED: nothing is open. */
enum { G9OTA_CK_OPEN = 1, G9OTA_CK_DONE = 2, G9OTA_CK_CLEARED = 3, G9OTA_CK_CHECKED = 4 };

void g9ota_ckpt_encode(const struct g9ota_ckpt *c, uint8_t out[G9OTA_CKPT_LEN]);
/** False for an empty record, a wrong magic or a failed CRC. */
bool g9ota_ckpt_decode(const uint8_t in[G9OTA_CKPT_LEN], struct g9ota_ckpt *c);

/** Read `len` bytes at the MRAM address `addr`. Return 0 or a negative errno. */
typedef int (*g9ota_ckpt_read_fn)(void *ctx, uint32_t addr, void *buf, uint32_t len);

/**
 * The newest valid record (highest seq) of the log in the two checkpoint sectors at `ckpt_base`. Returns false if
 * there is none or a read failed. The SSBL calls it with the MRAM reader.
 */
bool g9ota_ckpt_newest(g9ota_ckpt_read_fn rd, void *ctx, const uint32_t ckpt_base[2], struct g9ota_ckpt *out);

/**
 * True if `slot` holds an image that was sealed and never started: the newest record is DONE for it. The SSBL starts
 * such a slot only when the slot line names it (the nRF5340 applies the update), never as the fallback of the other
 * one (OTA.md, "Recovery"). A read error counts as false (the slot is not held back for a log that cannot be read).
 */
bool g9ota_ckpt_sealed_waiting(g9ota_ckpt_read_fn rd, void *ctx, const uint32_t ckpt_base[2], uint8_t slot);

#endif /* SENSEI_GAP9_G9OTA_CKPT_H_ */
