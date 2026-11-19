# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""End-to-end security of SENSEI, backend side: key store, envelope, replay window.

Mirror of lib/sensei_secure (C); its README has the byte layout. The shared vectors
(`python -m sensei_sdk.vectors`, lib/sensei_secure/tests/generated/vectors.h) check that both agree.

Every host that seals downlinks has its own sender id (32 bit, in the envelope and in the nonce), kept in the key
store file; the node tracks one replay window per sender. The file also holds a fingerprint of the host it was
used on (machine id and absolute path). A store that is opened somewhere else, for example a copy, takes a new
sender by itself (`KeyStore.sender_changed`). `python -m sensei_sdk.secure new-sender <file>` does it by hand.

The backend (collector, GUI) holds a P-256 key pair. At commissioning each node sends its public key
and receives the backend's. Both derive the same two AES-128-GCM keys with ECDH and HKDF-SHA256, so
gateways and the network server only ever see ciphertext.

Keys at rest. The node seals its secret settings records with a key from its hardware unique key (`seal_record`,
the mirror of lib/sensei_secure/include/sensei_e2e_seal.h). The key store file can be sealed as a whole with a 32 byte
key file kept outside the key store folder and its backups (`SENSEI_KEYSTORE_KEY_FILE`, see `KeyStore`):
`python -m sensei_sdk.secure seal-file | unseal-file | rotate-file-key`.

Forward secrecy. The key store file (version 3) keeps per node the generation in use, an answered mix and the previous
uplink chain; `open_sealed` walks the uplink chain under the limits of `chainwalk`, `KeyStore.rekey_answer` answers the
mix request of a node (responder only). See `KeyStore` and the README of this package.
"""

from __future__ import annotations

import argparse
import base64
import collections
import contextlib
import functools
import hashlib
import hmac
import json
import logging
import os
import re
import secrets
import shlex
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

try:  # not on Windows: the store then relies on one process per file
    import fcntl
except ImportError:  # pragma: no cover
    fcntl = None

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

from . import ratchet
from .chainwalk import ChainState, Walked, Walker
from .errors import EpochError, GapError, GenerationError, ReplayError, SecureError, UnknownKeyError, WalkBudgetError
from .framing import kind_of, looks_sealed

log = logging.getLogger("sensei.secure")

# Version 3 (2 and 0 are never used): bit 2 of the first byte is always set, so an envelope starts with
# 0xAC ... 0xAF, never with 0xA0 or 0xA8, which a plain Settings patch can (fields 20, 21, 36, 37, 52, 53).
VERSION = 3
MARKER = 0xA0  # high nibble of the first byte of every envelope
TAG_SIZE = 16
PUB_SIZE = 65
SENDER_SIZE = 4  # only in backend to node envelopes
FIXED_HEADER = 2 + 4 + 4  # flags, kind, key_id, counter
OVERHEAD = FIXED_HEADER + TAG_SIZE  # 26 B, node to backend
OVERHEAD_NODE_ID = OVERHEAD + 8  # 34 B with the optional node id
OVERHEAD_DOWN = OVERHEAD + SENDER_SIZE  # 30 B, backend to node
WINDOW = 64
MAX_SENDERS = 8  # senders a node tracks; a new one is refused when the table is full
HKDF_SALT = b"SENSEI-E2E-v1"

# message kinds (associated data)
KIND_UPLINK = 1  # encoded Uplink, node to backend
KIND_DOWNLINK = 2  # encoded Downlink, backend to node
KIND_NODE_TO_HOST = 3  # encoded NodeToHost data message, node to host
KIND_SETTINGS = 4  # encoded Settings patch for the BLE settings characteristic, backend to node
KIND_COMMISSION = 5  # commissioning channel (`commission.Channel`): both directions, generation 0 format, no node id
COMMISSION_SENDER = 1  # sender field of the commissioning envelopes that go to the node (the node sends 0)

# Kind byte (byte 1). Generation 0 (the keys of the commissioning): the kind alone, 1 ... 4. Generation 1 and later
# (the ratchet): 0x80 | (generation mod 4) << 4 | kind. Bit 6 is reserved: an envelope with it set is refused.
KIND_MASK = 0x0F
KIND_RATCHET = 0x80
KIND_RESERVED = 0x40
GEN_SHIFT = 4
GEN_MASK = 0x30

# direction bit of the first byte
DIR_UP = 0  # node to backend
DIR_DOWN = 1  # backend to node

_KIND_DIR = {KIND_UPLINK: DIR_UP, KIND_NODE_TO_HOST: DIR_UP, KIND_DOWNLINK: DIR_DOWN, KIND_SETTINGS: DIR_DOWN}
_KINDS = frozenset(_KIND_DIR) | {KIND_COMMISSION}  # kind 5 has no fixed direction: the sender field tells it
_CTR_MAX = 0xFFFFFFFF

# ---- keys ----


def generate_keypair() -> tuple[bytes, bytes]:
    """New P-256 key pair: (32 B private scalar, 65 B public point)."""
    key = ec.generate_private_key(ec.SECP256R1())
    priv = key.private_numbers().private_value.to_bytes(32, "big")
    return priv, _public_bytes(key)


def _public_bytes(key: ec.EllipticCurvePrivateKey) -> bytes:
    return key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)


def _load_private(priv: bytes) -> ec.EllipticCurvePrivateKey:
    try:
        return ec.derive_private_key(int.from_bytes(priv, "big"), ec.SECP256R1())
    except ValueError as exc:
        raise SecureError(f"bad private key: {exc}") from exc


def public_from_private(priv: bytes) -> bytes:
    return _public_bytes(_load_private(priv))


def _load_public(pub: bytes) -> ec.EllipticCurvePublicKey:
    if len(pub) != PUB_SIZE or pub[0] != 4:
        raise SecureError("public key must be a 65 B uncompressed P-256 point")
    try:
        return ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), pub)
    except ValueError as exc:
        raise SecureError(f"bad public key: {exc}") from exc


@dataclass(frozen = True)
class SessionKeys:
    k_up: bytes  # node to backend, 16 B
    k_down: bytes  # backend to node, 16 B
    key_id: int


def derive_keys(own_priv: bytes, peer_pub: bytes, node_pub: bytes, backend_pub: bytes) -> SessionKeys:
    """ECDH and HKDF-SHA256. Either side calls it with its own private key and the peer's public key."""
    secret = _load_private(own_priv).exchange(ec.ECDH(), _load_public(peer_pub))
    okm = HKDF(hashes.SHA256(), 36, HKDF_SALT, node_pub + backend_pub).derive(secret)
    key_id = struct.unpack("<I", okm[32:])[0]
    if key_id == 0:  # 0 means "no key" on the node: provision again with another key pair
        raise SecureError("derived key id is 0")
    return SessionKeys(okm[:16], okm[16:32], key_id)


# ---- envelope ----


def is_sealed(payload: bytes) -> bool:
    """True if the payload is an envelope of this version: marker 0xA and version bits (a plain Uplink or Downlink
    never starts with 0xAx, a plain Settings patch may start with 0xA0 or 0xA8 but never with 0xAC ... 0xAF). The
    one check, `framing.looks_sealed`, which needs no crypto import; a test keeps it in line with the constants here."""
    return looks_sealed(payload)


def gen_bits_of(payload: bytes) -> int:
    """Generation mod 4 of a sealed payload (0 for generation 0 and for a payload without the ratchet bit)."""
    return (payload[1] >> GEN_SHIFT) & 3 if payload[1] & KIND_RATCHET else 0


def is_ratchet(payload: bytes) -> bool:
    """True if the kind byte says generation 1 or later (the ratchet format)."""
    return bool(payload[1] & KIND_RATCHET)


def kind_byte(kind: int, gen: int = 0) -> int:
    """Byte 1 of an envelope: the kind for generation 0, with the ratchet bit and gen mod 4 for generation 1 and later."""
    if kind not in _KINDS:
        raise SecureError(f"unknown kind {kind}")
    if gen < 0:
        raise SecureError("negative generation")
    return kind if gen == 0 else KIND_RATCHET | ((gen & 3) << GEN_SHIFT) | kind


def _nonce(direction: int, sender: int, counter: int) -> bytes:
    return bytes([direction, 0, 0, 0]) + struct.pack("<II", sender, counter)


def seal_raw(
    key: bytes,
    key_id: int,
    counter: int,
    kind: int,
    plaintext: bytes,
    node_id: int | None = None,
    sender: int = 0,
    gen: int = 0,
) -> bytes:
    """Build one envelope. The caller owns the counter. A backend to node kind needs a sender id (not 0), a node
    to backend kind has none. gen: generation of the key (0: the commissioning keys, the format of version 3 as it
    always was; 1 and later: the ratchet bits in the kind byte, key = the message key or downlink key of that
    generation, see ratchet.py). Kind 5 (commissioning) goes both ways: sender 0 is node to backend, sender
    COMMISSION_SENDER is backend to node; it has no node id and no generation."""
    if kind not in _KINDS:
        raise SecureError(f"unknown kind {kind}")
    if not 0 < counter <= _CTR_MAX:
        raise SecureError("counter out of range")
    if kind == KIND_COMMISSION:
        if sender not in (0, COMMISSION_SENDER) or gen or node_id is not None:
            raise SecureError("a commissioning envelope has sender 0 or 1, no node id and generation 0")
        direction = DIR_DOWN if sender else DIR_UP
    else:
        direction = _KIND_DIR[kind]
    if direction == DIR_DOWN:
        if not 0 < sender <= 0xFFFFFFFF:
            raise SecureError("a downlink needs a sender id")
    elif sender:
        raise SecureError("an uplink has no sender id")
    first = MARKER | (VERSION << 2) | ((node_id is not None) << 1) | direction
    header = bytes([first, kind_byte(kind, gen)])
    if node_id is not None:
        header += struct.pack("<Q", node_id)
    header += struct.pack("<I", key_id)
    if direction == DIR_DOWN:
        header += struct.pack("<I", sender)
    header += struct.pack("<I", counter)
    return header + AESGCM(key).encrypt(_nonce(direction, sender, counter), plaintext, header)


@dataclass(frozen = True)
class Header:
    direction: int
    kind: int
    node_id: int | None
    key_id: int
    sender: int  # 0 for node to backend
    counter: int
    size: int  # header bytes, the ciphertext follows
    ratchet: bool = False  # generation 1 or later
    gen_bits: int = 0  # generation mod 4 (0 if not ratchet)


def parse_header(payload: bytes) -> Header:
    if not is_sealed(payload):
        raise SecureError("not a sealed payload")
    first = payload[0]
    if (first >> 2) & 3 != VERSION:
        raise SecureError(f"unsupported envelope version {(first >> 2) & 3}")
    has_node = bool(first & 2)
    direction = first & 1
    size = FIXED_HEADER + 8 * has_node + (SENDER_SIZE if direction == DIR_DOWN else 0)
    if len(payload) < size + TAG_SIZE:
        raise SecureError("sealed payload too short")
    raw = payload[1]
    if raw & KIND_RESERVED:
        raise SecureError("reserved bit of the kind byte is set")
    ratchet = bool(raw & KIND_RATCHET)
    if not ratchet and raw & GEN_MASK:
        raise SecureError("generation bits without the ratchet bit")
    gen_bits = (raw & GEN_MASK) >> GEN_SHIFT if ratchet else 0
    kind = kind_of(payload)
    node_id = struct.unpack_from("<Q", payload, 2)[0] if has_node else None
    key_id = struct.unpack_from("<I", payload, 10 if has_node else 2)[0]
    sender = struct.unpack_from("<I", payload, size - 8)[0] if direction == DIR_DOWN else 0
    counter = struct.unpack_from("<I", payload, size - 4)[0]
    if kind == KIND_COMMISSION:
        if ratchet or has_node or (direction == DIR_DOWN and sender != COMMISSION_SENDER):
            raise SecureError("bad commissioning envelope: generation, node id or sender")
    elif _KIND_DIR.get(kind) != direction:
        raise SecureError("kind does not match direction")
    if direction == DIR_DOWN and sender == 0:
        raise SecureError("downlink without a sender id")
    return Header(direction, kind, node_id, key_id, sender, counter, size, ratchet, gen_bits)


def open_raw(key: bytes, payload: bytes, header: Header | None = None) -> bytes:
    """Authenticate and decrypt one envelope (no replay check)."""
    h = header or parse_header(payload)
    try:
        return AESGCM(key).decrypt(_nonce(h.direction, h.sender, h.counter), payload[h.size:], payload[:h.size])
    except InvalidTag as exc:
        raise SecureError("authentication failed") from exc


class ReplayWindow:
    """Highest counter seen and a bitmap of the 64 counters up to it (bit i: highest - i was seen)."""

    def __init__(self, highest: int = 0, bitmap: int = 0) -> None:
        self.highest = highest
        self.bitmap = bitmap

    def check(self, counter: int) -> None:
        if counter == 0:
            raise ReplayError("counter 0")
        if counter > self.highest:
            return
        diff = self.highest - counter
        if diff >= WINDOW:
            raise ReplayError("counter too old")
        if (self.bitmap >> diff) & 1:
            raise ReplayError("counter already seen")

    def update(self, counter: int) -> None:
        if counter > self.highest:
            shift = counter - self.highest
            self.bitmap = ((self.bitmap << shift) | 1) & (2**WINDOW - 1) if shift < WINDOW else 1
            self.highest = counter
        else:
            self.bitmap |= 1 << (self.highest - counter)


class NodeSession:
    """The node's side of a session, for tests, simulators and the vectors (the node itself runs the C library).

    Like the node, it counts per sender and keeps `tx_counter` and `rx` when the keys change: pass them to the
    next session of the same node.

    It also plays the ratchet of lib/sensei_secure/src/sensei_e2e.c: `rekey_request` makes (or repeats) the request that the
    node appends to an uplink, `rekey_answer` takes the answer of the collector and switches to the new generation;
    from then on `seal` uses the message key of the epoch and `open` the downlink key of the generation (the one before
    stays valid until the next request). A new session of the same node (`NodeSession(...)` with the old `tx_counter`)
    is what provisioning does: generation 0 again, counters kept."""

    def __init__(
        self,
        node_priv: bytes,
        backend_pub: bytes,
        tx_counter: int = 0,
        rx: dict[int, ReplayWindow] | None = None,
    ) -> None:
        self.node_pub = public_from_private(node_priv)
        self.keys = derive_keys(node_priv, backend_pub, self.node_pub, backend_pub)
        self.tx_counter = tx_counter  # last counter used
        self.rx: dict[int, ReplayWindow] = rx if rx is not None else {}  # replay window by sender
        self.tx_reserved = 0  # the end of the block the node stored (a request starts above it)
        self.gen = 0
        self.root = ratchet.root0(self.keys.k_up, self.keys.k_down, self.keys.key_id)
        self.k_down = self.keys.k_down  # downlink key of the generation in use
        self.chain: ratchet.Chain | None = None  # uplink chain of the generation (None in generation 0)
        self.prev = False  # the downlink key of the generation before is still valid
        self.prev_k_down: bytes | None = None  # None with prev: generation 0, the static key
        self.pending: tuple[bytes, bytes, int] | None = None  # (private key, public key, up_base) of an open request
        self.epoch_cur: int | None = None  # epoch of the last uplink of this generation (None: a new epoch starts)
        self.rekeys = 0

    # ---- uplinks ----

    def seal(self, kind: int, plaintext: bytes, node_id: int | None = None, counter: int | None = None) -> bytes:
        """Seal an uplink. From generation 1 on the counter is moved to where the chain can serve it: a new epoch after a
        switch or `reboot()`, never before the chain (`new_epoch()` skips to the next one: an epoch in use for a day)."""
        if self.gen == 0:
            self.tx_counter = counter if counter is not None else self.tx_counter + 1
            return seal_raw(self.keys.k_up, self.keys.key_id, self.tx_counter, kind, plaintext, node_id)
        assert self.chain is not None
        c = counter if counter is not None else self.tx_counter + 1
        if counter is None:
            if self.epoch_cur is None:
                c = (c + ratchet.EPOCH_SIZE - 1) & ~(ratchet.EPOCH_SIZE - 1)
            c = max(c, (self.chain.up_base + self.chain.idx) << ratchet.EPOCH_BITS)
        key = self.chain.key_for(c, commit = True)
        self.tx_counter = c
        self.epoch_cur = ratchet.epoch(c)
        return seal_raw(key, self.keys.key_id, c, kind, plaintext, node_id, gen = self.gen)

    def new_epoch(self) -> None:
        """The epoch in use is old: the next uplink starts the next one."""
        self.tx_counter = ((self.tx_counter >> ratchet.EPOCH_BITS) + 1 << ratchet.EPOCH_BITS) - 1

    def reboot(self) -> None:
        """A restart: the next uplink starts a new epoch."""
        self.epoch_cur = None

    # ---- the mix ----

    def rekey_request(self) -> RekeyRequest:
        """The request to send with the next uplink: made at the first call, the same one until it is answered (the
        node repeats it every few hours). A new one drops the downlink key of the generation before."""
        if self.request_stale():
            self.pending = None  # too many epochs sent since the request: ask again (the answer walk stays short)
        if self.pending is None:
            used = max(self.tx_counter, self.tx_reserved)
            up_base = ratchet.epoch(used) + 1
            if self.chain is not None and up_base <= self.chain.up_base:
                up_base = self.chain.up_base + 1
            priv, pub = ratchet.x25519_keygen()
            self.pending = (priv, pub, up_base)
            self.prev, self.prev_k_down = False, None
        _, pub, up_base = self.pending
        return RekeyRequest(self.gen + 1, pub, up_base)

    def request_stale(self) -> bool:
        """The open request is older than `ratchet.STALE_EPOCHS` epochs of sending (the C rule of request_stale): the
        next `rekey_request` makes a new one, and `rekey_answer` refuses the answer to this one."""
        if self.pending is None:
            return False
        ep = ratchet.epoch(self.tx_counter)
        return ep > self.pending[2] and ep - self.pending[2] > ratchet.STALE_EPOCHS

    def rekey_answer(self, ans: RekeyAnswer) -> bool:
        """Take the answer. Ignored (False, nothing changes) if no request is open, the generation is not the next one,
        the up_base is not the one asked, the key has the wrong size, or the request is stale (more than
        `ratchet.STALE_EPOCHS` epochs sent since: -ETIMEDOUT in C; the next `rekey_request` is a new one); an all-zero DH
        result raises DhError. True when the node switched to generation g+1: the state is replaced at once and the counter
        jumps above everything sent."""
        if self.pending is None or ans.gen <= self.gen:
            return False
        priv, pub, up_base = self.pending
        if ans.gen != self.gen + 1 or ans.up_base != up_base or len(ans.pub) != ratchet.KEY_SIZE:
            return False
        if self.request_stale():
            return False  # -ETIMEDOUT; the node asks again with its next uplink
        mix = ratchet.mix_node(self.root, self.keys.key_id, ans.gen, up_base, priv, ans.pub)
        nxt = max(self.tx_counter + 1, up_base << ratchet.EPOCH_BITS)
        chain = ratchet.Chain(mix.ck, up_base)
        chain.advance_to(ratchet.epoch(nxt) - up_base)
        self.prev, self.prev_k_down = True, (self.k_down if self.gen != 0 else None)
        self.gen, self.root, self.k_down, self.chain = ans.gen, mix.root, mix.k_down, chain
        self.pending = None
        self.epoch_cur = None
        self.tx_counter = nxt - 1
        self.rekeys += 1
        return True

    # ---- downlinks ----

    def open(self, payload: bytes) -> tuple[int, bytes]:
        h = parse_header(payload)
        if h.direction != DIR_DOWN or h.kind == KIND_COMMISSION:
            raise SecureError("wrong direction")
        if h.key_id != self.keys.key_id:
            raise UnknownKeyError("wrong key id")
        if h.ratchet:
            if self.gen == 0:
                raise GenerationError("ratchet envelope: this session is in generation 0")
            if h.gen_bits == self.gen & 3:
                key = self.k_down
            elif self.gen >= 2 and self.prev and self.prev_k_down is not None and h.gen_bits == (self.gen - 1) & 3:
                key = self.prev_k_down
            else:
                raise GenerationError(
                    f"no downlink key for generation {h.gen_bits} (mod 4), this node is in {self.gen}")
        elif self.gen == 0 or (self.gen == 1 and self.prev):
            key = self.keys.k_down  # generation 0, still valid right after the first switch
        else:
            raise GenerationError("generation 0 envelope: dropped")
        window = self.rx.get(h.sender)
        if window is None and len(self.rx) >= MAX_SENDERS:
            raise SecureError("too many senders")
        (window or ReplayWindow()).check(h.counter)
        plain = open_raw(key, payload, h)
        if window is None:
            window = self.rx[h.sender] = ReplayWindow()
        window.update(h.counter)
        return h.kind, plain


# ---- sealed settings records (node, keys at rest) ----

SEAL_VERSION = 1
SEAL_NONCE_SIZE = 12
SEAL_OVERHEAD = 1 + SEAL_NONCE_SIZE + TAG_SIZE  # 29 B more than the plain value
SEAL_NAME_MAX = 32
SEAL_CTX_MAX = 8
_SEAL_AAD = b"sensei-e2e/"


def _seal_aad(name: str, ctx: bytes = b"") -> bytes:
    raw = name.encode()
    if not raw or len(raw) > SEAL_NAME_MAX:
        raise SecureError("bad setting name")
    if len(ctx) > SEAL_CTX_MAX:
        raise SecureError("bad context")
    return _SEAL_AAD + raw + bytes([SEAL_VERSION]) + ctx


def seal_record(key: bytes, name: str, plaintext: bytes, nonce: bytes | None = None, ctx: bytes = b"") -> bytes:
    """A sealed settings record as the node stores it: u8 version (1) | nonce[12] | AES-128-GCM ciphertext | tag[16].
    Associated data: "sensei-e2e/" || setting name || version [|| ctx], so a record does not open under another name
    (or another context: e2e_rat carries the key id of its key, 4 bytes little endian). `nonce` is for test vectors;
    leave it out to take 12 random bytes (a nonce must never repeat under one key)."""
    if len(key) != 16:
        raise SecureError("the sealing key is 16 bytes")
    nonce = secrets.token_bytes(SEAL_NONCE_SIZE) if nonce is None else nonce
    if len(nonce) != SEAL_NONCE_SIZE:
        raise SecureError("the nonce is 12 bytes")
    return bytes([SEAL_VERSION]) + nonce + AESGCM(key).encrypt(nonce, plaintext, _seal_aad(name, ctx))


def open_record(key: bytes, name: str, record: bytes, ctx: bytes = b"") -> bytes:
    """Open a sealed settings record of the node (see `seal_record`). Raises SecureError for a short record, an unknown
    version, a wrong key, a wrong name or a changed record."""
    if len(key) != 16:
        raise SecureError("the sealing key is 16 bytes")
    if len(record) < SEAL_OVERHEAD or record[0] != SEAL_VERSION:
        raise SecureError("not a sealed record of this version")
    try:
        return AESGCM(key).decrypt(record[1:1 + SEAL_NONCE_SIZE], record[1 + SEAL_NONCE_SIZE:], _seal_aad(name, ctx))
    except InvalidTag as exc:
        raise SecureError("sealed record does not open (wrong key or name, or changed)") from exc


# ---- key store file at rest ----

SEALED_FORMAT = "sensei-keystore-sealed"
SEALED_VERSION = 1
KEY_FILE_SIZE = 32
_KEY_ID_LABEL = b"sensei-keystore-key-id"
_SEALED_AAD = b"sensei-keystore-sealed-v1"
_ENV_KEY_FILE = "SENSEI_KEYSTORE_KEY_FILE"
_ENV_REQUIRE_SEALED = "SENSEI_KEYSTORE_REQUIRE_SEALED"
_ENV_RESPONDER = "SENSEI_REKEY_RESPONDER"
_ENV_RESPONDER_ID_FILE = "SENSEI_REKEY_RESPONDER_ID_FILE"
_ENV_MAX_SKIP = "SENSEI_E2E_MAX_SKIP"


def check_key_file_mode(path: str | os.PathLike) -> None:
    """Refuse a key file that the group or others can access (mode & 077), or that is not a plain file. Not checked on
    Windows."""
    p = Path(path)
    try:
        mode = p.stat().st_mode
    except OSError as exc:
        raise SecureError(f"cannot read the key store key file {p}: {exc}") from exc
    if not stat.S_ISREG(mode):
        raise SecureError(f"key store key file {p} is not a file")
    if os.name != "nt" and mode & 0o077:
        raise SecureError(f"key store key file {p} can be read by other users (mode {mode & 0o777:04o}) and holds the "
                          f"key of the key store: chmod 400 {shlex.quote(str(p))}")


def load_key_file(path: str | os.PathLike, check_mode: bool = True) -> bytes:
    """Read the 32 byte key file of a sealed key store (raw bytes, `new_key_file` makes one). A file that the group or
    others can access is refused (`check_mode`, see `check_key_file_mode`)."""
    p = Path(path)
    try:
        key = p.read_bytes()
    except OSError as exc:
        raise SecureError(f"cannot read the key store key file {p}: {exc}") from exc
    if len(key) != KEY_FILE_SIZE:  # before the mode: a file of the wrong kind says so first
        raise SecureError(f"key store key file {p} must be exactly {KEY_FILE_SIZE} raw bytes (it has {len(key)}): "
                          "make one with python -m sensei_sdk.secure new-key-file <file>")
    if check_mode:
        check_key_file_mode(p)
    return key


def new_key_file(path: str | os.PathLike) -> None:
    """Write 32 random bytes to a new file (mode 0400), sync the file and its folder, so that a power loss cannot leave
    an empty key file next to a sealed key store. Refuses to overwrite a key. A file that could not be written is
    removed."""
    p = Path(path)
    p.parent.mkdir(parents = True, exist_ok = True)
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o400)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(secrets.token_bytes(KEY_FILE_SIZE))
            f.flush()
            os.fsync(f.fileno())
    except BaseException:
        with contextlib.suppress(OSError):
            p.unlink()
        raise
    _fsync_dir(p.parent)


def load_responder_id(path: str | os.PathLike) -> str | None:
    """The id of a responder process: 16 random hex characters in a small file of its own (mode 0600), made at the first
    call. Keep the file in the config or state folder of the process that answers mixes, not in the key store folder: a
    copy of the key store must not carry it (`KeyStore` records the id in the key store file, one responder per store).

    An empty or damaged file (power loss, full disk) is replaced by a new id, written through a temporary file and a rename,
    with a warning. A folder that cannot be written gives a warning and None: nothing is recorded and the process goes on."""
    p = Path(path)
    try:
        text = p.read_text().strip()
    except FileNotFoundError:
        text = ""
    except (OSError, ValueError) as exc:  # unreadable, or not text
        log.warning("responder id file %s cannot be read, making a new id: %s", p, exc)
        text = ""
    if text and re.fullmatch(r"[0-9a-f]{16}", text):
        return text
    if text or p.exists():
        log.warning("responder id file %s is empty or damaged: replaced by a new id", p)
    new = secrets.token_hex(8)
    try:
        _write_atomic(p, new + "\n")  # temp file, fsync, mode 0600, rename
    except OSError as exc:
        log.warning("responder id file %s cannot be written (%s): the responder is not recorded in the key store", p,
                    exc)
        return None
    return new


def _file_key_id(key: bytes) -> bytes:
    return hmac.new(key, _KEY_ID_LABEL, hashlib.sha256).digest()[:8]


def _seal_text(inner: bytes, key: bytes) -> str:
    """The sealed key store file: a JSON wrapper around the inner key store JSON, AES-256-GCM under the key file with a
    new nonce every time. Associated data: "sensei-keystore-sealed-v1" || key id (8 raw bytes)."""
    key_id = _file_key_id(key)
    nonce = secrets.token_bytes(12)
    ct = AESGCM(key).encrypt(nonce, inner, _SEALED_AAD + key_id)
    return json.dumps(
        {
            "format": SEALED_FORMAT,
            "version": SEALED_VERSION,
            "key_id": key_id.hex(),
            "nonce": nonce.hex(),
            "ct": base64.b64encode(ct).decode(),
        },
        indent = 1)


def _unseal_text(outer: dict, key: bytes | None, where: object) -> bytes:
    """The inner JSON bytes of a sealed wrapper (already parsed)."""
    if outer.get("version") != SEALED_VERSION:
        raise SecureError(f"unsupported sealed key store version {outer.get('version')}")
    if key is None:
        raise SecureError(f"key store {where} is sealed: give the key file ({_ENV_KEY_FILE})")
    key_id = bytes.fromhex(outer["key_id"])
    if key_id != _file_key_id(key):
        raise SecureError(f"the key file does not belong to key store {where} (key id {_file_key_id(key).hex()}, "
                          f"the file wants {key_id.hex()})")
    try:
        return AESGCM(key).decrypt(bytes.fromhex(outer["nonce"]), base64.b64decode(outer["ct"], validate = True),
                                   _SEALED_AAD + key_id)
    except InvalidTag as exc:
        raise SecureError(f"key store {where} failed authentication: it was changed or is damaged") from exc


def _open_text(text: str, key: bytes | None, where: object, require_sealed: bool = False) -> tuple[dict, bool]:
    """Parse the text of a key store file, sealed or plain: (the inner JSON as a dict, was it sealed)."""
    outer = json.loads(text)
    if isinstance(outer, dict) and outer.get("format") == SEALED_FORMAT:
        return json.loads(_unseal_text(outer, key, where)), True
    if require_sealed:
        raise SecureError(f"key store {where} is not sealed but {_ENV_REQUIRE_SEALED}=1: "
                          "python -m sensei_sdk.secure seal-file <file> --key-file <key file>")
    return outer, False


def _write_atomic(path: Path, text: str) -> None:
    """Temp file in the same folder, fsync, mode 0600, rename, fsync of the folder: the old file stays until the new one is
    on disk."""
    path.parent.mkdir(parents = True, exist_ok = True)
    fd, tmp = tempfile.mkstemp(dir = path.parent, prefix = path.name + ".")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())  # the counters must survive a power loss
        os.chmod(tmp, 0o600)
        os.replace(tmp, path)
    except BaseException:
        with contextlib.suppress(OSError):
            os.unlink(tmp)
        raise
    _fsync_dir(path.parent)


@contextlib.contextmanager
def _flock(path: Path):
    """Exclusive lock on `<file>.lock` (the lock stays plain). Without fcntl (Windows) there is none."""
    path.parent.mkdir(parents = True, exist_ok = True)
    if fcntl is None:
        yield
        return
    with open(path.with_name(path.name + ".lock"), "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


# ---- key store ----

# Key store file version 3 (forward secrecy). Per node: the static keys of generation 0 (k_up, k_down, null once the
# previous generation 0 is dropped), `cur` (generation, root key, downlink key, uplink chain), `next` (the answered but
# not yet used mix) and `prev_up` (the uplink chain of the generation before, with the counter c1 where it ends). Up to
# version 2 a node had the static keys only; they migrate to generation 0 with the root key R_0 (`ratchet.root0`).
FILE_VERSION = 3
REREAD_S = 1.0  # shortest time between two re-reads of the file for an unknown key id
_monotonic = time.monotonic  # a module level name so that a test can move the time of the re-read limit
_MAX_EPOCH_IDX = 1 << 26  # an epoch of a 32 bit counter is below this


@dataclass
class _Cur:
    """The generation in use. Generation 0: `up` is None (the static k_up of the node). `root` is None in a follower."""

    gen: int
    root: bytes | None
    k_down: bytes  # 16 B, downlink key of the generation
    up: ChainState | None
    since: float = 0.0  # unix time this generation became the current one (0: not known, a file of before this field)


@dataclass
class _Next:
    """A mix that was answered and waits for the first uplink of the new generation. `up` is its chain at index 0."""

    gen: int
    e_n_pub: bytes
    e_c_pub: bytes
    root: bytes | None
    k_down: bytes
    up: ChainState
    seq: int = 0  # 1 for the first answer of a generation, +1 for each answer that replaces it: the newer one wins a merge

    def same_request(self, gen: int, up_base: int, e_n_pub: bytes) -> bool:
        return self.gen == gen and self.up.up_base == up_base and self.e_n_pub == e_n_pub


@dataclass
class _Node:
    node_pub: bytes
    key_id: int
    k_up: bytes | None  # static keys of generation 0, None once its chain is dropped
    k_down: bytes | None
    up: ReplayWindow  # counters of all generations (they only go up)
    cur: _Cur
    next: _Next | None = None
    prev: ChainState | None = None  # previous generation; gen 0: only gen, c1 and t count (the key is k_up)
    t_up: float = 0.0  # unix time of the last uplink that verified (any generation), 0 if unknown
    prov: int = 0  # provisioning counter: `add_node` raises it, a copy of the node with a higher one replaces the ratchet state

    @property
    def keys(self) -> SessionKeys:
        """The static keys of generation 0 (while the key store still has them)."""
        if self.k_up is None or self.k_down is None:
            raise SecureError("the static keys of generation 0 are dropped")
        return SessionKeys(self.k_up, self.k_down, self.key_id)


@dataclass
class _Retired:
    """The counters of a key that was removed (no secrets): if the same key comes back it goes on from them."""

    node_pub: bytes
    key_id: int
    up: ReplayWindow
    down_ctr: int = 0  # files of version 1 only: folded into KeyStore._down when the key returns
    prov: int = 0  # `prov` of the node, so that the same key added again goes on above it


def _node_text(node_id: int) -> str:
    return f"{node_id:016x}"


def _hex(b: bytes | None) -> str | None:
    return None if b is None else b.hex()


def _unhex(s: str | None, size: int | None = None) -> bytes | None:
    if s is None:
        return None
    b = bytes.fromhex(s)
    if size is not None and len(b) != size:
        raise ValueError(f"a key of {size} bytes was expected, got {len(b)}")
    return b


def _fresh_node(node_pub: bytes, keys: SessionKeys, window: ReplayWindow) -> _Node:
    """A node in generation 0 (the keys of the commissioning). R_0 comes from the static keys."""
    root = ratchet.root0(keys.k_up, keys.k_down, keys.key_id)
    return _Node(node_pub, keys.key_id, keys.k_up, keys.k_down, window, _Cur(0, root, keys.k_down, None))


def _chain_to_json(st: ChainState | None) -> dict:
    """up_base, up_idx, up_ck, up_t of a chain (null for generation 0, which has the static key)."""
    if st is None or st.gen == 0:
        return {"up_base": None, "up_idx": None, "up_ck": None, "up_t": None if st is None else round(st.t, 3)}
    return {"up_base": st.up_base, "up_idx": st.idx, "up_ck": st.ck.hex(), "up_t": round(st.t, 3)}


def _chain_from_json(gen: int, d: dict) -> ChainState | None:
    if d.get("up_ck") is None:
        return None
    base, idx = int(d["up_base"]), int(d["up_idx"])
    if not 0 < base < _MAX_EPOCH_IDX or not 0 <= idx < _MAX_EPOCH_IDX - base:
        raise ValueError("chain position out of range")
    return ChainState(gen, base, idx, _unhex(d["up_ck"], 32), float(d.get("up_t") or 0.0))


def _node_to_json(n: _Node) -> dict:
    out = {
        "node_pub": n.node_pub.hex(),
        "key_id": n.key_id,
        "k_up": _hex(n.k_up),
        "k_down": _hex(n.k_down),
        "up_highest": n.up.highest,
        "up_window": f"{n.up.bitmap:016x}",
        "t_up": round(n.t_up, 3),
        "prov": n.prov,
        "cur": {
            "gen": n.cur.gen,
            "since": round(n.cur.since, 3),
            "root": _hex(n.cur.root),
            "k_down": n.cur.k_down.hex(),
            **_chain_to_json(n.cur.up),
        },
        "next": None,
        "prev_up": None,
    }
    if n.next is not None:
        out["next"] = {
            "gen": n.next.gen,
            "up_base": n.next.up.up_base,
            "e_n_pub": n.next.e_n_pub.hex(),
            "e_c_pub": n.next.e_c_pub.hex(),
            "root": _hex(n.next.root),
            "k_down": n.next.k_down.hex(),
            "ck": n.next.up.ck.hex(),
            "t": round(n.next.up.t, 3),
            "seq": n.next.seq,
        }
    if n.prev is not None:
        out["prev_up"] = {"gen": n.prev.gen, "c1": n.prev.c1, **_chain_to_json(n.prev)}
    return out


def _retired_to_json(r: _Retired) -> dict:
    return {
        "node_pub": r.node_pub.hex(),
        "key_id": r.key_id,
        "up_highest": r.up.highest,
        "up_window": f"{r.up.bitmap:016x}",
        "down_ctr": r.down_ctr,
        "prov": r.prov,
    }


def _node_from_json(n: dict) -> _Node:
    """A node of a file of version 1, 2 (static keys only: generation 0) or 3."""
    node_pub, key_id = bytes.fromhex(n["node_pub"]), int(n["key_id"])
    k_up, k_down = _unhex(n.get("k_up"), 16), _unhex(n.get("k_down"), 16)
    window = ReplayWindow(int(n["up_highest"]), int(n["up_window"], 16))
    if "cur" not in n:  # version 1 or 2
        if k_up is None or k_down is None:
            raise ValueError("a node without keys")
        node = _fresh_node(node_pub, SessionKeys(k_up, k_down, key_id), window)
        node.cur.k_down = k_down
        return node
    c = n["cur"]
    gen = int(c["gen"])
    cur = _Cur(gen, _unhex(c.get("root"), 32), _unhex(c["k_down"], 16), None, float(c.get("since") or 0.0))
    if gen == 0:
        if k_up is None or k_down is None:
            raise ValueError("generation 0 without its keys")
    else:
        cur.up = _chain_from_json(gen, c)
        if cur.up is None:
            raise ValueError("a generation without its chain")
    node = _Node(node_pub,
                 key_id,
                 k_up,
                 k_down,
                 window,
                 cur,
                 t_up = float(n.get("t_up") or 0.0),
                 prov = int(n.get("prov", 0)))
    x = n.get("next")
    if x is not None:
        ck = _unhex(x["ck"], 32)
        base = int(x["up_base"])
        if not 0 < base < _MAX_EPOCH_IDX or int(x["gen"]) != gen + 1:
            raise ValueError("bad next")
        node.next = _Next(
            gen + 1,
            _unhex(x["e_n_pub"], 32),
            _unhex(x["e_c_pub"], 32),
            _unhex(x.get("root"), 32),
            _unhex(x["k_down"], 16),
            ChainState(gen + 1, base, 0, ck, float(x.get("t") or 0.0)),
            int(x.get("seq", 0)),
        )
    p = n.get("prev_up")
    if p is not None:
        pgen = int(p["gen"])
        if pgen != gen - 1:
            raise ValueError("bad prev_up")
        if pgen == 0:
            if k_up is None:
                raise ValueError("prev_up of generation 0 without the static key")
            node.prev = ChainState(0, 0, 0, b"", float(p.get("up_t") or 0.0), int(p["c1"]))
        else:
            st = _chain_from_json(pgen, p)
            if st is None:
                raise ValueError("prev_up without its chain")
            st.c1 = int(p["c1"])
            node.prev = st
    return node


def _retired_from_json(n: dict) -> _Retired:
    return _Retired(bytes.fromhex(n["node_pub"]), int(n["key_id"]),
                    ReplayWindow(int(n["up_highest"]), int(n["up_window"], 16)), int(n.get("down_ctr", 0)),
                    int(n.get("prov", 0)))


@dataclass
class _File:
    pub: bytes
    nodes: dict[int, _Node]
    retired: dict[int, _Retired]
    senders: dict[str, int]  # sender id by host fingerprint
    down: dict[int, int]  # last downlink counter by node id
    responder: str | None = None  # id of the process that answered mixes last (None: not recorded)


@functools.cache
def _machine_id() -> str:
    """An id of this machine that survives reboots: the OS machine id, else the host name."""
    for p in ("/etc/machine-id", "/var/lib/dbus/machine-id"):
        with contextlib.suppress(OSError):
            text = Path(p).read_text().strip()
            if text:
                return text
    if sys.platform == "darwin":
        with contextlib.suppress(OSError, subprocess.SubprocessError):
            out = subprocess.run(
                ["/usr/sbin/ioreg", "-rd1", "-c", "IOPlatformExpertDevice"],
                capture_output = True,
                text = True,
                timeout = 5,
                check = False,
            ).stdout
            m = re.search(r'"IOPlatformUUID" = "([^"]+)"', out)
            if m:
                return m.group(1)
    return socket.gethostname()


def host_fingerprint(path: str | os.PathLike) -> str:
    """Fingerprint of a key store file on this host: machine id and absolute path. A copy of the file on another
    machine, or in another place, has another one. The environment variable SENSEI_HOST_ID replaces the machine id
    (a container has none and a new host name at every start: set it, or give the container a fixed host name)."""
    host_id = os.environ.get("SENSEI_HOST_ID", "").strip() or _machine_id()
    return hashlib.sha256(f"{host_id}\0{Path(path).resolve()}".encode()).hexdigest()[:16]


def _fsync_dir(path: Path) -> None:
    """Make the rename durable. Not every platform can open a directory."""
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


def _merge_window(a: ReplayWindow, b: ReplayWindow) -> ReplayWindow:
    """Union of two replay windows: what either one has seen counts as seen."""
    high = max(a.highest, b.highest)
    bits = 0
    for w in (a, b):
        shift = high - w.highest
        if shift < WINDOW:
            bits |= (w.bitmap << shift) & (2**WINDOW - 1)
    return ReplayWindow(high, bits)


# ---- the mix messages (Uplink.rekey = 14, Downlink.rekey = 14: the same three fields) ----


def _put_varint(v: int) -> bytes:
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def _get_varint(buf: bytes, pos: int) -> tuple[int, int]:
    v = shift = 0
    while True:
        if pos >= len(buf) or shift > 28:
            raise SecureError("bad varint in a rekey message")
        b = buf[pos]
        pos += 1
        v |= (b & 0x7F) << shift
        if not b & 0x80:
            return v, pos
        shift += 7


@dataclass(frozen = True)
class RekeyMessage:
    """RekeyRequest (node to collector) and RekeyAnswer (back): generation g+1, a 32 B X25519 public key (the node's, or
    the collector's) and the first uplink epoch of the new generation, which also names the request."""

    gen: int
    pub: bytes
    up_base: int

    def encode(self) -> bytes:
        """The protobuf message: gen = 1, pub = 2, up_base = 3."""
        return b"\x08" + _put_varint(self.gen) + b"\x12" + _put_varint(len(
            self.pub)) + self.pub + b"\x18" + _put_varint(self.up_base)

    def encode_field(self) -> bytes:
        """Field 14 of an Uplink or a Downlink (tag 0x72), to append to the encoded message."""
        body = self.encode()
        return b"\x72" + _put_varint(len(body)) + body

    @classmethod
    def decode(cls, buf: bytes) -> RekeyMessage:
        gen = up_base = 0
        pub = b""
        pos = 0
        while pos < len(buf):
            tag, pos = _get_varint(buf, pos)
            if tag == 0x08 or tag == 0x18:
                v, pos = _get_varint(buf, pos)
                if tag == 0x08:
                    gen = v
                else:
                    up_base = v
            elif tag == 0x12:
                n, pos = _get_varint(buf, pos)
                if pos + n > len(buf):
                    raise SecureError("rekey message too short")
                pub, pos = buf[pos:pos + n], pos + n
            else:
                raise SecureError(f"unknown field in a rekey message (tag {tag:#x})")
        return cls(gen, pub, up_base)


class RekeyRequest(RekeyMessage):
    pass


class RekeyAnswer(RekeyMessage):
    pass


@dataclass(frozen = True)
class NodeState:
    """The ratchet state of a node, for a GUI and for metrics."""

    gen: int
    gen_since: float | None  # unix time this generation became the current one (None: not known)
    next_gen: int | None  # a mix that was answered and waits for its first uplink
    next_since: float | None  # when it was answered; stays at the first unanswered request while a request is replaced
    prev_gen: int | None  # the previous generation is still accepted for late messages
    prev_ends: int | None  # ... below this counter
    up_idx: int | None  # chain index of the stored key (None in generation 0)
    up_base: int | None
    last_verified: float | None  # unix time of the last message that opened under the current chain
    last_uplink: float | None  # unix time of the last uplink that verified, any generation (None: not known)
    highest: int  # highest uplink counter seen
    static_keys: bool  # the static keys of generation 0 are still held


@dataclass(frozen = True)
class NodeBackup:
    """What `KeyStore.add_node` replaced, for `KeyStore.rollback_node`. `changed` is False if add_node did nothing."""

    node_id: int
    old: _Node | None  # the node before (None: it was new)
    prov: int  # provisioning counter of the node that add_node made
    changed: bool


def _prune_prev(node: _Node) -> None:
    """Drop what is no longer needed: the previous chain once the replay window is past its end (highest - 63 >= c1, all
    its counters are refused anyway), one that does not belong to the generation before `cur`, and the static keys of
    generation 0 once nothing needs them."""
    prev = node.prev
    if prev is not None and (prev.gen != node.cur.gen - 1 or node.up.highest - (WINDOW - 1) >= prev.c1):
        node.prev = None
    if node.next is not None and node.next.gen != node.cur.gen + 1:
        node.next = None
    if node.cur.gen != 0 and (node.prev is None or node.prev.gen != 0):
        node.k_up = node.k_down = None


def _adopt_ratchet(mine: _Node, theirs: _Node) -> None:
    """`theirs` was provisioned again (higher `prov`): its whole ratchet state replaces ours, whatever the generations."""
    mine.cur, mine.next, mine.prev = theirs.cur, theirs.next, theirs.prev
    mine.k_up, mine.k_down = theirs.k_up, theirs.k_down
    mine.t_up = max(mine.t_up, theirs.t_up)
    mine.prov = theirs.prov
    _prune_prev(mine)


def _cur_pos(c: _Cur) -> tuple[int, int]:
    return c.gen, c.up.idx if c.up is not None else 0


def _merge_ratchet(mine: _Node, theirs: _Node) -> None:
    """Merge the ratchet state of the same node from another copy of the file (design 1.5), for copies with the same
    `prov`: the higher generation wins, in one generation the chain that is further; `next` only if it is the generation
    after `cur` (the newer answer, `seq`, wins; the file on a tie);
    `prev_up` of the generation before `cur`, the further chain and the later end; the static keys only while a
    generation 0 chain is in use."""
    a, b = mine.cur, theirs.cur
    if _cur_pos(b) > _cur_pos(a):
        mine.cur = a = b
    else:
        if a.root is None and b.root is not None and a.gen == b.gen:
            a.root = b.root
        if a.gen == b.gen and not a.since:
            a.since = b.since
        if a.up is not None and b.up is not None and a.gen == b.gen:
            a.up.t = max(a.up.t, b.up.t)
    # `next`: the newer answer wins (a node that asked again with a new key), the file on a tie
    cands = [c for c in (theirs.next, mine.next) if c is not None and c.gen == a.gen + 1]
    nxt = max(cands, key = lambda c: c.seq) if cands else None
    if nxt is not None and mine.next is not None and nxt.same_request(mine.next.gen, mine.next.up.up_base,
                                                                      mine.next.e_n_pub):
        nxt = mine.next  # the same mix: keep the object (its cache) and the later time
        if theirs.next is not None:
            nxt.up.t = max(nxt.up.t, theirs.next.up.t)
    mine.next = nxt
    best = None
    for cand in (mine.prev, theirs.prev):
        if cand is not None and cand.gen == a.gen - 1 and (best is None or cand.idx > best.idx):
            best = cand
    if best is not None:
        for cand in (mine.prev, theirs.prev):
            if cand is not None and cand.gen == best.gen:
                best.c1 = max(best.c1, cand.c1)
                best.t = max(best.t, cand.t)
    mine.prev = best
    mine.t_up = max(mine.t_up, theirs.t_up)
    if mine.k_up is None:
        mine.k_up, mine.k_down = theirs.k_up, theirs.k_down  # only used if still needed, see _prune_prev
    elif theirs.k_up is None and a.gen != 0 and (best is None or best.gen != 0):
        mine.k_up = mine.k_down = None
    _prune_prev(mine)


class KeyStore:
    """Backend key store: node public keys, derived keys and counters in one JSON file (mode 0600).

    The file holds secrets (the derived keys): keep it out of git and mount it as a secret. Deriving keys
    for a new node (add_node) needs the backend private key (PEM, see backend_keygen); opening and sealing do not.
    The private key comes from `private_pem`, else the environment variable SENSEI_BACKEND_KEY.

    Downlinks carry the store's `sender` id and a counter per node. The counter belongs to the node id, not to its
    key: it never goes back (not when a node is removed and added again, not with a new key). Processes that share
    the file take turns under a lock and merge, so they never use a counter twice. Two copies of the file do not
    see each other, so each host needs its own sender: the file keeps one sender per host fingerprint
    (`host_fingerprint`: machine id and path), so a restart finds its own again, also when another process (the GUI
    on the host and the collector in Docker) used the file in between. A store loaded with a fingerprint the file
    does not know yet takes a new sender (`sender_changed` is then true once; the GUI shows a warning). The node has
    room for 8 senders: keep the fingerprint stable. `new_sender` or the command line take a new one by hand.

    At rest the file can be sealed as a whole (AES-256-GCM) with a 32 byte key file, `key_file` or the environment
    variable SENSEI_KEYSTORE_KEY_FILE. Keep the key file outside the key store folder and its backups (a Docker secret):
    a stolen file or backup is then useless. With a key file every save writes a sealed file (a plain file is read and
    sealed at the next save), with a new nonce each time; a sealed file needs the key file, and a wrong key is named as
    such. `require_sealed` (SENSEI_KEYSTORE_REQUIRE_SEALED=1) also refuses a plain file and a missing key file. The lock
    file stays plain. A key file that is configured but missing is an error, never a silent fall back to a plain file.

    Forward secrecy (file version 3, `ratchet`). A node mixes its keys every few days: it asks (`RekeyRequest` in an
    uplink), a key store opened with `responder=True` (SENSEI_REKEY_RESPONDER=1) answers with `rekey_answer`, and the
    first uplink of the new generation that verifies against the answer makes it the current one. The keys of the
    generation before are kept as long as late messages can arrive (`prev_up`), then dropped. Uplinks are opened with
    `open_sealed`, which walks the uplink chain (with the limits of `chainwalk`); downlinks are sealed with the downlink
    key of the current generation. A GUI that keeps its own file and does not answer follows the file of the collector
    (`follow=`): it takes over the current generation, the answered mix and the previous chain (not the root key, the
    sender ids and the counters). A key store without it loses access at the next mix of a node.

    An envelope with an unknown key id makes the store read its file again (at most once a second, whatever the ids),
    so a node that another process added (the GUI, commissioning over BLE) is found without a restart.

    Provisioning again (`add_node`) raises the node's `prov`; the copy with the higher `prov` wins in a merge and in a
    follower, so the reset is not undone by a process that holds an older state. `add_node` returns a `NodeBackup` for
    `rollback_node`. `generation`, `node_state`, the follower refresh and every change take the store lock (re-entrant);
    the walk and the tag check run outside it.
    """

    def __init__(
        self,
        path: str | os.PathLike | None = None,
        private_pem: str | os.PathLike | None = None,
        autosave_s: float = 5.0,
        key_file: str | os.PathLike | None = None,
        require_sealed: bool | None = None,
        responder: bool | None = None,
        responder_id: str | None = None,
        follow: str | os.PathLike | None = None,
        follow_key_file: str | os.PathLike | None = None,
        walker: Walker | None = None,
        clock: Callable[[], float] = time.time,
    ) -> None:
        self.path = Path(path) if path else None
        key_file = key_file or os.environ.get(_ENV_KEY_FILE) or None
        if require_sealed is None:
            require_sealed = os.environ.get(_ENV_REQUIRE_SEALED, "").strip() == "1"
        self._key: bytes | None = load_key_file(key_file) if key_file else None
        self._require_sealed = require_sealed
        if require_sealed and self._key is None:
            raise SecureError(f"{_ENV_REQUIRE_SEALED}=1 needs a key file ({_ENV_KEY_FILE})")
        if responder is None:
            responder = os.environ.get(_ENV_RESPONDER, "").strip() == "1"
        if responder and follow:
            raise SecureError("a key store that answers mixes cannot follow another file")
        self.responder = bool(responder)  # answers the rekey requests of the nodes (`rekey_answer`)
        if responder_id is None and self.responder and os.environ.get(_ENV_RESPONDER_ID_FILE):
            responder_id = load_responder_id(os.environ[_ENV_RESPONDER_ID_FILE])
        # Id of this responder process (`load_responder_id`); a responder writes it to the file with every save. None: the
        # file is not marked.
        self.responder_id = responder_id if self.responder else None
        self._file_responder: str | None = None  # the id the file carries
        self._seen_responder: str | None = None  # the other id that was warned about last
        self._clock = clock
        if walker is None:
            skip = os.environ.get(_ENV_MAX_SKIP, "").strip()
            walker = Walker(max_skip = int(skip) if skip else ratchet.MAX_SKIP, clock = clock)
        self.walker = walker
        self.stats: collections.Counter[str] = collections.Counter()  # forged, refused and mix counts (metrics)
        self._log_at: dict[str, float] = {}
        self._follow = Path(follow) if follow else None
        self._follow_key = load_key_file(follow_key_file) if follow_key_file else self._key
        self._follow_sig: tuple | None = None
        self._resync_at = -1e9
        self._reread_at = -1e9  # monotonic time of the last re-read for an unknown key id (`_reread_unknown`)
        self.file_was_plain = False  # the file read last was not sealed (a key store without a key file: warn)
        self._warned_plain = False
        self.autosave_s = autosave_s
        self._lock = threading.RLock()
        self._nodes: dict[int, _Node] = {}
        self._by_key_id: dict[int, int] = {}
        self._down: dict[int, int] = {}  # last downlink counter by node id; never lowered, never dropped
        self._sender: int | None = None
        self._senders: dict[str, int] = {}  # all senders of the file, by host fingerprint
        self._new_sender = False
        self.sender_changed = False  # the file came from another host: this store took a new sender
        self._host = host_fingerprint(path) if path else ""
        # counters of removed nodes by key id: the same key added again must go on from them (a counter must
        # never be used twice with one key)
        self._retired: dict[int, _Retired] = {}
        self._added: set[int] = set()  # nodes added or changed here since the last save: they win a merge
        self._removed: set[int] = set()  # nodes removed here since the last save: a merge does not bring them back
        self._lock_depth = 0
        self._pub: bytes | None = None
        self._priv: bytes | None = None
        self._dirty = False
        self._last_save = 0.0
        pem = private_pem or os.environ.get("SENSEI_BACKEND_KEY")
        if pem and Path(pem).exists():
            key = serialization.load_pem_private_key(Path(pem).read_bytes(), None)
            if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
                raise SecureError("the backend key must be a P-256 private key")
            self._priv = key.private_numbers().private_value.to_bytes(32, "big")
            self._pub = _public_bytes(key)

    @property
    def sealed(self) -> bool:
        """True if this store writes its file sealed (it has a key file)."""
        return self._key is not None

    @property
    def following(self) -> Path | None:
        """The file this store follows (`follow=`: the key store file of the collector), or None. A follower takes over
        the ratchet state of the nodes from it; it cannot provision nodes or answer mixes."""
        return self._follow

    @classmethod
    def load(cls, path: str | os.PathLike, private_pem: str | os.PathLike | None = None, **kw) -> KeyStore:
        """Read the file; a missing file gives an empty store that save() creates."""
        store = cls(path, private_pem, **kw)
        p = Path(path)
        if not p.exists():
            return store
        f = store._read_file()
        if store._pub is not None and store._pub != f.pub:
            raise SecureError("the private key does not belong to this key store")
        store._pub = f.pub
        for node_id, node in f.nodes.items():
            _prune_prev(node)
            store._nodes[node_id] = node
            store._by_key_id[node.key_id] = node_id
        store._retired = f.retired
        store._down = f.down
        store._senders = f.senders
        store._note_responder(f.responder)
        if store._host in f.senders:
            store._sender = f.senders[store._host]
        elif f.senders:  # a copy, or another process: the sender must not be shared
            store.sender_changed = True
        store._follow_refresh()
        return store

    def _read_file(self) -> _File:
        """Parse the file, sealed or plain. Version 1 (no sender, counters inside the nodes) is read as well."""
        assert self.path is not None
        try:
            data, was_sealed = _open_text(self.path.read_text(), self._key, self.path, self._require_sealed)
            self.file_was_plain = not was_sealed
            if not was_sealed and self._key is None and not self._warned_plain:
                self._warned_plain = True
                log.warning("key store %s is not sealed: its keys are readable by anyone who gets the file (%s)",
                            self.path, _ENV_KEY_FILE)
            return self._parse_file(data)
        except (KeyError, ValueError, TypeError, OSError) as exc:
            raise SecureError(f"key store {self.path} is damaged: {exc}") from exc

    def _parse_file(self, data: dict) -> _File:
        """The content of a key store file (version 1, 2 or 3). Versions before 3 migrate: a node is in generation 0."""
        version = data.get("version")
        if version not in (1, 2, FILE_VERSION):
            raise SecureError(f"unsupported key store version {version}")
        nodes = {int(text, 16): _node_from_json(n) for text, n in data["nodes"].items()}
        retired = {int(text, 16): _retired_from_json(n) for text, n in data.get("retired", {}).items()}
        down = {int(text, 16): int(n) for text, n in data.get("down_ctr", {}).items()}
        for text, n in data["nodes"].items():  # version 1: the counter was in the node
            down[int(text, 16)] = max(down.get(int(text, 16), 0), int(n.get("down_ctr", 0)))
        senders = {str(h): int(text, 16) for h, text in data.get("senders", {}).items()}
        if "sender" in data and not senders:  # one sender: the fingerprint of its host, if the file has one
            senders[data.get("host", self._host)] = int(data["sender"], 16)
        responder = data.get("responder")
        return _File(bytes.fromhex(data["backend_public_key"]), nodes, retired, senders, down,
                     str(responder) if responder else None)

    @contextlib.contextmanager
    def _file_lock(self):
        """Exclusive lock on `<file>.lock`, so that processes sharing the file take turns (re-entrant)."""
        if self.path is None or fcntl is None or self._lock_depth:
            self._lock_depth += 1
            try:
                yield
            finally:
                self._lock_depth -= 1
            return
        with _flock(self.path):
            self._lock_depth = 1
            try:
                yield
            finally:
                self._lock_depth = 0

    def _merge_from_disk(self) -> None:
        """Take over what another process wrote to the file: nodes it added, higher counters. Never lowers a
        counter. Local changes since the last save win over the file."""
        if self.path is None or not self.path.exists():
            return
        f = self._read_file()
        if self._pub is not None and f.pub != self._pub:
            raise SecureError("the key store file belongs to another backend key")
        self._note_responder(f.responder)
        for node_id, n in f.down.items():
            self._down[node_id] = max(self._down.get(node_id, 0), n)
        # processes with this fingerprint that share the file share its sender (counters are merged under the lock)
        for host, sender in f.senders.items():
            if host != self._host:
                self._senders[host] = sender
        if self._host in f.senders and not self._new_sender:
            self._sender = f.senders[self._host]
        for node_id, theirs in f.nodes.items():
            mine = self._nodes.get(node_id)
            if node_id in self._removed and mine is None:
                continue
            same = mine is not None and (mine.node_pub, mine.key_id) == (theirs.node_pub, theirs.key_id)
            if mine is None or (not same and node_id not in self._added):
                if mine is not None:
                    del self._by_key_id[mine.key_id]
                owner = self._by_key_id.get(theirs.key_id)
                if owner is None or owner == node_id:
                    _prune_prev(theirs)
                    self._nodes[node_id] = theirs
                    self._by_key_id[theirs.key_id] = node_id
            elif same:
                mine.up = _merge_window(mine.up, theirs.up)
                # a node that was provisioned again (`add_node`) starts in generation 0 with a higher `prov`: that copy
                # wins in every process, the older one is ignored; the counters merge in any case
                if theirs.prov > mine.prov:
                    _adopt_ratchet(mine, theirs)
                elif theirs.prov == mine.prov:
                    _merge_ratchet(mine, theirs)
                else:
                    _prune_prev(mine)
        for key_id, theirs in f.retired.items():
            mine = self._retired.get(key_id)
            if mine is None:
                self._retired[key_id] = theirs
            else:
                mine.down_ctr = max(mine.down_ctr, theirs.down_ctr)
                mine.prov = max(mine.prov, theirs.prov)
                mine.up = _merge_window(mine.up, theirs.up)

    def _reread_unknown(self) -> bool:
        """An envelope came with a key id this store does not know. Another process (the GUI that just provisioned a
        node, or commissioned it over BLE) may have added the node to the file: read the file again and merge it, at
        most once per `REREAD_S` for the whole store, so a flood of unknown ids costs one read a second. Call with
        `self._lock` held. Returns true if the file was read (the caller looks the key id up once more). The read needs
        no file lock: the file is replaced atomically, and nothing is written."""
        if self.path is None:
            return False
        now = _monotonic()
        if now - self._reread_at < REREAD_S:
            return False
        self._reread_at = now
        try:
            self._merge_from_disk()
        except (OSError, SecureError) as exc:
            self._warn("reread", "key store %s: re-read for an unknown key id failed: %s", self.path, exc)
            return False
        self.stats["reread"] += 1
        return True

    def _note_responder(self, recorded: str | None) -> None:
        """The file names the process that answers mixes. A responder that finds another id warns (once a minute), counts
        it (`stats["responder_conflict"]`, the collector exports it) and goes on answering: two responders on one store
        can each lose the answer the node used."""
        self._file_responder = recorded
        if not (self.responder and self.responder_id and recorded and recorded != self.responder_id):
            self._seen_responder = None
            return
        if recorded != self._seen_responder:
            self._seen_responder = recorded
            self.stats["responder_conflict"] += 1
        self._warn("responder",
                   "key store %s records another responder (%s, this one is %s): one responder per key store",
                   self.path, recorded, self.responder_id)

    def save(self) -> None:
        """Write the file. Under a file lock, the file is read first and merged, so that a node another process
        added or a counter it advanced is not lost; the file is synced before it replaces the old one."""
        if self.path is None:
            return
        with self._lock, self._file_lock():
            self._merge_from_disk()
            _ = self.sender  # makes it if this store has none yet
            data = {
                "version": FILE_VERSION,
                "senders": {
                    h: f"{s:08x}" for h, s in sorted(self._senders.items())
                },
                "backend_public_key": self.backend_public_key().hex(),
                "nodes": {
                    _node_text(i): _node_to_json(n) for i, n in sorted(self._nodes.items())
                },
                "retired": {
                    f"{k:08x}": _retired_to_json(n) for k, n in sorted(self._retired.items())
                },
                "down_ctr": {
                    _node_text(i): n for i, n in sorted(self._down.items())
                },
            }
            responder = self.responder_id or self._file_responder  # a responder takes the record, others keep it
            if responder:
                data["responder"] = responder
                self._file_responder = responder
            inner = json.dumps(data, indent = 1)
            _write_atomic(self.path, _seal_text(inner.encode(), self._key) if self._key is not None else inner)
            self._dirty = False
            self._added.clear()
            self._removed.clear()
            self._last_save = time.monotonic()

    def _touch(self, now: bool = False) -> None:
        """Mark the store changed and save it if `now`, or if the last save is old. The save of a replay counter
        (`now` false) may fail (disk error, or a key file or key store file that changed under the process) without losing
        the message that moved it: it is tried again later. A counter that
        must not be reused (`now` true) raises."""
        self._dirty = True
        if self.path is None:
            return
        if now:
            self.save()
        elif time.monotonic() - self._last_save >= self.autosave_s:
            try:
                self.save()
            except (OSError, SecureError) as exc:
                # A SecureError here: the file or its key changed under the running process (rotate-file-key,
                # seal-file). The counter is marked in RAM already, so the message is still good: it is returned, the
                # save is tried again later and the key store keeps working in RAM.
                self._last_save = time.monotonic()  # do not retry on every message
                log.warning("key store %s not saved, will retry: %s", self.path, exc)

    def flush(self) -> None:
        """Write pending counter updates (call at shutdown)."""
        with self._lock:
            if self._dirty:
                self.save()

    @property
    def sender(self) -> int:
        """The sender id of this store in the downlinks it seals (32 bit, not 0). Made at random the first time and
        kept in the file."""
        if self._sender is None:
            self._sender = self._unused_sender()
        self._senders[self._host] = self._sender
        return self._sender

    def _unused_sender(self) -> int:
        """A random sender id that no host of the file uses (the same sender twice would use one nonce twice under the
        downlink key). The file is merged before: `save` and `new_sender` do it, `load` has read it."""
        taken = set(self._senders.values())
        while True:
            sender = secrets.randbelow(2**32 - 1) + 1
            if sender not in taken:
                return sender

    def new_sender(self) -> int:
        """Take a new sender id and save it. For a copy of a key store file that another host uses: without it both
        hosts seal with the same sender and could use one counter twice."""
        with self._lock, self._file_lock():
            self._merge_from_disk()
            self._sender = self._unused_sender()
            self._senders[self._host] = self._sender
            self._new_sender = True  # the save must not take the old one back from the file
            try:
                self._touch(now = True)
            finally:
                self._new_sender = False
            return self._sender

    def set_epoch_rate(self, node_id: int, rate: float | None) -> None:
        """Epochs per second that a node may start, for the time bound of the chain walk (default 1, which is 64 messages
        per second). Set it higher for a node that streams over another link (BLE, USB) and that the collector may not
        hear for hours: without it its first frame after such a gap is refused (`GapError`). None: the default again. Not
        saved; set it at start."""
        self.walker.set_epoch_rate(node_id, rate)

    def down_counter(self, node_id: int) -> int:
        """Last downlink counter used for a node (0 if none)."""
        return self._down.get(node_id, 0)

    def backend_public_key(self) -> bytes:
        if self._pub is None:
            raise SecureError("no backend key: run backend_keygen and load the store with its PEM")
        return self._pub

    def add_node(self, node_id: int, node_pub: bytes, reset: bool = True) -> NodeBackup:
        """Derive the keys of a node from its public key and start it in generation 0 (the node does the same when it is
        provisioned: it drops its ratchet state). A node that is added again with the same key keeps its counters (also
        what the key store has seen of it), but loses the ratchet state: `cur`, `next` and the previous chain are gone. A
        new key starts new counters. The key of a node counts as the same if the node public key and the key id are.

        The reset is saved at once and counts in every process that shares the file and in every follower: the node
        gets a higher provisioning counter (`prov`), and a copy with a lower one is ignored in a merge, so another
        process that still holds generation 1 cannot bring it back. With `reset=False` a node that is known with the
        same key is left as it is (nothing is reset, nothing is saved).

        The reset happens before the node confirms. Keep the returned `NodeBackup` while the node is provisioned and
        call `rollback_node` if that fails: the node then still has its old keys and the key store would refuse it."""
        if self._priv is None or self._pub is None:
            raise SecureError("the backend private key is not loaded")
        if not 0 <= node_id < 2**64:
            raise SecureError("bad node id")
        keys = derive_keys(self._priv, node_pub, node_pub, self._pub)
        with self._lock, self._file_lock():
            self._merge_from_disk()
            old = self._nodes.get(node_id)
            owner = self._by_key_id.get(keys.key_id)
            if owner is not None and owner != node_id:
                raise SecureError(f"key id {keys.key_id:08x} is used by node {_node_text(owner)}")
            same = old is not None and (old.node_pub, old.key_id) == (node_pub, keys.key_id)
            if same and (not reset or
                         (old.cur.gen == 0 and old.next is None and old.prev is None and old.k_up is not None)):
                return NodeBackup(node_id, old, old.prov, False)  # nothing to reset
            retired_before, down_before = dict(self._retired), self._down.get(node_id)
            window = ReplayWindow()
            prov = 0
            if same:
                window = ReplayWindow(old.up.highest, old.up.bitmap)  # a copy: the old node is kept for the backup
                prov = old.prov + 1
                del self._by_key_id[old.key_id]
            else:
                if old is not None:
                    prov = old.prov + 1
                    self._retire(old)
                    del self._by_key_id[old.key_id]
                # the same key as before (removed and added again) goes on from its counters
                back = self._retired.pop(keys.key_id, None)
                if back is not None and back.node_pub == node_pub:
                    window = back.up
                    prov = max(prov, back.prov + 1)
                    self._down[node_id] = max(self._down.get(node_id, 0), back.down_ctr)
            self.walker.forget_node(node_id)
            node = _fresh_node(node_pub, keys, window)
            node.cur.since = self._clock()
            node.prov = prov
            self._nodes[node_id] = node
            self._by_key_id[keys.key_id] = node_id
            self._added.add(node_id)
            self._removed.discard(node_id)
            try:
                self._touch(now = True)
            except BaseException:  # not saved: nothing changed, in memory either
                self._by_key_id.pop(keys.key_id, None)
                if old is None:
                    del self._nodes[node_id]
                else:
                    self._nodes[node_id] = old
                    self._by_key_id[old.key_id] = node_id
                self._retired = retired_before
                if down_before is None:
                    self._down.pop(node_id, None)
                else:
                    self._down[node_id] = down_before
                self._added.discard(node_id)
                raise
            return NodeBackup(node_id, old, prov, True)

    def rollback_node(self, backup: NodeBackup) -> None:
        """Undo an `add_node` that changed the node, because the provisioning of the node failed: the node has its old keys
        and ratchet state again, in this process, in the file, in the other processes and in the followers of it (the
        restored node gets a `prov` above the reset one). Counters only go up: what was received or sent meanwhile stays
        counted. A node that was new is removed again (its counters are kept for the same key). Does nothing for a
        backup of an `add_node` that changed nothing."""
        if not backup.changed:
            return
        node_id, old = backup.node_id, backup.old
        with self._lock, self._file_lock():
            self._merge_from_disk()
            cur = self._nodes.get(node_id)
            if old is None:
                if cur is not None:
                    self.remove_node(node_id)
                return
            owner = self._by_key_id.get(old.key_id)
            if owner is not None and owner != node_id:
                raise SecureError(f"key id {old.key_id:08x} is used by node {_node_text(owner)}")
            window = ReplayWindow(old.up.highest, old.up.bitmap)
            prov = max(backup.prov, old.prov)
            if cur is not None:
                prov = max(prov, cur.prov)
                if (cur.node_pub, cur.key_id) == (old.node_pub, old.key_id):
                    window = _merge_window(window, cur.up)
                else:
                    self._retire(cur)
                del self._by_key_id[cur.key_id]
            back = self._retired.pop(old.key_id, None)
            if back is not None and back.node_pub == old.node_pub:
                window = _merge_window(window, back.up)
                prov = max(prov, back.prov)
            self.walker.forget_node(node_id)
            self._nodes[node_id] = _Node(old.node_pub, old.key_id, old.k_up, old.k_down, window, old.cur, old.next,
                                         old.prev, old.t_up, prov + 1)
            self._by_key_id[old.key_id] = node_id
            self._added.add(node_id)
            self._removed.discard(node_id)
            self._touch(now = True)

    def remove_node(self, node_id: int) -> None:
        with self._lock, self._file_lock():
            self._merge_from_disk()
            node = self._nodes.pop(node_id, None)
            if node is not None:
                self._retire(node)
                del self._by_key_id[node.key_id]
                self._added.discard(node_id)
                self._removed.add(node_id)
                self.walker.forget_node(node_id)
                self._touch(now = True)

    def _retire(self, node: _Node) -> None:
        """Keep the counters of a key that is dropped: if the key comes back it must not restart them. The keys and the
        ratchet state are not kept."""
        self._retired[node.key_id] = _Retired(node.node_pub, node.key_id, node.up, prov = node.prov)

    def node_ids(self) -> list[int]:
        return sorted(self._nodes)

    def has_node(self, node_id: int) -> bool:
        return node_id in self._nodes

    def key_id(self, node_id: int) -> int:
        return self._nodes[node_id].key_id

    def generation(self, node_id: int) -> int:
        """Generation in use for the node: 0 until its first mix is used."""
        with self._lock:
            self._follow_refresh()
            return self._nodes[node_id].cur.gen

    def node_state(self, node_id: int) -> NodeState:
        """What a GUI or the metrics show of a node: generation, an answered mix that waits, the previous chain."""
        with self._lock:  # _import of a follower must not run in the middle of a commit or a promotion
            self._follow_refresh()
            n = self._nodes[node_id]
            up = n.cur.up
            return NodeState(
                gen = n.cur.gen,
                gen_since = n.cur.since or None,
                next_gen = None if n.next is None else n.next.gen,
                next_since = None if n.next is None else n.next.up.t,
                prev_gen = None if n.prev is None else n.prev.gen,
                prev_ends = None if n.prev is None else n.prev.c1,
                up_idx = None if up is None else up.idx,
                up_base = None if up is None else up.up_base,
                last_verified = None if up is None or not up.t else up.t,
                last_uplink = n.t_up or None,
                highest = n.up.highest,
                static_keys = n.k_up is not None,
            )

    # ---- the mix: the collector answers a request of a node ----

    def _warn(self, key: str, msg: str, *args) -> None:
        """A log line at most once a minute per key (a flood of forged frames must not flood the log)."""
        now = self._clock()
        last = self._log_at.get(key)
        if last is not None and 0 <= now - last < 60.0:
            return
        self._log_at[key] = now
        log.warning(msg, *args)

    def rekey_answer(self, node_id: int, req: RekeyRequest) -> RekeyAnswer | None:
        """Answer the RekeyRequest of a node (design 1.3). Call it for a request in an uplink that was opened with `cur`
        (a request of the generation after `cur`); only on a store opened with responder=True.

        Under the file lock: if the answer to this request exists (same generation, up_base and node key) it is returned
        again, else a new ephemeral key is made, the mix is computed, the private ephemeral key is dropped (Python has
        no way to wipe it from memory), `next` is stored and the file synced. Only then the answer is returned, so a
        crash cannot lose the key of an answer that was sent. Processes on one file give one answer: the second finds
        the first one's `next`. Returns None for a request that is ignored (generation not the one after `cur`, a node
        that is unknown, a bad key or up_base; logged)."""
        if not self.responder:
            raise SecureError("this key store does not answer mixes (responder=False, SENSEI_REKEY_RESPONDER)")
        with self._lock, self._file_lock():
            self._merge_from_disk()
            node = self._nodes.get(node_id)
            if node is None:
                self._warn(f"rk-unknown-{node_id}", "rekey request from unknown node %s", _node_text(node_id))
                return None
            cur = node.cur
            if req.gen != cur.gen + 1:  # an old request, or a node that is ahead
                self.stats["rekey_ignored"] += 1
                self._warn(f"rk-gen-{node_id}", "rekey request of node %s for generation %d ignored (current %d)",
                           _node_text(node_id), req.gen, cur.gen)
                return None
            if len(req.pub) != ratchet.KEY_SIZE or not 0 < req.up_base < _MAX_EPOCH_IDX:
                self.stats["rekey_ignored"] += 1
                self._warn(f"rk-bad-{node_id}", "rekey request of node %s is malformed", _node_text(node_id))
                return None
            if cur.root is None:
                raise SecureError("this key store has no root key for the node (a follower cannot answer)")
            nxt = node.next
            if nxt is not None and nxt.same_request(req.gen, req.up_base, req.pub):
                self.stats["rekey_resent"] += 1
                return RekeyAnswer(nxt.gen, nxt.e_c_pub, nxt.up.up_base)
            e_c_priv, e_c_pub = ratchet.x25519_keygen()
            try:
                mix = ratchet.mix_collector(cur.root, node.key_id, req.gen, req.up_base, e_c_priv, req.pub)
            except ratchet.DhError:
                self.stats["rekey_ignored"] += 1
                self._warn(f"rk-dh-{node_id}", "rekey request of node %s has a low-order public key",
                           _node_text(node_id))
                return None
            finally:
                del e_c_priv  # not kept anywhere
            # a request that replaces the open answer (the node dropped its state and asked again) makes a newer one, which
            # wins a merge with the file
            # `t` stays at the first unanswered request when a request is replaced: a node that never mixes keeps its age
            node.next = _Next(req.gen, req.pub, e_c_pub, mix.root, mix.k_down,
                              ChainState(req.gen, req.up_base, 0, mix.ck,
                                         self._clock() if nxt is None else nxt.up.t), 1 if nxt is None else nxt.seq + 1)
            try:
                self._touch(now = True)  # fsync before the answer leaves
            except BaseException:
                node.next = nxt  # nothing was sent: the old state stays
                raise
            self.stats["rekey_answered"] += 1
            log.info("node %s: answered the mix to generation %d (up_base %d)", _node_text(node_id), req.gen,
                     req.up_base)
            return RekeyAnswer(req.gen, e_c_pub, req.up_base)

    def _commit(self, node_id: int, node: _Node, sel: _Selection, h: Header, walked: Walked | None) -> bool:
        """A message verified (the lock is held): the replay window, the stored chain key, the promotion of an answered
        mix, the previous chain dropped when its end is behind the window. True if the node was promoted."""
        now = self._clock()
        promoted = sel.which == "next"
        node.t_up = now
        if promoted:
            _promote(self, node_id, node, sel, h, walked, now)
        else:
            node.up.update(h.counter)
            if sel.st is not None and sel.st.gen != 0:
                idx = ratchet.epoch(h.counter) - sel.st.up_base
                if sel.which == "cur":
                    want = max(0, ratchet.epoch(node.up.highest) - 1 - sel.st.up_base)
                else:  # the previous chain: the counters of the window may still be this epoch or the one before
                    want = max(0, idx - 1)
                _advance(self, sel.st, idx, walked, want, now)
        _prune_prev(node)
        try:
            self._touch(now = promoted)  # a promotion is synced to disk at once
        except (OSError, SecureError) as exc:
            # The message is verified and the node is promoted in memory: keep both and save again with the next
            # message (or `flush`). A crash before that is no loss: `next` is still in the file, the next uplink of the
            # new generation promotes it again.
            self.stats["save_failed"] += 1
            log.warning("key store %s: the promotion of node %s is not saved yet, will retry: %s", self.path,
                        _node_text(node_id), exc)
        return promoted

    # ---- followers ----

    def _follow_refresh(self) -> None:
        """A follower (`follow=`): read the followed file again if it changed (a stat per call; the file is replaced
        atomically) and take over the ratchet state of the nodes it knows. Takes the store lock (re-entrant), so the
        import never runs in the middle of a commit or a promotion on another thread."""
        if self._follow is None:
            return
        try:
            st = self._follow.stat()
        except OSError:
            return
        sig = (st.st_mtime_ns, st.st_size, st.st_ino)
        with self._lock:
            if sig == self._follow_sig:
                return
            self._follow_sig = sig
            f = self._read_followed()
            if f is None:
                return
            for node_id, theirs in f.nodes.items():
                mine = self._nodes.get(node_id)
                if mine is not None and (mine.node_pub, mine.key_id) == (theirs.node_pub, theirs.key_id):
                    self._import(mine, theirs)
                    self._dirty = True

    def _read_followed(self) -> _File | None:
        assert self._follow is not None
        try:
            data, _ = _open_text(self._follow.read_text(), self._follow_key, self._follow)
            return self._parse_file(data)
        except (KeyError, ValueError, TypeError, OSError, SecureError) as exc:
            self._warn("follow", "cannot follow key store %s: %s", self._follow, exc)
            return None

    def _import(self, mine: _Node, theirs: _Node) -> None:
        """Take over cur, next (without root) and prev_up of the followed copy: never the root key, the replay window or
        the counters. The chain of the followed file is used when it is a later generation, or further in the same one. A
        copy with a higher `prov` (the node was provisioned again there) replaces everything, a lower one is ignored."""
        a, b = mine.cur, theirs.cur
        mine.t_up = max(mine.t_up, theirs.t_up)
        if theirs.prov < mine.prov:
            return
        if theirs.prov > mine.prov:
            mine.cur = _Cur(b.gen, None, b.k_down, None if b.up is None else b.up.copy(), b.since)
            mine.prev = None if theirs.prev is None else theirs.prev.copy()
            mine.next = None
            mine.k_up, mine.k_down = theirs.k_up, theirs.k_down
            mine.prov = theirs.prov
        elif _cur_pos(b) > _cur_pos(a):
            up = None if b.up is None else b.up.copy()
            mine.cur = _Cur(b.gen, None, b.k_down, up, b.since)
            if b.gen > a.gen:
                mine.prev = None if theirs.prev is None else theirs.prev.copy()
        elif a.up is not None and b.up is not None and a.gen == b.gen:
            a.up.t = max(a.up.t, b.up.t)
        cur = mine.cur
        if theirs.next is not None and theirs.next.gen == cur.gen + 1:
            if mine.next is None or not mine.next.same_request(theirs.next.gen, theirs.next.up.up_base,
                                                               theirs.next.e_n_pub):
                t = theirs.next
                mine.next = _Next(t.gen, t.e_n_pub, t.e_c_pub, None, t.k_down, t.up.copy(), t.seq)
        elif mine.next is not None and (mine.next.gen != cur.gen + 1 or a.gen == b.gen):
            mine.next = None  # the followed file has dropped or replaced it
        if mine.prev is None and theirs.prev is not None and theirs.prev.gen == cur.gen - 1:
            mine.prev = theirs.prev.copy()
        _prune_prev(mine)

    def _resync(self) -> bool:
        """Another process on the same file may have promoted a node, or a follower's file has changed: look again, at most
        once a second. True if something was read."""
        self._follow_refresh()
        now = time.monotonic()
        if self.path is None or 0 <= now - self._resync_at < 1.0:
            return False
        self._resync_at = now
        with self._lock, self._file_lock():
            self._merge_from_disk()
        return True


def backend_keygen(private_pem_path: str | os.PathLike) -> bytes:
    """Create the backend key pair. The PEM (mode 0600, no passphrase) must stay out of git and backups
    that others can read. Refuses to overwrite a key. Returns the 65 B public key."""
    path = Path(private_pem_path)
    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    path.parent.mkdir(parents = True, exist_ok = True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    return _public_bytes(key)


def seal_file(path: str | os.PathLike, key_file: str | os.PathLike, create_key: bool = False) -> None:
    """Seal a plain key store file in place (under the file lock, the old file stays until the new one is on disk).
    `create_key` makes the key file first if it does not exist. Stop the processes that use the file, or give them the
    key file and restart them: a store without the key cannot read the sealed file."""
    p, kf = Path(path), Path(key_file)
    if create_key and not kf.exists():
        new_key_file(kf)
    key = load_key_file(kf)
    with _flock(p):
        inner, was_sealed = _open_text(p.read_text(), key, p)
        if was_sealed:
            raise SecureError(f"key store {p} is sealed already")
        _write_atomic(p, _seal_text(json.dumps(inner, indent = 1).encode(), key))


def unseal_file(path: str | os.PathLike, key_file: str | os.PathLike) -> None:
    """Write a sealed key store file back as plain JSON, in place. The keys of all nodes are then readable by anyone who
    gets the file."""
    p = Path(path)
    key = load_key_file(key_file)
    with _flock(p):
        inner, was_sealed = _open_text(p.read_text(), key, p)
        if not was_sealed:
            raise SecureError(f"key store {p} is not sealed")
        _write_atomic(p, json.dumps(inner, indent = 1))


def rotate_file_key(
    path: str | os.PathLike,
    key_file: str | os.PathLike,
    new_key: str | os.PathLike,
    create_key: bool = False,
) -> None:
    """Seal a sealed key store file again under another key file. Stop the processes that use it (or change their key file
    and restart them) at the same time: the old key no longer opens the file. Keep the old key file until the new
    one works, then destroy it and every backup of the old file."""
    p, old, new = Path(path), Path(key_file), Path(new_key)
    old_key = load_key_file(old)
    if create_key and not new.exists():
        new_key_file(new)
    new_bytes = load_key_file(new)
    if _file_key_id(new_bytes) == _file_key_id(old_key):
        raise SecureError("the new key file holds the old key")
    with _flock(p):
        inner, was_sealed = _open_text(p.read_text(), old_key, p)
        if not was_sealed:
            raise SecureError(f"key store {p} is not sealed: use seal-file")
        _write_atomic(p, _seal_text(json.dumps(inner, indent = 1).encode(), new_bytes))


@dataclass(frozen = True)
class Opened:
    """A verified uplink: who, what kind, the plaintext, the generation it was sealed in, and whether it made the answered
    mix the current generation of the node (the first uplink of a new generation)."""

    node_id: int
    kind: int
    plaintext: bytes
    gen: int
    promoted: bool = False


@dataclass(frozen = True)
class _Selection:
    which: str  # "static" (generation 0 key), "cur", "next" or "prev"
    gen: int
    st: ChainState | None  # None: the static key
    key: bytes | None  # the static key, copied under the lock
    snapshot: tuple[int, bytes] | None  # (idx, ck) of the chain as read under the lock
    ref_idx: int | None = None  # where the time bound counts from (see Walker.precheck)
    t_ref: float | None = None


def _select(node: _Node, h: Header, walker: Walker, now: float, node_id: int | None = None) -> _Selection:
    """The key material for an envelope, by its generation bits (cur, next and prev differ mod 4). Cheap: no walk, no
    crypto. Raises GenerationError (no such generation: a node that is ahead, or a generation that was dropped), EpochError
    or GapError."""
    cur = node.cur
    if not h.ratchet:
        if cur.gen == 0:
            return _Selection("static", 0, None, node.k_up, None)
        prev = node.prev
        if prev is not None and prev.gen == 0:
            if h.counter >= prev.c1:
                raise SecureError("generation 0 envelope beyond the end of generation 0")
            return _Selection("prev", 0, prev, node.k_up, None)
        raise GenerationError(f"generation 0 envelope from a node that is in generation {cur.gen}: dropped")
    bits = h.gen_bits
    if cur.gen >= 1 and cur.gen & 3 == bits:
        which, gen, st = "cur", cur.gen, cur.up
    elif node.next is not None and node.next.gen & 3 == bits:
        which, gen, st = "next", node.next.gen, node.next.up
    elif node.prev is not None and node.prev.gen >= 1 and node.prev.gen & 3 == bits:
        if h.counter >= node.prev.c1:
            raise SecureError(f"generation {node.prev.gen} envelope beyond the end of its generation")
        which, gen, st = "prev", node.prev.gen, node.prev
    else:
        raise GenerationError(f"no key for generation {bits} (mod 4) of this node (current {cur.gen}): "
                              "the node may be ahead of the key store, provision it again")
    idx = ratchet.epoch(h.counter) - st.up_base
    if idx < 0:
        raise EpochError(f"epoch {ratchet.epoch(h.counter)} is before the start of the chain ({st.up_base})")
    ref_idx = t_ref = None
    if which == "next":
        # The first uplink of the new generation is the next epoch boundary after the switch: far above up_base if the
        # request waited long. Count from the epoch of the last verified uplink of the node, not from the head.
        ref_idx = max(0, ratchet.epoch(node.up.highest) - st.up_base)
        t_ref = node.t_up or st.t
    walker.precheck(st, idx, now, None, ref_idx, t_ref, node_id)
    return _Selection(which, gen, st, None, (st.idx, st.ck), ref_idx, t_ref)


def _open_up(payload: bytes, store: KeyStore, commit: bool) -> Opened:
    h = parse_header(payload)
    if h.direction != DIR_UP or h.kind == KIND_COMMISSION:
        raise SecureError("not a node to backend envelope")
    resynced = False
    for _attempt in range(4):
        # 1. under the lock: look up the node, replay check, pick the key, refuse what is cheap to refuse
        with store._lock:
            store._follow_refresh()
            node_id = store._by_key_id.get(h.key_id)
            if node_id is None and store._reread_unknown():  # a node that another process added since the last read
                node_id = store._by_key_id.get(h.key_id)
            if node_id is None:
                raise UnknownKeyError(f"unknown key id {h.key_id:08x}")
            if h.node_id is not None and h.node_id != node_id:
                raise SecureError("node id does not match the key")
            node = store._nodes[node_id]
            if commit:
                node.up.check(h.counter)
            now = store._clock()
            try:
                sel = _select(node, h, store.walker, now, node_id)
            except GenerationError:
                if not resynced and store._resync():
                    resynced = True
                    continue
                store.stats["node_ahead"] += 1
                store._warn(
                    f"ahead-{node_id}", "node %s sent generation %d, the key store is at %d: node ahead of key "
                    "store, re-provision it", _node_text(node_id), h.gen_bits, node.cur.gen)
                raise
            except EpochError:
                store.stats["epoch_refused"] += 1
                raise
            except GapError as exc:
                store.stats["gap_refused"] += 1
                store._warn(f"gap-{node_id}", "node %s: %s (a forged frame, or the node was silent for long)",
                            _node_text(node_id), exc)
                raise
        # 2. outside the lock: the walk (limited, cached) and the tag
        walked = None
        if sel.st is None:
            key = sel.key
        elif sel.st.gen == 0:
            key = sel.key
        else:
            idx = ratchet.epoch(h.counter) - sel.st.up_base
            try:
                walked = store.walker.walk(node_id, sel.st, idx, sel.snapshot, now, sel.ref_idx, sel.t_ref)
            except WalkBudgetError as exc:
                store.stats["budget_refused"] += 1
                store._warn(f"budget-{node_id}", "node %s: %s", _node_text(node_id), exc)
                raise
            key = ratchet.message_key(walked.ck)
        try:
            plain = open_raw(key, payload, h)
        except SecureError:
            store.stats["auth_failed"] += 1
            raise
        if not commit:
            return Opened(node_id, h.kind, plain, sel.gen)
        # 3. under the lock again: the state may have moved meanwhile; commit only if the same key is still the right one
        with store._lock:
            node2 = store._nodes.get(node_id)
            if node2 is not node:
                continue
            node.up.check(h.counter)  # a duplicate that was processed meanwhile
            try:
                again = _select(node, h, store.walker, store._clock(), node_id)
            except (GenerationError, GapError, EpochError):
                continue
            if (again.which, again.gen) != (sel.which, sel.gen) or again.st is not sel.st:
                continue
            promoted = store._commit(node_id, node, sel, h, walked)
            return Opened(node_id, h.kind, plain, sel.gen, promoted)
    raise SecureError("key store changed while the envelope was opened, try again")


def _advance(store: KeyStore, st: ChainState, idx: int, walked: Walked | None, want: int, now: float) -> None:
    """After a verified message at chain index idx: keep the chain key at max(idx_lo, min(idx, want)); want is
    idx(highest) - 1 for the current chain (the 64 counters of the window span two epochs), idx - 1 for the previous one.
    The key comes from the walk (the key at idx or idx - 1); nothing is derived under the lock."""
    st.t = now
    lo = max(st.idx, min(idx, want))
    if lo <= st.idx or walked is None:
        return
    if lo == idx:
        ck = walked.ck
    elif lo == idx - 1 and walked.ck_before is not None:
        ck = walked.ck_before
    else:
        return  # the chain moved on its own meanwhile: the next message advances it
    st.ck, st.idx = ck, lo
    store.walker.drop_below(st, lo)


def _promote(store: KeyStore, node_id: int, node: _Node, sel: _Selection, h: Header, walked: Walked | None,
             now: float) -> None:
    """The first uplink of generation g+1 verified against `next`: it becomes `cur`, the chain of g becomes `prev_up`
    (ending before counter c1 = this one), the downlink key and the root key of g are dropped."""
    nxt = node.next
    assert nxt is not None and walked is not None
    old = node.cur
    prev = old.up if old.up is not None else ChainState(0, 0, 0, b"", now)
    prev.c1 = h.counter
    prev.t = now
    chain = nxt.up
    node.up.update(h.counter)  # the window first: _advance wants the highest counter
    _advance(store, chain,
             ratchet.epoch(h.counter) - chain.up_base, walked, max(0,
                                                                   ratchet.epoch(node.up.highest) - 1 - chain.up_base),
             now)
    node.prev = prev
    node.cur = _Cur(nxt.gen, nxt.root, nxt.k_down, chain, now)
    node.next = None
    store.stats["promoted"] += 1
    log.info("node %s: generation %d is in use", _node_text(node_id), nxt.gen)


def open_sealed(payload: bytes, store: KeyStore) -> tuple[int, int, bytes]:
    """Open a sealed payload from a node: (node_id, kind, plaintext). Raises SecureError (UnknownKeyError,
    ReplayError, GenerationError, EpochError, GapError are subclasses) if the key is unknown, the tag does not verify,
    the counter was seen or no key can be derived at a reasonable cost. A verified uplink of a generation that the store
    has not used yet (the answered mix) makes it the current one. `open_sealed_ex` also tells the generation."""
    o = open_sealed_ex(payload, store)
    return o.node_id, o.kind, o.plaintext


def open_sealed_ex(payload: bytes, store: KeyStore) -> Opened:
    """Like open_sealed, with the generation the message was sealed in and whether it promoted the mix."""
    return _open_up(payload, store, commit = True)


def peek_sealed(payload: bytes, store: KeyStore) -> tuple[int, bytes]:
    """Like open_sealed, but the replay window and the chains are not touched (and a mix is not promoted): a second
    reader of the same frames (the GUI recorder) may look inside without making the real reader see a replay. The walk
    obeys the same limits. Returns (kind, plaintext)."""
    o = _open_up(payload, store, commit = False)
    return o.kind, o.plaintext


def seal_downlink(node_id: int, kind: int, plaintext: bytes, store: KeyStore) -> bytes:
    """Seal a Downlink (KIND_DOWNLINK) or a settings write (KIND_SETTINGS) for a node, with the downlink key of the
    generation in use (generation 0: the static key and the format of version 3 as it always was). Before the node
    has used an answered mix, that is still the old generation: the node keeps its key until its next request."""
    if kind not in (KIND_DOWNLINK, KIND_SETTINGS):
        raise SecureError(f"kind {kind} is not a downlink kind")
    with store._lock, store._file_lock():
        store._follow_refresh()
        store._merge_from_disk()  # another process may have used counters, or promoted the node
        node = store._nodes.get(node_id)
        if node is None:
            raise UnknownKeyError(f"unknown node {_node_text(node_id)}")
        counter = store._down.get(node_id, 0)
        if counter >= _CTR_MAX:
            raise SecureError(
                f"downlink counter of node {_node_text(node_id)} used up (2^32 downlinks): the key store never "
                "reuses a counter, so it cannot seal more downlinks for this node id")
        counter += 1
        store._down[node_id] = counter
        out = seal_raw(node.cur.k_down,
                       node.key_id,
                       counter,
                       kind,
                       plaintext,
                       sender = store.sender,
                       gen = node.cur.gen)
        store._touch(now = True)  # the counter must never be reused
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog = "python -m sensei_sdk.secure", description = "Key store tools.")
    sub = ap.add_subparsers(dest = "cmd", required = True)
    ns = sub.add_parser("new-sender", help = "give a copy of a key store its own sender id")
    ns.add_argument("keystore", help = "key store file")
    nk = sub.add_parser("new-key-file", help = "make a 32 byte key file for sealed key stores (mode 0400)")
    nk.add_argument("key_file", help = "key file to create (never overwritten)")
    sf = sub.add_parser("seal-file", help = "seal a plain key store file in place")
    sf.add_argument("keystore", help = "key store file")
    sf.add_argument("--key-file",
                    default = os.environ.get(_ENV_KEY_FILE),
                    help = f"key file (default: ${_ENV_KEY_FILE})")
    sf.add_argument("--create-key", action = "store_true", help = "make the key file if it does not exist")
    uf = sub.add_parser("unseal-file", help = "write a sealed key store file back as plain JSON, in place")
    uf.add_argument("keystore", help = "key store file")
    uf.add_argument("--key-file",
                    default = os.environ.get(_ENV_KEY_FILE),
                    help = f"key file (default: ${_ENV_KEY_FILE})")
    rk = sub.add_parser("rotate-file-key", help = "seal a sealed key store file again under a new key file")
    rk.add_argument("keystore", help = "key store file")
    rk.add_argument("--key-file", default = os.environ.get(_ENV_KEY_FILE), help = "the current key file")
    rk.add_argument("--new-key-file", required = True, help = "the new key file")
    rk.add_argument("--create-key", action = "store_true", help = "make the new key file if it does not exist")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "new-sender":
            store = KeyStore.load(args.keystore)
            print(f"sender id {store.new_sender():08x}")
            return 0
        if args.cmd == "new-key-file":
            new_key_file(args.key_file)
            print(f"wrote {args.key_file}: keep it outside the key store folder and its backups")
            return 0
        if not args.key_file:
            ap.error(f"--key-file or ${_ENV_KEY_FILE} is needed")
        if args.cmd == "seal-file":
            seal_file(args.keystore, args.key_file, args.create_key)
            print(f"sealed {args.keystore}: give the processes the key file ({_ENV_KEY_FILE}) and restart them")
        elif args.cmd == "unseal-file":
            unseal_file(args.keystore, args.key_file)
            print(f"{args.keystore} is plain now: the keys of all nodes are readable by anyone who gets the file")
        elif args.cmd == "rotate-file-key":
            rotate_file_key(args.keystore, args.key_file, args.new_key_file, args.create_key)
            print(f"sealed {args.keystore} under {args.new_key_file}: switch the processes to it and restart them")
    except (SecureError, OSError, ValueError) as exc:
        print(f"error: {exc}", file = sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
