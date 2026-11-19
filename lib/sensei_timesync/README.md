<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# sensei_timesync

Relative time synchronization between SENSEI nodes (nRF5340), targeting about 1 us between nodes. It is the
base for localizing sound and vibration sources by time difference of arrival (TDOA): 1 us is 0.34 mm of sound
path in air.

The library adds no message timing of its own. Both radios of a BLE connection already see each connection event
at the same instant (the anchor point), and the SoftDevice Controller reports that instant on its clock. A
follower matches its anchor reports with the master's and fits a clock model to them. Hardware paths
(GPIOTE, DPPI, RTC and TIMER) then timestamp external edges and drive timed outputs without interrupt latency.

## Clocks

| Clock | What it is | Where |
|---|---|---|
| Controller clock | The SoftDevice Controller clock on the network core (RTC based, us). Anchor point reports use it | network core |
| Local time | A mirror of the controller clock on the application core, in ns | `sensei_ts_local_ns()` |
| Global time | The local time of the master. On the master, global == local | `sensei_ts_now()`, `sensei_ts_local_to_global()` |

How the local time works:
- **Coarse part.** The application core RTC is cleared over DPPI from IPC RECEIVE[4]. MPSL triggers that IPC
  channel when it starts the controller RTC (`CONFIG_MPSL_TRIGGER_IPC_TASK_ON_RTC_START`). So the application RTC
  counts the controller's 32.768 kHz ticks, with a constant app/net path delay that is the same on every node.
- **Fine part.** A TIMER is cleared on every RTC tick and gives the part below one tick: 62.5 ns at 16 MHz,
  1 us at 1 MHz.

## How a follower gets the global time

1. **Anchor reports.** The SoftDevice Controller reports `(event_counter, anchor_point_us)` for each connection event:
   - on the central, every interval;
   - on the peripheral, every event in which it received the central's packet.
2. **Master samples.** The master (central) sends `(counter, anchor_us, interval_us, epoch)` to each follower every
   `CONFIG_SENSEI_TIMESYNC_SAMPLE_PERIOD_MS`. This is a 20 byte GATT write without response; the format is in
   `ts_msg.h`.
3. **Pairing (`ts_pair.h`).** For each of the follower's anchor reports, the follower takes the master time of that
   event as `anchor_us + (c' - counter) * interval_us`.
   - The projection is done in the master's time domain, where the anchor points are exactly `interval_us` apart,
     so it adds no drift error.
   - Projections that might cross a connection update are refused.
4. **Averaging.** All pairs of one `CONFIG_SENSEI_TIMESYNC_PAIR_PERIOD_MS` are averaged into one point. Before that,
   each pair is screened against the current model.
5. **Clock model (`ts_model.h`).**
   - It fits `master = local + offset + rate * (local - x)` by least squares over the last
     `CONFIG_SENSEI_TIMESYNC_WINDOW` points.
   - It rejects outliers and resets on steps.
   - It estimates its own uncertainty, which grows in holdover.
6. **States.**
   - `SYNCED`: a valid model and recent samples.
   - `HOLDOVER`: the master link is down or silent for `CONFIG_SENSEI_TIMESYNC_HOLDOVER_MS`. Global time is still
     given, with a growing uncertainty.
   - `UNSYNCED`: no model.
   - `MASTER`: the node is the master.

## Files

- `include/sensei_timesync/ts_model.h`, `ts_pair.h`, `ts_msg.h`: the portable core (`src/`, no OS, tested on the
  host). The model also maps any monotonic counter to global time, for example the sample index of an audio
  stream on GAP9 (see below).
- `include/sensei_timesync/timesync.h`: the Zephyr API on the nRF5340 application core (`zephyr/`).
  - Clocks and conversions.
  - Hardware capture: `sensei_ts_capture_start()` (GPIO edge), `sensei_ts_capture_event_start()` (any peripheral
    event, for example `NRF_SAADC_EVENT_END`), `sensei_ts_capture_stop()` for both.
  - Timed output: `sensei_ts_pps_start()` (50 % toggle), `sensei_ts_pps_start_pulse()` (rising edge at k * period,
    falling edge `width` later), `sensei_ts_pps_stop()`.
  - BLE roles: `sensei_ts_ble_init()`, `sensei_ts_master_attach()`; the follower GATT service is built in.
- `tests/`: host tests. They include a simulation of the full chain: message, pairing and model with a +40 ppm
  master, a −25 ppm follower and 1 us anchor jitter.
- Example and measurement: `example/ble_timesync`. It has a one-wire accuracy test, a PPS output for a logic
  analyzer and a BabbleSim test.

## Use

Application `prj.conf`:

```
CONFIG_BT=y
CONFIG_SENSEI_TIMESYNC=y
CONFIG_SENSEI_TIMESYNC_FOLLOWER=y      # and/or CONFIG_SENSEI_TIMESYNC_MASTER=y (needs BT_CENTRAL)
```

The network core image (`sysbuild/ipc_radio.conf`) **must** have:

```
CONFIG_BT_CTLR_SDC_CONN_ANCHOR_POINT_REPORT=y
CONFIG_MPSL_TRIGGER_IPC_TASK_ON_RTC_START=y
CONFIG_MPSL_TRIGGER_IPC_TASK_ON_RTC_START_CHANNEL=4   # = CONFIG_SENSEI_TIMESYNC_IPC_CHANNEL
```

Code:

```c
bt_enable(NULL);
sensei_ts_ble_init();                  /* anchor reports and vendor event handler */
/* master: after connecting to a follower */
sensei_ts_master_attach(conn);
/* everyone */
sensei_ts_pps_start(&pps_pin, 500000000ull);       /* rising edge on every global second */
sensei_ts_capture_start(0, &drdy_pin, GPIO_INT_EDGE_RISING, on_edge, NULL);
sensei_ts_capture_event_start(1, nrf_saadc_event_address_get(NRF_SAADC, NRF_SAADC_EVENT_END), on_edge, NULL);
sensei_ts_pps_start_pulse(&strap_pin, 1000000000ull, 5000000ull); /* 5 ms pulse every global second, low while UNSYNCED */
/* in on_edge(ch, local_ns, user): sensei_ts_local_to_global(local_ns, &t) — or keep local_ns and convert later */
```

Use the anchor point connection only for the time: the master sets the connection parameters. A follower should not
request other parameters (`CONFIG_BT_GAP_AUTO_UPDATE_CONN_PARAMS=n`). A parameter update is handled: pairing
restarts. But every update costs one averaging period.

## Accuracy budget

| Source | Size | Notes |
|---|---|---|
| Anchor point jitter of the controller | about 1 us per report (Nordic) | averaged: about 10 pairs per point at 100 ms, then the fit over 16 points |
| Model fit, host simulation | 0.7 us RMS / 2.4 us max with one raw pair per second and 1 us jitter on both sides | the averaging of all pairs per period reduces this further (by about the square root of the pairs per period for the follower side) |
| Sub-tick TIMER | 62.5 ns resolution | crystal accurate with `CONFIG_SENSEI_TIMESYNC_HFXO=y` |
| Radio propagation | 3.3 ns per meter | 0.33 us at 100 m: below the budget; can be compensated with known node distances |
| Master/follower anchor definition | constant, unknown until measured | cancels between followers; master vs follower: calibrate with `CONFIG_SENSEI_TIMESYNC_FOLLOWER_BIAS_NS` or use a master that does not sense |
| Holdover (link down) | rate error times time, plus crystal wander with temperature (32.768 kHz crystals: about −0.035 ppm per °C² away from 25 °C) | `ts_fit_bridge()` spreads the error of a gap over it once synchronization is back |

Expected: about 1 us RMS between nodes while synced. This is not yet measured on hardware;
`example/ble_timesync` measures it.

## Event capture, pulse PPS and watchdog

- `sensei_ts_capture_event_start(channel, event_address, cb, user)` shares the path of the GPIO capture (event ->
  DPPI -> RTC CAPTURE + TIMER CAPTURE + EGU). The library sets the PUBLISH register of the event through nrfx_gppi,
  so the event must not be published to another DPPI channel by anyone else: a PUBLISH register holds one channel and
  the last writer wins. One channel serves one event source. `sensei_ts_capture_stop()` clears the PUBLISH register.
- `sensei_ts_pps_start_pulse(pin, period, width)`: rising edge at global k * period, falling edge `width` later, pin low
  otherwise (also while UNSYNCED and before the first edge). The single compare of the module is armed alternately for
  the two edges; the interrupt of the rising edge arms the falling one. The compare needs a target two RTC ticks ahead
  plus the interrupt latency, so `width` must be at least 100 us (and at most period / 2). If the falling edge cannot be
  armed in time the pulse is ended at once and a warning is logged.
- PPS watchdog: every armed edge schedules a delayable work item for its due time plus 100 ms. If no EGU edge
  interrupt came (a missed compare), it re-arms from the newest model and counts the event (log line "PPS watchdog").
  Without it a missed compare stopped the output for good. Not active in UNSYNCED (the output resumes with the
  state change). `CONFIG_SENSEI_TIMESYNC_PPS_GUARD_COUNTS` is unchanged.

## Resources (application core)

| Resource | Use |
|---|---|
| RTC0 (`CONFIG_SENSEI_TIMESYNC_RTC`) | mirror of the controller clock. CC0 is the PPS compare, CC1 the software capture, CC2 and CC3 the edge captures. The overflow interrupt counts overflows. RTC1 is the Zephyr system timer |
| TIMER2 (`CONFIG_SENSEI_TIMESYNC_TIMER`) | sub-tick part, cleared on every RTC tick. Same CC use as the RTC |
| EGU0 (`CONFIG_SENSEI_TIMESYNC_EGU`) | ch0 software capture, ch1 PPS edge done, ch2 mirror started, ch3 and up edge captures |
| DPPI | 3 channels for the timebase, 1 per capture channel, 2 and one channel group for the PPS (nrfx_gppi allocator) |
| GPIOTE | 1 channel per GPIO capture (none for an event capture) and 1 for the PPS, from the instance the Zephyr GPIO driver uses |
| IPC | receive channel `CONFIG_SENSEI_TIMESYNC_IPC_CHANNEL` (4) |
| HCI | the vendor event callback (`bt_hci_register_vnd_evt_cb` allows only one) |
| HFXO | requested for as long as the module runs (`CONFIG_SENSEI_TIMESYNC_HFXO`) |

## Power

The TIMER at 16 MHz and the HFXO run for as long as the module runs. They cost several hundred uA on top of the
sleep current. This suits phases of active sensing (GAP9 audio costs milliwatts anyway), not deep sleep.

For long idle phases (the avalanche case):
1. Disconnect.
2. Keep the local timestamps of the events.
3. Convert them after the next synchronization with `ts_fit_bridge()`.

Switching the fine timer and the HFXO on demand is a planned improvement.

## Limits and known issues

- **RTC tick race.** A capture within one or two 16 MHz cycles of an RTC tick can pair the new tick with the TIMER
  value of the previous tick. With the HFXO, counts beyond one tick are treated as stale. A capture in the last
  ~18 ns of a tick is then off by one tick (30.5 us). The probability is about 1e-3 per capture. Robust
  estimators (median, regression over many edges) remove such points.
- **Early network core start.** The network core must start after this module's init. The init runs at POST_KERNEL
  after the mbox driver, and the core is released later. If the network core already runs (an application-only
  reset, a bootloader that started it), `sensei_ts_ble_init()` fails with -EIO: reset both cores.
- **Single vendor event callback.** Only one vendor event callback exists in Zephyr. An application that needs
  other vendor events must dispatch them itself.
- **Multi-hop.** A node that is master and follower at the same time serves its followers in its own local time,
  not in the upstream global time.
- **Coded PHY.** Use 2M or 1M PHY for the best anchor timing. The precision on Coded PHY (long range) has not been
  measured.

## Using it for sensors

- **Data-ready lines on the nRF5340**, for example the LIS2DUXS12 INT1 on P0.23:
  1. Timestamp every edge with `sensei_ts_capture_start()`.
  2. Fit sample index to time with a `ts_model`.
  3. This gives every sample a global time, with no interrupt latency in it.
- **GAP9 audio.** The microphone is clocked by GAP9's own oscillator (24.576 MHz, unrelated to the nRF clocks).
  1. Drive `sensei_ts_pps_start()` on a line to GAP9. P1.06 to pad 57 is planned; it is unverified on hardware.
  2. On GAP9, record the audio sample index at each edge.
  3. On GAP9, fit index to global time with the portable `ts_model` (it builds without an OS).
  4. The precision is set by how exactly GAP9 reads the sample position at the edge, not by the radio.

## Background

A semester project (2025/26) synchronized nodes with an NTP-style two-way handshake of GATT messages timed in
application code. Its error contained a constant of 0.5 to 20 ms from asymmetric stack paths, which depended on the
connection interval and the firmware. Its reported ~10 us is the scatter after that constant was removed by hand per
run. Anchor points avoid this: both radios observe the same physical event, so no message delay enters the
estimate.

## Tests

- Host: `cmake -S tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>`, or
  `make test-host` in the SDK root.
- BabbleSim (simulated nRF5340s with crystal drift): `example/ble_timesync/bsim/`.
- Hardware: `example/ble_timesync` (Readme).
