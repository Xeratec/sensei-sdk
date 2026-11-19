# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Commissioning over BLE: SPAKE2+ (RFC 9383), registration, channel, proof, setup code, QR, and the key store re-read
for an unknown key id (onboarding design, step A)."""

from __future__ import annotations

import functools
import hashlib
import itertools
import secrets
import threading

import pytest
from sensei_sdk import commission as com
from sensei_sdk import p256, secure
from sensei_sdk import vectors_commission as vc
from sensei_sdk.commission import PROVER, VERIFIER, Channel, CodeError, CommissionError, InvalidPointError, Params, \
    ProofError, Prover, StateError, Verifier, WrongCodeError
from sensei_sdk.secure import KeyStore, NodeSession, ReplayError, SecureError, UnknownKeyError

RFC = vc.RFC9383_P256
NODE_ID = 0x00A1B2C3D4E5F607
CODE = "000000000003"  # fake: the bench never draws a code of one repeated digit
SALT = bytes(range(0x10, 0x20))
NG, NN = bytes(range(0x30, 0x40)), bytes(range(0x50, 0x60))
SID = bytes(range(0x70, 0x80))  # the id of a PAKE run


@pytest.fixture(scope = "module", autouse = True)
def cached_scrypt():
    """The tamper loops start hundreds of sessions: scrypt runs once per distinct (code, node, parameters)."""
    real = com.derive_w
    com.derive_w = functools.lru_cache(maxsize = None)(real)
    yield
    com.derive_w = real


@pytest.fixture(scope = "module")
def verifier() -> Verifier:
    return com.make_verifier(CODE, NODE_ID, SALT, log2n = 14)  # the lowest cost that a commissioner accepts


@pytest.fixture(scope = "module")
def prover_w(verifier):
    return com.derive_w(CODE, NODE_ID, verifier.params)


def make_prover(verifier, code = CODE, node_id = NODE_ID, ng = NG, nn = NN, x = None, params = None) -> Prover:
    return Prover.from_code(code, node_id, params or verifier.params, ng, nn, x)


def run(verifier, **kw):
    """A complete session. Returns prover, verifier session and the four messages."""
    p = make_prover(verifier, **kw)
    s = verifier.session(NODE_ID, NG, NN)
    share_p = p.share()
    share_v, confirm_v = s.pake1(share_p)
    confirm_p = p.finish(share_v, confirm_v)
    s.pake3(confirm_p)
    return p, s, (share_p, share_v, confirm_v, confirm_p)


# ---- RFC 9383 appendix C (P-256, SHA-256) ----


def rfc_sessions():
    w0, w1 = int.from_bytes(RFC["w0"], "big"), int.from_bytes(RFC["w1"], "big")
    p = Prover(w0, w1, vc.RFC_CONTEXT, vc.RFC_ID_PROVER, vc.RFC_ID_VERIFIER, 0, NG, NN, RFC["x"])
    s = com.VerifierSession(RFC["w0"], RFC["l"], vc.RFC_CONTEXT, vc.RFC_ID_PROVER, vc.RFC_ID_VERIFIER, 0, NG, NN,
                            RFC["y"])
    return p, s


def test_rfc_m_and_n_are_the_published_points():
    assert com.M in RFC["tt"] and com.N in RFC["tt"]
    assert com.M[:1] == com.N[:1] == b"\x04" and len(com.M) == len(com.N) == 65


def test_rfc_formulas_give_the_published_values():
    w0, w1 = int.from_bytes(RFC["w0"], "big"), int.from_bytes(RFC["w1"], "big")
    v = vc.compute_session(w0, w1, RFC["x"], RFC["y"], vc.RFC_CONTEXT, vc.RFC_ID_PROVER, vc.RFC_ID_VERIFIER)
    for name in ("share_p", "share_v", "z", "v", "tt"):
        assert v[name] == RFC[name], name
    sch = v["sch"]
    for name in ("k_main", "k_confirm_p", "k_confirm_v", "confirm_p", "confirm_v", "k_shared"):
        assert getattr(sch, name) == RFC[name], name
    assert p256.encode(p256.mul(w1, p256.G)) == RFC["l"]


def test_rfc_prover_and_verifier_sessions():
    p, s = rfc_sessions()
    assert p.share() == RFC["share_p"]
    share_v, confirm_v = s.pake1(RFC["share_p"])
    assert (share_v, confirm_v) == (RFC["share_v"], RFC["confirm_v"])
    assert p.finish(share_v, confirm_v) == RFC["confirm_p"]
    s.pake3(RFC["confirm_p"])
    assert p.keys == s.keys == com.channel_keys(RFC["k_shared"], 0, NG, NN)  # K_shared itself is not kept


def test_rfc_vectors_with_a_wrong_confirmation():
    p, s = rfc_sessions()
    p.share()
    share_v, _ = s.pake1(RFC["share_p"])
    with pytest.raises(WrongCodeError):
        p.finish(share_v, bytes(32))
    with pytest.raises(WrongCodeError):
        s.pake3(bytes(32))


# ---- registration (V10) ----


def test_registration_vectors_match(verifier):
    for r in vc.reg_cases():
        v = com.make_verifier(r.code, r.node_id, r.salt, r.log2n)
        assert v == r.verifier and v.encode() == r.verifier.encode()
        assert r.dk == com.scrypt_output(r.code, r.node_id, r.params)
        assert (r.w0, r.w1) == (int.from_bytes(r.dk[:40], "big") % p256.N, int.from_bytes(r.dk[40:], "big") % p256.N)
        assert r.verifier.l == p256.encode(p256.mul(r.w1, p256.G)) and r.verifier.w0 == r.w0.to_bytes(32, "big")


def test_password_input_layout():
    pwin = com.pwin(b"00000000000", NODE_ID)
    assert pwin == ((11).to_bytes(8, "little") + b"00000000000" + (19).to_bytes(8, "little") + b"SENSEI-commissioner" +
                    (19).to_bytes(8, "little") + b"SENSEI-node" + NODE_ID.to_bytes(8, "big"))
    assert len(com.ID_PROVER) == len(com.id_verifier(NODE_ID)) == 19


def test_scrypt_is_the_hashlib_one_at_the_default_cost():
    params = Params(SALT)
    assert (params.log2n, params.r, params.p) == (15, 8, 1)
    want = hashlib.scrypt(com.pwin(b"00000000000", NODE_ID),
                          salt = SALT,
                          n = 2**15,
                          r = 8,
                          p = 1,
                          maxmem = 64 << 20,
                          dklen = 80)
    assert com.scrypt_output(CODE, NODE_ID, params) == want


def test_every_node_and_every_salt_gives_another_registration():
    a = com.make_verifier(CODE, NODE_ID, SALT, 14)
    assert com.make_verifier(CODE, NODE_ID + 1, SALT, 14).w0 != a.w0
    assert com.make_verifier(CODE, NODE_ID, SALT[::-1], 14).w0 != a.w0
    assert com.make_verifier("111111111115", NODE_ID, SALT, 14).w0 != a.w0
    b, c = com.make_verifier(CODE, NODE_ID, log2n = 14), com.make_verifier(CODE, NODE_ID, log2n = 14)
    assert b.params.salt != c.params.salt and len(b.params.salt) == 16


def test_make_verifier_draws_again_if_w_is_zero(monkeypatch):
    real = com.derive_w
    calls = []

    def flaky(code, node_id, params):
        calls.append(params.salt)
        if len(calls) == 1:
            raise CommissionError("w0 or w1 is zero: use another salt")
        return real(code, node_id, params)

    monkeypatch.setattr(com, "derive_w", flaky)
    com.make_verifier(CODE, NODE_ID, log2n = 14)
    assert len(calls) == 2 and calls[0] != calls[1]
    calls.clear()
    with pytest.raises(CommissionError, match = "zero"):  # a fixed salt cannot be replaced
        com.make_verifier(CODE, NODE_ID, SALT, 14)


def test_verifier_record_round_trip_and_refusals(verifier):
    rec = verifier.encode()
    assert len(rec) == 121 and rec[:8] == bytes([1, 1, 1, 14, 8, 1, 0, 0]) and rec[8:24] == SALT
    assert Verifier.decode(rec) == verifier and len(verifier.record()) == 97 and verifier.record() == rec[24:]
    bad_w0 = (0).to_bytes(32, "big"), p256.N.to_bytes(32, "big"), b"\xff" * 32
    for w0 in bad_w0:
        with pytest.raises(CommissionError):
            Verifier.decode(rec[:24] + w0 + rec[56:])
    for bad in (
            rec[:-1],
            rec + b"\0",
            bytes([2]) + rec[1:],
            rec[:6] + b"\1\0" + rec[8:],
            b"",
            rec[:56] + rec[56:64] + bytes([rec[64] ^ 1]) + rec[65:],  # L off the curve
            rec[:56] + b"\x04" + bytes(64)):
        with pytest.raises(CommissionError):
            Verifier.decode(bad)
    with pytest.raises(com.ParamsError):  # a record with a cost outside the allowed range
        Verifier.decode(rec[:3] + bytes([20]) + rec[4:])


@pytest.mark.parametrize("change", [{
    "suite": 2
}, {
    "kdf": 2
}, {
    "kdf": 0
}, {
    "log2n": 13
}, {
    "log2n": 18
}, {
    "r": 4
}, {
    "p": 2
}, {
    "salt": bytes(15)
}, {
    "salt": bytes(17)
}])
def test_a_commissioner_refuses_other_parameters(change):
    params = Params(**{"salt": SALT, **change})
    with pytest.raises(com.ParamsError):
        params.check()
    with pytest.raises(com.ParamsError):
        Prover.from_code(CODE, NODE_ID, params, NG, NN)


def test_all_allowed_costs_are_accepted():
    for log2n in range(14, 18):
        Params(SALT, log2n = log2n).check()


def test_nonce_lengths():
    for ng, nn in ((NG[:15], NN), (NG, NN + b"\0"), (b"", NN)):
        with pytest.raises(com.ParamsError):
            com.context(Params(SALT), ng, nn)


# ---- context, transcript, keys (V11) ----


def test_context_layout():
    ctx = com.context(Params(SALT, log2n = 15), NG, NN)
    assert len(ctx) == 73 and ctx[:20] == b"SENSEI-COMMISSION-v1" and ctx[20:25] == bytes([1, 1, 15, 8, 1])
    assert ctx[25:41] == SALT and ctx[41:57] == NG and ctx[57:] == NN


def test_transcript_layout():
    tt = com.transcript(b"ctx", b"P", b"V", b"sp", b"sv", b"z", b"v", b"w")
    items = [b"ctx", b"P", b"V", com.M, com.N, b"sp", b"sv", b"z", b"v", b"w"]
    assert tt == b"".join(len(i).to_bytes(8, "little") + i for i in items)


def test_session_vectors_match_the_sessions():
    regs = vc.reg_cases()
    for ses in vc.ses_cases():
        r = regs[ses.reg]
        p = Prover.from_code(r.code, r.node_id, r.params, ses.nonce_g, ses.nonce_n, ses.x)
        s = r.verifier.session(r.node_id, ses.nonce_g, ses.nonce_n, ses.y)
        sch = ses.values["sch"]
        assert p.share() == ses.values["share_p"]
        share_v, confirm_v = s.pake1(ses.values["share_p"])
        assert (share_v, confirm_v) == (ses.values["share_v"], sch.confirm_v)
        assert p.finish(share_v, confirm_v) == sch.confirm_p
        s.pake3(sch.confirm_p)
        assert p.keys == s.keys == ses.keys == com.channel_keys(sch.k_shared, r.node_id, ses.nonce_g, ses.nonce_n)
        assert com.context(r.params, ses.nonce_g, ses.nonce_n) == ses.context


def test_channel_keys_derivation_and_binding():
    k = secrets.token_bytes(32)
    keys = com.channel_keys(k, NODE_ID, NG, NN)
    assert len(keys.k_c2n) == len(keys.k_n2c) == 16 and keys.k_c2n != keys.k_n2c and 0 < keys.sid < 2**32
    assert com.channel_keys(k, NODE_ID, NG, NN) == keys
    for other in (com.channel_keys(k, NODE_ID + 1, NG, NN), com.channel_keys(k, NODE_ID, NN, NG),
                  com.channel_keys(k, NODE_ID, NG, NG), com.channel_keys(secrets.token_bytes(32), NODE_ID, NG, NN)):
        assert other != keys and other.k_c2n != keys.k_c2n


def test_session_id_zero_aborts(monkeypatch):
    monkeypatch.setattr(com, "_hkdf", lambda *a: bytes(36))
    with pytest.raises(CommissionError, match = "session id"):
        com.channel_keys(bytes(32), NODE_ID, NG, NN)


def test_binding_of_the_context(verifier):
    """The context carries the parameters, the salt and the nonces: a node or a man in the middle that shows the commissioner
    other values than the node uses cannot make the confirmation fit."""
    for kw in ({"ng": NN}, {"nn": NG}, {"node_id": NODE_ID + 1}):
        with pytest.raises(CommissionError):
            run_split(verifier, **kw)
    other = Params(SALT[::-1], log2n = 14)  # another salt: w0 differs as well
    with pytest.raises(WrongCodeError):
        run_split(verifier, params = other)
    # another scrypt cost than the one the node registered with: wrong code
    with pytest.raises(WrongCodeError):
        run_split(verifier, params = Params(SALT, log2n = 15))


def run_split(verifier, ng = NG, nn = NN, node_id = NODE_ID, params = None):
    """The commissioner works with ng, nn, node_id, params; the node with the true values."""
    p = Prover.from_code(CODE, node_id, params or verifier.params, ng, nn)
    s = verifier.session(NODE_ID, NG, NN)
    share_v, confirm_v = s.pake1(p.share())
    confirm_p = p.finish(share_v, confirm_v)
    s.pake3(confirm_p)


# ---- a full session, wrong codes ----


def test_full_run(verifier):
    p, s, (share_p, share_v, confirm_v, confirm_p) = run(verifier)
    assert p.keys == s.keys
    assert not hasattr(p, "k_shared") and not hasattr(s, "k_shared")
    assert (len(share_p), len(share_v), len(confirm_v), len(confirm_p)) == (65, 65, 32, 32)
    # a second session of the same code has other keys (ephemeral scalars)
    p2, _, _ = run(verifier)
    assert p2.keys != p.keys


def test_full_run_with_random_code_at_the_default_cost():
    code = com.generate_code()
    v = com.make_verifier(code, NODE_ID)
    assert (v.params.log2n, v.params.r, v.params.p) == (15, 8, 1)
    p = Prover.from_code(com.parse_code(com.format_code(code)), NODE_ID, v.params, NG, NN)
    s = v.session(NODE_ID, NG, NN)
    sv, cv = s.pake1(p.share())
    s.pake3(p.finish(sv, cv))
    assert p.keys == s.keys


def test_an_ephemeral_scalar_may_be_given_in_any_length(verifier):
    a = make_prover(verifier, x = bytes(range(1, 33))).share()
    assert make_prover(verifier, x = bytes(8) + bytes(range(1, 33))).share() == a  # leading zeros
    assert make_prover(verifier, x = (p256.N + 5).to_bytes(40, "big")).share() == make_prover(verifier,
                                                                                              x = b"\5").share()
    with pytest.raises(CommissionError):
        make_prover(verifier, x = bytes(40))
    with pytest.raises(CommissionError):
        make_prover(verifier, x = p256.N.to_bytes(32, "big"))


def test_random_scalars_are_40_bytes_and_not_zero():
    asked = []

    def rng(n):
        asked.append(n)
        return bytes(n) if len(asked) == 1 else b"\1" * n

    assert com._random_scalar(rng) == int.from_bytes(b"\1" * 40, "big") % p256.N
    assert asked == [40, 40]


def valid(digits: str) -> str:
    """The 11 digits with their check digit: a typed code that passes the check but is not the right one."""
    return digits + str(com.verhoeff_digit(digits))


@pytest.mark.parametrize("wrong", [valid("00000000001"), valid("11111111111"), valid("99999999999")])
def test_wrong_code_is_noticed_by_the_commissioner(verifier, wrong):
    p = make_prover(verifier, code = wrong)
    s = verifier.session(NODE_ID, NG, NN)
    sv, cv = s.pake1(p.share())
    with pytest.raises(WrongCodeError, match = "wrong setup code"):
        p.finish(sv, cv)
    with pytest.raises(StateError):  # the session is over
        _ = p.keys
    with pytest.raises(StateError):
        p.finish(sv, cv)


def test_a_commissioner_that_ignores_the_check_fails_at_the_node(verifier):
    """A prover with a wrong code that skips the confirmV check still cannot make a valid confirmP."""
    p = make_prover(verifier, code = valid("11111111111"))
    share_p = p.share()
    for confirm_p in (bytes(32), secrets.token_bytes(32), b"", bytes(31), bytes(33)):
        s = verifier.session(NODE_ID, NG, NN)
        s.pake1(share_p)
        with pytest.raises(WrongCodeError):
            s.pake3(confirm_p)
        with pytest.raises(StateError):
            _ = s.keys


def test_wrong_code_and_node_id_in_the_commissioner(verifier):
    with pytest.raises(WrongCodeError):
        run_split(verifier, node_id = NODE_ID ^ 1)
    with pytest.raises(CodeError):  # check digit
        Prover.from_code("000000000004", NODE_ID, verifier.params, NG, NN)
    with pytest.raises(CodeError):
        com.make_verifier("00000000000", NODE_ID, SALT, 14)  # 11 digits: the check digit is missing


# ---- invalid points ----


@pytest.mark.parametrize("what, data", vc.bad_points(), ids = [w for w, _ in vc.bad_points()])
def test_the_node_refuses_an_invalid_share(verifier, what, data):
    s = verifier.session(NODE_ID, NG, NN)
    with pytest.raises(InvalidPointError):
        s.pake1(data)
    with pytest.raises(StateError):  # the session ended
        s.pake1(make_prover(verifier).share())


@pytest.mark.parametrize("what, data", vc.bad_points(), ids = [w for w, _ in vc.bad_points()])
def test_the_commissioner_refuses_an_invalid_share(verifier, what, data):
    p = make_prover(verifier)
    p.share()
    with pytest.raises(InvalidPointError):
        p.finish(data, bytes(32))
    with pytest.raises(StateError):
        p.finish(data, bytes(32))


def test_a_share_that_is_w0_times_m_is_refused(verifier, prover_w):
    """X - w0*M would be the identity: x = 0 (the commissioner never sends it; a peer that knows w0 might)."""
    w0, _ = prover_w
    s = verifier.session(NODE_ID, NG, NN)
    with pytest.raises(InvalidPointError, match = "identity"):
        s.pake1(p256.encode(p256.mul(w0, p256.decode(com.M))))


def test_a_share_that_is_w0_times_n_is_refused(verifier, prover_w):
    w0, _ = prover_w
    p = make_prover(verifier)
    p.share()
    with pytest.raises(InvalidPointError, match = "identity"):
        p.finish(p256.encode(p256.mul(w0, p256.decode(com.N))), bytes(32))


def test_the_group_generator_and_a_random_point_are_valid_shares_but_give_no_keys(verifier):
    for pt in (p256.G, p256.mul(secrets.randbelow(p256.N - 1) + 1, p256.G)):
        s = verifier.session(NODE_ID, NG, NN)
        s.pake1(p256.encode(pt))
        with pytest.raises(WrongCodeError):
            s.pake3(bytes(32))


# ---- tamper with every message, replay, order ----


def flip_byte(data: bytes, pos: int) -> bytes:
    return data[:pos] + bytes([data[pos] ^ 0x01]) + data[pos + 1:]


def test_tamper_with_share_p(verifier):
    # every byte of the point: a flipped bit leaves the curve (a valid point is shown below)
    for pos in range(65):
        s = verifier.session(NODE_ID, NG, NN)
        with pytest.raises(InvalidPointError):
            s.pake1(flip_byte(make_prover(verifier).share(), pos))
    # a valid point that is not the one sent
    p, s = make_prover(verifier), verifier.session(NODE_ID, NG, NN)
    other = p256.encode(p256.add(p256.decode(p.share()), p256.G))
    sv, cv = s.pake1(other)
    with pytest.raises(WrongCodeError):
        p.finish(sv, cv)
    with pytest.raises(WrongCodeError):
        s.pake3(bytes(32))


def make_prover_shared(verifier, x) -> Prover:
    p = make_prover(verifier, x = x)
    p.share()
    return p


def test_tamper_with_share_v_and_confirm_v(verifier):
    x, y = bytes(range(1, 33)), bytes(range(0x41, 0x61))  # fixed: the same two messages every time
    first = make_prover(verifier, x = x)
    sv, cv = verifier.session(NODE_ID, NG, NN, y).pake1(first.share())
    for pos in (*range(0, 65, 3), 64):
        with pytest.raises((InvalidPointError, WrongCodeError)):
            make_prover_shared(verifier, x).finish(flip_byte(sv, pos), cv)
    for pos in range(32):
        with pytest.raises(WrongCodeError):
            make_prover_shared(verifier, x).finish(sv, flip_byte(cv, pos))
    p, s = make_prover(verifier), verifier.session(NODE_ID, NG, NN)
    sv, cv = s.pake1(p.share())
    other = p256.encode(p256.add(p256.decode(sv), p256.G))  # a valid point that is not shareV
    with pytest.raises(WrongCodeError):
        p.finish(other, cv)


def test_tamper_with_confirm_p(verifier):
    for pos in range(32):
        p, s = make_prover(verifier), verifier.session(NODE_ID, NG, NN)
        sv, cv = s.pake1(p.share())
        cp = p.finish(sv, cv)
        with pytest.raises(WrongCodeError):
            s.pake3(flip_byte(cp, pos))
        with pytest.raises(StateError):  # no second try on the same session
            s.pake3(cp)


def test_replayed_messages_do_not_open_a_new_session(verifier):
    _, _, (share_p, share_v, confirm_v, confirm_p) = run(verifier)
    # recorded shareP and confirmP to a new node session: shareV is new, so confirmP does not fit
    s = verifier.session(NODE_ID, NG, NN)
    s.pake1(share_p)
    with pytest.raises(WrongCodeError):
        s.pake3(confirm_p)
    # recorded shareV and confirmV to a new commissioner session
    p = make_prover(verifier)
    p.share()
    with pytest.raises(WrongCodeError):
        p.finish(share_v, confirm_v)
    # same nonces and same salt in all three: still no (the ephemeral values are fresh)
    s = verifier.session(NODE_ID, NG, NN)
    sv, _ = s.pake1(share_p)
    assert sv != share_v


def test_steps_out_of_order(verifier):
    p, s = make_prover(verifier), verifier.session(NODE_ID, NG, NN)
    with pytest.raises(StateError):
        p.finish(bytes(65), bytes(32))  # before share()
    with pytest.raises(StateError):
        s.pake3(bytes(32))  # before pake1()
    with pytest.raises(StateError):
        _ = s.keys
    share_p = p.share()
    with pytest.raises(StateError):
        p.share()
    with pytest.raises(StateError):
        _ = p.keys
    sv, cv = s.pake1(share_p)
    with pytest.raises(StateError):
        s.pake1(share_p)  # a second shareP
    with pytest.raises(StateError):
        _ = s.keys  # before pake3()
    cp = p.finish(sv, cv)
    with pytest.raises(StateError):
        p.finish(sv, cv)
    s.pake3(cp)
    with pytest.raises(StateError):
        s.pake3(cp)
    assert p.keys == s.keys


def test_secrets_are_dropped(verifier):
    p, s, _ = run(verifier)
    assert p._x == p._w0 == p._w1 == 0 and s._y == 0 and s._sch is None


# ---- the channel (V12) ----


def pair(verifier):
    p, s, _ = run(verifier)
    return Channel(p.keys, PROVER), Channel(s.keys, VERIFIER)


def test_channel_both_ways(verifier):
    gui, node = pair(verifier)
    for i in range(1, 6):
        env = gui.seal(b"step %d" % i)
        assert secure.is_sealed(env) and env[1] == secure.KIND_COMMISSION and len(env) == 30 + 6
        assert node.open(env) == b"step %d" % i
        back = node.seal(b"ok %d" % i)
        assert len(back) == 26 + 4 and gui.open(back) == b"ok %d" % i
    h = secure.parse_header(env)
    assert (h.direction, h.sender, h.key_id, h.counter, h.node_id, h.ratchet) == (secure.DIR_DOWN, 1, gui.keys.sid, 5,
                                                                                  None, False)
    h = secure.parse_header(back)
    assert (h.direction, h.sender, h.counter) == (secure.DIR_UP, 0, 5) and gui.tx_counter == node.tx_counter == 5


def test_channel_counters_start_at_one_and_rise(verifier):
    gui, node = pair(verifier)
    assert [secure.parse_header(gui.seal(b"")).counter for _ in range(3)] == [1, 2, 3]
    assert secure.parse_header(node.seal(b"")).counter == 1  # each direction has its own


def test_channel_vectors_match(verifier):
    k = vc.ses_cases()[0].keys
    rx = {vc.ROLE_PROVER: Channel(k, PROVER), vc.ROLE_VERIFIER: Channel(k, VERIFIER)}
    for role, plain, env, expect, what in vc.channel_cases():
        code, opened = vc.channel_result(rx[role], env)
        assert code == expect and opened in (None, plain), what


def test_channel_replay_and_window(verifier):
    gui, node = pair(verifier)
    e = {c: gui.seal(b"c%d" % c, c) for c in (1, 2, 3, 70, 71, 6)}
    assert node.open(e[1]) == b"c1"
    with pytest.raises(ReplayError):
        node.open(e[1])
    assert node.open(e[3]) == b"c3" and node.open(e[2]) == b"c2"  # late but inside the window
    with pytest.raises(ReplayError):
        node.open(e[2])
    assert node.open(e[70]) == b"c70"
    with pytest.raises(ReplayError, match = "old"):
        node.open(e[6])  # 70 - 6 = 64: outside the window of 64
    assert node.open(e[71]) == b"c71"
    with pytest.raises(ReplayError, match = "old"):
        node.open(gui.seal(b"x", 7))  # 71 - 7 = 64: outside
    assert node.open(gui.seal(b"x", 8)) == b"x"  # 71 - 8 = 63: inside


def test_channel_counter_zero_and_overflow(verifier):
    gui, node = pair(verifier)
    with pytest.raises(SecureError, match = "counter"):
        gui.seal(b"", 0)
    with pytest.raises(SecureError, match = "counter"):
        gui.seal(b"", 2**32)
    gui.seal(b"", 0xFFFFFFFF)
    with pytest.raises(SecureError, match = "counter"):
        gui.seal(b"")  # the counter is used up: never wraps
    zero = vc._manual(gui.keys.k_c2n, gui.keys.sid, 0, 1, b"")
    with pytest.raises(ReplayError, match = "counter 0"):
        node.open(zero)


def test_channel_refuses_what_is_not_its_own(verifier):
    gui, node = pair(verifier)
    _, other_node = pair(verifier)
    env = gui.seal(b"hello")
    with pytest.raises(CommissionError):  # reflected frames: own direction
        gui.open(env)
    with pytest.raises(CommissionError):
        node.open(node.seal(b"hello"))
    with pytest.raises(CommissionError):  # another session (another session id)
        other_node.open(env)
    with pytest.raises(CommissionError, match = "too long"):
        node.open(env + bytes(400))
    with pytest.raises(SecureError):
        node.open(env[:25])
    # another kind under the right key: kind 2 is a downlink for the node, but not a commissioning frame
    kind2 = secure.seal_raw(gui.keys.k_c2n, gui.keys.sid, 9, secure.KIND_DOWNLINK, b"x", sender = 1)
    with pytest.raises(CommissionError):
        node.open(kind2)
    # the bits of the kind byte and the node id flag
    with pytest.raises(SecureError):
        node.open(bytes([env[0], env[1] | 0x80]) + env[2:])
    with pytest.raises(SecureError):
        node.open(bytes([env[0], env[1] | 0x40]) + env[2:])
    with pytest.raises(SecureError):
        node.open(bytes([env[0] | 2]) + env[1:])
    assert node.open(env) == b"hello"


def test_every_byte_of_an_envelope_is_authenticated(verifier):
    gui, node = pair(verifier)
    env = gui.seal(b"set time 1700000000")
    for pos in range(len(env)):
        for bit in (0x01, 0x80):
            bad = env[:pos] + bytes([env[pos] ^ bit]) + env[pos + 1:]
            with pytest.raises(SecureError):
                node.open(bad)
    assert node.rx.highest == 0  # nothing was taken
    assert node.open(env) == b"set time 1700000000"
    back = node.seal(b"result ok")
    for pos in range(len(back)):
        with pytest.raises(SecureError):
            gui.open(back[:pos] + bytes([back[pos] ^ 1]) + back[pos + 1:])
    assert gui.open(back) == b"result ok"


def test_channel_size_limit(verifier):
    gui, node = pair(verifier)
    big = gui.seal(bytes(290))
    assert len(big) == 320 and node.open(big) == bytes(290)
    with pytest.raises(CommissionError, match = "320"):
        gui.seal(bytes(291))
    assert len(node.seal(bytes(294))) == 320
    with pytest.raises(CommissionError):
        node.seal(bytes(295))


def test_channel_directions_use_two_keys_and_nonces(verifier):
    gui, node = pair(verifier)
    assert gui.keys.k_c2n != gui.keys.k_n2c
    a, b = gui.seal(b"same", 7), node.seal(b"same", 7)
    assert a[-20:] != b[-20:] and secure._nonce(1, 1, 7) != secure._nonce(0, 0, 7)
    # a frame of one direction does not open under the key of the other
    with pytest.raises(SecureError):
        secure.open_raw(gui.keys.k_n2c, a)


def test_channel_role_and_keys():
    keys = com.ChannelKeys(bytes(16), bytes(16), 5)
    with pytest.raises(ValueError):
        Channel(keys, "x")


def test_kind_5_header_rules():
    key = bytes(16)
    env = secure.seal_raw(key, 5, 1, secure.KIND_COMMISSION, b"x")
    assert env[0] == 0xAC and env[1] == 5 and secure.kind_of(env) == 5 and secure.parse_header(env).kind == 5
    down = secure.seal_raw(key, 5, 1, secure.KIND_COMMISSION, b"x", sender = 1)
    assert down[0] == 0xAD and len(down) == len(env) + 4
    for kw in ({"sender": 2}, {"node_id": 7}, {"gen": 1}, {"sender": 1, "node_id": 7}):
        with pytest.raises(SecureError):
            secure.seal_raw(key, 5, 1, secure.KIND_COMMISSION, b"x", **kw)


def test_other_receivers_refuse_kind_5(tmp_path):
    pem = tmp_path / "b.pem"
    secure.backend_keygen(pem)
    store = KeyStore.load(tmp_path / "k.json", private_pem = pem)
    priv, pub = secure.generate_keypair()
    store.add_node(NODE_ID, pub)
    node = NodeSession(priv, store.backend_public_key())
    # a kind 5 frame that carries the right key id, as an attacker could make it: never reaches the key
    up = secure.seal_raw(node.keys.k_up, node.keys.key_id, 1, secure.KIND_COMMISSION, b"x")
    down = secure.seal_raw(node.keys.k_down, node.keys.key_id, 1, secure.KIND_COMMISSION, b"x", sender = 1)
    for fn in (secure.open_sealed, secure.peek_sealed):
        with pytest.raises(SecureError, match = "not a node to backend"):
            fn(up, store)
    with pytest.raises(SecureError, match = "wrong direction"):
        node.open(down)
    with pytest.raises(SecureError):
        secure.seal_downlink(NODE_ID, secure.KIND_COMMISSION, b"x", store)
    assert store.stats["reread"] == 0


# ---- fingerprints and proof (V13) ----


def test_fingerprints():
    for node_id, pub in vc.fp_cases():
        fp = com.node_fingerprint(node_id, pub)
        assert fp == hashlib.sha256(b"SENSEI-NODE-FP-v1" + node_id.to_bytes(8, "big") + pub).digest()[:16]
        text = com.fingerprint_text(fp)
        assert text == " ".join(fp[:8].hex()[i:i + 4] for i in range(0, 16, 4)) and len(text) == 19
    pub = vc.fp_cases()[0][1]
    assert com.node_fingerprint(1, pub) != com.node_fingerprint(2, pub)
    assert com.node_fingerprint(1, pub) != com.node_fingerprint(1, vc.fp_cases()[2][1])
    with pytest.raises(CommissionError):
        com.node_fingerprint(1, pub[:64])
    cfp = com.collector_fingerprint(pub)
    assert cfp == hashlib.sha256(b"SENSEI-COLLECTOR-FP-v1" + pub).digest()[:8] and len(cfp) == 8


@pytest.fixture
def provisioned(tmp_path):
    """A key store with the node added, the node side keys, and the nonces of a session."""
    pem = tmp_path / "b.pem"
    secure.backend_keygen(pem)
    store = KeyStore.load(tmp_path / "k.json", private_pem = pem)
    priv, pub = secure.generate_keypair()
    store.add_node(NODE_ID, pub)
    node = NodeSession(priv, store.backend_public_key())
    return store, node, pem


def test_proof_of_possession(provisioned):
    store, node, _ = provisioned
    proof = com.make_proof(node.keys, NODE_ID, NG, NN, SID, 1)
    node.tx_counter = 1  # the proof used the node's next counter
    assert secure.parse_header(proof).kind == secure.KIND_NODE_TO_HOST and len(proof) == 26 + 24 + 8 + 32 + 16
    com.check_proof(proof, store, NODE_ID, NG, NN, SID)
    with pytest.raises(ProofError):  # a replay
        com.check_proof(proof, store, NODE_ID, NG, NN, SID)
    # the node goes on with its next counter
    assert secure.open_sealed(node.seal(secure.KIND_UPLINK, b"data"), store)[2] == b"data"


def test_proof_binds_the_session(provisioned):
    store, node, _ = provisioned
    other = bytes([SID[0] ^ 1]) + SID[1:]
    cases = [(NODE_ID, NG, NG, SID), (NODE_ID, NN, NN, SID), (NODE_ID + 1, NG, NN, SID), (NODE_ID, NG, NN, other)]
    for i, (nid, ng, nn, sid) in enumerate(cases):
        with pytest.raises(ProofError):
            com.check_proof(com.make_proof(node.keys, nid, ng, nn, sid, 1 + i), store, NODE_ID, NG, NN, SID)
    # a proof for another session is a replay of nothing: the counters it used are gone, a fresh one still works
    com.check_proof(com.make_proof(node.keys, NODE_ID, NG, NN, SID, 9), store, NODE_ID, NG, NN, SID)


def test_proof_every_byte_is_checked(provisioned):
    store, node, _ = provisioned
    proof = com.make_proof(node.keys, NODE_ID, NG, NN, SID, 1)
    for pos in range(len(proof)):
        with pytest.raises(ProofError):
            com.check_proof(proof[:pos] + bytes([proof[pos] ^ 1]) + proof[pos + 1:],
                            store,
                            NODE_ID,
                            NG,
                            NN,
                            SID,
                            commit = False)
    for bad in (b"", proof[:10], proof[:-1], proof + b"x"):
        with pytest.raises(ProofError):
            com.check_proof(bad, store, NODE_ID, NG, NN, SID)
    com.check_proof(proof, store, NODE_ID, NG, NN, SID, commit = False)  # peek: not consumed
    com.check_proof(proof, store, NODE_ID, NG, NN, SID)


def test_a_fake_node_with_the_real_public_key_cannot_prove_it(provisioned):
    """A node that replays the real node's public key (it saw it, or stole the label) has another private key: its k_up and
    its key id are not the store's."""
    store, node, _ = provisioned
    fake_priv, _ = secure.generate_keypair()
    fake = secure.derive_keys(fake_priv, store.backend_public_key(), node.node_pub, store.backend_public_key())
    assert fake.key_id != node.keys.key_id
    with pytest.raises(ProofError, match = "key id"):
        com.check_proof(com.make_proof(fake, NODE_ID, NG, NN, SID, 1), store, NODE_ID, NG, NN, SID)
    # even with the right key id on the envelope: the tag fails
    forged = secure.seal_raw(fake.k_up, node.keys.key_id, 1, secure.KIND_NODE_TO_HOST,
                             com.proof_plain(NODE_ID, NG, NN, SID))
    with pytest.raises(ProofError, match = "failed"):
        com.check_proof(forged, store, NODE_ID, NG, NN, SID)
    with pytest.raises(ProofError):  # a node the store does not know
        com.check_proof(com.make_proof(node.keys, NODE_ID + 5, NG, NN, SID, 3), store, NODE_ID + 5, NG, NN, SID)


def test_proof_plain_layout():
    assert com.proof_plain(NODE_ID, NG, NN,
                           SID) == b"SENSEI-COMMISSION-pop-v2" + NODE_ID.to_bytes(8, "big") + NG + NN + SID
    with pytest.raises(CommissionError):
        com.proof_plain(NODE_ID, NG[:5], NN, SID)


def test_the_proof_vectors_match(provisioned):
    keys = vc.fixed_keys()
    for node_id, ng, nn, sid, counter in vc.proof_cases():
        proof = com.make_proof(keys, node_id, ng, nn, sid, counter)
        assert secure.open_raw(keys.k_up, proof) == com.proof_plain(node_id, ng, nn, sid)


def test_both_sides_get_the_same_session_id_and_it_is_not_k_main(verifier):
    ver = verifier
    ng, nn = com.new_nonce(), com.new_nonce()
    prover = com.Prover.from_code(CODE, NODE_ID, ver.params, ng, nn)
    node = ver.session(NODE_ID, ng, nn)
    share_v, confirm_v = node.pake1(prover.share())
    node.pake3(prover.finish(share_v, confirm_v))
    assert len(prover.session_id) == com.SESSION_ID_SIZE == 16 and prover.session_id == node.session_id
    with pytest.raises(StateError):
        com.Prover.from_code(CODE, NODE_ID, ver.params, ng, nn).session_id


def test_the_session_id_is_a_hash_of_the_transcript_of_this_run(verifier):
    """Two runs with the same nonces (what a relay sees) have different ids: the shares are fresh."""
    ver = verifier
    ng, nn = com.new_nonce(), com.new_nonce()
    ids = []
    for _ in range(2):
        prover = com.Prover.from_code(CODE, NODE_ID, ver.params, ng, nn)
        node = ver.session(NODE_ID, ng, nn)
        sv, cv = node.pake1(prover.share())
        node.pake3(prover.finish(sv, cv))
        ids.append(node.session_id)
    assert ids[0] != ids[1]


def test_a_relay_between_two_pake_runs_cannot_pass_the_proof_on(provisioned, verifier):
    """The relay holds the code. It is the verifier to the commissioner (run 1) and the prover to the node (run 2), with the
    same nonces and salt. The node's proof is bound to run 2; the commissioner checks it against run 1."""
    store, node, _ = provisioned
    ver = verifier
    ng, nn = com.new_nonce(), com.new_nonce()
    # run 2: relay (prover) <-> node
    p2 = com.Prover.from_code(CODE, NODE_ID, ver.params, ng, nn)
    n2 = ver.session(NODE_ID, ng, nn)
    sv, cv = n2.pake1(p2.share())
    n2.pake3(p2.finish(sv, cv))
    # run 1: commissioner (prover) <-> relay (verifier)
    p1 = com.Prover.from_code(CODE, NODE_ID, ver.params, ng, nn)
    n1 = ver.session(NODE_ID, ng, nn)
    sv, cv = n1.pake1(p1.share())
    n1.pake3(p1.finish(sv, cv))
    assert p1.session_id != n2.session_id
    proof = com.make_proof(node.keys, NODE_ID, ng, nn, n2.session_id, 1)  # what the node sends
    with pytest.raises(ProofError):  # the commissioner's check, in its run
        com.check_proof(proof, store, NODE_ID, ng, nn, p1.session_id, commit = False)
    com.check_proof(proof, store, NODE_ID, ng, nn, n2.session_id)  # in the node's own run it passes


def test_a_prover_can_start_from_w0_and_w1(verifier):
    ver = verifier
    w0, w1 = com.derive_w(CODE, NODE_ID, ver.params)
    ng, nn = com.new_nonce(), com.new_nonce()
    prover = com.Prover.from_w(w0, w1, NODE_ID, ver.params, ng, nn)
    node = ver.session(NODE_ID, ng, nn)
    sv, cv = node.pake1(prover.share())
    node.pake3(prover.finish(sv, cv))
    assert prover.keys == node.keys
    with pytest.raises(CommissionError):
        com.Prover.from_w(0, w1, NODE_ID, ver.params, ng, nn)
    with pytest.raises(com.ParamsError):  # the parameters are checked as in from_code
        com.Prover.from_w(w0, w1, NODE_ID, com.Params(ver.params.salt, log2n = 18), ng, nn)


# ---- setup code (V14) ----


def test_verhoeff_known_values():
    assert com.verhoeff_digit("236") == 3 and com.verhoeff_valid("2363") and not com.verhoeff_valid("2364")
    assert com.verhoeff_digit("") == 0 and com.verhoeff_digit("0") == 4 and com.verhoeff_valid("04")
    assert com.verhoeff_digit("12345") == 1 and com.verhoeff_digit("142857") == 0  # known: 123451 and 1428570
    for digits in vc.VERHOEFF_INPUTS:
        assert com.verhoeff_valid(digits + str(com.verhoeff_digit(digits)))


def test_verhoeff_finds_every_single_digit_error():
    for _ in range(30):
        code = com.generate_code()
        for pos, d in itertools.product(range(12), range(10)):
            if str(d) != code[pos]:
                assert not com.verhoeff_valid(code[:pos] + str(d) + code[pos + 1:])


def test_verhoeff_finds_every_swap_of_neighbours():
    for _ in range(60):
        code = com.generate_code()
        for pos in range(11):
            if code[pos] != code[pos + 1]:
                assert not com.verhoeff_valid(code[:pos] + code[pos + 1] + code[pos] + code[pos + 2:])


def test_code_generation():
    for _ in range(200):
        code = com.generate_code()
        assert len(code) == 12 and code.isdigit() and com.verhoeff_valid(code) and com.parse_code(code) == code
        assert not com._weak(code[:11])
    assert len({com.generate_code() for _ in range(50)}) == 50
    # leading zeros are drawn like any other digit
    assert com.generate_code(lambda n: 12345).startswith("00000012345")


def test_weak_codes_are_drawn_again():
    drawn = iter([0, 1234567890, 98765432109, 55555555555, 10**11 - 1, 12345678902])
    asked = []

    def randbelow(n):
        asked.append(n)
        return next(drawn)

    code = com.generate_code(randbelow)
    assert code[:11] == "12345678902" and len(asked) == 6 and asked[0] == 10**11
    for weak in ("00000000000", "01234567890", "98765432109", "55555555555", "99999999999", "89012345678",
                 "10987654321"):
        assert com._weak(weak), weak
    for fine in ("12345678902", "00000000001", "13579246801", "12121212121"):
        assert not com._weak(fine), fine


def test_code_text():
    assert com.format_code("123456789012") == "1234-5678-9012"
    code = com.generate_code()
    assert com.parse_code(com.format_code(code)) == code == com.parse_code(" " + code[:4] + " " + code[4:] + "\n")
    assert com.password(code) == code[:11].encode()
    for bad, msg in [("", "12 digits"), ("12345678901", "12 digits"), ("1234567890123", "12 digits"),
                     ("12345678901A", "12 digits"), ("12345678 9012x", "12 digits")]:
        with pytest.raises(CodeError, match = msg):
            com.parse_code(bad)
    wrong = code[:11] + str((int(code[11]) + 1) % 10)
    with pytest.raises(CodeError, match = "swapped"):
        com.parse_code(wrong)


def test_qr_round_trip_and_format():
    node_id, code, fp = vc.qr_cases()[0]
    text = com.qr_payload(node_id, code, fp)
    assert text == f"SN1:00A1B2C3D4E5F607:{code}:{fp.hex().upper()}" and len(text) == 66
    assert com.parse_qr(text) == com.parse_qr(f"  {text}\n") == com.QrData(node_id, code, fp)
    for node_id, code, fp in vc.qr_cases():
        assert com.parse_qr(com.qr_payload(node_id, code, fp)) == com.QrData(node_id, code, fp)
    for _ in range(20):
        nid, c, f = secrets.randbits(64) or 1, com.generate_code(), secrets.token_bytes(16)
        assert com.parse_qr(com.qr_payload(nid, c, f)) == com.QrData(nid, c, f)
    assert com.parse_qr(com.qr_payload(2**64 - 1, code, bytes(16))).node_id == 2**64 - 1
    for bad in (0, -1, 2**64, True * 0, "1", 1.5, None):
        with pytest.raises(CodeError):
            com.qr_payload(bad, code, bytes(16))
    with pytest.raises(CodeError):
        com.qr_payload(1, code, bytearray(16))
    with pytest.raises(CodeError):
        com.qr_payload(1, code, bytes(15))
    with pytest.raises(CodeError):
        com.qr_payload(1, code[:11] + str((int(code[11]) + 1) % 10), bytes(16))


@pytest.mark.parametrize("what, text", vc.bad_qr(), ids = [w for w, _ in vc.bad_qr()])
def test_qr_refusals(what, text):
    with pytest.raises(CodeError):
        com.parse_qr(text)


def test_qr_single_digit_and_swap_errors_in_the_code():
    node_id, code, fp = vc.qr_cases()[1]
    text = com.qr_payload(node_id, code, fp)
    start = text.index(code)
    for pos in range(12):
        d = str((int(code[pos]) + 3) % 10)
        with pytest.raises(CodeError):
            com.parse_qr(text[:start + pos] + d + text[start + pos + 1:])
    for pos in range(11):
        if code[pos] != code[pos + 1]:
            sw = code[:pos] + code[pos + 1] + code[pos] + code[pos + 2:]
            with pytest.raises(CodeError):
                com.parse_qr(text.replace(code, sw))


def test_label_text():
    assert com.label_lines(NODE_ID, "123456789012",
                           bytes.fromhex("a1b2c3d4e5f60718" + "00" * 8)) == ("SENSEI 00A1B2C3 D4E5F607",
                                                                             "Setup code 1234-5678-9012",
                                                                             "Key a1b2 c3d4 e5f6 0718")


# ---- key store: re-read for an unknown key id (finding 7) ----


class Rig:
    """Two key stores on one file (the collector and the GUI of the design), one backend key."""

    def __init__(self, tmp_path, key_file = None):
        self.path = tmp_path / "keys.json"
        self.pem = tmp_path / "b.pem"
        secure.backend_keygen(self.pem)
        if key_file:
            secure.new_key_file(key_file)
        self.key_file = key_file
        self.collector = KeyStore.load(self.path, private_pem = self.pem, key_file = key_file)
        self.collector.save()
        self.gui = KeyStore.load(self.path, private_pem = self.pem, key_file = key_file)
        self.reads = 0
        real = self.collector._read_file

        def counting():
            self.reads += 1
            return real()

        self.collector._read_file = counting

    def add_by_gui(self, node_id: int) -> NodeSession:
        priv, pub = secure.generate_keypair()
        self.gui.add_node(node_id, pub)
        self.gui.save()
        return NodeSession(priv, self.gui.backend_public_key())


@pytest.fixture
def clock(monkeypatch):
    t = [1000.0]
    monkeypatch.setattr(secure, "_monotonic", lambda: t[0])
    return t


@pytest.mark.parametrize("sealed", [False, True], ids = ["plain", "sealed"])
def test_a_node_added_by_another_process_is_found(tmp_path, clock, sealed):
    rig = Rig(tmp_path, tmp_path / "key" if sealed else None)
    assert rig.collector.sealed == sealed
    node = rig.add_by_gui(NODE_ID)
    frame = node.seal(secure.KIND_UPLINK, b"first data")
    assert not rig.collector.has_node(NODE_ID)
    assert secure.open_sealed(frame, rig.collector) == (NODE_ID, secure.KIND_UPLINK, b"first data")
    assert rig.collector.has_node(NODE_ID) and rig.collector.stats["reread"] == 1
    # the node is known now: no more reads
    before = rig.reads
    for _ in range(5):
        secure.open_sealed(node.seal(secure.KIND_UPLINK, b"more"), rig.collector)
    assert rig.reads == before


def test_peek_finds_the_node_as_well(tmp_path, clock):
    rig = Rig(tmp_path)
    node = rig.add_by_gui(NODE_ID)
    assert secure.peek_sealed(node.seal(secure.KIND_UPLINK, b"x"), rig.collector) == (secure.KIND_UPLINK, b"x")


def test_a_flood_of_unknown_ids_reads_the_file_once_a_second(tmp_path, clock):
    rig = Rig(tmp_path)
    ids = [secrets.randbits(32) or 1 for _ in range(300)]
    for key_id in ids:
        frame = secure.seal_raw(bytes(16), key_id, 1, secure.KIND_UPLINK, b"x")
        with pytest.raises(UnknownKeyError):
            secure.open_sealed(frame, rig.collector)
    assert rig.reads == 1
    clock[0] += 0.9
    with pytest.raises(UnknownKeyError):
        secure.open_sealed(frame, rig.collector)
    assert rig.reads == 1
    clock[0] += 0.2  # 1.1 s since the read
    with pytest.raises(UnknownKeyError):
        secure.open_sealed(frame, rig.collector)
    assert rig.reads == 2
    for _ in range(100):
        with pytest.raises(UnknownKeyError):
            secure.open_sealed(frame, rig.collector)
    assert rig.reads == 2 and rig.collector.stats["reread"] == 2


def test_a_node_added_inside_the_second_is_found_in_the_next(tmp_path, clock):
    rig = Rig(tmp_path)
    stranger = secure.seal_raw(bytes(16), 77, 1, secure.KIND_UPLINK, b"x")
    with pytest.raises(UnknownKeyError):
        secure.open_sealed(stranger, rig.collector)  # reads at t = 1000
    node = rig.add_by_gui(NODE_ID)
    frame = node.seal(secure.KIND_UPLINK, b"hello")
    clock[0] += 0.5
    with pytest.raises(UnknownKeyError):  # too early: the limit holds
        secure.open_sealed(frame, rig.collector)
    clock[0] += 0.6
    assert secure.open_sealed(frame, rig.collector)[2] == b"hello"  # the frame that failed is retried by the caller
    assert rig.reads == 2


def test_the_reread_does_not_wait_for_the_file_lock(tmp_path, clock):
    rig = Rig(tmp_path)
    node = rig.add_by_gui(NODE_ID)
    frame = node.seal(secure.KIND_UPLINK, b"x")
    done = []
    worker = threading.Thread(target = lambda: done.append(secure.open_sealed(frame, rig.collector)))
    with secure._flock(rig.path):  # another process saves: it holds the lock
        worker.start()
        worker.join(5)
        assert done and not worker.is_alive()


def test_a_damaged_file_is_not_a_crash(tmp_path, clock, caplog):
    rig = Rig(tmp_path)
    rig.path.write_text("{ not json")
    frame = secure.seal_raw(bytes(16), 5, 1, secure.KIND_UPLINK, b"x")
    with pytest.raises(UnknownKeyError):
        secure.open_sealed(frame, rig.collector)
    assert rig.reads == 1 and rig.collector.stats["reread"] == 0
    assert "re-read" in caplog.text
    with pytest.raises(UnknownKeyError):  # and the limit holds
        secure.open_sealed(frame, rig.collector)
    assert rig.reads == 1


def test_a_store_without_a_file_does_not_read(clock):
    store = KeyStore()
    with pytest.raises(UnknownKeyError):
        secure.open_sealed(secure.seal_raw(bytes(16), 5, 1, secure.KIND_UPLINK, b"x"), store)
    assert store.stats["reread"] == 0


def test_the_merge_keeps_what_the_collector_holds(tmp_path, clock):
    """The re-read merges like a save does: the known nodes, their counters and the downlink counters stay."""
    rig = Rig(tmp_path)
    old = rig.add_by_gui(NODE_ID)
    secure.open_sealed(old.seal(secure.KIND_UPLINK, b"a"), rig.collector)
    sealed = secure.seal_downlink(NODE_ID, secure.KIND_DOWNLINK, b"cmd", rig.collector)
    assert old.open(sealed) == (secure.KIND_DOWNLINK, b"cmd")
    new = rig.add_by_gui(NODE_ID + 1)
    clock[0] += 2  # the limit of the re-read
    secure.open_sealed(new.seal(secure.KIND_UPLINK, b"b"), rig.collector)
    with pytest.raises(ReplayError):
        secure.open_sealed(old.seal(secure.KIND_UPLINK, b"c", counter = 1), rig.collector)
    assert secure.open_sealed(old.seal(secure.KIND_UPLINK, b"d"), rig.collector)[2] == b"d"
    assert secure.seal_downlink(NODE_ID, secure.KIND_DOWNLINK, b"cmd2", rig.collector) != sealed


# ---- review of step A ----


def test_qr_node_id_zero_and_wrong_types():
    node_id, code, fp = vc.qr_cases()[0]
    text = com.qr_payload(node_id, code, fp)
    with pytest.raises(CodeError, match = "node id 0"):
        com.parse_qr(text.replace(f"{node_id:016X}", "0" * 16))
    for bad in (text.encode(), None, 5, [text], bytearray(text.encode())):
        with pytest.raises(CodeError):
            com.parse_qr(bad)
        with pytest.raises(CodeError):
            com.parse_code(bad)


def test_public_constructors_check_w0_and_w1(verifier):
    ctx = com.context(verifier.params, NG, NN)
    ids = (com.ID_PROVER, com.id_verifier(NODE_ID))
    for w0, w1 in [(0, 5), (5, 0), (p256.N, 5), (5, p256.N), (2**256, 5), (5, 2**300), (-1, 5), (1.5, 5)]:
        with pytest.raises(CommissionError):
            Prover(w0, w1, ctx, *ids, NODE_ID, NG, NN)
    good = verifier.w0
    for w0 in (bytes(32), p256.N.to_bytes(32, "big"), b"\xff" * 32, good[:31], good + b"\0", 5):
        with pytest.raises(CommissionError):
            com.VerifierSession(w0, verifier.l, ctx, *ids, NODE_ID, NG, NN)
    for l in (verifier.l[:64], b"\x04" + bytes(64), verifier.l[:64] + bytes([verifier.l[64] ^ 1])):
        with pytest.raises(CommissionError):
            com.VerifierSession(good, l, ctx, *ids, NODE_ID, NG, NN)
    com.VerifierSession(good, verifier.l, ctx, *ids, NODE_ID, NG, NN)


def test_the_peek_path_checks_the_node(provisioned):
    store, node, _ = provisioned
    other_priv, other_pub = secure.generate_keypair()
    store.add_node(NODE_ID + 1, other_pub)
    other = NodeSession(other_priv, store.backend_public_key())
    proof = com.make_proof(other.keys, NODE_ID + 1, NG, NN, SID, 1)
    com.check_proof(proof, store, NODE_ID + 1, NG, NN, SID, commit = False)
    # a proof of the other node, offered for this node id: refused on the key id, on both paths
    for commit in (True, False):
        with pytest.raises(ProofError):
            com.check_proof(proof, store, NODE_ID, NG, NN, SID, commit = commit)
    # the proof of the right node but the plaintext of another node id
    wrong = com.make_proof(node.keys, NODE_ID + 7, NG, NN, SID, 1)
    for commit in (True, False):
        with pytest.raises(ProofError, match = "does not match"):
            com.check_proof(wrong, store, NODE_ID, NG, NN, SID, commit = commit)


# V15 and the replay of the negatives through the real classes


def fixed_store(tmp_path) -> tuple[KeyStore, NodeSession]:
    """A store and a node with the fixed keys of the envelope vectors (vec_sec_*)."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    key = ec.derive_private_key(int.from_bytes(bytes(range(0x41, 0x61)), "big"), ec.SECP256R1())
    pem = tmp_path / "fixed.pem"
    pem.write_bytes(
        key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    store = KeyStore.load(tmp_path / "fixed.json", private_pem = pem)
    node_priv = bytes(range(1, 33))
    store.add_node(NODE_ID, secure.public_from_private(node_priv))
    return store, NodeSession(node_priv, store.backend_public_key())


def test_v15a_kind_5_under_the_static_keys_is_refused(tmp_path):
    store, node = fixed_store(tmp_path)
    assert node.keys == vc.fixed_keys()
    for receiver, env, expect, what in vc.static_kind5_cases():
        assert secure.parse_header(env).key_id == node.keys.key_id, what
        with pytest.raises(SecureError) as err:
            if receiver == 0:
                secure.open_sealed(env, store)
            else:
                node.open(env)
        assert not isinstance(err.value,
                              (ReplayError, UnknownKeyError)) and "authentication" not in str(err.value), what
        assert expect == -1
    # nothing was consumed: the real traffic still goes
    assert secure.open_sealed(node.seal(secure.KIND_UPLINK, b"x"), store)[2] == b"x"
    assert node.open(secure.seal_downlink(NODE_ID, secure.KIND_DOWNLINK, b"y", store)) == (secure.KIND_DOWNLINK, b"y")


def test_v15b_a_share_of_w0_m_is_refused_without_an_attempt():
    regs = vc.reg_cases()
    for i, share_p, expect, counts in vc.w0m_cases():
        s = regs[i].verifier.session(regs[i].node_id, NG, NN)
        with pytest.raises(InvalidPointError, match = "identity"):
            s.pake1(share_p)
        assert (expect, counts) == (-1, 0)
        assert len(share_p) == 65


def test_v15c_bad_verifier_records_are_refused():
    rows = vc.bad_records()
    assert len(rows) >= 20
    for what, data in rows:
        with pytest.raises(CommissionError):
            Verifier.decode(data)
    assert Verifier.decode(vc.reg_cases()[0].verifier.encode()) == vc.reg_cases()[0].verifier


def test_v15d_a_wrong_code_session():
    wc = vc.wrong_code_case()
    reg, ses = vc.reg_cases()[0], vc.ses_cases()[0]
    p = Prover.from_code(wc["code"], reg.node_id, reg.params, ses.nonce_g, ses.nonce_n, ses.x)
    s = reg.verifier.session(reg.node_id, ses.nonce_g, ses.nonce_n, ses.y)
    assert p.share() == wc["share_p"] != ses.values["share_p"]
    share_v, confirm_v = s.pake1(wc["share_p"])
    assert (share_v, confirm_v) == (wc["share_v"], wc["confirm_v"])
    with pytest.raises(WrongCodeError):  # the commissioner cannot confirm the node ...
        p.finish(share_v, confirm_v)
    with pytest.raises(WrongCodeError):  # ... and the node refuses the confirmation the commissioner computed
        s.pake3(wc["confirm_p"])


# L1: a node that the file holds with another key


def test_a_replaced_key_keeps_its_window_through_a_rollback(tmp_path, clock):
    rig = Rig(tmp_path)
    rig.collector.autosave_s = 1e9  # the collector does not save by itself: the file stays behind its memory
    rig.collector._last_save = secure._monotonic()
    priv1, pub1 = secure.generate_keypair()
    rig.gui.add_node(NODE_ID, pub1)
    old = NodeSession(priv1, rig.gui.backend_public_key())
    frames = [old.seal(secure.KIND_UPLINK, b"f%d" % i) for i in range(1, 6)]
    # 1. the collector finds the node by re-reading, then receives 5 frames that no save has written
    assert secure.open_sealed(frames[0], rig.collector)[2] == b"f1"
    for f in frames[1:]:
        secure.open_sealed(f, rig.collector)
    assert rig.collector._nodes[NODE_ID].up.highest == 5
    # 2. the GUI provisions the node again with another key (add_node), the node's first frame arrives
    priv2, pub2 = secure.generate_keypair()
    backup = rig.gui.add_node(NODE_ID, pub2)
    new = NodeSession(priv2, rig.gui.backend_public_key())
    clock[0] += 2
    assert secure.open_sealed(new.seal(secure.KIND_UPLINK, b"new key"), rig.collector)[2] == b"new key"
    assert rig.collector.stats["reread"] == 2
    kept = rig.collector._retired[old.keys.key_id]
    assert kept.up.highest == 5  # not lost
    # 3. the provisioning fails: rollback_node writes the old key back, with the (stale) window the GUI had
    rig.gui.rollback_node(backup)
    assert rig.gui._nodes[NODE_ID].up.highest < 5
    # 4. the old node is back in the collector at its next unknown id; the frames it already delivered stay refused
    clock[0] += 2
    with pytest.raises(ReplayError):
        secure.open_sealed(frames[4], rig.collector)
    for f in frames:
        with pytest.raises(ReplayError):
            secure.open_sealed(f, rig.collector)
    assert secure.open_sealed(old.seal(secure.KIND_UPLINK, b"f6"), rig.collector)[2] == b"f6"


def test_retire_merges_an_entry_of_the_same_key(tmp_path):
    store = KeyStore()
    _, pub = secure.generate_keypair()
    node = secure._Node(pub, 7, bytes(16), bytes(16), secure.ReplayWindow(5, 0b11), None, None, None, 0.0, 2)
    store._retired[7] = secure._Retired(pub, 7, secure.ReplayWindow(9, 0b1), 3, 4)
    before = dict(store._retired)
    store._retire(node, 11)
    r = store._retired[7]
    assert (r.up.highest, r.down_ctr, r.prov) == (9, 11, 4) and before[7].down_ctr == 3  # the old object is untouched


def test_secrets_are_not_in_the_repr():
    """A log line, a traceback or a failed assertion prints a repr: no w0, no key of the schedule, no channel key."""
    ver = com.make_verifier(CODE, NODE_ID, log2n = 14)
    assert ver.w0.hex() not in repr(ver)
    tt = b"transcript"
    sched = com.key_schedule(tt, bytes(65), bytes(65))
    for key in (sched.k_main, sched.k_confirm_p, sched.k_confirm_v, sched.k_shared):
        assert key.hex() not in repr(sched)
    keys = com.channel_keys(sched.k_shared, NODE_ID, NG, NN)
    assert keys.k_c2n.hex() not in repr(keys) and keys.k_n2c.hex() not in repr(keys)
