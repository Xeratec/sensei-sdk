# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""pack_slot.py and check_handoff.py: the container out of a gapy MRAM image (partition table), the unsigned slot, the
signed image (imgtool, skipped where it is not installed) and, with a compiler, the cross-check with the writer of
the GAP9 firmware (lib/sensei_gap9/src/g9ota.c on a RAM MRAM).

    python3 -m pytest GAP9/ssbl/tools -q      (make test-python)
"""

import hashlib
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
SDK = HERE.parents[2]
HEADER = SDK / "lib/sensei_gap9/include/sensei_gap9/gap9_slot.h"


def load(name: str):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod  # dataclasses look the module up
    spec.loader.exec_module(mod)
    return mod


mk = load("pack_slot")
ch = load("check_handoff")
KIND = "sensei-test-gap9"


def fake_mram(app: bytes, readfs: bytes, extra_parts: bool = True) -> bytes:
    """A gapy MRAM image in miniature: FSBL, meta table, partition table, then readfs and app (as the real layout)."""
    meta, pt = 0x100, 0x200
    rfs_off = 0x400
    app_off = (rfs_off + len(readfs) + 0xFF) & ~0xFF
    img = bytearray(app_off + len(app))
    struct.pack_into("<I", img, 0, meta)
    struct.pack_into("<II", img, meta + 64, 0, pt)
    parts = []
    if extra_parts:
        parts += [(0, 2, 0xE2, 0, 0x100), (1, 2, 0xE4, meta, 96)]  # fsbl, meta table
    parts += [(2, 1, 0x81, rfs_off, len(readfs)), (3, 0, 0x71, app_off, len(app))]
    struct.pack_into("<HBBBHI", img, pt, 0x2BA, 2, len(parts), len(parts), 0, 0)
    for i, (uuid, ptype, sub, off, size) in enumerate(parts):
        at = pt + 16 + i * 32
        struct.pack_into("<HBBH", img, at, uuid, ptype, sub, 0)
        struct.pack_into("<IIII", img, at + 9, off, size, size, 0)
    img[rfs_off:rfs_off + len(readfs)] = readfs
    img[app_off:app_off + len(app)] = app
    return bytes(img)


def make_app(extra: int, areas = ()) -> bytes:
    """An app binary: the descriptor (header of 1332 B, a table of 52 B per section: start, ptr, size, crc, flags) and
    `extra` bytes of section data after it. `areas`: (start, ptr, size, flags) per section; none: one empty section."""
    areas = areas or [(0, 0x1C000100, 0, 0)]
    desc = bytearray(1332 + 52 * len(areas))
    struct.pack_into("<II", desc, 0, 0xC001B001, len(areas))
    for i, (start, ptr, size, flags) in enumerate(areas):
        struct.pack_into("<5I", desc, 1332 + 52 * i, start, ptr, size, 0, flags)
    return bytes(desc) + bytes((i * 13 + 5) & 0xFF for i in range(extra))


APP = make_app(1001)  # not a multiple of 16
READFS = bytes((i * 7 + 3) & 0xFF for i in range(2000))


def test_container_from_the_partition_table():
    mram = fake_mram(APP, READFS)
    parts = mk.parse_partitions(mram)
    assert [(p.type, p.subtype) for p in parts][-2:] == [(1, 0x81), (0, 0x71)]
    app, rfs = mk.split_mram(mram)
    assert app == APP and rfs == READFS
    c = mk.build_container(app, rfs)
    magic, ver, a_off, a_size, r_off, r_size = struct.unpack_from("<IIIIII", c, 0)
    assert (magic, ver) == (0x42413953, 1) and c[:4] == b"S9AB"
    assert a_off == 32 and a_size == len(APP) and r_off % 16 == 0 and r_off >= a_off + a_size and r_size == len(READFS)
    assert c[a_off:a_off + a_size] == APP and c[r_off:r_off + r_size] == READFS
    assert c[a_off + a_size:r_off] == b"\x00" * (r_off - a_off - a_size)
    assert len(c) == r_off + r_size


def test_bad_images_are_refused():
    with pytest.raises(mk.ImageError, match = "too short"):
        mk.parse_partitions(b"\x00" * 16)
    bad = bytearray(fake_mram(APP, READFS))
    struct.pack_into("<H", bad, 0x200, 0x1234)
    with pytest.raises(mk.ImageError, match = "magic"):
        mk.parse_partitions(bytes(bad))
    with pytest.raises(mk.ImageError, match = "0xC001B001"):
        mk.split_mram(fake_mram(b"\x00" * 64, READFS))
    with pytest.raises(mk.ImageError, match = "hand-off record"):  # a section that the SSBL's record would not survive
        mk.split_mram(fake_mram(make_app(400, [(0x600, 0x1C00FF00 - 0x10, 0x20, 0)]), READFS))
    # no readfs partition: the weights would be missing
    mram = bytearray(fake_mram(APP, READFS))
    pt = 0x200
    for i in range(struct.unpack_from("<B", mram, pt + 3)[0]):
        if mram[pt + 16 + i * 32 + 3] == 0x81:
            mram[pt + 16 + i * 32 + 3] = 0x80
    with pytest.raises(mk.ImageError, match = "readfs"):
        mk.split_mram(bytes(mram))


def test_app_check_follows_the_sdk_header():
    """check_app is the Python side of sensei_g9_app_check: the same limits, the same refusals."""
    base = 1332 + 52
    ok = (base, 0x1C000100, 0x100, 0)
    mk.check_app(make_app(0x400, [ok]))
    for areas, what in (
        ([(base, 0x1C100000 - 0x80, 0x100, 0)], "SSBL"),
        ([(base, 0x1C190000 - 0x10, 0x100, 0)], "end of the L2"),
        ([(base, 0x1C190000, 0x100, 0)], "end of the L2"),
        ([(base, 0x1C00FF00 - 0x40, 0x41, 0)], "hand-off"),
        ([(base, 0x1C00FF0C, 0x100, 0)], "hand-off"),
        ([(0x10000, 0x1C000100, 0x100, 0)], "outside"),
        ([(base, 0x1C000100, 0x1000, 0)], "runs out"),
        ([(base, 0x1C000100, 0x100, 2)], "without blocks"),
        ([(base, 0x1C000100, 0x100, 2 | (1 << 24))], "block"),
    ):
        with pytest.raises(mk.ImageError, match = what):
            mk.check_app(make_app(0x400, areas))
    mk.check_app(make_app(0x400, [(base, 0x1C00FF00 - 0x40, 0x40, 0)]))  # ends at the record
    mk.check_app(make_app(0x400, [(base, 0x10000004, 0x100, 0), (base, 0x1C100000, 0x100, 1)]))  # L1, overlay
    # LZ4: one block of 0x100 B that stays 0x100 B (stored)
    blk = struct.pack("<HH", 0xFF, 0xFF) + bytes(0x100)
    app = make_app(0, [(1332 + 52, 0x1C000100, 0x100, 2 | (1 << 24))]) + blk
    mk.check_app(app)
    bad = bytearray(app)
    struct.pack_into("<I", bad, 32, 4)  # XIP
    with pytest.raises(mk.ImageError, match = "XIP"):
        mk.check_app(bytes(bad))
    bad = bytearray(app)
    struct.pack_into("<I", bad, 4, 33)
    with pytest.raises(mk.ImageError, match = "sections"):
        mk.check_app(bytes(bad))
    # the constants are those of the SDK header
    if True:
        text = HEADER.read_text()
        for name, value in (
            ("SENSEI_G9_L2_BASE", mk.L2_BASE),
            ("SENSEI_G9_L2_LOADER_END", mk.L2_LOADER_END),
            ("SENSEI_G9_L2_END", mk.L2_END),
            ("SENSEI_G9_SSBL_LO", mk.SSBL_LO),
            ("SENSEI_G9_SSBL_HI", mk.SSBL_HI),
            ("SENSEI_G9_HANDOFF_ADDR", mk.HANDOFF_ADDR),
            ("SENSEI_G9_HANDOFF_SIZE", mk.HANDOFF_SIZE),
            ("SENSEI_G9_APP_HDR_SIZE", mk.APP_HDR_SIZE),
            ("SENSEI_G9_APP_AREA_SIZE", mk.APP_AREA_SIZE),
            ("SENSEI_G9_APP_NB_AREAS_MAX", mk.APP_NB_AREAS_MAX),
        ):
            m = re.search(rf"#define\s+{name}\s+(0x[0-9A-Fa-f]+|\d+)[uU]?", text)
            assert m and int(m.group(1), 0) == value, name


def fake_elf(secs) -> bytes:
    """A 32 bit little endian ELF with only a section table: (name, address, size, flags) per section."""
    names = b"\0" + b"".join(n.encode() + b"\0" for n, *_ in secs) + b".shstrtab\0"
    shstr_at = 52
    sh_at = shstr_at + len(names)
    sh_at += -sh_at % 4
    elf = bytearray(sh_at + 40 * (len(secs) + 2))
    elf[:16] = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", elf, 16, 2, 0xF3, 1, 0, 0, sh_at, 0, 52, 32, 0, 40, len(secs) + 2, len(secs) + 1)
    elf[shstr_at:shstr_at + len(names)] = names
    name_at = 1
    for i, (n, addr, size, flags) in enumerate(secs):
        struct.pack_into("<IIIIIIIIII", elf, sh_at + 40 * (i + 1), name_at, 1, flags, addr, 0, size, 0, 0, 1, 0)
        name_at += len(n) + 1
    struct.pack_into("<IIIIIIIIII", elf, sh_at + 40 * (len(secs) + 1), name_at, 3, 0, 0, shstr_at, len(names), 0, 0, 1,
                     0)
    return bytes(elf)


def test_check_handoff_finds_a_section_over_the_record(tmp_path):
    header = tmp_path / "gap9_slot.h"
    header.write_text("#define SENSEI_G9_HANDOFF_ADDR 0x1C00FF00U\n#define SENSEI_G9_HANDOFF_SIZE 16u\n")
    ok = fake_elf([(".data_fc", 0x1C000004, 0x5F0, 2), (".text", 0x1C010000, 0x100, 6), (".dbg", 0x1C00FF00, 8, 0)])
    bad = fake_elf([(".data_fc", 0x1C000004, 0x5F0, 2), (".bss", 0x1C00FF00 - 0x40, 0x41, 3)])
    edge = fake_elf([(".bss", 0x1C00FF10, 0x20, 3), (".a", 0x1C00FF00 - 0x10, 0x10, 3)])  # right above, right below
    for name, data, rc in (("ok", ok, 0), ("bad", bad, 1), ("edge", edge, 0)):
        f = tmp_path / f"{name}.elf"
        f.write_bytes(data)
        assert ch.main(["check_handoff.py", str(f), str(header)]) == rc, name
    # the real header and the section table of a real linked app agree on the record's place, and the tool finds the
    # header without an argument
    assert ch.read_define(HEADER.read_text(), "SENSEI_G9_HANDOFF_ADDR") == mk.HANDOFF_ADDR
    assert ch.DEFAULT_HEADER == HEADER


def test_version_file(tmp_path):
    v = tmp_path / "VERSION"
    v.write_text("VERSION_MAJOR = 1\nVERSION_MINOR = 2\nPATCHLEVEL = 3\n")
    assert mk.read_version(v) == (1, 2, 3)
    v.write_text("VERSION_MAJOR = 1\n")
    with pytest.raises(mk.ImageError):
        mk.read_version(v)
    assert mk.parse_version("1.2.3+4") == (1, 2, 3, 4) and mk.parse_version("1.2.3") == (1, 2, 3, 0)
    with pytest.raises(mk.ImageError):
        mk.parse_version("1.2")


def test_container_only_needs_no_key(tmp_path):
    m, out = tmp_path / "chip.soc.mram.bin", tmp_path / "c.bin"
    m.write_bytes(fake_mram(APP, READFS))
    assert mk.main(["--mram", str(m), "--container-only", "-o", str(out)]) == 0
    assert out.read_bytes() == mk.build_container(APP, READFS)
    assert mk.main(["--mram", str(m), "--kind", KIND, "--unsigned", "-o", str(out)]) == 0
    assert mk.main(["--mram", str(m), "--unsigned", "-o", str(out)]) == 1  # no board id


def test_no_key_is_an_error_not_an_unsigned_image(tmp_path, monkeypatch, capsys):
    """Without a key (and without $SENSEI_GAP9_KEY) the tool stops: an unsigned image only on request."""
    monkeypatch.delenv("SENSEI_GAP9_KEY", raising = False)
    m, out = tmp_path / "chip.soc.mram.bin", tmp_path / "slot.bin"
    m.write_bytes(fake_mram(APP, READFS))
    assert mk.main(["--mram", str(m), "--kind", KIND, "-o", str(out)]) == 1
    assert "no key" in capsys.readouterr().err and not out.exists()
    app = tmp_path / "app.bin"
    app.write_bytes(APP)
    assert mk.main(["--app", str(app), "--kind", KIND, "-o", str(out)]) == 1 and not out.exists()
    # on request: --unsigned, or --damage (also "none"), with --mram and with --app
    for extra in (["--unsigned"], ["--damage", "none"], ["--damage", "header"]):
        out.unlink(missing_ok = True)
        assert mk.main(["--app", str(app), "--kind", KIND, *extra, "-o", str(out)]) == 0 and out.exists()
        out.unlink()
        assert mk.main(["--mram", str(m), "--kind", KIND, *extra, "-o", str(out)]) == 0 and out.exists()
    # a key that does not exist is no reason to fall back to an unsigned image
    out.unlink()
    assert mk.main(["--mram", str(m), "--kind", KIND, "--key", str(tmp_path / "none.pem"), "-o", str(out)]) == 1
    assert not out.exists()


def test_app_path_checks_the_app(tmp_path):
    """--app gets the same checks as --mram (sensei_g9_app_check): a section over the hand-off record, the SSBL area or
    the end of the L2, and XIP, are not packed, signed or not."""
    app, out = tmp_path / "app.bin", tmp_path / "slot.bin"
    base = 1332 + 52
    xip = bytearray(make_app(0x400, [(base, 0x1C000100, 0x100, 0)]))
    struct.pack_into("<I", xip, 32, 4)
    bad = {
        "hand-off": make_app(0x400, [(base, 0x1C00FF00 - 0x10, 0x20, 0)]),
        "SSBL": make_app(0x400, [(base, 0x1C100000 - 0x80, 0x100, 0)]),
        "L2": make_app(0x400, [(base, 0x1C190000 - 0x10, 0x100, 0)]),
        "XIP": bytes(xip),
        "no descriptor": bytes(64),
    }
    for what, data in bad.items():
        app.write_bytes(data)
        for extra in (["--unsigned"], ["--dev"], ["--damage",
                                                  "none"], ["--container-only"], ["--split",
                                                                                  str(tmp_path / "s")]):
            out.unlink(missing_ok = True)
            assert mk.main(["--app", str(app), "--kind", KIND, *extra, "-o", str(out)]) == 1, (what, extra)
            assert not out.exists(), (what, extra)
    app.write_bytes(make_app(0x400, [(base, 0x1C000100, 0x100, 0)]))
    assert mk.main(["--app", str(app), "--kind", KIND, "--unsigned", "-o", str(out)]) == 0


def test_split(tmp_path):
    m = tmp_path / "chip.soc.mram.bin"
    m.write_bytes(fake_mram(APP, READFS))
    assert mk.main(["--mram", str(m), "--split", str(tmp_path / "parts")]) == 0
    assert (tmp_path / "parts/app.bin").read_bytes() == APP and (tmp_path / "parts/readfs.bin").read_bytes() == READFS


def mcuboot_tlvs(data: bytes) -> tuple[dict[int, bytes], dict[int, bytes], int, int, int]:
    """The protected and the other TLVs of an MCUboot image, its header size, image size and protected size."""
    magic, _, hdr, prot, img = struct.unpack_from("<IIHHI", data, 0)
    assert magic == 0x96F3B83D
    out = []
    pos = hdr + img
    for want, end in ((0x6908, hdr + img + prot), (0x6907, len(data))):
        tlvs = {}
        m, total = struct.unpack_from("<HH", data, pos)
        assert m == want and pos + total == end
        q = pos + 4
        while q < end:
            t, ln = struct.unpack_from("<HH", data, q)
            tlvs[t] = data[q + 4:q + 4 + ln]
            q += 4 + ln
        out.append(tlvs)
        pos = end
    return out[0], out[1], hdr, img, prot


def test_unsigned_slot(tmp_path):
    """The slot of the SSBL tests: header, container, kind TLV, hash over header, image and protected TLVs."""
    app, rfs, out = tmp_path / "app.bin", tmp_path / "rfs.bin", tmp_path / "slot.bin"
    app.write_bytes(APP)
    rfs.write_bytes(READFS)
    base = [
        "--app",
        str(app), "--readfs",
        str(rfs), "--kind", KIND, "--version", "1.2.3+4", "--unsigned", "-o",
        str(out)
    ]
    assert mk.main(base) == 0
    data = out.read_bytes()
    container = mk.build_container(APP, READFS)
    prot, tlv, hdr, img, plen = mcuboot_tlvs(data)
    assert hdr == 0x200 and img == len(container) and data[hdr:hdr + img] == container
    assert data[20:24] == struct.pack("<BBH", 1, 2, 3) and struct.unpack_from("<I", data, 24)[0] == 4
    assert prot == {0xA0: KIND.encode()}
    assert tlv == {0x10: hashlib.sha256(data[:hdr + img + plen]).digest()}
    # without readfs the container says so
    assert mk.main(["--app", str(app), "--kind", KIND, "--unsigned", "-o", str(out)]) == 0
    _, _, _, r_off, r_size = struct.unpack_from("<IIIII", out.read_bytes(), 0x200 + 4)
    assert (r_off, r_size) == (0, 0)
    # the damages of the tests
    clean = bytes(data)
    for how, check in (
        ("header", lambda d: d[:4] == bytes(4) and d[4:] == clean[4:]),
        ("app", lambda d: len(d) == len(clean) and sum(a != b for a, b in zip(d, clean)) == 1),
        ("trailer", lambda d: d == clean[:hdr + img]),
    ):
        assert mk.main([*base, "--damage", how]) == 0
        assert check(out.read_bytes()), how


def test_slot_must_fit(tmp_path):
    app, out = tmp_path / "app.bin", tmp_path / "slot.bin"
    app.write_bytes(make_app(mk.SLOT_SIZE))
    assert mk.main(["--app", str(app), "--kind", KIND, "--unsigned", "-o", str(out)]) == 1


def _imgtool() -> str:
    tool = os.environ.get("IMGTOOL") or shutil.which("imgtool")
    if not tool:
        pytest.skip("imgtool not installed")
    return tool


def _key(path: Path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    priv = ec.generate_private_key(ec.SECP256R1())
    path.write_bytes(
        priv.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                           serialization.NoEncryption()))
    return priv


def test_signed_image_round_trip(tmp_path):
    """imgtool image: header, container, protected kind TLV, hash, a signature that verifies, and the key rule."""
    pytest.importorskip("cryptography")
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec, utils

    tool = _imgtool()
    key, m, out = tmp_path / "key.pem", tmp_path / "chip.soc.mram.bin", tmp_path / "gap9.signed.bin"
    priv = _key(key)
    m.write_bytes(fake_mram(APP, READFS))
    args = ["--mram", str(m), "--key", str(key), "--kind", KIND, "--version", "3.4.5", "-o", str(out)]
    # a key in the temporary folder lies in no git work tree, so the only failure left is imgtool itself
    assert mk.main([*args, "--imgtool", tool]) == 0
    data = out.read_bytes()
    prot, tlv, hdr, img, plen = mcuboot_tlvs(data)
    assert hdr == 0x200 and img == len(mk.build_container(APP, READFS))
    assert data[20:24] == bytes([3, 4, 5, 0])  # version 3.4.5
    assert data[hdr:hdr + img] == mk.build_container(APP, READFS)
    assert prot == {0xA0: KIND.encode()}
    digest = hashlib.sha256(data[:hdr + img + plen]).digest()
    assert tlv[0x10] == digest
    # the DER signature over the hash (what the nRF5340 checks with sensei_crypto_ecdsa_verify_hash)
    priv.public_key().verify(tlv[0x22], digest, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    # a key inside a git work tree is refused
    inside = HERE / "_test_key.pem"
    try:
        inside.write_bytes(key.read_bytes())
        assert mk.main(["--mram", str(m), "--key", str(inside), "--kind", KIND, "-o", str(out), "--imgtool", tool]) == 1
    finally:
        inside.unlink(missing_ok = True)


def test_version_file_default_and_warning(tmp_path, capsys):
    pytest.importorskip("cryptography")
    tool = _imgtool()
    key, m, out, v = tmp_path / "key.pem", tmp_path / "m.bin", tmp_path / "o.bin", tmp_path / "VERSION"
    _key(key)
    m.write_bytes(fake_mram(APP, READFS))
    v.write_text("VERSION_MAJOR = 2\nVERSION_MINOR = 5\nPATCHLEVEL = 7\n")
    base = ["--mram", str(m), "--key", str(key), "--kind", KIND, "--version-file", str(v), "-o", str(out)]
    assert mk.main([*base, "--imgtool", tool]) == 0
    assert out.read_bytes()[20:24] == bytes([2, 5, 7, 0]) and "warning" not in capsys.readouterr().err
    assert mk.main([*base, "--version", "2.6.0", "--imgtool", tool]) == 0
    assert "differs" in capsys.readouterr().err


def test_image_goes_through_the_firmware_writer(tmp_path):
    """The writer of the GAP9 firmware (g9ota.c) accepts the file: hash, header, container, kind, image hash,
    signature shape. Needs a C compiler and imgtool; skipped otherwise."""
    pytest.importorskip("cryptography")
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, utils

    tool = _imgtool()
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        pytest.skip("no C compiler")
    gap9, link = SDK / "lib/sensei_gap9", SDK / "lib/sensei_link"
    nanopb = link / "third_party/nanopb"
    srcs = [
        gap9 / "tests/test_g9ota.c",
        gap9 / "src/g9ota.c",
        *(link / "src" / n for n in ("crc.c", "sha256.c")),
        link / "generated/sensei/types.pb.c",
        link / "generated/sensei/gap9_ota.pb.c",
        *(nanopb / n for n in ("pb_common.c", "pb_encode.c", "pb_decode.c")),
    ]
    if not all(s.exists() for s in srcs):
        pytest.skip("SDK sources not found (git submodule update --init)")
    exe = tmp_path / "ota_check"
    inc = [gap9 / "tests", gap9 / "include", SDK / "NRF/system/ota", link / "include", link / "generated", nanopb]
    cmd = [cc, "-std=gnu11", "-O1", "-o", str(exe), *map(str, srcs), *(f"-I{i}" for i in inc), "-DPB_FIELD_32BIT=1"]
    r = subprocess.run(cmd, capture_output = True, text = True)
    assert r.returncode == 0, r.stderr[:500]
    key, m, out = tmp_path / "key.pem", tmp_path / "m.bin", tmp_path / "g.signed.bin"
    priv = _key(key)
    app = make_app(256 * 300)
    m.write_bytes(fake_mram(app, READFS * 20))
    assert mk.main(
        ["--mram",
         str(m), "--key",
         str(key), "--kind", KIND, "--version", "1.2.3", "-o",
         str(out), "--imgtool", tool]) == 0
    r = subprocess.run([str(exe), str(out), "0", KIND], capture_output = True, text = True)
    assert r.returncode == 0, r.stdout + r.stderr
    data = out.read_bytes()
    _, tlv, hdr, img, plen = mcuboot_tlvs(data)
    digest = tlv[0x10]
    assert f"image_id {int.from_bytes(digest[:4], 'little'):08x}" in r.stdout and "version 1.2.3" in r.stdout
    assert digest.hex() in r.stdout
    # What the nRF5340 does with the writer's answer (hash, signature as r || s): the ECDSA check of the real image.
    # It runs sensei_crypto_ecdsa_verify_hash, here the same primitive from the key that signed.
    answer = re.search(r"hash ([0-9a-f]{64}) sig ([0-9a-f]{128})", r.stdout)
    assert answer and answer.group(1) == digest.hex()
    sig = bytes.fromhex(answer.group(2))
    der = utils.encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    prehashed = ec.ECDSA(utils.Prehashed(hashes.SHA256()))
    serialization.load_pem_private_key(key.read_bytes(), None).public_key().verify(der, digest, prehashed)
    with pytest.raises(InvalidSignature):  # another key does not verify it
        ec.generate_private_key(ec.SECP256R1()).public_key().verify(der, digest, prehashed)
    assert priv is not None
    # an image for another board is refused by the writer
    r = subprocess.run([str(exe), str(out), "0", "sensei-other-board"], capture_output = True, text = True)
    assert r.returncode == 1 and "fail 7" in r.stdout  # GAP9_OTA_FAIL_KIND


def test_layout_has_the_readfs_placeholder():
    """The application mounts the readfs of its slot through the stock readfs driver, which still looks for a partition
    labelled readfs_mram in the volume "app" of the partition table (lib/sensei_gap9/gap9/g9ota_mram.c:
    G9OTA_READFS_VOLUME). The A/B layout has to hold a small placeholder of that name there; its data is never read."""
    layout = SDK / "GAP9/layouts/sensei_ab.json"
    sections = json.loads(layout.read_text())["flashes"][0]["sections"]
    by_name = {sec["name"]: sec for sec in sections}
    vols = {v["name"]: v for v in by_name["volume_table"]["properties"]["volumes"]}
    assert "app" in vols and "readfs_mram" in vols["app"]["partitions"], "volume app with readfs_mram is missing"
    assert by_name["readfs_mram"]["template"] == "readfs"
    # the slots stay where gap9_slot.h says: the placeholder is paid for by the padding of the volume table
    names = [sec["name"] for sec in sections]
    assert names.index("readfs_mram") < names.index("checkpoint") < names.index("slot_a") < names.index("slot_b")


# The boot area of the A/B layout, in bytes: the sections that the layout sizes (the SSBL, the partition table, the
# volume table that pads the area, the readfs placeholder) add up to this. The FSBL and the meta table, which gapy sizes
# itself, take the rest of the 64 KiB. A change of one section (a bigger SSBL) has to change another by the same amount.
BOOT_AREA_FIXED_SECTIONS = 61840


def test_layout_boot_area_and_slot_offsets_follow_the_header():
    """The sum of the fixed boot sections and the offsets of the checkpoint and the slots are those of gap9_slot.h. The
    individual sizes are free (the SSBL grows, the volume table padding shrinks) as long as the sum stays."""
    header = HEADER.read_text()

    def define(name: str) -> int:
        return ch.read_define(header, name)

    sections = json.loads((SDK / "GAP9/layouts/sensei_ab.json").read_text())["flashes"][0]["sections"]
    size = {sec["name"]: sec["properties"]["size"] for sec in sections if "size" in sec["properties"]}
    fixed = ("ssbl", "partition_table", "volume_table", "readfs_mram")
    assert sum(size[n] for n in fixed) == BOOT_AREA_FIXED_SECTIONS
    assert BOOT_AREA_FIXED_SECTIONS < define("SENSEI_G9_BOOT_SIZE")  # room left for the FSBL and the meta table
    # order in the layout: boot area, checkpoint, slot A, slot B (gapy places the sections one after the other)
    names = [sec["name"] for sec in sections]
    assert names.index("readfs_mram") < names.index("checkpoint") < names.index("slot_a") < names.index("slot_b")
    assert names[-1] == "slot_b"
    # offsets of the header: the boot area ends where the checkpoint starts, the slots follow, slot B ends the MRAM
    assert define("SENSEI_G9_CKPT_OFF") == define("SENSEI_G9_BOOT_SIZE")
    assert define("SENSEI_G9_SLOT_A_OFF") == define("SENSEI_G9_CKPT_OFF") + define("SENSEI_G9_CKPT_SIZE")
    assert define("SENSEI_G9_SLOT_B_OFF") == define("SENSEI_G9_SLOT_A_OFF") + define("SENSEI_G9_SLOT_SIZE")
    assert define("SENSEI_G9_MRAM_SIZE") == define("SENSEI_G9_SLOT_B_OFF") + define("SENSEI_G9_SLOT_SIZE")
    # the sizes of the layout are those of the header
    assert size["checkpoint"] == define("SENSEI_G9_CKPT_SIZE")
    assert size["slot_a"] == size["slot_b"] == define("SENSEI_G9_SLOT_SIZE")
