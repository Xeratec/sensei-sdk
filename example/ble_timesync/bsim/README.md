<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# Time sync on BabbleSim

Measures the node-to-node time error of `lib/sensei_timesync` without hardware: three simulated nRF5340 (one master, two
followers) on the BabbleSim 2.4 GHz radio simulator, each with its own crystal drift. The PPS pin of every device is logged by the
GPIO model and `analyze.py` compares the edges of the followers with those of the master.

```
bs_2G4_phy_v1
   |-- device 0: master      drift    0 ppm   BLE central, time reference
   |-- device 1: follower A  drift +40 ppm   BLE peripheral
   '-- device 2: follower B  drift -30 ppm   BLE peripheral
```

Each device is the example `example/ble_timesync/src_NRF` (app core, `master.conf` or `follower.conf`) and the NCS `ipc_radio`
network core with `src_NRF/sysbuild/ipc_radio.conf` (SoftDevice Controller built for BabbleSim). The whole chain runs:
controller anchor reports, IPC and DPPI to the application core clock mirror (RTC + TIMER), pairing, clock model, PPS output
(RTC compare, TIMER compare, GPIOTE toggle).

## Run

```sh
export SENSEI_NCS_WORKSPACE=<west workspace of NCS v3.4.1, read-only>
example/ble_timesync/bsim/build.sh    # first time: fetches BabbleSim (about 10 min under emulation)
example/ble_timesync/bsim/run.sh      # 120 s of simulated time, about 3 min of wall time under emulation
```

`--docker` is the default on macOS (image `sensei-bsim:ncs-3.4.1`, linux/amd64: the simulated boards are 32-bit x86 programs),
`--native` elsewhere (Linux x86_64 with `gcc-multilib` and `g++-multilib`). `build.sh` fetches BabbleSim and the nRF HW models
at the revisions pinned by `zephyr/west.yml` of the workspace into `bsim/babblesim-src` (`SENSEI_BSIM_SRC`); build results and
logs go to `bsim/out` (`SENSEI_BSIM_BUILD`). Options of `run.sh`: `--length S`, `--skip S`, `--threshold-us US`, `--drift-a PPM`,
`--drift-b PPM`. Exit code 0 means all followers are within the threshold.

Pulse mode: `build.sh --pulse-us 5000` (with its own `SENSEI_BSIM_BUILD`) builds the example with
`CONFIG_APP_PPS_PULSE_WIDTH_US`, so the PPS is a 5 ms pulse (`sensei_ts_pps_start_pulse`); `run.sh --rising-only` then compares
the rising edges only and prints the pulse width of the master.

## How the error is measured

* `-xo_drift=<fraction>` (nrf_hw_models, `trivial_xo.c`) sets `dev_time = (1 + drift) * phy_time` for a device: all its clocks
  (HF, LF, RTC, TIMER, radio timing) run that much faster than true time. +40 ppm is a clock that is 40 ppm fast.
* `-gpio_out_file=<file>` logs `time(microsecond),port,pin,level` for every output change. The time is the **device** time
  (`nsi_hws_get_time()`), not the simulator time. `analyze.py` converts it to true time with `true = dev / (1 + drift)`.
  Port 3 is P1 of the application core: the PPS pin is P1.06.
* Every follower edge is paired with the master edge of the same level that is nearest in true time (edges are 500 ms apart).
  After the first 20 s the script prints per follower the count, mean, std, min and max of `follower - master` in ns.
* The log has 1 us resolution (the HW models run on a 1 us grid), so each difference has up to +-1 us of quantization noise
  (std about 0.4 us). The result cannot show anything below that. The default threshold is 5 us: the simulated anchors of
  the peripheral differ from those of the central by 0 to 2 us (see below), so +-3 us is the expected worst case, and 5 us
  leaves margin. A tracked-drift failure is far larger: untracked 40 ppm would give 20 us per 500 ms edge, and an edge
  one RTC tick late (a lost TIMER compare) is 30.5 us.
* The script fails if a follower has no edges, if |difference| exceeds the threshold, if a follower edge has no master edge
  within 5 ms, or if less than 95 % of the master edges after the skip time have a follower edge.

## Result (default drifts, 120 s, skip 20 s)

| Follower | drift | edges paired | mean | std | min | max |
|---|---|---|---|---|---|---|
| A | +40 ppm | 200 | -465 ns | 528 ns | -1000 ns | +1000 ns |
| B | -30 ppm | 199 | -1598 ns | 530 ns | -3000 ns | -1000 ns |

With both followers at 0 ppm the error is constant: A 0 ns, B -2000 ns. The fitted rate of follower A is -40.0 ppm with
about 100 ns residual (B: -30 ppm), so the drift is tracked and no edge drifts away. The constant part comes from the
anchor points: the simulated SoftDevice Controller reports for the peripheral anchors that are 0 to 2 us earlier than the anchor of the
central for the same event (for example event 30: central 3116349 us, follower A 3116349, 3116348, 3116347 in
successive events; central 3623849, follower B 3623847 us, constant). That is the anchor measurement of the controller on the model's
1 us grid, not the library. On hardware `SENSEI_TIMESYNC_FOLLOWER_BIAS_NS` is the place to remove such a constant.

## What is simulated and what is not

Validated: the register-level chain on the application core (IPC receive to RTC clear over DPPI, RTC tick to TIMER clear,
software capture, RTC/TIMER compare via a DPPI channel group to GPIOTE), the controller anchor report events and their
event counters, the GATT sample transfer, the pairing across 16-bit counter and connection interval, the least-squares
clock model with the drift of the crystals, and the PPS scheduling and re-arming.

Not validated (the simulator has none of it, so do not read the numbers as a prediction for hardware):

* The radio receive timestamp jitter and the offset between central and peripheral anchor points on a real radio (the
  model has an exact, noise free air time and the same event counter gives the same instant to within the 1 us grid).
* Crystal behaviour other than a constant frequency offset: no temperature change, aging, jitter or drift variation,
  and no LFCLK from the RC oscillator. Master and followers share the HF clock of the device.
* Latencies of the DPPI, of the RTC clock domain crossing (a few PCLK16M cycles) and of the interrupts: all zero in the models.
  The race of a capture at an RTC tick (stale TIMER value) cannot occur.
* The test pulse and edge capture path (`ts-test-out` to `ts-capture`): devices cannot be wired together. Only the PPS output
  is covered.
* Radio loss and interference, and several followers contending (there are two).

## Changes made so that the library builds and runs on BabbleSim

* `lib/sensei_timesync/zephyr/Kconfig`: `depends on SOC_COMPATIBLE_NRF5340_CPUAPP` (the bsim board does not set
  `SOC_NRF5340_CPUAPP`).
* `lib/sensei_timesync/zephyr/CMakeLists.txt`: `-lm` only if not `CONFIG_ARCH_POSIX` (the 32-bit host libm pulls PIC thunks
  that the native simulator link rejects).
* `lib/sensei_timesync/zephyr/ts_timebase.c`: the "network core released early" check reads `NRF_RESET->NETWORK.FORCEOFF`, which
  the simulated reset block does not hold; skipped on BabbleSim (both cores start at the same instant).
* `lib/sensei_timesync/zephyr/ts_pps.c`, Kconfig `SENSEI_TIMESYNC_PPS_GUARD_COUNTS` (default 1 = unchanged behaviour; the bsim board file sets 32): the TIMER
  compare value that is close to the end of a tick. See "Findings".
* `src_NRF/boards/nrf5340bsim_nrf5340_cpuapp.{overlay,conf}`: pins, native console instead of RTT, `SENSEI_TIMESYNC_TIMER=0`
  (see below), PPS guard 16.
* `netshim/`: test-only Zephyr module for the network core build. `MPSL_TRIGGER_IPC_TASK_ON_RTC_START` of the NCS depends on
  `SOC_NRF5340_CPUNET`, which the nrf5340bsim network core does not set, so Kconfig refuses it. The MPSL library for BabbleSim
  has the function behind it; the module defines the two macros for the sources.
* `patches/nrf_hw_models-rtc-tick.patch`: the HW model does not generate the RTC `TICK` event ("TICK functionality is not
  modelled"). The library clears the sub-tick TIMER on every tick, so the patch adds the event. `build.sh` applies it to the
  fetched copy of `nrf_hw_models`.

### Findings about the models

* The nRF5340 HW models are compiled with one `NRF_TIMER_Type` layout (1440 bytes) while the application core HAL code uses its
  own (1432 bytes). Registers of application core TIMER1 and TIMER2 are therefore misaligned in the model and tasks written to
  them are lost. TIMER0 works, so the test uses `SENSEI_TIMESYNC_TIMER=0`. On the board the default (TIMER2) is unchanged.
* The models schedule everything on a 1 us grid. RTC ticks are 30.5 us apart, so two consecutive ticks are 30 or 31 us apart
  and a TIMER compare value above 480 counts is sometimes after the next clear and is lost. The PPS then stops for good.
  Hardware has the same constraint with a much smaller margin (the TIMER clear by the next tick), which is what
  `SENSEI_TIMESYNC_PPS_GUARD_COUNTS` is for.
