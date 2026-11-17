<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# MAX-M10S GNSS driver (I2C)

Zephyr GNSS driver for the u-blox MAX-M10S on I2C (DDC, address 0x42), UBX protocol only. Zephyr's own u-blox drivers are UART only. Untested on hardware.

- Node `u-blox,max-m10s` (see the binding), in the sensor shield overlay as `gnss`, with RESET_N, TIMEPULSE and EXTINT on the PCA6416A expander (P13, P14, P15). SAFEBOOT_N has no net on the shield.
- Fixes and satellites come through the GNSS API (`GNSS_DATA_CALLBACK_DEFINE`, `GNSS_SATELLITES_CALLBACK_DEFINE` with `CONFIG_GNSS_SATELLITES`). Fix rate, navigation mode and systems are set with the standard calls.
- Extras in `<sensei/drivers/gnss_m10s.h>`: power save mode (PSMOO, PSMCT), single poll, standby, wake, reset.
- `gnss_m10s_set_bus_hooks()`: a try-lock around the polling work for a bus that others own (the GAP9 camera on I2C_B). The driver skips a round when the lock returns `-EBUSY`.
- The module is configured with UBX-CFG-VALSET (RAM and BBR layers): UBX only on I2C, NAV-PVT periodic or polled (`polled` property).
- `standby-at-boot` sends UBX-RXM-PMREQ (backup, wake on EXTINT) after start-up. `gnss_m10s_wake()` wakes it with an EXTINT edge. Software standby keeps the 3V3 rail on (about 37 uA).
- `ubx.c` has the framing, checksum and the NAV-PVT and NAV-SAT parsers without Zephyr dependencies. Host test: `NRF/tests/ubx` (ztest, see below).

Test (native_sim does not run on macOS, QEMU does):

```sh
west build -b qemu_cortex_m3 NRF/tests/ubx -d build-ubx --no-sysbuild && west build -d build-ubx -t run
```

Not done: the TIMEPULSE interrupt (`gnss_get_latest_timepulse`): the expander has no interrupt line to the nRF5340.
