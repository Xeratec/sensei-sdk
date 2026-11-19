# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Forward secrecy of the end-to-end envelope: root key, mix, uplink chain. Mirror of lib/sensei_secure/src/sensei_ratchet.c
(its README has the key schedule). The shared vectors (`python -m sensei_sdk.vectors`) check that both agree.

Generation 0 is the key pair derivation of `secure.derive_keys`. From it, `root0` gives the root key R_0. A mix
(X25519 between an ephemeral key of the node and one of the collector) takes R_g to R_{g+1}, a new head of the uplink
chain and a new downlink key. An uplink counter c belongs to the epoch `c >> 6`; its AES-128-GCM key is the message
key of that epoch, one HMAC step per epoch along the chain. All integers are u32 little endian, all labels ASCII.

This module has the primitives only. The state machine (key store v3, node session) is built on it.
"""

from __future__ import annotations

import hashlib
import hmac
import struct
from dataclasses import dataclass

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

from .secure import EpochError, GapError, SecureError

ROOT_LABEL = b"SENSEI-E2E-root-v1"
MIX_LABEL = b"SENSEI-RK-v1"
KEY_SIZE = 32  # X25519 keys, root key, chain key
MK_SIZE = 16  # AES-128-GCM message key
EPOCH_BITS = 6
EPOCH_SIZE = 1 << EPOCH_BITS  # counters per epoch = the replay window
MAX_SKIP = 1 << 20  # epochs a chain is walked in one step (SENSEI_E2E_MAX_SKIP)
_U32 = 0xFFFFFFFF


class DhError(SecureError):
    """The X25519 result is all zero (a low-order public key): the mix is refused."""


# ---- X25519 ----


def x25519_keygen() -> tuple[bytes, bytes]:
    """New ephemeral key: (32 B private, 32 B public)."""
    key = X25519PrivateKey.generate()
    return key.private_bytes_raw(), key.public_key().public_bytes_raw()


def x25519_public(priv: bytes) -> bytes:
    return X25519PrivateKey.from_private_bytes(priv).public_key().public_bytes_raw()


def x25519(priv: bytes, peer: bytes) -> bytes:
    """RFC 7748 X25519. Raises DhError if the result is all zero (a point of low order)."""
    if len(priv) != KEY_SIZE or len(peer) != KEY_SIZE:
        raise SecureError("an X25519 key is 32 bytes")
    try:
        out = X25519PrivateKey.from_private_bytes(priv).exchange(X25519PublicKey.from_public_bytes(peer))
    except ValueError as exc:  # the library refuses an all-zero result
        raise DhError("X25519 result is all zero (low-order public key)") from exc
    if not any(out):
        raise DhError("X25519 result is all zero (low-order public key)")
    return out


# ---- root key and mix ----


def root0(k_up: bytes, k_down: bytes, key_id: int) -> bytes:
    """R_0 = HKDF(ikm = k_up || k_down, salt = "SENSEI-E2E-root-v1", info = key_id, L = 32). Both sides hold k_up, k_down and
    key_id, so an existing key store needs no new secret."""
    return HKDF(hashes.SHA256(), KEY_SIZE, ROOT_LABEL, struct.pack("<I", key_id)).derive(k_up + k_down)


def transcript(key_id: int, gen: int, up_base: int, e_n_pub: bytes, e_c_pub: bytes) -> bytes:
    """th = "SENSEI-RK-v1" || key_id || gen(g+1) || up_base || e_N_pub || e_C_pub. gen is the new generation."""
    if len(e_n_pub) != KEY_SIZE or len(e_c_pub) != KEY_SIZE:
        raise SecureError("an X25519 key is 32 bytes")
    return MIX_LABEL + struct.pack("<III", key_id, gen & _U32, up_base & _U32) + e_n_pub + e_c_pub


@dataclass(frozen = True)
class Mix:
    """Result of a mix: the keys of generation g+1."""

    th: bytes  # the transcript that went into the KDF
    root: bytes  # R_{g+1}, salt of the next mix
    ck: bytes  # CKu_{g+1,0}, head of the uplink chain
    k_down: bytes  # k_down_{g+1}, 16 B, static downlink key of the generation


def mix_from_dh(root: bytes, dh: bytes, th: bytes) -> Mix:
    """okm = HKDF(ikm = dh, salt = R_g, info = th, L = 80): R_{g+1} = okm[0:32], CKu_{g+1,0} = okm[32:64],
    k_down_{g+1} = okm[64:80]. dh must not be all zero."""
    if len(root) != KEY_SIZE or len(dh) != KEY_SIZE:
        raise SecureError("root key and dh are 32 bytes")
    if not any(dh):
        raise DhError("dh is all zero")
    okm = HKDF(hashes.SHA256(), 80, root, th).derive(dh)
    return Mix(th, okm[:32], okm[32:64], okm[64:80])


def mix_node(root: bytes, key_id: int, gen: int, up_base: int, e_n_priv: bytes, e_c_pub: bytes) -> Mix:
    """The node's side of the mix to generation gen: its ephemeral private key and the collector's public key."""
    th = transcript(key_id, gen, up_base, x25519_public(e_n_priv), e_c_pub)
    return mix_from_dh(root, x25519(e_n_priv, e_c_pub), th)


def mix_collector(root: bytes, key_id: int, gen: int, up_base: int, e_c_priv: bytes, e_n_pub: bytes) -> Mix:
    """The collector's side of the mix: its ephemeral private key and the node's public key."""
    th = transcript(key_id, gen, up_base, e_n_pub, x25519_public(e_c_priv))
    return mix_from_dh(root, x25519(e_c_priv, e_n_pub), th)


# ---- uplink chain ----


def epoch(counter: int) -> int:
    """Epoch of a counter: 64 counters, the size of the replay window."""
    return counter >> EPOCH_BITS


def _hmac(ck: bytes, label: int) -> bytes:
    return hmac.new(ck, bytes([label]), hashlib.sha256).digest()


def message_key(ck: bytes) -> bytes:
    """MK_i = HMAC(CK_i, 0x01)[0:16], the AES-128-GCM key of epoch i."""
    return _hmac(ck, 1)[:MK_SIZE]


def next_chain_key(ck: bytes) -> bytes:
    """CK_{i+1} = HMAC(CK_i, 0x02)."""
    return _hmac(ck, 2)


class Chain:
    """One uplink chain: CK at index idx, where index 0 is the epoch up_base. Only moves forward."""

    def __init__(self, ck: bytes, up_base: int, idx: int = 0) -> None:
        if len(ck) != KEY_SIZE:
            raise SecureError("a chain key is 32 bytes")
        self.ck = ck
        self.up_base = up_base
        self.idx = idx

    def copy(self) -> Chain:
        return Chain(self.ck, self.up_base, self.idx)

    def index_of(self, counter: int) -> int:
        """Chain index of the epoch of a counter (negative: before the chain starts)."""
        return epoch(counter) - self.up_base

    def advance_to(self, idx: int, max_skip: int = MAX_SKIP) -> None:
        """Walk to index idx (CK of that epoch). A state that failed to walk is left as it was."""
        if idx < self.idx:
            raise EpochError(f"epoch index {idx} is behind the chain (at {self.idx})")
        if idx - self.idx > max_skip:
            raise GapError(f"epoch gap too large: {idx - self.idx} > {max_skip}")
        ck = self.ck
        for _ in range(idx - self.idx):
            ck = next_chain_key(ck)
        self.ck, self.idx = ck, idx

    def key_for(self, counter: int, max_skip: int = MAX_SKIP, commit: bool = False) -> bytes:
        """Message key of the epoch of a counter. The epoch must be at or after the position of the chain and not before
        its start. With commit the chain moves to that epoch (the keys of the epochs it passed are gone); without, it
        is left as it was."""
        idx = self.index_of(counter)
        if idx < 0:
            raise EpochError(f"epoch {epoch(counter)} is before the start of the chain ({self.up_base})")
        work = self if commit else self.copy()
        work.advance_to(idx, max_skip)
        return message_key(work.ck)
