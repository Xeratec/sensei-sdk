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
  library `ram_pwrdn`, `power_down_unused_ram()`). That call knows no other region, so the build checks that every devicetree
  `zephyr,memory-region` node, every `zephyr,retained-ram` node and the chosen `zephyr,ipc_shm` lie at or above the end of
  `zephyr,sram` (the SENSEI boards do: `nrf5340_senseiv1_retained.dtsi`); a board that does not cannot use it. Call it last in
  `main()`. Needs a libc arena other than -1 (`CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=0` when nothing calls `malloc()`) and not
  newlib (its `malloc()` grows into all free RAM). Untested on hardware.
- `CONFIG_SENSEI_PWR_RAM_ON_BOOT`: a `PRE_KERNEL_1` hook that powers on all 8 RAM blocks. On by default in MCUboot (nRF5340
  application core), where it needs no SENSEI option. Why: a watchdog, lockup or pin reset does not power RAM on again (only
  the software reboot of the nRF Connect SDK does), so a trial image that powered down more RAM than the old one and then hung
  would leave the old image, which MCUboot starts after the revert, on RAM that is off. MCUboot runs after every reset and
  gives every image all its RAM. About 100 B. Untested on hardware (check: T4.11, a watchdog reset of a smaller trial image).

## Start-up order

1. GPIO and the I2C buses: the TWIM driver starts at `POST_KERNEL` 50 (`CONFIG_I2C_INIT_PRIORITY`). The PSRAM
   driver (opt-in) also starts at 50 and keeps its supply off.
2. `POST_KERNEL` 55, `pwr_boot_hook`: first drives the GAP9 supply select (P1.15), the GAP9 I2C, SPI and shared-GPIO
   mux selects (P0.06, P0.20, P0.28) and the GAP9 slot line (P1.04) inactive, so GAP9 cannot boot or use the buses
   while the rails come up and the slot pad is not driven into an unpowered chip. Then it
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

The key-down time is the time of the PMIC interrupt that came with the flag. A flag read without a new interrupt
(`pwr_refresh()`, the 20 s pass) uses the time of the read, so the press can only look shorter. A key-down older than
`THREAD_PWR_BUTTON_MAX_MS` (20 s; the PMIC resets itself after 8 or 16 s of nEN) is a lost key-up: the next key-up is not
a long press.

The nIRQ and Soft_nRST lines (`gpio_pmic_irq`, `gpio_soft_nrst`) are set up by `pwr_init()` as inputs with the
devicetree flags: active low, internal pull-up (idle high; in case the board has no external one). Untested on hardware
(T4.8 of the node repo).

## Battery charge counter (`CONFIG_SENSEI_PWR_CHARGE`, `pwr/pwr_charge.h`)

The MAX77654 has no fuel gauge. This option counts the charge in software with `lib/sensei_charge` (read its README for the
method: floor, states, events, self-discharge, the report and the flags) and adds what only the PMIC side knows. Off by default.

- **API.** `pwr_charge_init(cfg)` (after `pwr_init()` and the settings load; the application gives the table and the settings),
  `pwr_charge_state(id, on)` and `pwr_charge_event(id, n)` (cheap, from any thread or interrupt), `pwr_charge_set_temp()`,
  `pwr_charge_report()`, `pwr_charge_ledger()`, `pwr_charge_new()` (a new battery), `pwr_charge_save()`, `pwr_charge_check(cls)`
  (AMUX sample while a job runs), `pwr_charge_configure()`, `pwr_charge_set_swap_cb()`.
- **Power thread** (every 20 s and on PMIC interrupts): battery voltage (the table is at 3.6 V and scales with 3.6 V / V_bat),
  external power (USB: the battery does not supply the node, only the ledger counts), the idle current (floor watchdog: one
  8.2 mA-scale sample when no state is on; three in a row more than `CONFIG_SENSEI_PWR_CHARGE_FLOOR_MARGIN_UA` above the floor
  set `FLOOR_HIGH`), the retained record. With the Li-ion choice also the charge current (`BATT_I_CHG`, integrated), "charger done"
  (`PWR_CHG_DONE`: used = 0, `LIION_SYNCED`), the capacity learned from a full discharge to 3.3 V, and the voltage hint after 30 min
  without load. Three short presses of the nEN button within 3 s, in the first 5 min after a power-on reset, declare a new battery
  (a callback tells the application; every press also resets GAP9, as before).
- **Measurement.** `max77654_measure_dischg_ua(scale)` of the driver: the NULL and the DISCHG conversion back to back (the BSP's
  `pmic_bsp_adc_read_fast()`: no 1 ms wait, 10 us after the channel switch), at the full resolution of the SAADC (6 uA at the 8.2 mA scale,
  211 uA at 40.5 mA), AMUX off afterwards, under the power mutex. The old `max77654_measure()` gives whole percent of the scale.
  The PMIC is on I2C A, which GAP9 does not take, so no bus claim is needed.
- **Persistence.** A 32 B record in the retained RAM (devicetree chosen `sensei,charge`, node `charge_record` at 0x50 of the retained
  region in `nrf5340_senseiv1_retained.dtsi`, the gap between `diag_state` and `diag_dump`; without it `noinit` RAM) written at
  every pass, and in the settings (`sensei_charge/rec` and `sensei_charge/ledger`, not under `sensei`, which the node's handler owns)
  every `CONFIG_SENSEI_PWR_CHARGE_SAVE_H` hours (6), on a new battery, on "charger done", and on `pwr_charge_save()` (the node calls
  it with the daily status and before planned reboots; `pwr_off_and_reboot()` calls it too). Start order: retained record, else
  settings record (flag `RESTORED`, up to 6 h lost), else zero; a power-on reset without the retained record is `UNCONFIRMED`
  unless the rested voltage is 150 mV above the last one (`AUTO_SWAP`, counter reset). MCUboot has no use for it: its board file
  (`..._cpuapp_bootloader.dts`) disables the node like the other retention areas.
- **Cost.** About 10 kB of flash and 0.9 kB of RAM in the node's Debug build, with the library, the settings records and the
  retention driver already there for the boot mode.

Untested on hardware: the AMUX samples (offset -0.5 ... +0.8 mA at 0 mA, accuracy +-15 % above 15 mA, datasheet), the retained
RAM over a power cut, the floor margin (the sample is taken while the CPU is awake in the power thread).

## Tests

Zephyr tests on native_sim (`NRF/tests/pwr`, `pwr_check`, `pwr_ram`; see `NRF/tests/README.md`). `pwr` runs this module
with the real MAX77654 driver and BSP on an I2C emulator of the PMIC, the GPIO emulator and the ADC emulator: boot hook
(order, wait after the reset, rails, settle time, retry in `pwr_init()`), charger and thresholds per battery choice,
events and status, battery levels, failed PMIC reads, the periodic work, power-off and the button. `pwr_check` runs
`sensei_pwr_check_idle()` on test devices, `pwr_ram` the size that `sensei_pwr_ram_powerdown()` reports, `pwr_charge` the charge counter
with the real driver and BSP on the PMIC emulator (conversion order and uA result, accounting, voltage scaling, floor watchdog,
external power, AMUX checks, the 6 h save, the retained record, a new battery, the button, Li-ion charge and "done"; and per build the
restore precedence: retained, settings, none, swap).
Not tested: the real PMIC (the emulator follows the driver's register map), the nEN edge and PMIC interrupt timing,
`pwr_ram_boot.c` and the RAM switches. The patched MAX77654 driver is checked here too (thermistor thresholds, ERC flags,
see `NRF/system/third_party/max77654/README.md`).
