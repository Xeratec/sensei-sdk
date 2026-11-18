/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Checks the C codec against the vectors that the Python one writes (generated/vectors.h, from
 * python -m sensei_sdk.vectors): packed fault records, the text of reset causes, the names of the reasons. */
#include <stdio.h>
#include <string.h>

#include "generated/vectors.h"
#include "sensei_diag.h"

static int fails;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                              \
      fails++;                                                                                                         \
    }                                                                                                                  \
  } while (0)

static void test_fault_records(void) {
  for (int i = 0; i < VEC_FAULTS_COUNT; i++) {
    const vec_fault_t *v = &vec_faults[i];
    struct sensei_diag_fault f;
    uint8_t packed[SENSEI_DIAG_FAULT_BYTES];

    /* the record of Python unpacks to the same fields in C */
    CHECK(sensei_diag_fault_unpack(v->packed, SENSEI_DIAG_FAULT_BYTES, &f) == 0);
    CHECK(f.reason == v->reason && f.flags == v->flags && f.last_job == v->last_job && f.pc == v->pc && f.lr == v->lr &&
          f.status == v->status && f.uptime_s == v->uptime_s && f.image_id == v->image_id &&
          f.boot_count == v->boot_count && strcmp(f.thread, v->thread) == 0);
    /* and C packs those fields to the same bytes */
    sensei_diag_fault_pack(&f, packed);
    CHECK(memcmp(packed, v->packed, sizeof(packed)) == 0);
  }
}

static void test_reset_text(void) {
  for (int i = 0; i < VEC_RESETS_COUNT; i++) {
    char buf[128];

    sensei_diag_reset_format(vec_resets[i].cause, buf, sizeof(buf));
    CHECK(strcmp(buf, vec_resets[i].text) == 0);
  }
}

static void test_reason_names(void) {
  for (unsigned r = 0; r < 256; r++) {
    CHECK(strcmp(sensei_diag_fault_reason_name((uint8_t)r), vec_reason_names[r]) == 0);
  }
}

int main(void) {
  test_fault_records();
  test_reset_text();
  test_reason_names();
  if (fails != 0) {
    printf("%d checks failed\n", fails);
    return 1;
  }
  printf("diag vectors ok\n");
  return 0;
}
