<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# SENSEI SSBL

Second stage bootloader of the GAP9 on the SENSEI base board. It starts the firmware of slot A or slot B of the MRAM. The
nRF5340 picks the slot with the level of GAP9 pad 56 (net `CS_nRF_GAP`, nRF P1.04): low A, high B. The SSBL does not decide
whether a firmware is good. The nRF5340 does (trial boot, [`gap9_trial.h`](../../lib/sensei_gap9/include/sensei_gap9/gap9_trial.h)).

Forked from `utils/ssbl/src` of the GAP SDK 5.22.4 (GreenWaves Technologies, Apache-2.0): the load of the app binary into L2
and the launch are the same SDK calls. The partition and volume table, the boot counter and the GAP CLI recovery are gone.
With `SENSEI_SSBL_RECOVERY` the SSBL runs the update writer when no slot boots (section "Recovery"); without it, nothing of
that is built in.

## Boot

1. Read pad 56 with the internal pull-down, five samples, majority. A line that nobody drives selects A.
2. Try that slot, then the other one, except that the other one is skipped when its image was sealed and never started
   (the newest record of the checkpoint log is DONE for it, `g9ota_ckpt_sealed_waiting()`, read only): such a slot starts
   when the slot line names it, which the nRF5340 does when it applies the update, never as the fallback. A slot is tried
   as follows:
   - the MCUboot header, the TLV areas behind the image (a slot whose write stopped early has none) and the container
     ([`gap9_slot.h`](../../lib/sensei_gap9/include/sensei_gap9/gap9_slot.h), host tested);
   - the app descriptor (area count, no XIP, every section and LZ4 block inside the app, no section beyond the physical L2
     (0x1C190000), over the SSBL or over the hand-off record);
   - `pi_app_bin_load()` with the CRC32 of every section (gap-sdk skips that check without `CONFIG_DRIVER_CRC32`: the
     config files set it and `CMakeLists.txt` stops the build if it is missing).
3. Write the hand-off record: magic, slot, ~slot at `SENSEI_G9_HANDOFF_ADDR` (L2 priv0, 0x1C00FF00,
   [`gap9_slot.h`](../../lib/sensei_gap9/include/sensei_gap9/gap9_slot.h)). It names the slot that started, after any fallback; the app
   reads it at the start of `main()`. L2 shared is no place for it: the runtime powers down banks without allocations.
   The app check refuses a section that covers the record.
4. Arm the 200 ms boot watchdog and launch the app. The SDK runtime disarms it in its start-up, the app needs no code.
5. If neither slot boots: with `SENSEI_SSBL_RECOVERY` the recovery below, else print and wait. The nRF5340 powers GAP9 off
   after its boot timeout and JTAG recovers the board. The SDK SSBL has a recovery CLI on the UART (pads 60 and 61) for this
   case; this one does not.

The signature and the SHA-256 of the image are not checked here: the nRF5340 checks them before an update goes live. A
slot that was damaged later fails its section CRC or its header and is skipped. A running firmware that hangs is not the
SSBL's business either: the nRF5340 sees no HELLO and boots the other slot.

## Recovery

Off by default, untested on hardware. `cmake ... -DSENSEI_SSBL_RECOVERY=ON -DSENSEI_SSBL_KIND=<board id>` (the node:
`make -C firmware/gap9 flash-factory SSBL_RECOVERY=1`). The rules are in [`OTA.md`](../../lib/sensei_gap9/OTA.md), "Recovery".

When neither slot boots, `src/recovery.c` sets up the writer of the update (`g9ota.c`) with the slot on the slot line as
the keep slot and serves it over the SPI link: the HELLO has the major 0xFFFF (so an old nRF5340 refuses it and powers GAP9
off), the jobs are `Gap9RecoveryJob` (begin, chunk, status, finish, seal, abort), `src/recovery_link.c` is the loop. The SPI is
set up only then (same pads and 2 MHz as the app, `gap9_link.h`); the normal boot touches none of it. The writer checks no signature: the
nRF5340 does and then seals. The keep slot, the boot area and everything outside the other slot and the two checkpoint sectors are
never written (the guard of the writer; the GVSoC test checks it). Neither is the SSBL: it stays as it is, the update is
for the slots only.

The strap (`-DSENSEI_SSBL_STRAP=ON`): pad 57 read with a pull-down at start-up; high enters recovery although a slot boots
(the nRF5340 drives P1.06 for `Gap9OtaBegin.force_recovery`). UNVERIFIED hardware: that the line is a free direct wire. Off by default.

Size: the SSBL is 24.7 KiB compressed without recovery and 43.4 KiB with it (SDK 5.22.4, `-Os`, LTO, the end of the data in the section of a board build); its section is 48 KiB. The rule that a sealed slot is no fallback (`g9ota_ckpt.c`, the CRC) adds 528 B of code to the build without recovery and 288 B to the one with it (raw, before compression).
Stack: 8 KiB main stack (the writer hashes and decodes messages on it), set in the three config files.

## Layout

[`GAP9/layouts/sensei_ab.json`](../layouts/sensei_ab.json) (MRAM, 2 MiB):

| Offset | Size | What |
|---|---|---|
| 0x000000 | 0x10000 | boot area: FSBL, meta table, SSBL (section fixed to 48 KiB: 24.7 KiB without recovery, 43.4 KiB with it), partition and volume table (the volume table section is padded to 11536 B, the real table is a few hundred bytes), a 128 B `readfs_mram` placeholder |
| 0x010000 | 0x04000 | checkpoint of the OTA writer |
| 0x014000 | 0xF6000 | slot A |
| 0x10A000 | 0xF6000 | slot B |

A slot is an MCUboot image whose payload is a container (app binary and readfs of gapy); the format is in `gap9_slot.h`.
The app mounts its readfs with the stock driver, which looks up a partition `readfs_mram` in the volume `app` of the partition
table; the placeholder is that entry (its data is never read, the app opens the readfs of its slot). The volume table section is
shrunk by the same amount so the slots do not move.

The boot area is written by the factory flash only: the SSBL is never updated over the air (the FSBL cannot fall back to
another SSBL). The sizes of the boot area sections are fixed in the layout so that the slots do not move; the GVSoC test
checks the offsets. The SSBL section was 28 KiB (30 KiB with GAP SDK 5.22.4 without the recovery) and the volume table pad 32016 B (29968 B) before the
recovery: a board that was flashed with such a layout needs one JTAG flash of the boot area (`make flash-boot` of the node keeps the slots,
[`sensei_ab_boot.json`](../layouts/sensei_ab_boot.json) is the layout of the boot area alone, `-DSENSEI_SSBL_BOOT_ONLY=ON`). The slots
and the checkpoint did not move.

## Build

In the GAP SDK environment (Docker image `gap9:v5.22.4-sensei`):

```
tools/ssbl_docker.sh build <outdir>     # board build, <outdir>/ssbl is the ELF for the layout (ssbl property)
cmake -B build . [-DSENSEI_PLATFORM=gvsoc] [-DSENSEI_SSBL_FORCE_SLOT=0|1] [-DSENSEI_SSBL_SLOT_A=<file> -DSENSEI_SSBL_SLOT_B=<file>]
      [-DSENSEI_SSBL_RECOVERY=ON -DSENSEI_SSBL_KIND=<board id> [-DSENSEI_SSBL_STRAP=ON]] [-DSENSEI_SSBL_BOOT_ONLY=ON]
```

`tools/pack_slot.py` is the one builder of slot images: from `chip.soc.mram.bin` (or an app binary and a readfs) it makes
the container, and the unsigned slot for the tests and test boards (only with `--unsigned` or `--damage`), or the image
signed with imgtool for the update over the air (`--kind` is the board id, `--key` or `--dev` signs; without a key and
without `--unsigned` the tool stops). The app binary of `--app` gets the same checks as the one of `--mram`. An app also uses it for its factory flash. `tools/check_handoff.py`
fails the build of an app if a section covers the hand-off record. `make test-python` in the SDK root runs
`tools/test_pack_slot.py` (needs pytest, cryptography and imgtool: without imgtool the signed-image tests skip, in CI
they fail; the workflow `python-tests.yml` installs them). See [`lib/sensei_gap9/OTA.md`](../../lib/sensei_gap9/OTA.md).

## Test

`tools/ssbl_docker.sh test` runs `test/gvsoc_cases.sh` in Docker: it builds a test app twice, packs two slots, builds the SSBL
with a forced slot (GVSoC does not model the pad) and runs it. Cases: slot A boots, slot B boots, B with a wrong section CRC,
B erased, B without TLV areas fall back to A, A with a wrong CRC falls back to B, both damaged. The test app prints the slot of
the hand-off record: after a fallback it names the slot that started. The `sealed_*` cases put a checkpoint record into the MRAM
image (`-DSENSEI_SSBL_CKPT=<file>`): DONE for B (sealed, never started) is no fallback for a damaged A, but starts when the slot
line names B; CLEARED (it ran once) is a fallback again; DONE for the slot that is not the fallback changes nothing. It also
checks the layout offsets. Prints `SSBL TEST PASS`.
The recovery test build exists only with `-DSENSEI_PLATFORM=gvsoc` and `SENSEI_SSBL_RECOVERY=ON` (CMake stops otherwise, and
`recovery_feed.c` does not compile without the simulator define): it seals without a signature check and must never reach a
board. The options are CMake cache options, so a build directory keeps them: the node Makefile passes `ON` or `OFF` for all of
them, and so does the test script.
Recovery cases: GVSoC has no nRF5340, so the test build (`SENSEI_SSBL_RECOVERY_TEST`, `test/recovery_feed.c`) takes the jobs from a
packed image that the script put into the keep slot, 512 KiB behind its own image (`pack_slot.py --test-signature` makes an image whose
signature TLV has the right shape; the writer checks the shape, the nRF5340 the signature) and sends begin, chunks, finish and seal
through the same job server. `rec_both_bad`: neither slot boots, the writer fills slot B, the SSBL does not take the sealed slot
B as the fallback of the damaged slot A, and boots it when the test sets the slot line to B (the apply; hand-off B).
`rec_no_seal`: no seal, slot B never parses. `rec_forced`: the strap is high (test define), slot A boots after the recovery wrote
slot B.
The slot checks without GVSoC are in `lib/sensei_gap9/tests`.

## Hardware checks (untested on hardware)

- Pad 56 against P1.04: scope P1.04 and pad 56 during power-up. P1.04 goes to its level right after the GAP9 rail is on (the
  SSBL reads it a few ms after the reset): check that the level is there in time.
- No back-powering: with GAP9 off (rail off), P1.04 must be low and the rail must stay at 0 V, also during the nRF5340 boot
  (`pwr_boot_hook`).
- Fallback: damage slot B (erase its first sector over JTAG), set the line high: GAP9 must come up on A (HELLO of A).
- Recovery (build with `SENSEI_SSBL_RECOVERY`): erase both slots over JTAG, power up: the node must report recovery after one
  power-up and not cycle the rail per job; the SPI at the SSBL clock; the time of `finish` against the 8 s job timeout; ten random
  power cuts during a recovery transfer; an unsigned or wrong-key image refused and still in recovery after a power cycle
  (`firmware/docs/TESTING.md`, T18.14 and T18.18 to T18.22). Without the recovery build: GAP9 waits, reflash through JTAG.
- The strap (pad 57 against P1.06): scope both, no other load on the line, no back-powering with GAP9 off.
- Boot config of the factory flash: the SSBL config uses `CONFIG_BOOT_DEVICE_MRAM=y` (the GAP SDK's flashing guide: the boot
  device selects where the flash writes; the files in the MRAM are those of the layout either way). Check that the board
  boots from the MRAM after `make flash-factory` (boot pins select MRAM).
- Boot time and idle current of the new SSBL compared with the SDK SSBL.
