<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# sensei_pwr_math

Pure logic of the power module in `NRF/system/pwr`. Plain C, no Zephyr, so the firmware and the host tests use the
same code.

- `sensei_battery_perc(chem, mv)`: charge estimate from the battery voltage. `SENSEI_BATTERY_LISOCL2` is the
  Saft LS33600 primary cell of the SENSEI board (flat curve around 3.6 V, so only a hint), `SENSEI_BATTERY_LIION` a
  4.2 V Li-ion or LiPo cell.
- `sensei_button_down()`, `sensei_button_up()`: press detection of the nEN button. A key-up without a key-down is
  ignored, a new key-down replaces an older one (its key-up was missed), a press longer than the limit is
  `SENSEI_BUTTON_LONG`.

## Use

**Zephyr**: `CONFIG_SENSEI_PWR_MATH=y` (`CONFIG_SENSEI_PWR` selects it).

**Host or plain CMake**: `add_subdirectory(<sdk>/lib/sensei_pwr_math sensei_pwr_math)` and link `sensei_pwr_math`.

## Tests

`make test-host` at the SDK root, or `cmake -S lib/sensei_pwr_math/tests -B build && cmake --build build &&
ctest --test-dir build`.
