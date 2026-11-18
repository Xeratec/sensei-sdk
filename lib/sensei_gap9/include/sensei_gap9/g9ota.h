/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file g9ota.h
 * @brief Writer of the GAP9 update: the rules of one transfer into the MRAM slot that GAP9 does not
 * run from. No GAP SDK: the MRAM is behind `struct g9ota_flash`, the host tests run the code on a RAM
 * "MRAM".
 *
 * Messages are in lib/sensei_link/proto/sensei/gap9_ota.proto (Gap9Ota*), the rules of the whole update in OTA.md.
 *
 * - The image is a signed MCUboot-format container (SDK sensei_gap9/gap9_slot.h). Bytes arrive in
 * order; `offset` is the next byte wanted. A chunk below it is a repeat (nothing is written twice),
 * one above it is refused.
 * - Bytes collect in an 8 KiB buffer (one MRAM sector). A full sector is erased, programmed and
 * read back; then a checkpoint (offset, size, SHA-256 of the file) is appended to a log in two
 * checkpoint sectors that take turns. A power cut loses at most the sector in progress: `begin`
 * with `resume` finds the checkpoint and answers its offset (a multiple of the sector), the host
 * sends from there.
 * - The slot GAP9 runs from is never written: every erase and program goes through an address guard
 * that allows only the other slot and the checkpoint sectors.
 * - A fresh `begin` erases the header sector of the slot. The first MRAM word of the file (16
 * bytes, with the MCUboot magic) is left out while the image comes in (the checkpoint records keep
 * it) and written by `seal`, the last step: a slot that is not sealed never parses, whatever is in
 * the rest of it.
 * - `finish` reads the slot back and checks, in this order: SHA-256 of the whole file (as in
 * `begin`), the MCUboot header and the container, the kind TLV, the image hash TLV against the
 * bytes, the app descriptor (as the SSBL checks it), the signature TLV (present, shape), the
 * version against the running one; then it records the transfer as checked (state CHECKED) and
 * returns the hash, the signature as r || s and the key hash for the nRF5340, which owns the public
 * key. It does not write the magic.
 * - `seal` (deferred sealing) writes the magic. The nRF5340 sends it after it checked the signature
 * over the hash of the answer to `finish`; GAP9 does not check the signature. So an image that the
 * nRF5340 did not accept (a bad signature, a lost abort, a node that was reset) has no magic and
 * the SSBL never starts it, not even as the fallback of a bad confirmed slot. A slot is "valid" for
 * `g9ota_slot_info` only if its structure parses (sealed) and no transfer into it is open.
 * - `begin` with `resume` for a transfer that was checked answers like `finish` (VERIFIED, with the
 * proof; `sealed` tells whether the magic is in): the checks are made again.
 * - Applying is not a write: the nRF5340 starts the slot with the slot line. Nothing here changes
 * on `apply`.
 */

#ifndef SENSEI_GAP9_G9OTA_H_
#define SENSEI_GAP9_G9OTA_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei/gap9_ota.pb.h"
#include "sensei_gap9/g9ota_ckpt.h"
#include "sensei_link/sha256.h"

/** Largest Gap9OtaData.data. (G9OTA_SECTOR, the checkpoint record and its states: g9ota_ckpt.h.) */
#define G9OTA_CHUNK_MAX 1024u
/** The oldest job protocol major of a GAP9 app that takes update requests (deferred sealing, the seal job). An app
 * with a lower major runs the normal jobs, but the nRF5340 does not send it update requests (-EPROTONOSUPPORT). */
#define G9OTA_APP_MAJOR_MIN 2u
/** Shortest time between two fresh transfers, ms (the slot's sectors wear out). Needs g9ota_touch().
 */
#define G9OTA_OPEN_MIN_MS 10000u

/** The MRAM. Return 0 or a negative errno. `erase` takes the address of one G9OTA_SECTOR, `program`
 * an address and length that are multiples of 16 (the MRAM word) inside an erased range. */
struct g9ota_flash {
  int (*read)(void *ctx, uint32_t addr, void *buf, uint32_t len);
  int (*erase)(void *ctx, uint32_t addr);
  int (*program)(void *ctx, uint32_t addr, const void *buf, uint32_t len);
};

/** Where things are in the MRAM (sensei_gap9/gap9_slot.h): two slots and two checkpoint sectors; and
 * the board id that an image must carry in its kind TLV (MCUboot TLV 0xa0, for example
 * "sensei-ieq-gap9"). An image of another kind is refused (GAP9_OTA_FAIL_KIND). `kind` is at most
 * SENSEI_IMAGE_KIND_MAX (48) bytes long (g9ota_init() refuses a longer one) and stays valid for the life of the
 * writer; NULL refuses every image. */
struct g9ota_cfg {
  uint32_t slot_base[2];
  uint32_t slot_size;
  uint32_t ckpt_base[2];
  const char *kind;
};

struct g9ota {
  const struct g9ota_flash *fl;
  void *ctx;
  struct g9ota_cfg cfg;
  uint8_t booted; /* the slot this application runs from: never written */
  uint8_t target; /* the other one */

  sensei_Gap9OtaStage stage;
  sensei_Gap9OtaFail fail;
  int detail;
  uint32_t size;
  uint8_t sha[32];
  uint8_t hdr0[16]; /* the first 16 bytes of the file: not programmed until `seal` (seal_header) */
  uint32_t offset;  /* next byte wanted */
  uint32_t flushed; /* bytes that are in the slot (a multiple of G9OTA_SECTOR, or size at the end) */
  uint32_t nbuf;    /* bytes in buf: offset - flushed */

  /* checkpoint log */
  bool ck_loaded;
  struct g9ota_ckpt ck; /* the newest valid record */
  uint32_t ck_seq;      /* its sequence number */
  int ck_sector;        /* the sector the next record goes to, -1: not decided */
  uint32_t ck_next;     /* index of the next free record in that sector */

  /* after a successful finish */
  uint8_t image_hash[32];
  uint8_t signature[64];
  uint8_t key_hash[32];
  bool have_key_hash;
  sensei_Version image_version; /* from the header of the finished image (the signature covers it) */
  bool sealed;                  /* the magic is written */

  bool timed;
  bool opened;
  uint32_t now_ms;
  uint32_t open_ms;
  uint32_t writes_outside; /* writes the guard refused: must stay 0 */

  uint8_t buf[G9OTA_SECTOR];
};

/** Decide whether the writer may run. `handoff` is the slot of the hand-off record that the SSBL
 * leaves (< 0: no record). The writer is armed only for a record whose slot holds a complete image
 * (header, TLV areas, container): that proves the SSBL started this application from a slot of the
 * A/B layout, so the other slot and the checkpoint sectors are free. Anything else (the
 * single-image layout of `make flash`, where the app and its readfs lie where the checkpoint and
 * slot A are; a JTAG load; a reset without the SSBL) writes nothing. The slot line is no evidence.
 * Returns the slot this application runs from (0 or 1), or a negative errno. */
int g9ota_arm(const struct g9ota_flash *fl, void *ctx, const struct g9ota_cfg *cfg, int handoff);

/** Check a layout: `kind` is NULL or at most SENSEI_IMAGE_KIND_MAX bytes, the slot size is a multiple of G9OTA_SECTOR,
 * the two slots and the two checkpoint sectors are sector aligned and share no byte (so the checkpoint sectors and the
 * target never overlap the booted slot), and `booted_slot` is 0 or 1. Returns 0 or -EINVAL. */
int g9ota_cfg_check(const struct g9ota_cfg *cfg, uint8_t booted_slot);

/** Choose the layout for the writer. `real` is the A/B layout, `test_area` a layout for the local test or NULL.
 * Without `test_area` this is g9ota_arm() plus g9ota_cfg_check(); it fills `out` with `real` and returns the slot
 * this application runs from. With `test_area` it returns 0 and fills `out` with the area (and the kind of `real`)
 * only if `test_allowed` (the build has the local test: a compile-time choice of the app) and `handoff` is < 0
 * (no record: the single-image layout); else -EPERM. A layout that fails g9ota_cfg_check() is -EINVAL. */
int g9ota_select(const struct g9ota_flash *fl, void *ctx, const struct g9ota_cfg *real,
                 const struct g9ota_cfg *test_area, bool test_allowed, int handoff, struct g9ota_cfg *out);

/** Set up the writer. `fl` needs all three operations. Returns 0, or -EINVAL for a missing operation or a layout that
 * fails g9ota_cfg_check(); `o` is then not usable. */
int g9ota_init(struct g9ota *o, const struct g9ota_flash *fl, void *ctx, const struct g9ota_cfg *cfg,
               uint8_t booted_slot);

/** Tell the core the time (any ms clock, wraps after 49 days) before each request; without it the
 * rate rule is off. */
void g9ota_touch(struct g9ota *o, uint32_t now_ms);

/** Call once after g9ota_init() when the application starts: the record of a finished transfer into
 * the slot that runs now is cleared (the image is running: nothing waits for apply, and an abort
 * must not erase a slot that is only the way back). */
void g9ota_boot_note(struct g9ota *o);

/** Each call fills `info` and returns its `result`. A request that does not fit is refused
 * (INVALID) and changes nothing, except `abort`. `begin` with `resume`, a repeated `finish` and a
 * repeated chunk are answered again. */
sensei_Gap9OtaResult g9ota_begin(struct g9ota *o, const sensei_Gap9OtaBegin *req, sensei_Gap9OtaInfo *info);
sensei_Gap9OtaResult g9ota_chunk(struct g9ota *o, const sensei_Gap9OtaData *req, sensei_Gap9OtaInfo *info);
sensei_Gap9OtaResult g9ota_finish(struct g9ota *o, sensei_Gap9OtaInfo *info);
/** Write the MCUboot magic of the finished transfer (stage VERIFIED): the nRF5340 sends it after it checked the
 * signature. `req->image_hash` must be the hash that `finish` answered, else INVALID. Repeating it is answered OK. */
sensei_Gap9OtaResult g9ota_seal(struct g9ota *o, const sensei_Gap9OtaSeal *req, sensei_Gap9OtaInfo *info);
sensei_Gap9OtaResult g9ota_abort(struct g9ota *o, sensei_Gap9OtaInfo *info);
/** Look at a slot as `finish` does (container, kind, image hash TLV against the bytes, app
 * descriptor, signature shape) without a transfer, and answer the hash, signature and key hash for
 * the node's signature check. OK, FAILED
 * (`fail` says why: the transfer's state is not touched) or INVALID while a transfer is open or cut
 * in that slot. */
sensei_Gap9OtaResult g9ota_verify(struct g9ota *o, uint8_t slot, sensei_Gap9OtaInfo *info);
sensei_Gap9OtaResult g9ota_status(struct g9ota *o, sensei_Gap9OtaInfo *info);

/** One request of the nRF5340 to the writer: the six update jobs. The app takes it out of its own job
 * message (the nRF5340 puts it in), and answers with the Gap9OtaInfo. */
enum g9ota_op {
  G9OTA_OP_BEGIN,
  G9OTA_OP_CHUNK,
  G9OTA_OP_STATUS, /* also the verify of a slot */
  G9OTA_OP_FINISH,
  G9OTA_OP_ABORT,
  G9OTA_OP_SEAL,
};

struct g9ota_req {
  enum g9ota_op op;
  union {
    sensei_Gap9OtaBegin begin;
    sensei_Gap9OtaData chunk;
    sensei_Gap9OtaStatus status;
    sensei_Gap9OtaSeal seal;
  } u;
};

/** Run one request: touches the clock with `now_ms` (the rate rule), calls the matching function above
 * and fills `info`. Returns 0, or -EINVAL for a request that is no job (unknown op, a verify of a
 * slot other than 0 and 1: the app answers BAD_JOB). The verdict is in `info->result`. */
int g9ota_dispatch(struct g9ota *o, const struct g9ota_req *req, uint32_t now_ms, sensei_Gap9OtaInfo *info);

/** The address guard: true if [addr, addr + len) lies in the slot that is not running or in a
 * checkpoint sector. */
bool g9ota_addr_ok(const struct g9ota *o, uint32_t addr, uint32_t len);

/** Look at a slot: header, container and kind. Returns false (and a zero `out`) for an empty or
 * damaged slot, and for the slot a transfer is open into. */
bool g9ota_slot_info(struct g9ota *o, uint8_t slot, sensei_Gap9Slot *out);

/** The version (major.minor.revision packed as MCUboot compares them) of a MCUboot header; false if
 * it is none. */
bool g9ota_header_version(const uint8_t hdr[32], uint32_t *packed);

/** ECDSA signature TLV (DER SEQUENCE of two INTEGER) to r || s (64 B). false if it is not a P-256
 * signature. */
bool g9ota_sig_der_to_raw(const uint8_t *der, size_t len, uint8_t out[64]);

#endif /* SENSEI_GAP9_G9OTA_H_ */
