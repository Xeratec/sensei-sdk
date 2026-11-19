# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Test vectors that the Python and the C implementations of the SDK libraries check against each other.

Writes, in the SDK:
    lib/sensei_link/tests/generated/vectors.h     CRC, COBS, stream frames, SPI frames, fragments, acknowledgements
    lib/sensei_secure/tests/generated/vectors.h   keys, sealed envelopes, sealed settings records and the forward secrecy
                                                  ratchet (root key, X25519, mix, chain, generation 1 and later
                                                  envelopes, a scripted transcript); fixed test keys, never used on a
                                                  device
    lib/sensei_diag/tests/generated/vectors.h     packed fault records, reset and reason names

    python -m sensei_sdk.vectors             write the headers
    python -m sensei_sdk.vectors --check     fail if a committed header is out of date (CI)

The SDK is found from the location of this file in a checkout, or with --sdk-root. An application that tests its own
protocol the same way (messages of its own) builds on `pattern`, `array`, `pairs` and `frag_cases`.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

from . import diag, framing, ratchet, secure

SDK_ROOT = Path(__file__).resolve().parents[3]

LINK_HEADER = "lib/sensei_link/tests/generated/vectors.h"
SECURE_HEADER = "lib/sensei_secure/tests/generated/vectors.h"
DIAG_HEADER = "lib/sensei_diag/tests/generated/vectors.h"

LICENSE = ("/*\n * SPDX-"
           "FileCopyrightText: 2026 ETH Zurich and University of Bologna\n"
           " * SPDX-"
           "License-Identifier: Apache-2.0\n *\n"
           " * Authors:\n * - Philip Wiese <wiesep@iis.ee.ethz.ch>\n *\n")


def pattern(n: int, seed: int = 0) -> bytes:
    """Deterministic bytes that include zeros (every 11th byte)."""
    return bytes(0 if (i + seed) % 11 == 0 else (i * 7 + seed * 13 + 3) & 0xFF or 1 for i in range(n))


# ---- link ----


def crc_inputs() -> list[bytes]:
    return [b"", b"123456789", b"\x00", b"\xff" * 5, pattern(300)]


def cobs_inputs() -> list[bytes]:
    return [
        b"",
        b"\x00",
        b"\x00\x00",
        b"\x01",
        b"\x11\x22\x00\x33",
        bytes(range(1, 254)),  # 253 nonzero
        bytes(range(1, 255)),  # 254 nonzero: block ends exactly at the input end
        bytes(range(1, 255)) + b"\x00",
        bytes(range(1, 255)) + b"\x01",
        bytes([1 + i % 255 for i in range(600)]),
        pattern(500),
    ]


def stream_cases() -> list[tuple[int, bytes]]:
    return [
        (framing.CH_HOST_TO_NODE, b""),
        (framing.CH_HOST_TO_NODE, b"\x00"),
        (framing.CH_NODE_TO_HOST, pattern(100)),
        (framing.CH_UPLINK, pattern(251, 1)),
        (framing.CH_UPLINK_CHUNK, pattern(240, 2)),
        (framing.CH_NODE_TO_HOST, pattern(2095, 3)),
    ]


def spi_cases() -> list[tuple[int, int, bytes]]:
    return [
        (framing.SPI_GAP9_JOB, 0, b""),
        (framing.SPI_GAP9_RESULT, framing.SPI_FLAG_ERROR, pattern(7)),
        (framing.SPI_LPWAN_REQUEST, 0, pattern(1046)),
        (framing.SPI_GAP9_JOB, framing.SPI_FLAG_MORE, pattern(7192, 4)),
    ]


def frag_cases() -> list[tuple[int, bytes, int]]:
    """(message id, message, MTU). An application appends its own messages."""
    return [
        (1, b"", framing.MTU_LORA),
        (2, pattern(1), framing.MTU_LORA),
        (3, pattern(219), framing.MTU_LORA),  # exactly one full chunk
        (4, pattern(220), framing.MTU_LORA),  # one byte over
        (5, pattern(438), framing.MTU_LORA),  # exactly two chunks
        (6, pattern(2086, 5), framing.MTU_LORA),
        (7, pattern(2086, 6), framing.MTU_BLE),
        (8, pattern(2086, 7), framing.MTU_LTE),
        (255, pattern(50), 10),
    ]


# ---- secure ----


def secure_cases() -> list[tuple[int, int | None, int, int, bytes]]:
    """(kind, node id or None, sender, counter, plaintext) of the envelope vectors. The sender is 0 for uplinks."""
    return [
        (secure.KIND_UPLINK, None, 0, 1, b""),
        (secure.KIND_UPLINK, None, 0, 2, pattern(219)),
        (secure.KIND_UPLINK, 0x0123456789ABCDEF, 0, 70000, pattern(40, 1)),
        (secure.KIND_NODE_TO_HOST, None, 0, 0xFFFFFFFF, pattern(100, 2)),
        (secure.KIND_DOWNLINK, None, 0x01020304, 1, pattern(30, 3)),
        (secure.KIND_DOWNLINK, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF, 5, pattern(12, 4)),
        (secure.KIND_SETTINGS, None, 0x0A0B0C0D, 9, pattern(77, 5)),
        (secure.KIND_DOWNLINK, None, 0x11223344, 1, pattern(30, 6)),  # same counter as above, other sender
    ]


SEAL_KEY = bytes(range(0x90, 0xA0))  # fixed test key of the sealed settings records, never used on a device
SEAL_NONCE = bytes(range(0xB0, 0xBC))


def seal_cases() -> list[tuple[str, bytes, bytes]]:
    """(setting name, nonce, plaintext) of the sealed settings records (keys at rest). Vector 7 of the design."""
    return [
        ("e2e_privx", SEAL_NONCE, bytes(range(1, 33))),  # the node private key of the test keys above
        ("e2e_rat", bytes(range(0xC0, 0xCC)), pattern(152, 9)),  # the name reserved for the ratchet state
        ("e2e_privx", bytes(12), b""),  # empty value, all-zero nonce
    ]


# ---- ratchet (forward secrecy) ----

# Fixed ephemeral keys of the test mixes (any 32 bytes are an X25519 key).
E_N_PRIV = bytes(range(0x21, 0x41))
E_C_PRIV = bytes(range(0x61, 0x81))
E_N2_PRIV = bytes(range(0xA1, 0xC1))
E_C2_PRIV = bytes(range(0xD1, 0xF1))
UP_BASE_1 = 3  # first uplink epoch of the test generation 1
UP_BASE_4 = 7  # and of the test generation 4 (the generation bits wrap)
UP_BASE_T = 1  # of the generation 1 of the transcript
RAT_SENDER = 0x01020304  # sender id of the downlinks of the transcript
X25519_BASE = bytes([9]) + bytes(31)

# Public keys of low order: X25519 gives all zero. u = 0, 1, p - 1 and the two other points of order 8 of RFC 7748.
LOW_ORDER = [
    bytes(32),
    bytes([1]) + bytes(31),
    bytes.fromhex("e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800"),
    bytes.fromhex("5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157"),
    bytes.fromhex("ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"),
]

# Error codes of sensei_secure.h that a vector expects.
ERR_FORMAT, ERR_AUTH, ERR_REPLAY, ERR_GEN, ERR_EPOCH = -1, -3, -4, -12, -13

# Operations of the transcript.
OP_SEAL_N, OP_SEAL_B, OP_SWITCH, OP_OPEN_B, OP_OPEN_N, OP_DROP_PREV = range(6)


def fixed_keys() -> secure.SessionKeys:
    """The keys of the envelope vectors (generation 0)."""
    node_priv, backend_priv = bytes(range(1, 33)), bytes(range(0x41, 0x61))
    node_pub, backend_pub = secure.public_from_private(node_priv), secure.public_from_private(backend_priv)
    return secure.derive_keys(node_priv, backend_pub, node_pub, backend_pub)


def x25519_cases() -> list[tuple[bytes, bytes, bytes]]:
    """(private, peer, result): RFC 7748 section 5.2 (two vectors) and section 6.1 (both public keys and the shared
    secret both ways)."""
    h = bytes.fromhex
    a = h("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
    b = h("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
    a_pub = h("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
    b_pub = h("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
    shared = h("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")
    cases = [
        (h("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
         h("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"),
         h("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552")),
        (h("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
         h("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493"),
         h("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957")),
        (a, X25519_BASE, a_pub),
        (b, X25519_BASE, b_pub),
        (a, b_pub, shared),
        (b, a_pub, shared),
    ]
    for priv, peer, out in cases:  # the library agrees with the RFC
        assert ratchet.x25519(priv, peer) == out
    for low in LOW_ORDER:
        try:
            ratchet.x25519(a, low)
        except ratchet.DhError:
            continue
        raise AssertionError("a low-order public key was not refused")
    return cases


@dataclass(frozen = True)
class MixCase:
    root: bytes  # R_g
    gen: int  # the new generation
    up_base: int
    e_n_priv: bytes
    e_c_priv: bytes
    e_n_pub: bytes
    e_c_pub: bytes
    dh: bytes
    mix: ratchet.Mix


def make_mix(root: bytes, key_id: int, gen: int, up_base: int, e_n_priv: bytes, e_c_priv: bytes) -> MixCase:
    e_n_pub, e_c_pub = ratchet.x25519_public(e_n_priv), ratchet.x25519_public(e_c_priv)
    node = ratchet.mix_node(root, key_id, gen, up_base, e_n_priv, e_c_pub)
    assert node == ratchet.mix_collector(root, key_id, gen, up_base, e_c_priv, e_n_pub)  # both sides agree
    return MixCase(root, gen, up_base, e_n_priv, e_c_priv, e_n_pub, e_c_pub, ratchet.x25519(e_n_priv, e_c_pub), node)


def mix_cases() -> list[MixCase]:
    """The mix of generation 0 to 1, and one to generation 4 (the generation bits wrap to 0) from the root of the first."""
    keys = fixed_keys()
    first = make_mix(ratchet.root0(keys.k_up, keys.k_down, keys.key_id), keys.key_id, 1, UP_BASE_1, E_N_PRIV, E_C_PRIV)
    return [first, make_mix(first.mix.root, keys.key_id, 4, UP_BASE_4, E_N2_PRIV, E_C2_PRIV)]


def chain_keys(ck0: bytes, n: int = 4, far: int = 1000) -> tuple[list[bytes], list[bytes], bytes]:
    """CK_0 ... CK_{n-1}, MK_0 ... MK_{n-1} and CK_far of the chain that starts at ck0."""
    cks, mks, ck = [], [], ck0
    for i in range(far):
        if i < n:
            cks.append(ck)
            mks.append(ratchet.message_key(ck))
        ck = ratchet.next_chain_key(ck)
    return cks, mks, ck


def ratchet_envelope_cases() -> list[tuple[int, int, int | None, int, int, bytes, bytes, int]]:
    """(kind, generation, node id, sender, counter, plaintext, sealed, expected result of opening). The generations
    are those of mix_cases(): 1 and 4. The first rows must open (expected 0); the rest are refused."""
    keys = fixed_keys()
    m1, m4 = mix_cases()
    ch1, ch4 = ratchet.Chain(m1.mix.ck, m1.up_base), ratchet.Chain(m4.mix.ck, m4.up_base)

    def up(chain: ratchet.Chain, gen: int, counter: int, kind: int, plain: bytes, node_id: int | None = None) -> bytes:
        return secure.seal_raw(chain.key_for(counter), keys.key_id, counter, kind, plain, node_id, gen = gen)

    def down(mix: MixCase, gen: int, counter: int, kind: int, plain: bytes, sender: int) -> bytes:
        return secure.seal_raw(mix.mix.k_down, keys.key_id, counter, kind, plain, sender = sender, gen = gen)

    U, D, N, S = secure.KIND_UPLINK, secure.KIND_DOWNLINK, secure.KIND_NODE_TO_HOST, secure.KIND_SETTINGS
    rows = []
    # (kind, gen, node id, sender, counter, plaintext, sealed)
    for kind, gen, node_id, sender, counter, plain in [
        (U, 1, None, 0, 64 * UP_BASE_1, b""),  # idx 0
        (U, 1, None, 0, 64 * (UP_BASE_1 + 5) + 17, pattern(219, 1)),  # idx 5
        (N, 1, 0x0123456789ABCDEF, 0, 64 * (UP_BASE_1 + 40) + 63, pattern(60, 2)),  # idx 40, last counter of an epoch
        (D, 1, None, 0x01020304, 1, pattern(30, 3)),
        (S, 1, None, 0x0A0B0C0D, 9, pattern(77, 4)),
        (U, 4, None, 0, 64 * UP_BASE_4 + 1, pattern(40,
                                                    5)),  # generation 4: the bits are those of 0 with the ratchet bit
        (D, 4, None, 0x11223344, 77, pattern(12, 6)),
        (N, 4, None, 0, 64 * (UP_BASE_4 + 3) + 5, pattern(100, 7)),
    ]:
        mix, chain = (m1, ch1) if gen == 1 else (m4, ch4)
        sealed = (down(mix, gen, counter, kind, plain, sender) if kind in (D, S) else up(
            chain, gen, counter, kind, plain, node_id))
        rows.append((kind, gen, node_id, sender, counter, plain, sealed, 0))
    first = rows[0][6]
    # Negatives
    bad = bytearray(first)
    bad[1] |= secure.KIND_RESERVED  # bit 6 set
    rows.append((U, 1, None, 0, 64 * UP_BASE_1, b"", bytes(bad), ERR_FORMAT))
    bad = bytearray(first)
    bad[1] = U | 0x10  # generation bits without the ratchet bit
    rows.append((U, 1, None, 0, 64 * UP_BASE_1, b"", bytes(bad), ERR_FORMAT))
    # epoch 2 is before the start of generation 1 (epoch 3): refused before the tag is looked at
    rows.append((U, 1, None, 0, 64 * (UP_BASE_1 - 1) + 5, pattern(9, 8),
                 secure.seal_raw(m1.mix.ck[:16], keys.key_id, 64 * (UP_BASE_1 - 1) + 5, U, pattern(9, 8),
                                 gen = 1), ERR_EPOCH))
    # the generation bits are authenticated: gen 1 changed to 2 has no key, changed to 4 has another key
    bad = bytearray(first)
    bad[1] ^= 0x30  # bits 01 -> 10 (generation 2)
    rows.append((U, 1, None, 0, 64 * UP_BASE_1, b"", bytes(bad), ERR_GEN))
    bad = bytearray(rows[3][6])
    bad[1] ^= 0x10  # bits 01 -> 00 with the ratchet bit: generation 4, other key
    rows.append((D, 1, None, 0x01020304, 1, pattern(30, 3), bytes(bad), ERR_AUTH))
    bad = bytearray(rows[1][6])
    bad[-1] ^= 1  # the tag
    rows.append((U, 1, None, 0, 64 * (UP_BASE_1 + 5) + 17, pattern(219, 1), bytes(bad), ERR_AUTH))
    return rows


@dataclass(frozen = True)
class Step:
    op: int
    kind: int = 0
    gen: int = 0
    sender: int = 0
    counter: int = 0
    plain: bytes = b""
    sealed: bytes = b""
    ref: int = 0  # OPEN: index of the step whose envelope is opened
    expect: int = 0  # OPEN: result


def transcript_steps() -> list[Step]:
    """The scripted run (generation 0, request, answer, late generation 0 uplink, generation 1 across epoch jumps, late
    previous epoch, previous generation downlink, previous generation dropped). The sender of an envelope is sealed
    by the step; an OPEN step names the SEAL step by index. Steps are replayed in order by the C test and by a
    Python model (tests/test_ratchet.py)."""
    keys = fixed_keys()
    mix = make_mix(ratchet.root0(keys.k_up, keys.k_down, keys.key_id), keys.key_id, 1, UP_BASE_T, E_N_PRIV, E_C_PRIV)
    chain = ratchet.Chain(mix.mix.ck, mix.up_base)
    U, D = secure.KIND_UPLINK, secure.KIND_DOWNLINK
    steps: list[Step] = []

    def seal_n(gen: int, counter: int, seed: int) -> int:
        plain = pattern(24 + seed, seed)
        key = keys.k_up if gen == 0 else chain.key_for(counter)
        steps.append(
            Step(OP_SEAL_N, U, gen, 0, counter, plain, secure.seal_raw(key, keys.key_id, counter, U, plain, gen = gen)))
        return len(steps) - 1

    def seal_b(gen: int, counter: int, seed: int) -> int:
        plain = pattern(18 + seed, seed)
        key = keys.k_down if gen == 0 else mix.mix.k_down
        steps.append(
            Step(OP_SEAL_B, D, gen, RAT_SENDER, counter, plain,
                 secure.seal_raw(key, keys.key_id, counter, D, plain, sender = RAT_SENDER, gen = gen)))
        return len(steps) - 1

    def op(code: int, ref: int = 0, expect: int = 0) -> None:
        steps.append(Step(code, ref = ref, expect = expect))

    up1, up2, up_late = seal_n(0, 1, 1), seal_n(0, 2, 2), seal_n(0, 3, 3)  # up2 carries the request
    op(OP_OPEN_B, up1)
    op(OP_OPEN_B, up2)
    ans = seal_b(0, 1, 1)  # the answer
    late1, late2 = seal_b(0, 2, 2), seal_b(0, 3, 3)  # sealed in generation 0, delivered after the node switched
    op(OP_OPEN_N, ans)
    op(OP_SWITCH)
    g1a, g1b = seal_n(1, 64 * UP_BASE_T, 4), seal_n(1, 64 * UP_BASE_T + 1, 5)  # first epoch of generation 1
    op(OP_OPEN_B, g1a)  # the collector promotes
    op(OP_OPEN_B, g1b)
    op(OP_OPEN_B, up_late)  # the late generation 0 uplink, 62 behind the highest counter
    op(OP_OPEN_B, up_late, ERR_REPLAY)
    op(OP_OPEN_N, late1)  # generation 0 downlink: the node still has the previous downlink key
    dn1 = seal_b(1, 4, 4)  # after the promotion the collector seals in generation 1
    op(OP_OPEN_N, dn1)
    e2 = seal_n(1, 128, 6)  # epoch 2, index 1
    e9 = seal_n(1, 600, 7)  # the node jumped to epoch 9 (a reboot: a new epoch)
    e10 = seal_n(1, 640, 8)  # and to epoch 10
    op(OP_OPEN_B, e2)
    op(OP_OPEN_B, e10)  # the collector walks 8 epochs
    op(OP_OPEN_B, e9)  # late, previous epoch: still inside the window
    op(OP_OPEN_B, e2, ERR_REPLAY)  # older than the window
    op(OP_OPEN_B, g1a, ERR_REPLAY)
    op(OP_DROP_PREV)
    op(OP_OPEN_N, late2, ERR_GEN)  # generation 0 downlink after the previous key is gone
    op(OP_OPEN_N, dn1, ERR_REPLAY)
    dn2 = seal_b(1, 5, 9)
    op(OP_OPEN_N, dn2)
    return steps


# ---- diag ----


def fault_cases() -> list[diag.Fault]:
    return [
        diag.Fault(),
        diag.Fault(reason = 3,
                   pc = 0x12345,
                   lr = 0x23457,
                   thread = "app_wq",
                   uptime_s = 86400,
                   last_job = 9,
                   image_id = 0xDEADBEEF,
                   boot_count = 12,
                   status = 0x82,
                   coredump = True),
        diag.Fault(reason = 0x80,
                   pc = 0xFFFFFFFF,
                   lr = 0xFFFFFFFF,
                   thread = "a" * 15,
                   uptime_s = 0xFFFFFFFF,
                   last_job = 0xFFFF,
                   image_id = 0xFFFFFFFF,
                   boot_count = 0xFFFFFFFF,
                   status = 0x0100000F),
        diag.Fault(reason = 24, pc = 0x1001, lr = 0x1003, thread = "ISR", uptime_s = 5, boot_count = 1, status = 0x1D),
    ]


def reset_causes() -> list[int]:
    return [0, 1 << 4, (1 << 1) | (1 << 24), (1 << 3) | (1 << 25), 0x1FFFF, 0x1FFFF | (1 << 24) | (1 << 25), 1 << 30]


# ---- render ----


def array(name: str, data: bytes) -> str:
    """A `static const uint8_t` array, 16 bytes per line."""
    items = [f"0x{b:02X}" for b in data] or ["0"]
    lines = ["    " + ", ".join(items[i:i + 16]) + "," for i in range(0, len(items), 16)]
    return f"static const uint8_t {name}[] = {{\n" + "\n".join(lines) + "\n};\n"


def pairs(name: str, cases: list[tuple[bytes, bytes]]) -> list[str]:
    """Arrays `<name>_in_<i>` and `<name>_out_<i>`, the table `<name>` of `vec_pair_t` and `<NAME>_COUNT`."""
    parts = []
    for i, (a, b) in enumerate(cases):
        parts.append(array(f"{name}_in_{i}", a))
        parts.append(array(f"{name}_out_{i}", b))
    rows = ",\n".join(f"    {{{name}_in_{i}, {len(a)}, {name}_out_{i}, {len(b)}}}" for i, (a, b) in enumerate(cases))
    parts.append(f"static const vec_pair_t {name}[] = {{\n{rows},\n}};\n")
    parts.append(f"#define {name.upper()}_COUNT {len(cases)}\n")
    return parts


PAIR_TYPE = ("typedef struct {\n    const uint8_t *in;\n    size_t in_len;\n    const uint8_t *out;\n"
             "    size_t out_len;\n} vec_pair_t;\n")


def frag_pairs(cases: list[tuple[int, bytes, int]]) -> list[tuple[bytes, bytes]]:
    """in = msg_id, mtu (u16 LE), message; out = chunks, each prefixed by its length (u16 LE)."""
    out = []
    for mid, msg, mtu in cases:
        chunks = framing.fragment(mid, msg, mtu)
        packed = b"".join(len(c).to_bytes(2, "little") + c for c in chunks)
        out.append((bytes([mid]) + mtu.to_bytes(2, "little") + msg, packed))
    return out


def render_link() -> str:
    parts = [
        LICENSE + " * Generated by python -m sensei_sdk.vectors. Do not edit.\n */\n",
        "#ifndef SENSEI_LINK_VECTORS_H\n#define SENSEI_LINK_VECTORS_H\n\n#include <stddef.h>\n#include <stdint.h>\n",
        PAIR_TYPE,
    ]
    parts += pairs("vec_crc16", [(d, framing.crc16(d).to_bytes(2, "little")) for d in crc_inputs()])
    parts += pairs("vec_crc32", [(d, framing.crc32(d).to_bytes(4, "little")) for d in crc_inputs()])
    parts += pairs("vec_cobs", [(d, framing.cobs_encode(d)) for d in cobs_inputs()])
    # stream: in = channel byte + payload, out = frame
    parts += pairs("vec_stream", [(bytes([c]) + p, framing.stream_encode(c, p)) for c, p in stream_cases()])
    # spi: in = type, flags, payload; out = frame
    parts += pairs("vec_spi", [(bytes([t, f]) + p, framing.spi_encode(t, p, f)) for t, f, p in spi_cases()])
    parts += pairs("vec_frag", frag_pairs(frag_cases()))
    # acknowledgement: in = msg_id, index; out = the 3 bytes
    parts += pairs("vec_ack", [(bytes([m, i]), framing.ack(m, i)) for m, i in ((0, 0), (7, 0), (0x21, 3), (255, 254))])
    parts.append("#endif\n")
    return "\n".join(parts)


def render_secure() -> str:
    node_priv = bytes(range(1, 33))
    backend_priv = bytes(range(0x41, 0x61))
    node_pub = secure.public_from_private(node_priv)
    backend_pub = secure.public_from_private(backend_priv)
    keys = secure.derive_keys(node_priv, backend_pub, node_pub, backend_pub)
    assert keys == secure.derive_keys(backend_priv, node_pub, node_pub, backend_pub)
    parts = [
        LICENSE + " * Generated by python -m sensei_sdk.vectors. Do not edit. Fixed test keys, never used on a "
        "device.\n */\n",
        "#ifndef SENSEI_SECURE_VECTORS_H\n#define SENSEI_SECURE_VECTORS_H\n\n#include <stddef.h>\n#include <stdint.h>\n",
    ]
    for name, data in [
        ("node_priv", node_priv),
        ("backend_priv", backend_priv),
        ("node_pub", node_pub),
        ("backend_pub", backend_pub),
        ("k_up", keys.k_up),
        ("k_down", keys.k_down),
    ]:
        parts.append(array(f"vec_sec_{name}", data))
    parts.append(f"#define VEC_SEC_KEY_ID 0x{keys.key_id:08X}u\n")
    parts.append("typedef struct {\n    uint8_t kind;\n    uint8_t has_node_id;\n    uint64_t node_id;\n"
                 "    uint32_t sender;\n    uint32_t counter;\n"
                 "    const uint8_t *plain;\n    size_t plain_len;\n    const uint8_t *sealed;\n"
                 "    size_t sealed_len;\n} vec_sec_t;\n")
    rows = []
    for i, (kind, node_id, sender, ctr, plain) in enumerate(secure_cases()):
        key = keys.k_up if kind in (secure.KIND_UPLINK, secure.KIND_NODE_TO_HOST) else keys.k_down
        sealed = secure.seal_raw(key, keys.key_id, ctr, kind, plain, node_id, sender)
        parts.append(array(f"vec_sec_plain_{i}", plain))
        parts.append(array(f"vec_sec_sealed_{i}", sealed))
        rows.append(f"    {{{kind}, {int(node_id is not None)}, {node_id or 0}ull, {sender}u, {ctr}u, "
                    f"vec_sec_plain_{i}, {len(plain)}, vec_sec_sealed_{i}, {len(sealed)}}}")
    parts.append("static const vec_sec_t vec_sec[] = {\n" + ",\n".join(rows) + ",\n};\n")
    parts.append(f"#define VEC_SEC_COUNT {len(rows)}\n")
    # sealed settings records (keys at rest): AES-128-GCM under a fixed key, the setting name is in the associated data
    parts.append(array("vec_seal_key", SEAL_KEY))
    parts.append("typedef struct {\n    const char *name;\n    const uint8_t *nonce;\n    const uint8_t *plain;\n"
                 "    size_t plain_len;\n    const uint8_t *record;\n    size_t record_len;\n} vec_seal_t;\n")
    rows = []
    for i, (name, nonce, plain) in enumerate(seal_cases()):
        record = secure.seal_record(SEAL_KEY, name, plain, nonce)
        parts.append(array(f"vec_seal_nonce_{i}", nonce))
        parts.append(array(f"vec_seal_plain_{i}", plain))
        parts.append(array(f"vec_seal_record_{i}", record))
        rows.append(f'    {{"{name}", vec_seal_nonce_{i}, vec_seal_plain_{i}, {len(plain)}, vec_seal_record_{i}, '
                    f"{len(record)}}}")
    parts.append("static const vec_seal_t vec_seal[] = {\n" + ",\n".join(rows) + ",\n};\n")
    parts.append(f"#define VEC_SEAL_COUNT {len(rows)}\n")
    parts += render_ratchet()
    parts.append("#endif\n")
    return "\n".join(parts)


def render_ratchet() -> list[str]:
    """Items 1 to 6 of the forward secrecy test vectors: R_0, X25519, mix, chain, generation 1 and later envelopes,
    the transcript. All names start with vec_rat_."""
    keys = fixed_keys()
    parts = [
        f"#define VEC_RAT_UP_BASE_1 {UP_BASE_1}u\n#define VEC_RAT_UP_BASE_4 {UP_BASE_4}u\n"
        f"#define VEC_RAT_UP_BASE_T {UP_BASE_T}u\n#define VEC_RAT_SENDER 0x{RAT_SENDER:08X}u\n"
    ]
    # 1. R_0
    parts.append(array("vec_rat_root0", ratchet.root0(keys.k_up, keys.k_down, keys.key_id)))
    # 2. X25519
    parts.append("typedef struct {\n    const uint8_t *priv;\n    const uint8_t *peer;\n    const uint8_t *out;\n"
                 "} vec_rat_x25519_t;\n")
    rows = []
    for i, (priv, peer, out) in enumerate(x25519_cases()):
        parts += [
            array(f"vec_rat_x_priv_{i}", priv),
            array(f"vec_rat_x_peer_{i}", peer),
            array(f"vec_rat_x_out_{i}", out)
        ]
        rows.append(f"    {{vec_rat_x_priv_{i}, vec_rat_x_peer_{i}, vec_rat_x_out_{i}}}")
    parts.append("static const vec_rat_x25519_t vec_rat_x25519[] = {\n" + ",\n".join(rows) + ",\n};\n")
    parts.append(f"#define VEC_RAT_X25519_COUNT {len(rows)}\n")
    rows = []
    for i, low in enumerate(LOW_ORDER):
        parts.append(array(f"vec_rat_low_{i}", low))
        rows.append(f"    vec_rat_low_{i}")
    parts.append("static const uint8_t *const vec_rat_low[] = {\n" + ",\n".join(rows) + ",\n};\n")
    parts.append(f"#define VEC_RAT_LOW_COUNT {len(rows)}\n")
    # 3. mix
    parts.append("typedef struct {\n    uint32_t gen;\n    uint32_t up_base;\n    const uint8_t *root;\n"
                 "    const uint8_t *e_n_priv;\n    const uint8_t *e_c_priv;\n    const uint8_t *e_n_pub;\n"
                 "    const uint8_t *e_c_pub;\n    const uint8_t *th;\n    const uint8_t *dh;\n"
                 "    const uint8_t *root_out;\n    const uint8_t *ck;\n    const uint8_t *k_down;\n} vec_rat_mix_t;\n")
    rows = []
    mixes = mix_cases()
    for i, m in enumerate(mixes):
        for name, data in [("root", m.root), ("e_n_priv", m.e_n_priv), ("e_c_priv", m.e_c_priv), ("e_n_pub", m.e_n_pub),
                           ("e_c_pub", m.e_c_pub), ("th", m.mix.th), ("dh", m.dh), ("root_out", m.mix.root),
                           ("ck", m.mix.ck), ("k_down", m.mix.k_down)]:
            parts.append(array(f"vec_rat_mix{i}_{name}", data))
        rows.append(f"    {{{m.gen}u, {m.up_base}u, " +
                    ", ".join(f"vec_rat_mix{i}_{n}" for n in ("root", "e_n_priv", "e_c_priv", "e_n_pub", "e_c_pub",
                                                              "th", "dh", "root_out", "ck", "k_down")) + "}")
    parts.append("static const vec_rat_mix_t vec_rat_mix[] = {\n" + ",\n".join(rows) + ",\n};\n")
    parts.append(f"#define VEC_RAT_MIX_COUNT {len(rows)}\n")
    # 4. chain of the first mix: CK_0 ... CK_3, MK_0 ... MK_3, CK_1000 (one array each, in order)
    cks, mks, ck_far = chain_keys(mixes[0].mix.ck)
    parts.append(array("vec_rat_chain_ck", b"".join(cks)))
    parts.append(array("vec_rat_chain_mk", b"".join(mks)))
    parts.append(array("vec_rat_chain_ck1000", ck_far))
    # 5. envelopes of generation 1 and later
    parts.append("typedef struct {\n    uint8_t kind;\n    uint8_t gen;\n    uint8_t has_node_id;\n"
                 "    uint64_t node_id;\n    uint32_t sender;\n    uint32_t counter;\n    const uint8_t *plain;\n"
                 "    size_t plain_len;\n    const uint8_t *sealed;\n    size_t sealed_len;\n"
                 "    int expect;\n} vec_rat_env_t;\n")
    rows = []
    for i, (kind, gen, node_id, sender, counter, plain, sealed, expect) in enumerate(ratchet_envelope_cases()):
        parts += [array(f"vec_rat_env_plain_{i}", plain), array(f"vec_rat_env_sealed_{i}", sealed)]
        rows.append(f"    {{{kind}, {gen}, {int(node_id is not None)}, {node_id or 0}ull, {sender}u, {counter}u, "
                    f"vec_rat_env_plain_{i}, {len(plain)}, vec_rat_env_sealed_{i}, {len(sealed)}, {expect}}}")
    parts.append("static const vec_rat_env_t vec_rat_env[] = {\n" + ",\n".join(rows) + ",\n};\n")
    parts.append(f"#define VEC_RAT_ENV_COUNT {len(rows)}\n")
    # 6. transcript
    parts.append(
        "enum {\n    VEC_RAT_OP_SEAL_N,\n    VEC_RAT_OP_SEAL_B,\n    VEC_RAT_OP_SWITCH,\n    VEC_RAT_OP_OPEN_B,\n"
        "    VEC_RAT_OP_OPEN_N,\n    VEC_RAT_OP_DROP_PREV,\n};\n")
    parts.append("typedef struct {\n    uint8_t op;\n    uint8_t kind;\n    uint8_t gen;\n    uint32_t sender;\n"
                 "    uint32_t counter;\n    const uint8_t *plain;\n    size_t plain_len;\n    const uint8_t *sealed;\n"
                 "    size_t sealed_len;\n    int ref;\n    int expect;\n} vec_rat_step_t;\n")
    names = ["SEAL_N", "SEAL_B", "SWITCH", "OPEN_B", "OPEN_N", "DROP_PREV"]
    rows = []
    for i, st in enumerate(transcript_steps()):
        parts += [array(f"vec_rat_step_plain_{i}", st.plain), array(f"vec_rat_step_sealed_{i}", st.sealed)]
        rows.append(f"    {{VEC_RAT_OP_{names[st.op]}, {st.kind}, {st.gen}, {st.sender}u, {st.counter}u, "
                    f"vec_rat_step_plain_{i}, {len(st.plain)}, vec_rat_step_sealed_{i}, {len(st.sealed)}, {st.ref}, "
                    f"{st.expect}}}")
    parts.append("static const vec_rat_step_t vec_rat_steps[] = {\n" + ",\n".join(rows) + ",\n};\n")
    parts.append(f"#define VEC_RAT_STEPS_COUNT {len(rows)}\n")
    return parts


def render_diag() -> str:
    parts = [
        LICENSE + " * Generated by python -m sensei_sdk.vectors. Do not edit.\n */\n",
        "#ifndef SENSEI_DIAG_VECTORS_H\n#define SENSEI_DIAG_VECTORS_H\n\n#include <stddef.h>\n#include <stdint.h>\n",
        "typedef struct {\n    uint8_t reason;\n    uint8_t flags;\n    uint16_t last_job;\n    uint32_t pc;\n"
        "    uint32_t lr;\n    uint32_t status;\n    uint32_t uptime_s;\n    uint32_t image_id;\n"
        "    uint32_t boot_count;\n    const char *thread;\n    const uint8_t *packed;\n} vec_fault_t;\n",
    ]
    rows = []
    for i, f in enumerate(fault_cases()):
        parts.append(array(f"vec_fault_packed_{i}", diag.pack_fault(f)))
        rows.append(f'    {{{f.reason}, {int(f.coredump)}, {f.last_job}, {f.pc}u, {f.lr}u, {f.status}u, '
                    f'{f.uptime_s}u, {f.image_id}u, {f.boot_count}u, "{f.thread}", vec_fault_packed_{i}}}')
    parts.append("static const vec_fault_t vec_faults[] = {\n" + ",\n".join(rows) + ",\n};\n")
    parts.append(f"#define VEC_FAULTS_COUNT {len(rows)}\n")
    # the text of a reset cause, and the name of every reason code
    parts.append("typedef struct {\n    uint32_t cause;\n    const char *text;\n} vec_reset_t;\n")
    rows = [f'    {{0x{c:08X}u, "{diag.reset_text(c)}"}}' for c in reset_causes()]
    parts.append("static const vec_reset_t vec_resets[] = {\n" + ",\n".join(rows) + ",\n};\n")
    parts.append(f"#define VEC_RESETS_COUNT {len(rows)}\n")
    names = ",\n".join(f'    "{diag.reason_name(r)}"' for r in range(256))
    parts.append(f"static const char *const vec_reason_names[256] = {{\n{names},\n}};\n")
    parts.append("#endif\n")
    return "\n".join(parts)


def outputs(root: Path) -> list[tuple[Path, str]]:
    return [(root / LINK_HEADER, render_link()), (root / SECURE_HEADER, render_secure()),
            (root / DIAG_HEADER, render_diag())]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog = "python -m sensei_sdk.vectors", description = __doc__.split("\n")[0])
    ap.add_argument("--check", action = "store_true", help = "fail if a committed header is out of date")
    ap.add_argument("--sdk-root",
                    type = Path,
                    default = SDK_ROOT,
                    help = "the SDK checkout (default: the one of this file)")
    args = ap.parse_args(argv)
    outs = outputs(args.sdk_root)
    if args.check:
        bad = [p for p, text in outs if not p.exists() or p.read_text() != text]
        if bad:
            print(f"{', '.join(str(p) for p in bad)} out of date: run python -m sensei_sdk.vectors", file = sys.stderr)
            return 1
        print("vectors are current")
        return 0
    for path, text in outs:
        path.parent.mkdir(parents = True, exist_ok = True)
        path.write_text(text)
        print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
