# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Framing of the SENSEI message layer: CRC, COBS stream frames, SPI frames and fragments.

Mirror of lib/sensei_link (C). The test vectors of `sensei_sdk.vectors` (lib/sensei_link/tests/generated/vectors.h)
check that both agree, and tests/test_sdk_constants.py checks the constants below against sensei_link.h.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field

# The constants below mirror lib/sensei_link/include/sensei_link/sensei_link.h
# (tests/test_sdk_constants.py checks them).
PROTO_VERSION = 2  # SENSEI_LINK_VERSION: the SPI frame CRC covers header bytes 0-5 and the payload

STREAM_MAX_MSG = 2144
SPI_MAX_PAYLOAD = 8192

MTU_LORA = 222
MTU_BLE = 244
MTU_LTE = 1024

# Channel byte of a stream frame (0x01-0x3F SDK, 0x40-0xFF app)
CH_HOST_TO_NODE = 0x01
CH_NODE_TO_HOST = 0x02
CH_UPLINK = 0x03
CH_DOWNLINK = 0x04
CH_UPLINK_CHUNK = 0x05
CH_DOWNLINK_CHUNK = 0x06
CH_SEALED = 0x07  # sealed envelope (lib/sensei_secure/README.md)

# Type byte of an SPI frame
SPI_GAP9_JOB = 0x10
SPI_GAP9_RESULT = 0x11
SPI_LPWAN_REQUEST = 0x20
SPI_LPWAN_RESPONSE = 0x21
SPI_LPWAN_EVENT = 0x22

SPI_MAGIC = 0x5E
SPI_HEADER_SIZE = 8
SPI_FLAG_ERROR = 0x01
SPI_FLAG_MORE = 0x02

FRAG_HEADER_SIZE = 3
ACK_SIZE = 3  # msg_id, index, 0: a chunk header with count 0, which no chunk has


class FramingError(ValueError):
    pass


def looks_sealed(payload: bytes) -> bool:
    """Envelope marker of lib/sensei_secure (`secure.is_sealed` without the crypto import): a plain protobuf message
    never starts with 0xAx, the version bits (3) make it 0xAC ... 0xAF."""
    return len(payload) >= 26 and (payload[0] & 0xFC) == 0xAC


# ---- CRC ----


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc32(data: bytes) -> int:
    """CRC-32 (IEEE), as zlib.crc32."""
    import zlib

    return zlib.crc32(data) & 0xFFFFFFFF


# ---- COBS ----


def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_pos = 0
    code = 1
    for b in data:
        if b != 0:
            out.append(b)
            code += 1
        if b == 0 or code == 0xFF:
            out[code_pos] = code
            code_pos = len(out)
            out.append(0)
            code = 1
    out[code_pos] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    if not data:
        raise FramingError("empty COBS input")
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        code = data[i]
        i += 1
        if code == 0 or code - 1 > n - i:
            raise FramingError("bad COBS code")
        block = data[i:i + code - 1]
        if 0 in block:
            raise FramingError("zero byte in COBS data")
        out += block
        i += code - 1
        if code != 0xFF and i < n:
            out.append(0)
    return bytes(out)


# ---- stream frames ----


def stream_encode(channel: int, payload: bytes) -> bytes:
    """channel | payload | crc16 (little endian), COBS encoded, 0x00 delimiter."""
    raw = bytes([channel]) + payload
    raw += struct.pack("<H", crc16(raw))
    return cobs_encode(raw) + b"\x00"


@dataclass
class StreamDecoder:
    """Feed received bytes, get (channel, payload) frames. Bad frames are counted and dropped."""

    max_frame: int = STREAM_MAX_MSG * 2
    n_frames: int = 0
    n_crc_err: int = 0
    n_format_err: int = 0
    n_overflow: int = 0
    _buf: bytearray = field(default_factory = bytearray)
    _overflow: bool = False

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        frames = []
        for b in data:
            if b != 0:
                if self._overflow:
                    continue
                if len(self._buf) >= self.max_frame:
                    self._overflow = True
                    continue
                self._buf.append(b)
                continue
            if self._overflow:
                self._overflow = False
                self._buf.clear()
                self.n_overflow += 1
                continue
            if not self._buf:
                continue
            enc = bytes(self._buf)
            self._buf.clear()
            try:
                raw = cobs_decode(enc)
            except FramingError:
                raw = b""
            if len(raw) < 3:
                self.n_format_err += 1
                continue
            if crc16(raw[:-2]) != struct.unpack("<H", raw[-2:])[0]:
                self.n_crc_err += 1
                continue
            self.n_frames += 1
            frames.append((raw[0], raw[1:-2]))
        return frames


# ---- SPI frames ----


def spi_encode(type_: int, payload: bytes, flags: int = 0) -> bytes:
    if len(payload) > SPI_MAX_PAYLOAD:
        raise FramingError("SPI payload too long")
    head = struct.pack("<BBBBH", SPI_MAGIC, PROTO_VERSION, type_, flags, len(payload))
    return head + struct.pack("<H", crc16(payload, crc16(head))) + payload


def spi_decode(frame: bytes) -> tuple[int, int, bytes]:
    """Returns (type, flags, payload). Extra bytes after the frame are ignored."""
    if len(frame) < SPI_HEADER_SIZE:
        raise FramingError("short SPI frame")
    magic, version, type_, flags, length, crc = struct.unpack("<BBBBHH", frame[:SPI_HEADER_SIZE])
    if magic != SPI_MAGIC:
        raise FramingError("bad SPI magic")
    if version != PROTO_VERSION:
        raise FramingError("bad SPI version")
    if length > SPI_MAX_PAYLOAD:
        raise FramingError("SPI length too large")
    if len(frame) < SPI_HEADER_SIZE + length:
        raise FramingError("short SPI payload")
    payload = frame[SPI_HEADER_SIZE:SPI_HEADER_SIZE + length]
    if crc16(payload, crc16(frame[:6])) != crc:
        raise FramingError("SPI CRC mismatch")
    return type_, flags, payload


# ---- fragments ----


def frag_count(length: int, mtu: int) -> int:
    """Chunks needed for `length` bytes; 0 if mtu < 4 or more than 255 chunks."""
    if mtu <= FRAG_HEADER_SIZE:
        return 0
    data = mtu - FRAG_HEADER_SIZE
    n = 1 if length == 0 else -(-length // data)
    return 0 if n > 255 else n


def fragment(msg_id: int, msg: bytes, mtu: int) -> list[bytes]:
    """Split a message into chunks of at most `mtu` bytes (3-byte header: msg_id, index, count)."""
    count = frag_count(len(msg), mtu)
    if count == 0:
        raise FramingError("message does not fit in 255 chunks of this MTU")
    data = mtu - FRAG_HEADER_SIZE
    return [bytes([msg_id & 0xFF, i, count]) + msg[i * data:(i + 1) * data] for i in range(count)]


def ack(msg_id: int, index: int) -> bytes:
    """The acknowledgement of chunk `index` of message `msg_id`: `msg_id | index | 0`. LTE: the collector answers the
    last chunk of a message with it when it has no downlink to send. Mirror of sensei_frag_ack()."""
    return bytes([msg_id & 0xFF, index & 0xFF, 0])


def parse_ack(data: bytes) -> tuple[int, int] | None:
    """(msg_id, index) if `data` is an acknowledgement, else None."""
    if len(data) == ACK_SIZE and data[2] == 0:
        return data[0], data[1]
    return None


def _digest(data: bytes) -> bytes:
    return hashlib.blake2b(data, digest_size = 8).digest()


@dataclass
class _Partial:
    count: int
    chunks: dict[int, bytes] = field(default_factory = dict)
    last_rx: float = 0.0


class Reassembler:
    """Reassemble messages from chunks. Several messages (ids) can be in flight.

    A message whose last chunk arrived more than `timeout` seconds ago is dropped. Time is
    whatever the caller passes as `now` (monotonic seconds). Completed ids are remembered for
    `dup_ttl` seconds (default 30 s or the timeout, if shorter) together with a hash of each chunk, so that
    duplicated chunks do not restart a message and a copy of a finished message is not delivered twice. Every copy
    renews the time. The id has 8 bits: it comes back after 256 messages, and a node that restarts counts from 0
    again; a chunk of a remembered id whose bytes differ from the stored ones starts a new message. `dup_ttl` is
    short on LoRa (a LoRa chunk waits up to 10 minutes for the duty cycle, the id is the only link between chunks);
    on LTE the collector sets it to minutes, as long as a node may retry the last chunk. A chunk that differs from
    the one stored under the same index belongs to a new message and starts it over.
    """

    def __init__(self, timeout: float = 300.0, mtu: int | None = None, dup_ttl: float | None = None):
        self.timeout = timeout
        self.dup_ttl = min(30.0, timeout) if dup_ttl is None else dup_ttl
        self.chunk_data = None if mtu is None else mtu - FRAG_HEADER_SIZE
        self.partial: dict[int, _Partial] = {}
        self.done: dict[int, float] = {}
        self._sig: dict[int, tuple[int, dict[int, bytes]]] = {}  # finished id -> (count, hash of each chunk)
        self.n_complete = 0
        self.n_timeout = 0
        self.n_errors = 0

    def poll(self, now: float) -> list[int]:
        """Drop messages that timed out. Returns their ids."""
        dropped = [i for i, p in self.partial.items() if now - p.last_rx > self.timeout]
        for i in dropped:
            del self.partial[i]
            self.n_timeout += 1
        for i in [i for i, t in self.done.items() if now - t > self.dup_ttl]:
            del self.done[i]
            self._sig.pop(i, None)
        return dropped

    def feed(self, chunk: bytes, now: float) -> bytes | None:
        """Returns the message when it is complete, else None. Malformed chunks are counted."""
        return self.feed_ex(chunk, now)[0]

    def feed_ex(self, chunk: bytes, now: float) -> tuple[bytes | None, bool]:
        """Like `feed`, and says whether the chunk is a copy of one of a finished message (the sender did not get the
        answer and sends again): (None, True). Such a copy is not delivered again."""
        self.poll(now)
        if len(chunk) < FRAG_HEADER_SIZE:
            self.n_errors += 1
            return None, False
        msg_id, index, count = chunk[0], chunk[1], chunk[2]
        data = chunk[FRAG_HEADER_SIZE:]
        if count == 0 or index >= count:
            self.n_errors += 1
            return None, False
        if self.chunk_data is not None and (len(data) > self.chunk_data or
                                            (index + 1 < count and len(data) != self.chunk_data)):
            self.n_errors += 1
            return None, False
        if msg_id in self.done:
            sig = self._sig[msg_id]
            if sig[0] == count and sig[1].get(index) == _digest(data):
                self.done[msg_id] = now
                return None, True
            del self.done[msg_id], self._sig[msg_id]  # the same id with other bytes: a new message
        part = self.partial.get(msg_id)
        if part is None or part.count != count or part.chunks.get(index, data) != data:
            part = self.partial[msg_id] = _Partial(count)
        part.last_rx = now
        part.chunks.setdefault(index, data)
        if len(part.chunks) < count:
            return None, False
        del self.partial[msg_id]
        self.done[msg_id] = now
        self._sig[msg_id] = (count, {i: _digest(c) for i, c in part.chunks.items()})
        self.n_complete += 1
        return b"".join(part.chunks[i] for i in range(count)), False
