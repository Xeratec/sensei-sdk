<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# GAP9 flash layouts

| File | What |
|---|---|
| `sensei_ab.json` | MRAM with the SENSEI SSBL (section 48 KiB, with room for the recovery) and two firmware slots (A and B) for over-the-air updates. See [`GAP9/ssbl`](../ssbl/README.md). |
| `sensei_ab_boot.json` | The boot area of `sensei_ab.json` alone (FSBL, SSBL, tables): flashes the first 64 KiB and leaves the checkpoint and the slots (`SENSEI_SSBL_BOOT_ONLY`). |

Use it as the layout of a GAP9 app (`CONFIG_FLASH_LAYOUT_PATH`, gapy `--multi-flash-content`) and give the sections their
content with `--flash-property=<file>@mram:<section>:binary` (`fsbl`, `ssbl`) or `:file` (`slot_a`, `slot_b`).
The other apps keep their own layout and the SDK SSBL.

The layout holds a 128 B `readfs_mram` placeholder in a volume `app`: the stock readfs driver needs the entry, the app reads its
readfs from its slot.
