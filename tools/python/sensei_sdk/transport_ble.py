# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""BLE transport (bleak) for a GATT service that carries SENSEI stream bytes.

The UUIDs and the advertising name belong to the application: it describes them in a `BleProfile` and gives it to
`scan` and `BleTransport`. Host RX/TX carry COBS stream bytes. Uplink chunks (3 B fragment header) are wrapped into
CH_UPLINK_CHUNK stream frames so the same decoder reassembles them. The SMP service of MCUboot (`smp.SMP_SERVICE`)
is the same on every device and needs no profile.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import struct
from dataclasses import dataclass

from . import framing
from .transport import Transport, TransportError

log = logging.getLogger("sensei.ble")


@dataclass(frozen = True)
class BleProfile:
    """GATT service of an application. A characteristic that is None is not used: `BleTransport` subscribes only to
    the ones that are set, and the matching read and write calls raise TransportError."""

    service: str | None = None
    adv_name: str | None = None
    char_rx: str | None = None  # write without response, bytes to the device
    char_tx: str | None = None  # notify, bytes from the device
    char_uplink: str | None = None  # notify: uplink chunks
    char_settings: str | None = None  # read and write: the application's settings (one write, no long write)
    char_time: str | None = None  # write: u32 LE Unix time


async def scan(profile: BleProfile, timeout: float = 5.0) -> list[tuple[str, str, int]]:
    """(address, name, rssi) of nearby devices that advertise the profile's name or service."""
    from bleak import BleakScanner

    found = await BleakScanner.discover(timeout = timeout, return_adv = True)
    out = []
    for address, (dev, adv) in found.items():
        name = dev.name or adv.local_name
        if (profile.adv_name is not None
                and name == profile.adv_name) or (profile.service is not None
                                                  and profile.service in (adv.service_uuids or [])):
            out.append((address, dev.name or adv.local_name or "", adv.rssi))
    return sorted(out, key = lambda t: -t[2])


def split_write(data: bytes, payload: int) -> list[bytes]:
    return [data[i:i + payload] for i in range(0, len(data), payload)]


class BleTransport(Transport):
    name = "ble"
    # bleak waits 10 s by default; a node that has gone away holds the connect lock of the manager for that long
    CONNECT_TIMEOUT = 6.0

    def __init__(self, address: str, profile: BleProfile, client_factory = None) -> None:
        super().__init__()
        self.address = address
        self.profile = profile
        self._factory = client_factory
        self._client = None
        self._write_lock = asyncio.Lock()  # one message is written in pieces: two writers must not interleave

    async def open(self) -> None:
        if self._factory is None:
            from bleak import BleakClient

            self._factory = BleakClient
        self._client = self._factory(self.address, disconnected_callback = lambda _c: self._closed())
        try:
            await asyncio.wait_for(self._client.connect(), self.CONNECT_TIMEOUT)
            if self.profile.char_tx:
                await self._client.start_notify(self.profile.char_tx, lambda _h, d: self._deliver(bytes(d)))
            if self.profile.char_uplink:
                await self._client.start_notify(self.profile.char_uplink, self._on_uplink)
            await self._acquire_mtu()
        except Exception as exc:
            with contextlib.suppress(Exception):
                await self._client.disconnect()  # a connect that timed out may be half done
            raise TransportError(f"BLE connect failed: {exc or type(exc).__name__}") from exc
        self.is_open = True

    async def _acquire_mtu(self) -> None:
        """bleak on BlueZ reports an MTU of 23 until it has asked for the real one. Without it every write is cut
        into 20 B pieces. Other backends know their MTU and have no such call."""
        backend = getattr(self._client, "_backend", self._client)
        acquire = getattr(backend, "_acquire_mtu", None)
        if acquire is None:
            return
        try:
            await acquire()
        except Exception as exc:  # the link works with the small MTU, only slower
            log.warning("BLE %s: MTU not negotiated, writing 20 B pieces: %s", self.address, exc)

    def _on_uplink(self, _handle, data) -> None:
        self._deliver(framing.stream_encode(framing.CH_UPLINK_CHUNK, bytes(data)))

    async def close(self) -> None:
        self.is_open = False
        if self._client is not None:
            try:
                await self._client.disconnect()
            except Exception:
                pass

    @property
    def payload_size(self) -> int:
        mtu = getattr(self._client, "mtu_size", 23) or 23
        return max(20, mtu - 3)

    @staticmethod
    async def _gatt(what: str, call):
        """Await a bleak call. Whatever it raises (BleakError, a D-Bus error, a timeout) becomes a TransportError,
        so that the callers need to know one kind of link error only."""
        try:
            return await call
        except TransportError:
            raise
        except Exception as exc:
            raise TransportError(f"BLE {what} failed: {exc}") from exc

    async def write(self, data: bytes) -> None:
        if self._client is None or not self.is_open:
            raise TransportError("BLE not connected")
        async with self._write_lock:
            for chunk in split_write(data, self.payload_size):
                await self._gatt("write", self._client.write_gatt_char(self._char("char_rx"), chunk, response = False))

    def _char(self, name: str) -> str:
        uuid = getattr(self.profile, name)
        if not uuid:
            raise TransportError(f"BLE: the profile has no {name}")
        return uuid

    def smp_link(self):
        from .smp import BleSmpLink

        return BleSmpLink(self)

    # GATT characteristics outside the stream
    async def read_settings(self) -> bytes:
        if self._client is None or not self.is_open:
            raise TransportError("BLE not connected")
        return bytes(await self._gatt("settings read", self._client.read_gatt_char(self._char("char_settings"))))

    async def write_settings(self, patch: bytes) -> None:
        # The node takes the patch in one ATT write (no long write): it must fit MTU - 3.
        if len(patch) > self.payload_size:
            raise TransportError(
                f"settings write of {len(patch)} B does not fit the BLE link (ATT payload {self.payload_size} B, "
                "MTU not negotiated); use USB or a host that raises the MTU")
        if self._client is None or not self.is_open:
            raise TransportError("BLE not connected")
        async with self._write_lock:
            await self._gatt("settings write",
                             self._client.write_gatt_char(self._char("char_settings"), patch, response = True))

    async def write_time(self, unix_time: int) -> None:
        if self._client is None or not self.is_open:
            raise TransportError("BLE not connected")
        async with self._write_lock:
            await self._gatt(
                "time write",
                self._client.write_gatt_char(self._char("char_time"), struct.pack("<I", unix_time), response = True),
            )
