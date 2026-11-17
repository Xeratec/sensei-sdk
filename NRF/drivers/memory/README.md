<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# APS12804O PSRAM driver

16 MiB QSPI PSRAM on the nRF5340 QSPI pins (P0.13 to P0.18), supply behind the load switch P1.14, chip select mux P0.19 (low = PSRAM). Untested on hardware.

Node `psram` in the base board devicetree, disabled. Enable it with `&psram { status = "okay"; };` and keep `&qspi` disabled, the driver uses nrfx QSPI directly (`<sensei/drivers/psram_aps12804o.h>`).

- SPI mode with the quad commands (read 0xEB, write 0x38); reset, ID and half-sleep as custom instructions.
- Accesses are cut into 32 byte pieces because CE# may stay low for 8 us at most (3 us at high temperature).
- States: on (about 64 uA), half-sleep (about 19.5 uA, data kept), off (load switch, data lost).

Check first on the board: the nrfx QSPI start reads a status byte, which the PSRAM does not define (init may time out); nRF5340 anomaly 159 (`-EPERM`, see the nrfx_qspi notes); chip select mux polarity; 32 MHz clock.
