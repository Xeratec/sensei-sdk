# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Forged messages and the cost of the chain walk (Decision M2 of the Phase 2 review): time-bound gap, cache of walked
keys, budgets, walk outside the store lock, and the legitimate long gap that must still open."""

from __future__ import annotations

import os
import threading
import time

import pytest
from fs_helpers import NODE_ID, Clock, make_rig, slack_walker
from sensei_sdk import chainwalk, ratchet, secure
from sensei_sdk.chainwalk import ChainState, Walker
from sensei_sdk.secure import GapError, KeyStore, NodeSession, ReplayError, SecureError, WalkBudgetError


def gen1_rig(tmp_path, **kw):
    """A node in generation 1 that the store has seen for a while (a few epochs in)."""
    rig = make_rig(tmp_path, **kw)
    rig.mix()
    rig.promote()
    for _ in range(300):
        secure.open_sealed(rig.up(b"warm"), rig.store)
    return rig


def forged(rig, counter: int, node: NodeSession | None = None) -> bytes:
    """A frame with the right header (key id, generation bits) and a wrong key."""
    node = node or rig.node
    return secure.seal_raw(os.urandom(16), node.keys.key_id, counter, secure.KIND_UPLINK, b"forged", gen = node.gen)


def far_counter(rig, epochs: int) -> int:
    return rig.store.node_state(NODE_ID).highest + 64 * epochs


def timed(fn) -> tuple[float, object]:
    t = time.perf_counter()
    try:
        fn()
    except SecureError as exc:
        return time.perf_counter() - t, exc
    return time.perf_counter() - t, None


# ---- (1) the time bound ----


def test_a_forged_far_counter_is_refused_cheaply(tmp_path):
    rig = gen1_rig(tmp_path)
    bad = forged(rig, far_counter(rig, (1 << 20) - 10))  # 2^20 epochs ahead: the old worst case
    steps = rig.store.walker.steps_total
    dt, exc = timed(lambda: secure.open_sealed(bad, rig.store))
    assert isinstance(exc, GapError) and not isinstance(exc, WalkBudgetError)
    assert dt < 0.02 and rig.store.walker.steps_total == steps  # no walk at all
    assert rig.store.stats["gap_refused"] == 1
    # the replay window and the chain did not move
    st = rig.store.node_state(NODE_ID)
    assert secure.open_sealed(rig.up(b"still fine"), rig.store)[2] == b"still fine"
    assert rig.store.node_state(NODE_ID).up_idx - st.up_idx <= 1
    # beyond the hard ceiling whatever the time
    rig.clock.advance(10 * 365 * 86400)
    exc = timed(lambda: secure.open_sealed(forged(rig, 0xFFFFFFFF), rig.store))[1]
    assert isinstance(exc, GapError) and "too large" in str(exc)


def test_the_gap_allowed_grows_with_time(tmp_path):
    rig = gen1_rig(tmp_path)
    walker = rig.store.walker
    st = rig.store._nodes[NODE_ID].cur.up
    assert walker.allowed_gap(st.t) == 4096
    rig.clock.advance(1000)
    assert walker.allowed_gap(st.t) == 5096
    rig.clock.advance(10**7)
    assert walker.allowed_gap(st.t) == ratchet.MAX_SKIP
    # a clock that goes back does not shrink it below the slack
    rig.clock.advance(-10**8)
    assert walker.allowed_gap(st.t) == 4096


def test_forged_in_the_slack_costs_at_most_the_slack(tmp_path):
    rig = gen1_rig(tmp_path)
    steps = rig.store.walker.steps_total
    exc = timed(lambda: secure.open_sealed(forged(rig, far_counter(rig, 4000)), rig.store))[1]
    assert isinstance(exc, SecureError) and "authentication" in str(exc)
    assert 3000 < rig.store.walker.steps_total - steps <= 4096


# ---- (2) the cache ----


def test_repeated_forgeries_hit_the_cache(tmp_path):
    rig = gen1_rig(tmp_path)
    rig.clock.advance(120_000)  # the time bound allows ~124000 epochs now
    walker = rig.store.walker
    st = rig.store._nodes[NODE_ID].cur.up
    s0 = walker.steps_total
    t1, exc = timed(lambda: secure.open_sealed(forged(rig, far_counter(rig, 100_000)), rig.store))
    assert isinstance(exc, SecureError) and "authentication" in str(exc)
    first = walker.steps_total - s0
    assert 99_000 < first <= 100_100
    assert walker.cached(st) == 100_000 // 1024  # a checkpoint every 2^10 epochs
    costs = []
    t_rep = 0.0
    for k in range(40):  # forgeries at other far counters: each starts at the last checkpoint
        s = walker.steps_total
        t, exc = timed(lambda k = k: secure.open_sealed(forged(rig, far_counter(rig, 100_000 - 7 * k)), rig.store))
        assert "authentication" in str(exc)
        costs.append(walker.steps_total - s)
        t_rep += t
    assert max(costs) <= 1 << chainwalk.CHECKPOINT_SHIFT
    assert t_rep / 40 < t1 / 20  # at least 20 times cheaper than the first walk
    # the cache holds keys that the stored chain key derives, and nothing behind it
    assert walker.cached(st) <= chainwalk.CHECKPOINTS_MAX
    ck, idx = st.ck, st.idx
    for k in sorted(st.cache):
        assert k > idx
        while idx < k:
            ck, idx = ratchet.next_chain_key(ck), idx + 1
        assert st.cache[k] == ck


def test_cache_follows_the_chain_and_keeps_nothing_behind_it(tmp_path):
    rig = gen1_rig(tmp_path)
    rig.clock.advance(50_000)
    st = rig.store._nodes[NODE_ID].cur.up
    exc = timed(lambda: secure.open_sealed(forged(rig, far_counter(rig, 20_000)), rig.store))[1]
    assert "authentication" in str(exc) and rig.store.walker.cached(st) == 19
    # the real node goes on past some checkpoints: the cache drops what is behind the new key
    rig.node.tx_counter += 64 * 10_000
    assert secure.open_sealed(rig.up(b"real"), rig.store)[2] == b"real"
    assert st.idx >= 10_000 - 1
    assert all(k > st.idx for k in st.cache) and 0 < len(st.cache) < 19


def test_cache_is_bounded_per_chain_and_lru_across_chains():
    clock = Clock()
    w = Walker(cache_entries = 12, clock = clock)
    chains = [ChainState(1, 1, 0, bytes([i]) * 32, t = 0.0) for i in range(4)]
    for i, st in enumerate(chains):  # 5 checkpoints each (idx 5000 passes 1024 .. 4096)
        w.walk(i, st, 5000)
        assert w.cached(st) == 4
    assert sum(w.cached(st) for st in chains) <= 12  # the oldest chain lost its cache
    assert w.cached(chains[0]) == 0 and w.cached(chains[3]) == 4
    # a chain that is used again is the youngest
    w.walk(1, chains[1], 5000)
    w.walk(9, ChainState(1, 1, 0, b"z" * 32, t = 0.0), 5000)
    assert w.cached(chains[1]) == 4
    # per chain: at most 1024 checkpoints = 32 kB of keys
    big = ChainState(1, 1, 0, b"y" * 32, t = 0.0)
    w2 = Walker(clock = clock,
                node_steps_min = 1 << 40,
                node_burst = 1 << 40,
                global_steps_min = 1 << 40,
                global_burst = 1 << 40)
    w2.walk(1, big, (1 << 20) - 1)
    assert w2.cached(big) == chainwalk.CHECKPOINTS_MAX - 1
    assert sum(len(k) for k in big.cache.values()) <= 32 * 1024


# ---- (3) the budgets ----


def test_node_budget_caps_the_steps_and_the_walk_goes_on(tmp_path):
    rig = gen1_rig(tmp_path)
    rig.clock.advance(3 * 86400)  # 263000 epochs are allowed in time
    walker = rig.store.walker
    s0 = walker.steps_total
    # forgeries at far gaps, as fast as they come
    errors = []
    for k in range(30):
        exc = timed(lambda k = k: secure.open_sealed(forged(rig, far_counter(rig, 200_000 + 1000 * k)), rig.store))[1]
        errors.append(type(exc).__name__)
    assert "WalkBudgetError" in errors
    assert walker.steps_total - s0 <= chainwalk.NODE_BURST  # no more than the burst for this node
    assert rig.store.stats["budget_refused"] >= 1
    # a minute later the node has 2^16 more steps; the walk went on from the checkpoints
    s1 = walker.steps_total
    rig.clock.advance(60)
    timed(lambda: secure.open_sealed(forged(rig, far_counter(rig, 230_000)), rig.store))
    assert 0 < walker.steps_total - s1 <= chainwalk.NODE_STEPS_MIN
    # short walks are free: the node's real traffic is never starved by the flood
    assert secure.open_sealed(rig.up(b"real traffic"), rig.store)[2] == b"real traffic"


def test_global_budget_covers_all_nodes(tmp_path):
    clock = Clock()
    walker = Walker(node_burst = 1 << 20,
                    node_steps_min = 1 << 20,
                    global_steps_min = 1 << 12,
                    global_burst = 1 << 15,
                    clock = clock)
    rig = make_rig(tmp_path, clock = clock, walker = walker)
    nodes = []
    for i in range(4):
        priv, pub = secure.generate_keypair()
        rig.store.add_node(100 + i, pub)
        node = NodeSession(priv, rig.store.backend_public_key())
        req = node.rekey_request()
        assert node.rekey_answer(rig.store.rekey_answer(100 + i, req))
        assert secure.open_sealed_ex(node.seal(secure.KIND_UPLINK, b"x"), rig.store).promoted
        nodes.append(node)
    clock.advance(3 * 86400)
    s0 = walker.steps_total
    for node in nodes:
        for k in range(5):
            timed(lambda node = node, k = k: secure.open_sealed(
                forged(rig, 64 * (200_000 + 5000 * k) + node.tx_counter, node), rig.store))
    assert walker.steps_total - s0 <= (1 << 15) + 1024  # the whole store, not per node
    assert rig.store.stats["budget_refused"] >= 10


# ---- (4) the legitimate long gap still opens ----


def test_legitimate_gap_after_an_outage_opens(tmp_path):
    rig = gen1_rig(tmp_path)
    # the collector was down for a day; the node sent 80000 epochs meanwhile (one a second, the design maximum)
    rig.clock.advance(86400)
    rig.node.tx_counter += 64 * 80_000
    t, exc = timed(lambda: secure.open_sealed(rig.up(b"back"), rig.store))
    assert exc is None
    assert rig.store.node_state(NODE_ID).up_idx >= 80_000
    # the same after a restart of the collector (the time of the last message is in the file)
    rig.store.flush()
    rig.clock.advance(3 * 86400)
    rig.node.tx_counter += 64 * 120_000
    again = KeyStore.load(rig.path, responder = True, clock = rig.clock)
    assert secure.open_sealed(rig.up(b"after the restart"), again)[2] == b"after the restart"


def test_gap_beyond_the_burst_opens_after_a_few_minutes(tmp_path):
    rig = gen1_rig(tmp_path)
    rig.clock.advance(86400 * 5)
    rig.node.tx_counter += 64 * 300_000  # 300000 epochs: more than the burst of a node
    env = rig.up(b"far")
    outcomes = []
    for _ in range(10):
        exc = timed(lambda: secure.open_sealed(env, rig.store))[1]
        outcomes.append(type(exc).__name__)
        if exc is None:
            break
        assert isinstance(exc, WalkBudgetError)  # refused, nothing consumed: the replay window is untouched
        rig.clock.advance(60)
    assert outcomes[-1] == "NoneType" and 1 <= len(outcomes) <= 5, outcomes
    assert rig.store.node_state(NODE_ID).up_idx >= 299_000
    with pytest.raises(ReplayError):
        secure.open_sealed(env, rig.store)


def test_a_node_that_just_spoke_cannot_be_far_ahead(tmp_path):
    rig = gen1_rig(tmp_path)
    rig.clock.advance(10)
    rig.node.tx_counter += 64 * 100_000  # 100000 epochs in 10 s: no node does that
    with pytest.raises(GapError):
        secure.open_sealed(rig.up(b"x"), rig.store)


# ---- (5) walk outside the store lock ----


def test_the_walk_runs_outside_the_store_lock(tmp_path):
    rig = gen1_rig(tmp_path)
    rig.clock.advance(5000)
    store = rig.store
    inside, release = threading.Event(), threading.Event()
    real = store.walker.walk

    def slow(*a, **kw):
        inside.set()
        assert release.wait(30)
        return real(*a, **kw)

    store.walker.walk = slow
    rig.node.tx_counter += 64 * 3000
    env = rig.up(b"a far one")
    counter = secure.parse_header(env).counter
    result = {}
    th = threading.Thread(target = lambda: result.update(r = _try(lambda: secure.open_sealed(env, store))))
    th.start()
    assert inside.wait(30)
    # the walk is running: the store is free for everybody else
    assert store._lock.acquire(timeout = 2)
    store._lock.release()
    store.walker.walk = real
    d = secure.seal_downlink(NODE_ID, secure.KIND_DOWNLINK, b"x", store)
    assert rig.node.open(d)[1] == b"x"
    release.set()
    th.join(30)
    assert result["r"][2] == b"a far one"
    assert store.node_state(NODE_ID).highest == counter


def test_commit_rechecks_after_the_walk(tmp_path):
    """While one message walks, a later one of the same node is opened: the first one is then too old and is refused at
    the commit (the replay check is done again), and the state stays what the later one made it."""
    rig = gen1_rig(tmp_path)
    rig.clock.advance(5000)
    store = rig.store
    inside, release = threading.Event(), threading.Event()
    real = store.walker.walk
    state = {"n": 0}

    def slow(*a, **kw):
        state["n"] += 1
        if state["n"] == 1:
            inside.set()
            assert release.wait(30)
        return real(*a, **kw)

    store.walker.walk = slow
    rig.node.tx_counter += 64 * 3000
    old = rig.up(b"old")
    rig.node.tx_counter += 64 * 5  # five epochs later
    new = rig.up(b"new")
    result = {}
    th = threading.Thread(target = lambda: result.update(r = _try(lambda: secure.open_sealed(old, store))))
    th.start()
    assert inside.wait(30)
    assert secure.open_sealed(new, store)[2] == b"new"
    release.set()
    th.join(30)
    assert isinstance(result["r"], ReplayError)
    assert store.node_state(NODE_ID).highest == secure.parse_header(new).counter


def _try(fn):
    try:
        return fn()
    except SecureError as exc:
        return exc


# ---- the hard ceiling, from the environment ----


def test_max_skip_from_the_environment(tmp_path, monkeypatch):
    monkeypatch.setenv("SENSEI_E2E_MAX_SKIP", "100")
    rig = make_rig(tmp_path)
    assert rig.store.walker.max_skip == 100
    rig.mix()
    rig.promote()
    rig.clock.advance(10**6)
    rig.node.tx_counter += 64 * 101
    with pytest.raises(GapError, match = "too large"):
        secure.open_sealed(rig.up(b"x"), rig.store)


# ---- timings, before and after (run with -s to see them) ----


def test_timings_before_and_after(tmp_path, capsys):
    """Before Decision M2 a forged uplink at the far end of MAX_SKIP cost a full walk (about 1.4 s here); now it is refused
    without one, a flood at the edge of the time bound costs the first walk and then one checkpoint step each."""
    after = gen1_rig(tmp_path / "after")
    before = gen1_rig(tmp_path / "before", walker = slack_walker(Clock()))
    before.store.walker.clock = before.clock
    bad_after = forged(after, far_counter(after, (1 << 20) - 10))
    bad_before = forged(before, far_counter(before, (1 << 20) - 10))
    t_before, exc_before = timed(lambda: secure.open_sealed(bad_before, before.store))
    t_after, exc_after = timed(lambda: secure.open_sealed(bad_after, after.store))
    assert "authentication" in str(exc_before) and isinstance(exc_after, GapError)
    # a flood of 100 forgeries at the edge of what the time bound allows
    flood = gen1_rig(tmp_path / "flood")
    flood.clock.advance(60_000)
    ts = [
        timed(lambda k = k: secure.open_sealed(forged(flood, far_counter(flood, 60_000 - k)), flood.store))[0]
        for k in range(100)
    ]
    with capsys.disabled():
        print(f"\nforged uplink at +2^20 epochs: before {t_before * 1000:.0f} ms, after {t_after * 1000:.2f} ms;"
              f" flood at the edge of the time bound: first {ts[0] * 1000:.0f} ms, then median"
              f" {sorted(ts)[50] * 1000:.2f} ms, max {max(ts[1:]) * 1000:.1f} ms")
    assert t_before > 0.2 and t_after < 0.02
    assert sorted(ts)[50] < 0.01
