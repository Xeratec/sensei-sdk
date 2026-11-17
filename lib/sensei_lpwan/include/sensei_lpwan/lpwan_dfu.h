/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_dfu.h
 * @brief Rules of the shield update on the nRF9151 side: one transfer at a time, offsets, resume, the SHA-256 check
 *        and the checkpoints. No Zephyr: the flash and the modem are behind lpwan_dfu_ops, the logic runs on the host
 *        in the tests (RAM flash).
 *
 * Flow (messages in lpwan.proto): begin, chunk ... chunk, finish, apply.
 *
 * - The bytes arrive in order. `offset` in DfuStatus is the next byte wanted. A chunk at a lower offset is a repeat
 *   (it is answered, nothing is written twice); one at a higher offset is refused (DFU_FAIL_OFFSET) unless it
 *   has `gap` set: then the bytes in between are 0xFF, were not sent, and are erased (app image only: the padding of
 *   a merged TF-M and app image is half of it).
 * - APP: the flash sectors are erased when the transfer reaches them, never before. After every sector
 *   (LPWAN_DFU_SECTOR) a checkpoint is saved (offset, size, SHA-256 of the file), so a transfer survives a restart of
 *   the shield: `begin` with `resume` finds it and answers the offset; the sender continues there, at most one sector
 *   back. At `finish` the written bytes are read back and hashed, so the check covers what is in the flash.
 * - MODEM: the delta patch goes to the modem in order. The SHA-256 is computed while it arrives, so a transfer that
 *   the shield forgot (restart) starts again at 0; the modem checks the signature of the patch itself.
 * - A request that does not fit the state is refused and changes nothing, except `abort`.
 */

#ifndef SENSEI_LPWAN_DFU_H_
#define SENSEI_LPWAN_DFU_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei/lpwan.pb.h"
#include "sensei_link/sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Erase unit of the app flash (nRF91: 4 kB). Checkpoints follow it. */
#define LPWAN_DFU_SECTOR 4096u
/** Largest DfuChunk.data the shield takes. */
#define LPWAN_DFU_CHUNK_MAX 512u
/** Bytes of one checkpoint record in the log (a sector holds LPWAN_DFU_SECTOR / LPWAN_DFU_CKPT_LEN of them). */
#define LPWAN_DFU_CKPT_LEN 64u

/** Bits of LpwanVersion.dfu_targets. */
#define LPWAN_DFU_TARGET_APP_BIT 1u
#define LPWAN_DFU_TARGET_MODEM_BIT 2u

struct lpwan_dfu;

/**
 * What the core needs from a target (one table for the app, one for the modem). Return 0 or a negative errno
 * (the modem may return its positive error code). Optional hooks may be NULL.
 *
 * App: `erase`, `write`, `read`, the `log_*` hooks (a sector of its own for the checkpoints), `read_headers` and
 * `apply` are set. Modem: `erase` and `read` are NULL (no gap, the hash is made while the patch arrives, no
 * checkpoints) and `open`, `busy`, `finish`, `apply` and `apply_state` do the work.
 */
struct lpwan_dfu_ops {
  /** Bytes the target takes (the slot, or the modem DFU area); 0: the target is not built in. */
  uint32_t (*capacity)(void *ctx, sensei_DfuTarget t);
  /** A new transfer starts (`fresh`) or an open one is resumed. Modem: starts the erase of its DFU area. */
  int (*open)(void *ctx, sensei_DfuTarget t, uint32_t size, bool fresh);
  /** > 0 while the target is not ready (modem erase running), < 0 on an error, 0 ready. */
  int (*busy)(void *ctx);
  /** Erase [off, off + len), both multiples of LPWAN_DFU_SECTOR. */
  int (*erase)(void *ctx, uint32_t off, uint32_t len);
  /** Write at `off` (a multiple of 4), `len` a multiple of 4. The modem takes the stream in order. */
  int (*write)(void *ctx, uint32_t off, const uint8_t *data, size_t len);
  int (*read)(void *ctx, uint32_t off, uint8_t *data, size_t len);
  /** The checkpoint log: one erase unit, written in records of LPWAN_DFU_CKPT_LEN. Offsets are in the log. */
  int (*log_read)(void *ctx, uint32_t off, uint8_t *data, size_t len);
  int (*log_write)(void *ctx, uint32_t off, const uint8_t *data, size_t len);
  int (*log_erase)(void *ctx);
  /** All bytes are in (modem: close the write). */
  int (*finish)(void *ctx, sensei_DfuTarget t);
  /** App: the 32 byte MCUboot headers of the new image (slot 1) and of the running one (slot 0), for the checks. */
  int (*read_headers)(void *ctx, uint8_t new_hdr[32], uint8_t running_hdr[32]);
  /** App: mark the image for one trial boot. Modem: start the update (runs on, see apply_state). */
  int (*apply)(void *ctx, sensei_DfuTarget t);
  /** Modem: 0 running, 1 done, < 0 failed (detail). App: not used. */
  int (*apply_state)(void *ctx, int *detail);
  /** Called between erases and while reading back, so a watchdog can be fed. */
  void (*kick)(void *ctx);
};

struct lpwan_dfu {
  const struct lpwan_dfu_ops *app;   /* NULL: the app cannot be updated */
  const struct lpwan_dfu_ops *modem; /* NULL: the modem firmware cannot be updated */
  const struct lpwan_dfu_ops *ops;   /* the table of the open transfer's target */
  void *ctx;

  sensei_DfuState state;
  sensei_DfuTarget target;
  uint32_t size;
  uint8_t sha[32];
  uint32_t offset;    /* next byte wanted */
  uint32_t erased_to; /* sectors below are erased (app) */
  uint8_t tail[4];    /* bytes after the last whole word, not written yet */
  uint8_t ntail;
  sensei_sha256_t hash; /* modem: hash of the bytes so far */
  uint32_t ckpt_off;    /* last offset saved */
  int log_next;         /* next free record in the log, -1: not looked at yet */
  sensei_DfuFail fail;
  int detail;
};

/** Checkpoint record: where a transfer stands. */
struct lpwan_dfu_ckpt {
  sensei_DfuTarget target;
  uint32_t size;
  uint32_t offset; /* multiple of LPWAN_DFU_SECTOR */
  uint8_t sha[32];
};

void lpwan_dfu_init(struct lpwan_dfu *d, const struct lpwan_dfu_ops *app, const struct lpwan_dfu_ops *modem, void *ctx);

/* Each call fills `st` with the status to send back and returns the LpwanResult for the answer. */
sensei_LpwanResult lpwan_dfu_begin(struct lpwan_dfu *d, const sensei_DfuBegin *req, sensei_DfuStatus *st);
sensei_LpwanResult lpwan_dfu_chunk(struct lpwan_dfu *d, const sensei_DfuChunk *req, sensei_DfuStatus *st);
sensei_LpwanResult lpwan_dfu_finish(struct lpwan_dfu *d, sensei_DfuStatus *st);
sensei_LpwanResult lpwan_dfu_apply(struct lpwan_dfu *d, sensei_DfuStatus *st);
sensei_LpwanResult lpwan_dfu_abort(struct lpwan_dfu *d, sensei_DfuStatus *st);
sensei_LpwanResult lpwan_dfu_status(struct lpwan_dfu *d, sensei_DfuStatus *st);

/** Targets the ops table can serve (LPWAN_DFU_TARGET_*_BIT), for LpwanVersion.dfu_targets. */
uint32_t lpwan_dfu_targets(struct lpwan_dfu *d);

/* ---- Pure helpers (also tested alone) ---- */

/** MCUboot header checks of a finished app image of `size` bytes: DFU_FAIL_NONE, DFU_FAIL_IMAGE (not an MCUboot
 *  image, or the sizes do not fit the file) or DFU_FAIL_DOWNGRADE (older than the running image; the same version
 *  is allowed). A running header that is not an MCUboot header (a development flash) skips the version check. */
sensei_DfuFail lpwan_dfu_check_header(const uint8_t new_hdr[32], uint32_t size, const uint8_t running_hdr[32]);

/** Version (major.minor.revision) of an MCUboot header; false if it is not a header. */
bool lpwan_dfu_header_version(const uint8_t hdr[32], uint8_t *major, uint8_t *minor, uint16_t *revision);

void lpwan_dfu_ckpt_encode(const struct lpwan_dfu_ckpt *c, uint8_t out[LPWAN_DFU_CKPT_LEN]);
/** False if the record is empty, has the wrong magic or fails its CRC. */
bool lpwan_dfu_ckpt_decode(const uint8_t in[LPWAN_DFU_CKPT_LEN], struct lpwan_dfu_ckpt *c);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_DFU_H_ */
