/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_gap9/g9rec.h"

#include <errno.h>
#include <string.h>

void g9rec_init(struct g9rec *r, struct g9ota *writer) {
  memset(r, 0, sizeof(*r));
  r->o = writer;
}

bool g9rec_decode(const uint8_t *payload, size_t len, void *job) {
  return sensei_decode_Gap9RecoveryJob(payload, len, (sensei_Gap9RecoveryJob *)job);
}

int32_t g9rec_encode(const sensei_Gap9RecoveryResult *res, uint8_t *buf, size_t cap) {
  return sensei_encode_Gap9RecoveryResult(buf, cap, res);
}

int g9rec_run(struct g9rec *r, const sensei_Gap9RecoveryJob *job, uint32_t now_ms, sensei_Gap9RecoveryResult *res) {
  struct g9ota_req req;

  if (r->have_last && job->seq == r->last_seq) {
    *res = r->last; /* the answer was lost: say it again, do not run the job twice */
    return res->status == sensei_Gap9RecoveryStatus_GAP9_RECOVERY_OK ? 0 : -EINVAL;
  }
  memset(&req, 0, sizeof(req));
  switch (job->which_body) {
  case sensei_Gap9RecoveryJob_begin_tag:
    req.op = G9OTA_OP_BEGIN;
    req.u.begin = job->begin;
    break;
  case sensei_Gap9RecoveryJob_chunk_tag:
    req.op = G9OTA_OP_CHUNK;
    req.u.chunk = job->chunk;
    break;
  case sensei_Gap9RecoveryJob_status_tag:
    req.op = G9OTA_OP_STATUS;
    req.u.status = job->status;
    break;
  case sensei_Gap9RecoveryJob_finish_tag:
    req.op = G9OTA_OP_FINISH;
    break;
  case sensei_Gap9RecoveryJob_abort_tag:
    req.op = G9OTA_OP_ABORT;
    break;
  case sensei_Gap9RecoveryJob_seal_tag:
    req.op = G9OTA_OP_SEAL;
    req.u.seal = job->seal;
    break;
  default:
    memset(res, 0, sizeof(*res));
    res->seq = job->seq;
    res->status = sensei_Gap9RecoveryStatus_GAP9_RECOVERY_BAD_JOB;
    return -EINVAL;
  }
  memset(res, 0, sizeof(*res));
  res->seq = job->seq;
  if (g9ota_dispatch(r->o, &req, now_ms, &res->info) != 0) {
    res->status = sensei_Gap9RecoveryStatus_GAP9_RECOVERY_BAD_JOB;
    memset(&res->info, 0, sizeof(res->info));
    r->last = *res;
    r->last_seq = job->seq;
    r->have_last = true;
    return -EINVAL;
  }
  res->status = sensei_Gap9RecoveryStatus_GAP9_RECOVERY_OK;
  res->has_info = true;
  r->last = *res;
  r->last_seq = job->seq;
  r->have_last = true;
  return 0;
}
