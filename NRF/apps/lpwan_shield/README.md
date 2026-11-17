<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# LPWAN shield firmware (nRF9151)

Firmware for the nRF9151 on the SENSEI LPWAN shield. It is an SPI slave of the nRF5340 and gives
it LTE-M, NB-IoT and LoRaWAN (through the LR1121). Any SENSEI app on the nRF5340 can use it through
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
west build -b sensei_lpwan_nrf9151/nrf9151/ns -- $M -DEXTRA_CONF_FILE=conf/release.conf
```

The SENSEI IEQ node builds it with `make nrf9151` and `make nrf9151-release` (`firmware/Makefile`).

| Build | Flash (non-secure image) | RAM |
|---|---|---|
| Release | 91 kB | 51 kB |
| Debug (RTT log) | 141 kB | 59 kB |

The non-secure slot is 212 kB. Kconfig: `LPWAN_SHIELD_LTE`, `LPWAN_SHIELD_LORA` (both default y).
Turn one off to save flash.

## Flash

```sh
west flash -d <build dir>
```

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

Low power: the modem stays off until the first connect. After the uplink, the nRF5340 sends
`sleep(0)`: the nRF9151 stays registered and the modem sleeps in PSM; the CPU idles with the SPIS
armed. `sleep(1)` or `lte_disconnect` puts the modem offline.

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

The radio (LoRa, LTE-M or NB-IoT) is chosen by the app per send.

## Open points

- MCUboot is off. It builds with TF-M for the nRF9151 in NCS v3.4.1
  (`-DSB_CONFIG_BOOTLOADER_MCUBOOT=y`, merged image 64 kB bootloader plus TF-M and app), but has not
  been run on hardware. The devicetree partitions are the Zephyr default (mcuboot 64 kB, two image
  slots with secure and non-secure part, TF-M storage), so OTA can follow.
  The request set has room for a DFU transfer: `LpwanRequest` tags 13 and up are free (the node reserves 40 to 59 for OTA and GNSS).
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
