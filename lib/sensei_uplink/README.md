<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# sensei_uplink

Uplink scheduling for an app with one or more radios: two queues, one chunk in flight, retries, deferral and the receive
side of a downlink. Plain C, no Zephyr, no locking: the app serialises the calls (the node: one work queue) and brings
the clock, a timer, the queue memory and the radios. One scheduler per program: the radios' `done()` callbacks find it through a
static pointer, so a second `us_init()` takes over from the first. It is its own library, not part of `sensei_lpwan`, because it does
not know the shield: any radio (BLE, LoRa, LTE-M, a test fake) fits behind `struct uplink_radio`. The radios of
`sensei_lpwan` plug into it.

| File | What |
|---|---|
| `include/sensei_uplink/uplink_radio.h` | `struct uplink_radio`: what the app tells about a radio (chunk size, timeouts, send, link check) |
| `include/sensei_uplink/uplink_sched.h`, `src/uplink_sched.c` | the scheduler `us_*`; `struct us_ops` (clock, timer, error, random, message end, prepare) |
| `include/sensei_uplink/msg_queue.h`, `src/msg_queue.c` | byte-bounded FIFO of messages with two flag bits (`mq_*`) |
| `include/sensei_uplink/dl_rx.h`, `src/dl_rx.c` | joins the chunks of a downlink (`dl_rx_*`) |
| `zephyr/` | Zephyr library behind `CONFIG_SENSEI_UPLINK` (Kconfig: buffer sizes) |
| `sensei_uplink.cmake` | source and include lists for projects that build the sources into their own target |
| `tests/` | host tests (ctest): `test_sched` (fake radios and clock), `test_queue`, `test_dl_rx` |

## Use

```c
static const struct us_ops ops = {.now_ms = ..., .schedule = ..., .error = ..., .rand = ..., .msg_end = ..., .prepare = ...};
us_init(&sched, queue_buf, sizeof(queue_buf), alert_buf, sizeof(alert_buf), &ops, ctx);
us_register(&sched, &my_radio);                     /* static storage, up to US_MAX_RADIOS */
us_set_config(&sched, sensei_Radio_RADIO_LORA, 2);  /* radio from the settings (else BLE), retries */
us_submit_flags(&sched, enc, len, alert, flags);    /* queue an encoded message */
us_start(&sched);                                   /* at the uplink time; also when a radio is up again */
/* timer: us_run(&sched). Radio completion: done(err, user) as the radio was given. */
```

- `now_ms()` is a free-running ms counter (wraps are handled). `schedule(delay)` asks for one `us_run()` after the delay and
  replaces an earlier request. `done()` may run inside `send()`.
- Alerts and status go to the alert queue (`alert` true) and are sent first. A full queue drops its oldest messages
  (`US_ERR_QUEUE_FULL`, `us_stats.msgs_evicted`); a radio that is down for days keeps the newest data.
- Each message is cut with `sensei_frag` to the chunk size of the radio, read once per message (`mtu_now`, else `mtu`). A
  failed chunk is retried `retries` times (2 s, 4 s, ... 60 s, plus up to 50 % jitter); after three failed sessions the
  message is dropped. A chunk not done after `chunk_timeout_ms` (default 90 s) counts as failed.
- `-EAGAIN` and `-ENOTCONN` from a radio defer the message. `-ENOTCONN` counts per message without progress (3 times,
  `US_MAX_CONN_DEFERS`, then a failed session). `-EMSGSIZE` chunks the message again if the radio's chunk size changed.
  `US_SENT_UNCONFIRMED` from `done()`: the last chunk is sent once more, then the message counts as sent.
- `send_last()` tells the radio which chunk ends the session (the last chunk of the last queued message), for the RAI flag
  of LTE. `link_check()` lets a radio hold data on a bad link up to `us_set_defer()` ms; alerts are never held.
- `MQ_FLAG_PLAIN` marks an unsealed message. With `us_ops.prepare` and `uplink_radio.prepare` the app can change it
  when it starts on that radio (the node adds `Uplink.node_id` on plain LTE). `prepare` returns the new length, at most
  `cap`, or 0 to drop the message. `MQ_FLAG_REPORT` and `MQ_FLAG_REKEY` (a message with a key mix request: the node asks again if it is dropped) call `us_ops.msg_end` when the message is sent or dropped.
- `us_node_offset_s()` and `us_next_tick_ms()`: the per-node offset in the uplink period and the delay to the next tick.
- The full rules are in the comment of `uplink_sched.h`; the node's view is in its `src/comm/README.md`.

## Collector limits

The scheduler and the radios must keep two times below what the collector expects:

- **`US_RESUME_MAX_MS` (300 s)** must stay below the collector's `LORA_REASSEMBLY_TIMEOUT` (700 s in the SENSEI collector,
  `tools/collector/sensei_collector/config.py` of the node repository). A deferred LoRa message goes on at its refused chunk
  only if the last chunk went out less than 300 s ago and `keeps_reasm` is set. Later, or on a radio without `keeps_reasm`
  (LTE: a new connection comes from a new source address), it starts again at chunk 0 with a new message id. If the collector
  timeout is shortened, lower `US_RESUME_MAX_MS` too.
- The collector answers a repeated last chunk again and stores the message once, so the retry after `US_SENT_UNCONFIRMED`
  loses nothing.

## Downlink receive

`dl_rx` joins the chunks (`msg_id | index | count | data`) that the collector or TTN sends, also for a message of one chunk.
The app gives the buffer for the largest message (the node: a `Downlink` at its generated maximum inside the largest
envelope, 337 B) and one receiver per radio. A 3-byte acknowledgement is dropped and counted (`n_acks`); a copy of a finished
message gives no message and is not counted; a bad chunk and a message over the buffer give no message and count in `n_bad`.
`mtu` of `dl_rx_init()` is the largest chunk of the sender. The sender may use smaller ones (LoRa: the chunk size follows the
data rate of the uplink, 51, 115 or 222 B): all chunks of a message but the last must have the size of the first one that came
(`sensei_link/frag.h`). The time without a chunk after which a message in progress is dropped and the time a finished message is
remembered are two values (`dl_rx_set_timeouts()`, changeable at run time): LoRa chunks come one uplink interval apart, a finished
message is remembered for a short time only (the 8 bit message id comes round).

## Configuration

Sizes of the buffers inside `struct uplink_sched` (Kconfig on Zephyr; on the host the same names as macros, set for the whole
target with `target_compile_definitions(sensei_uplink PUBLIC CONFIG_SENSEI_UPLINK_MAX_MSG=...)`). All code that includes the
header must see the same values.

| Kconfig | Default | What |
|---|---|---|
| `SENSEI_UPLINK_MAX_MSG` | 2144 | largest queued message (`US_MAX_MSG`); the node: a sealed `Uplink` |
| `SENSEI_UPLINK_PREPARE_ROOM` | 9 | room for what `prepare` adds (`US_PREPARE_ROOM`); the node id field |
| `SENSEI_UPLINK_MAX_MTU` | 1024 | largest chunk of any radio (`US_MAX_MTU`) |
| `SENSEI_UPLINK_MAX_RADIOS` | 4 | radios that can be registered (`US_MAX_RADIOS`) |

The queue sizes are the arguments of `us_init()`. RAM of `struct uplink_sched`: about `US_MAX_MSG + US_PREPARE_ROOM + US_MAX_MTU`
plus about 300 B on a 32-bit target.

## Build

- Zephyr: `CONFIG_SENSEI_UPLINK=y` (selects `SENSEI_LINK`).
- Host and others: `add_subdirectory(<sdk>/lib/sensei_uplink)` and link `sensei_uplink`, or take `SENSEI_UPLINK_SOURCES` and
  `SENSEI_UPLINK_INCLUDE_DIRS` from `sensei_uplink.cmake` when the target already has the `sensei_link` sources.
- Tests: `make test-host` of the SDK, or `cmake -S lib/sensei_uplink/tests -B <builddir>`.

## Not tested here

The fake radios stand in for the real ones: the LoRa duty cycle, LTE RAI and the shield are tested with `sensei_lpwan` and
the node (native_sim, the LPWAN e2e test); nothing runs on hardware from this library alone.
