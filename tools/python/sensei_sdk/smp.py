# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""SMP (mcumgr) client for MCUboot image updates: upload, test, confirm, reset.

Only the small subset needed for updates is implemented (no `smpclient` dependency, which would pull in
pydantic and its own BLE and serial stacks next to the ones used here):

- OS group: echo, reset.
- Image group: state read, state write (test, confirm), upload, erase.
- Links: USB serial (mcumgr line framing: base64, CRC-16, 0x0609 / 0x0414 line prefixes) and the BLE SMP
  characteristic (whole packets, notifications may be fragmented).

The device side is MCUboot with mcumgr (`NRF/system/ota`); `smp_sim.SimSmpServer` stands in for it in tests.
"""

from __future__ import annotations

import abc
import asyncio
import base64
import hashlib
import io
import json
import struct
import zipfile
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from pathlib import Path

# --- constants ---------------------------------------------------------------------------------------
SMP_SERVICE = "8d53dc1d-1db7-4cd3-868b-8a527460aa84"
SMP_CHAR = "da2e7828-fbce-4e01-ae9e-261174997c48"  # write without response, notify

OP_READ, OP_READ_RSP, OP_WRITE, OP_WRITE_RSP = 0, 1, 2, 3
GROUP_OS, GROUP_IMAGE = 0, 1
OS_ECHO, OS_RESET = 0, 5
BOOT_MODE_BOOTLOADER = 1  # os reset with this boot_mode: MCUboot stays in serial recovery (BOOT_MODE_TYPE_BOOTLOADER)
IMG_STATE, IMG_UPLOAD, IMG_ERASE = 0, 1, 5
HEADER = struct.Struct(">BBHHBB")  # op, flags, length, group, sequence, command id

MGMT_ERR_EINVAL = 3
MGMT_ERR_ENOENT = 5

# mcumgr error codes (zephyr/mgmt/mcumgr/mgmt/mgmt.h) and the image group's own codes (img_mgmt.h)
MGMT_ERRORS = (
    "ok",
    "unknown error",
    "out of memory",
    "invalid value",
    "timeout",
    "no such entry",
    "bad state",
    "response too long",
    "not supported",
    "corrupt",
    "busy",
    "access denied",
    "protocol too old",
    "protocol too new",
)
IMG_ERRORS = (
    "ok",
    "unknown error",
    "flash configuration query failed",
    "no image",
    "no TLVs",
    "invalid TLV",
    "several hashes in the image",
    "TLV of invalid size",
    "hash not found",
    "no free slot (the running image is still on trial, or a test image is pending)",
    "flash open failed",
    "flash read failed",
    "flash write failed",
    "flash erase failed",
    "invalid slot",
    "out of memory",
    "flash context already set",
    "flash context not set",
    "flash device missing",
    "invalid page offset",
    "invalid offset",
    "invalid length",
    "invalid image header",
    "invalid image header magic",
    "invalid hash",
    "invalid flash address",
    "version read failed",
    "refused by the version rule (an older image, or a test or confirm while slot 1 holds an older image)",
    "image already pending",
    "invalid image vector table",
    "image too large for the slot",
    "image data overrun",
    "confirmation denied",
    "test of the active image denied",
    "active slot not known",
)


def describe_rc(rc: int, group: int | None = None) -> str:
    """Words for an SMP return code. `group` is set for SMP v2 errors (1 = image group)."""
    table = IMG_ERRORS if group == GROUP_IMAGE else MGMT_ERRORS
    name = table[rc] if 0 <= rc < len(table) else f"code {rc}"
    return f"{name}, image group" if group == GROUP_IMAGE else name


IMAGE_MAGIC = 0x96F3B83D
TLV_INFO_MAGIC = 0x6907
TLV_PROT_INFO_MAGIC = 0x6908
TLV_SHA256 = 0x10
# Protected custom TLV that says which board an image is for (imgtool --custom-tlv 0xa0 <kind>, see sysbuild.cmake of
# the shield app). The signature covers it. The kind of an application is its own: the functions that check it take it
# as a parameter.
TLV_KIND = 0x00A0
KIND_SHIELD = "sensei-lpwan-nrf9151"  # NRF/apps/lpwan_shield


class SmpError(Exception):
    pass


class SmpTimeout(SmpError):
    pass


class SmpRcError(SmpError):
    """The node answered with a non-zero return code. `group` is set for SMP v2 errors."""

    def __init__(self, rc: int, group: int | None = None):
        super().__init__(f"SMP error {rc}: {describe_rc(rc, group)}")
        self.rc = rc
        self.group = group


# --- minimal CBOR (maps, arrays, ints, bytes, text, bool, null) -----------------------------------------
def _head(major: int, n: int) -> bytes:
    if n < 24:
        return bytes([major << 5 | n])
    for ai, fmt in ((24, ">B"), (25, ">H"), (26, ">I"), (27, ">Q")):
        if n < 1 << (8 * struct.calcsize(fmt)):
            return bytes([major << 5 | ai]) + struct.pack(fmt, n)
    raise SmpError("integer too large for CBOR")


def cbor_encode(obj) -> bytes:
    if obj is None:
        return b"\xf6"
    if obj is True:
        return b"\xf5"
    if obj is False:
        return b"\xf4"
    if isinstance(obj, int):
        return _head(0, obj) if obj >= 0 else _head(1, -1 - obj)
    if isinstance(obj, (bytes, bytearray)):
        return _head(2, len(obj)) + bytes(obj)
    if isinstance(obj, str):
        raw = obj.encode()
        return _head(3, len(raw)) + raw
    if isinstance(obj, (list, tuple)):
        return _head(4, len(obj)) + b"".join(cbor_encode(x) for x in obj)
    if isinstance(obj, dict):
        return _head(5, len(obj)) + b"".join(cbor_encode(k) + cbor_encode(v) for k, v in obj.items())
    raise SmpError(f"cannot encode {type(obj).__name__}")


def cbor_decode(data: bytes):
    obj, end = _decode(memoryview(data), 0)
    if end != len(data):
        raise SmpError("trailing bytes after CBOR item")
    return obj


_BREAK = object()


def _decode(buf: memoryview, i: int):
    try:
        ib = buf[i]
        major, ai = ib >> 5, ib & 31
        i += 1
        if ai < 24:
            n = ai
        elif ai in (24, 25, 26, 27):
            size = 1 << (ai - 24)
            n = int.from_bytes(buf[i:i + size], "big")
            if len(buf[i:i + size]) != size:
                raise SmpError("truncated CBOR")
            i += size
        elif ai == 31:
            n = None  # indefinite length (arrays, maps) or break
        else:
            raise SmpError(f"unsupported CBOR item {ib:#x}")
        if major == 0:
            return n, i
        if major == 1:
            return -1 - n, i
        if major in (2, 3):
            if n is None or i + n > len(buf):
                raise SmpError("bad CBOR string")
            raw = bytes(buf[i:i + n])
            return (raw if major == 2 else raw.decode()), i + n
        if major == 4:
            out = []
            while n is None or len(out) < n:
                if n is None and buf[i] == 0xFF:
                    return out, i + 1
                item, i = _decode(buf, i)
                out.append(item)
            return out, i
        if major == 5:
            out = {}
            count = 0
            while n is None or count < n:
                if n is None and buf[i] == 0xFF:
                    return out, i + 1
                k, i = _decode(buf, i)
                v, i = _decode(buf, i)
                out[k] = v
                count += 1
            return out, i
        if major == 7:
            if ai == 20:
                return False, i
            if ai == 21:
                return True, i
            if ai in (22, 23):
                return None, i
        raise SmpError(f"unsupported CBOR item {ib:#x}")
    except IndexError:
        raise SmpError("truncated CBOR") from None


# --- packets -------------------------------------------------------------------------------------------
def encode_packet(op: int, group: int, cmd: int, seq: int, body: dict) -> bytes:
    payload = cbor_encode(body)
    return HEADER.pack(op, 0, len(payload), group, seq, cmd) + payload


@dataclass
class Packet:
    op: int
    group: int
    cmd: int
    seq: int
    body: dict


def decode_packet(data: bytes) -> Packet:
    if len(data) < HEADER.size:
        raise SmpError("short SMP packet")
    op, _flags, length, group, seq, cmd = HEADER.unpack_from(data)
    payload = data[HEADER.size:HEADER.size + length]
    if len(payload) != length:
        raise SmpError("truncated SMP packet")
    body = cbor_decode(payload) if payload else {}
    if not isinstance(body, dict):
        raise SmpError("SMP body is not a map")
    return Packet(op & 7, group, cmd, seq, body)


def packet_length(buf: bytes) -> int | None:
    """Total length of the packet that starts in `buf`, or None while the header is incomplete."""
    if len(buf) < HEADER.size:
        return None
    return HEADER.size + struct.unpack_from(">H", buf, 2)[0]


# --- serial framing ------------------------------------------------------------------------------------
SERIAL_FIRST = b"\x06\x09"
SERIAL_NEXT = b"\x04\x14"
SERIAL_LINE_CHARS = 124  # base64 characters per line (line = 2 + 124 + newline = 127 B)


def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def serial_encode(packet: bytes) -> bytes:
    """SMP packet as mcumgr serial lines."""
    raw = struct.pack(">H", len(packet) + 2) + packet + struct.pack(">H", crc16_xmodem(packet))
    text = base64.b64encode(raw)
    lines = []
    for i in range(0, len(text), SERIAL_LINE_CHARS):
        lines.append((SERIAL_FIRST if i == 0 else SERIAL_NEXT) + text[i:i + SERIAL_LINE_CHARS] + b"\n")
    return b"".join(lines)


class SerialDecoder:
    """Collects mcumgr serial lines out of a byte stream (other bytes are ignored) and returns packets."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self._text = bytearray()
        self.n_errors = 0

    def feed(self, data: bytes) -> list[bytes]:
        out = []
        self._buf += data
        while True:
            nl = self._buf.find(b"\n")
            if nl < 0:
                if len(self._buf) > 4096:
                    self._buf.clear()
                break
            line = bytes(self._buf[:nl])
            del self._buf[:nl + 1]
            start = max(line.rfind(SERIAL_FIRST), line.rfind(SERIAL_NEXT))
            if start < 0:
                continue
            line = line[start:]
            if line.startswith(SERIAL_FIRST):
                self._text.clear()
            elif not self._text:
                self.n_errors += 1  # continuation without a start
                continue
            self._text += line[2:].strip()
            pkt = self._try_packet()
            if pkt is not None:
                out.append(pkt)
        return out

    def _try_packet(self) -> bytes | None:
        try:
            raw = base64.b64decode(bytes(self._text) + b"=" * (-len(self._text) % 4), validate = True)
        except ValueError:
            return None
        if len(raw) < 2:
            return None
        total = struct.unpack_from(">H", raw)[0]
        if len(raw) - 2 < total:
            return None  # more lines to come
        self._text.clear()
        body = raw[2:2 + total]
        packet, crc = body[:-2], struct.unpack(">H", body[-2:])[0]
        if crc16_xmodem(packet) != crc:
            self.n_errors += 1
            return None
        return packet


# --- links ---------------------------------------------------------------------------------------------
class SmpLink(abc.ABC):
    """One SMP packet in, one out. `mtu` is the largest packet the node takes in one write."""

    mtu = 244

    def __init__(self) -> None:
        self.on_packet: Callable[[bytes], None] = lambda p: None

    @abc.abstractmethod
    async def open(self) -> None:
        ...

    @abc.abstractmethod
    async def close(self) -> None:
        ...

    @abc.abstractmethod
    async def send(self, packet: bytes) -> None:
        ...


class CallbackSmpLink(SmpLink):
    """Packet link over a write function. Received bytes go to `feed()`; fragments are joined by the SMP
    header length. Used for the BLE characteristic and for the packet-mode simulator."""

    def __init__(
        self,
        write: Callable[[bytes], Awaitable[None]],
        mtu: int = 244,
        attach: Callable[[Callable[[bytes], None]], Callable[[], None] | None] | None = None,
    ) -> None:
        """attach(feed) registers the receive path and may return a function that removes it."""
        super().__init__()
        self._write, self.mtu, self._attach = write, mtu, attach
        self._rx = bytearray()
        self._unhook: Callable[[], None] = lambda: None

    async def open(self) -> None:
        if self._attach is not None:
            self._unhook = self._attach(self.feed) or (lambda: None)

    async def close(self) -> None:
        self._unhook()
        self._unhook = lambda: None

    def feed(self, data: bytes) -> None:
        self._rx += data
        while True:
            n = packet_length(bytes(self._rx))
            if n is None or len(self._rx) < n:
                return
            pkt = bytes(self._rx[:n])
            del self._rx[:n]
            self.on_packet(pkt)

    async def send(self, packet: bytes) -> None:
        await self._write(packet)


class SerialSmpLink(SmpLink):
    """SMP over a byte transport (USB CDC). It takes over `transport.on_data` while open, so the node's
    stream must be off during the update."""

    mtu = 256

    def __init__(self, transport) -> None:
        super().__init__()
        self.transport = transport
        self._dec = SerialDecoder()
        self._saved = None

    async def open(self) -> None:
        self._saved = self.transport.on_data
        self.transport.on_data = self._on_bytes

    async def close(self) -> None:
        if self.transport.on_data == self._on_bytes:
            self.transport.on_data = self._saved

    def _on_bytes(self, data: bytes) -> None:
        for pkt in self._dec.feed(data):
            self.on_packet(pkt)

    async def send(self, packet: bytes) -> None:
        await self.transport.write(serial_encode(packet))


class BleSmpLink(SmpLink):
    """SMP over the GATT SMP characteristic of an open `BleTransport`."""

    def __init__(self, transport) -> None:
        super().__init__()
        self.transport = transport
        self._rx = CallbackSmpLink(self._write)
        self._rx.on_packet = lambda p: self.on_packet(p)

    @property
    def _client(self):
        return self.transport._client

    async def open(self) -> None:
        if self._client is None or not self.transport.is_open:
            raise SmpError("BLE not connected")
        self.mtu = self.transport.payload_size
        try:
            await self._client.start_notify(SMP_CHAR, lambda _h, d: self._rx.feed(bytes(d)))
        except Exception as exc:
            # Release builds need a paired link: pair once (not supported on every platform), then retry.
            try:
                await self._client.pair()
                await self._client.start_notify(SMP_CHAR, lambda _h, d: self._rx.feed(bytes(d)))
            except Exception:
                raise SmpError(f"no SMP characteristic (pairing needed in Release builds?): {exc}") from exc

    async def close(self) -> None:
        try:
            await self._client.stop_notify(SMP_CHAR)
        except Exception:
            pass

    async def _write(self, data: bytes) -> None:
        await self.transport._gatt("SMP write", self._client.write_gatt_char(SMP_CHAR, data, response = False))

    async def send(self, packet: bytes) -> None:
        await self._write(packet)


# --- MCUboot images ------------------------------------------------------------------------------------
@dataclass
class ImageInfo:
    version: tuple[int, int, int, int]  # major, minor, revision, build
    hash: bytes  # SHA-256 TLV, the id MCUboot and mcumgr use
    size: int  # whole file
    kind: str | None = None  # protected TLV 0xa0: the board the image is for, None if the image has none

    @property
    def version_str(self) -> str:
        v = self.version
        return ".".join(str(x) for x in (v[:3] if v[3] == 0 else v))

    @property
    def version3(self) -> tuple[int, int, int]:
        """major.minor.revision: what MCUboot's downgrade check and the node's Status compare."""
        return self.version[:3]


def parse_version(text: str) -> tuple[int, int, int]:
    """major.minor.revision of an mcumgr version string ("1.2.3" or "1.2.3.4"); (0, 0, 0) if unreadable."""
    try:
        parts = [int(x) for x in text.split("+")[0].split(".")]
    except ValueError:
        return (0, 0, 0)
    return tuple(([*parts, 0, 0, 0])[:3])  # type: ignore[return-value]


def is_downgrade(running: str, image: ImageInfo) -> bool:
    """True if `image` is older than the running version (major.minor.revision, as MCUboot compares): MCUboot
    would erase it at the reset."""
    return parse_version(running) > image.version3


def load_image(source: str | Path | bytes, kind: str | None = None) -> bytes:
    """A signed MCUboot image from bytes, a `.bin` file or the NCS `dfu_application.zip` (its application
    image). Raises SmpError if it is not an MCUboot image, or if it is made for another board: `kind` is the board
    that takes it (the board id of the application, for example `KIND_SHIELD`). An image without a kind TLV is let
    through (an older build, or an image of another product); one with another kind is refused. kind=None (the
    default): no check."""
    if isinstance(source, (bytes, bytearray)):
        data = bytes(source)
    else:
        path = Path(source)
        try:
            data = path.read_bytes()
        except OSError as exc:
            raise SmpError(f"cannot read {path}: {exc}") from exc
    if data[:2] == b"PK":  # NCS DFU package
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                names = z.namelist()
                pick = None
                if "manifest.json" in names:
                    for f in json.loads(z.read("manifest.json")).get("files", []):
                        if f.get("type") == "application" and str(f.get("image_index", "0")) == "0":
                            pick = f.get("file")
                            break
                pick = pick or next((n for n in names if n.endswith(".bin")), None)
                if pick is None or pick not in names:
                    raise SmpError("no application image in the DFU package")
                data = z.read(pick)
        except (zipfile.BadZipFile, ValueError, KeyError) as exc:
            raise SmpError(f"bad DFU package: {exc}") from exc
    info = parse_image(data)
    if kind is not None and info.kind is not None and info.kind != kind:
        raise SmpError(f"this image is for '{info.kind}', not for '{kind}'")
    return data


def parse_image(data: bytes) -> ImageInfo:
    """Header, hash TLV and kind TLV of a signed MCUboot image (.bin). Raises SmpError if it is not one."""
    if len(data) < 32:
        raise SmpError("too short for an MCUboot image")
    magic, _load, hdr_size, _prot_size, img_size, _flags = struct.unpack_from("<IIHHII", data)
    major, minor, revision, build = struct.unpack_from("<BBHI", data, 20)
    if magic != IMAGE_MAGIC:
        raise SmpError("not an MCUboot image (bad magic; use the signed app_update.bin)")
    off = hdr_size + img_size
    if off + 4 > len(data):
        raise SmpError("image is truncated")
    end = None
    pos = off
    sha = None
    kind = None
    # protected TLVs come first (if any), then the normal ones. Only the protected area counts for the kind: the
    # signature does not cover the other one.
    for _ in range(2):
        if pos + 4 > len(data):
            break
        m, tlv_total = struct.unpack_from("<HH", data, pos)
        if m not in (TLV_INFO_MAGIC, TLV_PROT_INFO_MAGIC):
            raise SmpError("no TLV area after the image")
        end = pos + tlv_total
        p = pos + 4
        while p + 4 <= min(end, len(data)):
            typ, ln = struct.unpack_from("<HH", data, p)
            if typ == TLV_SHA256 and ln == 32:
                sha = bytes(data[p + 4:p + 4 + 32])
            elif typ == TLV_KIND and m == TLV_PROT_INFO_MAGIC:
                kind = bytes(data[p + 4:p + 4 + ln]).decode("ascii", "replace")
            p += 4 + ln
        pos = end
        if m == TLV_INFO_MAGIC:
            break
    if sha is None:
        raise SmpError("image has no SHA-256 TLV")
    if end is not None and end > len(data):
        raise SmpError("image is truncated")
    return ImageInfo((major, minor, revision, build), sha, len(data), kind)


# --- client --------------------------------------------------------------------------------------------
@dataclass
class ImageSlot:
    image: int
    slot: int
    version: str
    hash: bytes
    bootable: bool = True
    pending: bool = False
    confirmed: bool = False
    active: bool = False
    permanent: bool = False


Progress = Callable[[int, int], None]


class SmpClient:
    """Requests with sequence numbers, timeouts and retries over an `SmpLink`."""

    def __init__(self, link: SmpLink, timeout: float = 3.0, retries: int = 2) -> None:
        self.link = link
        self.timeout, self.retries = timeout, retries
        self._seq = 0
        self._pending: dict[int, asyncio.Future] = {}
        link.on_packet = self._on_packet

    async def __aenter__(self) -> SmpClient:
        await self.link.open()
        return self

    async def __aexit__(self, *exc) -> None:
        await self.link.close()

    def _on_packet(self, data: bytes) -> None:
        try:
            pkt = decode_packet(data)
        except SmpError:
            return
        fut = self._pending.get(pkt.seq)
        if fut is not None and not fut.done():
            fut.set_result(pkt)

    async def request(self, op: int, group: int, cmd: int, body: dict | None = None, timeout: float | None = None):
        """Send a request, return the response body (dict). Retries on timeouts."""
        timeout = self.timeout if timeout is None else timeout
        self._seq = (self._seq + 1) & 0xFF
        seq = self._seq
        packet = encode_packet(op, group, cmd, seq, body or {})
        if len(packet) > self.link.mtu:
            raise SmpError(f"packet of {len(packet)} B exceeds the link MTU {self.link.mtu}")
        fut = asyncio.get_running_loop().create_future()
        self._pending[seq] = fut
        try:
            for attempt in range(self.retries + 1):
                await self.link.send(packet)
                try:
                    rsp = await asyncio.wait_for(asyncio.shield(fut), timeout)
                    break
                except TimeoutError:
                    if attempt == self.retries:
                        raise SmpTimeout(f"no SMP response (group {group}, id {cmd}) in {timeout} s") from None
        finally:
            self._pending.pop(seq, None)
        body = rsp.body
        if isinstance(body.get("err"), dict):  # SMP v2 error
            raise SmpRcError(int(body["err"].get("rc", -1)), body["err"].get("group"))
        if body.get("rc", 0):
            raise SmpRcError(int(body["rc"]))
        return body

    # --- OS group ---------------------------------------------------------------------------------
    async def echo(self, text: str = "sensei") -> str:
        return (await self.request(OP_WRITE, GROUP_OS, OS_ECHO, {"d": text})).get("r", "")

    async def reset(self, boot_mode: int | None = None) -> None:
        """Reboot the node. The link usually drops before or right after the reply, so a missing reply is fine.
        `boot_mode` BOOT_MODE_BOOTLOADER: the bootloader stays in serial recovery (the node needs
        MCUMGR_GRP_OS_RESET_BOOT_MODE, the SENSEI node has it on the board). A reply (or none) does not say that the
        node took the mode: one that ignores it restarts into its application. The caller can only say "requested"."""
        body = {} if boot_mode is None else {"boot_mode": boot_mode}
        try:
            await self.request(OP_WRITE, GROUP_OS, OS_RESET, body, timeout = 1.0)
        except SmpTimeout:
            pass

    # --- image group ------------------------------------------------------------------------------
    @staticmethod
    def _slots(body: dict) -> list[ImageSlot]:
        out = []
        for im in body.get("images", []):
            out.append(
                ImageSlot(
                    image = int(im.get("image", 0)),
                    slot = int(im.get("slot", 0)),
                    version = str(im.get("version", "")),
                    hash = bytes(im.get("hash", b"")),
                    bootable = bool(im.get("bootable", True)),
                    pending = bool(im.get("pending", False)),
                    confirmed = bool(im.get("confirmed", False)),
                    active = bool(im.get("active", False)),
                    permanent = bool(im.get("permanent", False)),
                ))
        return out

    async def image_state(self) -> list[ImageSlot]:
        return self._slots(await self.request(OP_READ, GROUP_IMAGE, IMG_STATE))

    async def test(self, image_hash: bytes) -> list[ImageSlot]:
        """Mark the image in the secondary slot for one trial boot; it reverts unless confirmed."""
        return self._slots(await self.request(OP_WRITE, GROUP_IMAGE, IMG_STATE, {"hash": image_hash, "confirm": False}))

    async def confirm(self, image_hash: bytes | None = None) -> list[ImageSlot]:
        """Make the running image permanent (no hash), or mark the given image permanent for the next boot."""
        body: dict = {"confirm": True}
        if image_hash is not None:
            body["hash"] = image_hash
        return self._slots(await self.request(OP_WRITE, GROUP_IMAGE, IMG_STATE, body))

    async def erase(self, slot: int = 1) -> None:
        await self.request(OP_WRITE, GROUP_IMAGE, IMG_ERASE, {"slot": slot}, timeout = 20.0)

    def _chunk_size(self, total: int) -> int:
        first = {"image": 0, "len": total, "off": 0, "sha": bytes(32), "data": b"", "upgrade": True}
        overhead = HEADER.size + len(cbor_encode(first)) + 3  # 3 B: the length head of the data bytes
        size = self.link.mtu - overhead
        if size < 16:
            raise SmpError(f"link MTU {self.link.mtu} is too small for an upload")
        return size

    async def upload(
        self,
        image: bytes,
        progress: Progress | None = None,
        image_index: int = 0,
        upgrade: bool = False,
        first_timeout: float = 30.0,
        chunk: int | None = None,
    ) -> None:
        """Send the image in chunks. The node's reported offset decides where to continue (resumes and
        resynchronises after lost packets). The first chunk may take long: the node erases the slot.

        `upgrade` is the "upgrade only" flag of the image group: the node then refuses an image whose version
        is not higher than the running one (rc 27), so a rebuild of the same version would fail. It is off by
        default: MCUboot's downgrade check at the reset still erases an older image."""
        total = len(image)
        chunk = chunk or self._chunk_size(total)
        sha = hashlib.sha256(image).digest()
        off = 0
        first = True
        stalls = 0
        while off < total:
            body: dict = {"off": off, "data": image[off:off + chunk]}
            if off == 0:
                body = {"image": image_index, "len": total, "off": 0, "sha": sha, **body, "upgrade": upgrade}
            rsp = await self.request(OP_WRITE,
                                     GROUP_IMAGE,
                                     IMG_UPLOAD,
                                     body,
                                     timeout = first_timeout if first else None)
            first = False
            new_off = int(rsp.get("off", off + len(body["data"])))
            if new_off <= off:
                stalls += 1
                if stalls > 5:
                    raise SmpError(f"upload stalled at offset {off}")
            else:
                stalls = 0
            off = new_off
            if progress:
                progress(min(off, total), total)


# --- command line ---------------------------------------------------------------------------------------
async def _cli(args, kind: str | None, ble_profile) -> int:
    from .transport_ble import BleProfile, BleTransport
    from .transport_serial import SerialTransport

    tr = SerialTransport(args.serial) if args.serial else BleTransport(args.ble, ble_profile or BleProfile())
    await tr.open()
    try:
        async with SmpClient(tr.smp_link(), timeout = args.timeout) as sc:
            if args.cmd == "list":
                for s in await sc.image_state():
                    flags = [n for n in ("active", "confirmed", "pending", "permanent") if getattr(s, n)]
                    print(f"image {s.image} slot {s.slot}  {s.version:<10} {s.hash.hex()[:16]}  {' '.join(flags)}")
            elif args.cmd == "upload":
                data = load_image(args.file, kind = args.kind or kind)
                info = parse_image(data)
                last = [-1]

                def show(done: int, total: int) -> None:
                    pct = 100 * done // total
                    if pct != last[0] and pct % 10 == 0:
                        last[0] = pct
                        print(f"upload {pct}%", flush = True)

                await sc.upload(data, progress = show)
                if args.test:
                    await sc.test(info.hash)
                    print(f"image {info.version_str} marked for a test boot: reset to run it")
                if args.reset:
                    await sc.reset()
            elif args.cmd == "test":
                await sc.test(bytes.fromhex(args.hash))
            elif args.cmd == "confirm":
                await sc.confirm()
            elif args.cmd == "reset":
                await sc.reset()
            elif args.cmd == "echo":
                print(await sc.echo("sensei"))
    finally:
        await tr.close()
    return 0


def main(argv: list[str] | None = None,
         *,
         kind: str | None = None,
         ble_profile = None,
         prog: str = "python -m sensei_sdk.smp") -> int:
    """Talk to MCUboot's serial recovery or to the SMP server of a device, without the application protocol.
    An application wraps it to give its own defaults: `kind` (board id that `upload` checks; `--kind` overrides it)
    and `ble_profile` (`transport_ble.BleProfile`; the SMP service alone needs none)."""
    import argparse

    ap = argparse.ArgumentParser(prog = prog, description = main.__doc__)
    link = ap.add_mutually_exclusive_group(required = True)
    link.add_argument("--serial", metavar = "PORT", help = "USB serial port of the node or of MCUboot")
    link.add_argument("--ble", metavar = "ADDRESS", help = "BLE address (or id on macOS) of the node")
    ap.add_argument("--timeout", type = float, default = 3.0, help = "seconds per request")
    sub = ap.add_subparsers(dest = "cmd", required = True)
    sub.add_parser("list", help = "images and their state")
    up = sub.add_parser("upload", help = "upload a signed image (.bin or dfu_application.zip)")
    up.add_argument("file")
    up.add_argument("--kind", help = "board id the image must be for (the kind TLV), default: no check")
    up.add_argument("--test", action = "store_true", help = "mark it for one trial boot")
    up.add_argument("--reset", action = "store_true", help = "reset the node afterwards")
    t = sub.add_parser("test", help = "mark the image with this SHA-256 (hex) for one trial boot")
    t.add_argument("hash")
    sub.add_parser("confirm", help = "make the running image permanent")
    sub.add_parser("reset", help = "reset the node")
    sub.add_parser("echo", help = "check that the SMP server answers")
    args = ap.parse_args(argv)
    try:
        return asyncio.run(_cli(args, kind, ble_profile))
    except (SmpError, OSError) as exc:
        print(f"error: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
