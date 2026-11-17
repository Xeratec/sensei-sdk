#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Pack a GAP9 app binary (and a readfs) into a slot image for the SSBL (lib/sensei_gap9/include/sensei_gap9/gap9_slot.h).

The slot has the layout of an MCUboot image (header, image, protected TLV with the kind, TLV with the SHA-256) but no
signature: it is for the SSBL tests and the factory flash of a test board. Released images are signed with imgtool
(the node's tools/make_gap9_ota.py), the SSBL does not look at the signature.

  pack_slot.py --app app.bin [--readfs rfs.bin] [--version 1.2.3+4] [--damage app|header|trailer|none] -o slot.bin
"""

import argparse
import hashlib
import struct
import sys

MAGIC = 0x96F3B83D
HDR = 0x200
ALIGN = 16
CONTAINER_MAGIC = 0x42413953  # "S9AB"
TLV_INFO = 0x6907
TLV_PROT = 0x6908
TLV_SHA256 = 0x10
TLV_KIND = 0xA0
SLOT_SIZE = 0xF6000


def pad(data: bytes, n: int = ALIGN) -> bytes:
    return data + bytes(-len(data) % n)


def tlv(kind: int, value: bytes) -> bytes:
    return struct.pack("<HH", kind, len(value)) + value


def build(app: bytes, readfs: bytes, version: str, kind: str) -> bytes:
    ver, _, build_num = version.partition("+")
    major, minor, rev = (int(x) for x in ver.split("."))
    app_p = pad(app)
    rfs_p = pad(readfs)
    app_off = 32
    rfs_off = app_off + len(app_p)
    container = struct.pack("<IIIIIIII", CONTAINER_MAGIC, 1, app_off, len(app), rfs_off if readfs else 0, len(readfs),
                            0, 0)
    image = container + app_p + rfs_p
    prot_body = tlv(TLV_KIND, kind.encode())
    prot = struct.pack("<HH", TLV_PROT, 4 + len(prot_body)) + prot_body
    header = struct.pack(
        "<IIHHIIBBHII",
        MAGIC,
        0,
        HDR,
        len(prot),
        len(image),
        0,
        major,
        minor,
        rev,
        int(build_num or 0),
        0,
    )
    header = header + bytes(HDR - len(header))
    digest = hashlib.sha256(header + image + prot).digest()
    info_body = tlv(TLV_SHA256, digest)
    info = struct.pack("<HH", TLV_INFO, 4 + len(info_body)) + info_body
    return header + image + prot + info


def main() -> int:
    ap = argparse.ArgumentParser(description = __doc__, formatter_class = argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", required = True, help = "gapy app binary section")
    ap.add_argument("--readfs", help = "gapy readfs section")
    ap.add_argument("--version", default = "1.0.0+0")
    ap.add_argument("--kind", default = "sensei-ieq-gap9")
    ap.add_argument("--damage", choices = ["none", "app", "header", "trailer"], default = "none", help = "for tests")
    ap.add_argument("-o", "--output", required = True)
    a = ap.parse_args()

    app = open(a.app, "rb").read()
    readfs = open(a.readfs, "rb").read() if a.readfs else b""
    slot = bytearray(build(app, readfs, a.version, a.kind))
    if len(slot) > SLOT_SIZE:
        print(f"slot image is {len(slot)} B, a slot holds {SLOT_SIZE} B", file = sys.stderr)
        return 1
    if a.damage == "app":  # one bit in the app data: the CRC of its section fails
        slot[HDR + 32 + len(app) - 64] ^= 0x01
    elif a.damage == "header":  # an erased slot
        slot[0:4] = bytes(4)
    elif a.damage == "trailer":  # a write that stopped before the TLV areas
        del slot[HDR + 32 + len(pad(app)) + len(pad(readfs)):]
    open(a.output, "wb").write(slot)
    print(f"{a.output}: {len(slot)} B, app {len(app)} B, readfs {len(readfs)} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
