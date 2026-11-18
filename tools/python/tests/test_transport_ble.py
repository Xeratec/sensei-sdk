# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""BLE transport with a profile of the application's own UUIDs, against a fake bleak client."""

import asyncio

import pytest
from sensei_sdk import framing
from sensei_sdk.transport import TransportError
from sensei_sdk.transport_ble import BleProfile, BleTransport

PROFILE = BleProfile(
    service = "0000aaaa-0000-1000-8000-00805f9b34fb",
    adv_name = "DEMO",
    char_rx = "0000aaa1-0000-1000-8000-00805f9b34fb",
    char_tx = "0000aaa2-0000-1000-8000-00805f9b34fb",
    char_uplink = "0000aaa3-0000-1000-8000-00805f9b34fb",
    char_settings = "0000aaa4-0000-1000-8000-00805f9b34fb",
    char_time = "0000aaa5-0000-1000-8000-00805f9b34fb",
)


class FakeClient:
    mtu_size = 23 + 20

    def __init__(self, address, disconnected_callback = None):
        self.notify, self.writes = {}, []

    async def connect(self):
        pass

    async def disconnect(self):
        pass

    async def start_notify(self, char, cb):
        self.notify[char] = cb

    async def write_gatt_char(self, char, data, response):
        self.writes.append((char, bytes(data), response))

    async def read_gatt_char(self, char):
        return b"settings"


def test_the_profile_gives_the_uuids():

    async def go():
        made = []

        def factory(address, disconnected_callback = None):
            made.append(FakeClient(address))
            return made[0]

        t = BleTransport("AA", PROFILE, client_factory = factory)
        got = []
        t.on_data = got.append
        await t.open()
        fc = made[0]
        assert set(fc.notify) == {PROFILE.char_tx, PROFILE.char_uplink}
        fc.notify[PROFILE.char_tx](0, b"\x01\x02")
        fc.notify[PROFILE.char_uplink](0, b"\x07\x00\x01abc")
        assert got[0] == b"\x01\x02"
        assert framing.StreamDecoder().feed(got[1]) == [(framing.CH_UPLINK_CHUNK, b"\x07\x00\x01abc")]
        await t.write(b"x" * 50)  # payload 40: two pieces
        assert [(c, len(d), r) for c, d, r in fc.writes] == [
            (PROFILE.char_rx, 40, False),
            (PROFILE.char_rx, 10, False),
        ]
        await t.write_time(5)
        assert fc.writes[-1] == (PROFILE.char_time, b"\x05\x00\x00\x00", True)
        assert await t.read_settings() == b"settings"

    asyncio.run(go())


def test_a_profile_without_a_characteristic_refuses_the_call():

    async def go():
        t = BleTransport("AA",
                         BleProfile(adv_name = "X"),
                         client_factory = lambda a, disconnected_callback = None: FakeClient(a))
        await t.open()  # SMP only: nothing to subscribe to
        for call in (t.write(b"x"), t.read_settings(), t.write_time(1)):
            with pytest.raises(TransportError, match = "profile has no"):
                await call

    asyncio.run(go())
