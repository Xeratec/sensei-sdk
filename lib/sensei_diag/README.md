<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# sensei_diag

Pure part of the field diagnostics in `NRF/system/diag`. Plain C, no Zephyr, so the firmware, the host tests and the
host tools use the same layout.

- **Reset cause bits** (`SENSEI_RESET_*`): bits 0 to 16 are the `RESET_*` flags of the Zephyr hwinfo API
  (checked at build time in `NRF/system/diag`), bit 24 `FAULT` (a fault record was found), bit 25 `UNKNOWN` (no hwinfo).
  `sensei_diag_reset_map()` maps the hwinfo flags: the nRF reset reason register is empty after a power-on or brown-out
  reset and hwinfo then gives no flag, so no flags means `POR` (a brown-out cannot be told apart).
  `sensei_diag_reset_format()` gives the text (`SOFT|FAULT`).
- **Fault record** (`struct sensei_diag_fault`): reason, PC, LR, xPSR, uptime, image id, boot counter, last job id, thread
  name. `sensei_diag_fault_pack()` and `_unpack()` give the 52 B form that goes into the retained RAM and the settings
  (little endian, magic, version, CRC-32). The unpack tells a foreign RAM content (`-ENOMSG`) from a damaged record
  (`-EBADMSG`). `tools/host/sensei_host/diag.py` reads the same bytes.
- **Boot state**: `sensei_diag_boot_count_next()` counts a boot in a 12 B state (starts at 1 if the state is not valid).

## Use

**Zephyr**: `CONFIG_SENSEI_DIAG_CODEC=y` (`CONFIG_SENSEI_DIAG` selects it).

**Host or plain CMake**: `add_subdirectory(<sdk>/lib/sensei_diag sensei_diag)` and link `sensei_diag`.

## Tests

`make test-host` at the SDK root, or `cmake -S lib/sensei_diag/tests -B build && cmake --build build &&
ctest --test-dir build`.
