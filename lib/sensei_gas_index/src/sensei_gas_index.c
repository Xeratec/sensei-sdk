/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <stddef.h>

#include "sensei_gas_index.h"

void sensei_gas_index_init(struct sensei_gas_index *g, float sampling_interval_s) {
  GasIndexAlgorithm_init_with_sampling_interval(&g->voc, GasIndexAlgorithm_ALGORITHM_TYPE_VOC, sampling_interval_s);
  GasIndexAlgorithm_init_with_sampling_interval(&g->nox, GasIndexAlgorithm_ALGORITHM_TYPE_NOX, sampling_interval_s);
}

bool sensei_gas_index_process(struct sensei_gas_index *g, uint16_t sraw_voc, uint16_t sraw_nox, int32_t *voc_index,
                              int32_t *nox_index) {
  int32_t voc = 0;
  int32_t nox = 0;

  GasIndexAlgorithm_process(&g->voc, (int32_t)sraw_voc, &voc);
  GasIndexAlgorithm_process(&g->nox, (int32_t)sraw_nox, &nox);
  if (voc_index != NULL) {
    *voc_index = voc;
  }
  if (nox_index != NULL) {
    *nox_index = nox;
  }
  /* Both algorithms output 0 during their initial blackout (45 s of algorithm time). */
  return voc > 0 || nox > 0;
}
