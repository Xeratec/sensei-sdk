<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# LPWAN shield firmware (nRF9151)

Firmware for the nRF9151 on the SENSEI LPWAN shield. It is an SPI slave of the nRF5340 and gives
it LTE-M, NB-IoT, LoRaWAN (through the LR1121) and a GNSS fix of the nRF9151. Any SENSEI app on the nRF5340 can use it through
the client library `lib/sensei_lpwan`.

- Pin map and hardware findings: [HARDWARE.md](HARDWARE.md)
- Messages: `lib/sensei_link/proto/sensei/lpwan.proto` (`LpwanRequest`, `LpwanResponse`, `LpwanEvent`)
- nRF5340 client and the protocol cores: [lib/sensei_lpwan](../../../lib/sensei_lpwan/README.md)

## Build

NCS v3.4.1. The board `sensei_lpwan_nrf9151` comes from the SENSEI SDK. The non-secure target
`sensei_lpwan_nrf9151/nrf9151/ns` is the one to use: sysbuild builds TF-M (secure image) and the app and
merges them. `sensei_lpwan_nrf9151/nrf9151` is the secure target without TF-M.

```sh
source <ncs v3.4.1 toolchain env>
export ZEPHYR_BASE=<ncs>/zephyr SENSEI_SDK_ROOT=<path to the SENSEI SDK>
cd $SENSEI_SDK_ROOT/NRF/apps/lpwan_shield
M=-DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF
west build -b sensei_lpwan_nrf9151/nrf9151/ns -- $M -DEXTRA_CONF_FILE=conf/debug.conf
SENSEI_SHIELD_MCUBOOT_KEY=<PEM file> \
west build -b sensei_lpwan_nrf9151/nrf9151/ns -- $M -DEXTRA_CONF_FILE=conf/release.conf -DSB_CONFIG_SENSEI_RELEASE=y
```

MCUboot is part of every build (sysbuild). Debug is signed with MCUboot's public development key (anyone can sign for it: lab
only). Release needs its own private ECDSA P-256 key (`imgtool keygen -k key.pem -t ecdsa-p256`) in
`SENSEI_SHIELD_MCUBOOT_KEY`. There is no fallback to the key of the nRF5340 node (`SENSEI_MCUBOOT_KEY`): the build stops
without it, stops if it is the same key as the node's, and stops if the Release configuration and `SB_CONFIG_SENSEI_RELEASE`
do not match. Keep the key outside git; MCUboot holds its public half for the life of the shield.
Both images carry a protected TLV with the board they are made for (`imgtool --custom-tlv 0xa0 sensei-lpwan-nrf9151` here,
`sensei-ieq-nrf5340` for the node; `sysbuild.cmake`, the signature covers it). The shield refuses an image without its
kind at `dfu_finish` (`DFU_FAIL_BOARD`), the host tools refuse a file of the wrong kind.
The SENSEI IEQ node builds it with `make nrf9151` and `make nrf9151-release` (`firmware/Makefile`).

| Build | Flash (non-secure image) | RAM | MCUboot | TF-M |
|---|---|---|---|---|
| Release | 113.8 kB | 59 kB | 27.4 kB of 64 kB | 64.3 kB |
| Debug (RTT log, modem trace) | 174.9 kB | 69 kB | 27.4 kB of 64 kB | 64.3 kB |

The non-secure slot is 196.6 kB (188 kB usable) with MCUboot; 212 kB without. Kconfig: `LPWAN_SHIELD_LTE`, `LPWAN_SHIELD_LORA`,
`LPWAN_SHIELD_GNSS` (all default y). Turn one off to save flash; the GNSS costs 5.6 kB in Release and 6.7 kB in Debug (before it: 108.2 kB and 168.2 kB).

## Simulated shield (native_sim)

The same app builds for `native_sim/native/64` without hardware: the SPI link is a socket, the LTE modem and the LR1121 are
fakes, the LTE socket is a host socket. A simulated node (SENSEI IEQ `firmware/sim`) talks to it, and the radio path runs
against a collector. Build, options, control commands and limits: [sim/README.md](sim/README.md).

```sh
west build -b native_sim/native/64 --no-sysbuild -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF
```

## Flash

```sh
west flash -d <build dir>        # the first time: MCUboot, TF-M and the app (tfm_merged.hex + mcuboot)
```

Later updates need no probe: see "Update over the air".

The shield has no UART. The debug header J12 gives SWD and RTT. In the Debug build the log is on RTT
channel 0. The nRF9151 reset and SWD are not reachable from the nRF5340.

## How it works

```
nRF5340 (master)                           nRF9151 (slave)
  lpwan_client.c --- request frame ------>  link.c (SPIS thread) --> service.c --> lte.c / lora.c
                 <-- response frame (same seq) --
                 <-- event frames (EVENT line) --  lte.c / lora.c --> svc_emit()
```

- **Transfer:** every SPI transfer is full duplex with a fixed length (1056 B). The slave arms its
  buffers and drives BUSY low. The master may start only while BUSY is low. BUSY goes high in the
  SPIS interrupt when the transfer ends. The master sends a `LpwanRequest` (SPI frame type 0x20),
  the slave answers in the next transfer with a `LpwanResponse` (0x21) of the same `seq`.
- **Events:** the slave raises EVENT while it holds events (`LpwanEvent`, 0x22): joined, connected,
  disconnected, tx done, downlink, network time, error. The master fetches them with idle
  transfers until EVENT is low.
- **Reliability:** frames carry a CRC-16 over header and message, and a frame version. The slave keeps every message
  armed until the master confirms it (ACK flag plus the toggle bit of that message), the master drops repeats (toggle
  flag), and a repeated request with the same `seq` is not executed twice. Both sides have timeouts (BUSY wait,
  response wait, transmit result, connect). `LpwanVersion.protocol` is checked by the client at power-on.
- **Diagnostics:** the SDK module `NRF/system/diag` keeps the reset cause, a boot counter and the last fatal error in the
  retained RAM (the last 1 kB of the application RAM, `sensei_lpwan_nrf9151_nrf9151_common.dtsi`). A fatal error of the
  application core is recorded and resets the chip. A fault of the modem core (`nrf_modem_fault_handler`) is recorded the same way
  (reason `0x80`, PC in the modem firmware, the modem library's code as status) before the reset. `src/diag.c` puts
  `reset_cause`, `boot_count` and the `fault` into `LpwanStatus` (fields 9 to 11); the fault stays in every status until the nRF5340 has it
  and sends `diag_ack` (kept in RAM only). The nRF5340 asks for the status after each
  version read and reports the reset cause as `Status.shield_reset_cause`. The shield power is cut between uses, which clears the retained RAM:
  the record survives a reset of the running shield, not a power cut. Untested on hardware: reading the reset reason from the non-secure image.
- **Self-protection:** the link thread feeds a task watchdog (hardware watchdog behind it, 15 s); a modem fault resets
  the chip (`nrf_modem_fault_handler` in `main.c`); a send without result ends with `tx_done(failed)` after the time the
  backend expects plus 5 s (LoRa, up to 10 min), else after `LPWAN_SHIELD_SEND_TIMEOUT_S` (45 s), so the state machine
  never stays in SENDING. While the LR1121 still holds a LoRa frame the send is not failed at that time (the frame
  would go out later, and a retry would send it twice): the watchdog looks again, and resets the LR1121 after twice
  the time (the frame is dropped; `tx_done(failed)`, an error and `disconnected`). Each send has one `tx_done`: a real
  result that no send waits for is kept for the status (`late_tx_seq`, once). The watchdog acts only for the send it was
  armed for. The nRF5340 sees a reset as a lost
  link and power-cycles the shield if it does not come back. The LTE worker can wait up to 600 s for a registration
  (legitimate), so the task watchdog does not cover it.
- **State machine** (`lib/sensei_lpwan/src/lpwan_sm.c`, on both MCUs): idle, connecting, connected, sending,
  disconnecting, error. One technology at a time: none, LTE, LoRa or BLE. A request that does not fit
  the state gets a result code (`LPWAN_BUSY`, `LPWAN_NOT_JOINED`, `LPWAN_NOT_CONNECTED`, ...). The
  master checks locally first.
- `lte_disconnect` releases the active technology (LTE detaches, LoRa is parked, the session stays).

The protocol cores (`lib/sensei_lpwan/src/link_slave.c` and `lpwan_master.c`) have no Zephyr
dependency and are tested on the host (`lib/sensei_lpwan/tests`).

## Why the commissioning version stopped answering (and what is different)

The commissioning project stopped answering after `nrf_modem_lib_init()` unless RTT was attached.
Not reproduced on hardware. The code shows four causes, all removed here (`src/link.c`):

1. BUSY was released after `arm + k_sleep(10 us)` steps. A sleep of a few us is one kernel tick, so
   the timing depended on the rest of the system (a debugger or RTT changes it). Here BUSY goes low only
   after `spi_transceive_cb()` returned, and high in the SPIS interrupt.
2. A 24 s timeout left a transfer armed, then the loop armed another one at once. The driver refuses
   while the first is pending. The loop spun on that error at the highest priority and starved the modem
   threads. Here an armed transfer never times out and a failed arm backs off 10 ms.
3. One mutex was shared by the SPI thread and the LTE and LoRa workers, and held while the command
   handler ran. Here the handler only queues work.
4. The modem library start (seconds) ran in a worker while BUSY stayed high with no way to answer. Here
   the link answers at any time, the modem starts in its own thread at the first connect.

Also: logging is deferred (immediate mode with RTT disables interrupts while it copies), and
`CONFIG_SEGGER_RTT_MODE_NO_BLOCK_SKIP` stops a full RTT buffer from blocking when no debugger is attached.

## LTE-M and NB-IoT (`src/lte.c`)

Request `lte_configure` (from the client configuration): mode, PSM TAU and active time, eDRX, APN, server host
and port, UDP or TCP. `lte_connect` starts the modem library (first time only), sets the system mode,
the power saving, registers, resolves the server and opens the socket, then reports `connected` and the
network time (from the modem through `date_time`). Three paths run in their own threads: connect and
send (worker), receive (own thread, blocks in `poll`), and the `lte_lc` callbacks. Downlinks (UDP
datagrams) become `DownlinkRx` events. PSM and eDRX values are encoded in `lib/sensei_lpwan/src/lpwan_timers.c`
(3GPP timer coding, host tested; the eDRX code is the 3GPP value, NB-IoT has ten of the sixteen).
`lte_disconnect` ends a registration wait at once (a cancel flag; only `lte_connect` clears it, and the connect checks it
and `want_connected` after it reset the semaphore, so a disconnect that comes before the connect starts is not lost, and
a connect request that follows is not taken for registered). Coverage lost while connected gives an error event (`registration
lost`); sends fail at once until the modem registers again.

**RAI and the answer of the server** (`CONFIG_LPWAN_SHIELD_RAI`, default y; `AT%RAI` is switched on at the modem start). A send
carries `Send.rai` (the last chunk of a message): `lte_send()` sets the socket option `SO_RAI` through `lpwan_rai_send()`
(`lib/sensei_lpwan/src/lpwan_rai.c`): `LTE_RAI_LAST` before the send; `LTE_RAI_ONE_RESP` before the send, then the worker waits up
to `LPWAN_SHIELD_LTE_ANSWER_WAIT_S` (10 s, `_NBIOT_S` 30 s on NB-IoT; the time counts from `send()`) for the answer and sets `RAI_NO_DATA`. The receive thread decides what a datagram is:
the 3-byte ack of the chunk that was sent (consumed), an ack of another chunk (dropped), or a downlink (forwarded as `DownlinkRx`);
the ack and the downlink both end the wait. `tx_done` is ok when the datagram went out: with the answer `TxDone.acked`, without it `TxDone.unconfirmed` ("sent, unconfirmed", not a
failure); `LpwanStatus.lte_answers` and `lte_no_answer` count. The RSRP in the status and in `conn_eval` has a validity flag (`rssi_valid`,
`rsrp_valid`). The server rule is in `lib/sensei_lpwan/README.md`. The RAI configuration of the cell
(`LTE_LC_EVT_RAI_UPDATE`) is `LpwanStatus.rai_support`. A failed `setsockopt` is logged and does not fail the send.

**Counters for the charge counter of the node**: `LpwanStatus.rrc_connected_ms` (time in the RRC connected state, from
`LTE_LC_EVT_RRC_UPDATE`; a period ends at idle or when the registration is lost), `attaches` (registrations) and `psm_entries`
(`LTE_LC_EVT_MODEM_SLEEP_ENTER` of type PSM; needs `LTE_LC_MODEM_SLEEP_NOTIFICATIONS`, which the app selects with the
threshold at its minimum of 10.24 s) count since the shield started (`lib/sensei_lpwan/src/lpwan_counters.c`, host tested). Untested on
hardware: whether the modem reports every PSM entry and the time of the RRC events.

**Connection evaluation** (`CONFIG_LPWAN_SHIELD_LTE_EVAL`, default y): the request `lte_eval` posts `lte_lc_conn_eval_params_get()` to
the worker (a few seconds at most); the `conn_eval` event carries energy estimate, RSRP, RSRQ, coverage level, repetitions and the RAI
support of the cell. A result of 1 to 4 or an error is `valid = false`.

**Proprietary PSM** (`CONFIG_LPWAN_SHIELD_PROPRIETARY_PSM`, default y): `LTE_PROPRIETARY_PSM_REQ`. The modem sleeps as in PSM when the
network grants none (the event `LTE_LC_EVT_PSM_UPDATE` with a TAU of -1 is logged). It acts only then; needs a modem firmware that has it.

Low power: the modem stays off until the first connect. After the uplink, the nRF5340 sends
`sleep(0)`: the nRF9151 stays registered and the modem sleeps in PSM; the CPU idles with the SPIS
armed. `sleep(1)` or `lte_disconnect` puts the modem offline.

## GNSS (`src/gnss.c`)

`CONFIG_LPWAN_SHIELD_GNSS` (default y, needs `LPWAN_SHIELD_LTE` for the modem library). One fix per request, on the NCS `nrf_modem_gnss`
API. Not tested on hardware.

- **Messages** (`lpwan.proto`, tags 50 to 59): the request `gnss_fix { timeout_s }` (0: 120 s, at most 900) is answered at once (`LPWAN_OK`,
  `LPWAN_BUSY`, `LPWAN_INVALID`); the result is the event `gnss_result`: `GnssFixResult` with the request's `seq`, a status (`ok`, `timeout`,
  `blocked`, `failed`, `cancelled`; `busy` and `not supported` are the client's verdicts), latitude and longitude in degrees x 1e7, altitude
  (above the WGS-84 ellipsoid, as the modem reports it), accuracy (2D, 1 sigma, rounded up), satellites used, the UTC time of the fix and the time
  to fix. `gnss_cancel` ends a running fix (the result follows as `cancelled`). `LpwanVersion.gnss` says that the app has the GNSS;
  `LpwanStatus.gnss_attempts`, `gnss_fixes` and `gnss_on_ms` (time GNSS was active) count since the shield started. A shield without it (an older
  app, or `LPWAN_SHIELD_GNSS=n`) answers `LPWAN_UNSUPPORTED`: the client takes that, and a missing `gnss` in the version, as "not supported" and
  nothing else changes.
- **Modem.** The system mode is LTE-M or NB-IoT **with GPS** (`LTE_LC_SYSTEM_MODE_LTEM_GPS`, `_NBIOT_GPS`), set at the connect and, for a fix
  without LTE (a LoRa-only node), by the fix itself (`lte_gnss_begin()` starts the modem library and, if the modem has no GNSS in its system mode
  and is off or offline, sets it). The normal functional mode activates the GNSS that is in the system mode, so the connect takes it back at once
  (`LTE_LC_FUNC_MODE_DEACTIVATE_GNSS`): GNSS is active only for an attempt: a worker thread sets functional mode 31, starts a single fix
  (`fix_interval` 0, `fix_retry` = the timeout, no NMEA strings), waits for a valid PVT frame, the timeout or a cancel, stops and ends the fix
  (`lte_gnss_end()`): functional mode 30 if the modem was running when the fix began, **0 (power off) if it was off (mode 0 or 4)**, also when LTE is wanted but the connect failed, so that the modem saves what the GNSS learned
  (ephemerides, time, last position) before the node cuts the shield's supply. Then it queues the event. It works with LTE off. The functional
  mode changes of a fix and of the LTE worker (release, deep sleep) take one lock; a release or deep sleep that comes while a fix has GNSS active
  deactivates only LTE (mode 20), and the end of the fix does the rest. The fix time stamps the PVT frame: the result carries `age_ms` and the
  UTC of the fix rounded with the receiver's milliseconds. **Warm and cold start.** A power cycle of the shield between two fixes is a warm start
  (the modem keeps its data in its flash); a cold start needs `nrf_modem_gnss_nv_data_delete()`, which is a debug feature. There is no shell on
  the shield: change `gnss.c` for a test. Untested on hardware: that mode 0 before the cut keeps the data (node repo `firmware/docs/TESTING.md`, T15.15).
- **Fix and LTE.** The modem runs GNSS only while LTE is idle (PSM, eDRX gaps, LTE off); there is no priority mode. If it keeps GNSS blocked
  (`BLOCKED` event, or the PVT flags say that the windows were missed or too short, typical with PSM and eDRX both off) until the timeout, the
  result is `blocked`. The rule for a fix and an LTE message (`lib/sensei_lpwan/include/sensei_lpwan/lpwan_gnss.h`, host-tested, the order of the
  checks is `lpwan_admit_request()`, which `service.c` calls): an LTE send goes through during a fix (the modem gives LTE priority); what changes the
  functional mode (LTE connect, release, configure, deep sleep) and the start of a shield update exclude the fix. A fix is refused
  (`LPWAN_BUSY`) while LTE connects, sends or disconnects; while one runs the shield refuses those requests with `LPWAN_BUSY` (the connect would
  set mode 1 and then 30, which ends the fix), and the nRF5340 client keeps an LTE connect and a release back until the result. `gnss_cancel` is
  also allowed during an update. LoRa is not touched, but a `sleep` request is refused for any radio.
  A fix and LTE evaluation or sends run side by side. Priority mode (`nrf_modem_gnss_prio_mode_enable()`, GNSS over LTE idle procedures, for
  repeated `NOT_ENOUGH_WINDOW_TIME`) is not used: a follow-up if fixes end "blocked" in the field.
- **Antenna.** The active antenna is supplied through the modem's COEX0 pin (net `EN_3V3_GNSS_ANT`, load switch U5, see HARDWARE.md). The
  shield selects the NCS library `modem_antenna` (`CONFIG_MODEM_ANTENNA`), which sends `AT%XCOEX0=1,1,1565,1586` at every start of the modem
  library: COEX0 is high while GNSS is active in 1565 to 1586 MHz. No `AT%XMAGPIO` (the shield has no antenna tuner on those pins). **Untested on
  hardware:** the supply, whether this COEX0 setting is the one the load switch needs, and the LR1121 coexistence line (COEX2, `AT%XCOEX2`) is
  not set.
- **Not done.** No A-GNSS and no nRF Cloud (no account): the receiver starts from what the modem keeps (ephemeris, almanac, last position). Assistance
  data over the node's own link is a follow-up, as is a charge-counter state for `gnss_on_ms` in the node.
- **Simulation.** The simulated shield has a fake `nrf_modem_gnss` and keeps the functional mode of the modem (sim/README.md): a PVT frame every
  second, a fix after a time, a timeout, blocked (and unblocked), or an error; a fix is refused when GNSS is not active.

## LoRaWAN (`src/lora.c`)

The LR1121 runs the Semtech **Modem-E** firmware (LoRaWAN stack inside the LR1121). The nRF9151 uses
the vendored host driver (`third_party/lr1121_modem/`) over SPI1 with a devicetree-based HAL
(`src/lr1121_zephyr_hal.c`). EU868, class A, DR / ADR / confirmed / port from the client configuration. States
as in the commissioning project: idle, joining, joined, ready, sending; the class and ADR are set
after the join, uplink waits for `ready`. `tx done` events carry the uplink `seq`. Downlinks are
events. Network time uses `DeviceTimeReq` (an empty uplink) and the LoRaWAN MAC time event.

Before an uplink the shield asks the LR1121 how much fits (`get_next_tx_max_payload`: data rate and pending MAC commands;
0 counts as not known) and refuses a chunk that is too long (`LPWAN_INVALID` with what fits in
`SendInfo.next_max_payload`; the client reports `-EMSGSIZE` if the size to chunk by is smaller). If less than 4 bytes
fit, MAC answers use the room: the shield sends an empty uplink (`request_empty_tx`, no port) to flush them and answers
`LPWAN_BUSY` with the time it takes. It asks how long the duty cycle holds the frame back (`get_duty_cycle_status`: 1 %
off time after a frame, 277 s after a full chunk at DR0, 67 s at DR3) and how often the frame is sent
(`get_nb_trans`, plus one if it is confirmed; each repeat waits for the duty cycle again) and answers the send with the
expected time to `tx_done` (`SendInfo.expected_ms`, at most 10 min). If the wait alone would push a send beyond 10 min,
the send is refused before it is queued: `LPWAN_BUSY` with the time after which it fits. If a call to the LR1121 fails
or gives nonsense the estimate is conservative. The watchdog and the client's deadline follow the estimate. A frame
cannot be taken back from the LR1121: a send that comes while one is queued gets `LPWAN_BUSY` with `queued_seq` (the
send whose frame it is) and the time left. `tx_done` carries `next_max_payload` (what the next uplink takes, MAC answers
counted) for the client's chunk size. The decisions are in `lib/sensei_lpwan/src/lpwan_lora.c` (host tested).

`lte_disconnect` during a join queues a `leave_network` after the join, so a late `joined` does not connect a radio the
master gave up. A self-reset of the LR1121 gives `tx_done(failed)` and
`disconnected`, and the radio setup runs again at the next join. The event line (DIO9) is read again after every command
until it is low, so an edge that was missed does not leave the backend deaf.

The LR1121 stays in reset until LoRa is first used. After that the Modem-E firmware sleeps by
itself. It keeps the LoRaWAN session (keys and counters) in its own flash. After the nRF5340 powers the
shield down and up again, `join` reports `joined` at once if the session is still valid. So the shield
can be powered down between uplinks without a new join.

**Modem-E firmware update:** the nRF9151 app does not have to flash the LR1121. The Modem-E image
(2.0.2) is programmed once in production (Semtech tool or a test bench). If the LR1121 does not
answer as Modem-E, the service reports `LPWAN_FAILED` ("flash the Modem-E firmware"). The vendored
bootloader driver (`lr1121_bootloader.*`) is included in the folder for a later update over the air
but not compiled.

### Provisioning the LoRaWAN keys

Keys are never in the repository or the firmware image. At run time:

```
sensei lpwan provision <DevEUI 16 hex> <JoinEUI 16 hex> <AppKey 32 hex>
```

or `lpwan_lora_provision()` from the host link. The nRF5340 powers the shield, sends `LoraProvision`,
joins, and clears the keys from its RAM. The nRF9151 writes them to the LR1121 at the join and wipes its copy at once
(also if writing fails: provision again). The frame buffers that held the request are cleared too. The shell command
leaves the keys in the shell history and in the log of the transport: bench use only. The LR1121 stores them. If the keys are missing in the LR1121 at a join, the service reports
`LPWAN_NOT_CONFIGURED`.

## Configuration the client sends

The nRF5340 client (`lib/sensei_lpwan`, `struct sensei_lpwan_config`) sends this at each connect:

| Field | Use |
|---|---|
| `lte_server`, `lte_port` | UDP server |
| `lte_psm_tau_s`, `lte_psm_active_s` | PSM request (0 TAU: PSM off) |
| `lte_edrx` | eDRX on, cycle and window from the client Kconfig (`SENSEI_LPWAN_EDRX_*`) |
| `lora_dr`, `lora_adr`, `lora_port`, `lora_confirmed` | LoRaWAN |

The RAI mode (`lte_rai`) and the flag of the last chunk travel with each send (`Send.rai`), not with this configuration.

The radio (LoRa, LTE-M or NB-IoT) is chosen by the app per send.

## Modem trace (Debug only)

`conf/debug.conf` switches the modem trace on and sends it to RTT (`CONFIG_NRF_MODEM_LIB_TRACE`, backend
`NRF_MODEM_LIB_TRACE_BACKEND_RTT`): the second RTT up channel `modem_trace` next to the log on channel 0. Read it with Nordic's
Trace Collector or `nrfutil trace` and open it with the Wireshark dissector of the modem trace; it shows RRC state changes and whether
the RAI went to the network. It takes a 16 kB region in the modem's shared RAM (`cpucell_cpuapp_ipc_shm_trace` in `app.overlay`, the
unused part of the 40 kB; `lte.c` checks at build time that it follows the IPC regions, fits the modem RAM and is as large as
`NRF_MODEM_LIB_SHMEM_TRACE_SIZE`) and an RTT buffer of 1 kB. The Release build has no trace (`release.conf` does not set it). The
trace needs the debugger attached; without one the buffer skips (`NO_BLOCK_SKIP`).

## Update over the air

The nRF5340 sends the new app, or a modem delta patch, over the SPI link (`dfu_*` requests of `lpwan.proto`, rules in
`lib/sensei_lpwan/include/sensei_lpwan/lpwan_dfu.h`). The user guide for the SENSEI IEQ node is in its
`firmware/docs/UPDATE.md`, section "LPWAN shield".

- **Boot.** MCUboot with swap using move and revert, ECDSA P-256 signature, software downgrade check (the same version is
  allowed). `NRF/apps/lpwan_shield/VERSION` is the image version and `LpwanVersion.firmware`: raise it for each update.
  NCS 3.4.1 needs no extra devicetree: the Zephyr default partitions of the nRF91 with TF-M fit (`nrf91xx_partition.dtsi`:
  MCUboot 64 kB, slot 0 and slot 1 of 448 kB each with TF-M 256 kB and the app 192 kB inside, TF-M storage, and `storage`
  32 kB for the app, non-secure with `TFM_NRF_NS_STORAGE`). The signed file `lpwan_shield.signed.bin` (also in
  `dfu_application.zip`) is TF-M and app in one image; the gap between them is 0xFF.
- **App update.** `src/dfu.c` writes slot 1 (`flash_area`), erasing each sector when the transfer reaches it, keeps a checkpoint per
  sector in the first sector of `storage` (a restart of the shield resumes at most one sector back), reads slot 1 back at
  `dfu_finish` for the SHA-256 and checks the MCUboot header (image, version) and the kind TLV, and at `dfu_apply` erases the
  trailer of slot 1 and requests one test boot (`boot_request_upgrade`); `dfu_abort` erases the trailer again. The image gets
  the slot less three sectors (trailer and the free sector of the swap with move). The nRF5340 then cuts the shield power and
  powers it again (the shield cannot reset itself reliably in a way the nRF5340 sees, and the nRF5340 owns the power).
  A repeated `finish`, `apply` or `begin` of the open transfer is answered with the state; a new transfer opens at most every 10 s
  (flash wear); the radios wait only while a sender was heard in the last `LPWAN_SHIELD_DFU_HOLD_S` (150 s) or the modem works.
- **Trial boot.** The new image answers `get_version` with `trial` set. Only when the nRF5340 accepted the handshake it sends
  `dfu_confirm` (with the image id: another id is refused; sent again until `trial` is false, harmless to repeat);
  `dfu_init()` then confirms the image (`boot_write_img_confirmed`) after `LPWAN_SHIELD_CONFIRM_DELAY_S` (5 s) with the
  rule of `NRF/system/ota/sensei_ota_trial.h`. Without `dfu_confirm`, or if the nRF5340 refuses the protocol version, the image
  reverts at the next reset or power cycle, and the shield resets by itself after `LPWAN_SHIELD_TRIAL_TIMEOUT_S` (180 s).
- **Modem.** `nrf_modem_delta_dfu` (target `DFU_TARGET_MODEM`): a work queue of its own (`LPWAN_SHIELD_MODEM_DFU_STACK`) starts the
  modem library if needed and erases the DFU area (state `ERASING` meanwhile), so the link thread keeps answering and feeding its
  watchdog. The patch goes in order, the SHA-256 is computed on the way (no resume after a restart of the shield), `dfu_apply`
  schedules the update and the same work queue restarts the modem library; `dfu_status` reports it. Success is
  `NRF_MODEM_DFU_RESULT_OK`, or, when the library reports nothing, a modem UUID that changed; `VOLTAGE_LOW` is a failure here (the
  modem tries again at its next start). `dfu_abort` closes the write session, also after a failed write. The nRF5340 keeps the
  session and the power until the state is DONE or FAILED. The modem checks the signature and UUID of the patch itself. Disable with `CONFIG_LPWAN_SHIELD_MODEM_DFU=n`, the whole update with
  `CONFIG_LPWAN_SHIELD_DFU=n` (then no MCUboot needed: `sysbuild.conf`).
- **During an update** the shield refuses connect, join and send (`LPWAN_BUSY`); `get_version`, `get_status`, `lte_disconnect`
  and the `dfu_*` requests work. The link thread runs the app update (stack 3 kB) and feeds its watchdog during erases.
- Everything is untested on hardware.

## Open points

- The MCUboot swap time of 448 kB (about 112 sectors moved and copied) and the modem erase time are not measured.
- No recovery over the link: a shield with a broken app is flashed over SWD (J12). The shield has no UART and no USB.
- Everything was built and tested on the host only. See the list of items to check on hardware in the
  report of the LPWAN work and `HARDWARE.md` (marked **verify**).

## Credits and licenses

- LPWAN shield commissioning project (ETH Zurich, 2026, Serge Wüest): the LR1121 and LTE
  procedures, radio setup values (RF switch, TCXO, TX offset), the transfer idea (BUSY, event line,
  global state machine). Files adapted from it name him in their header.
- Semtech LR1121 Modem-E host driver v1.1.0, `third_party/lr1121_modem/`: Clear BSD License
  (`LICENSES/BSD-3-Clause-Clear.txt`). Unchanged, except that the Semtech HAL and helper files were
  replaced by `src/lr1121_zephyr_hal.c`.
- Nordic NCS libraries (modem library, `lte_lc`, `date_time`) are used as libraries, no Nordic
  sample code is copied.
