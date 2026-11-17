<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# sensei_gas_index

VOC and NOx index from SGP41 raw ticks. A thin wrapper (`include/sensei_gas_index.h`) around the Sensirion gas
index algorithm, which keeps state across samples. Floating point, needs `libm`. Plain C, no Zephyr.

- `sensei_gas_index_init(g, interval_s)` starts both algorithms. `interval_s` is the time between two samples
  (Sensirion default 1 s). The library counts time as calls times the interval: with 1 s the 45 s blackout (index 0)
  ends after 46 calls, with 60 s after the first call. The index under sparse sampling or a gated heater is not
  Sensirion-validated; compare it with continuous 1 s operation.
- `sensei_gas_index_process(g, sraw_voc, sraw_nox, &voc_index, &nox_index)` feeds one sample and returns true once an
  index is above 0.

The vendor code is in `third_party/sensirion_gas_index_algorithm` (Sensirion `gas-index-algorithm` 3.2.2, BSD-3-Clause,
floating-point version, unchanged).

## Use

**Zephyr**: `CONFIG_SENSEI_GAS_INDEX=y`.

**Host or plain CMake**: `add_subdirectory(<sdk>/lib/sensei_gas_index sensei_gas_index)` and link `sensei_gas_index`.

## Tests

`make test-host` at the SDK root, or `cmake -S lib/sensei_gas_index/tests -B build && cmake --build build &&
ctest --test-dir build`. The test checks the blackout length at 1 s and 60 s, the settled indices for constant ticks
and the restart.
