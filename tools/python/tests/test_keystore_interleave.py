# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""A message is opened while the state moves under it (review of Phase 4): the walk runs outside the store lock, so a
promotion, a merge with the file or the refresh of a follower can come in between. The commit must notice and open the
message again against the new state."""

from __future__ import annotations

import json
import threading

import pytest
from fs_helpers import NODE_ID, make_rig
from sensei_sdk import ratchet, secure
from sensei_sdk.secure import KIND_DOWNLINK, KeyStore


def two_frames(rig):
    """After an answered mix (the store is still in generation 0): the first frame of generation 1 (near), which promotes,
    and a later one (300 epochs on) that needs a walk."""
    rig.mix()
    near = rig.up(b"near")
    rig.node.tx_counter += 64 * 300
    far = rig.up(b"far")
    return near, far, secure.parse_header(far).counter


def during_the_first_walk(store, action):
    """Run `action` once, inside the first walk of `store` (outside its lock); returns the list of walk calls."""
    real = store.walker.walk
    calls: list = []

    def walk(*a, **kw):
        calls.append(a[1].gen)
        if len(calls) == 1:
            action()
        return real(*a, **kw)

    store.walker.walk = walk
    return calls


def test_a_walk_interleaved_with_a_promotion_is_opened_again(tmp_path):
    rig = make_rig(tmp_path)
    store = rig.store
    near, far, counter = two_frames(rig)
    calls = during_the_first_walk(store, lambda: secure.open_sealed_ex(near, store))
    o = secure.open_sealed_ex(far, store)
    assert o.plaintext == b"far" and o.gen == 1 and not o.promoted  # the other one promoted; this one is a plain message
    assert len(calls) == 3  # the far frame (answered mix), the near frame, the far frame again (current chain)
    assert store.stats["promoted"] == 1 and store.generation(NODE_ID) == 1
    st = store.node_state(NODE_ID)
    assert st.highest == counter and st.up_idx >= ratchet.epoch(counter) - st.up_base - 1
    store.flush()
    on_disk = json.loads(rig.path.read_text())["nodes"][f"{NODE_ID:016x}"]
    assert on_disk["cur"]["gen"] == 1 and on_disk["next"] is None
    with pytest.raises(secure.ReplayError):
        secure.open_sealed(near, store)
    assert secure.open_sealed(rig.up(b"after"), store)[2] == b"after"


@pytest.mark.parametrize("merge", ["downlink", "save"])
def test_a_walk_interleaved_with_a_merge(tmp_path, merge):
    """Another process (same file) promotes while this one walks. `downlink`: this process merges the file meanwhile (a
    downlink takes the file lock and merges); `save`: it does not, and finds the promotion at its own save."""
    rig = make_rig(tmp_path)
    near, far, counter = two_frames(rig)
    a = rig.store
    b = KeyStore.load(rig.path, responder = True, clock = rig.clock)

    def other_process():
        assert secure.open_sealed_ex(near, b).promoted
        b.flush()
        if merge == "downlink":
            secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", a)

    calls = during_the_first_walk(a, other_process)
    o = secure.open_sealed_ex(far, a)
    assert o.plaintext == b"far" and o.gen == 1
    assert len(calls) == (1 if merge == "save" else 2
                         )  # `save`: promotes by itself; `downlink`: the state changed, walks again
    assert a.generation(NODE_ID) == 1
    a.flush()
    on_disk = json.loads(rig.path.read_text())["nodes"][f"{NODE_ID:016x}"]
    assert on_disk["cur"]["gen"] == 1 and on_disk["next"] is None
    assert on_disk["up_highest"] == counter  # the counters of both processes
    assert on_disk["cur"]["up_idx"] >= ratchet.epoch(counter) - on_disk["cur"]["up_base"] - 1
    with pytest.raises(secure.ReplayError):  # a's window has what b saw
        secure.open_sealed(near, a)
    for store in (a, KeyStore.load(rig.path, responder = True, clock = rig.clock)):
        assert secure.open_sealed(rig.up(b"after"), store)[2] == b"after"


def test_a_follower_that_refreshes_during_an_open(tmp_path):
    rig = make_rig(tmp_path)
    follower = KeyStore.load(tmp_path / "gui.json", private_pem = rig.pem, follow = rig.path, clock = rig.clock)
    follower.add_node(NODE_ID, rig.store._nodes[NODE_ID].node_pub)
    near, far, counter = two_frames(rig)
    assert follower.node_state(NODE_ID).next_gen == 1

    def collector_promotes_and_the_gui_looks():
        assert secure.open_sealed_ex(near, rig.store).promoted
        rig.store.flush()
        assert follower.node_state(NODE_ID).gen == 1  # the refresh imports the promotion in the middle of the open

    calls = during_the_first_walk(follower, collector_promotes_and_the_gui_looks)
    o = secure.open_sealed_ex(far, follower)
    assert o.plaintext == b"far" and o.gen == 1 and not o.promoted
    assert len(calls) == 2
    n = follower._nodes[NODE_ID]
    assert n.cur.gen == 1 and n.cur.root is None and n.next is None
    assert follower.node_state(NODE_ID).highest == counter
    assert secure.open_sealed(rig.up(b"after"), follower)[2] == b"after"


def test_a_follower_read_by_another_thread_while_frames_open(tmp_path):
    """node_state and generation of a follower run on a GUI thread while the pipeline opens frames and the collector
    promotes: they take the store lock, so the import never runs in the middle of a commit."""
    rig = make_rig(tmp_path)
    follower = KeyStore.load(tmp_path / "gui.json", private_pem = rig.pem, follow = rig.path, clock = rig.clock)
    follower.add_node(NODE_ID, rig.store._nodes[NODE_ID].node_pub)
    stop = threading.Event()
    errors: list = []
    seen: list = []

    def gui():
        while not stop.is_set():
            try:
                st = follower.node_state(NODE_ID)
                seen.append((st.gen, follower.generation(NODE_ID)))
            except Exception as exc:  # pragma: no cover - the failure this test is for
                errors.append(exc)
                return

    th = threading.Thread(target = gui, daemon = True)
    th.start()
    try:
        for g in range(3):
            rig.mix()
            for i in range(120):
                env = rig.up(b"m%d" % i)
                secure.open_sealed(env, rig.store)
                if i % 10 == 0:
                    rig.store.flush()
                if i % 3 == 0:
                    secure.open_sealed(rig.up(b"f%d" % i), follower)
    finally:
        stop.set()
        th.join(30)
    assert not errors and seen
    assert rig.store.generation(NODE_ID) == 3
