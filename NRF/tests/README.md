<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# Zephyr tests

Ztest suites that run on `native_sim/native/64` with Twister, so the Zephyr side of the SDK is checked without
hardware. Two suites (`diag_fault`, `diag_isr`) run on QEMU (`qemu_cortex_m3`, `mps2/an521/cpu0`) because they need a real fault and a reset. One
folder per suite, all under this tree (the pure C logic of the libraries has host tests in `lib/<name>/tests`, see
`make test-host`).

| Folder | Test ID | What it checks |
|---|---|---|
| `crypto` | `sensei.crypto` | SENSEI crypto library on PSA (Mbed TLS): SHA-256, HMAC, HKDF, AES-GCM, ECDH and ECDSA P-256 with NIST and RFC vectors |
| `secure` | `sensei.secure` | `sensei_secure` with the Zephyr backend: the host test of the OpenSSL backend and its Python vectors |
| `secure_settings` | `sensei.secure.settings` | `sensei_e2e` on the settings subsystem (NVS on the flash simulator): key names and sizes, state after a restart (keys, counters, table of senders, floor), erase |
| `busarb` | `sensei.busarb` | `sensei_busarb` Zephyr wrapper with the real kernel: claim, expiry, listener, `bus_claim_wait()` |
| `link` | `sensei.link` | `sensei_link` as Zephyr library on the NCS nanopb: messages, framing, SPI frame, fragments, Kconfig option |
| `scd41`, `sgp41`, `bh1730` | `sensei.drivers.<name>` | sensor drivers against I2C emulators: sensor API and split API, conversion, power, sleep, probe and re-probe |
| `ubx` | `sensei.drivers.gnss.ubx` | u-blox UBX parser |
| `diag` | `sensei.diag` | `NRF/system/diag` on native_sim with a RAM-array retained memory driver: boot counter, reset cause, fault record taken once, damaged and foreign RAM |
| `diag_fault` | `sensei.diag.fault` | Fault capture on QEMU (`qemu_cortex_m3`): `k_oops()` and an undefined instruction reset the board, the record and the coredump are checked after each reboot |
| `ota_reset` | `sensei.ota.reset` | The SMP reset hook of `NRF/system/ota` in the mcumgr callback chain (native_sim): a reset into the MCUboot recovery without USB power (as over Bluetooth) is refused, a normal reset and a reset with USB power go through |
| `wdt` | `sensei.wdt` | `NRF/system/wdt` with a test watchdog (FFF): device not ready, failed install or setup, timeout and options, feed period and channel, no feed while the app is not healthy, second start refused, timeout checks, failed feed counted |
| `ota` | `sensei.ota.trial*` | Trial supervision of `NRF/system/ota` with MCUboot state and reset as fakes, one build per case: the check starts after the delay, an SMP request is needed to confirm (also one before `sensei_ota_init()`), a failed confirm write is retried, reset at the end of the trial without a request or a good check, no reset after a host confirm, and the option cases (`CONFIRM_NEEDS_LINK=n`, `CONFIRM=n`); image id read from the flash simulator |
| `pwr` | `sensei.pwr`, `.liion`, `.boot_fail`, `.no_boot_rails` | `NRF/system/pwr` with the real MAX77654 driver on an I2C emulator of the PMIC: boot hook (GAP9 lines first, reset, wait, rails, settle, LED supply), retry in `pwr_init()`, charger and thresholds per battery choice, status and events from the flags, battery levels with hysteresis, bad ADC readings, failed PMIC reads, periodic work, GAP9 reset, power-off, button (key-down time, lost key-up), input pins of nIRQ and Soft_nRST, thermistor thresholds and ERC flags of the MAX77654 driver |
| `pwr_check` | `sensei.pwr.check` | `sensei_pwr_check_idle()` on test devices in every power state (active, runtime PM idle and held, suspended, off, not ready, no PM) |
| `pwr_ram` | `sensei.pwr.ram`, `.ram.full` | `sensei_pwr_ram_powerdown()`: one library call, the 4 kB sections it covers (fake of the NCS library) |
| `diag_isr` | `sensei.diag.fault.isr`, `sensei.diag.fault.isr_stack` | Fault capture on QEMU (`mps2/an521/cpu0`, Cortex-M33 with stack guards): a fault and `k_panic()` in an interrupt, a thread stack overflow; an overflow of the interrupt stack (the core locks up, QEMU stops) |

Some suites have several Twister scenarios in their `testcase.yaml`: the same sources, built with other options
(`extra_configs`) or with a test option of the folder's `Kconfig`. This is for things that happen once per boot (the PMIC
boot hook, an image that is confirmed once, a reset that ends the run). The scenario is in the test ID (`.liion`,
`.no_link`, ...).

Why one tree: Twister needs the SDK as Zephyr module (`-x=EXTRA_ZEPHYR_MODULES=<sdk>/NRF`), so all suites
need the NRF module anyway, and one `-T NRF/tests` finds them.

## Run

```sh
scripts/twister.sh                           # all suites on native_sim and QEMU, in Docker (also: make twister)
scripts/twister.sh -- -s sensei.busarb       # one suite (Twister arguments after --)
scripts/twister.sh --host                    # no Docker: Linux with gcc, a west workspace and Twister's Python packages
```

The script is described in its header. Docker needs the image `sensei-twister:ncs-3.4.1` (built on first use from
`scripts/docker/twister.Dockerfile`; its base is `sensei-nrf:ncs-3.4.1` of the node repository; it adds the host compiler for
native_sim and QEMU for `qemu_cortex_m3` and `mps2/an521/cpu0`, and builds the ARM test with the Zephyr SDK of the base image) and an NCS v3.4.1
west workspace (`SENSEI_NCS_WORKSPACE`, default `~/Documents/work/Tools/sensei-ncs-v3.4.1`), mounted read-only.
Output goes to `build/twister`. A fresh workspace for the tests alone: `scripts/west-twister.yml`. CI does that:
`.github/workflows/twister.yml` (native_sim only: it has no Zephyr SDK or QEMU; `TWISTER_PLATFORM` adds platforms).

## Coverage

```sh
scripts/twister.sh --coverage                # also: make twister-coverage
scripts/twister.sh --coverage -- -s sensei.pwr
```

Builds the native_sim tests with gcov (`twister --coverage`, gcovr) and writes `<out>/twister-out/coverage/`:
`index.html` and `coverage-summary.txt` (one line per source file, missing lines). The report covers the SDK sources
only; `NRF/system/third_party` is left out, except the MAX77654 driver. A coverage run builds native_sim only (not QEMU). There is
no threshold. CI uploads the folder as artifact `twister-coverage` and prints the summary in the job summary. gcovr is in
the Docker image; for `--host` install it with pip.

## Shared files

- `common/sensei_sim.overlay`: board devices that `CONFIG_SENSEI` code expects (`i2ca`, an ADC channel) as native_sim
  emulators. Add it with `list(APPEND EXTRA_DTC_OVERLAY_FILE ...)` before `find_package(Zephyr)`.
- `common/sensirion_emul.h`: CRC-8 and word helpers of the SCD41 and SGP41 emulators.
- `common/sensei_emul.cmake`: `sensei_emul_sources(<target> scd41 sgp41 bh1730)` adds the emulators of the driver tests
  (sources and include paths, they stay in their test folders) to an app on native_sim. The node repository uses it for its
  simulated node (`firmware/sim`): its overlay has the sensor nodes, its code sets the values in `scd41_emul_get()` and
  the like. The app needs `CONFIG_EMUL`, `CONFIG_I2C_EMUL` and, for the rails, `CONFIG_GPIO_EMUL` and `CONFIG_REGULATOR`.

## Sensor emulators

Each driver test has its emulator in `src/emul_<name>.c` (Zephyr I2C emulator, same devicetree node as the driver).
The head of the file says which datasheet section it follows and what it leaves out. The emulators refuse what the
sensors refuse: a command sent while the previous one runs (a missing wait of the library, counted in `early`), a
wrong CRC, a read without a result, any access with the supply rails off. The test hooks (present, result values,
failure injection, command log) are in `emul_<name>.h`.

The drivers keep some state for good (chip found), so the tests of a driver run in name order (`test_01_...`).
Keep the numbers when you add a test.

## Not covered

Everything that needs the real chip or board: timing on the bus, the real PSA drivers (Oberon, CC3xx) of the
nRF5340, GAP9 and LPWAN links over SPI, BLE, the other sensor drivers (AS7331, BME688, ILPS28QSW, LIS2DUXS12). Of
the system modules: the MCUboot swap and the trailer flags (the tests fake `boot_is_img_confirmed()` and
`boot_write_img_confirmed()`), the SMP transports and the SMP stream, VBUS sensing; the real PMIC timing and the nEN
and nIRQ pins (the emulator follows the driver's register map, not a datasheet run); the RAM power switches
(`pwr_ram_boot.c`, the NCS library behind `sensei_pwr_ram_powerdown()`); the nRF watchdog and its reset.
