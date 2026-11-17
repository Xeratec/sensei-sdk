<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# SENSEI SSBL

Second stage bootloader of the GAP9 on the SENSEI base board. It starts the firmware of slot A or slot B of the MRAM. The
nRF5340 picks the slot with the level of GAP9 pad 56 (net `CS_nRF_GAP`, nRF P1.04): low A, high B. The SSBL does not decide
whether a firmware is good. The nRF5340 does (trial boot, [`sensei_gap9_trial.h`](../../NRF/system/ota/sensei_gap9_trial.h)).

Forked from `utils/ssbl/src` of the GAP SDK 5.22.3 (GreenWaves Technologies, Apache-2.0): the load of the app binary into L2
and the launch are the same SDK calls. The partition and volume table, the boot counter and the GAP CLI recovery are gone.

## Boot

1. Read pad 56 with the internal pull-down, five samples, majority. A line that nobody drives selects A.
2. Try that slot, then the other one. A slot is tried as follows:
   - the MCUboot header, the TLV areas behind the image (a slot whose write stopped early has none) and the container
     ([`gap9_slot.h`](../../lib/sensei_gap9/include/sensei_gap9/gap9_slot.h), host tested);
   - the app descriptor (area count, no XIP, no section over the SSBL in the upper L2);
   - `pi_app_bin_load()` with the CRC32 of every section.
3. Write the hand-off record: magic, slot, ~slot at `SENSEI_G9_HANDOFF_ADDR` (L2 priv0, 0x1C00FF00,
   [`gap9_slot.h`](../../lib/sensei_gap9/include/sensei_gap9/gap9_slot.h)). It names the slot that started, after any fallback; the app
   reads it at the start of `main()`. L2 shared is no place for it: the runtime powers down banks without allocations.
   The app check refuses a section that covers the record.
4. Arm the 200 ms boot watchdog and launch the app. The SDK runtime disarms it in its start-up, the app needs no code.
5. If neither slot boots, print and wait. The nRF5340 powers GAP9 off after its boot timeout. JTAG recovers the board.

The signature and the SHA-256 of the image are not checked here: the nRF5340 checks them before an update goes live. A
slot that was damaged later fails its section CRC or its header and is skipped. A running firmware that hangs is not the
SSBL's business either: the nRF5340 sees no HELLO and boots the other slot.

## Layout

[`GAP9/layouts/sensei_ab.json`](../layouts/sensei_ab.json) (MRAM, 2 MiB):

| Offset | Size | What |
|---|---|---|
| 0x000000 | 0x10000 | boot area: FSBL, meta table, SSBL (section fixed to 28 KiB, the SSBL needs 23.7 KiB compressed), partition and volume table (padded), a 128 B `readfs_mram` placeholder |
| 0x010000 | 0x04000 | checkpoint of the OTA writer |
| 0x014000 | 0xF6000 | slot A |
| 0x10A000 | 0xF6000 | slot B |

A slot is an MCUboot image whose payload is a container (app binary and readfs of gapy); the format is in `gap9_slot.h`.
The app mounts its readfs with the stock driver, which looks up a partition `readfs_mram` in the volume `app` of the partition
table; the placeholder is that entry (its data is never read, the app opens the readfs of its slot). The volume table section is
shrunk by the same amount so the slots do not move.

The boot area is written by the factory flash only: the SSBL is never updated over the air (the FSBL cannot fall back to
another SSBL). The sizes of the boot area sections are fixed in the layout so that the slots do not move; the GVSoC test
checks the offsets.

## Build

In the GAP SDK environment (Docker image `gap9:v5.22.3-sensei`):

```
tools/ssbl_docker.sh build <outdir>     # board build, <outdir>/ssbl is the ELF for the layout (ssbl property)
cmake -B build . [-DSENSEI_PLATFORM=gvsoc] [-DSENSEI_SSBL_FORCE_SLOT=0|1] [-DSENSEI_SSBL_SLOT_A=<file> -DSENSEI_SSBL_SLOT_B=<file>]
```

`tools/pack_slot.py` packs an app binary (and a readfs) into an unsigned slot image for tests and test boards. Released
images are signed with imgtool by the node's tools.

## Test

`tools/ssbl_docker.sh test` runs `test/gvsoc_cases.sh` in Docker: it builds a test app twice, packs two slots, builds the SSBL
with a forced slot (GVSoC does not model the pad) and runs it. Cases: slot A boots, slot B boots, B with a wrong section CRC,
B erased, B without TLV areas fall back to A, A with a wrong CRC falls back to B, both damaged. The test app prints the slot of
the hand-off record: after a fallback it names the slot that started. It also checks the layout offsets. Prints `SSBL TEST PASS`.
The slot checks without GVSoC are in `lib/sensei_gap9/tests`.

## Hardware checks (untested on hardware)

- Pad 56 against P1.04: scope P1.04 and pad 56 during power-up. P1.04 must be at its level before the GAP9 rail rises, the
  SSBL reads it a few ms after the reset.
- No back-powering: with GAP9 off (rail off), P1.04 must be low and the rail must stay at 0 V, also during the nRF5340 boot
  (`pwr_boot_hook`).
- Fallback: damage slot B (erase its first sector over JTAG), set the line high: GAP9 must come up on A (HELLO of A).
- Recovery: with both slots damaged GAP9 waits. Reflash through JTAG (`make flash-factory` of the node).
- Boot time and idle current of the new SSBL compared with the SDK SSBL.
