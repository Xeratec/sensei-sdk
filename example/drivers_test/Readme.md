This example probes every IC the SDK has a driver for, prints one reading and puts the IC into its lowest-power state. Tests only run if the devicetree node exists. Untested on hardware.

| IC | Driver | Low-power step |
|---|---|---|
| MAX77654 | SDK power module (`pwr_get_status`, events) | stays on |
| IS31FL3194 | Zephyr | LED off, LED_EN low |
| LIS2DUXS12 | Zephyr and `lis2duxs12_extra` | deep power-down |
| ISM330DHCX | Zephyr | odr 0 |
| ILPS28QSW | SDK | one-shot, powers itself down |
| BME688 | SDK (Bosch API) | forced mode, sleeps by itself |
| SCD41 | SDK (Sensirion library) | single shot, then rail off (5 s) |
| SGP41 | SDK (Sensirion library) | conditioning and a measurement, then rail off (about 10 s) |
| AS7331 | SDK (register library) | measurement, then power-down and bus switch open (about 2 s) |
| BH1730FVC | SDK (register library) | measurement, then power-down |
| PCA6416A, TCAL6408 | Zephyr | none |
| MAX-M10S | SDK | software standby |
| APS12804O | SDK, node disabled | half-sleep, then off |
| nRF21540 | node only, disabled | not tested here |

## NRF

```sh
cd example/drivers_test/src_NRF
west build -b nrf5340_senseiv1/nrf5340/cpuapp -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF
# with the LPWAN shield too:
#   -DSHIELD="UT_SensorShield_v1;LPWAN_Shield_v1"
# with the PSRAM test: an overlay with  &psram { status = "okay"; };  (-DEXTRA_DTC_OVERLAY_FILE=...)
```

The Zephyr ST drivers need `hal_st` in the west workspace. The result goes to the USB console and to RTT.

## Bring-up checklist
- LIS2DUXS12 address: SA0 is not tied on the base board (R18 not fitted). The test reports whether 0x18 or 0x19 answers. Set `reg` in the base board devicetree.
- MAX-M10S: the module needs its first start (cold start, no almanac) outdoors with sky view for a fix.
- PSRAM: see `NRF/drivers/memory/README.md`.
- MAX77654: unplug and plug the USB cable and check the printed events; check the battery voltage against a multimeter.
