<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# ble_timesync

Measures the time difference between nRF5340 nodes that are synchronized with `lib/sensei_timesync`. Target: about
1 us (1 us is 0.34 mm of sound path), the base for localizing sound and vibration sources by time difference of
arrival. The example needs one wire and a few log lines. A logic analyzer gives a second, independent measurement.

## How it works

- The master is the BLE central. Its clock is the global time. It connects to up to four followers (BLE peripherals) with
  a 100 ms connection interval, 2M PHY, latency 0, and sends them a time sample every `CONFIG_SENSEI_TIMESYNC_SAMPLE_PERIOD_MS`.
- Both sides of a connection see the same connection event at the same physical instant (the anchor point). The
  controller reports it on its own clock. A follower fits a clock model (offset and rate) over these instants. The
  library description is in `lib/sensei_timesync/README.md`.
- Every node drives the pin `ts-pps` (500 ms period in global time: a rising edge on every full second, a falling edge
  half a second later; `CONFIG_APP_PPS_PULSE_WIDTH_US` > 0 makes it a pulse of that width instead) and timestamps edges on `ts-capture` in hardware.
- The master raises `ts-test-out` for 2 ms at irregular times (1.0 to 1.6 s apart). You wire it to `ts-capture` of
  every board. All boards see the same edge. Each follower notifies the global time of the edge to the master
  (example characteristic `c0f5a4f1-7d1b-4c8e-9a51-3e2f6b7d8a10`). The master matches the report with its own capture of
  the edge (closest in time, within 50 ms) and keeps the statistics of `follower - master` per follower.
- Positive error: the follower clock is ahead.

## Build and flash

Set `SENSEI_SDK_ROOT` and `ZEPHYR_BASE` as described in the root `Readme.md`. The role is chosen by a configuration file.
The build uses sysbuild and builds the network core image (`sysbuild/ipc_radio.conf`) as well.

```sh
cd $SENSEI_SDK_ROOT/example/ble_timesync/src_NRF

# SENSEI base board
west build -p -b nrf5340_senseiv1/nrf5340/cpuapp -d build/sensei_master   -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF -DEXTRA_CONF_FILE=master.conf
west build -p -b nrf5340_senseiv1/nrf5340/cpuapp -d build/sensei_follower -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF -DEXTRA_CONF_FILE=follower.conf

# nRF5340 DK
west build -p -b nrf5340dk/nrf5340/cpuapp -d build/dk_master   -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF -DEXTRA_CONF_FILE=master.conf
west build -p -b nrf5340dk/nrf5340/cpuapp -d build/dk_follower -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF -DEXTRA_CONF_FILE=follower.conf

west flash -d build/sensei_master
```

`CMakePresets.json` has the same four configurations (`sensei_master`, `sensei_follower`, `dk_master`, `dk_follower`).
Boards of both kinds can be mixed (see the voltage note below). One master, one to four followers.

Logs are on RTT (`JLinkRTTViewer` or `nrfutil` / `west attach`). The network core logs on RTT as well. For the
error of every single match, build with `-DCONFIG_APP_LOG_LEVEL_DBG=y`.

Application options (`Kconfig`): `APP_CONN_INTERVAL_MS` (100), `APP_TEST_PULSE*`, `APP_MATCH_WINDOW_MS` (50),
`APP_STATS_EVERY` (20), `APP_STATUS_PERIOD_S` (10). Library options: `CONFIG_SENSEI_TIMESYNC_*`.

## Wiring

All pins are 1.8 V on the SENSEI base board and 3.0 V (default) on the DK. Connect the grounds of all boards.

| Function | SENSEI base board | nRF5340 DK | Notes |
|---|---|---|---|
| `ts-test-out` (master only, output) | P1.13 (J2.25, `MSP_DR1`) | P1.04 (Arduino D2) | to `ts-capture` of every board |
| `ts-capture` (all, input, pull-down) | P1.12 (J2.23, `MSP_CS`) | P1.05 (Arduino D3) | from `ts-test-out` of the master |
| `ts-pps` (all, output) | P0.31 (J1.2) | P1.06 (Arduino D4) | to the logic analyzer |
| GND | any GND pin | any GND pin | common to all boards |

Test wiring (star):

```
master ts-test-out --+--> master ts-capture
                     +--> follower 1 ts-capture
                     +--> follower 2 ts-capture
GND of all boards joined
```

The wire delay is about 5 ns per meter, 1 us is 200 m of wire: the length does not matter at this accuracy.
Do not connect `ts-test-out` of a follower (it is not driven there).

Why these pins are free: on the SENSEI base board nothing in the board devicetree or the SDK drives P1.12, P1.13 and P0.31 unless the LPWAN
shield (P1.12, P1.13 and P0.31 are used there) or the sensor shield (P0.31) is fitted: build and run without shields.
They are not any of the reserved pins (P1.15, P0.06, P0.20, P0.28, P1.04, P1.05, P1.06, P0.08 to P0.11, P1.00, P1.01).
On the DK, P1.04 to P1.06 are Arduino header pins that no devicetree node of the board uses (LEDs are P0.28 to P0.31,
buttons P0.23, P0.24, P0.08, P0.09, the UART is P0.20 and P0.22, the Arduino UART is P1.00 and P1.01).

Voltage note: a 1.8 V output does not reach the input threshold of a 3.0 V input (0.7 VDD = 2.1 V). Mixing SENSEI
boards and DKs on the capture wire needs a level shifter, or set the DK VDD to 1.8 V with the nRF Connect Board
Configurator. Boards of the same kind need nothing.

## Run

1. Flash the master and one or more followers. Connect the wires.
2. Start the RTT logs. The master finds and connects to every follower by the service UUID, the followers advertise.
3. The follower state goes UNSYNCED, then SYNCED once the model has `CONFIG_SENSEI_TIMESYNC_MIN_POINTS` points (one
   point per `CONFIG_SENSEI_TIMESYNC_PAIR_PERIOD_MS`, 1 s by default). The estimate keeps improving until the window
   (`CONFIG_SENSEI_TIMESYNC_WINDOW` points) is full. The statistics include the settling phase: press the button sw0 of the master after about 30 s. It prints
   the statistics and starts them again.

### Log lines

Master:

```
F0 connected: AA:BB:CC:DD:EE:FF (random)
F0: subscribed to edge reports
Status: MASTER, points 0, rate 0.000 ppm, resid 0 ns, uncert 0 ns
F0 AA:BB:CC:DD:EE:FF (random): n 40, mean 120 ns, std 450 ns, min -900 ns, max 1100 ns, uncert 1500 ns, missed 0, not synced 3, unmatched 0
```

- `n`, `mean`, `std`, `min`, `max`: statistics of `follower - master` in ns over all matched edges since the follower
  connected. Printed every 20 matches, on a disconnection, and when the button sw0 of the master is pressed (all followers; the
  statistics restart then).
- `uncert`: the uncertainty the follower reported with its last edge.
- `missed`: reports missing in the sequence of the follower (a lost notification or a lost edge on the master side).
- `not synced`: reports from a follower that was not SYNCED (not in the statistics).
- `unmatched`: synced reports for which the master has no capture within the window. Nonzero means a missed edge on
  the master, or a time error above 50 ms.

Follower: `Time sync state: SYNCED`, and a status every 10 s:
`Status: SYNCED, points 16, rate -12.345 ppm, resid 300 ns, uncert 900 ns` (rate: master clock relative to the
local clock; resid: RMS residual of the fit).

Expected, not yet measured on hardware: `mean` within a few hundred ns of zero after the fit has settled, `std` about
1 us, `max - min` below 5 us. A constant `mean` offset that stays across runs is a hardware path difference between the
boards: `CONFIG_SENSEI_TIMESYNC_FOLLOWER_BIAS_NS` removes it. Measure it with the same wiring on both boards.

### Logic analyzer

Independent of the capture path: connect `ts-pps` of the master and of the followers to the analyzer, ground to the
boards, 100 MHz or more (the edges are 10 ns apart in the best case, 8 MS/s resolves 125 ns).

1. Trigger on the rising edge of the master `ts-pps`.
2. Measure the time from the master edge to the rising edge of each follower. This is the error, with the same sign as
   above (a follower edge before the master edge: follower clock ahead).
3. Repeat for many seconds. The edges of both polarities are timed (period 500 ms), so every second gives two
   measurements. The spread is the error of the synchronization, including the follower model and the GPIOTE and
   timer paths. A constant difference is the hardware path difference as above.

The capture method measures the input path (GPIOTE to RTC and TIMER capture), the PPS method the output path (compare
to GPIOTE). If both give the same spread the result is confirmed.

## Troubleshooting

| Symptom | Check |
|---|---|
| `sensei_ts_ble_init failed` | the network core image has no `CONFIG_BT_CTLR_SDC_CONN_ANCHOR_POINT_REPORT`: use the sysbuild of this example (`sysbuild/ipc_radio.conf`) and `west build -p` after changes |
| Follower stays UNSYNCED | `samples` in the status stays 0: no connection, or the master did not discover the service. `points added` grows but the state does not change: wait for the window to fill. Resets count high: the interval changes (see below) or the anchors jump |
| `PPS start failed` or `Capture start failed` | no GPIOTE or DPPI channel left (-ENODEV), or the pin is in use |
| Master log shows no `Found follower` | follower not advertising (`Advertising the time sync service` in its log), other master already connected to it, `CONFIG_BT_MAX_CONN` reached |
| `unmatched` grows | master capture missing: wire from `ts-test-out` to the master `ts-capture` open, or the 1.8 V and 3.0 V mix |
| `missed` grows | capture wire noisy (extra edges) or a follower notification lost. Edge counts differ: check the pull-down and wire length |
| Error of exactly 30.5 us | a capture within one RTC tick race of the library (see `lib/sensei_timesync/README.md`) |
| Mean offset of hundreds of ns, constant | hardware path difference, calibrate with `FOLLOWER_BIAS_NS` |
| Connection interval log shows another value than 100 ms | a peripheral requested new parameters: the follower uses `CONFIG_BT_GAP_AUTO_UPDATE_CONN_PARAMS=n`, other devices may not |
| No RTT output | the SENSEI board needs the debug probe on the SWD pins; the DK has an on-board J-Link |

The HFXO is requested while the library runs (`CONFIG_SENSEI_TIMESYNC_HFXO`): the sleep current is higher than
without it. This example measures, it is not a low power example.
