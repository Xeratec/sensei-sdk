#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Build-time check: nothing of the GAP9 application lies on the hand-off record of the SSBL.

The SSBL leaves the slot it started in 16 bytes of L2 priv0 (`SENSEI_G9_HANDOFF_ADDR` in the SDK header
`lib/sensei_gap9/include/sensei_gap9/gap9_slot.h`), and the application reads it at the start of main(). The record
survives only if no section of the application (code, data, bss, stacks) covers it: the loader and the start-up code
would write there. This tool reads the section table of the ELF and fails if an allocated section overlaps the record.
The heap of L2 priv0 is the other user of that area: it is off with CONFIG_MALLOC_NO_L2_PRIV=y, which the cmake of the app
checks in the config file of the app.

    check_handoff.py app.elf [gap9_slot.h]      (the header: lib/sensei_gap9/include/sensei_gap9/ of this SDK)

Tests: test_pack_slot.py in this folder.
"""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

SHF_ALLOC = 0x2
DEFAULT_HEADER = Path(__file__).resolve().parents[3] / "lib/sensei_gap9/include/sensei_gap9/gap9_slot.h"


def read_define(header: str, name: str) -> int:
    m = re.search(rf"#define\s+{name}\s+(0x[0-9A-Fa-f]+|\d+)[uU]?", header)
    if not m:
        raise SystemExit(f"check_handoff.py: {name} not found in the header")
    return int(m.group(1), 0)


def sections(elf: bytes) -> list[tuple[str, int, int]]:
    """(name, address, size) of the allocated sections of a 32 bit little endian ELF."""
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 1:
        raise SystemExit("check_handoff.py: not a 32 bit little endian ELF file")
    shoff = struct.unpack_from("<I", elf, 32)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 46)
    hdrs = [struct.unpack_from("<IIIIIIIIII", elf, shoff + i * shentsize) for i in range(shnum)]
    str_off = hdrs[shstrndx][4]
    out = []
    for name, _type, flags, addr, _off, size, *_rest in hdrs:
        if flags & SHF_ALLOC and size:
            end = elf.index(b"\0", str_off + name)
            out.append((elf[str_off + name:end].decode(errors = "replace"), addr, size))
    return out


def main(argv: list[str]) -> int:
    if not 2 <= len(argv) <= 3:
        print(__doc__)
        return 2
    header = Path(argv[2]) if len(argv) == 3 else None
    if header is None:
        header = DEFAULT_HEADER
    text = header.read_text()
    lo = read_define(text, "SENSEI_G9_HANDOFF_ADDR")
    hi = lo + read_define(text, "SENSEI_G9_HANDOFF_SIZE")
    bad = [(n, a, s) for n, a, s in sections(Path(argv[1]).read_bytes()) if a < hi and a + s > lo]
    for n, a, s in bad:
        print(f"check_handoff.py: section {n} (0x{a:08x}, {s} B) covers the hand-off record 0x{lo:08x}",
              file = sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
