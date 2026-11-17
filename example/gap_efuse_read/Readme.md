<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

This example reads and prints all GAP9 efuse registers. Reading is
non-destructive (see the note in `src_GAP/main.c` about the permanence of
*programming* efuses).

## GAP9

This is a GAP9-only example. Its stdout uses **JTAG semihosting**
(`CONFIG_IO_TYPE_JTAG=y`), so it must be run **attached** via a debugger — it
prints to the `gap run` console. Do not boot it standalone from flash: JTAG
semihosting hangs when there is no debugger attached.

```sh
cd src_GAP
gap init
gap run
```
