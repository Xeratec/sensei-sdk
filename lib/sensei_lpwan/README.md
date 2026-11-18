<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

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
| `src/` | protocol cores without OS: `link_slave.c` (nRF9151), `lpwan_master.c` (nRF5340), `lpwan_sm.c` (both), `lpwan_timers.c` (3GPP PSM and eDRX timer coding), `lpwan_lora.c` (LoRa payload limit, airtime, send time estimate, what a send gets before it is queued, the send tracking of both sides), `lpwan_dfu.c` (rules of the shield update: offsets, resume, SHA-256, checkpoints, MCUboot header and kind checks, repeats, an abort that undoes the marking, rate limit and link hold; flash and modem behind an ops table), `lpwan_upd.c` (what the client remembers about the update: when a swap may be running, the trial of a new image, the end of a session; pure), `lpwan_sim.c` (stream framing of the simulated shield link), `lpwan_gnss.c` (GNSS of the shield: timeout rule, admission of a fix next to LTE, which requests wait for a fix, how an attempt ended, UTC to Unix time and the conversions of the modem's values; pure), `lpwan_rai.c` (LTE uplink policy: the RAI flag of a chunk, the send sequence of RAI option, send and wait for the answer behind an ops table, the answer of the server, the connection evaluation classes and the decision when to ask; pure) |
| `zephyr/` | nRF5340 client: `lpwan_client.c` (thread, shield power, shared SPI_A bus, radios, the update session), `lpwan_shell.c`, `sim/` (simulated shield link, native_sim) |
| `tests/` | host tests: master against slave through a simulated SPI with faults, state machine, timer coding, LoRa limits and airtime, a model of the LR1121 (duty cycle, NbTrans, repeats) with the shield and the client around it, `test_lpwan_dfu` (a shield update on a RAM flash, with gaps, resume after a restart, bad images, repeats, abort, the modem path, and over the SPI link with damaged frames), `test_lpwan_dfu_fuzz` (random chunking, lost replies, restarts and power loss in the middle of a flash operation), `test_lpwan_upd` (swap wait, trial, session rules), `test_lpwan_rai` (RAI flag and sequence on a fake socket, ack or downlink as the answer, evaluation classes and decision), `test_lpwan_counters` (RRC connected time, attaches, PSM entries and how they travel in `LpwanStatus`), `test_lpwan_gnss` (the GNSS rules, and `gnss_fix`, `gnss_cancel`, `gnss_result`, `LpwanVersion.gnss` and the GNSS counters between the master and a model of the shield, also an older shield, and the event slot of the result); the `on_power` hook is checked on native_sim by `NRF/tests/lpwan_power` |

## Client API (nRF5340, `sensei_lpwan/lpwan.h`)

- `sensei_lpwan_init(hooks, config)`: starts the `lpwan` thread. The shield stays off until a radio is needed.
- `struct sensei_lpwan_config`: LTE server and port, PSM, eDRX, LoRa data rate, ADR, port, confirmed. Change it with
  `sensei_lpwan_set_config()`. It takes effect at the next connect or send. APN, eDRX cycle and window are Kconfig options.
- `struct sensei_lpwan_hooks`: `on_up` (joined or connected), `on_downlink`, `on_time`, `on_error`, `on_power` and the work queue that
  runs them (not `on_error` and `on_power`). `on_power(bool on)`: the shield's 3.3 V went on or off, once per real change,
  on the client thread, short and non-blocking (the charge counter of the node times the shield with it). A bring-up that
  fails calls on, then off after the boot timeout.
- `sensei_lpwan_radio_ready()` and `sensei_lpwan_radio_send()`: what an app registers as its uplink radios (in a
  `struct uplink_radio` of [`sensei_uplink`](../sensei_uplink/README.md), the scheduler that queues, chunks and retries).
  MTU: LTE `SENSEI_LPWAN_MTU_LTE` 1024 B; LoRa `sensei_lpwan_lora_mtu()`, from the data rate (EU868: DR0 to DR2 51 B, DR3
  115 B, DR4 and up 222 B, no MAC commands in FOpts). With ADR the network may lower the rate: 51 B. After every send the
  nRF9151 reports what the LR1121 takes in the next uplink (MAC answers it still owes use room): if that is less,
  `sensei_lpwan_lora_mtu()` returns it minus 15 B. The app reads it again for every message and after a configuration
  change and keeps that size for all chunks of the message. `radio_send` refuses a chunk above the nominal size
  (`-EMSGSIZE`: the data rate was lowered after the message was chunked). The nRF9151 refuses a chunk that does not
  fit the next uplink and says what fits: `done(-EMSGSIZE)` if the size to chunk by is smaller than the chunk,
  `sensei_lpwan_lora_mtu()` follows it, the app chunks the message again with the new size. If the LR1121 says 0, the
  size is not known and the chunk goes without a check. If MAC answers leave no room for any chunk, the nRF9151 sends an
  empty uplink to flush them and refuses the send with `done(-EAGAIN)`.
- `-EAGAIN` (from `done()` or `radio_send`): the LR1121 would hold the frame back for longer than a send may take (10
  min), so the nRF9151 refused it before queueing it. Do not count it as a failure and keep the message.
  `sensei_lpwan_lora_retry_after_ms()` says when it may be tried again; `radio_ready(LORA)` is false until then and
  `on_up` comes when it is over.
- `-ENOTCONN` (from `done()`): the shield restarted or lost its connection. The client connects again and `on_up` follows. Keep
  the message, count these per message without progress, give up after a few. For LTE start the message again from its first
  chunk (the new connection is a new socket); for LoRa the chunks sent before are kept by the collector for a while.
- `sensei_lpwan_lora_provision()`: LoRaWAN keys at run time. It connects LoRa now (it releases another active radio first).
  The keys are sent to the shield and dropped from RAM (after three failed tries too: provision again).
  `sensei_lpwan_lora_keys_queue()` only keeps them for the next LoRa connect, without touching the connection: for a node
  whose radio is not LoRa. The client has no host test for this (it needs Zephyr); untested on hardware. The shell command puts the keys in the shell history: bench use only.

- `sensei_lpwan_radio_send_ex(radio, chunk, len, last, done, user)`: like `radio_send` and says if the chunk is the last of its
  message. See "LTE: RAI, the answer of the server" below.
- `sensei_lpwan_send_attempts()`: inside `done()`, `TxDone.attempts` of the send: how often the shield transmitted the chunk (LoRa:
  NbTrans, one more for a confirmed uplink without ack, 0 for an unconfirmed uplink with TX_DONE status NOT_SENT, `lpwan_lora_tx_done_attempts()`; the LR1121 does not count its frames, so this
  is the model of the time estimate, a lower bound for an acknowledged uplink; LTE: the datagrams). 0: nothing was sent, or the send
  ended without a result (timeout, lost link), or an older shield. A charge counter counts this per attempt, not once per `send()`.
- `sensei_lpwan_send_unconfirmed()`: inside `done()`, true if the chunk went out but the answer of the server did not come.
- `sensei_lpwan_lte_no_answer()`: how many sends the shield sent, waited for the answer and got none, since boot. It counts sends
  (a last chunk sent again counts twice); a failed send and a shield that did not wait are not counted. The SENSEI IEQ node
  reports its own count per message as `Status.lte_no_answer`.
- `sensei_lpwan_gnss_fix(timeout_s, done, user)`, `sensei_lpwan_gnss_cancel()`, `sensei_lpwan_gnss_busy()`, `sensei_lpwan_gnss_supported()`: one GNSS
  fix of the shield's nRF9151, see "GNSS" below.
- `sensei_lpwan_lte_link_check()` and `sensei_lpwan_lte_info()`: the connection evaluation (energy estimate, RSRP, coverage
  level) of the LTE modem. See "LTE: connection evaluation".
- `sensei_lpwan_shield_diag()`: reset cause and fault record of the shield, and the LTE counters (`rrc_connected_ms`, `attaches`,
  `psm_entries`, since the shield started; `counters_valid` is false for an older shield). They are as old as the last status
  read, which comes with every version read: refresh with `sensei_lpwan_query_version()`. See "Diagnostics of the shield".

Power: with LoRa the shield is switched off `SENSEI_LPWAN_IDLE_S` after the last traffic, never while a send is in
flight or its frame waits in the LR1121 (the LR1121 keeps the LoRaWAN session). With LTE it stays on and the modem sleeps in PSM.

## LTE: RAI, the answer of the server

Release assistance indication (RAI) tells the modem that the uplink is over, so that it leaves the RRC connected state at once
and not after the inactivity timer of the network (seconds at 70 to 130 mW, see the SENSEI IEQ `firmware/docs/ENERGY.md`).
Whether the network acts on it depends on the operator.

- The app sets `struct sensei_lpwan_config.lte_rai` (`sensei_LteRai`: `LTE_RAI_OFF`, `LTE_RAI_LAST`, `LTE_RAI_ONE_RESP`) and sends the
  chunks of a message with `sensei_lpwan_radio_send_ex(..., last, ...)`. Only the last chunk gets the flag (`Send.rai` of the
  link, `lpwan_rai_for_chunk()`); earlier chunks never do.
- The shield sets the socket option RAI for that send (`lpwan_rai_send()`, host tested on a fake socket). `LAST`: the option, the
  send. `ONE_RESP`: the option `RAI_ONE_RESP`, the send, a wait for the answer, then `RAI_NO_DATA`. The wait counts from `send()`,
  so it includes the RRC set-up of the modem: 10 s on LTE-M, 30 s on NB-IoT (`LPWAN_SHIELD_LTE_ANSWER_WAIT_S`,
  `_NBIOT_S`). It ends early when the coverage is lost. The option is also taken back when the wait ends without an answer.
- **The server answers an LTE uplink with exactly one datagram**: the first pending downlink, or the ack of the SENSEI collector,
  `msg_id | index | 0` (3 bytes, the form of a chunk with count 0, which no chunk has: `sensei_frag_ack()`). The shield
  consumes an ack of the chunk it sent (`lpwan_rx_decide()`, `lpwan_answer_classify()`), forwards a downlink as `DownlinkRx` and
  counts either as the answer. An ack of another chunk, or a late one after the wait, is dropped. The client drops an ack-shaped
  datagram too (a shield older than this rule would pass it on).
- **Acks are best effort.** The ack of a plain node has no MAC: a forged `msg_id | index | 0`, or any datagram that arrives while
  a send waits, counts as the answer. With E2E nodes the sealed messages are authenticated, the acks are not. Do not read an ack as
  proof of delivery against an attacker; it tells the node that the path works.
- With `ONE_RESP` a send without the answer is **sent, unconfirmed**, not failed: `TxDone.ok` true, `TxDone.unconfirmed` true,
  `done(0)` and `sensei_lpwan_send_unconfirmed()` true inside the callback. The datagram may have arrived (the ack was lost or the
  network was slow), so the app does not resend the chunk at once and does not count the message as delivered; the collector
  stores a repeated message once only within its duplicate window. `TxDone.acked` says the answer came; `sensei lpwan status` and
  `LpwanStatus.lte_no_answer` count unconfirmed sends (only when `tx_done.ok` and the shield waited). A server that never answers (an older collector, or another one) makes
  every last chunk unconfirmed: use `LTE_RAI_LAST` or `OFF` with it.
- The modem reports what the network supports (`LTE_LC_EVT_RAI_UPDATE`, `%RAI`): `LpwanStatus.rai_support` and the
  evaluation event. A modem or network without it ignores the option; the shield logs a warning.

## GNSS

The nRF9151 has a GNSS. `sensei_lpwan_gnss_fix()` powers the shield if it is off, asks for one fix (`gnss_fix`, `timeout_s` 0 is 120 s, at most
900) and calls `done(err, res, user)` on the hook work queue with the result event (`GnssFixResult`: status, position in degrees x 1e7,
altitude, accuracy, satellites, UTC time, time to fix). It works with LTE off (a LoRa-only node). The shield stays on until the result, then
the idle time applies. `err`: 0 (read `res->status`: `GNSS_FIX_OK`, `TIMEOUT`, `BLOCKED`, `CANCELLED`, `FAILED`), `-ENOTSUP` (the shield has no
GNSS: its version says so, or it answered `LPWAN_UNSUPPORTED`, which is what an older app does for a request it does not know; nothing else
breaks), `-EBUSY` (not started, try later: LTE connecting or sending, GAP9 on the bus, an update of the shield, a fix already pending),
`-ETIMEDOUT` (no result in the time plus 20 s, the client sent a cancel), `-ENOTCONN` (the shield lost its power or the link). The callback may
start the next fix.

The rule for a fix and LTE, in `sensei_lpwan/lpwan_gnss.h` (host tested), the same on both sides:

- The modem gives LTE priority and runs GNSS only while LTE is idle (PSM, eDRX gaps, LTE off). A send does not wait for a fix; a fix takes
  longer and may end "blocked".
- The changes of the modem's functional mode and the fix exclude each other. The shield refuses a fix (`LPWAN_BUSY`) while LTE connects,
  sends or disconnects, and refuses LTE connect, disconnect, configure, evaluation, deep sleep and LTE sends while a fix runs.
- The client keeps LTE out of the way from the call to the callback: `sensei_lpwan_radio_ready(LTE)` is false, an LTE send gives `-EAGAIN`, an
  LTE connect waits, and `on_up(RADIO_BLE)` comes after the callback. An LTE message waits at most the timeout of the fix. LoRa is not held.
- The shield is not powered down while a fix runs; a power cut or a lost link ends it (`-ENOTCONN`).

`sensei lpwan gnss [timeout_s]` (shell) asks for a fix and prints the result. `sensei_lpwan_shield_diag()` has the shield's GNSS counters
(`gnss_attempts`, `gnss_fixes`, `gnss_on_ms`). Untested on hardware.

## LTE: connection evaluation

`sensei_lpwan_lte_link_check()` is for the app's uplink scheduler before a message that can wait (not alerts, not status). The
shield runs `AT%CONEVAL` (`lte_lc_conn_eval_params_get()`: energy estimate, RSRP, RSRQ, coverage enhancement level, repetitions)
on request (`lte_eval`, the result is the `conn_eval` event) and the client keeps the result for `SENSEI_LPWAN_EVAL_VALID_S`
(600 s).

- `SENSEI_LPWAN_LINK_OK`: send. `_BAD`: the estimate is excessive (`lpwan_energy_is_bad()`; unknown and poor are not bad), the
  app may hold the data. `_WAIT`: the shield is evaluating, `on_up` follows when it knows.
- No result (not registered, radio busy, no event within 30 s): a failed evaluation. The send goes ahead (OK) and the shield is not
  asked again for `SENSEI_LPWAN_EVAL_RETRY_S` (300 s). An older shield that answers `LPWAN_UNSUPPORTED` is not asked again for an
  hour. `SENSEI_LPWAN_LTE_EVAL=n` makes the check always say OK. A lost `conn_eval` event therefore costs one ask per retry time, not
  one per check (`lpwan_eval_decide()`, host tested).
- `sensei_lpwan_lte_info()` gives the last result (RSRP with `rsrp_valid`, CE level, estimate, RAI of the cell, age) for the app's
  status. An invalid RSRP is not turned into 0 dBm: `rsrp_valid` is false.
- Untested on hardware: how the modem answers `%CONEVAL` while it sleeps in PSM.

## Diagnostics of the shield

The client reads the shield's status (`get_status`: reset cause, boot count, fault record) after each version read, after a restart
that the shield reports (a request answered `NOT_CONNECTED`) and, when the link is lost, once before it cuts the shield power:
`link_recover()` waits up to `SENSEI_LPWAN_BOOT_TIMEOUT_MS` for the shield's BUSY line and asks `get_status`, so the record of a
shield that faulted and reset itself is not lost with the power cut. The shield keeps the record in RAM and reports it with every
status until the client has stored it and sends `diag_ack` (an older shield refuses it, harmless). A reset or a power cut of the
shield before that loses it. `sensei_lpwan_shield_diag()` returns the last reading. Untested on hardware.

## Update of the shield (`sensei_lpwan_dfu_submit()`)

The app of the nRF5340 relays the image it gets from a host to the nRF9151 piece by piece and keeps none of it. The
requests are `dfu_begin`, `dfu_chunk`, `dfu_finish`, `dfu_apply`, `dfu_abort`, `dfu_status`, `dfu_confirm` (`lpwan.proto`,
tags 40 to 46); each answer is a `DfuStatus`. The rules and the flow are in `lpwan_dfu.h`, the node's description in
`firmware/docs/UPDATE.md` of SENSEI IEQ. What the client adds:

- `sensei_lpwan_dfu_submit(request, done, user)`: one request at a time, the answer on the hook work queue
  (`struct sensei_lpwan_dfu_result`: verdict, status, version). `-ENOTSUP` for a shield app without update support
  (`LpwanVersion.dfu_targets` is 0), `-EBUSY` while a send or connect runs, GAP9 holds the bus, or the last session was too long.
- `dfu_begin` opens a session that holds the link: `sensei_lpwan_radio_ready()` is false and `radio_send` gives `-EAGAIN` (the
  app waits for `on_up`, no failure is counted), the shield is not powered down, `BUS_DFU` keeps GAP9 off the bus, an active
  radio is released. It ends with `dfu_abort`, after a restart into the new app, when no transfer is open any more, after 120 s
  without a request, or 45 min after it began (no new transfer for 10 min then: a sender that renews it for ever must not hold
  the uplinks and GAP9). `on_up(RADIO_BLE)` tells the app that data may go again. The shield keeps its transfer, `dfu_begin`
  with `resume` continues it (also after a power cycle: checkpoints).
- Requests may take seconds on the shield (a flash erase): the client waits 8 s per SPI try. `sensei_lpwan_shield_version()`
  gives the version of the last handshake (the node's Status).
- `dfu_apply` of the app: the client cuts the shield power, waits 300 ms, switches it on and waits up to 180 s for the new
  image (MCUboot swaps first; the `BUS_DFU` lease is renewed meanwhile). If the answer is lost it asks `dfu_status` and goes on
  when the image is marked. The client owns the trial of the new image (`LpwanVersion.trial`): it accepts it when the protocol
  matches, the update support is kept and `dfu_status` answers, sends `dfu_confirm` with the image id, repeats it every 5 s
  and keeps the shield on until `trial` is false (at most 200 s; the image resets itself after 180 s). Otherwise MCUboot goes
  back to the old image at the next reset or power cycle.
- A swap may be running whenever the shield is switched on after the node started, after an apply, after a power cut during a
  trial, and after a bring-up that timed out: the client then waits up to 180 s for the shield and does not cut the power
  (`lpwan_upd.h`; a short wait of 3 s would cut it again and again in the middle of the 25 to 45 s swap).
- Modem: the client keeps the session and the power while the modem erases or updates, and asks `dfu_status` every 5 s
  itself, so the update ends cleanly also when the host is gone.

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
  starts again from idle and connects again. A send that meets a restarted shield ends with `-ENOTCONN` (not `-EAGAIN`: that is the duty cycle) and the client connects
  again (`on_up` follows): the message is not held until the next uplink time. The app counts these per message and gives up after a
  few; for LTE it starts the message again from chunk 0, because the new connection is a new socket.
- **Lost link:** `SENSEI_LPWAN_LINK_FAILURES` requests (default 3) without an answer, or that many sends without a result,
  power-cycle the shield. A shield that does not answer at power-on is cut and tried again after 15 s. A request
  does not count while GAP9 owns the bus.
- **Stuck send:** the nRF9151 ends a send without result with `tx_done(failed)`, the client later. Both leave SENDING.
  LoRa sends can wait for the duty cycle of the LR1121 (1 %: 277 s after a full chunk at DR0, 67 s at DR3), and the
  LR1121 repeats a frame NbTrans times (ADR), once more if it is confirmed, each after the duty cycle again. The
  nRF9151 asks the LR1121 (`get_duty_cycle_status`, `get_nb_trans`) and puts the expected time to `tx_done` in the answer
  to the send (`SendInfo.expected_ms`, at most 10 min); if a call fails or gives nonsense the estimate is conservative
  (DR0, confirmed, a wait as after a frame of the same size). The nRF9151 looks at the send 5 s after it, the client 20
  s after it; without an estimate (LTE) after `LPWAN_SHIELD_SEND_TIMEOUT_S` (45 s) and `SENSEI_LPWAN_TX_TIMEOUT_S` (60 s).
  - A frame cannot be taken back from the LR1121 and the power would drop it. So while the LR1121 still holds it the
    nRF9151 does not fail the send at that time, and the client asks the status (`queued`) at its deadline and waits
    once more. After twice the limit the nRF9151 resets the LR1121 (the frame is dropped), reports `tx_done(failed)`, an
    error and `disconnected`.
  - If the client lost track of a send (it timed out, or the answer was lost) and sends again, the shield answers BUSY
    with `SendInfo.queued_seq` and the time left. The same chunk takes over that frame (one transmission, its `tx_done`
    is the result); another chunk waits for it and goes after. A result that no open send waits for is kept in the
    status once (`late_tx_seq`, `late_tx_ok`): the client reads it at its deadline.
  - A send whose duty-cycle wait would take longer than 10 min is refused before queueing: BUSY with the time after
    which it fits (`SendInfo.expected_ms`, no `queued_seq`). The client honours it (`-EAGAIN`, see above).
  - The shield is never switched off while a frame may be held (until twice the limit has passed).
  Each send has one `tx_done`; a second one is kept for the status on the nRF9151. Only the `tx_done` of the send in
  flight counts (it carries the request's seq). The shield's watchdog acts only for the send it was armed for.
- **Events:** state events (joined, connected, disconnected, tx_done, error) have two reserved queue slots, downlinks and
  time cannot take them. The state machine moves only if the event was queued. `LpwanStatus.dropped_events` counts what
  did not fit. A joined or connected event of another radio than the active one is ignored.
- **Versions:** SPI frame version `SENSEI_LINK_VERSION` (2 frames with another one in a row: `-EPROTONOSUPPORT`) and
  `LPWAN_PROTOCOL_VERSION` (in `LpwanVersion.protocol`, read at power-on). On a mismatch the client refuses the shield
  for an hour (`sensei lpwan status` says so, the self-test returns `-EPROTONOSUPPORT`), then asks again from a fresh
  start of the shield: two damaged version bytes in a row look like another version, and the shield may have been updated.
  Bump the protocol version with every change of `lpwan.proto` or of the transfer rules that an older peer would misread
  (now 2; the update messages (tags 40 to 46, `LpwanVersion` 4 to 7), the GNSS additions (`gnss_fix` 50, `gnss_cancel` 51, the event `gnss_result` 50, `LpwanVersion.gnss` 8, `LpwanStatus` 16 to 18) and the LTE additions (`Send.rai`, `lte_eval`, `ConnEval`, `TxDone.acked`, `TxDone.unconfirmed`, `ConnEval.rsrp_valid`, `LpwanStatus` 20 to 22 and `rssi_valid`, `diag_ack`) are additions and did not bump it: an old shield app reports
  `dfu_targets` 0, an old client never sends `dfu_confirm`, so a new shield image is not kept; the earlier changes: `SendInfo`, `TxDone.next_max_payload`, `SendInfo.next_max_payload`). `SendInfo.queued_seq`, `LpwanStatus.queued`,
  `late_tx_seq` and `late_tx_ok` are new fields: an older peer ignores them and treats BUSY as a failure.

## Simulated shield link (native_sim)

`CONFIG_SENSEI_LPWAN_SIM` (native_sim only) replaces SPI, the I2C expander and the pins of the client by a TCP connection to the
simulated shield app (`NRF/apps/lpwan_shield/sim`): `zephyr.exe -lpwan_shield=<host>:<port>`. Powering the shield connects, cutting
the power closes the connection and the shield process restarts. The frames are those of the board; the stream format is in
`include/sensei_lpwan/lpwan_sim.h` (pure, host tested in `tests/test_lpwan_sim.c`). The client keeps the rest of its logic: bus
arbitration, recovery, timeouts. Code: `zephyr/sim/lpwan_sim_master.c` (client side), `zephyr/sim/lpwan_sim_host.c` (host sockets,
built into the native simulator runner). The few places in `lpwan_client.c` that touch the hardware have a second branch.

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
