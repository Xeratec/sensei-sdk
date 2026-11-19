# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Forward secrecy primitives (sensei_sdk.ratchet) and the generation bits of the envelope (sensei_sdk.secure).

The C library checks the same vectors (lib/sensei_secure/tests/test_ratchet.c); here the Python side checks them
against an independent HKDF, replays the scripted transcript with a small model of the two sides, and runs the
property test of the design: no (key, nonce) pair twice."""

from __future__ import annotations

import hashlib
import hmac
import struct

import pytest
from sensei_sdk import framing, ratchet, secure, vectors
from sensei_sdk.secure import EpochError, GapError, GenerationError, KeyStore, NodeSession, ReplayError, ReplayWindow, \
    SecureError

KEYS = vectors.fixed_keys()

# ---- HKDF written out (RFC 5869), so the module is not checked against itself ----


def hkdf(ikm: bytes, salt: bytes, info: bytes, length: int) -> bytes:
    prk = hmac.new(salt or bytes(32), ikm, hashlib.sha256).digest()
    out, t = b"", b""
    for i in range(1, -(-length // 32) + 1):
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        out += t
    return out[:length]


def test_root0():
    expect = hkdf(KEYS.k_up + KEYS.k_down, b"SENSEI-E2E-root-v1", struct.pack("<I", KEYS.key_id), 32)
    assert ratchet.root0(KEYS.k_up, KEYS.k_down, KEYS.key_id) == expect
    assert ratchet.root0(KEYS.k_up, KEYS.k_down, KEYS.key_id ^ 1) != expect  # the key id is in the derivation
    assert len(expect) == 32


def test_x25519_vectors_and_low_order():
    cases = vectors.x25519_cases()  # RFC 7748 section 5.2 and 6.1; asserts that the library agrees
    assert len(cases) == 6
    priv = cases[0][0]
    for low in vectors.LOW_ORDER:
        with pytest.raises(ratchet.DhError):
            ratchet.x25519(priv, low)
    with pytest.raises(SecureError):
        ratchet.x25519(priv, bytes(31))
    # the top bit of u is ignored (RFC 7748)
    _, peer, out = cases[1]
    assert ratchet.x25519(cases[1][0], peer[:31] + bytes([peer[31] ^ 0x80])) == out
    a_priv, a_pub = ratchet.x25519_keygen()
    b_priv, b_pub = ratchet.x25519_keygen()
    assert ratchet.x25519_public(a_priv) == a_pub and ratchet.x25519(a_priv, b_pub) == ratchet.x25519(b_priv, a_pub)


def test_mix_matches_the_written_out_schedule():
    for m in vectors.mix_cases():
        th = b"SENSEI-RK-v1" + struct.pack("<III", KEYS.key_id, m.gen, m.up_base) + m.e_n_pub + m.e_c_pub
        assert len(th) == 88 and m.mix.th == th
        okm = hkdf(m.dh, m.root, th, 80)
        assert (m.mix.root, m.mix.ck, m.mix.k_down) == (okm[:32], okm[32:64], okm[64:80])
        assert m.dh == ratchet.x25519(m.e_c_priv, m.e_n_pub)  # X25519 both ways
        # everything is bound
        base = (m.root, KEYS.key_id, m.gen, m.up_base, m.e_n_priv, m.e_c_pub)
        for i, other in enumerate((bytes(range(32)), KEYS.key_id ^ 1, m.gen + 1, m.up_base + 1)):
            args = list(base)
            args[i] = other
            assert ratchet.mix_node(*args) != m.mix
        with pytest.raises(ratchet.DhError):
            ratchet.mix_node(m.root, KEYS.key_id, m.gen, m.up_base, m.e_n_priv, vectors.LOW_ORDER[0])
        with pytest.raises(ratchet.DhError):
            ratchet.mix_from_dh(m.root, bytes(32), m.mix.th)
    first, fourth = vectors.mix_cases()
    assert fourth.root == first.mix.root and fourth.gen == 4  # the second mix builds on the first


def test_chain_written_out():
    first = vectors.mix_cases()[0]
    cks, mks, far = vectors.chain_keys(first.mix.ck)
    ck = first.mix.ck
    for i in range(4):
        assert cks[i] == ck
        assert mks[i] == hmac.new(ck, b"\x01", hashlib.sha256).digest()[:16]
        ck = hmac.new(ck, b"\x02", hashlib.sha256).digest()
    assert ck == ratchet.next_chain_key(cks[3])
    chain = ratchet.Chain(first.mix.ck, first.up_base)
    chain.advance_to(1000)
    assert chain.ck == far and chain.idx == 1000


def test_chain_walk_rules():
    ck0 = vectors.mix_cases()[0].mix.ck
    base = 10
    chain = ratchet.Chain(ck0, base)
    mks = vectors.chain_keys(ck0)[1]
    assert ratchet.epoch(0) == 0 and ratchet.epoch(63) == 0 and ratchet.epoch(64) == 1
    assert ratchet.epoch(0xFFFFFFFF) == 0x3FFFFFF and ratchet.EPOCH_SIZE == secure.WINDOW
    # the 64 counters of an epoch share a key, the next epoch has another
    assert chain.key_for(64 * base) == chain.key_for(64 * base + 63) == mks[0]
    assert chain.key_for(64 * (base + 3) + 5) == mks[3] and chain.idx == 0  # not committed
    assert chain.key_for(64 * (base + 3), commit = True) == mks[3] and chain.idx == 3
    assert chain.key_for(64 * (base + 3) + 63, commit = True) == mks[3]
    with pytest.raises(EpochError):  # behind the chain
        chain.key_for(64 * (base + 2))
    with pytest.raises(EpochError):  # before the start
        ratchet.Chain(ck0, base).key_for(64 * base - 1)
    assert chain.idx == 3
    # MAX_SKIP: refused beyond, the chain stays
    chain = ratchet.Chain(ck0, base)
    chain.advance_to(100, max_skip = 100)
    with pytest.raises(GapError):
        chain.advance_to(201, max_skip = 100)
    assert chain.idx == 100
    chain.advance_to(200, max_skip = 100)
    assert chain.idx == 200
    with pytest.raises(GapError):
        ratchet.Chain(ck0, base).key_for(64 * (base + ratchet.MAX_SKIP + 1))
    with pytest.raises(GapError):
        ratchet.Chain(ck0, 0).key_for(0xFFFFFFFF)
    assert ratchet.MAX_SKIP == 1 << 20


# ---- envelope: generation bits ----


def test_generation_zero_is_unchanged():
    env = secure.seal_raw(bytes(16), 1, 1, secure.KIND_UPLINK, b"x" * 20)
    assert env[1] == secure.KIND_UPLINK and env[0] == 0xAC and len(env) == 46
    assert env == secure.seal_raw(bytes(16), 1, 1, secure.KIND_UPLINK, b"x" * 20, gen = 0)
    h = secure.parse_header(env)
    assert (h.kind, h.ratchet, h.gen_bits) == (1, False, 0)
    assert secure.kind_of(env) == 1 and framing.kind_of(env) == 1 and secure.gen_bits_of(env) == 0
    assert not secure.is_ratchet(env)


@pytest.mark.parametrize("kind", [1, 2, 3, 4])
@pytest.mark.parametrize("gen", [1, 2, 3, 4, 5, 8, 1000])
def test_generation_bits_in_the_kind_byte(kind, gen):
    down = kind in (secure.KIND_DOWNLINK, secure.KIND_SETTINGS)
    env = secure.seal_raw(bytes(16), 7, 5, kind, b"hello", sender = 3 if down else 0, gen = gen)
    assert env[1] == 0x80 | ((gen & 3) << 4) | kind
    assert len(env) == len(b"hello") + (30 if down else 26)  # the size does not change
    assert secure.is_sealed(env) and framing.looks_sealed(env) and env[0] == (0xAD if down else 0xAC)
    h = secure.parse_header(env)
    assert (h.kind, h.ratchet, h.gen_bits, h.counter, h.key_id) == (kind, True, gen & 3, 5, 7)
    assert secure.kind_of(env) == kind and secure.gen_bits_of(env) == gen & 3 and secure.is_ratchet(env)
    assert secure.open_raw(bytes(16), env) == b"hello"
    # the gen bits are in the associated data: changing them breaks the tag
    bad = bytearray(env)
    bad[1] ^= 0x10
    with pytest.raises(SecureError, match = "authentication"):
        secure.open_raw(bytes(16), bytes(bad))
    # generation 4 and generation 0 differ only by the ratchet bit
    if gen == 4:
        assert env[1] & 0x30 == 0 and env[1] & 0x80


def test_header_refuses_what_it_does_not_know():
    env = secure.seal_raw(bytes(16), 1, 1, secure.KIND_UPLINK, b"x" * 20, gen = 1)
    bad = bytearray(env)
    bad[1] |= 0x40  # reserved bit
    with pytest.raises(SecureError, match = "reserved"):
        secure.parse_header(bytes(bad))
    bad[1] = secure.KIND_UPLINK | 0x10  # gen bits without the ratchet bit
    with pytest.raises(SecureError, match = "generation bits"):
        secure.parse_header(bytes(bad))
    bad[1] = 0x80 | 0x10 | secure.KIND_DOWNLINK  # a downlink kind in an uplink
    with pytest.raises(SecureError, match = "direction"):
        secure.parse_header(bytes(bad))
    with pytest.raises(SecureError):
        secure.seal_raw(bytes(16), 1, 1, secure.KIND_UPLINK, b"x", gen = -1)


def test_generation_zero_readers_refuse_ratchet_envelopes(tmp_path):
    pem = tmp_path / "b.pem"
    secure.backend_keygen(pem)
    store = KeyStore.load(tmp_path / "k.json", private_pem = pem)
    priv, pub = secure.generate_keypair()
    store.add_node(1, pub)
    node = NodeSession(priv, store.backend_public_key())
    env = secure.seal_raw(node.keys.k_up, node.keys.key_id, 1, secure.KIND_UPLINK, b"x", gen = 1)
    with pytest.raises(GenerationError):
        secure.open_sealed(env, store)
    with pytest.raises(GenerationError):
        secure.peek_sealed(env, store)
    down = secure.seal_raw(node.keys.k_down, node.keys.key_id, 1, secure.KIND_DOWNLINK, b"x", sender = 5, gen = 1)
    with pytest.raises(GenerationError):
        node.open(down)
    # the refusals changed nothing: the same counters open as generation 0
    ok = node.seal(secure.KIND_UPLINK, b"y")
    assert secure.open_sealed(ok, store)[2] == b"y"


# ---- the vectors ----


def test_envelope_vectors_are_what_they_claim():
    keys = KEYS
    m1, m4 = vectors.mix_cases()
    for kind, gen, node_id, sender, counter, plain, sealed, expect in vectors.ratchet_envelope_cases():
        if expect == 0:
            h = secure.parse_header(sealed)
            assert (h.kind, h.counter, h.sender, h.node_id, h.ratchet, h.gen_bits) == (kind, counter, sender, node_id,
                                                                                       True, gen & 3)
            mix, up = (m1, 1) if gen == 1 else (m4, 4)
            if kind in (secure.KIND_UPLINK, secure.KIND_NODE_TO_HOST):
                key = ratchet.Chain(mix.mix.ck, mix.up_base).key_for(counter)
            else:
                key = mix.mix.k_down
            assert secure.open_raw(key, sealed) == plain
            assert h.key_id == keys.key_id and up in (1, 4)
        elif expect == vectors.ERR_FORMAT:
            with pytest.raises(SecureError):
                secure.parse_header(sealed)
        else:  # the header is fine, the refusal comes from the key (epoch, generation) or the tag
            h = secure.parse_header(sealed)
            assert h.ratchet
            if expect == vectors.ERR_EPOCH:
                assert ratchet.epoch(h.counter) < m1.up_base
            if expect == vectors.ERR_AUTH:
                key = m1.mix.k_down if h.kind in (secure.KIND_DOWNLINK, secure.KIND_SETTINGS) else m1.mix.ck[:16]
                with pytest.raises(SecureError, match = "authentication"):
                    secure.open_raw(key, sealed, h)


# ---- the transcript, replayed by a model of the two sides ----


class Model:
    """The two sides of the transcript, in the little that is needed (the key stores of Phases 3 and 4 do this for real)."""

    def __init__(self):
        mix = vectors.make_mix(ratchet.root0(KEYS.k_up, KEYS.k_down, KEYS.key_id), KEYS.key_id, 1, vectors.UP_BASE_T,
                               vectors.E_N_PRIV, vectors.E_C_PRIV)
        self.mix = mix
        self.n_gen = 0
        self.n_chain = None
        self.n_prev = False
        self.n_rx: dict[int, ReplayWindow] = {}
        self.n_last = 0
        self.b_chain = None
        self.b_promoted = False
        self.b_prev = False
        self.b_rx = ReplayWindow()
        self.b_down = 0

    def seal_n(self, st: vectors.Step) -> bytes:
        assert st.counter > self.n_last  # a counter never goes back
        self.n_last = st.counter
        key = KEYS.k_up if self.n_gen == 0 else self.n_chain.key_for(st.counter, commit = True)
        return secure.seal_raw(key, KEYS.key_id, st.counter, st.kind, st.plain, gen = self.n_gen)

    def seal_b(self, st: vectors.Step) -> bytes:
        assert st.counter > self.b_down
        self.b_down = st.counter
        key = KEYS.k_down if st.gen == 0 else self.mix.mix.k_down
        return secure.seal_raw(key, KEYS.key_id, st.counter, st.kind, st.plain, sender = st.sender, gen = st.gen)

    def switch(self):
        self.n_gen, self.n_prev = 1, True
        self.n_chain = ratchet.Chain(self.mix.mix.ck, self.mix.up_base)
        self.b_chain = ratchet.Chain(self.mix.mix.ck, self.mix.up_base)
        self.b_promoted, self.b_prev = False, True

    def open_b(self, sealed: bytes) -> tuple[int, bytes | None]:
        h = secure.parse_header(sealed)
        try:
            self.b_rx.check(h.counter)
        except ReplayError:
            return vectors.ERR_REPLAY, None
        try:
            if h.ratchet:
                if h.gen_bits != 1:
                    raise GenerationError("no such generation")
                key = self.b_chain.key_for(h.counter)
            elif self.b_promoted and not self.b_prev:
                raise GenerationError("generation 0 is dropped")
            else:
                key = KEYS.k_up
        except GenerationError:
            return vectors.ERR_GEN, None
        except EpochError:
            return vectors.ERR_EPOCH, None
        try:
            plain = secure.open_raw(key, sealed, h)
        except SecureError:
            return vectors.ERR_AUTH, None
        self.b_rx.update(h.counter)
        if h.ratchet:  # promotion, and the chain is kept at idx_lo = max(0, idx(highest) - 1)
            self.b_promoted = True
            idx = ratchet.epoch(self.b_rx.highest) - self.b_chain.up_base
            if idx - 1 > self.b_chain.idx:
                self.b_chain.advance_to(idx - 1)
        return 0, plain

    def open_n(self, sealed: bytes) -> tuple[int, bytes | None]:
        h = secure.parse_header(sealed)
        win = self.n_rx.setdefault(h.sender, ReplayWindow())
        try:
            win.check(h.counter)
        except ReplayError:
            return vectors.ERR_REPLAY, None
        if h.ratchet:
            if self.n_gen == 0 or h.gen_bits != self.n_gen & 3:
                return vectors.ERR_GEN, None
            key = self.mix.mix.k_down
        elif self.n_gen == 0 or self.n_prev:
            key = KEYS.k_down
        else:
            return vectors.ERR_GEN, None
        try:
            plain = secure.open_raw(key, sealed, h)
        except SecureError:
            return vectors.ERR_AUTH, None
        win.update(h.counter)
        return 0, plain


def test_transcript_replay():
    steps = vectors.transcript_steps()
    assert len(steps) >= 30
    m = Model()
    for i, st in enumerate(steps):
        if st.op == vectors.OP_SEAL_N:
            assert m.seal_n(st) == st.sealed, f"step {i}"
        elif st.op == vectors.OP_SEAL_B:
            assert m.seal_b(st) == st.sealed, f"step {i}"
        elif st.op == vectors.OP_SWITCH:
            m.switch()
        elif st.op in (vectors.OP_OPEN_B, vectors.OP_OPEN_N):
            ref = steps[st.ref]
            code, plain = (m.open_b if st.op == vectors.OP_OPEN_B else m.open_n)(ref.sealed)
            assert code == st.expect, f"step {i}: {code} != {st.expect}"
            assert plain == (ref.plain if code == 0 else None)
        elif st.op == vectors.OP_DROP_PREV:
            m.n_prev = m.b_prev = False
        else:
            raise AssertionError(st)


def test_transcript_covers_what_the_design_lists():
    steps = vectors.transcript_steps()
    gens = [(s.op, s.gen) for s in steps if s.op in (vectors.OP_SEAL_N, vectors.OP_SEAL_B)]
    assert (vectors.OP_SEAL_N, 0) in gens and (vectors.OP_SEAL_N, 1) in gens
    assert (vectors.OP_SEAL_B, 0) in gens and (vectors.OP_SEAL_B, 1) in gens
    counters = [s.counter for s in steps if s.op == vectors.OP_SEAL_N and s.gen == 1]
    assert counters == sorted(counters) and ratchet.epoch(counters[-1]) - ratchet.epoch(counters[0]) >= 8  # epoch jumps
    expects = {s.expect for s in steps}
    assert {0, vectors.ERR_REPLAY, vectors.ERR_GEN} <= expects


# ---- property: no (key, nonce) twice ----


def test_no_key_nonce_pair_twice():
    """1e5 uplinks over 20 mixes with epoch jumps, and downlinks of 8 senders (design 1.9)."""
    import random

    rng = random.Random(7)
    seen: set[bytes] = set()
    per_key: dict[bytes, int] = {}
    root = ratchet.root0(KEYS.k_up, KEYS.k_down, KEYS.key_id)
    gen, counter, chain, mk, last_ep = 0, 0, None, KEYS.k_up, None
    kdown = [KEYS.k_down]
    mixes = 0
    n_up = 100_000
    for i in range(n_up):
        if mixes < 20 and i == (mixes + 1) * (n_up // 21):
            up_base = ratchet.epoch(counter) + 1 + rng.randrange(3)
            n_priv, n_pub = ratchet.x25519_keygen()
            c_priv, c_pub = ratchet.x25519_keygen()
            gen += 1
            node = ratchet.mix_node(root, KEYS.key_id, gen, up_base, n_priv, c_pub)
            assert node == ratchet.mix_collector(root, KEYS.key_id, gen, up_base, c_priv, n_pub)
            root = node.root
            kdown.append(node.k_down)
            chain = ratchet.Chain(node.ck, up_base)
            counter = max(counter, 64 * up_base - 1)
            last_ep = None
            mixes += 1
        counter += 1
        if rng.randrange(97) == 0:  # a reboot or an old epoch: the next epoch starts
            counter = (counter + 63) & ~63
            if rng.randrange(5) == 0:
                counter += 64 * rng.randrange(7)
        if gen >= 1:
            ep = ratchet.epoch(counter)
            assert ep >= chain.up_base
            if ep != last_ep:
                mk, last_ep = chain.key_for(counter, commit = True), ep
        pair = mk + struct.pack("<BIII", 0, 0, 0, counter)[1:]
        assert pair not in seen
        seen.add(pair)
        if gen >= 1:
            per_key[mk] = per_key.get(mk, 0) + 1
    assert mixes == 20 and max(per_key.values()) <= ratchet.EPOCH_SIZE
    assert len(per_key) > 1000
    dctr = [0] * 8
    for i in range(40_000):
        s = rng.randrange(8)
        dctr[s] += 1 + (rng.randrange(100) if rng.randrange(3) == 0 else 0)
        pair = kdown[i * 21 // 40_000] + struct.pack("<BIII", 1, 0x1000 + s, dctr[s], 0)[:9]
        assert pair not in seen
        seen.add(pair)
