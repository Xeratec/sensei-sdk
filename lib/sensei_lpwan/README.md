# sensei_lpwan

Client library for the SENSEI LPWAN shield: LTE-M, NB-IoT and LoRaWAN (LR1121 with Semtech Modem-E firmware) over the
shield's nRF9151. The nRF9151 side is the app [`NRF/apps/lpwan_shield`](../../NRF/apps/lpwan_shield/README.md), the
pin map is in its [HARDWARE.md](../../NRF/apps/lpwan_shield/HARDWARE.md). The messages are
`../sensei_link/proto/sensei/lpwan.proto`.

```
nRF5340 (master)                          nRF9151 (slave)
  client (this lib) -- request frame --->  link_slave (app) --> LTE / LoRa
                    <- response frame ---
                    <- event frames -----
```

| Folder | What |
|---|---|
| `include/sensei_lpwan/lpwan_link.h` | transfer scheme, flags, shared state machine API |
| `src/` | protocol cores without OS: `link_slave.c` (nRF9151), `lpwan_master.c` (nRF5340), `lpwan_sm.c` (both), `lpwan_timers.c` (3GPP PSM and eDRX timer coding), `lpwan_lora.c` (LoRa payload limit from the data rate) |
| `zephyr/` | nRF5340 client: `lpwan_client.c` (thread, shield power, shared SPI_A bus, radios), `lpwan_shell.c` |
| `tests/` | host tests: master against slave through a simulated SPI with faults, state machine, timer coding, LoRa limits |

## Client API (nRF5340, `sensei_lpwan/lpwan.h`)

- `sensei_lpwan_init(hooks, config)`: starts the `lpwan` thread. The shield stays off until a radio is needed.
- `struct sensei_lpwan_config`: LTE server and port, PSM, eDRX, LoRa data rate, ADR, port, confirmed. Change it with
  `sensei_lpwan_set_config()`. It takes effect at the next connect or send. APN, eDRX cycle and window are Kconfig options.
- `struct sensei_lpwan_hooks`: `on_up` (joined or connected), `on_downlink`, `on_time`, `on_error`, and the work queue that
  runs them.
- `sensei_lpwan_radio_ready()` and `sensei_lpwan_radio_send()`: what an app registers as its uplink radios.
  MTU: LTE `SENSEI_LPWAN_MTU_LTE` 1024 B; LoRa `sensei_lpwan_lora_mtu()`, from the data rate (EU868: DR0 to DR2 51 B, DR3
  115 B, DR4 and up 222 B, no MAC commands in FOpts). With ADR the network may lower the rate: 51 B. The app chunks by
  this value and reads it again after a configuration change; `radio_send` refuses a longer LoRa chunk (`-EINVAL`). The
  nRF9151 also asks the LR1121 what fits the next uplink and fails a chunk that does not.
- `sensei_lpwan_lora_provision()`: LoRaWAN keys at run time. They are sent to the shield and dropped from RAM (after
  three failed tries too: provision again). The shell command puts the keys in the shell history: bench use only.

Power: with LoRa the shield is switched off `SENSEI_LPWAN_IDLE_S` after the last traffic (the LR1121 keeps the LoRaWAN
session). With LTE it stays on and the modem sleeps in PSM.

SPI_A (P0.08 to P0.11) is shared with GAP9. The client takes it for each exchange through `sensei_busarb` (internal, not
part of the API); it also claims the bus during LTE registration and transmission, because GAP9 must not run then. The
GAP9 chip select also reaches the nRF9151 through the level shifter, so the client switches the shifter off (expander P7)
while GAP9 holds the bus and on again after it. Untested on hardware.

## Reliability

- **Frames:** CRC-16 over the header and the message. The slave keeps every message armed until the master confirms it:
  `LPWAN_FLAG_ACK` with the toggle bit of that message in `LPWAN_FLAG_ACK_TOGGLE`. A late or repeated ACK does not
  release the next message. The master drops repeats by the toggle bit; the first message after a start is always taken.
  The master keeps an ACK until a frame that carries it has gone out, and does not confirm a message it could not decode.
- **Shield restart:** the client resets the master's link state when it powers the shield (`lpwan_master_reset`). The
  nRF9151 forgets everything when it restarts: the client then sees `NOT_CONNECTED` for a request its own state allowed,
  starts again from idle and connects again.
- **Lost link:** `SENSEI_LPWAN_LINK_FAILURES` requests (default 3) without an answer, or that many sends without a result,
  power-cycle the shield. A shield that does not answer at power-on is cut and tried again after 15 s. A request
  does not count while GAP9 owns the bus.
- **Stuck send:** the nRF9151 ends a send without result after `LPWAN_SHIELD_SEND_TIMEOUT_S` (45 s) with `tx_done(failed)`;
  the client after `SENSEI_LPWAN_TX_TIMEOUT_S` (60 s). Both leave SENDING. Only the `tx_done` of the send in flight
  counts (it carries the request's seq).
- **Events:** state events (joined, connected, disconnected, tx_done, error) have two reserved queue slots, downlinks and
  time cannot take them. The state machine moves only if the event was queued. `LpwanStatus.dropped_events` counts what
  did not fit. A joined or connected event of another radio than the active one is ignored.
- **Versions:** SPI frame version `SENSEI_LINK_VERSION` (2 frames with another one in a row: `-EPROTONOSUPPORT`) and
  `LPWAN_PROTOCOL_VERSION` (in `LpwanVersion.protocol`, read at power-on). On a mismatch the client refuses the shield
  until the node reboots (`sensei lpwan status` says so, the self-test returns `-EPROTONOSUPPORT`). Bump the protocol
  version with every change of `lpwan.proto` or of the transfer rules.

## Use it

- Kconfig: `CONFIG_SENSEI_LPWAN=y` (selects `SENSEI_LPWAN_PROTO`, `SENSEI_LINK`, `SENSEI_BUSARB`), options
  `SENSEI_LPWAN_LORA`, `_LTE`, `_POWER_GATING`, `_IDLE_S`, `_LINK_FAILURES`, timeouts. The largest SPI frame is
  `CONFIG_SENSEI_SPI_MAX_PAYLOAD` of `sensei_link`. `SENSEI_LPWAN_SHELL` adds `sensei lpwan status | power |
  join | connect | disconnect | provision`; it needs an app shell root command `sensei`.
- The build needs the shield overlay `LPWAN_Shield_v1` (aliases `lpwan-link`, `lpwan-spi`): `-DSHIELD=LPWAN_Shield_v1`.
- The nRF9151 app uses `CONFIG_SENSEI_LPWAN_PROTO=y` only.
- Host tests: `cmake -S tests -B <builddir>`, or `make test-host` in the SDK root.

Untested on hardware. The SENSEI IEQ node shows the use (`firmware/nrf5340/src/lpwan`).

## Credits

Adapted from the LPWAN shield commissioning project (ETH Zurich, 2026, Serge Wüest): the LR1121 and LTE procedures, the
transfer idea (BUSY, event line, state machine). Files adapted from it name him in their header.
