#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""The one builder of GAP9 slot images (lib/sensei_gap9/include/sensei_gap9/gap9_slot.h).

A slot holds an MCUboot-format image: a header, the image, a protected TLV area with the board id (kind TLV 0xa0) and
a TLV area with the SHA-256, and for released images the ECDSA P-256 signature. The image is a container of the
GAP9 app binary and its readfs:

    32 B container header  magic "S9AB", version 1, app offset and size, readfs offset and size (little endian)
    app binary             the bytes of the gapy "app binary" partition (first word 0xC001B001)
    padding to 16 B
    readfs                 the bytes of the gapy readfs partition

Input is the app binary and the readfs (`--app`, `--readfs`), or the MRAM image of a GAP9 build
(`--mram chip.soc.mram.bin`, made by `cmake --build <dir> -t image`): the tool takes both out of it with the partition
table, and checks the descriptor of the app binary the way the SSBL and the update writer do (`sensei_g9_app_check`):
no section over the SSBL area, the hand-off record or the end of the L2, no XIP, at most 32 sections, every section
inside the app. An app that fails is not packed.

  pack_slot.py --mram build/chip.soc.mram.bin --kind <board> --key key.pem -o app.signed.bin   (signed with imgtool)
  pack_slot.py --mram build/chip.soc.mram.bin --kind <board> --dev -o app.signed.bin   (the MCUboot development key)
  pack_slot.py --mram build/chip.soc.mram.bin --container-only -o container.bin         (no key, no imgtool)
  pack_slot.py --mram build/chip.soc.mram.bin --split dir                       (dir/app.bin and dir/readfs.bin)
  pack_slot.py --app app.bin [--readfs rfs.bin] --kind <board> --unsigned [--damage app|header|trailer|none] -o slot.bin
                                                          (unsigned, for the SSBL tests and the factory flash of a test board)

`--kind` is the board id that the update writer and the nRF5340 check (for example "sensei-ieq-gap9"). The version is
`--version X.Y.Z[+B]`, or the file given with `--version-file` (a Zephyr style VERSION file; the version must be the one
the firmware reports in its HELLO, or the nRF5340 reverts the update). The key is a PEM file (ECDSA P-256) from `--key`
or `$SENSEI_GAP9_KEY`; it must lie outside git. `--dev` signs with the public development key of MCUboot
(`root-ec-p256.pem`, `$MCUBOOT_DEV_KEY` or `$ZEPHYR_BASE/../bootloader/mcuboot`): anyone can sign for it (lab nodes
only). imgtool: `$IMGTOOL` or the one on PATH. An unsigned image (the SSBL does not look at the signature, the writer and
the nRF5340 do, and refuse it) is made only on request: `--unsigned`, or `--damage`. Without a key and without these
the tool stops, so that a missing key never gives an image that the node would refuse (or, on a board without the
check, accept). The app binary of `--app` gets the same checks as the one of `--mram`.

Tests: test_pack_slot.py in this folder.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

MAGIC = 0x96F3B83D
HEADER_SIZE = 0x200
ALIGN = 16
CONTAINER_MAGIC = 0x42413953  # "S9AB"
CONTAINER_VERSION = 1
CONTAINER_SIZE = 32
TLV_INFO = 0x6907
TLV_PROT = 0x6908
TLV_SHA256 = 0x10
TLV_KIND = 0xA0
SLOT_SIZE = 0xF6000  # one slot, 984 KiB (gap9_slot.h)
APP_MAGIC = 0xC001B001  # first word of a gapy app binary

# The descriptor of an app binary (gap-sdk bsp/app_bin.h) and the L2 map: the values of gap9_slot.h of the SDK
# (lib/sensei_gap9), which test_pack_slot.py compares with the header.
APP_HDR_SIZE = 1332  # sizeof(app_bin_desc_header_t)
APP_AREA_SIZE = 52  # sizeof(app_bin_desc_mem_area_t)
APP_NB_AREAS_MAX = 32
APP_FLAG_OVERLAY, APP_FLAG_LZ4 = 1, 2
L2_BASE, L2_LOADER_END, L2_END = 0x1C000000, 0x1D000000, 0x1C190000
SSBL_LO, SSBL_HI = 0x1C100000, 0x1C190000
HANDOFF_ADDR, HANDOFF_SIZE = 0x1C00FF00, 16

# gap-sdk bsp/flash_partition_v2.h
TYPE_APP, TYPE_DATA = 0x00, 0x01
SUBTYPE_APP_BIN, SUBTYPE_DATA_READFS = 0x71, 0x81
PT_MAGIC = 0x02BA
PT_ENTRY = 32


class ImageError(Exception):
    pass


@dataclass
class Partition:
    type: int
    subtype: int
    offset: int
    size: int


def parse_partitions(mram: bytes) -> list[Partition]:
    """The partitions of the partition table of a gapy MRAM image. The first word is the address of the meta table,
    whose entry at +64 is the address of partition table A."""
    if len(mram) < 0x100:
        raise ImageError("the MRAM image is too short")
    meta = struct.unpack_from("<I", mram, 0)[0]
    if meta + 96 > len(mram):
        raise ImageError("no meta table in the MRAM image (is it a gapy mram.bin?)")
    pt = struct.unpack_from("<II", mram, meta + 64)[1]
    if pt + 16 > len(mram):
        raise ImageError("the partition table is outside the image")
    magic, _version, count = struct.unpack_from("<HBB", mram, pt)
    if magic != PT_MAGIC:
        raise ImageError(f"wrong partition table magic 0x{magic:x}")
    parts = []
    for i in range(count):
        at = pt + 16 + i * PT_ENTRY
        if at + PT_ENTRY > len(mram):
            raise ImageError("the partition table is cut off")
        _uuid, ptype, subtype, _flags = struct.unpack_from("<HBBH", mram, at)
        offset, size, _max, _crc = struct.unpack_from("<IIII", mram, at + 9)
        parts.append(Partition(ptype, subtype, offset, size))
    return parts


def _one(parts: list[Partition], ptype: int, subtype: int, what: str) -> Partition:
    found = [p for p in parts if p.type == ptype and p.subtype == subtype]
    if len(found) != 1:
        raise ImageError(f"expected one {what} partition in the MRAM image, found {len(found)}")
    return found[0]


def split_mram(mram: bytes) -> tuple[bytes, bytes]:
    """(app binary, readfs) from a gapy MRAM image."""
    parts = parse_partitions(mram)
    app = _one(parts, TYPE_APP, SUBTYPE_APP_BIN, "app")
    rfs = _one(parts, TYPE_DATA, SUBTYPE_DATA_READFS, "readfs")
    out = []
    for name, p in (("app", app), ("readfs", rfs)):
        if p.size == 0 or p.offset + p.size > len(mram):
            raise ImageError(f"the {name} partition (0x{p.offset:x}, {p.size} B) does not lie in the image")
        out.append(mram[p.offset:p.offset + p.size])
    if struct.unpack_from("<I", out[0], 0)[0] != APP_MAGIC:
        raise ImageError("the app partition does not start with an app binary (0xC001B001)")
    check_app(out[0])
    return out[0], out[1]


def check_app(app: bytes) -> None:
    """The checks of `sensei_g9_app_check` (gap9_slot.h) on an app binary: what the SSBL would load must not harm the
    SSBL, the hand-off record or anything beyond the L2. Raises ImageError."""
    if len(app) < APP_HDR_SIZE or struct.unpack_from("<I", app, 0)[0] != APP_MAGIC:
        raise ImageError("the app binary is too short or has no descriptor (0xC001B001)")
    n = struct.unpack_from("<I", app, 4)[0]
    if n == 0 or n > APP_NB_AREAS_MAX:
        raise ImageError(f"the app binary has {n} sections (1 to {APP_NB_AREAS_MAX} are allowed)")
    if struct.unpack_from("<I", app, 32)[0] != 0:
        raise ImageError("the app binary uses XIP: the loader would map absolute flash addresses")
    if len(app) < APP_HDR_SIZE + n * APP_AREA_SIZE:
        raise ImageError("the section table of the app binary is cut off")
    for i in range(n):
        start, ptr, size, _crc, flags = struct.unpack_from("<5I", app, APP_HDR_SIZE + i * APP_AREA_SIZE)
        if size == 0 or flags & APP_FLAG_OVERLAY:
            continue  # nothing is loaded
        what = f"section {i} (to 0x{ptr:08x}, {size} B)"
        if start >= len(app):
            raise ImageError(f"{what}: its data lies outside the app binary")
        if flags & APP_FLAG_LZ4:
            blocks, cur, total = flags >> 24, 0, 0
            if blocks == 0:
                raise ImageError(f"{what}: LZ4 section without blocks")
            for _ in range(blocks):
                if len(app) - start - cur < 4:
                    raise ImageError(f"{what}: an LZ4 block lies outside the app binary")
                dec, comp = (x + 1 for x in struct.unpack_from("<HH", app, start + cur))
                if comp > len(app) - start - cur - 4:
                    raise ImageError(f"{what}: an LZ4 block runs out of the app binary")
                cur += 4 + comp
                total += dec
            if total != size:
                raise ImageError(f"{what}: the LZ4 blocks add up to {total} B")
        elif size > len(app) - start:
            raise ImageError(f"{what}: its data runs out of the app binary")
        if L2_BASE <= ptr < L2_LOADER_END:  # the loader treats this as L2
            if ptr >= L2_END or size > L2_END - ptr:
                raise ImageError(f"{what}: beyond the end of the L2 (0x{L2_END:08x})")
            if ptr < SSBL_HI and ptr + size > SSBL_LO:
                raise ImageError(f"{what}: over the area of the SSBL (0x{SSBL_LO:08x} to 0x{SSBL_HI:08x})")
            if ptr < HANDOFF_ADDR + HANDOFF_SIZE and ptr + size > HANDOFF_ADDR:
                raise ImageError(f"{what}: over the hand-off record of the SSBL (0x{HANDOFF_ADDR:08x})")


def pad(data: bytes, n: int = ALIGN) -> bytes:
    return data + bytes(-len(data) % n)


def build_container(app: bytes, readfs: bytes = b"") -> bytes:
    """Container header, app binary, padding, readfs (offset and size 0 if there is none)."""
    app_off = CONTAINER_SIZE
    readfs_off = app_off + len(pad(app))
    head = struct.pack("<IIIIII8x", CONTAINER_MAGIC, CONTAINER_VERSION, app_off, len(app), readfs_off if readfs else 0,
                       len(readfs))
    assert len(head) == CONTAINER_SIZE
    return head + pad(app) + readfs


def parse_version(version: str) -> tuple[int, int, int, int]:
    """major, minor, revision and build number of "X.Y.Z" or "X.Y.Z+B"."""
    ver, _, build = version.partition("+")
    try:
        major, minor, rev = (int(x) for x in ver.split("."))
        return major, minor, rev, int(build or 0)
    except ValueError as exc:
        raise ImageError(f"version {version!r}: needs X.Y.Z or X.Y.Z+B") from exc


def read_version(path: Path) -> tuple[int, int, int]:
    """major, minor, patch of a Zephyr style VERSION file."""
    vals = {}
    for line in path.read_text().splitlines():
        if "=" in line:
            k, v = (s.strip() for s in line.split("=", 1))
            vals[k] = v
    try:
        return int(vals["VERSION_MAJOR"]), int(vals["VERSION_MINOR"]), int(vals["PATCHLEVEL"])
    except (KeyError, ValueError) as exc:
        raise ImageError(f"{path}: needs VERSION_MAJOR, VERSION_MINOR and PATCHLEVEL") from exc


def _tlv(kind: int, value: bytes) -> bytes:
    return struct.pack("<HH", kind, len(value)) + value


TLV_ECDSA_SIG = 0x22


def shaped_signature_tlv() -> bytes:
    """A TLV of the shape of an ECDSA P-256 signature (DER of two 32 byte integers) that is not one. The recovery writer
    of the SSBL checks the shape only (the nRF5340 checks the signature): the GVSoC test feeds it images with this. The
    nRF5340 refuses such an image."""
    der = b"\x30\x44" + b"\x02\x20" + bytes(range(0x20, 0x40)) + b"\x02\x20" + bytes(range(0x40, 0x60))
    return _tlv(TLV_ECDSA_SIG, der)


def build_unsigned(container: bytes, version: str, kind: str, shaped_signature: bool = False) -> bytes:
    """The slot as imgtool would make it, without the signature: header, container, protected TLV with the kind,
    TLV area with the SHA-256. `shaped_signature`: also a TLV that has the shape of a signature (tests of the recovery
    writer only)."""
    major, minor, rev, build = parse_version(version)
    prot_body = _tlv(TLV_KIND, kind.encode())
    prot = struct.pack("<HH", TLV_PROT, 4 + len(prot_body)) + prot_body
    header = struct.pack("<IIHHIIBBHII", MAGIC, 0, HEADER_SIZE, len(prot), len(container), 0, major, minor, rev, build,
                         0)
    header += bytes(HEADER_SIZE - len(header))
    digest = hashlib.sha256(header + container + prot).digest()
    info_body = _tlv(TLV_SHA256, digest) + (shaped_signature_tlv() if shaped_signature else b"")
    info = struct.pack("<HH", TLV_INFO, 4 + len(info_body)) + info_body
    return header + container + prot + info


def _in_git(path: Path) -> bool:
    folder = path.resolve().parent
    try:
        r = subprocess.run(["git", "-C", str(folder), "rev-parse", "--is-inside-work-tree"],
                           capture_output = True,
                           check = False)
    except OSError:
        return False
    return r.returncode == 0


def dev_key() -> Path:
    """The public development key of MCUboot, the one that Debug builds of the nRF5340 trust."""
    cands = []
    if os.environ.get("MCUBOOT_DEV_KEY"):
        cands.append(Path(os.environ["MCUBOOT_DEV_KEY"]))
    if os.environ.get("ZEPHYR_BASE"):
        cands.append(Path(os.environ["ZEPHYR_BASE"]).parent / "bootloader" / "mcuboot" / "root-ec-p256.pem")
    for c in cands:
        if c.is_file():
            return c
    raise ImageError("the MCUboot development key root-ec-p256.pem was not found (set $MCUBOOT_DEV_KEY)")


def sign(container: Path, out: Path, key: Path, version: str, kind: str, imgtool: str, dev: bool = False) -> None:
    """imgtool sign: the 0x200 B header, the protected TLV 0xa0 with the board id, the SHA-256 and the ECDSA P-256
    signature."""
    if not dev and _in_git(key):
        raise ImageError(f"the key {key} lies inside a git work tree: keys never go into git")
    tool = shutil.which(imgtool) or (imgtool if Path(imgtool).is_file() else None)
    if tool is None:
        raise ImageError("imgtool not found (pip install imgtool, or set $IMGTOOL)")
    cmd = [
        tool, "sign", "--key",
        str(key), "--header-size",
        hex(HEADER_SIZE), "--align", "8", "--slot-size",
        hex(SLOT_SIZE), "--version", version, "--pad-header", "--custom-tlv",
        hex(TLV_KIND), kind,
        str(container),
        str(out)
    ]
    r = subprocess.run(cmd, capture_output = True, text = True, check = False)
    if r.returncode != 0:
        raise ImageError(f"imgtool sign failed: {r.stderr.strip() or r.stdout.strip()}")
    v = subprocess.run([tool, "verify", "--key", str(key), str(out)], capture_output = True, text = True, check = False)
    if v.returncode != 0:
        raise ImageError("the signed image does not verify")


def damage(slot: bytearray, how: str, app_len: int, image_len: int) -> None:
    """Break an unsigned slot on purpose, for the SSBL tests."""
    if how == "app":  # one bit in the app data: the CRC of its section fails
        slot[HEADER_SIZE + CONTAINER_SIZE + app_len - 64] ^= 0x01
    elif how == "header":  # an erased slot
        slot[0:4] = bytes(4)
    elif how == "trailer":  # a write that stopped before the TLV areas
        del slot[HEADER_SIZE + image_len:]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description = __doc__, formatter_class = argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mram", type = Path, help = "chip.soc.mram.bin of the GAP9 build: app and readfs come out of it")
    ap.add_argument("--app", type = Path, help = "gapy app binary section")
    ap.add_argument("--readfs", type = Path, help = "gapy readfs section")
    ap.add_argument("-o", "--out", "--output", dest = "out", type = Path, help = "output file")
    ap.add_argument("--split", type = Path, metavar = "DIR", help = "write DIR/app.bin and DIR/readfs.bin and stop")
    ap.add_argument("--kind", help = "board id of the image (kind TLV), for example sensei-ieq-gap9")
    ap.add_argument("--version", help = "X.Y.Z[+B] (default: --version-file, else 1.0.0+0)")
    ap.add_argument("--version-file", type = Path, help = "Zephyr style VERSION file with the version of the firmware")
    ap.add_argument("--key", type = Path, default = os.environ.get("SENSEI_GAP9_KEY"), help = "PEM key (ECDSA P-256)")
    ap.add_argument("--dev", action = "store_true", help = "sign with the MCUboot development key (Debug nodes only)")
    ap.add_argument("--container-only", action = "store_true", help = "write the container only, no key needed")
    ap.add_argument("--unsigned",
                    action = "store_true",
                    help = "write an unsigned slot image (SSBL tests, factory flash of a test board); needs no key")
    ap.add_argument("--imgtool", default = os.environ.get("IMGTOOL", "imgtool"))
    ap.add_argument(
        "--test-signature",
        action = "store_true",
        help = "unsigned slot with a TLV of the shape of a signature (for the GVSoC test of the SSBL recovery, "
        "which checks the shape only; the nRF5340 refuses it)")
    ap.add_argument("--damage",
                    choices = ["none", "app", "header", "trailer"],
                    default = None,
                    help = "break the unsigned slot (for tests; implies --unsigned)")
    args = ap.parse_args(argv)
    try:
        if args.mram:
            app, readfs = split_mram(args.mram.read_bytes())
        elif args.app:
            app = args.app.read_bytes()
            readfs = args.readfs.read_bytes() if args.readfs else b""
            check_app(app)  # as for --mram: an app that would harm the SSBL or the record is not packed
        else:
            raise ImageError("--mram or --app is needed")
        if args.split:
            args.split.mkdir(parents = True, exist_ok = True)
            (args.split / "app.bin").write_bytes(app)
            (args.split / "readfs.bin").write_bytes(readfs)
            print(f"{args.split}: app {len(app)} B, readfs {len(readfs)} B")
            return 0
        if not args.out:
            raise ImageError("-o is needed")
        container = build_container(app, readfs)
        if len(container) + HEADER_SIZE + 0x200 > SLOT_SIZE:
            raise ImageError(f"the image ({len(container)} B) does not fit a slot of {SLOT_SIZE} B")
        if args.container_only:
            args.out.write_bytes(container)
            print(f"{args.out}: container, app {len(app)} B, readfs {len(readfs)} B, {len(container)} B")
            return 0
        if not args.kind:
            raise ImageError("--kind is needed: the board id of the image")
        shipped = None
        if args.version_file:
            shipped = ".".join(str(x) for x in read_version(args.version_file))
        version = args.version or shipped or "1.0.0+0"
        if shipped and version.split("+")[0] != shipped:
            print(
                f"warning: version {version} differs from {args.version_file} ({shipped}), the version that the firmware"
                " reports in its HELLO: the nRF5340 reverts an image whose two versions differ",
                file = sys.stderr)
        if args.dev:
            args.key = dev_key()
        if args.key:
            tmp = args.out.with_name(args.out.name + ".container")
            tmp.write_bytes(container)
            try:
                sign(tmp, args.out, Path(args.key), version, args.kind, args.imgtool, args.dev)
            finally:
                tmp.unlink(missing_ok = True)
            how = f"version {version}, kind {args.kind}, app {len(app)} B, readfs {len(readfs)} B"
            print(f"{args.out}: {how}, {args.out.stat().st_size} B of {SLOT_SIZE}")
            return 0
        if not (args.unsigned or args.damage is not None or args.test_signature):
            raise ImageError(
                "no key: give --key (or $SENSEI_GAP9_KEY) or --dev to sign, or --unsigned for an image that "
                "the writer and the nRF5340 refuse")
        slot = bytearray(build_unsigned(container, version, args.kind, args.test_signature))
        if len(slot) > SLOT_SIZE:
            raise ImageError(f"slot image is {len(slot)} B, a slot holds {SLOT_SIZE} B")
        damage(slot, args.damage or "none", len(app), len(container))
        args.out.write_bytes(slot)
        print(f"{args.out}: {len(slot)} B (unsigned), app {len(app)} B, readfs {len(readfs)} B")
        return 0
    except ImageError as exc:
        print(f"pack_slot.py: {exc}", file = sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
