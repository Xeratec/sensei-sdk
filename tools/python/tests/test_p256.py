# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""sensei_sdk.p256 against the `cryptography` package, and the point checks."""

import secrets

import pytest
from cryptography.hazmat.primitives.asymmetric import ec
from sensei_sdk import p256, secure
from sensei_sdk.vectors_commission import bad_points


def scalars() -> list[int]:
    return [
        1, 2, 3, 255, 256, p256.N - 2, p256.N - 1, 1 << 255, *(secrets.randbelow(p256.N - 1) + 1 for _ in range(12))
    ]


@pytest.mark.parametrize("k", scalars())
def test_scalar_times_g_matches_cryptography(k):
    assert p256.encode(p256.mul(k, p256.G)) == secure.public_from_private(k.to_bytes(32, "big"))


def test_scalar_times_any_point_matches_ecdh():
    """x(k * Q) is what ECDH returns."""
    for _ in range(5):
        k, q = secrets.randbelow(p256.N - 1) + 1, secrets.randbelow(p256.N - 1) + 1
        pt = p256.mul(q, p256.G)
        ours = p256.mul(k, pt)
        priv = ec.derive_private_key(k, ec.SECP256R1())
        peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), p256.encode(pt))
        assert priv.exchange(ec.ECDH(), peer) == ours[0].to_bytes(32, "big")


def test_group_laws():
    a, b = p256.mul(5, p256.G), p256.mul(11, p256.G)
    assert p256.add(a, b) == p256.mul(16, p256.G) == p256.add(b, a)
    assert p256.add(a, p256.neg(a)) is None
    assert p256.add(None, a) == a and p256.add(a, None) == a
    assert p256.add(a, a) == p256.mul(10, p256.G)
    assert p256.mul(p256.N, p256.G) is None and p256.mul(0, p256.G) is None
    assert p256.mul(-3, p256.G) == p256.neg(p256.mul(3, p256.G))
    assert p256.on_curve(a) and not p256.on_curve((a[0], a[1] ^ 1))


def test_decode_encode_round_trip():
    for k in (1, 7, p256.N - 1):
        pt = p256.mul(k, p256.G)
        assert p256.decode(p256.encode(pt)) == pt
    with pytest.raises(p256.PointError):
        p256.encode(None)


@pytest.mark.parametrize("what, data", bad_points(), ids = [w for w, _ in bad_points()])
def test_decode_refuses(what, data):
    with pytest.raises(p256.PointError):
        p256.decode(data)


def test_subgroup_check_is_run(monkeypatch):
    """A point that the multiplication by n does not send to infinity is refused (cannot happen on P-256, whose
    cofactor is 1; the check is kept for other curves)."""
    monkeypatch.setattr(p256, "mul", lambda k, pt: pt)
    with pytest.raises(p256.PointError, match = "subgroup"):
        p256.decode(p256.encode(p256.G))


def test_decompress():
    for k in (1, 2, p256.N - 1, 12345):
        pt = p256.mul(k, p256.G)
        assert p256.decompress(bytes([2 + (pt[1] & 1)]) + pt[0].to_bytes(32, "big")) == pt
    for bad in (b"", b"\x04" + bytes(32), b"\x02" + bytes(31), b"\x02" + p256.P.to_bytes(32, "big")):
        with pytest.raises(p256.PointError):
            p256.decompress(bad)
