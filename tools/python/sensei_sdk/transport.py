# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Byte transports to a node. A transport moves COBS stream bytes; framing is done by the RPC client."""

from __future__ import annotations

import abc
import time
from collections.abc import Callable


class TransportError(OSError):
    pass


class Transport(abc.ABC):
    """Async byte pipe. `on_data` is called from the event loop thread with received bytes."""

    name = "transport"

    def __init__(self) -> None:
        self.on_data: Callable[[bytes], None] | None = None
        self.on_close: Callable[[], None] | None = None
        self.is_open = False

    @abc.abstractmethod
    async def open(self) -> None:
        ...

    @abc.abstractmethod
    async def close(self) -> None:
        ...

    @abc.abstractmethod
    async def write(self, data: bytes) -> None:
        ...

    def smp_link(self):
        """SMP (mcumgr) link for a firmware update over this connection. See `sensei_sdk.smp`."""
        raise TransportError(f"{self.name}: no SMP link")

    def _deliver(self, data: bytes) -> None:
        if data and self.on_data is not None:
            self.on_data(data)

    def _closed(self) -> None:
        self.is_open = False
        if self.on_close is not None:
            self.on_close()


class LoopbackTransport(Transport):
    """In-process link to a node object with `receive(bytes)` and an `emit` callback (the simulator)."""

    name = "simulator"

    def __init__(self, node, smp: str = "serial") -> None:
        """smp: how the simulated SMP server is reached, "serial" (mcumgr line framing on this stream, as
        over USB) or "packet" (whole SMP packets in fragments, as over the BLE SMP characteristic)."""
        super().__init__()
        self.node = node
        self.smp = smp

    async def open(self) -> None:
        if time.monotonic() < getattr(self.node, "offline_until", 0.0):
            raise TransportError("node is offline")
        self.node.emit = self._deliver
        self.node.on_link_drop = self._closed
        self.is_open = True
        await self.node.start()

    async def close(self) -> None:
        self.is_open = False
        await self.node.stop()
        self.node.emit = lambda data: None
        self.node.on_link_drop = lambda: None

    def smp_link(self):
        from .smp import CallbackSmpLink, SerialSmpLink

        if getattr(self.node, "smp", None) is None:
            raise TransportError("simulated node has no SMP server")
        if self.smp == "serial":
            return SerialSmpLink(self)
        port = self.node.smp.packet_port()
        return CallbackSmpLink(port.write, mtu = 244, attach = port.attach)

    async def write(self, data: bytes) -> None:
        if not self.is_open:
            raise TransportError("loopback closed")
        self.node.receive(data)

    async def write_settings(self, patch: bytes) -> None:
        """The BLE settings characteristic of the simulated node (a plain patch or a sealed envelope)."""
        if not self.is_open:
            raise TransportError("loopback closed")
        if not self.node.write_settings(patch):
            raise TransportError("settings write refused")

    async def write_time(self, unix_time: int) -> None:
        """The BLE time characteristic of the simulated node (refused with e2e on, like the firmware)."""
        if not self.is_open:
            raise TransportError("loopback closed")
        if not self.node.write_time(unix_time):
            raise TransportError("time write refused")
