# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""SMP client against the simulated MCUboot server: CBOR, serial framing, upload, test, confirm, reset."""

import asyncio

import pytest
from sensei_sdk import smp
from sensei_sdk.smp_sim import SimSmpServer, make_image


def run(coro):
    return asyncio.run(coro)


def test_cbor_roundtrip():
    obj = {"image": 0, "data": b"\x00" * 300, "off": 70000, "neg": -5, "ok": True, "n": None, "l": [1, "a", {"x": 2}]}
    assert smp.cbor_decode(smp.cbor_encode(obj)) == obj
    assert smp.cbor_decode(b"\xbf\x61a\x01\xff") == {"a": 1}  # indefinite-length map
    with pytest.raises(smp.SmpError):
        smp.cbor_decode(b"\xa1\x61a")  # truncated


def test_packet_roundtrip():
    pkt = smp.encode_packet(smp.OP_WRITE, smp.GROUP_IMAGE, smp.IMG_UPLOAD, 7, {"off": 0})
    p = smp.decode_packet(pkt)
    assert (p.op, p.group, p.cmd, p.seq, p.body) == (smp.OP_WRITE, smp.GROUP_IMAGE, smp.IMG_UPLOAD, 7, {"off": 0})


def test_serial_framing_multi_line_and_noise():
    pkt = smp.encode_packet(smp.OP_WRITE, 1, 1, 3, {"data": bytes(range(256)) * 2})
    lines = smp.serial_encode(pkt)
    assert lines.count(b"\n") > 1 and lines.startswith(smp.SERIAL_FIRST)
    assert all(len(ln) <= 127 for ln in lines.split(b"\n"))
    dec = smp.SerialDecoder()
    stream = b"\x01\x02junk\n" + lines
    out = []
    for i in range(0, len(stream), 17):  # arbitrary read sizes
        out += dec.feed(stream[i:i + 17])
    assert out == [pkt]
    bad = bytearray(lines)
    bad[10] ^= 0x01  # still base64, wrong content
    assert smp.SerialDecoder().feed(bytes(bad)) == []


def test_image_info():
    img = make_image((1, 2, 3, 0), size = 500)
    info = smp.parse_image(img)
    assert info.version_str == "1.2.3" and len(info.hash) == 32 and info.size == len(img)
    assert smp.parse_image(make_image((1, 2, 3, 4))).version_str == "1.2.3.4"
    with pytest.raises(smp.SmpError):
        smp.parse_image(b"\x00" * 100)
    with pytest.raises(smp.SmpError):
        smp.parse_image(img[:-10])


async def _client(server, fragment = 23, mtu = 244):
    port = server.packet_port(fragment)
    link = smp.CallbackSmpLink(port.write, mtu = mtu, attach = port.attach)
    c = smp.SmpClient(link, timeout = 0.2, retries = 2)
    await link.open()
    return c


def test_upload_test_reset_confirm_packet_link():

    async def go():
        server = SimSmpServer(make_image((1, 0, 0, 0)))
        c = await _client(server)
        assert await c.echo("hi") == "hi"
        new = make_image((1, 1, 0, 0), size = 6000, seed = 3)
        info = smp.parse_image(new)
        seen = []
        await c.upload(new, progress = lambda d, t: seen.append(d))
        assert seen[-1] == len(new) and seen == sorted(seen) and server.upload_chunks > 10
        slots = await c.image_state()
        assert [s.version for s in slots] == ["1.0.0", "1.1.0"]
        assert slots[0].active and slots[0].confirmed and not slots[1].pending
        await c.test(info.hash)
        assert (await c.image_state())[1].pending
        await c.reset()
        assert server.reset_count == 1 and server.active.version_str == "1.1.0"
        slots = await c.image_state()
        assert slots[0].active and not slots[0].confirmed
        await c.confirm()
        assert (await c.image_state())[0].confirmed
        # a confirmed image stays across resets
        await c.reset()
        assert server.active.version_str == "1.1.0"

    run(go())


def test_unconfirmed_image_reverts():

    async def go():
        server = SimSmpServer(make_image((1, 0, 0, 0)))
        c = await _client(server)
        new = make_image((2, 0, 0, 0), size = 800)
        await c.upload(new)
        await c.test(smp.parse_image(new).hash)
        await c.reset()
        assert server.active.version_str == "2.0.0"
        await c.reset()  # no confirm: back to the old image
        assert server.active.version_str == "1.0.0" and server.slots[0].confirmed

    run(go())


def test_lost_packets_are_retried_and_errors_reported():

    async def go():
        server = SimSmpServer()
        c = await _client(server)
        new = make_image((1, 2, 0, 0), size = 2000)
        server.drop_next = 2  # first chunk is retried twice
        await c.upload(new, first_timeout = 0.2)
        assert server.slots[1] is not None
        server.drop_next = 10
        with pytest.raises(smp.SmpTimeout):
            await c.echo()
        server.drop_next = 0
        with pytest.raises(smp.SmpRcError):
            await c.test(b"\x00" * 32)  # unknown hash
        # corrupted image is rejected by the server at the end of the upload
        server.corrupt = True
        with pytest.raises(smp.SmpRcError):
            await c.upload(make_image((1, 3, 0, 0), size = 500))
        server.corrupt = False
        server.reject_upload_after = 300
        with pytest.raises(smp.SmpRcError):
            await c.upload(make_image((1, 4, 0, 0), size = 2000))

    run(go())


def test_small_mtu_and_resume_offset():

    async def go():
        server = SimSmpServer()
        c = await _client(server, mtu = 128)
        new = make_image((1, 5, 0, 0), size = 1500)
        await c.upload(new)
        assert smp.parse_image(server.slots[1].image).version_str == "1.5.0"
        with pytest.raises(smp.SmpError):
            await _small(server)

    async def _small(server):
        c = await _client(server, mtu = 40)
        await c.upload(make_image(size = 100))

    run(go())


# --- image files -------------------------------------------------------------------------------------------
def test_load_image_from_bin_and_dfu_zip(tmp_path):
    import json
    import zipfile

    img = make_image((2, 1, 0, 0), size = 700, seed = 2)
    (tmp_path / "zephyr.signed.bin").write_bytes(img)
    assert smp.load_image(tmp_path / "zephyr.signed.bin") == img
    assert smp.load_image(img) == img
    # the NCS package: manifest.json names the application image
    zpath = tmp_path / "dfu_application.zip"
    with zipfile.ZipFile(zpath, "w") as z:
        z.writestr("other.bin", b"\x00" * 64)
        z.writestr("app.signed.bin", img)
        z.writestr(
            "manifest.json",
            json.dumps({"files": [{
                "type": "application",
                "image_index": "0",
                "file": "app.signed.bin"
            }]}),
        )
    assert smp.load_image(zpath) == img
    with zipfile.ZipFile(tmp_path / "bad.zip", "w") as z:
        z.writestr("readme.txt", "no image here")
    with pytest.raises(smp.SmpError):
        smp.load_image(tmp_path / "bad.zip")
    with pytest.raises(smp.SmpError):
        smp.load_image(tmp_path / "missing.bin")
    with pytest.raises(smp.SmpError):
        smp.load_image(b"not an image at all, but long enough to pass the length check........")


def test_version_helpers_and_error_text():
    assert smp.parse_version("1.2.3") == (1, 2, 3) and smp.parse_version("0.2.0.7") == (0, 2, 0)
    assert smp.parse_version("junk") == (0, 0, 0)
    assert smp.parse_image(make_image((1, 2, 3, 9))).version3 == (1, 2, 3)
    assert "out of memory" in str(smp.SmpRcError(2))
    assert "too large" in str(smp.SmpRcError(30, smp.GROUP_IMAGE))


# --- the simulated MCUboot and node firmware -----------------------------------------------------------------
def _install(server, image):

    async def go():
        c = await _client(server)
        await c.upload(image)
        await c.test(smp.parse_image(image).hash)
        await c.reset()

    run(go())


async def _confirm(server):
    await (await _client(server)).confirm()


def test_mcuboot_refuses_wrong_key_and_downgrade():
    server = SimSmpServer(make_image((1, 5, 0, 0)), auto_confirm_s = None)
    _install(server, make_image((1, 6, 0, 0), size = 600, key = 7))  # signed with another key
    assert server.active.version_str == "1.5.0" and server.refused == ["bad signature"]
    assert server.slots[1] is None  # erased, as MCUboot does
    _install(server, make_image((1, 4, 0, 0), size = 600))  # older than the running image
    assert server.active.version_str == "1.5.0" and server.refused[-1] == "downgrade prevention"
    _install(server, make_image((1, 5, 0, 0), size = 800, seed = 4))  # same version, other build: allowed
    assert server.active.version_str == "1.5.0" and len(server.refused) == 2 and server.reset_count == 3
    assert not server.slots[0].confirmed  # on trial
    server.downgrade_prevention = False
    run(_confirm(server))  # the node refuses an upload while the running image is on trial
    _install(server, make_image((1, 1, 0, 0), size = 500, seed = 5))
    assert server.active.version_str == "1.1.0"


def test_trial_image_is_confirmed_by_the_node_or_reverts():

    async def go():
        server = SimSmpServer(make_image((1, 0, 0, 0)), auto_confirm_s = 0.05, trial_timeout_s = 0.3)
        c = await _client(server)
        new = make_image((1, 1, 0, 0), size = 500)
        await c.upload(new)
        await c.test(smp.parse_image(new).hash)
        await c.reset()
        assert not server.slots[0].confirmed
        await asyncio.sleep(0.15)
        assert server.slots[0].confirmed and server.confirmed_by_node == 1
        await asyncio.sleep(0.4)  # nothing happens any more: no reset
        assert server.reset_count == 1 and server.active.version_str == "1.1.0"

        # a new image that never passes its check: the node resets itself after the trial time, MCUboot reverts
        server.healthy = False
        bad = make_image((1, 2, 0, 0), size = 500, seed = 3)
        await c.upload(bad)
        await c.test(smp.parse_image(bad).hash)
        await c.reset()
        assert server.active.version_str == "1.2.0"
        await asyncio.sleep(0.5)
        assert server.active.version_str == "1.1.0" and server.slots[0].confirmed and server.reverts == 1
        assert server.reset_count == 3

    run(go())


def test_host_confirm_ends_the_trial():

    async def go():
        server = SimSmpServer(make_image((1, 0, 0, 0)), auto_confirm_s = None, trial_timeout_s = 0.2)
        c = await _client(server)
        new = make_image((1, 1, 0, 0), size = 500)
        await c.upload(new)
        await c.test(smp.parse_image(new).hash)
        await c.reset()
        await c.confirm()
        await asyncio.sleep(0.4)
        assert server.reset_count == 1 and server.active.version_str == "1.1.0" and server.slots[0].confirmed

    run(go())


def test_serial_recovery_writes_the_primary_slot():

    async def go():
        server = SimSmpServer(make_image((1, 0, 0, 0)), recovery = True)
        c = await _client(server)
        assert [s.version for s in await c.image_state()] == ["1.0.0"]
        new = make_image((0, 9, 0, 0), size = 900, key = 3)  # recovery does not look at version or key here
        await c.upload(new)
        assert server.active.version_str == "0.9.0"
        await c.reset()
        assert server.active.version_str == "0.9.0" and server.refused == []

    run(go())


def test_command_line_talks_to_a_simulated_recovery(monkeypatch, capsys, tmp_path):
    from sensei_sdk import transport_serial

    server = SimSmpServer(make_image((1, 0, 0, 0)), recovery = True)

    class FakePort:

        def __init__(self, port):
            self.port = port

        async def open(self):
            pass

        async def close(self):
            pass

        def smp_link(self):
            p = server.packet_port()
            return smp.CallbackSmpLink(p.write, mtu = 244, attach = p.attach)

    monkeypatch.setattr(transport_serial, "SerialTransport", FakePort)
    img = tmp_path / "app.bin"
    img.write_bytes(make_image((1, 1, 0, 0), size = 3000, seed = 8))
    assert smp.main(["--serial", "/dev/fake", "echo"]) == 0
    assert smp.main(["--serial", "/dev/fake", "upload", str(img), "--reset"]) == 0
    assert server.active.version_str == "1.1.0" and server.reset_count == 1
    assert smp.main(["--serial", "/dev/fake", "list"]) == 0
    out = capsys.readouterr().out
    assert "sensei" in out and "upload 100%" in out and "1.1.0" in out and "active" in out
    assert smp.main(["--serial", "/dev/fake", "upload", str(tmp_path / "nope.bin")]) == 1


# --- what the Zephyr image group and the node firmware refuse -------------------------------------------------
def test_upgrade_flag_is_off_by_default_and_the_node_refuses_it_for_the_same_version():

    async def go():
        sent = []

        class Rec(smp.CallbackSmpLink):

            async def send(self, packet):
                sent.append(smp.decode_packet(packet))
                await super().send(packet)

        server = SimSmpServer(make_image((0, 2, 0, 0), seed = 1))
        port = server.packet_port()
        link = Rec(port.write, mtu = 244, attach = port.attach)
        c = smp.SmpClient(link, timeout = 0.2, retries = 1)
        await link.open()
        same = make_image((0, 2, 0, 0), seed = 2)  # a rebuild of the running version
        with pytest.raises(smp.SmpRcError) as exc:
            await c.upload(same, upgrade = True)  # what the old client sent: the node says "not newer"
        assert exc.value.rc == 27 and exc.value.group == smp.GROUP_IMAGE and server.slots[1] is None
        sent.clear()
        await c.upload(same)
        first = next(p for p in sent if p.cmd == smp.IMG_UPLOAD)
        assert first.body["upgrade"] is False
        assert server.slots[1] is not None and server.slots[1].info.hash == smp.parse_image(same).hash

    run(go())


def test_upload_is_refused_while_the_running_image_is_on_trial_or_a_test_image_is_pending():

    async def go():
        server = SimSmpServer(make_image((1, 0, 0, 0)), auto_confirm_s = None)
        c = await _client(server)
        new = make_image((1, 1, 0, 0), size = 500)
        await c.upload(new)
        await c.test(smp.parse_image(new).hash)
        with pytest.raises(smp.SmpRcError) as exc:  # a test image is pending in slot 1
            await c.upload(make_image((1, 2, 0, 0), size = 500))
        assert exc.value.rc == 9 and "no free slot" in str(exc.value)
        await c.reset()  # runs 1.1.0 on trial; slot 1 holds the revert image
        with pytest.raises(smp.SmpRcError) as exc:
            await c.upload(make_image((1, 2, 0, 0), size = 500))
        assert exc.value.rc == 9
        await c.confirm()
        await c.upload(make_image((1, 2, 0, 0), size = 500))
        assert server.slots[1].info.version_str == "1.2.0"

    run(go())


def test_image_too_large_for_the_slot_is_refused_at_the_upload():

    async def go():
        server = SimSmpServer(make_image((1, 0, 0, 0), size = 500), slot_size = 8192, footer_size = 4096)
        c = await _client(server)
        big = make_image((1, 1, 0, 0), size = 5000)  # fits the slot, not the slot minus the swap trailer
        with pytest.raises(smp.SmpRcError) as exc:
            await c.upload(big)
        assert exc.value.rc == 30 and server.slots[1] is None
        await c.upload(make_image((1, 1, 0, 0), size = 3000))
        # a node without the size check accepts it, and MCUboot drops it at the reset
        old = SimSmpServer(make_image((1, 0, 0, 0), size = 500),
                           slot_size = 8192,
                           footer_size = 4096,
                           too_large_check = False)
        c = await _client(old)
        await c.upload(big)
        await c.test(smp.parse_image(big).hash)
        await c.reset()
        assert old.refused == ["image too large for the swap"] and old.active.version_str == "1.0.0"

    run(go())


def test_release_node_confirms_only_after_the_host_reached_it():

    async def go():
        server = SimSmpServer(make_image((1, 0, 0, 0)), auto_confirm_s = 0.05, trial_timeout_s = 0.6, needs_link = True)
        c = await _client(server)
        new = make_image((1, 1, 0, 0), size = 500)
        await c.upload(new)
        await c.test(smp.parse_image(new).hash)
        await c.reset()
        await asyncio.sleep(0.3)  # healthy, but nobody talked to the new image
        assert server.active.version_str == "1.1.0" and not server.slots[0].confirmed
        await c.echo()  # an SMP request on the new image
        await asyncio.sleep(0.2)
        assert server.slots[0].confirmed and server.confirmed_by_node == 1

        # an image that cannot be reached at all is not kept: the node resets after the trial time, MCUboot reverts
        bad = make_image((1, 2, 0, 0), size = 500, seed = 3)
        await c.upload(bad)
        await c.test(smp.parse_image(bad).hash)
        await c.reset()
        assert server.active.version_str == "1.2.0"
        await asyncio.sleep(0.9)
        assert server.active.version_str == "1.1.0" and server.reverts == 1

    run(go())


def test_load_image_checks_the_kind_when_asked():
    shield = make_image(kind = smp.KIND_SHIELD)
    plain = make_image()
    assert smp.load_image(shield) == shield  # no kind given: no check
    assert smp.load_image(shield, kind = smp.KIND_SHIELD) == shield
    with pytest.raises(smp.SmpError, match = "not for 'other-board'"):
        smp.load_image(shield, kind = "other-board")
    assert smp.load_image(plain, kind = "other-board") == plain  # an image without the TLV is let through
