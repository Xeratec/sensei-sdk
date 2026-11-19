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
"""

from __future__ import annotations

import argparse
import base64
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

from .framing import looks_sealed

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

# direction bit of the first byte
DIR_UP = 0  # node to backend
DIR_DOWN = 1  # backend to node

_KIND_DIR = {KIND_UPLINK: DIR_UP, KIND_NODE_TO_HOST: DIR_UP, KIND_DOWNLINK: DIR_DOWN, KIND_SETTINGS: DIR_DOWN}
_CTR_MAX = 0xFFFFFFFF


class SecureError(Exception):
    """Authentication, replay, format or key error."""


class UnknownKeyError(SecureError):
    pass


class ReplayError(SecureError):
    pass


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
) -> bytes:
    """Build one envelope. The caller owns the counter. A backend to node kind needs a sender id (not 0), a node
    to backend kind has none."""
    if kind not in _KIND_DIR:
        raise SecureError(f"unknown kind {kind}")
    if not 0 < counter <= _CTR_MAX:
        raise SecureError("counter out of range")
    direction = _KIND_DIR[kind]
    if direction == DIR_DOWN:
        if not 0 < sender <= 0xFFFFFFFF:
            raise SecureError("a downlink needs a sender id")
    elif sender:
        raise SecureError("an uplink has no sender id")
    first = MARKER | (VERSION << 2) | ((node_id is not None) << 1) | direction
    header = bytes([first, kind])
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
    kind = payload[1]
    node_id = struct.unpack_from("<Q", payload, 2)[0] if has_node else None
    key_id = struct.unpack_from("<I", payload, 10 if has_node else 2)[0]
    sender = struct.unpack_from("<I", payload, size - 8)[0] if direction == DIR_DOWN else 0
    counter = struct.unpack_from("<I", payload, size - 4)[0]
    if _KIND_DIR.get(kind) != direction:
        raise SecureError("kind does not match direction")
    if direction == DIR_DOWN and sender == 0:
        raise SecureError("downlink without a sender id")
    return Header(direction, kind, node_id, key_id, sender, counter, size)


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
    next session of the same node."""

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

    def seal(self, kind: int, plaintext: bytes, node_id: int | None = None, counter: int | None = None) -> bytes:
        self.tx_counter = counter if counter is not None else self.tx_counter + 1
        return seal_raw(self.keys.k_up, self.keys.key_id, self.tx_counter, kind, plaintext, node_id)

    def open(self, payload: bytes) -> tuple[int, bytes]:
        h = parse_header(payload)
        if h.direction != DIR_DOWN:
            raise SecureError("wrong direction")
        if h.key_id != self.keys.key_id:
            raise UnknownKeyError("wrong key id")
        window = self.rx.get(h.sender)
        if window is None and len(self.rx) >= MAX_SENDERS:
            raise SecureError("too many senders")
        (window or ReplayWindow()).check(h.counter)
        plain = open_raw(self.keys.k_down, payload, h)
        if window is None:
            window = self.rx[h.sender] = ReplayWindow()
        window.update(h.counter)
        return h.kind, plain


# ---- sealed settings records (node, keys at rest) ----

SEAL_VERSION = 1
SEAL_NONCE_SIZE = 12
SEAL_OVERHEAD = 1 + SEAL_NONCE_SIZE + TAG_SIZE  # 29 B more than the plain value
SEAL_NAME_MAX = 32
_SEAL_AAD = b"sensei-e2e/"


def _seal_aad(name: str) -> bytes:
    raw = name.encode()
    if not raw or len(raw) > SEAL_NAME_MAX:
        raise SecureError("bad setting name")
    return _SEAL_AAD + raw + bytes([SEAL_VERSION])


def seal_record(key: bytes, name: str, plaintext: bytes, nonce: bytes | None = None) -> bytes:
    """A sealed settings record as the node stores it: u8 version (1) | nonce[12] | AES-128-GCM ciphertext | tag[16].
    Associated data: "sensei-e2e/" || setting name || version, so a record does not open under another name. `nonce`
    is for test vectors; leave it out to take 12 random bytes (a nonce must never repeat under one key)."""
    if len(key) != 16:
        raise SecureError("the sealing key is 16 bytes")
    nonce = secrets.token_bytes(SEAL_NONCE_SIZE) if nonce is None else nonce
    if len(nonce) != SEAL_NONCE_SIZE:
        raise SecureError("the nonce is 12 bytes")
    return bytes([SEAL_VERSION]) + nonce + AESGCM(key).encrypt(nonce, plaintext, _seal_aad(name))


def open_record(key: bytes, name: str, record: bytes) -> bytes:
    """Open a sealed settings record of the node (see `seal_record`). Raises SecureError for a short record, an unknown
    version, a wrong key, a wrong name or a changed record."""
    if len(key) != 16:
        raise SecureError("the sealing key is 16 bytes")
    if len(record) < SEAL_OVERHEAD or record[0] != SEAL_VERSION:
        raise SecureError("not a sealed record of this version")
    try:
        return AESGCM(key).decrypt(record[1:1 + SEAL_NONCE_SIZE], record[1 + SEAL_NONCE_SIZE:], _seal_aad(name))
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


@dataclass
class _Node:
    node_pub: bytes
    keys: SessionKeys
    up: ReplayWindow
    down_ctr: int = 0  # files of version 1 only, in retired entries: folded into KeyStore._down when the key returns


def _node_text(node_id: int) -> str:
    return f"{node_id:016x}"


def _node_to_json(n: _Node) -> dict:
    return {
        "node_pub": n.node_pub.hex(),
        "key_id": n.keys.key_id,
        "k_up": n.keys.k_up.hex(),
        "k_down": n.keys.k_down.hex(),
        "up_highest": n.up.highest,
        "up_window": f"{n.up.bitmap:016x}",
        "down_ctr": n.down_ctr,  # 0 except in retired entries migrated from version 1
    }


def _node_from_json(n: dict) -> _Node:
    keys = SessionKeys(bytes.fromhex(n["k_up"]), bytes.fromhex(n["k_down"]), int(n["key_id"]))
    return _Node(
        bytes.fromhex(n["node_pub"]),
        keys,
        ReplayWindow(int(n["up_highest"]), int(n["up_window"], 16)),
        int(n.get("down_ctr", 0)),
    )


@dataclass
class _File:
    pub: bytes
    nodes: dict[int, _Node]
    retired: dict[int, _Node]
    senders: dict[str, int]  # sender id by host fingerprint
    down: dict[int, int]  # last downlink counter by node id


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
    """

    def __init__(
        self,
        path: str | os.PathLike | None = None,
        private_pem: str | os.PathLike | None = None,
        autosave_s: float = 5.0,
        key_file: str | os.PathLike | None = None,
        require_sealed: bool | None = None,
    ) -> None:
        self.path = Path(path) if path else None
        key_file = key_file or os.environ.get(_ENV_KEY_FILE) or None
        if require_sealed is None:
            require_sealed = os.environ.get(_ENV_REQUIRE_SEALED, "").strip() == "1"
        self._key: bytes | None = load_key_file(key_file) if key_file else None
        self._require_sealed = require_sealed
        if require_sealed and self._key is None:
            raise SecureError(f"{_ENV_REQUIRE_SEALED}=1 needs a key file ({_ENV_KEY_FILE})")
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
        self._retired: dict[int, _Node] = {}
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
            store._nodes[node_id] = node
            store._by_key_id[node.keys.key_id] = node_id
        store._retired = f.retired
        store._down = f.down
        store._senders = f.senders
        if store._host in f.senders:
            store._sender = f.senders[store._host]
        elif f.senders:  # a copy, or another process: the sender must not be shared
            store.sender_changed = True
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
            version = data.get("version")
            if version not in (1, 2):
                raise SecureError(f"unsupported key store version {version}")
            nodes = {int(text, 16): _node_from_json(n) for text, n in data["nodes"].items()}
            retired = {int(text, 16): _node_from_json(n) for text, n in data.get("retired", {}).items()}
            down = {int(text, 16): int(n) for text, n in data.get("down_ctr", {}).items()}
            for node_id, node in nodes.items():  # version 1: the counter was in the node
                down[node_id] = max(down.get(node_id, 0), node.down_ctr)
                node.down_ctr = 0
            senders = {str(h): int(text, 16) for h, text in data.get("senders", {}).items()}
            if "sender" in data and not senders:  # one sender: the fingerprint of its host, if the file has one
                senders[data.get("host", self._host)] = int(data["sender"], 16)
            return _File(bytes.fromhex(data["backend_public_key"]), nodes, retired, senders, down)
        except (KeyError, ValueError, TypeError, OSError) as exc:
            raise SecureError(f"key store {self.path} is damaged: {exc}") from exc

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
            if mine is None or (mine.keys != theirs.keys and node_id not in self._added):
                if mine is not None:
                    del self._by_key_id[mine.keys.key_id]
                owner = self._by_key_id.get(theirs.keys.key_id)
                if owner is None or owner == node_id:
                    self._nodes[node_id] = theirs
                    self._by_key_id[theirs.keys.key_id] = node_id
            elif mine.keys == theirs.keys:
                mine.up = _merge_window(mine.up, theirs.up)
        for key_id, theirs in f.retired.items():
            mine = self._retired.get(key_id)
            if mine is None:
                self._retired[key_id] = theirs
            else:
                mine.down_ctr = max(mine.down_ctr, theirs.down_ctr)
                mine.up = _merge_window(mine.up, theirs.up)

    def save(self) -> None:
        """Write the file. Under a file lock, the file is read first and merged, so that a node another process
        added or a counter it advanced is not lost; the file is synced before it replaces the old one."""
        if self.path is None:
            return
        with self._lock, self._file_lock():
            self._merge_from_disk()
            _ = self.sender  # makes it if this store has none yet
            data = {
                "version": 2,
                "senders": {
                    h: f"{s:08x}" for h, s in sorted(self._senders.items())
                },
                "backend_public_key": self.backend_public_key().hex(),
                "nodes": {
                    _node_text(i): _node_to_json(n) for i, n in sorted(self._nodes.items())
                },
                "retired": {
                    f"{k:08x}": _node_to_json(n) for k, n in sorted(self._retired.items())
                },
                "down_ctr": {
                    _node_text(i): n for i, n in sorted(self._down.items())
                },
            }
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
            self._sender = secrets.randbelow(2**32 - 1) + 1
        self._senders[self._host] = self._sender
        return self._sender

    def new_sender(self) -> int:
        """Take a new sender id and save it. For a copy of a key store file that another host uses: without it both
        hosts seal with the same sender and could use one counter twice."""
        with self._lock, self._file_lock():
            self._merge_from_disk()
            self._sender = secrets.randbelow(2**32 - 1) + 1
            self._senders[self._host] = self._sender
            self._new_sender = True  # the save must not take the old one back from the file
            try:
                self._touch(now = True)
            finally:
                self._new_sender = False
            return self._sender

    def down_counter(self, node_id: int) -> int:
        """Last downlink counter used for a node (0 if none)."""
        return self._down.get(node_id, 0)

    def backend_public_key(self) -> bytes:
        if self._pub is None:
            raise SecureError("no backend key: run backend_keygen and load the store with its PEM")
        return self._pub

    def add_node(self, node_id: int, node_pub: bytes) -> None:
        """Derive the keys of a node from its public key. A node that is added again with the same key
        keeps its counters; a new key starts new counters."""
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
            if old is not None and old.keys == keys:
                return
            if old is not None:
                self._retire(old)
                del self._by_key_id[old.keys.key_id]
            # the same key as before (removed and added again) goes on from its counters
            back = self._retired.pop(keys.key_id, None)
            node = _Node(node_pub, keys, ReplayWindow())
            if back is not None and back.keys == keys:
                node.up = back.up
                self._down[node_id] = max(self._down.get(node_id, 0), back.down_ctr)
            self._nodes[node_id] = node
            self._by_key_id[keys.key_id] = node_id
            self._added.add(node_id)
            self._removed.discard(node_id)
            self._touch(now = True)

    def remove_node(self, node_id: int) -> None:
        with self._lock, self._file_lock():
            self._merge_from_disk()
            node = self._nodes.pop(node_id, None)
            if node is not None:
                self._retire(node)
                del self._by_key_id[node.keys.key_id]
                self._added.discard(node_id)
                self._removed.add(node_id)
                self._touch(now = True)

    def _retire(self, node: _Node) -> None:
        """Keep the counters of a key that is dropped: if the key comes back it must not restart them."""
        self._retired[node.keys.key_id] = node

    def node_ids(self) -> list[int]:
        return sorted(self._nodes)

    def has_node(self, node_id: int) -> bool:
        return node_id in self._nodes

    def key_id(self, node_id: int) -> int:
        return self._nodes[node_id].keys.key_id


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


def open_sealed(payload: bytes, store: KeyStore) -> tuple[int, int, bytes]:
    """Open a sealed payload from a node: (node_id, kind, plaintext). Raises SecureError (UnknownKeyError,
    ReplayError are subclasses) if the key is unknown, the tag does not verify or the counter was seen."""
    h = parse_header(payload)
    if h.direction != DIR_UP:
        raise SecureError("not a node to backend envelope")
    with store._lock:
        node_id = store._by_key_id.get(h.key_id)
        if node_id is None:
            raise UnknownKeyError(f"unknown key id {h.key_id:08x}")
        if h.node_id is not None and h.node_id != node_id:
            raise SecureError("node id does not match the key")
        node = store._nodes[node_id]
        node.up.check(h.counter)
        plain = open_raw(node.keys.k_up, payload, h)
        node.up.update(h.counter)
        store._touch()
    return node_id, h.kind, plain


def peek_sealed(payload: bytes, store: KeyStore) -> tuple[int, bytes]:
    """Like open_sealed, but the replay window is not touched: a second reader of the same frames (the GUI
    recorder) may look inside without making the real reader see a replay. Returns (kind, plaintext)."""
    h = parse_header(payload)
    if h.direction != DIR_UP:
        raise SecureError("not a node to backend envelope")
    with store._lock:
        node_id = store._by_key_id.get(h.key_id)
        if node_id is None:
            raise UnknownKeyError(f"unknown key id {h.key_id:08x}")
        if h.node_id is not None and h.node_id != node_id:
            raise SecureError("node id does not match the key")
        key = store._nodes[node_id].keys.k_up
    return h.kind, open_raw(key, payload, h)


def seal_downlink(node_id: int, kind: int, plaintext: bytes, store: KeyStore) -> bytes:
    """Seal a Downlink (KIND_DOWNLINK) or a settings write (KIND_SETTINGS) for a node."""
    if kind not in (KIND_DOWNLINK, KIND_SETTINGS):
        raise SecureError(f"kind {kind} is not a downlink kind")
    with store._lock, store._file_lock():
        store._merge_from_disk()  # another process may have used counters
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
        out = seal_raw(node.keys.k_down, node.keys.key_id, counter, kind, plaintext, sender = store.sender)
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
