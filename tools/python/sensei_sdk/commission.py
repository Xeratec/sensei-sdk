# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Adding a node over BLE with a setup code: SPAKE2+ (RFC 9383), the secure channel, the label.

The commissioner (GUI, CLI) is the Prover and knows the code; the node is the Verifier and holds only w0 and L. The
protocol is in the onboarding design (sections 1 and 2). This module is the host side; the C mirror comes with the
node. The message bodies are plain bytes here: the protobuf layer belongs to the app.

    bench:  code = generate_code(); v = make_verifier(code, node_id)      # v.encode() goes to the node
    host:   begin: nonce_g = new_nonce()  ->  node answers with params (salt, kdf) and nonce_n
            p = Prover.from_code(code, node_id, params, nonce_g, nonce_n)
            share_p = p.share()                           # node: s = v.session(...); share_v, confirm_v = s.pake1(share_p)
            confirm_p = p.finish(share_v, confirm_v)      # wrong code: WrongCodeError; node: s.pake3(confirm_p)
            ch = Channel(p.keys, PROVER)                  # node: Channel(s.keys, VERIFIER); kind 5 envelopes both ways
            fingerprint(node_id, node_pub) == QR fingerprint; make_proof / check_proof after the key step

Suite: P256-SHA256-HKDF-SHA256-HMAC-SHA256 with the final RFC 9383 key schedule, identities that are never empty and a
context that binds the scrypt parameters, the salt and both nonces. The P-256 arithmetic is `p256` (not constant time:
host only). Python cannot wipe memory: the sessions drop their secrets, nothing more.
"""

from __future__ import annotations

import hashlib
import hmac
import itertools
import re
import secrets
import struct
from collections.abc import Callable
from dataclasses import dataclass, field

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

from . import p256, secure
from .errors import SecureError
from .secure import KIND_COMMISSION, KIND_NODE_TO_HOST

# ---- parameters (design section 1.2) ----

SUITE = 1  # P256-SHA256-HKDF-SHA256-HMAC-SHA256
KDF_SCRYPT = 1
LOG2N = 15  # scrypt N = 2^15 (32 MiB), r = 8, p = 1: the RFC 9383 recommendation
SCRYPT_R = 8
SCRYPT_P = 1
LOG2N_MIN, LOG2N_MAX = 14, 17  # what a commissioner accepts from a node
SALT_SIZE = 16
NONCE_SIZE = 16
DK_LEN = 80  # w0s || w1s, 2 * (256 + 64) bits
ID_PROVER = b"SENSEI-commissioner"
ID_VERIFIER_PREFIX = b"SENSEI-node"
CONTEXT_LABEL = b"SENSEI-COMMISSION-v1"
CONTEXT_SIZE = 73
CHANNEL_INFO = b"channel"
POP_LABEL = b"SENSEI-COMMISSION-pop-v2"
SESSION_ID_SIZE = 16  # of the PAKE run
FP_NODE_LABEL = b"SENSEI-NODE-FP-v1"
FP_COLLECTOR_LABEL = b"SENSEI-COLLECTOR-FP-v1"
NODE_FP_SIZE = 16
COLLECTOR_FP_SIZE = 8
MAX_ENVELOPE = 320  # largest kind 5 envelope
RECORD_VERSION = 1
RECORD_SIZE = 121  # ver, suite, kdf, log2n, r, p, u16 0, salt[16], w0[32], L[65]
CODE_DIGITS = 11  # random digits; the twelfth is the Verhoeff check digit
CODE_SIZE = CODE_DIGITS + 1
QR_PREFIX = "SN1"

# M and N of RFC 9383 section 4 (P-256), compressed as published, kept as 65 B points.
M = p256.encode(p256.decompress(bytes.fromhex("02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f")))
N = p256.encode(p256.decompress(bytes.fromhex("03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49")))
_M_PT, _N_PT = p256.decode(M), p256.decode(N)

PROVER, VERIFIER = "prover", "verifier"  # the two sides of a `Channel`


class CommissionError(SecureError):
    """Base of the errors of a commissioning step."""


class InvalidPointError(CommissionError):
    """A share is not a valid point of the group, or it is the identity after removing w0*M or w0*N."""


class WrongCodeError(CommissionError):
    """The key confirmation failed: the other side does not know the same code (or the message was changed)."""


class StateError(CommissionError):
    """A step out of order, repeated, or after the session ended."""


class ParamsError(CommissionError):
    """Parameters that this commissioner does not accept (suite, kdf, scrypt cost, salt or nonce length)."""


class CodeError(CommissionError):
    """A setup code or QR payload that does not parse, or whose check digit is wrong."""


class ProofError(CommissionError):
    """The proof of possession of the node key does not verify."""


# ---- setup code: 11 digits and a Verhoeff check digit ----

_VD = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9],
    [1, 2, 3, 4, 0, 6, 7, 8, 9, 5],
    [2, 3, 4, 0, 1, 7, 8, 9, 5, 6],
    [3, 4, 0, 1, 2, 8, 9, 5, 6, 7],
    [4, 0, 1, 2, 3, 9, 5, 6, 7, 8],
    [5, 9, 8, 7, 6, 0, 4, 3, 2, 1],
    [6, 5, 9, 8, 7, 1, 0, 4, 3, 2],
    [7, 6, 5, 9, 8, 2, 1, 0, 4, 3],
    [8, 7, 6, 5, 9, 3, 2, 1, 0, 4],
    [9, 8, 7, 6, 5, 4, 3, 2, 1, 0],
]
_VP = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9],
    [1, 5, 7, 6, 2, 8, 3, 0, 9, 4],
    [5, 8, 0, 3, 7, 9, 6, 1, 4, 2],
    [8, 9, 1, 6, 0, 4, 3, 5, 2, 7],
    [9, 4, 5, 3, 1, 2, 6, 8, 7, 0],
    [4, 2, 8, 6, 5, 7, 3, 9, 0, 1],
    [2, 7, 9, 3, 8, 0, 6, 4, 1, 5],
    [7, 0, 4, 6, 9, 1, 3, 2, 5, 8],
]
_VINV = [0, 4, 3, 2, 1, 5, 6, 7, 8, 9]


def verhoeff_digit(digits: str) -> int:
    """Check digit to append to a string of decimal digits."""
    c = 0
    for i, ch in enumerate(reversed(digits)):
        c = _VD[c][_VP[(i + 1) % 8][int(ch)]]
    return _VINV[c]


def verhoeff_valid(digits: str) -> bool:
    """True if the last digit is the check digit of the others. Catches every single digit error and every swap of
    two neighbours."""
    c = 0
    for i, ch in enumerate(reversed(digits)):
        c = _VD[c][_VP[i % 8][int(ch)]]
    return c == 0


def _weak(digits: str) -> bool:
    """All digits equal, or an ascending or descending run (the step is the same everywhere: 0, +1 or -1 mod 10)."""
    steps = {(int(b) - int(a)) % 10 for a, b in itertools.pairwise(digits)}
    return len(steps) == 1 and steps <= {0, 1, 9}


def generate_code(randbelow: Callable[[int], int] = secrets.randbelow) -> str:
    """A new setup code: 11 random digits and the check digit, 12 digits in a string (no dashes). Draws again for
    all the same digit and for ascending or descending runs. `randbelow` is for tests."""
    while True:
        digits = f"{randbelow(10**CODE_DIGITS):0{CODE_DIGITS}d}"
        if not _weak(digits):
            return digits + str(verhoeff_digit(digits))


def format_code(code: str) -> str:
    """1234-5678-9012"""
    return "-".join(code[i:i + 4] for i in range(0, CODE_SIZE, 4))


def parse_code(text: str) -> str:
    """The 12 digits of a code typed or scanned, with dashes or spaces allowed. Checks the length and the check digit."""
    if not isinstance(text, str):
        raise CodeError("a setup code is text")
    code = re.sub(r"[-\s]", "", text)
    if not re.fullmatch(rf"[0-9]{{{CODE_SIZE}}}", code):
        raise CodeError(f"a setup code has {CODE_SIZE} digits")
    if not verhoeff_valid(code):
        raise CodeError("the last digit does not fit: a digit is wrong or two digits are swapped")
    return code


def password(code: str) -> bytes:
    """The password of the PAKE: the 11 digits as ASCII, without the check digit."""
    return parse_code(code)[:CODE_DIGITS].encode()


# ---- QR payload and label ----


@dataclass(frozen = True)
class QrData:
    node_id: int
    code: str  # 12 digits
    fp: bytes  # 16 B, `node_fingerprint`


_QR_RE = re.compile(rf"{QR_PREFIX}:([0-9A-F]{{16}}):([0-9]{{{CODE_SIZE}}}):([0-9A-F]{{{2 * NODE_FP_SIZE}}})")


def qr_payload(node_id: int, code: str, fp: bytes) -> str:
    """`SN1:<node id 16 hex>:<code 12 digits>:<fingerprint 32 hex>`, upper case, 66 characters (QR alphanumeric)."""
    parse_code(code)
    if not isinstance(node_id, int) or not 0 < node_id < 2**64:
        raise CodeError("the node id is 1 ... 2^64-1 (0 means any node in the protocol)")
    if not isinstance(fp, bytes) or len(fp) != NODE_FP_SIZE:
        raise CodeError("the fingerprint is 16 bytes")
    return f"{QR_PREFIX}:{node_id:016X}:{code}:{fp.hex().upper()}"


def parse_qr(text: str) -> QrData:
    """Inverse of `qr_payload`. Strict: exactly this format, upper case hex, valid check digit (surrounding blanks
    are ignored)."""
    if not isinstance(text, str):
        raise CodeError("a QR payload is text")
    m = _QR_RE.fullmatch(text.strip())
    if not m:
        raise CodeError("not a SENSEI setup QR code")
    node_id = int(m.group(1), 16)
    if node_id == 0:  # 0 means "any node" in commission_begin: a label never carries it
        raise CodeError("node id 0 is not a node")
    return QrData(node_id, parse_code(m.group(2)), bytes.fromhex(m.group(3)))


def label_lines(node_id: int, code: str, fp: bytes) -> tuple[str, str, str]:
    """The text of the label: `SENSEI 00A1B2C3 D4E5F607`, `Setup code 1234-5678-9012`, `Key a1b2 c3d4 e5f6 0718`.
    The first and the last line go on the outside (node id and fingerprint); the code stays on the card.
    These texts are not authenticated by anything but the label itself. A change of the format or of what the
    fingerprint covers must change the version (QR prefix `SN1`, labels `-v1`); an old and a new commissioner must never
    read the same string in two ways."""
    nid = f"{node_id:016X}"
    return f"SENSEI {nid[:8]} {nid[8:]}", f"Setup code {format_code(code)}", f"Key {fingerprint_text(fp)}"


# ---- fingerprints and proof of possession ----


def node_fingerprint(node_id: int, node_pub: bytes) -> bytes:
    """SHA-256("SENSEI-NODE-FP-v1" || node id (8 BE) || node public key (65))[0:16]."""
    if len(node_pub) != secure.PUB_SIZE:
        raise CommissionError("the node public key is 65 bytes")
    return hashlib.sha256(FP_NODE_LABEL + node_id.to_bytes(8, "big") + node_pub).digest()[:NODE_FP_SIZE]


def fingerprint_text(fp: bytes) -> str:
    """The first 8 bytes in groups of 2: `a1b2 c3d4 e5f6 0718`."""
    h = fp[:8].hex()
    return " ".join(h[i:i + 4] for i in range(0, 16, 4))


def collector_fingerprint(backend_pub: bytes) -> bytes:
    """SHA-256("SENSEI-COLLECTOR-FP-v1" || collector public key (65))[0:8]: tells whether a node is already ours."""
    return hashlib.sha256(FP_COLLECTOR_LABEL + backend_pub).digest()[:COLLECTOR_FP_SIZE]


def proof_plain(node_id: int, nonce_g: bytes, nonce_n: bytes, session_id: bytes) -> bytes:
    """"SENSEI-COMMISSION-pop-v2" || node id (8 BE) || nonce_G || nonce_N || session_id (16 B).

    `session_id` names the PAKE run (`Schedule.session_id`, `Prover.session_id`, `VerifierSession.session_id`). A relay
    that holds the code and runs one PAKE with the node and another with the commissioner (the same nonces passed on)
    has two session ids, so the proof of the node does not pass at the commissioner."""
    if len(nonce_g) != NONCE_SIZE or len(nonce_n) != NONCE_SIZE:
        raise CommissionError("a nonce is 16 bytes")
    if len(session_id) != SESSION_ID_SIZE:
        raise CommissionError("a session id is 16 bytes")
    return POP_LABEL + node_id.to_bytes(8, "big") + nonce_g + nonce_n + session_id


def make_proof(keys: secure.SessionKeys, node_id: int, nonce_g: bytes, nonce_n: bytes, session_id: bytes,
               counter: int) -> bytes:
    """The node's proof that it holds the key it just sent: a kind 3 envelope under k_up with the node's next counter.
    A fake node that replays the public key of the real one cannot make it."""
    return secure.seal_raw(keys.k_up, keys.key_id, counter, KIND_NODE_TO_HOST,
                           proof_plain(node_id, nonce_g, nonce_n, session_id))


def check_proof(proof: bytes,
                store: secure.KeyStore,
                node_id: int,
                nonce_g: bytes,
                nonce_n: bytes,
                session_id: bytes,
                commit: bool = True) -> None:
    """The commissioner's check, with the key store entry that `add_node` made. The key id must be the store's, the
    envelope must open with the store's keys (in any generation: the node may have mixed already) and carry exactly this
    session's plaintext, which includes the id of this PAKE run. `commit` marks the counter
    as seen, as for any uplink (a replayed proof fails the second time). Raises ProofError.

    What it proves: whoever sent it holds the private key behind the node public key that `add_node` used, and saw
    this session's nonces and PAKE run (a relay between two runs cannot pass it on). The node id is not part of the key derivation (HKDF info = node_pub || backend_pub): that the
    key belongs to this node id comes from the fingerprint, which the commissioner must have compared with the QR
    (or had confirmed by the user) before `add_node`."""
    try:
        if not store.has_node(node_id):
            raise ProofError("the key store does not know this node")
        want = store.key_id(node_id)
        if not secure.is_sealed(proof) or secure.parse_header(proof).key_id != want:
            raise ProofError("the key id of the proof is not the one of the new key")
        if commit:
            got_id, kind, plain = secure.open_sealed(proof, store)
        else:
            opened = secure._open_up(proof, store,
                                     False)  # the node id of the key that opened it (peek_sealed hides it)
            got_id, kind, plain = opened.node_id, opened.kind, opened.plaintext
    except ProofError:
        raise
    except SecureError as exc:
        raise ProofError(f"proof of possession failed: {exc}") from exc
    if got_id != node_id or kind != KIND_NODE_TO_HOST or not hmac.compare_digest(
            plain, proof_plain(node_id, nonce_g, nonce_n, session_id)):
        raise ProofError("proof of possession does not match this session")


def check_key_proof(proof: bytes,
                    store: secure.KeyStore,
                    node_id: int,
                    nonce_g: bytes,
                    nonce_n: bytes,
                    session_id: bytes,
                    commit: bool = True) -> secure.NodeBackup | None:
    """The check of the proof of a commissioning key step, for a node that `store.add_node_for_commissioning` added or
    left as it was. If the proof says that the node started again at generation 0 while the store is ahead, the store is
    reset first (`KeyStore.reset_if_node_restarted`); then the proof is checked against the state as it is, in any
    generation. Returns the backup of that reset (None if there was none), which the caller rolls back with the entry of
    `add_node_for_commissioning` if the key step fails after all. If the proof does not pass, the reset made here is
    rolled back and ProofError is raised."""
    reset = None
    try:
        reset = store.reset_if_node_restarted(node_id, proof)
        check_proof(proof, store, node_id, nonce_g, nonce_n, session_id, commit)
    except BaseException as exc:
        if reset is not None:
            try:
                store.rollback_node(reset)
            except SecureError:
                pass
        if isinstance(exc, SecureError) and not isinstance(exc, ProofError):
            raise ProofError(f"proof of possession failed: {exc}") from exc
        raise
    return reset


# ---- registration (scrypt) ----


@dataclass(frozen = True)
class Params:
    """What the node sends in CommissionParams (besides its id and nonce): suite, scrypt and the salt."""
    salt: bytes
    suite: int = SUITE
    kdf: int = KDF_SCRYPT
    log2n: int = LOG2N
    r: int = SCRYPT_R
    p: int = SCRYPT_P

    def check(self) -> None:
        """What a commissioner accepts: suite 1, kdf 1 (scrypt), log2n 14 to 17, r 8, p 1, a 16 B salt."""
        if self.suite != SUITE or self.kdf != KDF_SCRYPT:
            raise ParamsError("unknown suite or kdf")
        if not LOG2N_MIN <= self.log2n <= LOG2N_MAX or self.r != SCRYPT_R or self.p != SCRYPT_P:
            raise ParamsError("scrypt cost out of range")
        if len(self.salt) != SALT_SIZE:
            raise ParamsError("the salt is 16 bytes")


def id_verifier(node_id: int) -> bytes:
    """"SENSEI-node" || node id (8 BE): 19 bytes."""
    return ID_VERIFIER_PREFIX + node_id.to_bytes(8, "big")


def _le8(data: bytes) -> bytes:
    return struct.pack("<Q", len(data)) + data


def pwin(pw: bytes, node_id: int) -> bytes:
    """RFC 9383 section 3.2: len(pw) || pw || len(idProver) || idProver || len(idVerifier) || idVerifier (8 B LE)."""
    return _le8(pw) + _le8(ID_PROVER) + _le8(id_verifier(node_id))


def scrypt_output(code: str, node_id: int, params: Params) -> bytes:
    """w0s || w1s: 80 bytes of scrypt over `pwin` with the salt. 32 MiB and about 0.1 s at the default cost."""
    params.check()
    n = 1 << params.log2n
    return hashlib.scrypt(pwin(password(code), node_id),
                          salt = params.salt,
                          n = n,
                          r = params.r,
                          p = params.p,
                          maxmem = max(64 << 20, 160 * params.r * n),
                          dklen = DK_LEN)


def derive_w(code: str, node_id: int, params: Params) -> tuple[int, int]:
    """(w0, w1) of RFC 9383: the halves of the scrypt output reduced mod n. Zero is refused (new salt at the bench)."""
    out = scrypt_output(code, node_id, params)
    w0, w1 = (int.from_bytes(out[i:i + 40], "big") % p256.N for i in (0, 40))
    if w0 == 0 or w1 == 0:
        raise CommissionError("w0 or w1 is zero: use another salt")
    return w0, w1


@dataclass(frozen = True)
class Verifier:
    """What the node stores for one code: scrypt parameters and salt, w0 and L = w1*G. The code is not in it."""
    params: Params
    w0: bytes = field(repr = False)  # 32 B (not in a log line or a traceback)
    l: bytes  # 65 B

    def __post_init__(self) -> None:
        self.params.check()
        if len(self.w0) != 32 or not 0 < int.from_bytes(self.w0, "big") < p256.N:
            raise CommissionError("w0 must be a scalar in 1 ... n-1")
        try:
            p256.decode(self.l)
        except p256.PointError as exc:
            raise CommissionError(f"L is not a valid point: {exc}") from exc

    def record(self) -> bytes:
        """w0 || L, 97 bytes: what PSA_ALG_SPAKE2P imports as a verifier."""
        return self.w0 + self.l

    def encode(self) -> bytes:
        """The 121 B record of the node: ver, suite, kdf, log2n, r, p, u16 0, salt, w0, L."""
        q = self.params
        out = (bytes([RECORD_VERSION, q.suite, q.kdf, q.log2n, q.r, q.p]) + b"\0\0" + q.salt + self.record())
        assert len(out) == RECORD_SIZE
        return out

    @classmethod
    def decode(cls, data: bytes) -> Verifier:
        if len(data) != RECORD_SIZE or data[0] != RECORD_VERSION or data[6:8] != b"\0\0":
            raise CommissionError("not a verifier record")
        suite, kdf, log2n, r, p = data[1:6]
        return cls(Params(data[8:24], suite, kdf, log2n, r, p), data[24:56], data[56:])

    def session(self,
                node_id: int,
                nonce_g: bytes,
                nonce_n: bytes,
                y: bytes | None = None,
                rng: Callable[[int], bytes] = secrets.token_bytes) -> VerifierSession:
        """The node side of one commissioning session (Verifier of RFC 9383)."""
        return VerifierSession(self.w0, self.l, context(self.params, nonce_g, nonce_n), ID_PROVER, id_verifier(node_id),
                               node_id, nonce_g, nonce_n, y, rng)


def make_verifier(code: str,
                  node_id: int,
                  salt: bytes | None = None,
                  log2n: int = LOG2N,
                  rng: Callable[[int], bytes] = secrets.token_bytes) -> Verifier:
    """Registration at the bench: scrypt over the code, w0 and L = w1*G. With no `salt`, a fresh 16 B salt (drawn
    again in the 2^-250 case that w0 or w1 is zero)."""
    while True:
        params = Params(salt if salt is not None else rng(SALT_SIZE), log2n = log2n)
        try:
            w0, w1 = derive_w(code, node_id, params)
        except CommissionError:
            if salt is not None:
                raise
            continue
        return Verifier(params, w0.to_bytes(32, "big"), p256.encode(p256.mul(w1, p256.G)))


# ---- context, transcript, key schedule ----


def context(params: Params, nonce_g: bytes, nonce_n: bytes) -> bytes:
    """"SENSEI-COMMISSION-v1" || suite || kdf || log2n || r || p || salt || nonce_G || nonce_N (73 B)."""
    if len(nonce_g) != NONCE_SIZE or len(nonce_n) != NONCE_SIZE:
        raise ParamsError("a nonce is 16 bytes")
    ctx = (CONTEXT_LABEL + bytes([params.suite, params.kdf, params.log2n, params.r, params.p]) + params.salt + nonce_g +
           nonce_n)
    if len(ctx) != CONTEXT_SIZE:
        raise ParamsError("the salt is 16 bytes")
    return ctx


def transcript(ctx: bytes, id_prover: bytes, id_verifier_: bytes, share_p: bytes, share_v: bytes, z: bytes, v: bytes,
               w0: bytes) -> bytes:
    """TT of RFC 9383 section 3.3: every item with an 8 byte little endian length in front."""
    return b"".join(_le8(x) for x in (ctx, id_prover, id_verifier_, M, N, share_p, share_v, z, v, w0))


@dataclass(frozen = True)
class Schedule:
    k_main: bytes = field(repr = False)
    k_confirm_p: bytes = field(repr = False)
    k_confirm_v: bytes = field(repr = False)
    k_shared: bytes = field(repr = False)
    confirm_p: bytes  # HMAC(K_confirmP, shareV): sent by the prover
    confirm_v: bytes  # HMAC(K_confirmV, shareP): sent by the verifier
    session_id: bytes  # HKDF(nil, K_main, "SessionId", 16): names this PAKE run (not K_main itself)


def _hkdf(ikm: bytes, length: int, salt: bytes | None, info: bytes) -> bytes:
    return HKDF(hashes.SHA256(), length, salt, info).derive(ikm)


def key_schedule(tt: bytes, share_p: bytes, share_v: bytes) -> Schedule:
    """RFC 9383 section 3.4 with SHA-256, HKDF-SHA256 (salt nil) and HMAC-SHA256."""
    k_main = hashlib.sha256(tt).digest()
    conf = _hkdf(k_main, 64, None, b"ConfirmationKeys")
    k_shared = _hkdf(k_main, 32, None, b"SharedKey")
    session_id = _hkdf(k_main, SESSION_ID_SIZE, None, b"SessionId")
    return Schedule(k_main, conf[:32], conf[32:], k_shared, hmac.digest(conf[:32], share_v, "sha256"),
                    hmac.digest(conf[32:], share_p, "sha256"), session_id)


@dataclass(frozen = True)
class ChannelKeys:
    k_c2n: bytes = field(repr = False)  # commissioner to node, 16 B
    k_n2c: bytes = field(repr = False)  # node to commissioner, 16 B
    sid: int  # session id: the key id field of the envelopes


def channel_keys(k_shared: bytes, node_id: int, nonce_g: bytes, nonce_n: bytes) -> ChannelKeys:
    """HKDF(ikm = K_shared, salt = "SENSEI-COMMISSION-v1", info = "channel" || node id (8 BE) || nonce_G || nonce_N,
    L = 36): k_c2n = okm[0:16], k_n2c = okm[16:32], sid = u32 LE okm[32:36] (0 aborts)."""
    okm = _hkdf(k_shared, 36, CONTEXT_LABEL, CHANNEL_INFO + node_id.to_bytes(8, "big") + nonce_g + nonce_n)
    sid = struct.unpack("<I", okm[32:])[0]
    if sid == 0:
        raise CommissionError("session id 0")
    return ChannelKeys(okm[:16], okm[16:32], sid)


def new_nonce(rng: Callable[[int], bytes] = secrets.token_bytes) -> bytes:
    return rng(NONCE_SIZE)


def _random_scalar(rng: Callable[[int], bytes]) -> int:
    """40 random bytes mod n (bias below 2^-64); never 0."""
    while True:
        k = int.from_bytes(rng(40), "big") % p256.N
        if k:
            return k


def _ephemeral(value: bytes | None, rng: Callable[[int], bytes]) -> int:
    if value is None:
        return _random_scalar(rng)
    k = int.from_bytes(value, "big") % p256.N
    if not k:
        raise CommissionError("the ephemeral scalar is zero")
    return k


def _peer_point(share: bytes, w: int, base: tuple[int, int]) -> tuple[int, int]:
    """Check a share (valid point, not the identity after removing w*base) and return share - w*base."""
    try:
        pt = p256.decode(share)
    except p256.PointError as exc:
        raise InvalidPointError(str(exc)) from exc
    rest = p256.add(pt, p256.neg(p256.mul(w, base)))
    if rest is None:
        raise InvalidPointError("the share minus w*M (or w*N) is the identity")
    return rest


# ---- the two sides ----


class Prover:
    """The commissioner. Steps: `share()`, then `finish(share_v, confirm_v)`; `keys` after that. A step out of order
    or after a failure raises StateError. One object runs one session; it does not retry."""

    def __init__(self,
                 w0: int,
                 w1: int,
                 ctx: bytes,
                 id_prover: bytes,
                 id_verifier_: bytes,
                 node_id: int,
                 nonce_g: bytes,
                 nonce_n: bytes,
                 x: bytes | None = None,
                 rng: Callable[[int], bytes] = secrets.token_bytes) -> None:
        """The general form, with w0, w1, context and identities given (the RFC vectors use it). `x`: the ephemeral
        scalar, big endian bytes of any length reduced mod n; None draws 40 random bytes."""
        for w in (w0, w1):
            if not isinstance(w, int) or not 0 < w < p256.N:
                raise CommissionError("w0 and w1 must be scalars in 1 ... n-1")
        self._w0, self._w1, self._ctx = w0, w1, ctx
        self._ids = (id_prover, id_verifier_)
        self._node_id, self._ng, self._nn = node_id, nonce_g, nonce_n
        self._x = _ephemeral(x, rng)
        self._share: bytes | None = None
        self._state = "new"
        self._keys: ChannelKeys | None = None
        self._session_id = b""

    @classmethod
    def from_w(cls,
               w0: int,
               w1: int,
               node_id: int,
               params: Params,
               nonce_g: bytes,
               nonce_n: bytes,
               x: bytes | None = None,
               rng: Callable[[int], bytes] = secrets.token_bytes) -> Prover:
        """Like `from_code` from the two scalars that `derive_w(code, node_id, params)` gave. The commissioner derives them
        once (scrypt) and drops the code string; a second session on the same node (the parameters and the node id must
        be the same) starts from them. w0 and w1 give the same power over this node as the code does."""
        params.check()
        return cls(w0, w1, context(params, nonce_g, nonce_n), ID_PROVER, id_verifier(node_id), node_id, nonce_g,
                   nonce_n, x, rng)

    @classmethod
    def from_code(cls,
                  code: str,
                  node_id: int,
                  params: Params,
                  nonce_g: bytes,
                  nonce_n: bytes,
                  x: bytes | None = None,
                  rng: Callable[[int], bytes] = secrets.token_bytes) -> Prover:
        """The commissioner of a SENSEI node: runs scrypt over the code (this takes about 0.1 s), checks the parameters
        the node sent (`Params.check`). Raises CodeError for a code with a wrong check digit."""
        w0, w1 = derive_w(code, node_id, params)
        return cls(w0, w1, context(params, nonce_g, nonce_n), ID_PROVER, id_verifier(node_id), node_id, nonce_g,
                   nonce_n, x, rng)

    def share(self) -> bytes:
        """shareP = x*G + w0*M, 65 B (message Pake1)."""
        if self._state != "new":
            raise StateError("share() was called already")
        pt = p256.add(p256.mul(self._x, p256.G), p256.mul(self._w0, _M_PT))
        self._share = p256.encode(pt)
        self._state = "shared"
        return self._share

    def finish(self, share_v: bytes, confirm_v: bytes) -> bytes:
        """Check shareV and confirmV (Pake2), return confirmP (Pake3). WrongCodeError if the verifier does not know the
        same code; InvalidPointError for a bad share. Either ends the session."""
        if self._state != "shared":
            raise StateError("finish() needs share() first and runs once")
        self._state = "failed"  # until it succeeds
        try:
            rest = _peer_point(share_v, self._w0, _N_PT)
            z = p256.encode(p256.mul(self._x, rest))
            v = p256.encode(p256.mul(self._w1, rest))
            w0 = self._w0.to_bytes(32, "big")
            assert self._share is not None
            tt = transcript(self._ctx, *self._ids, self._share, share_v, z, v, w0)
            sch = key_schedule(tt, self._share, share_v)
            if len(confirm_v) != 32 or not hmac.compare_digest(confirm_v, sch.confirm_v):
                raise WrongCodeError("wrong setup code (key confirmation failed)")
            self._keys = channel_keys(sch.k_shared, self._node_id, self._ng, self._nn)
            self._session_id = sch.session_id
            self._state = "done"  # K_shared is not kept: the channel keys are all that follows from it
            return sch.confirm_p
        finally:
            self._x = self._w0 = self._w1 = 0

    @property
    def keys(self) -> ChannelKeys:
        if self._keys is None:
            raise StateError("the session is not complete")
        return self._keys

    @property
    def session_id(self) -> bytes:
        """The id of this PAKE run (16 B), after `finish`: it goes into the proof of possession."""
        if not self._session_id:
            raise StateError("the session is not complete")
        return self._session_id


class VerifierSession:
    """The node. Steps: `pake1(share_p)` returns (shareV, confirmV); `pake3(confirm_p)`; `keys` after that. Made by
    `Verifier.session`. An invalid share or a wrong confirmation ends the session (the node counts the second as a
    failed attempt, the first not)."""

    def __init__(self,
                 w0: bytes,
                 l: bytes,
                 ctx: bytes,
                 id_prover: bytes,
                 id_verifier_: bytes,
                 node_id: int,
                 nonce_g: bytes,
                 nonce_n: bytes,
                 y: bytes | None = None,
                 rng: Callable[[int], bytes] = secrets.token_bytes) -> None:
        if not isinstance(w0, bytes) or len(w0) != 32 or not 0 < int.from_bytes(w0, "big") < p256.N:
            raise CommissionError("w0 must be a 32 byte scalar in 1 ... n-1")
        try:
            p256.decode(l)
        except p256.PointError as exc:
            raise CommissionError(f"L is not a valid point: {exc}") from exc
        self._w0, self._l, self._ctx = w0, l, ctx
        self._ids = (id_prover, id_verifier_)
        self._node_id, self._ng, self._nn = node_id, nonce_g, nonce_n
        self._y = _ephemeral(y, rng)
        self._state = "new"
        self._sch: Schedule | None = None
        self._keys: ChannelKeys | None = None
        self._session_id = b""

    def pake1(self, share_p: bytes) -> tuple[bytes, bytes]:
        """Check shareP (a valid point, not w0*M), compute shareV = y*G + w0*N and confirmV."""
        if self._state != "new":
            raise StateError("pake1 runs once per session")
        self._state = "failed"
        w0 = int.from_bytes(self._w0, "big")
        rest = _peer_point(share_p, w0, _M_PT)
        share_v = p256.encode(p256.add(p256.mul(self._y, p256.G), p256.mul(w0, _N_PT)))
        z = p256.encode(p256.mul(self._y, rest))
        v = p256.encode(p256.mul(self._y, p256.decode(self._l)))
        tt = transcript(self._ctx, *self._ids, share_p, share_v, z, v, self._w0)
        self._sch = key_schedule(tt, share_p, share_v)
        self._session_id = self._sch.session_id
        self._y = 0
        self._state = "sent"
        return share_v, self._sch.confirm_v

    def pake3(self, confirm_p: bytes) -> None:
        """Check confirmP. WrongCodeError if it is wrong. After it, `keys` is available."""
        if self._state != "sent" or self._sch is None:
            raise StateError("pake3 needs pake1 first and runs once")
        sch, self._sch = self._sch, None
        self._state = "failed"
        if len(confirm_p) != 32 or not hmac.compare_digest(confirm_p, sch.confirm_p):
            raise WrongCodeError("wrong setup code (key confirmation failed)")
        self._keys = channel_keys(sch.k_shared, self._node_id, self._ng, self._nn)
        self._state = "done"

    @property
    def keys(self) -> ChannelKeys:
        if self._keys is None:
            raise StateError("the session is not complete")
        return self._keys

    @property
    def session_id(self) -> bytes:
        """The id of this PAKE run (16 B), from `pake1` on: it goes into the proof of possession the node seals."""
        if not self._session_id:
            raise StateError("the session is not complete")
        return self._session_id


# ---- the channel (kind 5 envelopes) ----


class Channel:
    """The commissioning channel after the PAKE: kind 5 envelopes on `secure.seal_raw` and `open_raw`, key id = sid,
    no node id, generation 0. The node sends with k_n2c (direction up, sender 0), the commissioner with k_c2n
    (direction down, sender 1). Counters start at 1 and live in RAM. The receiver keeps a 64 wide replay window, refuses
    another key id, direction, kind, ratchet bit or node id field, and anything over 320 bytes."""

    def __init__(self, keys: ChannelKeys, role: str) -> None:
        if role not in (PROVER, VERIFIER):
            raise ValueError("role is PROVER or VERIFIER")
        self.keys, self.role = keys, role
        self.tx_counter = 0  # last counter sent
        self.rx = secure.ReplayWindow()
        self._send_key, self._recv_key = ((keys.k_c2n, keys.k_n2c) if role == PROVER else (keys.k_n2c, keys.k_c2n))
        self._sender = secure.COMMISSION_SENDER if role == PROVER else 0
        self._recv_dir = secure.DIR_UP if role == PROVER else secure.DIR_DOWN

    def seal(self, plaintext: bytes, counter: int | None = None) -> bytes:
        """One envelope. `counter` is for tests; the default is the next one."""
        ctr = self.tx_counter + 1 if counter is None else counter
        env = secure.seal_raw(self._send_key, self.keys.sid, ctr, KIND_COMMISSION, plaintext, sender = self._sender)
        if len(env) > MAX_ENVELOPE:
            raise CommissionError(f"envelope of {len(env)} bytes: the limit is {MAX_ENVELOPE}")
        self.tx_counter = max(self.tx_counter, ctr)
        return env

    def open(self, envelope: bytes) -> bytes:
        """Authenticate, check the replay window, return the plaintext. SecureError or ReplayError otherwise."""
        if len(envelope) > MAX_ENVELOPE:
            raise CommissionError("envelope too long")
        h = secure.parse_header(envelope)
        if h.kind != KIND_COMMISSION or h.direction != self._recv_dir or h.key_id != self.keys.sid:
            raise CommissionError("not a commissioning envelope of this session")
        self.rx.check(h.counter)
        plain = secure.open_raw(self._recv_key, envelope, h)
        self.rx.update(h.counter)
        return plain
