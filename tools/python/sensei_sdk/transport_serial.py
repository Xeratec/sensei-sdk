# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""USB CDC serial transport (pyserial, reader thread). Pattern: BioGUI serial worker.

The port can also be a pyserial URL, for example `socket://localhost:7001` (a simulated device)."""

from __future__ import annotations

import asyncio
import threading

from .transport import Transport, TransportError


def list_ports() -> list[tuple[str, str]]:
    """(device, description) of the serial ports."""
    from serial.tools import list_ports as lp

    return [(p.device, p.description) for p in sorted(lp.comports(), key = lambda p: p.device)]


def list_ports_ids() -> list[tuple[str, str, int | None, int | None]]:
    """(device, description, vid, pid) of the serial ports."""
    from serial.tools import list_ports as lp

    return [(p.device, p.description, p.vid, p.pid) for p in sorted(lp.comports(), key = lambda p: p.device)]


class SerialTransport(Transport):
    name = "serial"
    WRITE_TIMEOUT = 1.0  # seconds a write may block (a hung node or hub stops reading): then the link is in error

    def __init__(self, port: str, baudrate: int = 115200) -> None:
        super().__init__()
        self.port, self.baudrate = port, baudrate
        self._ser = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._write_lock = asyncio.Lock()  # the write runs in a thread: two writers must not interleave

    async def open(self) -> None:
        import serial

        self._loop = asyncio.get_running_loop()
        try:
            # serial_for_url opens a device name as serial.Serial does, and a URL (socket://host:port) as its type
            self._ser = serial.serial_for_url(self.port,
                                              baudrate = self.baudrate,
                                              timeout = 0.05,
                                              write_timeout = self.WRITE_TIMEOUT)
        except (serial.SerialException, OSError) as exc:
            raise TransportError(f"cannot open {self.port}: {exc}") from exc
        self._stop.clear()
        self._thread = threading.Thread(target = self._reader, daemon = True)
        self._thread.start()
        self.is_open = True

    def _reader(self) -> None:
        loop, ser = self._loop, self._ser
        try:
            while not self._stop.is_set():
                data = ser.read(ser.in_waiting or 1)
                if data:
                    loop.call_soon_threadsafe(self._deliver, data)
        except Exception:
            if not self._stop.is_set():
                loop.call_soon_threadsafe(self._closed)

    async def close(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout = 1.0)
        if self._ser is not None:
            self._ser.close()
        self.is_open = False

    def smp_link(self):
        from .smp import SerialSmpLink

        return SerialSmpLink(self)

    async def write(self, data: bytes) -> None:
        if self._ser is None or not self.is_open:
            raise TransportError("port closed")
        try:
            async with self._write_lock:
                # pyserial has its own write timeout; this one also covers a port object that blocks without it
                await asyncio.wait_for(asyncio.get_running_loop().run_in_executor(None, self._ser.write, data),
                                       self.WRITE_TIMEOUT + 1.0)
        except TimeoutError:
            raise TransportError(f"write to {self.port} timed out") from None
        except OSError as exc:
            raise TransportError(str(exc)) from exc
