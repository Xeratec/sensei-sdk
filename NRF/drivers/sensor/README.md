<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# Sensor drivers

Drivers for the ICs of the SENSEI boards that Zephyr 4.4 does not cover, or covers in a way that does not fit.
Each driver has a devicetree binding (`NRF/dts/bindings/sensor`), a header with its extras
(`<sensei/drivers/...>`) and a Kconfig symbol. The nodes of the sensor shield are in
`NRF/boards/shields/UT_SensorShield`. Everything is untested on hardware.

| Node | Driver | Kconfig | Zephyr alternative and why it is not used |
|---|---|---|---|
| `sensei,scd41` | SCD41 CO2, single shot | `SCD41_SENSEI` | Zephyr `sensirion,scd41` (`SCD4X`): `sample_fetch` blocks for the 5 s of a single shot, no rail switch, no ASC periods, no start/fetch split. Different compatible, so both can exist. |
| `sensirion,sgp41` | SGP41 raw ticks, conditioning, heater off | `SGP41_SENSEI` | Zephyr has only `sensirion,sgp40`; the SGP41 has other commands, NOx and a conditioning step. |
| `bosch,bme688` | BME688, Bosch API, forced mode with one heater step | `BME688_SENSEI` | Zephyr `bosch,bme680`: heater and oversampling are compile-time constants, `sample_fetch` blocks for the whole measurement, no start/fetch split. BSEC stays out (proprietary). |
| `ams,as7331` | AS7331 UV, command mode, uW/cm2 | `AS7331_SENSEI` | None in Zephyr. |
| `sensei,bh1730` | BH1730FVC, one-time measurement | `BH1730_SENSEI` | Zephyr `rohm,bh1730`: lux only (no raw channels), blocks during the measurement, no power-down. |
| `st,ilps28qsw` | ILPS28QSW, one-shot | `ILPS28QSW_SENSEI` | None in Zephyr (its LPS22/LPS28 drivers use another register map). |
| `st,lis2duxs12` | Zephyr driver plus `lis2duxs12_extra` | `LIS2DUXS12_EXTRA` | Zephyr driver used; the extra adds wake-up interrupt and deep power-down. |

The ISM330DHCX and the LIS2DUXS12 use the Zephyr drivers, which need the `hal_st` module.

## How the shield sensors are built

- **Standard sensor API**: `sensor_sample_fetch()` runs a whole measurement and waits for it; `sensor_channel_get()`
  returns the values. Channels the API has no name for are `SENSOR_CHAN_PRIV_START + n` (see each header).
- **Start and fetch**: `sensei_<name>_start()` triggers a measurement and returns the ms until the result is
  ready, `sensei_<name>_fetch()` reads it (`-EBUSY` if it is too early). A scheduler can sleep in between. The
  sensor API fetch is built from the two.
- **Power**: the SCD41, SGP41, AS7331 and BH1730 drivers use runtime device PM. `pm_device_runtime_get()` switches
  on the rail and the I2C isolation switch (`vdd-supply` and `bus-switch-supply`, regulators in the shield
  devicetree) or clears the software power-down; `pm_device_runtime_put()` reverses it. `sensor_sample_fetch()` takes
  and drops a reference itself; hold one yourself to keep a sensor powered between measurements. These drivers
  depend on `CONFIG_PM_DEVICE_RUNTIME=y` (set it with `CONFIG_PM_DEVICE=y` in the application) and are off by
  default. The BME688 and the ILPS28QSW sleep by themselves and need no PM.
- **Probe**: nothing touches the hardware at boot (the board rails may not be up yet). `sensei_<name>_probe()` checks
  the ID at the first use and keeps the result once the chip answered. A chip that did not answer is probed again
  at the next use.
- **Vendor libraries**: compiled by the drivers from `NRF/system/third_party` (see its README). The two Sensirion
  libraries keep the I2C address and the bus in globals, so there is one SCD41 and one SGP41, and every use of
  them goes through one mutex (`<sensei/drivers/sensirion_hal.h>`).
- **Not thread safe per device**: serialise the calls to one device. The node firmware does that with its bus lock.

## SCD41

Single-shot mode: `start` wakes the sensor if it sleeps, triggers a shot and returns 5000 ms (the library sleeps 5 s
inside the command; the HAL defers that sleep). `sensei_scd41_sleep()` sends power_down between shots when the sensor
stays powered. The ASC settings are written after every cold start (`sensei_scd41_set_asc()`): with a switched rail
ASC cannot converge, so turn it off; with the rail on, scale its periods to the shot period
(`sensei_scd41_asc_hours()` in `sensei_sensor_math.h`). `sensei_scd41_forced_recalibration()` is for commissioning.

## SGP41

The driver has no state machine. After a cold start the sensor needs conditioning (10 commands, 1 s apart) before
the NOx value is valid: call `sensei_sgp41_condition()`, then `sensei_sgp41_measure()`. `sensor_sample_fetch()`
does the whole cycle blocking (`SGP41_SENSEI_CONDITIONING_S`). The VOC and NOx index come from `lib/sensei_gas_index`,
which keeps state across samples and has to be fed at a fixed interval. Compensation ticks:
`sensei_sgp41_rh_ticks()` and `sensei_sgp41_t_ticks()` in `sensei_sensor_math.h`.

## BME688

Forced mode, one heater step (devicetree `heater-temp-c` 320, `heater-duration-ms` 150, oversampling T 2x, p 4x,
H 2x; `sensei_bme688_set_heater()` changes the heater at run time). Pressure is in kPa like the Zephyr channel,
gas resistance is 0 if the gas reading or the heater was not stable.

## AS7331

Command mode, results in uW/cm2 from the datasheet responsivity (`lib/sensei_sensor_math`). Default gain 2x (CREG1
GAIN 10) and 2048 ms integration (CREG1 TIME 11); `sensei_as7331_configure()` changes them. The shield has no rail
switch: suspend sets the PD bit and opens the I2C switch. `ready-gpios` (expander P4) is read by
`sensei_as7331_ready()`; the driver itself reads the status register.

## BH1730

One-time measurement of both channels with gain x64 and 51 ms integration (devicetree `gain`, `itime`). Channels:
`SENSOR_CHAN_LIGHT` (lux) and the raw counts `SENSEI_BH1730_CHAN_VIS_RAW` and `_IR_RAW`. Suspend clears the POWER bit.

## ILPS28QSW

One-shot mode with the sensor powering down by itself. `sensei_ilps28qsw_start()` and `_fetch()` split the
conversion (20 ms).

## LIS2DUXS12 extras

Wake-up (motion) interrupt on INT1 and deep power-down, which the Zephyr driver does not offer
(`<sensei/drivers/lis2duxs12_extra.h>`). Use it next to the Zephyr node.
