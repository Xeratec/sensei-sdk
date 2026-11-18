# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Framing tests: known vectors, round trips, error handling, fragmentation."""

import random

import pytest
from sensei_sdk import framing as fr
from sensei_sdk.vectors import pattern


def test_crc_check_values():
    assert fr.crc16(b"123456789") == 0x29B1
    assert fr.crc16(b"") == 0xFFFF
    assert fr.crc32(b"123456789") == 0xCBF43926
    assert fr.crc16(b"56789", fr.crc16(b"1234")) == 0x29B1


def test_cobs_known():
    assert fr.cobs_encode(b"\x00") == b"\x01\x01"
    assert fr.cobs_encode(b"\x11\x22\x00\x33") == b"\x03\x11\x22\x02\x33"
    assert fr.cobs_encode(b"") == b"\x01"


@pytest.mark.parametrize("n", [*list(range(0, 600, 7)), 253, 254, 255, 508, 509])
def test_cobs_round_trip(n):
    rng = random.Random(n)
    for zero_p in (0.0, 0.3, 0.99):
        data = bytes(0 if rng.random() < zero_p else rng.randrange(1, 256) for _ in range(n))
        enc = fr.cobs_encode(data)
        assert 0 not in enc
        assert len(enc) <= n + n // 254 + 1
        assert fr.cobs_decode(enc) == data


@pytest.mark.parametrize("bad", [b"", b"\x05\x01", b"\x02\x00", b"\x00\x01"])
def test_cobs_bad(bad):
    with pytest.raises(fr.FramingError):
        fr.cobs_decode(bad)


def test_stream_round_trip_and_chunked_feed():
    dec = fr.StreamDecoder()
    frames = [(fr.CH_NODE_TO_HOST, pattern(n, n)) for n in (0, 1, 100, 2095)]
    wire = b"".join(fr.stream_encode(c, p) for c, p in frames)
    got = []
    for i in range(0, len(wire), 7):
        got += dec.feed(wire[i:i + 7])
    assert got == frames
    assert dec.n_frames == 4


def test_stream_corruption_and_resync():
    good = fr.stream_encode(fr.CH_UPLINK, pattern(40))
    dec = fr.StreamDecoder()
    bad = bytearray(good)
    bad[10] ^= 1
    out = dec.feed(bytes(bad) + good)
    assert out == [(fr.CH_UPLINK, pattern(40))]
    assert dec.n_crc_err + dec.n_format_err == 1
    # garbage ends at a delimiter, then a good frame
    out = dec.feed(b"\x55\x55\x00" + good)
    assert out == [(fr.CH_UPLINK, pattern(40))]


def test_stream_overflow():
    dec = fr.StreamDecoder(max_frame = 16)
    long = fr.stream_encode(1, pattern(100))
    assert dec.feed(long + fr.stream_encode(1, b"abc")) == [(1, b"abc")]
    assert dec.n_overflow == 1


def test_spi_round_trip_and_errors():
    for n in (0, 1, 1046, fr.SPI_MAX_PAYLOAD):
        p = pattern(n)
        f = fr.spi_encode(fr.SPI_GAP9_JOB, p, fr.SPI_FLAG_MORE)
        assert f[0] == 0x5E and len(f) == 8 + n
        assert fr.spi_decode(f + b"\xff") == (fr.SPI_GAP9_JOB, fr.SPI_FLAG_MORE, p)
    f = bytearray(fr.spi_encode(fr.SPI_GAP9_JOB, b"abc"))
    with pytest.raises(fr.FramingError):
        fr.spi_decode(bytes(f[:-1]))
    f[-1] ^= 1
    with pytest.raises(fr.FramingError):
        fr.spi_decode(bytes(f))
    with pytest.raises(fr.FramingError):
        fr.spi_encode(1, bytes(fr.SPI_MAX_PAYLOAD + 1))
    with pytest.raises(fr.FramingError):
        fr.spi_decode(b"\x00" * 8)


def test_spi_crc_covers_the_header():
    f = fr.spi_encode(fr.SPI_LPWAN_REQUEST, b"abc", 0x04)
    for i in (2, 3, 4):  # type, flags, length (length 4 would also be short, so keep it valid)
        g = bytearray(f + b"\x00" * 4)
        g[i] ^= 0x01
        with pytest.raises(fr.FramingError):
            fr.spi_decode(bytes(g))
    assert f[1] == 2  # SENSEI_LINK_VERSION


def test_fragment_counts():
    assert fr.frag_count(0, 222) == 1
    assert fr.frag_count(219, 222) == 1
    assert fr.frag_count(220, 222) == 2
    assert fr.frag_count(10, 3) == 0
    assert fr.frag_count(255 * 219, 222) == 255
    assert fr.frag_count(255 * 219 + 1, 222) == 0
    with pytest.raises(fr.FramingError):
        fr.fragment(1, bytes(255 * 219 + 1), 222)


@pytest.mark.parametrize("mtu", [fr.MTU_LORA, fr.MTU_BLE, fr.MTU_LTE, 4, 10])
@pytest.mark.parametrize("n", [0, 1, 218, 219, 220, 438, 2086])
def test_fragment_round_trip(mtu, n):
    if fr.frag_count(n, mtu) == 0:
        pytest.skip("too many chunks")
    msg = pattern(n, 3)
    chunks = fr.fragment(9, msg, mtu)
    assert all(len(c) <= mtu for c in chunks)
    order = list(range(len(chunks)))
    random.Random(n).shuffle(order)
    r = fr.Reassembler(timeout = 10, mtu = mtu)
    out = None
    for k, i in enumerate(order):
        got = r.feed(chunks[i], now = k)
        if k < len(order) - 1:
            assert got is None
        else:
            out = got
    assert out == msg


def test_reassembly_duplicates_timeout_interleave():
    msg = pattern(1000)
    a = fr.fragment(1, msg, 222)
    b = fr.fragment(2, msg[::-1], 222)
    r = fr.Reassembler(timeout = 5)
    done = []
    for ca, cb in zip(a, b):  # two messages interleaved, with duplicates
        for c in (ca, ca, cb):
            m = r.feed(c, now = 1.0)
            if m is not None:
                done.append(m)
    assert done == [msg, msg[::-1]]
    assert r.feed(a[-1], now = 2.0) is None  # late duplicate of a finished message
    assert r.n_complete == 2

    r = fr.Reassembler(timeout = 5)
    assert r.feed(a[0], now = 0.0) is None
    assert r.poll(6.0) == [1]
    assert r.feed(a[1], now = 6.5) is None  # first half is gone
    assert r.n_timeout == 1
    # malformed chunks are counted
    assert r.feed(b"\x01", now = 7) is None
    assert r.feed(bytes([1, 3, 3]) + b"x", now = 7) is None
    assert r.n_errors == 2


def test_reassembler_repeat_of_a_finished_message():
    """A copy of a finished message is said to be a repeat (feed_ex) and is not delivered again, for dup_ttl;
    the same id with other bytes is a new message."""
    r = fr.Reassembler(timeout = 300, dup_ttl = 600)
    msg = pattern(500)
    chunks = fr.fragment(4, msg, 222)
    assert len(chunks) == 3
    assert [r.feed_ex(c, 0.0) for c in chunks] == [(None, False), (None, False), (msg, False)]
    assert r.feed_ex(chunks[-1], 100.0) == (None, True)  # the last chunk again, long after the default 30 s
    assert r.feed_ex(chunks[0], 500.0) == (None, True)  # every copy renews the time
    assert r.feed(chunks[1], 1000.0) is None and r.n_complete == 1
    other = fr.fragment(4, pattern(500, 7), 222)  # a restarted sender uses the id again
    assert r.feed_ex(other[2], 1100.0) == (None, False)
    assert [r.feed_ex(c, 1100.0)[0] for c in other[:2]] == [None, pattern(500, 7)]
    # after dup_ttl the id is free again
    r.poll(3000.0)
    assert r.feed_ex(chunks[0], 3000.0) == (None, False) and not r.done
    # the short default
    d = fr.Reassembler(timeout = 300)
    for c in chunks:
        d.feed(c, 0.0)
    assert d.feed_ex(chunks[-1], 29.0)[1] is True and d.feed_ex(chunks[-1], 100.0)[1] is False
