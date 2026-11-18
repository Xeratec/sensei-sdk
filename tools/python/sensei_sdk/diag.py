# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Field diagnostics on the host: the reset cause and the fault record of a device, and the addresses of a fault as
source lines.

The names and the record are those of lib/sensei_diag (`sensei_diag.h`, message `FaultRecord` of types.proto).
`describe` and `restart_text` read the fields `reset_cause`, `boot_count`, `shield_reset_cause` and `fault` of the
application's status message. `addr2line` runs on the ELF of the image that faulted: the record carries `image_id`
(first 4 bytes of the image hash), and `find_elf` looks for the build whose signed image has that id. Without an ELF
the plain addresses are shown. A build without OTA has no image hash: its `image_id` is the git hash, `find_elf` does
not find a build for it, and the ELF is given by hand.

The job ids of `Fault.last_job` are the application's: the functions that print them take a `job_names` table.
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import zlib
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

# Reset cause bits: 0 to 16 are the RESET_* flags of the Zephyr hwinfo API, 24 and 25 are added by the module.
RESET_NAMES = {
    0: "PIN",
    1: "SOFT",
    2: "BROWNOUT",
    3: "POR",
    4: "WDT",
    5: "DEBUG",
    6: "SECURITY",
    7: "WAKE",
    8: "LOCKUP",
    9: "PARITY",
    10: "PLL",
    11: "CLOCK",
    12: "HARDWARE",
    13: "USER",
    14: "TEMP",
    15: "BOOTLOADER",
    16: "FLASH",
    24: "FAULT",
    25: "UNKNOWN",
}
RESET_SOFTWARE = 1 << 1
RESET_POR = 1 << 3
RESET_WATCHDOG = 1 << 4
RESET_FAULT = 1 << 24

# Reasons: K_ERR_* of Zephyr, K_ERR_ARM_* (Cortex-M, from 16, as in Zephyr 4.4), 0x80 the nRF91 modem core.
_ARM_REASONS = (
    "mem-generic mem-stacking mem-unstacking mem-data-access mem-instruction-access mem-fp-lazy "
    "bus-generic bus-stacking bus-unstacking bus-precise-data bus-imprecise-data bus-instruction bus-fp-lazy "
    "usage-generic usage-div-0 usage-unaligned usage-stack-overflow usage-no-coprocessor usage-illegal-exc-return "
    "usage-illegal-epsr usage-undefined-instruction secure-generic secure-entry-point secure-integrity-signature "
    "secure-exception-return secure-attribution-unit secure-transition secure-fp-lazy secure-lazy-state-error").split()
REASON_NAMES = {0: "exception", 1: "spurious-irq", 2: "stack", 3: "oops", 4: "panic", 0x80: "modem"}
REASON_NAMES.update({16 + i: n for i, n in enumerate(_ARM_REASONS)})


def reset_names(cause: int) -> list[str]:
    """Names of the set bits of a reset cause, lowest bit first. Bits without a name are left out."""
    return [name for bit, name in sorted(RESET_NAMES.items()) if cause & (1 << bit)]


def reset_text(cause: int) -> str:
    """ "POR", "SOFT|FAULT", "-" for none: the same text as the node's `sensei diag`."""
    return "|".join(reset_names(cause)) or "-"


def reason_name(reason: int) -> str:
    if reason in REASON_NAMES:
        return REASON_NAMES[reason]
    return "arch" if 16 <= reason < 0x80 else "unknown"


def job_name(job: int, job_names: Mapping[int, str] | None = None) -> str:
    """Name of a job id from the application's table, "job N" without one."""
    return (job_names or {}).get(job, f"job {job}")


@dataclass
class Fault:
    """The record of the last fatal error of a node (message FaultRecord)."""

    reason: int = 0
    pc: int = 0
    lr: int = 0
    thread: str = ""
    uptime_s: int = 0
    last_job: int = 0
    image_id: int = 0
    boot_count: int = 0
    status: int = 0  # Cortex-M: stacked xPSR, bits 0 to 8 the interrupt number
    coredump: bool = False

    @classmethod
    def from_pb(cls, msg) -> Fault:
        return cls(
            reason = msg.reason,
            pc = msg.pc,
            lr = msg.lr,
            thread = msg.thread,
            uptime_s = msg.uptime_s,
            last_job = msg.last_job,
            image_id = msg.image_id,
            boot_count = msg.boot_count,
            status = msg.status,
            coredump = msg.coredump,
        )

    @property
    def reason_name(self) -> str:
        return reason_name(self.reason)

    @property
    def in_interrupt(self) -> int:
        """Number of the interrupt that the fault hit (Cortex-M xPSR), 0 in a thread."""
        return self.status & 0x1FF


# The packed record (settings, retained RAM): little endian, magic 0xD1F7, version 1, CRC-32 at the end.
FAULT_BYTES = 52
_FAULT = struct.Struct("<HBBIIIIIIHH16s")  # up to and without the CRC: 48 bytes
FAULT_MAGIC = 0xD1F7
FAULT_VERSION = 1


def pack_fault(f: Fault) -> bytes:
    """The byte form of lib/sensei_diag (sensei_diag_fault_pack). Used by the tests; the node writes it."""
    name = bytes(c if 0x20 <= c < 0x7F else 0x3F for c in f.thread.encode("ascii", "replace")[:15])
    body = _FAULT.pack(
        FAULT_MAGIC,
        FAULT_VERSION,
        f.reason & 0xFF,
        f.pc,
        f.lr,
        f.status,
        f.uptime_s,
        f.image_id,
        f.boot_count,
        f.last_job,
        1 if f.coredump else 0,
        name,
    )
    return body + struct.pack("<I", zlib.crc32(body))


def unpack_fault(raw: bytes) -> Fault:
    """Read a packed record, for example a dump of the retained RAM. Raises ValueError if it is not a valid one."""
    if len(raw) != FAULT_BYTES:
        raise ValueError(f"a fault record has {FAULT_BYTES} bytes, got {len(raw)}")
    magic, version, reason, pc, lr, status, uptime, image_id, boots, job, flags, name = _FAULT.unpack_from(raw)
    if magic != FAULT_MAGIC or version != FAULT_VERSION:
        raise ValueError("not a fault record")
    if struct.unpack_from("<I", raw, 48)[0] != zlib.crc32(raw[:48]):
        raise ValueError("fault record damaged (CRC)")
    return Fault(
        reason = reason,
        pc = pc,
        lr = lr,
        thread = name.split(b"\0")[0].decode("ascii", "replace"),
        uptime_s = uptime,
        last_job = job,
        image_id = image_id,
        boot_count = boots,
        status = status,
        coredump = bool(flags & 1),
    )


# --- symbols -------------------------------------------------------------------------------------------
def find_addr2line() -> str | None:
    """addr2line of the Zephyr SDK or an ARM toolchain: $SENSEI_ADDR2LINE, then $ZEPHYR_SDK_INSTALL_DIR, then PATH."""
    env = os.environ.get("SENSEI_ADDR2LINE")
    if env:
        return env
    sdk = os.environ.get("ZEPHYR_SDK_INSTALL_DIR")
    if sdk:
        cand = Path(sdk) / "gnu" / "arm-zephyr-eabi" / "bin" / "arm-zephyr-eabi-addr2line"
        if cand.exists():
            return str(cand)
    for name in ("arm-zephyr-eabi-addr2line", "arm-none-eabi-addr2line", "llvm-addr2line"):
        found = shutil.which(name)
        if found:
            return found
    return None


def addr2line(elf: str | os.PathLike, addresses: list[int], tool: str | None = None) -> list[str]:
    """Function and source line of each address ("name at file:line"). Bit 0 (Thumb) is cleared. An address that
    the ELF does not know gives "??". Raises OSError if there is no addr2line."""
    tool = tool or find_addr2line()
    if tool is None:
        raise OSError("no addr2line found (set SENSEI_ADDR2LINE or ZEPHYR_SDK_INSTALL_DIR)")
    cmd = [tool, "-f", "-C", "-e", str(elf), *[f"0x{a & ~1:08x}" for a in addresses]]
    out = subprocess.run(cmd, capture_output = True, text = True, check = True, timeout = 30).stdout.splitlines()
    # two lines per address: function, then file:line
    return [f"{out[2 * i]} at {out[2 * i + 1]}" for i in range(len(addresses)) if 2 * i + 1 < len(out)]


def image_id_of(signed_bin: str | os.PathLike) -> int | None:
    """Id (first 4 bytes of the SHA-256 TLV, little endian) of a signed MCUboot image, None if it is not one."""
    from . import smp

    try:
        info = smp.parse_image(Path(signed_bin).read_bytes())
    except (OSError, smp.SmpError):
        return None
    return int.from_bytes(info.hash[:4], "little")


def find_elf(image_id: int, roots: list[str | os.PathLike]) -> Path | None:
    """The zephyr.elf of the build whose signed image (zephyr.signed.bin or *.signed.bin next to it) has this id.
    `roots` are build folders or folders that hold several builds. Without an image file the ELF cannot be matched."""
    if not image_id:
        return None
    for root in roots:
        root = Path(root)
        elfs = [root] if root.is_file() else sorted(root.rglob("*.elf"))
        for elf in elfs:
            for img in sorted(elf.parent.glob("*.signed.bin")):
                if image_id_of(img) == image_id:
                    return elf
    return None


# --- text ----------------------------------------------------------------------------------------------
def describe(status) -> list[str]:
    """One line for the reset cause and boot count of a Status message, "" if it carries neither."""
    parts = []
    if status.reset_cause:
        parts.append(f"reset {reset_text(status.reset_cause)}")
    if status.boot_count:
        parts.append(f"boot {status.boot_count}")
    if status.shield_reset_cause:
        parts.append(f"shield reset {reset_text(status.shield_reset_cause)}")
    return parts


def restart_text(status) -> str:
    """One line for the GUI: the reset cause, the boot count and, if the device reports one, the last fatal error.
    "-" for a device that reports none of it."""
    parts = diag_parts = describe(status)
    if status.HasField("fault"):
        f = Fault.from_pb(status.fault)
        where = f"interrupt {f.in_interrupt}" if f.in_interrupt else f.thread
        parts = [*diag_parts, f"fault {f.reason_name} in {where} at 0x{f.pc:08x}"]
    return ", ".join(parts) or "-"


def fault_summary(f: Fault, job_names: Mapping[int, str] | None = None) -> str:
    """One line: for the GUI and logs."""
    where = f"interrupt {f.in_interrupt}" if f.in_interrupt else f"thread {f.thread}"
    return (f"fatal error {f.reason_name} in {where}, pc 0x{f.pc:08x}, job {job_name(f.last_job, job_names)}, "
            f"uptime {f.uptime_s} s, boot {f.boot_count}")


def fault_lines(
    f: Fault,
    elf: str | os.PathLike | None = None,
    tool: str | None = None,
    job_names: Mapping[int, str] | None = None,
) -> list[str]:
    """Several lines for a status command: the record, and with an ELF the source lines of PC and LR."""
    lines = [
        f"last fatal error: {f.reason_name} (reason {f.reason}), " +
        (f"interrupt {f.in_interrupt}" if f.in_interrupt else f"thread {f.thread}"),
        f"  job {job_name(f.last_job, job_names)}, uptime {f.uptime_s} s, boot {f.boot_count}, image {f.image_id:08x}" +
        (", coredump kept on the node" if f.coredump else ""),
    ]
    symbols: list[str] | None = None
    if elf is not None:
        try:
            symbols = addr2line(elf, [f.pc, f.lr], tool)
        except (OSError, subprocess.SubprocessError) as exc:
            lines.append(f"  (addr2line failed: {exc})")
    lines.append(f"  pc 0x{f.pc:08x}" + (f"  {symbols[0]}" if symbols else ""))
    lines.append(f"  lr 0x{f.lr:08x}" + (f"  {symbols[1]}" if symbols and len(symbols) > 1 else ""))
    if elf is None and f.image_id:
        lines.append(f"  no ELF given for image {f.image_id:08x} (--elf-dir <build folders>): plain addresses only")
    return lines
