<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# Power module (`CONFIG_SENSEI_PWR`)

MAX77654 PMIC of the SENSEI base board: rails, charger, battery reading, nIRQ events and the nEN button.
Untested on hardware.

## API (`pwr/pwr.h`)

- `pwr_init()`, `pwr_start()`: call both from `main()`. `pwr_init()` sets the pins and the board hook
  (`pwr_bsp_init()`), `pwr_start()` powers the LED and runs `pwr_bsp_start()` and starts the power thread.
- `pwr_bat_mV()`, `pwr_bat_perc()`, `pwr_get_status()`, `pwr_set_event_cb()`, `pwr_refresh()`: battery, charger and
  PMIC events.
- `pwr_off_and_reboot()`: rails off, then a cold reboot (used for a long button press).
- `gap9_pwr()`: GAP9 supply switch (P1.15).
- `pwr_log_voltages()`: logs the PMIC voltages (bring-up).

`pwr/pwr_board.h` is for the application's board hooks (`bsp/pwr_bsp.h`): `pwr_pmic()` gives the PMIC handle.
`pwr_priv.h` is internal.

## Checks for the sleep floor

Both are independent of `CONFIG_SENSEI_PWR` (no PMIC needed).

- `CONFIG_SENSEI_PWR_CHECK`, `pwr/pwr_check.h`: `sensei_pwr_check_idle(cb, user)` walks all devices with device PM and calls
  `cb` for each one that is not suspended (state, runtime PM usage count, whether runtime PM is on). Use it from a shell
  command or a test when the system should be idle. Devices without runtime PM stay active by design, so the callback
  says whether runtime PM is on.
- `CONFIG_SENSEI_PWR_RAM_POWERDOWN`, `pwr/pwr_ram.h`: `sensei_pwr_ram_powerdown()` powers down the 4 kB RAM sections of the
  nRF5340 application core between the end of the image RAM and the end of `zephyr,sram` (uses the nRF Connect SDK
  library `ram_pwrdn`). Left alone: every devicetree `zephyr,memory-region` node (retained RAM), the chosen
  `zephyr,ipc_shm` and the partial section at the end. Call it last in `main()`. Needs a libc arena other than -1
  (`CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=0` when nothing calls `malloc()`). Untested on hardware.

## Start-up order

1. GPIO and the I2C buses: the TWIM driver starts at `POST_KERNEL` 50 (`CONFIG_I2C_INIT_PRIORITY`). The PSRAM
   driver (opt-in) also starts at 50 and keeps its supply off.
2. `POST_KERNEL` 55, `pwr_boot_hook`: first drives the GAP9 supply select (P1.15) and the GAP9 I2C, SPI and shared-GPIO
   mux selects (P0.06, P0.20, P0.28) inactive, so GAP9 cannot boot or use the buses while the rails come up. Then it
   resets the PMIC (rails go off for 300 ms), applies the config, and with
   `SENSEI_PWR_BOOT_RAILS` (default with the sensor shield) switches the rails on (SBB0 3.3 V, SBB1 2.8 V, SBB2 1.2 V,
   LDO0 3.3 V), waits 50 ms, and switches the LED supply (SDB, P0.22) on.
3. `POST_KERNEL` 60: PCA6416A expander (the sensor shield sets `GPIO_PCA_SERIES_INIT_PRIORITY` to 60), 61: its GPIO
   hog (camera clock gated), then the regulators (75), ISM330DHCX, LED and the other drivers (90). Their chips are powered. The shield sensors with their
   own rail switch (SCD41, SGP41, AS7331, BH1730) do nothing at boot; the PM runtime of their driver switches them.
4. `main()`: `pwr_init()` (no PMIC reset), `pwr_start()`.

The rails are cycled once, before the drivers start, and never again. If the hook fails (PMIC does not answer),
`pwr_init()` tries again and the shield drivers have failed to start: check the log.

## Battery

One Kconfig choice, `SENSEI_PWR_BATTERY_*`, sets the charger, the charge table and the level thresholds
(`PWR_BAT_*_MV`) together:

| Choice | Charger | Table | critical / low / ok |
|---|---|---|---|
| `LISOCL2` (default) | off | Li-SOCl2 (3.65 V = 100 %, 3.0 V = 0 %) | 2.9 / 3.1 / 3.3 V |
| `LIION` | on, 4.2 V, 15 mA | Li-ion (4.2 V = 100 %, 3.3 V = 0 %) | 3.1 / 3.3 / 3.5 V |

The SENSEI board carries a Saft LS33600 (Li-SOCl2, 3.6 V, 17 Ah; `energy-model/config/hardware.yaml` of the node repo). That cell is
not rechargeable: the old default (charge to 3.6 V, Li-ion table with 4.2 V = 100 %) would have charged a depleted
primary cell and shown about 20 % for a fresh one. A Li-SOCl2 curve is flat near 3.6 V, so the percentage is a hint only.
The table and the button logic are in `lib/sensei_pwr_math` (host tests).

## Button

Key-down is the nEN falling edge (interrupt flag read from the PMIC), key-up is the Soft_nRST edge. A key-up without a
key-down is ignored, and a new key-down replaces an older one (its key-up was missed). The key-up is handled only
after a good flag read. A press longer than 2 s (`THREAD_PWR_SOFT_RESET_LATENCY_MS`) switches the rails off and
reboots.
