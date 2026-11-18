/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file g9rec.h
 * @brief The job server of the recovery in the GAP9 boot loader (GAP9/ssbl): the update writer (g9ota.h) behind the
 * jobs of `Gap9RecoveryJob` (gap9_ota.proto). No OS, no GAP SDK: the SSBL, the GVSoC test and the host tests run it.
 *
 * The writer runs with `booted` = the keep slot, the slot the nRF5340 has on the slot line (the confirmed one). The
 * keep slot, the checkpoint sectors' neighbours and the boot area are never written (the guard of g9ota.c); the
 * transfer goes into the other slot. The server checks no signature: the nRF5340 does, then sends `seal`. A job
 * with the `seq` of the last one is answered again from a copy, not run twice (the answer got lost).
 */

#ifndef SENSEI_GAP9_G9REC_H_
#define SENSEI_GAP9_G9REC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei_gap9/g9ota.h"
#include "sensei_link/pb.h"

SENSEI_PB_DECLARE(Gap9RecoveryJob)
SENSEI_PB_DECLARE(Gap9RecoveryResult)

struct g9_codec;

/** The codec of the recovery jobs for the nRF5340 link (src/g9rec_codec.c): g9_set_recovery(&g, &g9rec_codec). */
extern const struct g9_codec g9rec_codec;

struct g9rec {
  struct g9ota *o; /* the writer, set up by the caller with booted = keep */
  bool have_last;
  uint32_t last_seq;
  sensei_Gap9RecoveryResult last; /* the answer to last_seq */
};

/** Serve `writer` (already g9ota_init()ed with the keep slot). */
void g9rec_init(struct g9rec *r, struct g9ota *writer);

/** The recovery job for a request of the update flow (what the nRF5340 glue sends; in g9rec_codec.c). Returns 0, or
 * -EINVAL for an op that is no job. `seq` is left 0: the link numbers the jobs. */
int g9rec_job_from_req(const struct g9ota_req *req, sensei_Gap9RecoveryJob *job);

/** Decode a job frame payload (the signature of g9l_decode_fn). */
bool g9rec_decode(const uint8_t *payload, size_t len, void *job);

/**
 * Run one job and fill `res` (seq, status, info). `now_ms` is any ms clock (the rate rule of the writer). A job with
 * the seq of the previous one is not run again: `res` is the previous answer. Returns 0, or -EINVAL for a job with no
 * body of this protocol (status BAD_JOB).
 */
int g9rec_run(struct g9rec *r, const sensei_Gap9RecoveryJob *job, uint32_t now_ms, sensei_Gap9RecoveryResult *res);

/** Encode a result into `buf`. Returns the length or a negative value. */
int32_t g9rec_encode(const sensei_Gap9RecoveryResult *res, uint8_t *buf, size_t cap);

#endif /* SENSEI_GAP9_G9REC_H_ */
