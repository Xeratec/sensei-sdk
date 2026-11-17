# sensei_gap9

Link between the nRF5340 and the GAP9 of the SENSEI base board. The nRF5340 sends jobs, GAP9 runs them and sends results.
GAP9 is powered only while jobs are pending. The wires, frames and timing are in [PROTOCOL.md](PROTOCOL.md).

The link moves opaque job and result messages. The app describes them with a `struct g9_codec` (encode, decode, sequence
numbers, status to errno, which job needs the camera bus), so any message schema works. The SENSEI IEQ node uses
`Gap9Job` and `Gap9Result` (`firmware/nrf5340/src/gap9/gap9_codec.c`).

| Folder | Side | What |
|---|---|---|
| `include/sensei_gap9/gap9_link.h` | both | frame types, flags, buffer sizes, timing constants |
| `src/gap9_core.c`, `gap9_core.h` | nRF5340 | protocol state machine: job queue, boot, frames, retries, timeouts, bus lock, idle power-off (no OS, host tested) |
| `zephyr/` | nRF5340 | `gap9_link.c` (thread, SPIS3, trigger line, power, shared SPI_A), `gap9_shell.c`, `gap9_link.overlay` |
| `gap9/gap9_side.c`, `gap9_side.h` | GAP9 | SPI master, trigger line, hello, fetch, result, bus free (GAP SDK) |
| `tests/` | host | the state machine against a simulated GAP9 master |

## nRF5340 (Zephyr)

- `CONFIG_SENSEI_GAP9=y` (selects `SENSEI_LINK`, `SENSEI_BUSARB`; needs `SENSEI_PWR` for `gap9_pwr()`). Options: queue depth, result ring,
  timeouts, idle power-off, bus wait. `SENSEI_GAP9_SHELL` adds `sensei gap9 [on|off|ping|status]` (needs an app shell root
  command `sensei`).
- Add `zephyr/gap9_link.overlay` to `EXTRA_DTC_OVERLAY_FILE` of the app. It defines the aliases `gap9-trigger`, `gap9-spi-sel`,
  `gap9-i2c-sel`, `gap9-spis` (SPIS3 on P0.08 to P0.11, trigger P1.05).
- The app defines the storage with `GAP9_LINK_STORAGE_DEFINE(name, job_t, result_t)`, fills `struct gap9_link_cfg` (codec, hooks,
  storage) and calls `gap9_link_init()`, then `gap9_link_submit()`. Hooks: camera bus lock, camera clock, error counter, energy
  marker, the work queue for the result callbacks.
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

Host tests: `cmake -S tests -B <builddir>`, or `make test-host` in the SDK root. The GAP9 side and the nRF5340 side together
were tested on GVSoC only in local mode (no SPI); the SPI path is untested on hardware.
