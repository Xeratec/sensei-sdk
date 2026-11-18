<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# sensei_gap9

Link between the nRF5340 and the GAP9 of the SENSEI base board. The nRF5340 sends jobs, GAP9 runs them and sends results.
GAP9 is powered only while jobs are pending. The wires, frames and timing are in [PROTOCOL.md](PROTOCOL.md).

The link moves opaque job and result messages. The app describes them with a `struct g9_codec` (encode, decode, sequence
numbers, status to errno, which job needs the camera bus), so any message schema works. The SENSEI IEQ node uses
`Gap9Job` and `Gap9Result` (`firmware/nrf5340/src/gap9/gap9_codec.c`).

| Folder | Side | What |
|---|---|---|
| `include/sensei_gap9/gap9_link.h` | both | frame types, flags, buffer sizes, timing constants |
| `include/sensei_gap9/gap9_slot.h` | all | A/B firmware slots in the GAP9 MRAM: layout, slot format, checks of the SSBL, hand-off record of the SSBL (header only) |
| `src/gap9_core.c`, `gap9_core.h` | nRF5340 | protocol state machine: job queue, boot, frames, retries, timeouts, bus lock, idle power-off, hold, reboot (no OS, host tested) |
| `zephyr/` | nRF5340 | `gap9_link.c` (thread, SPIS3, trigger line, slot line, power, shared SPI_A), `gap9_shell.c`, `gap9_link.overlay` |
| `gap9/gap9_side.c`, `gap9_side.h` | GAP9 | SPI master, trigger line, hello, fetch, result, bus free (GAP SDK) |
| `include/sensei_gap9/gap9_trial.h` | nRF5340 | the rule of the trial boot of a slot (pure function) |
| `src/g9ota.c`, `g9ota.h` | GAP9 | writer of the firmware update: one transfer into the other slot (no OS, no GAP SDK, host tested) |
| `gap9/g9ota_mram.c`, `g9ota_mram.h` | GAP9 | MRAM backend of the writer, slot detection, readfs of the slot (GAP SDK) |
| `src/g9ota_flow.c`, `g9ota_flow.h` | nRF5340 | update flow: session, signature, trial boot, revert (no OS, host tested) |
| `zephyr/gap9_ota.c`, `gap9_ota_zephyr.h` | nRF5340 | glue of the flow: settings, signature check (`sensei_crypto`), GAP9 link, timer (`CONFIG_SENSEI_GAP9_OTA`) |
| `tests/` | host | the state machine against a simulated GAP9 master (`test_gap9_core.c`), the slot checks and the trial rule (`test_gap9_slot.c`, `test_gap9_trial.c`), the writer and the flow of the update (`test_g9ota.c`, `test_g9ota_flow.c`) |

The firmware update of GAP9 over the air (A/B slots, signed images, trial boot) is described in [OTA.md](OTA.md).

## nRF5340 (Zephyr)

- `CONFIG_SENSEI_GAP9=y` (selects `SENSEI_LINK`, `SENSEI_BUSARB`; needs `SENSEI_PWR` for `gap9_pwr()`). Options: queue depth, staging depth, result ring,
  timeouts, idle power-off, bus wait. `SENSEI_GAP9_SHELL` adds `sensei gap9 [on|off|ping|status]` (needs an app shell root
  command `sensei`).
- Add `zephyr/gap9_link.overlay` to `EXTRA_DTC_OVERLAY_FILE` of the app. It defines the aliases `gap9-trigger`, `gap9-spi-sel`,
  `gap9-i2c-sel`, `gap9-spis` (SPIS3 on P0.08 to P0.11, trigger P1.05), `gap9-slot-sel` (P1.04).
- The app defines the storage with `GAP9_LINK_STORAGE_DEFINE(name, job_t, result_t)`, fills `struct gap9_link_cfg` (codec, hooks,
  storage) and calls `gap9_link_init()`, then `gap9_link_submit()`. Hooks: camera bus lock, camera clock, error counter, energy
  marker, the work queue for the result callbacks. The codec also names the GAP9 firmware major version it needs
  (`gap9_major`, checked against HELLO).
- Firmware slots (A and B in the GAP9 MRAM, started by the SSBL in `GAP9/ssbl`): the alias `gap9-slot-sel` (P1.04 to GAP9
  pad 56) goes high only after the GAP9 rail is on (the SSBL reads it milliseconds later) and is low when GAP9 is off. `gap9_link_set_slot()` picks the slot of the next
  power-ups, `gap9_link_hold()` keeps GAP9 on for an update session, `gap9_link_reboot()` power cycles it into a slot and
  reports its HELLO. Details in [PROTOCOL.md](PROTOCOL.md).
- `CONFIG_SENSEI_GAP9_OTA=y` adds the update flow (selects `SETTINGS`, `SENSEI_CRYPTO_ECDSA`): the app fills a
  `struct sensei_gap9_ota_cfg` (public key, job callback, trial check, work queue), calls `sensei_gap9_ota_init()` after
  `gap9_link_init()` and forwards the host requests to `g9ota_flow_begin/_chunk/_command()` with `sensei_gap9_ota_flow()`. See OTA.md.
- `gap9_link_submit()` returns `-ENOMEM` when all staging slots are taken; every other error comes through the callback
  (`final` is set on the last call of a job). `gap9_link_reset()` is for the user button.
- SPI_A is shared with the LPWAN shield (`sensei_busarb`): GAP9 starts only when the shield holds neither the bus nor an LTE transmission.

## GAP9

```cmake
include(${SENSEI_SDK_ROOT}/lib/sensei_gap9/sensei_gap9.cmake)
include(${SENSEI_SDK_ROOT}/lib/sensei_link/sensei_link.cmake)
target_sources(app PRIVATE ${SENSEI_GAP9_SIDE_SOURCES} ${SENSEI_LINK_SOURCES})
target_include_directories(app PRIVATE ${SENSEI_GAP9_SIDE_INCLUDE_DIRS} ${SENSEI_LINK_INCLUDE_DIRS})
```

A job server is a loop: `g9l_init()`, `g9l_hello()`, then `g9l_fetch()`, run the job, `g9l_send_result()`, and `g9l_wait_work()` when
the nRF5340 has no job. `g9l_busfree()` tells the nRF5340 that the camera is done with the shared I2C bus.

For the update, add `SENSEI_GAP9_OTA_SIDE_SOURCES`, `SENSEI_GAP9_OTA_INCLUDE_DIRS` and
`SENSEI_GAP9_OTA_LINK_OPTIONS` (the wrapper of `pi_fs_conf_init`) in the same way. `g9ota_mram_init(kind, NULL)` opens the
MRAM at start-up and `g9ota_dispatch()` runs one update request (see OTA.md).

Host tests: `cmake -S tests -B <builddir>`, or `make test-host` in the SDK root. The GAP9 side and the nRF5340 side together
were tested on GVSoC only in local mode (no SPI); the SPI path is untested on hardware.
