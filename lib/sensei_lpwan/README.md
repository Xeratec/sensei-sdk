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
| `src/` | protocol cores without OS: `link_slave.c` (nRF9151), `lpwan_master.c` (nRF5340), `lpwan_sm.c` (both), `lpwan_timers.c` (3GPP PSM and eDRX timer coding) |
| `zephyr/` | nRF5340 client: `lpwan_client.c` (thread, shield power, shared SPI_A bus, radios), `lpwan_shell.c` |
| `tests/` | host tests: master against slave through a simulated SPI with faults, timer coding |

## Client API (nRF5340, `sensei_lpwan/lpwan.h`)

- `sensei_lpwan_init(hooks, config)`: starts the `lpwan` thread. The shield stays off until a radio is needed.
- `struct sensei_lpwan_config`: LTE server and port, PSM, eDRX, LoRa data rate, ADR, port, confirmed. Change it with
  `sensei_lpwan_set_config()`. It takes effect at the next connect or send. APN, eDRX cycle and window are Kconfig options.
- `struct sensei_lpwan_hooks`: `on_up` (joined or connected), `on_downlink`, `on_time`, `on_error`, and the work queue that
  runs them.
- `sensei_lpwan_radio_ready()` and `sensei_lpwan_radio_send()`: what an app registers as its uplink radios
  (MTU `SENSEI_LPWAN_MTU_LORA` 222 B, `SENSEI_LPWAN_MTU_LTE` 1024 B).
- `sensei_lpwan_lora_provision()`: LoRaWAN keys at run time. They are sent to the shield and dropped from RAM.

Power: with LoRa the shield is switched off `SENSEI_LPWAN_IDLE_S` after the last traffic (the LR1121 keeps the LoRaWAN
session). With LTE it stays on and the modem sleeps in PSM.

SPI_A (P0.08 to P0.11) is shared with GAP9. The client takes it for each exchange through `sensei_busarb`; it also
claims the bus during LTE registration and transmission, because GAP9 must not run then.

## Use it

- Kconfig: `CONFIG_SENSEI_LPWAN=y` (selects `SENSEI_LPWAN_PROTO`, `SENSEI_LINK`, `SENSEI_BUSARB`), options
  `SENSEI_LPWAN_LORA`, `_LTE`, `_POWER_GATING`, `_IDLE_S`, timeouts. `SENSEI_LPWAN_SHELL` adds `sensei lpwan status | power |
  join | connect | disconnect | provision`; it needs an app shell root command `sensei`.
- The build needs the shield overlay `LPWAN_Shield_v1` (aliases `lpwan-link`, `lpwan-spi`): `-DSHIELD=LPWAN_Shield_v1`.
- The nRF9151 app uses `CONFIG_SENSEI_LPWAN_PROTO=y` only.
- Host tests: `cmake -S tests -B <builddir>`, or `make test-host` in the SDK root.

Untested on hardware. The SENSEI IEQ node shows the use (`firmware/nrf5340/src/lpwan`).

## Credits

Adapted from the LPWAN shield commissioning project (ETH Zurich, 2026, Serge Wüest): the LR1121 and LTE procedures, the
transfer idea (BUSY, event line, state machine). Files adapted from it name him in their header.
