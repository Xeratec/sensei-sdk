<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# nRF5340 to GAP9 link

The nRF5340 sends jobs. GAP9 runs them and sends results. GAP9 is powered only while jobs are pending.
Constants are in `include/sensei_gap9/gap9_link.h` (shared by both sides). The nRF5340 state machine is
`src/gap9_core.c` (host-tested in `tests/`). The GAP9 side is `gap9/gap9_side.c`. The job and result
messages are the app's: the link moves them as opaque payloads (`struct g9_codec`).

## Wires

| Signal | nRF5340 | GAP9 | Direction |
|---|---|---|---|
| SCK, MOSI, MISO, CS | SPIS3: P0.08, P0.09, P0.10, P0.11 (SPI_A) | SPI1: pads 33, 38, 39, CS pad 34 (a GPIO) | GAP9 is master |
| Trigger (net GAP_CTRL) | P1.05, output | pad 58, input | "GAP9: start a transaction now" |
| SPI select | P0.20 (SPI_GAP_SEL) | | high connects SPI_A to GAP9 |
| I2C select | P0.06 (I2C_GAP_SEL) | | high connects the camera bus to GAP9 |
| Power | P1.15 (`gap9_pwr()` of the SDK) | | |

The SPI is mode 3 (CPOL 1, CPHA 1), 8 bit, MSB first, 2 MHz (`SENSEI_SPI_HZ`). The wire is called `GAP9_nIRQ` in the SDK device tree (input of the
nRF5340). The link uses it the other way round, as an output. Both ends are plain GPIOs, no buffer between them.
Two more direct lines exist and stay unused: P1.04 `CS_nRF_GAP` (GAP9 pad 56) and P1.06 `EXT_RX_SPI` (pad 57).
SPI_B (P1.07 and following, GAP9 SPI2) is a different path and not used.

## Frames

Every frame is a `sensei_spi` frame: 8 byte header (`0x5E, version, type, flags, len u16, crc16 u16`; the CRC covers
header bytes 0 to 5 and the payload, link version 2) and a payload of `len` bytes (`lib/sensei_link/include/sensei_link/spi.h`).

| Type | Direction | Payload |
|---|---|---|
| `0x10` JOB | nRF5340 to GAP9 | the app's job message (at most `SENSEI_SPI_MAX_PAYLOAD`, 8192 B) |
| `0x11` RESULT | GAP9 to nRF5340 | the app's result message |
| `0x12` HELLO | GAP9 to nRF5340 | `Version` (firmware version, sent once after boot) |
| `0x13` POLL | GAP9 to nRF5340 | none (header only) |
| `0x14` NONE | nRF5340 to GAP9 | none: no job |
| `0x15` BUSFREE | GAP9 to nRF5340 | `seq` u32 little endian: the camera is done |
| `0x16` PEEK | GAP9 to nRF5340 | none (header only): like POLL, but nothing is taken |

Flags: `ERROR` (0x01) means "the last frame you sent me was bad". `MORE` (0x02) on a RESULT means "more
result frames of this job follow" (the node uses it for JPEG chunks).

## Transactions

The SPIS needs its buffers before the master starts. So the nRF5340 keeps one transaction armed all the time:
TX is the frame it wants GAP9 to fetch (a JOB, or NONE), RX is as long as the longest TX frame. GAP9 starts a
transaction only when the trigger line is high ("armed"). One chip select is one transaction. There are two kinds.

- **Push** (GAP9 sends): GAP9 clocks a frame on MOSI (HELLO, BUSFREE or RESULT). What comes back on MISO is
  ignored.
- **Poll** (GAP9 fetches): GAP9 clocks a POLL header on MOSI and reads the 8 byte header from MISO. If the
  header is a JOB it clocks `len` more bytes (a NONE has none). The nRF5340 counts the armed frame as fetched
  if GAP9 clocked all of it.
- **Peek** (GAP9 only looks): same, with a PEEK header, and GAP9 stops after the 8 bytes. GAP9 does this right
  after a push, to read the `ERROR` flag. The nRF5340 never takes the armed frame on a PEEK, so a NONE (8 bytes)
  is not taken by a peek.

The nRF5340 knows what happened from the type of the header and the number of bytes the SPIS received (the
master clocks the same number in both directions).

The nRF5340 SPIS reports the end of the transaction, drops the trigger line, checks the frame, and arms the next
one. Then it raises the line again. GAP9 waits 300 us after each chip select (so the nRF5340 can drop the line),
then waits for the line to go high (at most 1 s).

### Trigger line

High means armed, and GAP9 should poll or push. It is low when the nRF5340 is busy, or when it has told GAP9
that there is nothing to do. The nRF5340 drives it low whenever GAP9 is off.

### Sequence

1. A job is submitted. GAP9 is off: the nRF5340 sets the select lines, powers GAP9, arms a NONE frame and
   raises the trigger (boot timeout 4 s).
2. GAP9 boots, pushes HELLO. The nRF5340 checks the version (see below) and arms the first JOB (for a job that
   uses the camera bus it takes the bus lock first) and starts the fetch timer. GAP9 peeks (see "Acknowledge").
3. GAP9 polls and fetches the JOB. The nRF5340 marks it "in flight" and starts the job timer. The same JOB stays
   armed for a retry (a fetch that failed the CRC at GAP9). GAP9 must not run a job twice: the node's server
   ignores a `seq` it has run already.
4. GAP9 runs the job. A job that uses the camera bus pushes BUSFREE as soon as the camera is done. The nRF5340
   releases the bus lock.
5. GAP9 pushes the RESULT. The nRF5340 completes the job and arms the next JOB, or NONE.
6. GAP9 polls. With a JOB it goes to 3. With NONE the nRF5340 drops the trigger line and starts the idle timer
   (200 ms). GAP9 sleeps until the line rises.
7. A job arrives during the idle time: arm it, raise the line, stop the timer. Otherwise the timer expires: the
   nRF5340 powers GAP9 off and parks the select lines.

### Acknowledge, CRC errors, retries

- GAP9 checks every frame it receives (CRC-16). It checks every frame it sends by peeking the next nRF5340
  header: the header carries `ERROR` if the nRF5340 rejected the last push (bad magic, version, length, CRC, type
  or an unexpected seq). GAP9 then sends the frame again. It tries 3 times.
- Any good frame from GAP9, a POLL or PEEK too, clears the nRF5340's `ERROR` flag. A header with `ERROR` is
  not a reason to skip its payload: GAP9 reads the payload of a fetched frame in any case.
- If a JOB fails the CRC at GAP9, GAP9 polls again with `ERROR` set in its POLL header. The nRF5340 does not
  consume the job and arms it again. 3 tries.
- If a push or poll never gets an armed trigger line within 1 s, GAP9 gives up that step and goes back to waiting
  for the line.
- A result with the wrong `seq` is dropped and counted as an error.
- A job that fails before the camera was used (bad job, detector not built) may answer with the RESULT at once,
  without BUSFREE: an error result also frees the bus.
- HELLO while the link is running means GAP9 restarted by itself. The job in flight runs again from the start
  (bus lock taken again, BUSFREE expected again), unless some of its result frames were delivered already: then
  it fails with `-ECONNRESET`. The restart counts as an error.
- The nRF5340 counts every error through the app's error hook.

### Version check

`Version.major` in HELLO is the job protocol version of the GAP9 firmware. The app gives the major it needs
(`g9_codec.gap9_major`). Another major (or a HELLO that does not decode) counts as an error: GAP9 is powered off and
the queued jobs fail with `-EPROTO`. Raise the major of the GAP9 firmware when the frames, `Gap9Job` or
`Gap9Result` change so that old firmware cannot follow.

### Timeouts and recovery (nRF5340)

| Timer | Default | On expiry |
|---|---|---|
| boot (power on to HELLO) | 4000 ms | power cycle once, then fail the queued jobs with `-ETIMEDOUT` |
| bus free (start of a camera job to BUSFREE) | 4000 ms | job fails with `-ETIMEDOUT`, bus lock released, power cycle |
| job (start or BUSFREE to result; after each JPEG frame) | 8000 ms | job fails with `-ETIMEDOUT`, power cycle |
| fetch (a job is armed in the running state, until GAP9 takes it) | 8000 ms (job timer) | the armed job fails with `-ETIMEDOUT`, power cycle. Polls and peeks do not restart it |
| idle (NONE served) | 200 ms | power off |
| power cycle off time | 150 ms | boot again if jobs are pending |

After a power cycle the other queued jobs run on the freshly booted GAP9. The bus lock is always released after
GAP9 is off, never before (the camera could still use the bus). `gap9_link_power_off_now()` powers off and fails the
queued and staged jobs with `-ECANCELED`. `gap9_link_reset()` (the user button) power cycles GAP9, fails the job in
flight with `-ECANCELED` and runs the waiting jobs on the new boot.

## SPIS3 while GAP9 is off

The SPIS3 transfer stays armed when GAP9 is powered off, and is used again on the next boot: the Zephyr SPIS
driver cannot abort a transfer, and disabling the peripheral under it would leave the driver state wrong. Only
the pins are parked (sleep state: input buffers off) and the select lines are low. The LPWAN client's SPIM4 sets
its own pin states on the same pads. Untested on hardware: the idle current with GAP9 off, and MISO (P0.10)
during an LPWAN exchange, are in the node's `docs/TESTING.md` (T7.14).

## Sizes

- nRF5340 TX buffer 8200 B (header and the largest job), RX buffer 8200 B, `SENSEI_GAP9_STAGE_DEPTH` staging slots and
  `SENSEI_GAP9_QUEUE_DEPTH` queue entries of the app's job struct.
- A JOB frame at 2 MHz: 7192 B take 29 ms.

The first part of this file is the SDK's; the job formats are the app's (SENSEI IEQ node: `firmware/gap9/PROTOCOL.md`).
