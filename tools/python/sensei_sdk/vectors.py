# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Test vectors that the Python and the C implementations of the SDK libraries check against each other.

Writes, in the SDK:
    lib/sensei_link/tests/generated/vectors.h     CRC, COBS, stream frames, SPI frames, fragments, acknowledgements
    lib/sensei_secure/tests/generated/vectors.h   keys and sealed envelopes (fixed test keys, never used on a device)
    lib/sensei_diag/tests/generated/vectors.h     packed fault records, reset and reason names

    python -m sensei_sdk.vectors             write the headers
    python -m sensei_sdk.vectors --check     fail if a committed header is out of date (CI)

The SDK is found from the location of this file in a checkout, or with --sdk-root. An application that tests its own
protocol the same way (messages of its own) builds on `pattern`, `array`, `pairs` and `frag_cases`.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import diag, framing, secure

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
    parts.append("#endif\n")
    return "\n".join(parts)


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
