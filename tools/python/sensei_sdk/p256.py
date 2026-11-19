# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""P-256 point arithmetic with Python ints, for the SPAKE2+ of `commission` (the `cryptography` package cannot add
points). Points are affine `(x, y)` tuples; `None` is the point at infinity.

NOT constant time: it leaks the scalar through timing. It is for a commissioning host (a PC at the bench or in the
office) and for tests. Do not use it where an attacker can time it on a secret scalar from nearby, and never on a node.
"""

from __future__ import annotations

from .errors import SecureError

P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551  # group order (cofactor 1)
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
     0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)
POINT_SIZE = 65  # SEC1 uncompressed: 0x04 || x || y

Point = tuple[int, int] | None


class PointError(SecureError):
    """Not a valid point of the P-256 group (length, encoding, off the curve, infinity)."""


def on_curve(pt: Point) -> bool:
    if pt is None:
        return True
    x, y = pt
    return 0 <= x < P and 0 <= y < P and (y * y - (x * x * x - 3 * x + B)) % P == 0


def neg(pt: Point) -> Point:
    return None if pt is None else (pt[0], (-pt[1]) % P)


def add(a: Point, b: Point) -> Point:
    if a is None:
        return b
    if b is None:
        return a
    (x1, y1), (x2, y2) = a, b
    if x1 == x2:
        if (y1 + y2) % P == 0:
            return None
        lam = (3 * x1 * x1 - 3) * pow(2 * y1, -1, P) % P
    else:
        lam = (y2 - y1) * pow(x2 - x1, -1, P) % P
    x3 = (lam * lam - x1 - x2) % P
    return x3, (lam * (x1 - x3) - y1) % P


def mul(k: int, pt: Point) -> Point:
    """k * pt (double and add). k is not reduced, so `mul(N, pt)` tells whether pt is in the group."""
    if k < 0:
        k, pt = -k, neg(pt)
    acc: Point = None
    while k:
        if k & 1:
            acc = add(acc, pt)
        pt = add(pt, pt)
        k >>= 1
    return acc


def decompress(data: bytes) -> tuple[int, int]:
    """Parse a 33 B compressed point (02/03 || x), as RFC 9383 lists M and N. Only for constants: peers send 65 B."""
    if len(data) != 33 or data[0] not in (2, 3):
        raise PointError("a compressed point is 33 bytes: 0x02 or 0x03, x")
    x = int.from_bytes(data[1:], "big")
    if x >= P:
        raise PointError("coordinate out of range")
    y = pow(x * x * x - 3 * x + B, (P + 1) // 4, P)  # p = 3 mod 4
    if y * y % P != (x * x * x - 3 * x + B) % P:
        raise PointError("point is not on the curve")
    return x, (y if y & 1 == data[0] & 1 else P - y)


def encode(pt: Point) -> bytes:
    """65 B uncompressed encoding. The point at infinity has none."""
    if pt is None:
        raise PointError("the point at infinity cannot be encoded")
    return b"\x04" + pt[0].to_bytes(32, "big") + pt[1].to_bytes(32, "big")


def decode(data: bytes) -> tuple[int, int]:
    """Parse a 65 B uncompressed point. Refuses a wrong length or prefix, a coordinate >= p, a point off the curve and
    the point at infinity. The subgroup check (n * pt = infinity) is cheap here and kept for the day another curve with
    a cofactor is used."""
    if len(data) != POINT_SIZE or data[0] != 4:
        raise PointError("a point is 65 bytes: 0x04, x, y")
    pt = (int.from_bytes(data[1:33], "big"), int.from_bytes(data[33:], "big"))
    if pt[0] >= P or pt[1] >= P or not on_curve(pt):
        raise PointError("point is not on the curve")
    if mul(N, pt) is not None:
        raise PointError("point is not in the prime order subgroup")
    return pt
