/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The codec of the recovery jobs for the link state machine of the nRF5340 (gap9_core.h, g9_set_recovery()). */

#include <errno.h>
#include <string.h>

#include "sensei_gap9/g9rec.h"
#include "sensei_gap9/gap9_core.h"

int g9rec_job_from_req(const struct g9ota_req *req, sensei_Gap9RecoveryJob *job) {
  memset(job, 0, sizeof(*job));
  switch (req->op) {
  case G9OTA_OP_BEGIN:
    job->which_body = sensei_Gap9RecoveryJob_begin_tag;
    job->begin = req->u.begin;
    return 0;
  case G9OTA_OP_CHUNK:
    job->which_body = sensei_Gap9RecoveryJob_chunk_tag;
    job->chunk = req->u.chunk;
    return 0;
  case G9OTA_OP_STATUS:
    job->which_body = sensei_Gap9RecoveryJob_status_tag;
    job->status = req->u.status;
    return 0;
  case G9OTA_OP_FINISH:
    job->which_body = sensei_Gap9RecoveryJob_finish_tag;
    return 0;
  case G9OTA_OP_ABORT:
    job->which_body = sensei_Gap9RecoveryJob_abort_tag;
    return 0;
  case G9OTA_OP_SEAL:
    job->which_body = sensei_Gap9RecoveryJob_seal_tag;
    job->seal = req->u.seal;
    return 0;
  default:
    return -EINVAL;
  }
}

static int32_t job_encode(uint8_t *buf, size_t cap, const void *job) {
  return sensei_encode_Gap9RecoveryJob(buf, cap, job);
}

static bool result_decode(const uint8_t *buf, size_t len, void *res) {
  return sensei_decode_Gap9RecoveryResult(buf, len, res);
}

static uint32_t job_seq(const void *job) { return ((const sensei_Gap9RecoveryJob *)job)->seq; }

static void job_set_seq(void *job, uint32_t seq) { ((sensei_Gap9RecoveryJob *)job)->seq = seq; }

static uint32_t result_seq(const void *res) { return ((const sensei_Gap9RecoveryResult *)res)->seq; }

static int result_err(const void *res) {
  return ((const sensei_Gap9RecoveryResult *)res)->status == sensei_Gap9RecoveryStatus_GAP9_RECOVERY_OK ? 0 : -EINVAL;
}

static bool job_uses_bus(const void *job) {
  (void)job;
  return false;
}

const struct g9_codec g9rec_codec = {
    .job_size = sizeof(sensei_Gap9RecoveryJob),
    .result_size = sizeof(sensei_Gap9RecoveryResult),
    .encode_job = job_encode,
    .decode_result = result_decode,
    .job_seq = job_seq,
    .job_set_seq = job_set_seq,
    .result_seq = result_seq,
    .result_err = result_err,
    .job_uses_bus = job_uses_bus,
    .gap9_major = G9L_RECOVERY_PROTO,
};
