# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Test vectors V9 to V14 of the commissioning (onboarding design, step A). `sensei_sdk.vectors` writes them to
lib/sensei_secure/tests/generated/vectors_commission.h, next to the other vectors; the C side reads the same file.

    V9   RFC 9383 appendix C, P256-SHA256-HKDF-SHA256-HMAC-SHA256 (published values), the constants M and N, scalar
         times G, reduction of 40 bytes mod n, invalid points
    V10  scrypt registration with fake codes: password input, scrypt output, w0, w1, L, the 121 B verifier record
    V11  context, transcript and keys of full sessions with fixed ephemeral scalars
    V12  kind 5 envelopes of the channel, in order, with the replay and format negatives
    V13  fingerprints and the proof of possession
    V14  Verhoeff check digit, QR payload, label text, strings that must not parse

The codes are fake (all the same digit or an ascending run: the bench never makes them). Nothing here is a secret.
"""

from __future__ import annotations

from dataclasses import dataclass

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from . import commission as com
from . import p256, secure
from .vectors import ERR_AUTH, ERR_FORMAT, ERR_REPLAY, LICENSE, array, fixed_keys, pattern

_h = bytes.fromhex

# RFC 9383 appendix C, first block (P-256, SHA-256): the values as published. Context, idProver and idVerifier are the
# strings printed above them in the RFC; w0 and w1 are given directly (the choice of PBKDF is omitted there).
RFC_CONTEXT = b"SPAKE2+-P256-SHA256-HKDF-SHA256-HMAC-SHA256 Test Vectors"
RFC_ID_PROVER = b"client"
RFC_ID_VERIFIER = b"server"
RFC9383_P256 = {
    "w0":
        _h("bb8e1bbcf3c48f62c08db243652ae55d3e5586053fca77102994f23ad95491b3"),
    "w1":
        _h("7e945f34d78785b8a3ef44d0df5a1a97d6b3b460409a345ca7830387a74b1dba"),
    "l":
        _h("04eb7c9db3d9a9eb1f8adab81b5794c1f13ae3e225efbe91ea487425854c7fc0"
           "0f00bfedcbd09b2400142d40a14f2064ef31dfaa903b91d1faea7093d835966e"
           "fd"),
    "x":
        _h("d1232c8e8693d02368976c174e2088851b8365d0d79a9eee709c6a05a2fad539"),
    "share_p":
        _h("04ef3bd051bf78a2234ec0df197f7828060fe9856503579bb1733009042c15c0"
           "c1de127727f418b5966afadfdd95a6e4591d171056b333dab97a79c7193e3417"
           "27"),
    "y":
        _h("717a72348a182085109c8d3917d6c43d59b224dc6a7fc4f0483232fa6516d8b3"),
    "share_v":
        _h("04c0f65da0d11927bdf5d560c69e1d7d939a05b0e88291887d679fcadea75810"
           "fb5cc1ca7494db39e82ff2f50665255d76173e09986ab46742c798a9a68437b0"
           "48"),
    "z":
        _h("04bbfce7dd7f277819c8da21544afb7964705569bdf12fb92aa388059408d500"
           "91a0c5f1d3127f56813b5337f9e4e67e2ca633117a4fbd559946ab474356c418"
           "39"),
    "v":
        _h("0458bf27c6bca011c9ce1930e8984a797a3419797b936629a5a937cf2f11c8b9"
           "514b82b993da8a46e664f23db7c01edc87faa530db01c2ee405230b18997f16b"
           "68"),
    "tt":
        _h("38000000000000005350414b45322b2d503235362d5348413235362d484b4446"
           "2d5348413235362d484d41432d534841323536205465737420566563746f7273"
           "0600000000000000636c69656e74060000000000000073657276657241000000"
           "0000000004886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497"
           "333d8fa12f5ff355163e43ce224e0b0e65ff02ac8e5c7be09419c785e0ca547d"
           "55a12e2d20410000000000000004d8bbd6c639c62937b04d997f38c3770719c6"
           "29d7014d49a24b4f98baa1292b4907d60aa6bfade45008a636337f5168c64d9b"
           "d36034808cd564490b1e656edbe7410000000000000004ef3bd051bf78a2234e"
           "c0df197f7828060fe9856503579bb1733009042c15c0c1de127727f418b5966a"
           "fadfdd95a6e4591d171056b333dab97a79c7193e341727410000000000000004"
           "c0f65da0d11927bdf5d560c69e1d7d939a05b0e88291887d679fcadea75810fb"
           "5cc1ca7494db39e82ff2f50665255d76173e09986ab46742c798a9a68437b048"
           "410000000000000004bbfce7dd7f277819c8da21544afb7964705569bdf12fb9"
           "2aa388059408d50091a0c5f1d3127f56813b5337f9e4e67e2ca633117a4fbd55"
           "9946ab474356c4183941000000000000000458bf27c6bca011c9ce1930e8984a"
           "797a3419797b936629a5a937cf2f11c8b9514b82b993da8a46e664f23db7c01e"
           "dc87faa530db01c2ee405230b18997f16b682000000000000000bb8e1bbcf3c4"
           "8f62c08db243652ae55d3e5586053fca77102994f23ad95491b3"),
    "k_main":
        _h("4c59e1ccf2cfb961aa31bd9434478a1089b56cd11542f53d3576fb6c2a438a29"),
    "k_confirm_p":
        _h("871ae3f7b78445e34438fb284504240239031c39d80ac23eb5ab9be5ad6db58a"),
    "k_confirm_v":
        _h("ccd53c7c1fa37b64a462b40db8be101cedcf838950162902054e644b400f1680"),
    "confirm_p":
        _h("926cc713504b9b4d76c9162ded04b5493e89109f6d89462cd33adc46fda27527"),
    "confirm_v":
        _h("9747bcc4f8fe9f63defee53ac9b07876d907d55047e6ff2def2e7529089d3e68"),
    "k_shared":
        _h("0c5f8ccd1413423a54f6c1fb26ff01534a87f893779c6e68666d772bfd91f3e7"),
}

# ---- V9: points ----

# Scalars whose multiple of G is listed (the C backends check scalar * G, here from the p256 module, which the tests
# compare with the `cryptography` package).
SCALARS = [
    (1).to_bytes(32, "big"),
    (2).to_bytes(32, "big"),
    (3).to_bytes(32, "big"),
    (p256.N - 1).to_bytes(32, "big"),
    (1 << 255).to_bytes(32, "big"),
    RFC9383_P256["x"],
    RFC9383_P256["y"],
    bytes(range(1, 33)),
]


def reduce_inputs() -> list[bytes]:
    """40 byte inputs of the reduction mod n (OS2IP, big endian)."""
    n = p256.N
    return [
        bytes(40),
        n.to_bytes(40, "big"),
        (n + 1).to_bytes(40, "big"),
        (n - 1).to_bytes(40, "big"),
        (2 * n + 5).to_bytes(40, "big"),
        b"\xff" * 40,
        pattern(40, 2),
        bytes(8) + RFC9383_P256["x"],
    ]


def bad_points() -> list[tuple[str, bytes]]:
    """Encodings that a verifier must refuse: (reason, bytes)."""
    g = p256.encode(p256.G)
    x, y = g[1:33], g[33:]
    off = g[:64] + bytes([g[64] ^ 1])  # y changed: not on the curve
    p = p256.P.to_bytes(32, "big")
    return [
        ("infinity", b"\x00"),
        ("empty", b""),
        ("compressed", b"\x02" + x),
        ("too short", g[:64]),
        ("too long", g + b"\x00"),
        ("bad prefix", b"\x05" + x + y),
        ("hybrid prefix", b"\x06" + x + y),
        ("zero point", b"\x04" + bytes(64)),
        ("off curve", off),
        ("x is p", b"\x04" + p + y),
        ("y is p", b"\x04" + x + p),
        ("x and y swapped", b"\x04" + y + x),
    ]


# ---- V10: registration ----


@dataclass
class Reg:
    code: str
    node_id: int
    salt: bytes
    log2n: int
    params: com.Params
    pwin: bytes
    dk: bytes
    w0: int
    w1: int
    verifier: com.Verifier


def reg_cases() -> list[Reg]:
    """Fake codes (never drawn by `generate_code`), fixed salts. Case 1 and 2 use the lowest allowed cost."""
    raw = [
        ("00000000000", 0x00A1B2C3D4E5F607, bytes(range(0x10, 0x20)), 15),
        ("11111111111", 0x0123456789ABCDEF, pattern(16, 3), 14),
        ("01234567890", 0x0000000000000001, bytes(range(0xF0, 0x100)), 14),
    ]
    out = []
    for digits, node_id, salt, log2n in raw:
        code = digits + str(com.verhoeff_digit(digits))
        params = com.Params(salt, log2n = log2n)
        w0, w1 = com.derive_w(code, node_id, params)
        out.append(
            Reg(code, node_id, salt, log2n, params, com.pwin(com.password(code), node_id),
                com.scrypt_output(code, node_id, params), w0, w1, com.make_verifier(code, node_id, salt, log2n)))
    return out


# ---- V11: sessions ----


def compute_session(w0: int, w1: int, x: bytes, y: bytes, ctx: bytes, id_p: bytes, id_v: bytes) -> dict:
    """RFC 9383 from the formulas, with the p256 functions, for fixed x and y: the prover's view. The sessions of
    `commission` must give the same values (the tests compare them)."""
    xs, ys = int.from_bytes(x, "big") % p256.N, int.from_bytes(y, "big") % p256.N
    m, n = p256.decode(com.M), p256.decode(com.N)
    share_p = p256.add(p256.mul(xs, p256.G), p256.mul(w0, m))
    share_v = p256.add(p256.mul(ys, p256.G), p256.mul(w0, n))
    rest = p256.add(share_v, p256.neg(p256.mul(w0, n)))
    z, v = p256.mul(xs, rest), p256.mul(w1, rest)
    sp, sv = p256.encode(share_p), p256.encode(share_v)
    tt = com.transcript(ctx, id_p, id_v, sp, sv, p256.encode(z), p256.encode(v), w0.to_bytes(32, "big"))
    sch = com.key_schedule(tt, sp, sv)
    return {"share_p": sp, "share_v": sv, "z": p256.encode(z), "v": p256.encode(v), "tt": tt, "sch": sch}


@dataclass
class Ses:
    reg: int
    node_id: int
    nonce_g: bytes
    nonce_n: bytes
    x: bytes
    y: bytes
    context: bytes
    values: dict
    keys: com.ChannelKeys


def ses_cases() -> list[Ses]:
    regs = reg_cases()
    raw = [
        (0, bytes(range(0x30, 0x40)), bytes(range(0x50, 0x60)), bytes(range(1, 33)), bytes(range(0x41, 0x61))),
        (1, pattern(16, 5), pattern(16, 6), bytes(range(0x80, 0xA0)), bytes(range(0xC0, 0xE0))),
    ]
    out = []
    for i, ng, nn, x, y in raw:
        r = regs[i]
        ctx = com.context(r.params, ng, nn)
        vals = compute_session(r.w0, r.w1, x, y, ctx, com.ID_PROVER, com.id_verifier(r.node_id))
        keys = com.channel_keys(vals["sch"].k_shared, r.node_id, ng, nn)
        out.append(Ses(i, r.node_id, ng, nn, x, y, ctx, vals, keys))
    return out


# ---- V12: channel ----

ROLE_PROVER, ROLE_VERIFIER = 0, 1  # who receives the frame


def _manual(key: bytes, sid: int, counter: int, sender: int, plain: bytes) -> bytes:
    """A kind 5 envelope that `seal_raw` refuses to make (counter 0, sender 2): a valid tag, wrong content."""
    direction = secure.DIR_DOWN if sender else secure.DIR_UP
    header = bytes([secure.MARKER |
                    (secure.VERSION << 2) | direction, secure.KIND_COMMISSION]) + sid.to_bytes(4, "little")
    header += (sender.to_bytes(4, "little") if direction else b"") + counter.to_bytes(4, "little")
    return header + AESGCM(key).encrypt(secure._nonce(direction, sender, counter), plain, header)


def channel_result(ch: com.Channel, envelope: bytes) -> tuple[int, bytes | None]:
    """What a receiver answers: (0, plaintext) accepted, else (code, None): -1 format, -3 authentication, -4 replay
    (the codes of sensei_secure.h)."""
    try:
        return 0, ch.open(envelope)
    except secure.ReplayError:
        return ERR_REPLAY, None
    except secure.SecureError as exc:
        return (ERR_AUTH if "authentication" in str(exc) else ERR_FORMAT), None


def channel_cases() -> list[tuple[int, bytes, bytes, int, str]]:
    """(receiver, plaintext, envelope, expected result, what), in the order a receiver sees them. Each receiver starts
    with an empty replay window and a refused frame leaves it as it was. The envelopes are those of the first session of
    V11 (keys and session id there). The generator replays the table through `Channel` and stops if one result differs."""
    k = ses_cases()[0].keys
    c2n, n2c = com.Channel(k, com.PROVER), com.Channel(k, com.VERIFIER)  # the senders
    out: list[tuple[int, bytes, bytes, int, str]] = []

    def add(role: int, plain: bytes, env: bytes, expect: int, what: str) -> None:
        out.append((role, plain, env, expect, what))

    def flip(env: bytes, pos: int) -> bytes:
        b = bytearray(env)
        b[pos] ^= 0x01
        return bytes(b)

    def put(env: bytes, pos: int, value: int) -> bytes:
        return env[:pos] + bytes([value]) + env[pos + 1:]

    V, P = ROLE_VERIFIER, ROLE_PROVER
    # commissioner to node: header 14 bytes (flags, kind, sid, sender, counter), then ciphertext and tag
    e1 = c2n.seal(b"get identity", 1)
    add(V, b"get identity", e1, 0, "first frame")
    add(V, b"get identity", e1, ERR_REPLAY, "replay of the first frame")
    add(V, pattern(40), c2n.seal(pattern(40), 3), 0, "counter jumps ahead")
    e2 = c2n.seal(b"", 2)
    add(V, b"", e2, 0, "late frame inside the window")
    add(V, b"", e2, ERR_REPLAY, "replay inside the window")
    add(V, pattern(290, 1), c2n.seal(pattern(290, 1), 67), 0, "largest envelope, 320 bytes")
    add(V, pattern(40), c2n.seal(pattern(40), 3), ERR_REPLAY, "counter 64 behind the highest")
    e68 = c2n.seal(b"next", 68)
    add(V, b"next", flip(e68, len(e68) - 1), ERR_AUTH, "tag changed")
    add(V, b"next", flip(e68, 10), ERR_AUTH, "counter changed in the header (68 to 69)")
    add(V, b"next", flip(e68, 14), ERR_AUTH, "ciphertext changed")
    add(V, b"next", e68[:-1], ERR_AUTH, "tag cut short")
    add(V, b"next", e68[:20], ERR_FORMAT, "truncated below header and tag")
    add(V, b"next", secure.seal_raw(k.k_c2n, k.sid ^ 1, 68, secure.KIND_COMMISSION, b"next", sender = 1), ERR_FORMAT,
        "other session id")
    add(V, b"x", n2c.seal(b"x", 5), ERR_FORMAT, "frame in the direction of the node itself")
    add(V, b"next", secure.seal_raw(k.k_c2n, k.sid, 68, secure.KIND_DOWNLINK, b"next", sender = 1), ERR_FORMAT,
        "kind 2 under the channel key")
    add(V, b"next", put(e68, 1, e68[1] | 0x80), ERR_FORMAT, "ratchet bit set")
    add(V, b"next", put(e68, 1, e68[1] | 0x40), ERR_FORMAT, "reserved bit set")
    add(V, b"next", put(e68, 0, e68[0] | 0x02), ERR_FORMAT, "node id flag set")
    add(V, b"next", _manual(k.k_c2n, k.sid, 68, 2, b"next"), ERR_FORMAT, "sender 2")
    add(V, b"next", _manual(k.k_c2n, k.sid, 0, 1, b"next"), ERR_REPLAY, "counter 0")
    add(V, pattern(291), _manual(k.k_c2n, k.sid, 69, 1, pattern(291)), ERR_FORMAT, "envelope of 321 bytes")
    add(V, b"next", e68, 0, "the frame that was damaged above, unchanged")
    add(V, b"last", c2n.seal(b"last", 0xFFFFFFFF), 0, "largest counter")
    add(V, b"next", e68, ERR_REPLAY, "everything before is too old now")
    # node to commissioner: header 10 bytes (no sender)
    n1 = n2c.seal(pattern(100, 4), 1)
    add(P, pattern(100, 4), n1, 0, "first frame")
    add(P, pattern(100, 4), n1, ERR_REPLAY, "replay")
    add(P, b"", c2n.seal(b"", 9), ERR_FORMAT, "frame in the direction of the commissioner itself")
    add(P, b"x", flip(n2c.seal(b"x", 2), 6), ERR_AUTH, "counter changed in the header (2 to 3)")
    add(P, b"result", n2c.seal(b"result", 2), 0, "next frame")
    add(P, b"x", secure.seal_raw(k.k_n2c, k.sid, 3, secure.KIND_NODE_TO_HOST, b"x"), ERR_FORMAT, "kind 3, not 5")
    add(P, b"x", secure.seal_raw(k.k_c2n, k.sid, 3, secure.KIND_COMMISSION, b"x"), ERR_AUTH,
        "key of the other direction")
    # self check: the table is what the code does
    rx = {P: com.Channel(k, com.PROVER), V: com.Channel(k, com.VERIFIER)}
    for role, plain, env, expect, what in out:
        got, opened = channel_result(rx[role], env)
        assert got == expect, f"vector '{what}': {got}, expected {expect}"
        assert opened in (None, plain), f"vector '{what}': plaintext"
    return out


# ---- V13: fingerprints and proof ----


def fp_cases() -> list[tuple[int, bytes]]:
    """(node id, node public key). The third uses another key and the largest id."""
    node_pub = secure.public_from_private(bytes(range(1, 33)))
    backend_pub = secure.public_from_private(bytes(range(0x41, 0x61)))
    return [(0x00A1B2C3D4E5F607, node_pub), (0x0123456789ABCDEF, node_pub), (0xFFFFFFFFFFFFFFFF, backend_pub)]


def proof_cases() -> list[tuple[int, bytes, bytes, int]]:
    """(node id, nonce_G, nonce_N, counter) of the proofs, made with the fixed keys of the envelope vectors."""
    return [
        (0x00A1B2C3D4E5F607, bytes(range(0x30, 0x40)), bytes(range(0x50, 0x60)), 1),
        (0x0123456789ABCDEF, pattern(16, 5), pattern(16, 6), 70000),
    ]


# ---- V14: Verhoeff, QR ----

VERHOEFF_INPUTS = [
    "", "0", "236", "00000000000", "11111111111", "01234567890", "12345678901", "99999999999", "57284619305",
    "31415926535"
]


def qr_cases() -> list[tuple[int, str, bytes]]:
    regs = reg_cases()
    fps = [com.node_fingerprint(n, pub) for n, pub in fp_cases()]
    return [(regs[0].node_id, regs[0].code, fps[0]), (regs[1].node_id, regs[1].code, fps[1]),
            (regs[2].node_id, regs[2].code, fps[2])]


def bad_codes() -> list[str]:
    """Twelve digit strings with one wrong digit or two swapped neighbours, made from a valid code: all must fail the
    check digit."""
    code = "31415926535" + str(com.verhoeff_digit("31415926535"))
    out = []
    for pos in (0, 5, 10, 11):
        out.append(code[:pos] + str((int(code[pos]) + 1) % 10) + code[pos + 1:])
        out.append(code[:pos] + str((int(code[pos]) + 9) % 10) + code[pos + 1:])
    for pos in (0, 4, 9, 10):
        assert code[pos] != code[pos + 1]
        out.append(code[:pos] + code[pos + 1] + code[pos] + code[pos + 2:])
    return out


def bad_qr() -> list[tuple[str, str]]:
    """(reason, text) that `parse_qr` and the C parser refuse."""
    n, code, fp = qr_cases()[0]
    good = com.qr_payload(n, code, fp)
    return [
        ("empty", ""),
        ("other version", good.replace("SN1", "SN2", 1)),
        ("lower case prefix", good.replace("SN1", "sn1", 1)),
        ("lower case hex", good.lower().replace("sn1", "SN1", 1)),
        ("missing field", good.rsplit(":", 1)[0]),
        ("extra field", good + ":00"),
        ("short node id", good.replace(f"{n:016X}", f"{n:015X}", 1)),
        ("short fingerprint", good[:-1]),
        ("long fingerprint", good + "0"),
        ("dashes in the code", good.replace(code, com.format_code(code), 1)),
        ("eleven digits", good.replace(f":{code}:", f":{code[:-1]}:", 1)),
        ("wrong check digit", good.replace(f":{code}:", f":{code[:-1]}{(int(code[-1]) + 1) % 10}:", 1)),
        ("not hex", good.replace(f"{n:016X}", "G" + f"{n:016X}"[1:], 1)),
        ("letter in the code", good.replace(f":{code}:", f":{code[:-1]}A:", 1)),
        ("space inside", good.replace(":", " ", 1)),
    ]


# ---- render ----


def _c(text: str) -> str:
    assert text.isascii() and '"' not in text and "\\" not in text and "\n" not in text
    return f'"{text}"'


def _table(name: str, ctype: str, rows: list[str]) -> list[str]:
    return [
        f"static const {ctype} {name}[] = {{\n" + ",\n".join(f"    {r}" for r in rows) + ",\n};\n",
        f"#define {name.upper()}_COUNT {len(rows)}\n"
    ]


def render() -> str:
    parts = [
        (LICENSE + " * Generated by python -m sensei_sdk.vectors. Do not edit. Commissioning vectors V9 to V14 of the "
         "onboarding\n * design (sensei_sdk/vectors_commission.py has what each one holds). Fake codes and fixed test keys "
         "only.\n */\n"),
        ("#ifndef SENSEI_SECURE_VECTORS_COMMISSION_H\n#define SENSEI_SECURE_VECTORS_COMMISSION_H\n\n"
         "#include <stddef.h>\n#include <stdint.h>\n"),
    ]
    # V9
    parts.append(
        "/* V9: RFC 9383 appendix C, P256-SHA256-HKDF-SHA256-HMAC-SHA256 (the first block of the appendix) */\n")
    parts.append(array("vec_com_m", com.M))
    parts.append(array("vec_com_n", com.N))
    parts.append(array("vec_com_rfc_context", RFC_CONTEXT))
    parts.append(array("vec_com_rfc_id_prover", RFC_ID_PROVER))
    parts.append(array("vec_com_rfc_id_verifier", RFC_ID_VERIFIER))
    for name, data in RFC9383_P256.items():
        parts.append(array(f"vec_com_rfc_{name}", data))
    parts.append(f"#define VEC_COM_RFC_TT_LEN {len(RFC9383_P256['tt'])}\n")
    parts.append("/* scalar (32 B big endian) times G, 65 B point */\ntypedef struct {\n    const uint8_t *k;\n"
                 "    const uint8_t *point;\n} vec_com_mul_t;\n")
    rows = []
    for i, k in enumerate(SCALARS):
        parts += [
            array(f"vec_com_mul_k_{i}", k),
            array(f"vec_com_mul_pt_{i}", p256.encode(p256.mul(int.from_bytes(k, "big"), p256.G)))
        ]
        rows.append(f"{{vec_com_mul_k_{i}, vec_com_mul_pt_{i}}}")
    parts += _table("vec_com_mul", "vec_com_mul_t", rows)
    parts.append("/* 40 bytes big endian reduced mod n, 32 B big endian */\ntypedef struct {\n    const uint8_t *in;\n"
                 "    const uint8_t *out;\n} vec_com_red_t;\n")
    rows = []
    for i, data in enumerate(reduce_inputs()):
        out = (int.from_bytes(data, "big") % p256.N).to_bytes(32, "big")
        parts += [array(f"vec_com_red_in_{i}", data), array(f"vec_com_red_out_{i}", out)]
        rows.append(f"{{vec_com_red_in_{i}, vec_com_red_out_{i}}}")
    parts += _table("vec_com_red", "vec_com_red_t", rows)
    parts.append("/* encodings that a point check must refuse */\ntypedef struct {\n    const char *what;\n"
                 "    const uint8_t *data;\n    size_t len;\n} vec_com_bad_t;\n")
    rows = []
    for i, (what, data) in enumerate(bad_points()):
        try:
            p256.decode(data)
        except p256.PointError:
            pass
        else:
            raise AssertionError(f"bad point '{what}' decodes")
        parts.append(array(f"vec_com_bad_{i}", data))
        rows.append(f"{{{_c(what)}, vec_com_bad_{i}, {len(data)}}}")
    parts += _table("vec_com_bad", "vec_com_bad_t", rows)
    # V10
    parts.append(
        "/* V10: scrypt registration (fake codes, fixed salts): code, node id, cost, salt, password input, scrypt\n"
        " * output (80 B), w0, w1, L, the 121 B verifier record */\n"
        "typedef struct {\n    const char *code;\n    uint64_t node_id;\n    uint8_t log2n;\n"
        "    const uint8_t *salt;\n    const uint8_t *pwin;\n    size_t pwin_len;\n    const uint8_t *dk;\n"
        "    const uint8_t *w0;\n    const uint8_t *w1;\n    const uint8_t *l;\n    const uint8_t *record;\n"
        "} vec_com_reg_t;\n")
    rows = []
    for i, r in enumerate(reg_cases()):
        w0, w1 = r.w0.to_bytes(32, "big"), r.w1.to_bytes(32, "big")
        assert r.verifier.w0 == w0 and r.verifier.l == p256.encode(p256.mul(r.w1, p256.G))
        for name, data in [("salt", r.salt), ("pwin", r.pwin), ("dk", r.dk), ("w0", w0), ("w1", w1),
                           ("l", r.verifier.l), ("record", r.verifier.encode())]:
            parts.append(array(f"vec_com_reg{i}_{name}", data))
        rows.append(
            f"{{{_c(r.code)}, {r.node_id}ull, {r.log2n}, vec_com_reg{i}_salt, vec_com_reg{i}_pwin, {len(r.pwin)}, "
            f"vec_com_reg{i}_dk, vec_com_reg{i}_w0, vec_com_reg{i}_w1, vec_com_reg{i}_l, vec_com_reg{i}_record}}")
    parts += _table("vec_com_reg", "vec_com_reg_t", rows)
    # V11
    parts.append(
        "/* V11: sessions of the registrations above with fixed x (prover) and y (verifier): context, shares, Z, V,\n"
        " * transcript, key schedule, channel keys */\n"
        "typedef struct {\n    int reg;\n    uint64_t node_id;\n    const uint8_t *nonce_g;\n"
        "    const uint8_t *nonce_n;\n    const uint8_t *context;\n    const uint8_t *x;\n"
        "    const uint8_t *share_p;\n    const uint8_t *y;\n    const uint8_t *share_v;\n    const uint8_t *z;\n"
        "    const uint8_t *v;\n    const uint8_t *tt;\n    size_t tt_len;\n    const uint8_t *k_main;\n"
        "    const uint8_t *k_confirm_p;\n    const uint8_t *k_confirm_v;\n    const uint8_t *confirm_p;\n"
        "    const uint8_t *confirm_v;\n    const uint8_t *k_shared;\n    const uint8_t *k_c2n;\n"
        "    const uint8_t *k_n2c;\n    uint32_t sid;\n} vec_com_ses_t;\n")
    rows = []
    for i, ses in enumerate(ses_cases()):
        v, sch = ses.values, ses.values["sch"]
        items = [("nonce_g", ses.nonce_g), ("nonce_n", ses.nonce_n), ("context", ses.context), ("x", ses.x),
                 ("share_p", v["share_p"]), ("y", ses.y), ("share_v", v["share_v"]), ("z", v["z"]), ("v", v["v"]),
                 ("tt", v["tt"]), ("k_main", sch.k_main), ("k_confirm_p", sch.k_confirm_p),
                 ("k_confirm_v", sch.k_confirm_v), ("confirm_p", sch.confirm_p), ("confirm_v", sch.confirm_v),
                 ("k_shared", sch.k_shared), ("k_c2n", ses.keys.k_c2n), ("k_n2c", ses.keys.k_n2c)]
        for name, data in items:
            parts.append(array(f"vec_com_ses{i}_{name}", data))
        names = [n for n, _ in items]
        cells = {n: f"vec_com_ses{i}_{n}" for n in names}
        row = [str(ses.reg), f"{ses.node_id}ull"] + [cells[n] for n in names[:10]] + [str(len(v["tt"]))]
        row += [cells[n] for n in names[10:]] + [f"0x{ses.keys.sid:08X}u"]
        rows.append("{" + ", ".join(row) + "}")
    parts += _table("vec_com_ses", "vec_com_ses_t", rows)
    # V12
    parts.append(
        "/* V12: kind 5 envelopes of the first session. role: 0 the commissioner receives, 1 the node receives.\n"
        " * Receivers start empty and are fed in table order; expect: 0 accepted (plaintext as given), -1 format,\n"
        " * -3 authentication, -4 replay. A refused frame changes nothing. */\n"
        "typedef struct {\n    uint8_t role;\n    const uint8_t *plain;\n    size_t plain_len;\n"
        "    const uint8_t *env;\n    size_t env_len;\n    int expect;\n    const char *what;\n} vec_com_env_t;\n")
    rows = []
    for i, (role, plain, env, expect, what) in enumerate(channel_cases()):
        parts += [array(f"vec_com_env_plain_{i}", plain), array(f"vec_com_env_{i}", env)]
        rows.append(
            f"{{{role}, vec_com_env_plain_{i}, {len(plain)}, vec_com_env_{i}, {len(env)}, {expect}, {_c(what)}}}")
    parts += _table("vec_com_env", "vec_com_env_t", rows)
    # V13
    keys = fixed_keys()
    parts.append(
        "/* V13: fingerprint = SHA-256(label || node id || node public key)[0:16]; text = first 8 bytes in groups */\n"
        "typedef struct {\n    uint64_t node_id;\n    const uint8_t *node_pub;\n    const uint8_t *fp;\n"
        "    const char *text;\n} vec_com_fp_t;\n")
    rows = []
    for i, (node_id, pub) in enumerate(fp_cases()):
        fp = com.node_fingerprint(node_id, pub)
        parts += [array(f"vec_com_fp_pub_{i}", pub), array(f"vec_com_fp_{i}", fp)]
        rows.append(f"{{{node_id}ull, vec_com_fp_pub_{i}, vec_com_fp_{i}, {_c(com.fingerprint_text(fp))}}}")
    parts += _table("vec_com_fp", "vec_com_fp_t", rows)
    parts.append("/* collector fingerprint = SHA-256(label || collector public key)[0:8], of the public key below */\n")
    backend_pub = secure.public_from_private(bytes(range(0x41, 0x61)))
    parts += [array("vec_com_cfp_pub", backend_pub), array("vec_com_cfp", com.collector_fingerprint(backend_pub))]
    parts.append(
        "/* proof of possession: a kind 3 envelope under k_up (no node id field) with the plaintext given */\n")
    parts.append(array("vec_com_pop_k_up", keys.k_up))
    parts.append(f"#define VEC_COM_POP_KEY_ID 0x{keys.key_id:08X}u\n")
    parts.append("typedef struct {\n    uint64_t node_id;\n    const uint8_t *nonce_g;\n    const uint8_t *nonce_n;\n"
                 "    uint32_t counter;\n    const uint8_t *plain;\n    size_t plain_len;\n    const uint8_t *proof;\n"
                 "    size_t proof_len;\n} vec_com_pop_t;\n")
    rows = []
    for i, (node_id, ng, nn, counter) in enumerate(proof_cases()):
        plain = com.proof_plain(node_id, ng, nn)
        proof = com.make_proof(keys, node_id, ng, nn, counter)
        parts += [
            array(f"vec_com_pop_ng_{i}", ng),
            array(f"vec_com_pop_nn_{i}", nn),
            array(f"vec_com_pop_plain_{i}", plain),
            array(f"vec_com_pop_{i}", proof)
        ]
        rows.append(f"{{{node_id}ull, vec_com_pop_ng_{i}, vec_com_pop_nn_{i}, {counter}u, vec_com_pop_plain_{i}, "
                    f"{len(plain)}, vec_com_pop_{i}, {len(proof)}}}")
    parts += _table("vec_com_pop", "vec_com_pop_t", rows)
    # V14
    parts.append(
        "/* V14: Verhoeff check digit of a digit string (\"\" gives 0), then the same inputs as 12 digit codes */\n"
        "typedef struct {\n    const char *digits;\n    uint8_t check;\n} vec_com_vh_t;\n")
    rows = [f"{{{_c(d)}, {com.verhoeff_digit(d)}}}" for d in VERHOEFF_INPUTS]
    parts += _table("vec_com_vh", "vec_com_vh_t", rows)
    parts.append("/* 12 digit strings with a wrong digit or two swapped neighbours: the check must fail */\n")
    for d in bad_codes():
        assert not com.verhoeff_valid(d)
    parts.append("static const char *const vec_com_bad_code[] = {\n" + ",\n".join(f"    {_c(d)}" for d in bad_codes()) +
                 ",\n};\n")
    parts.append(f"#define VEC_COM_BAD_CODE_COUNT {len(bad_codes())}\n")
    parts.append(
        "/* QR payload, code with dashes and the three label lines of a node */\n"
        "typedef struct {\n    uint64_t node_id;\n    const char *code;\n    const uint8_t *fp;\n"
        "    const char *qr;\n    const char *code_text;\n    const char *line_id;\n    const char *line_code;\n"
        "    const char *line_key;\n} vec_com_qr_t;\n")
    rows = []
    for i, (node_id, code, fp) in enumerate(qr_cases()):
        parts.append(array(f"vec_com_qr_fp_{i}", fp))
        l1, l2, l3 = com.label_lines(node_id, code, fp)
        assert com.parse_qr(com.qr_payload(node_id, code, fp)) == com.QrData(node_id, code, fp)
        rows.append(f"{{{node_id}ull, {_c(code)}, vec_com_qr_fp_{i}, {_c(com.qr_payload(node_id, code, fp))}, "
                    f"{_c(com.format_code(code))}, {_c(l1)}, {_c(l2)}, {_c(l3)}}}")
    parts += _table("vec_com_qr", "vec_com_qr_t", rows)
    parts.append(
        "/* texts that the QR parser must refuse */\ntypedef struct {\n    const char *what;\n    const char *text;\n"
        "} vec_com_badqr_t;\n")
    rows = []
    for what, text in bad_qr():
        try:
            com.parse_qr(text)
        except com.CodeError:
            pass
        else:
            raise AssertionError(f"bad QR '{what}' parses")
        rows.append(f"{{{_c(what)}, {_c(text)}}}")
    parts += _table("vec_com_badqr", "vec_com_badqr_t", rows)
    parts.append("#endif\n")
    return "\n".join(parts)
