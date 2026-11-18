# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Field diagnostics: reset cause names, the fault record (bytes as lib/sensei_diag writes them), addr2line and the
ELF lookup by image id."""

import subprocess
import textwrap
import zlib
from pathlib import Path

import pytest
from sensei_sdk import diag, smp
from sensei_sdk.smp_sim import make_image


def sample() -> diag.Fault:
    return diag.Fault(
        reason = 3,
        pc = 0x00012345,
        lr = 0x0001ABCD,
        thread = "app_wq",
        uptime_s = 86400,
        last_job = 7,
        image_id = 0xDEADBEEF,
        boot_count = 12,
        status = 0x01000000,
        coredump = True,
    )


def test_reset_names_and_text():
    assert diag.reset_names(0) == []
    assert diag.reset_text(0) == "-"
    assert diag.reset_text(diag.RESET_POR) == "POR"
    assert diag.reset_text(diag.RESET_SOFTWARE | diag.RESET_FAULT) == "SOFT|FAULT"
    assert diag.reset_text(1 << 4 | 1 << 0) == "PIN|WDT"
    assert diag.reset_text(1 << 20) == "-"  # no name for this bit


def test_reason_names():
    assert diag.reason_name(0) == "exception" and diag.reason_name(2) == "stack" and diag.reason_name(0x80) == "modem"
    assert diag.reason_name(16) == "mem-generic"
    assert diag.reason_name(36) == "usage-undefined-instruction"
    assert diag.reason_name(44) == "secure-lazy-state-error"
    assert diag.reason_name(45) == "arch" and diag.reason_name(9) == "unknown"


def test_packed_record_layout_matches_the_c_codec():
    """The bytes and the checks of lib/sensei_diag/tests/test_diag.c: little endian, magic D1F7, CRC-32 at 48."""
    raw = diag.pack_fault(sample())
    assert len(raw) == diag.FAULT_BYTES == 52
    assert raw[:4] == bytes([0xF7, 0xD1, 1, 3])
    assert raw[4:8] == bytes([0x45, 0x23, 0x01, 0x00])  # pc
    assert raw[32:39] == b"app_wq\0"
    assert int.from_bytes(raw[48:], "little") == zlib.crc32(raw[:48])
    assert diag.unpack_fault(raw) == sample()


def test_unpack_refuses_bad_records():
    raw = bytearray(diag.pack_fault(sample()))
    with pytest.raises(ValueError, match = "52"):
        diag.unpack_fault(bytes(raw[:-1]))
    with pytest.raises(ValueError, match = "not a fault record"):
        diag.unpack_fault(bytes(52))
    bad = bytearray(raw)
    bad[2] = 2  # another version
    with pytest.raises(ValueError, match = "not a fault record"):
        diag.unpack_fault(bytes(bad))
    for i in range(4, 52):  # every flipped byte is found
        bad = bytearray(raw)
        bad[i] ^= 0x10
        with pytest.raises(ValueError, match = "CRC"):
            diag.unpack_fault(bytes(bad))


def test_thread_name_is_cut_and_filtered():
    f = sample()
    f.thread = "a" * 40
    assert len(diag.unpack_fault(diag.pack_fault(f)).thread) == 15
    f.thread = "x\ny"
    assert diag.unpack_fault(diag.pack_fault(f)).thread == "x?y"


def _arm_tool(name):
    """A tool of the toolchain that addr2line comes from (arm-zephyr-eabi-gcc next to arm-zephyr-eabi-addr2line)."""
    a2l = diag.find_addr2line()
    if a2l is None:
        return None
    p = Path(a2l)
    cand = p.with_name(p.name.replace("addr2line", name))
    return str(cand) if cand.exists() else None


@pytest.mark.skipif(_arm_tool("gcc") is None, reason = "needs an ARM GCC toolchain next to addr2line")
def test_addr2line_and_elf_lookup(tmp_path):
    """Build a tiny Cortex-M ELF with debug info, look up its addresses, and find it by the id of a signed image."""
    gcc, nm_tool, tool = _arm_tool("gcc"), _arm_tool("nm"), diag.find_addr2line()
    src = tmp_path / "f.c"
    src.write_text(
        textwrap.dedent("""
            int __attribute__((noinline)) crash_here(int x) { return x * 3 + 1; }
            int main(void) { return crash_here(9); }
            """))
    ld = tmp_path / "f.ld"
    ld.write_text("ENTRY(main) SECTIONS { . = 0x1000; .text : { *(.text*) } }")
    build = tmp_path / "build" / "nrf5340" / "zephyr"
    build.mkdir(parents = True)
    elf = build / "zephyr.elf"
    subprocess.run(
        [
            gcc,
            "-g",
            "-O0",
            "-mcpu=cortex-m33",
            "-mthumb",
            "-nostdlib",
            "-nostartfiles",
            "-T",
            str(ld),
            "-o",
            str(elf),
            str(src),
        ],
        check = True,
    )
    nm = subprocess.run([nm_tool, str(elf)], capture_output = True, text = True, check = True).stdout
    addr = int(next(line.split()[0] for line in nm.splitlines() if line.endswith(" crash_here")), 16)
    out = diag.addr2line(elf, [addr + 4], tool)  # inside the function
    assert "crash_here" in out[0] and "f.c:" in out[0]
    assert diag.addr2line(elf, [(addr + 4) | 1], tool) == out  # the Thumb bit of an LR is dropped
    assert diag.addr2line(elf, [0x10], tool)[0].startswith("??")

    # the ELF is matched with the signed image next to it
    image = make_image((1, 2, 0, 0), size = 600, seed = 3)
    (build / "zephyr.signed.bin").write_bytes(image)
    image_id = int.from_bytes(smp.parse_image(image).hash[:4], "little")
    assert diag.find_elf(image_id, [tmp_path / "build"]) == elf
    assert diag.find_elf(image_id ^ 1, [tmp_path / "build"]) is None
    assert diag.find_elf(0, [tmp_path / "build"]) is None
    # and the lines of `status` use it
    f = diag.Fault(reason = 3, pc = addr + 4, lr = addr + 5, thread = "main", image_id = image_id)
    lines = diag.fault_lines(f, elf, tool)
    assert "crash_here" in lines[2] and "crash_here" in lines[3]


def test_find_elf_without_image_files(tmp_path):
    (tmp_path / "zephyr.elf").write_bytes(b"\x7fELF")
    assert diag.find_elf(0x1234, [tmp_path]) is None  # nothing to match the id with


def test_fault_lines_without_addr2line(monkeypatch):
    monkeypatch.setattr(diag, "find_addr2line", lambda: None)
    lines = diag.fault_lines(diag.Fault(reason = 3, pc = 0x100, lr = 0x101, image_id = 1), "x.elf")
    assert any("addr2line failed" in line for line in lines) and any("pc 0x00000100" in line for line in lines)


def test_job_names_come_from_the_application():
    assert diag.job_name(7) == "job 7"
    assert diag.job_name(7, {7: "cycle"}) == "cycle" and diag.job_name(8, {7: "cycle"}) == "job 8"
    f = diag.Fault(reason = 3, last_job = 7, thread = "main")
    assert "job 7" in diag.fault_summary(f) and "job cycle" in diag.fault_summary(f, {7: "cycle"})
    assert "job cycle" in diag.fault_lines(f, job_names = {7: "cycle"})[1]
