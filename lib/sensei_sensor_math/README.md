<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# sensei_sensor_math

Unit conversions and timings of the sensor shield drivers in `NRF/drivers/sensor`. Plain C, no Zephyr, so the node
firmware and the host tests use the same code.

- `sensei_sgp41_rh_ticks()`, `sensei_sgp41_t_ticks()`: SGP41 compensation ticks from %RH and degC.
- `sensei_as7331_temp_c()`, `sensei_as7331_uw_cm2()`: AS7331 die temperature and irradiance in uW/cm2 from counts, gain
  and integration time (datasheet DS001047 v4-00, Figure 7).
- `sensei_scd41_asc_hours()`: SCD41 ASC periods scaled to the single-shot period.
- `sensei_bh1730_meas_ms()`: BH1730 measurement time for an ITIME value.
- `sensei_lis2duxs12_wake_ths()`: LIS2DUXS12 wake-up threshold as the 6-bit register field and step weight.

## Use

**Zephyr**: `CONFIG_SENSEI_SENSOR_MATH=y` (the shield drivers select it).

**Host or plain CMake**: `add_subdirectory(<sdk>/lib/sensei_sensor_math sensei_sensor_math)` and link `sensei_sensor_math`.

## Tests

`make test-host` at the SDK root, or `cmake -S lib/sensei_sensor_math/tests -B build && cmake --build build &&
ctest --test-dir build`.
