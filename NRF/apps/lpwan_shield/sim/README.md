<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# Simulated shield (native_sim)

The shield app built for Zephyr's `native_sim/native/64`: a Linux process with the real service, state machines, LTE
and LoRa backends and link protocol. The hardware under them is replaced. For tests without hardware: the simulated
SENSEI node (`firmware/sim` of SENSEI IEQ) talks to it, and the whole radio path can run against a collector.

| Part | In the simulated shield |
|---|---|
| Link to the nRF5340 | `sim_link.c` instead of `src/link.c`: a TCP connection carries the SPI frames, BUSY and EVENT ([lpwan_sim.h](../../../../lib/sensei_lpwan/include/sensei_lpwan/lpwan_sim.h)). `link_slave.c` and the frame code are the code of the board. The shield listens on `-link_port` (127.0.0.1) |
| Power | The connection is the supply. When the node cuts it, the process executes itself again: RAM gone, as after a power cycle. A reset of the shield (modem fault, `shield reset`) does the same and breaks the link |
| LTE modem | `sim_modem.c`: fakes of the modem library, `lte_lc`, `date_time` and the AT commands that `lte.c` uses, with the signatures of the NCS headers (they are the real headers). Registration after 2 s, PSM, eDRX and RAI reports, connection evaluation, network time from the host clock. The RAI socket option is counted and logged, not sent |
| LTE data | The UDP socket of `lte.c` is an offloaded host socket (`CONFIG_NET_NATIVE_OFFLOADED_SOCKETS`): real datagrams reach the server the node configured, and its answers come back |
| GNSS | `sim_gnss.c`: a fake of the `nrf_modem_gnss` calls of `gnss.c` (and `lte_lc_func_mode_set` in `sim_modem.c`). A scenario says what the receiver does after the start: a valid fix after a time (position, satellites, UTC time from the host clock), no fix until the fix retry time ends the attempt, blocked by LTE from the start, or a failing start. Set with the command `gnss` or the option `-gnss="..."` |
| LoRa | `sim_lr1121.c`: a fake of the driver calls that `lora.c` makes (the `lr1121_modem_*` functions, with the driver's headers), not of the SPI commands. EVENT is an emulated GPIO pin. Join, uplinks with the 1 % duty cycle of EU868 (the model of `lib/sensei_lpwan/tests/test_lpwan_lora.c`), NbTrans, repeats of a confirmed uplink, receive windows, downlinks, reset. The session is kept in a file (`-lr_state`) like the radio keeps it in its flash |
| LoRaWAN network | The harness: the fake radio sends joins and uplinks over the control connection (`-ctl_port`) and takes the answer from it. `sensei_host.sim_lorans` of SENSEI IEQ is a network server that does this and talks TTN's MQTT |
| Not simulated | SPI timing and the level shifter, BUSY timing of the real SPIS, the SPI commands and CRCs of the LR1121 driver, the modem, RRC, radio timing, MCUboot and the update (`LPWAN_SHIELD_DFU` is off), the watchdog, retained RAM (the boot counter starts at 1 after every restart) |

## Build and run

NCS v3.4.1 workspace as for the other apps (see `../README.md`); on macOS in Docker, see `firmware/sim/build.sh` of SENSEI
IEQ, which builds both simulators:

```sh
west build -b native_sim/native/64 NRF/apps/lpwan_shield --no-sysbuild -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF
```

The board files `boards/native_sim_native_64.{conf,overlay}` are picked up by the board name. The overlay replaces
`app.overlay` (watchdog of the nRF9151). `LPWAN_SHIELD_SIM` (Kconfig) switches the fakes on and the modem library,
`lte_lc` and the SPI driver off.

```sh
zephyr.exe -link_port=7101 -ctl_port=7102 -lr_state=shield.state -rt -rt-ratio=20
```

| Option | |
|---|---|
| `-link_port=N` | the node connects here (127.0.0.1 only: the link is trusted) |
| `-ctl_port=N` | control connection of the harness; `-bind=ADDR` is its address (default 127.0.0.1). The port has no login: whoever reaches it can reset the shield, inject downlinks and load keys. Keep it on 127.0.0.1. `-bind=0.0.0.0` is for the container of the node simulator only, whose port is published on 127.0.0.1 of the host (`firmware/sim/README.md` of SENSEI IEQ says why that is acceptable) |
| `-lr_state=FILE` | what the fake LR1121 keeps in its flash (keys, session, counter) |
| `-gnss="<words>"` | scenario of the fake GNSS, the words of the `gnss` command below, e.g. `-gnss="fix 5 473764000 85476000 410 5 8"`. It applies at every start of the process, so it survives the power cycles of the shield; a control command does not |
| `-rt -rt-ratio=R` | simulated seconds per real second (as for every native_sim); `-no-rt` as fast as possible. The node must run at the same ratio |

The node simulator is started with `-lpwan_shield=127.0.0.1:<link_port>`. `firmware/sim/run_lpwan.sh` of SENSEI IEQ starts
both.

## Control connection

Text lines, `\n` at the end. The harness connects to `-ctl_port`; the shield sends `hello <uptime_s>` when it does, so a
restarted shield is seen. One harness at a time (a new one replaces the old).

Shield to harness:

| Line | |
|---|---|
| `join <DevEUI> <JoinEUI> <DevNonce> <MIC> <region>` | a join request (hex). The MIC is the one of a LoRaWAN 1.0.x join request, made with the AppKey the radio holds (`lpwan_sim_join_mic()`); the key itself is not sent. The network server checks it against its own key for the DevEUI, as a real one does, so a wrong AppKey fails the join |
| `up <fcnt> <port> <confirmed> <dr> <t_ms> <payload hex or ->` | the first transmission of an uplink; `t_ms` is the uptime of the shield |
| `lte_rai <option>` | the RAI socket option that `lte.c` set (1 no data, 2 last, 3 one response) |
| `restart <why>` | the process restarts |

Harness to shield:

| Line | |
|---|---|
| `lr join accept` or `reject` | the answer to a join request. No answer for 20 s counts as a rejection |
| `lr rx <ack 0/1> <fport> <payload hex or ->` | the receive windows of that uplink: acknowledged, and the downlink if any. The radio waits for it before TX_DONE (20 s at most) |
| `lr down <fport> <hex>` | a downlink now, whatever the radio does |
| `lr keys <DevEUI> <JoinEUI> <AppKey>` | what the radio holds from the factory: a shortcut for tests that do not provision the keys through the node. The key travels in clear on this connection, so use it on 127.0.0.1 only. The e2e test of SENSEI IEQ gives the keys to the node with `lora_provision` instead |
| `lr reset` | the LR1121 resets by itself: its volatile state and the frame in flight are gone, the session stays |
| `lr maxpayload N` / `nbtrans N` / `dr N` / `slow PCT` / `hang 0/1` | the payload the next uplink takes (MAC answers owed, 0: by data rate), NbTrans, data rate, air time stretched, TX_DONE never comes |
| `modem cover 0/1` | the network is lost or back (registration events) |
| `modem rrc 0/1` | RRC state change (idle, connected): the time in the connected state goes into the status counters. Attaches follow the registration; the modem sleep events (PSM entries) do not exist in the sim |
| `modem rai 0/1` / `modem energy N` | the cell supports RAI or not; the energy class of the connection evaluation (5 excessive, 7 normal) |
| `gnss fix [delay_s [lat_e7 lon_e7 alt_m acc_m sats]]` | the next fix attempts end with a valid fix after `delay_s` (default 3 s; default place: 47.3764 N, 8.5476 E, 410 m, 5 m, 8 satellites) |
| `gnss timeout` / `gnss blocked` / `gnss error` | no fix until the retry time ends the attempt (`timeout`); GNSS blocked by LTE and never unblocked, so the result is `blocked`; the start of the receiver fails (`failed`) |
| `modem fault` | the modem core crashes: `nrf_modem_fault_handler` records it and resets the chip |
| `shield reset` | the shield restarts |

## Files

| File | Content |
|---|---|
| `sim_shield.c`, `sim_shield.h` | command line, control connection, restart, hex helpers |
| `sim_link.c` | the link (`link.h` interface) over the socket |
| `sim_modem.c` | modem library, `lte_lc`, `date_time`, AT commands |
| `sim_lr1121.c` | the LR1121 with Modem-E firmware |
| `sim_gnss.c` | the GNSS of the modem |
| `../boards/native_sim_native_64.{conf,overlay}` | board files |

The seams in the production code are small: `CMakeLists.txt` and `Kconfig` pick the files, `main.c` has no watchdog and starts
the control connection, `lte.c` hands the RAI option to `sim_modem_set_rai()` (a host socket has no such option) and
logs `send answered`.
