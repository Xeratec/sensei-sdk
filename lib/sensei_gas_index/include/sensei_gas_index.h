/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_gas_index.h
 * @brief VOC and NOx index from SGP41 raw ticks (Sensirion gas index algorithm, floating point).
 *
 * The algorithms keep state across samples and count time as calls times the sampling interval. Call
 * sensei_gas_index_process() once per sample, always with the interval given to sensei_gas_index_init().
 * Plain C, no Zephyr.
 */

#ifndef SENSEI_GAS_INDEX_H
#define SENSEI_GAS_INDEX_H

#include <stdbool.h>
#include <stdint.h>

#include "sensirion_gas_index_algorithm.h"

#ifdef __cplusplus
extern "C" {
#endif

struct sensei_gas_index {
  GasIndexAlgorithmParams voc;
  GasIndexAlgorithmParams nox;
};

/**
 * @brief Start both algorithms from scratch.
 * @param sampling_interval_s time between two process calls in s (Sensirion default 1.0). The learning times
 *        of the library scale with it; with an interval of 60 s the 45 s blackout ends at the first call.
 */
void sensei_gas_index_init(struct sensei_gas_index *g, float sampling_interval_s);

/**
 * @brief Feed one SGP41 sample.
 * @param sraw_voc SRAW_VOC ticks
 * @param sraw_nox SRAW_NOX ticks (not valid while the sensor is still in conditioning)
 * @param voc_index out, 1 to 500, 0 during the blackout
 * @param nox_index out, 1 to 500, 0 during the blackout
 * @return true once an index is above 0 (the blackout is over)
 */
bool sensei_gas_index_process(struct sensei_gas_index *g, uint16_t sraw_voc, uint16_t sraw_nox, int32_t *voc_index,
                              int32_t *nox_index);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_GAS_INDEX_H */
