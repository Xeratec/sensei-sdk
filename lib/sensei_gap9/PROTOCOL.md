<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-ND-4.0

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

Every frame is a `sensei_spi` frame: 8 byte header (`0x5E, version, type, flags, len u16, crc16 u16`) and a
payload of `len` bytes (`lib/sensei_link/include/sensei_link/spi.h`).

| Type | Direction | Payload |
|---|---|---|
| `0x10` JOB | nRF5340 to GAP9 | the app's job message (at most `SENSEI_SPI_MAX_PAYLOAD`, 8192 B) |
| `0x11` RESULT | GAP9 to nRF5340 | the app's result message |
| `0x12` HELLO | GAP9 to nRF5340 | `Version` (firmware version, sent once after boot) |
| `0x13` POLL | GAP9 to nRF5340 | none (header only) |
| `0x14` NONE | nRF5340 to GAP9 | none: no job |
| `0x15` BUSFREE | GAP9 to nRF5340 | `seq` u32 little endian: the camera is done |

Flags: `ERROR` (0x01) means "the last frame you sent me was bad". `MORE` (0x02) on a RESULT means "more
result frames of this job follow" (the node uses it for JPEG chunks).

## Transactions

The SPIS needs its buffers before the master starts. So the nRF5340 keeps one transaction armed all the time:
TX is the frame it wants GAP9 to fetch (a JOB, or NONE), RX is as long as the longest TX frame. GAP9 starts a
transaction only when the trigger line is high ("armed"). One chip select is one transaction. There are two kinds.

- **Push** (GAP9 sends): GAP9 clocks a frame on MOSI (HELLO, BUSFREE or RESULT). What comes back on MISO is
  ignored.
- **Poll** (GAP9 fetches): GAP9 clocks a POLL header on MOSI and reads the 8 byte header from MISO. If the
  header is a JOB (or NONE with a payload length) it clocks `len` more bytes. If GAP9 stops after 8 bytes, it is a
  "peek": nothing is consumed.

The nRF5340 knows what happened from the number of bytes the SPIS received (the master clocks the same number
in both directions). A frame it armed counts as fetched only if GAP9 clocked all of it.

The nRF5340 SPIS reports the end of the transaction, drops the trigger line, checks the frame, and arms the next
one. Then it raises the line again. GAP9 waits 300 us after each chip select (so the nRF5340 can drop the line),
then waits for the line to go high (at most 1 s).

### Trigger line

High means armed, and GAP9 should poll or push. It is low when the nRF5340 is busy, or when it has told GAP9
that there is nothing to do. The nRF5340 drives it low whenever GAP9 is off.

### Sequence

1. A job is submitted. GAP9 is off: the nRF5340 sets the select lines, powers GAP9, arms a NONE frame and
   raises the trigger (boot timeout 4 s).
2. GAP9 boots, pushes HELLO. The nRF5340 checks it and arms the first JOB (for a job that uses the camera
   bus it takes the bus lock first). GAP9 peeks (see "Acknowledge").
3. GAP9 polls and fetches the JOB. The nRF5340 marks it "in flight" and starts the job timer. The same JOB stays
   armed for a retry.
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
- If a JOB fails the CRC at GAP9, GAP9 polls again with `ERROR` set in its POLL header. The nRF5340 does not
  consume the job and arms it again. 3 tries.
- If a push or poll never gets an armed trigger line within 1 s, GAP9 gives up that step and goes back to waiting
  for the line.
- A result with the wrong `seq` is dropped and counted as an error.
- The nRF5340 counts every error through the app's error hook.

### Timeouts and recovery (nRF5340)

| Timer | Default | On expiry |
|---|---|---|
| boot (power on to HELLO) | 4000 ms | power cycle once, then fail the queued jobs with `-ETIMEDOUT` |
| bus free (start of a camera job to BUSFREE) | 4000 ms | job fails with `-ETIMEDOUT`, bus lock released, power cycle |
| job (start or BUSFREE to result; after each JPEG frame) | 8000 ms | job fails with `-ETIMEDOUT`, power cycle |
| idle (NONE served) | 200 ms | power off |
| power cycle off time | 150 ms | boot again if jobs are pending |

After a power cycle the other queued jobs run on the freshly booted GAP9. Power off always releases the bus lock
and fails queued jobs with `-ECANCELED`.

## Sizes

- nRF5340 TX buffer 8200 B (header and the largest job), RX buffer 8200 B, one job staging slot and `SENSEI_GAP9_QUEUE_DEPTH` queue
  entries of the app's job struct.
- A JOB frame at 2 MHz: 7192 B take 29 ms.

The first part of this file is the SDK's; the job formats are the app's (SENSEI IEQ node: `firmware/gap9/PROTOCOL.md`).
