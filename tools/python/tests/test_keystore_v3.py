# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Key store file version 3: the mix (responder), promotion, previous chain, migration, merge, followers, the keys at
rest, and a full run against the node twin (design 1.3 to 1.5, 1.9)."""

from __future__ import annotations

import json
import logging
import random
import subprocess
import sys

import pytest
from fs_helpers import NODE_ID, make_rig
from sensei_sdk import ratchet, secure
from sensei_sdk.secure import KIND_DOWNLINK, KIND_UPLINK, KeyStore, NodeSession, RekeyAnswer, RekeyRequest, SecureError

KEY = f"{NODE_ID:016x}"


def stored(rig) -> dict:
    return json.loads(rig.path.read_text())["nodes"][KEY]


# ---- the mix, one generation ----


def test_one_mix_end_to_end(tmp_path):
    rig = make_rig(tmp_path)
    store, node = rig.store, rig.node
    for i in range(3):
        assert secure.open_sealed(rig.up(b"g0-%d" % i), store)[2] == b"g0-%d" % i
    assert store.generation(NODE_ID) == 0 and store.node_state(NODE_ID).static_keys
    late = rig.up(b"sealed in generation 0, sent late")
    ans = rig.mix()
    assert isinstance(ans, RekeyAnswer) and ans.gen == 1 and node.gen == 1
    st = store.node_state(NODE_ID)
    assert st.gen == 0 and st.next_gen == 1  # answered, not used yet
    assert stored(rig)["next"]["gen"] == 1  # on disk before the answer left
    # before the promotion the downlink still goes out in generation 0, and the node takes it (prev)
    d0 = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"down0", store)
    assert d0[1] == KIND_DOWNLINK and node.open(d0) == (KIND_DOWNLINK, b"down0")
    o = rig.promote(b"first of generation 1")
    assert (o.gen, o.promoted, o.plaintext) == (1, True, b"first of generation 1")
    st = store.node_state(NODE_ID)
    assert (st.gen, st.next_gen, st.prev_gen) == (1, None, 0) and st.static_keys  # generation 0 is kept for late ones
    on_disk = stored(rig)
    assert on_disk["cur"]["gen"] == 1 and on_disk["next"] is None and on_disk["prev_up"]["gen"] == 0
    assert on_disk["cur"]["root"] and on_disk["k_up"]  # R_1; the static key until the late messages are done
    # the late generation 0 message opens through prev_up, once
    assert secure.open_sealed(late, store)[2] == b"sealed in generation 0, sent late"
    with pytest.raises(secure.ReplayError):
        secure.open_sealed(late, store)
    # the downlink is now generation 1
    d1 = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"down1", store)
    assert d1[1] & 0x80 and node.open(d1) == (KIND_DOWNLINK, b"down1")
    # a late generation 0 frame that the window no longer holds, and a message with the keys of generation 0 after it
    for i in range(70):
        assert secure.open_sealed(rig.up(b"n%d" % i), store)[2] == b"n%d" % i
    store.flush()
    on_disk = stored(rig)
    assert on_disk["prev_up"] is None and on_disk["k_up"] is None and on_disk["k_down"] is None  # dropped
    legacy = secure.seal_raw(b"\0" * 16, node.keys.key_id, 10_000, KIND_UPLINK, b"x")
    with pytest.raises(secure.GenerationError):
        secure.open_sealed(legacy, store)


def test_second_mix_drops_the_chain_before_the_previous(tmp_path):
    rig = make_rig(tmp_path)
    rig.mix()
    rig.promote()
    rig.mix()
    rig.promote()
    st = rig.store.node_state(NODE_ID)
    assert (st.gen, st.prev_gen, st.static_keys) == (2, 1, False)
    rig.mix()
    rig.promote()
    st = rig.store.node_state(NODE_ID)
    assert (st.gen, st.prev_gen) == (3, 2)
    assert stored(rig)["prev_up"]["gen"] == 2


def test_responder_only_and_validation(tmp_path, monkeypatch):
    rig = make_rig(tmp_path, responder = False)
    req = rig.node.rekey_request()
    with pytest.raises(SecureError, match = "does not answer"):
        rig.store.rekey_answer(NODE_ID, req)
    monkeypatch.delenv("SENSEI_REKEY_RESPONDER", raising = False)
    assert KeyStore.load(rig.path).responder is False  # the first rollout step: off
    monkeypatch.setenv("SENSEI_REKEY_RESPONDER", "1")
    assert KeyStore.load(rig.path).responder is True
    store = KeyStore.load(rig.path, responder = True)
    secure.open_sealed(rig.up(b"x"), store)
    assert store.rekey_answer(NODE_ID, RekeyRequest(2, req.pub, req.up_base)) is None  # not the generation after cur
    assert store.rekey_answer(NODE_ID, RekeyRequest(0, req.pub, req.up_base)) is None
    assert store.rekey_answer(NODE_ID, RekeyRequest(1, req.pub, 0)) is None  # up_base 0 is not a valid start
    assert store.rekey_answer(NODE_ID, RekeyRequest(1, req.pub[:31], req.up_base)) is None
    assert store.rekey_answer(NODE_ID, RekeyRequest(1, bytes(32), req.up_base)) is None  # low-order public key
    assert store.rekey_answer(NODE_ID + 1, req) is None  # unknown node
    assert store.node_state(NODE_ID).next_gen is None  # nothing was stored
    assert store.rekey_answer(NODE_ID, req) is not None
    with pytest.raises(SecureError):
        KeyStore(rig.path, responder = True, follow = rig.path)


def test_the_same_request_gets_the_same_answer_after_every_reboot(tmp_path):
    """Review of Phase 3: a pending request is sent again with the same e_N after each reboot; it matches on (gen, up_base,
    e_N) and hits the stored next."""
    rig = make_rig(tmp_path)
    secure.open_sealed(rig.up(b"x"), rig.store)
    req = rig.node.rekey_request()
    first = rig.store.rekey_answer(NODE_ID, req)
    for _ in range(3):
        rig.node.reboot()
        again = rig.node.rekey_request()
        assert again == req
        assert rig.store.rekey_answer(NODE_ID, again) == first
        fresh = KeyStore.load(rig.path, responder = True)  # a restarted collector too
        assert fresh.rekey_answer(NODE_ID, again) == first
    assert rig.store.stats["rekey_answered"] == 1 and rig.store.stats["rekey_resent"] == 3
    # another e_N (the node dropped its state and asked again) replaces next
    other = RekeyRequest(1, ratchet.x25519_keygen()[1], req.up_base + 1)
    new = rig.store.rekey_answer(NODE_ID, other)
    assert new is not None and new != first and new.up_base == other.up_base
    assert rig.store.node_state(NODE_ID).next_gen == 1
    # the node of the old request cannot use it: its answer is gone, nothing from it opens
    assert not rig.node.rekey_answer(RekeyAnswer(1, new.pub, new.up_base))  # up_base does not match its request


def test_dropped_answers_then_data_keeps_flowing(tmp_path):
    rig = make_rig(tmp_path)
    req = rig.node.rekey_request()
    for i in range(5):  # the answer never arrives: both stay in generation 0
        assert secure.open_sealed(rig.up(b"p%d" % i), rig.store)[2] == b"p%d" % i
        ans = rig.store.rekey_answer(NODE_ID, rig.node.rekey_request())
        assert rig.store.generation(NODE_ID) == 0
    assert ans is not None and rig.node.rekey_answer(ans) and rig.node.gen == 1
    assert rig.promote().gen == 1
    assert req.gen == 1


def test_a_request_rides_the_first_uplink_of_the_next_generation(tmp_path):
    """Review of Phase 3: the gen+2 request can be in the first gen+1 uplink. The caller opens first (that promotes), then
    answers; answering before would be refused (the generation is not the one after cur)."""
    rig = make_rig(tmp_path)
    rig.mix()
    req2 = rig.node.rekey_request()  # the node asks for the next one at once (rekey_now)
    assert req2.gen == 2
    assert rig.store.rekey_answer(NODE_ID, req2) is None  # not promoted yet: ignored, logged
    o = rig.promote(b"with request")
    assert o.promoted
    ans2 = rig.store.rekey_answer(NODE_ID, req2)
    assert ans2 is not None and rig.node.rekey_answer(ans2)
    assert rig.promote(b"gen 2").gen == 2


def test_request_for_an_old_generation_after_promotion_is_ignored(tmp_path):
    rig = make_rig(tmp_path)
    secure.open_sealed(rig.up(b"x"), rig.store)
    req = rig.node.rekey_request()
    ans = rig.store.rekey_answer(NODE_ID, req)
    assert rig.node.rekey_answer(ans)
    rig.promote()
    assert rig.store.rekey_answer(NODE_ID, req) is None  # a late copy of the request
    assert rig.store.generation(NODE_ID) == 1


def test_crash_between_saving_next_and_sending(tmp_path):
    """The answer is on disk before it leaves: a collector that dies right after the save answers the repeat with the same
    key, and one that died before the save leaves the old state (the node asks again)."""
    rig = make_rig(tmp_path)
    secure.open_sealed(rig.up(b"x"), rig.store)
    req = rig.node.rekey_request()
    ans = rig.store.rekey_answer(NODE_ID, req)  # saved; "crash": the answer is never sent
    del rig.store
    revived = KeyStore.load(rig.path, responder = True)
    assert revived.node_state(NODE_ID).next_gen == 1
    again = revived.rekey_answer(NODE_ID, req)
    assert again == ans and rig.node.rekey_answer(again)
    assert secure.open_sealed_ex(rig.up(b"after"), revived).promoted

    # the save itself fails: nothing is stored, nothing is returned
    rig2 = make_rig(tmp_path / "two")
    secure.open_sealed(rig2.up(b"x"), rig2.store)
    req = rig2.node.rekey_request()
    real = secure._write_atomic

    def boom(path, text):
        raise OSError("disk full")

    secure._write_atomic = boom
    try:
        with pytest.raises(OSError):
            rig2.store.rekey_answer(NODE_ID, req)
    finally:
        secure._write_atomic = real
    assert rig2.store.node_state(NODE_ID).next_gen is None
    ans = rig2.store.rekey_answer(NODE_ID, req)
    assert ans is not None


def test_store_missing_next_while_the_node_is_at_g_plus_1(tmp_path, caplog):
    rig = make_rig(tmp_path)
    secure.open_sealed(rig.up(b"x"), rig.store)
    copy = tmp_path / "old-copy.json"
    copy.write_text(rig.path.read_text())  # a restored backup: from before the request
    rig.mix()
    old = KeyStore.load(copy, responder = True, private_pem = rig.pem)
    env = rig.up(b"gen 1")
    with caplog.at_level(logging.WARNING, logger = "sensei.secure"):
        with pytest.raises(secure.GenerationError, match = "provision"):
            secure.open_sealed(env, old)
    assert "node ahead of key store" in caplog.text and "re-provision" in caplog.text
    assert old.stats["node_ahead"] == 1
    # a node that is ahead by more than one generation looks the same
    rig.promote()
    rig.mix()
    rig.promote()
    with pytest.raises(secure.GenerationError):
        secure.open_sealed(rig.up(b"gen 2"), old)
    assert old.generation(NODE_ID) == 0


def test_add_node_resets_the_ratchet_and_keeps_counters(tmp_path):
    rig = make_rig(tmp_path)
    rig.mix()
    rig.promote()
    for _ in range(3):
        secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", rig.store)
    highest = rig.store.node_state(NODE_ID).highest
    # the node is provisioned again with the same key: a new session of the same node, counters kept
    again = NodeSession(rig.node_priv, rig.store.backend_public_key(), tx_counter = rig.node.tx_counter)
    rig.store.add_node(NODE_ID, again.node_pub)
    st = rig.store.node_state(NODE_ID)
    assert (st.gen, st.next_gen, st.prev_gen, st.static_keys) == (0, None, None, True)
    assert st.highest == highest and rig.store.down_counter(NODE_ID) == 3
    on_disk = stored(rig)
    assert on_disk["cur"]["gen"] == 0 and on_disk["k_up"] and on_disk["prev_up"] is None
    assert secure.open_sealed(again.seal(KIND_UPLINK, b"back in generation 0"), rig.store)[2] == b"back in generation 0"
    rig.store.add_node(NODE_ID, again.node_pub)  # nothing to reset any more
    assert rig.store.node_state(NODE_ID).highest > highest
    # a second process that still holds generation 1 does not bring it back
    stale = KeyStore.load(rig.path, responder = True)
    assert stale.generation(NODE_ID) == 0


# ---- file: migration, merge ----


def _v2_file(path, store, node_priv):
    """A key store file as the code before forward secrecy wrote it (version 2)."""
    data = json.loads(store.path.read_text())
    n = data["nodes"][KEY]
    keys = store._nodes[NODE_ID].keys
    v2 = {
        "node_pub": n["node_pub"],
        "key_id": n["key_id"],
        "k_up": keys.k_up.hex(),
        "k_down": keys.k_down.hex(),
        "up_highest": n["up_highest"],
        "up_window": n["up_window"],
        "down_ctr": 0,
    }
    data["version"] = 2
    data["nodes"][KEY] = v2
    data["retired"] = {"0000beef": dict(v2, key_id = 0xBEEF, node_pub = "04" + "11" * 64)}
    path.write_text(json.dumps(data))


def test_v2_file_migrates_to_v3_through_r0(tmp_path):
    rig = make_rig(tmp_path)
    for i in range(5):
        secure.open_sealed(rig.up(b"x"), rig.store)
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", rig.store)
    path = tmp_path / "v2.json"
    _v2_file(path, rig.store, None)
    old = json.loads(path.read_text())
    assert old["version"] == 2 and "cur" not in old["nodes"][KEY]
    store = KeyStore.load(path, private_pem = rig.pem, responder = True, clock = rig.clock)
    keys = rig.node.keys
    assert store._nodes[NODE_ID].cur.root == ratchet.root0(keys.k_up, keys.k_down, keys.key_id)
    assert store.generation(NODE_ID) == 0 and store.down_counter(NODE_ID) == 1
    assert secure.open_sealed(rig.up(b"after the migration"), store)[2] == b"after the migration"
    d = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"d", store)
    assert rig.node.open(d) == (KIND_DOWNLINK, b"d")
    new = json.loads(path.read_text())
    assert new["version"] == 3 and new["nodes"][KEY]["cur"]["gen"] == 0
    assert new["nodes"][KEY]["cur"]["root"] == ratchet.root0(keys.k_up, keys.k_down, keys.key_id).hex()
    retired = new["retired"]["0000beef"]
    assert "k_up" not in retired and "k_down" not in retired  # counters only
    # the migrated store mixes
    req = rig.node.rekey_request()
    ans = store.rekey_answer(NODE_ID, req)
    assert rig.node.rekey_answer(ans)
    assert secure.open_sealed_ex(rig.up(b"gen 1"), store).promoted
    # the code before forward secrecy refuses any version but 1 and 2, with "unsupported key store version 3"
    assert json.loads(path.read_text())["version"] == 3


def test_sealed_file_keeps_working_with_v3(tmp_path):
    keyf = tmp_path / "file.key"
    secure.new_key_file(keyf)
    rig = make_rig(tmp_path, key_file = keyf)
    rig.mix()
    rig.promote()
    text = rig.path.read_text()
    assert json.loads(text)["format"] == secure.SEALED_FORMAT and "root" not in text and "up_ck" not in text
    again = KeyStore.load(rig.path, responder = True, key_file = keyf)
    assert again.generation(NODE_ID) == 1
    assert secure.open_sealed(rig.up(b"sealed file"), again)[2] == b"sealed file"
    with pytest.raises(SecureError, match = "sealed"):
        KeyStore.load(rig.path)


def test_merge_rules(tmp_path):
    """Two copies of one file: the higher generation wins, in one generation the further chain, next only for cur + 1."""
    rig = make_rig(tmp_path)
    for i in range(3):
        secure.open_sealed(rig.up(b"x"), rig.store)
    other = KeyStore.load(rig.path, responder = True, clock = rig.clock)
    # the first store goes ahead: a mix, a promotion, messages
    rig.mix()
    rig.promote()
    for _ in range(200):
        secure.open_sealed(rig.up(b"y"), rig.store)
    rig.store.flush()
    # the other still has generation 0 and merges at its next save (a counter of its own)
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", other)
    assert other.generation(NODE_ID) == 1
    on_disk = stored(rig)
    assert on_disk["cur"]["gen"] == 1 and on_disk["next"] is None  # a stale next does not come back
    assert on_disk["cur"]["up_idx"] >= 2
    assert other.node_state(NODE_ID).up_idx == rig.store.node_state(NODE_ID).up_idx
    assert other.node_state(NODE_ID).highest == rig.store.node_state(NODE_ID).highest


def test_two_stores_one_file_promotion_by_either(tmp_path):
    """The collector and a GUI on one file: either may see the first uplink of the new generation; the other follows by
    its resync, and a message of the new generation opens in both."""
    rig = make_rig(tmp_path)
    secure.open_sealed(rig.up(b"x"), rig.store)
    gui = KeyStore.load(rig.path, clock = rig.clock)  # not a responder
    rig.mix()
    env = rig.up(b"gen 1 first")
    assert secure.open_sealed_ex(env, gui).promoted  # the GUI sees next in the file after its resync
    assert rig.store.generation(NODE_ID) == 0
    env2 = rig.up(b"gen 1 second")
    o = secure.open_sealed_ex(env2, rig.store)  # the collector: resync finds the promotion
    assert o.gen == 1 and rig.store.generation(NODE_ID) == 1


# ---- two processes, one request ----

_RACE = """
import sys, time, pathlib
from sensei_sdk import secure
path, go, node_id, pub, up_base = sys.argv[1:6]
store = secure.KeyStore.load(path, responder = True)
while not pathlib.Path(go).exists():
    time.sleep(0.001)
ans = store.rekey_answer(int(node_id), secure.RekeyRequest(1, bytes.fromhex(pub), int(up_base)))
print(ans.pub.hex(), ans.up_base, store.stats["rekey_answered"], store.stats["rekey_resent"])
"""


def test_two_processes_racing_one_request_give_one_answer(tmp_path):
    rig = make_rig(tmp_path)
    secure.open_sealed(rig.up(b"x"), rig.store)
    req = rig.node.rekey_request()
    go = tmp_path / "go"
    procs = [
        subprocess.Popen(
            [sys.executable, "-c", _RACE,
             str(rig.path),
             str(go),
             str(NODE_ID),
             req.pub.hex(),
             str(req.up_base)],
            stdout = subprocess.PIPE,
            text = True) for _ in range(4)
    ]
    go.write_text("go")
    outs = [p.communicate(timeout = 60)[0].split() for p in procs]
    assert all(p.returncode == 0 for p in procs)
    pubs = {o[0] for o in outs}
    assert len(pubs) == 1  # one ephemeral key, one answer
    assert sum(int(o[2]) for o in outs) == 1  # exactly one process made it
    assert sum(int(o[3]) for o in outs) == 3  # the others found it
    ans = RekeyAnswer(1, bytes.fromhex(pubs.pop()), req.up_base)
    assert rig.node.rekey_answer(ans)
    revived = KeyStore.load(rig.path, responder = True)
    assert secure.open_sealed_ex(rig.up(b"gen 1"), revived).promoted


# ---- followers ----


def test_follower_takes_over_cur_next_and_prev_without_secrets(tmp_path):
    rig = make_rig(tmp_path)
    secure.open_sealed(rig.up(b"x"), rig.store)
    gui_path = tmp_path / "gui" / "keys.json"
    gui = KeyStore.load(gui_path, private_pem = rig.pem, follow = rig.path, clock = rig.clock)
    gui.add_node(NODE_ID, rig.store._nodes[NODE_ID].node_pub)
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"gui", gui)  # its own counter and sender
    assert gui.sender != rig.store.sender
    ans = rig.mix()
    assert ans.gen == 1
    # the follower sees the answered mix (no root) and promotes by itself on the first uplink
    env = rig.up(b"gen 1")
    assert gui.node_state(NODE_ID).next_gen == 1
    assert gui._nodes[NODE_ID].next.root is None
    o = secure.open_sealed_ex(env, gui)
    assert o.promoted and gui.generation(NODE_ID) == 1
    assert gui._nodes[NODE_ID].cur.root is None
    data = json.loads(gui_path.read_text())["nodes"][KEY]
    assert data["cur"]["root"] is None and data["cur"]["gen"] == 1
    # the follower seals downlinks of the generation with its own counters and sender
    d = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"gui down", gui)
    assert d[1] & 0x80 and rig.node.open(d) == (KIND_DOWNLINK, b"gui down")
    assert gui.down_counter(NODE_ID) == 2 and rig.store.down_counter(NODE_ID) == 0
    # it cannot answer
    with pytest.raises(SecureError):
        gui.rekey_answer(NODE_ID, rig.node.rekey_request())
    # the collector promotes, the follower catches up with the file when it changes
    secure.open_sealed(rig.up(b"more"), rig.store)
    assert rig.store.generation(NODE_ID) == 1
    # the next mix: the follower knows it when the collector has answered
    rig.mix()
    rig.promote(b"gen 2 first")
    secure.open_sealed(rig.up(b"gen 2 more"), gui)
    assert gui.generation(NODE_ID) == 2
    # peek works in the follower and leaves its state
    before = gui.node_state(NODE_ID)
    kind, plain = secure.peek_sealed(rig.up(b"peek"), gui)
    assert plain == b"peek" and gui.node_state(NODE_ID).highest == before.highest


def test_follower_is_not_fooled_by_another_node_or_key(tmp_path):
    rig = make_rig(tmp_path)
    gui = KeyStore.load(tmp_path / "gui.json", private_pem = rig.pem, follow = rig.path, clock = rig.clock)
    priv, pub = secure.generate_keypair()
    gui.add_node(NODE_ID, pub)  # same node id, another key: not the followed node
    rig.mix()
    rig.up(b"x")
    gui._follow_refresh()
    assert gui.generation(NODE_ID) == 0 and gui.node_state(NODE_ID).next_gen is None


def test_non_following_copy_loses_access_at_the_mix(tmp_path):
    rig = make_rig(tmp_path)
    secure.open_sealed(rig.up(b"x"), rig.store)
    copy = tmp_path / "copy.json"
    copy.write_text(rig.path.read_text())
    lone = KeyStore.load(copy, private_pem = rig.pem, clock = rig.clock)
    rig.mix()
    rig.promote()
    with pytest.raises(secure.GenerationError):
        secure.open_sealed(rig.up(b"gen 1"), lone)
    assert secure.open_sealed(rig.up(b"gen 1"), rig.store)[2] == b"gen 1"


# ---- tolerance: the previous chain, late messages, the walk ----


def test_late_messages_around_the_switch(tmp_path):
    rig = make_rig(tmp_path)
    store, node = rig.store, rig.node
    secure.open_sealed(rig.up(b"x"), store)
    older = [rig.up(b"old%d" % i) for i in range(5)]  # sealed in generation 0, in flight
    rig.mix()
    first = rig.up(b"first")
    assert secure.open_sealed_ex(first, store).promoted
    # generation 0 messages sealed before the switch arrive after: all open, in any order, once
    for env in reversed(older):
        assert secure.open_sealed_ex(env, store).gen == 0
    for env in older:
        with pytest.raises(secure.ReplayError):
            secure.open_sealed(env, store)
    # a generation 0 counter beyond the end of generation 0 is not a message of the node
    forged = secure.seal_raw(node.keys.k_up, node.keys.key_id, node.tx_counter + 500, KIND_UPLINK, b"x")
    with pytest.raises(SecureError, match = "beyond the end"):
        secure.open_sealed(forged, store)


def test_first_counter_far_above_up_base_opens(tmp_path):
    """Review of Phase 3: the first counter of the new generation is the next epoch boundary after the switch, far above
    up_base when the request waited long (the node kept sending in the old generation). The time bound counts from the
    last verified uplink, not from the head of the chain."""
    rig = make_rig(tmp_path)
    secure.open_sealed(rig.up(b"x"), rig.store)
    req = rig.node.rekey_request()
    ans = rig.store.rekey_answer(NODE_ID, req)
    t = rig.clock.t
    for _ in range(12_000):  # 12000 epochs in generation 0 while the answer is on its way, no time passes
        rig.node.new_epoch()
        secure.open_sealed(rig.up(b"still gen 0"), rig.store)
    assert rig.clock.t == t
    assert rig.node.rekey_answer(ans)
    o = secure.open_sealed_ex(rig.up(b"gen 1"), rig.store)
    assert o.promoted
    idx = ratchet.epoch(rig.node.tx_counter) - req.up_base
    assert idx >= 12_000 and rig.store.node_state(NODE_ID).up_idx >= idx - 1
    # the same distance measured from a frame that is not backed by a verified uplink is refused
    rig2 = make_rig(tmp_path / "two")
    secure.open_sealed(rig2.up(b"x"), rig2.store)
    req2 = rig2.node.rekey_request()
    ans2 = rig2.store.rekey_answer(NODE_ID, req2)
    assert rig2.node.rekey_answer(ans2)
    rig2.node.tx_counter += 64 * 12_000  # the node sent 12000 epochs the key store never saw, in no time
    with pytest.raises(secure.GapError):
        secure.open_sealed(rig2.up(b"far"), rig2.store)
    assert rig2.store.generation(NODE_ID) == 0


def test_downlinks_around_promotion_and_the_previous_key(tmp_path):
    rig = make_rig(tmp_path)
    rig.mix()
    d_old1 = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"in 0", rig.store)  # generation 0, before the promotion
    d_old2 = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"in 0 too", rig.store)
    rig.promote()
    d_new = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"in 1", rig.store)
    assert d_old1[1] == KIND_DOWNLINK and d_new[1] & 0x80
    assert rig.node.open(d_new)[1] == b"in 1"
    assert rig.node.open(d_old1)[1] == b"in 0"  # the node keeps the previous key until its next request
    rig.node.rekey_request()  # drops it
    with pytest.raises(secure.GenerationError):
        rig.node.open(d_old2)
    d_new2 = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"still 1", rig.store)
    assert rig.node.open(d_new2)[1] == b"still 1"


# ---- the full run against the node twin ----


def test_full_mix_across_generations_with_losses(tmp_path):
    """12 generations, uplinks and downlinks all the time; answers dropped; late messages; reboots and epoch jumps; the
    collector restarts. Every message that is inside the window opens exactly once, with the right content."""
    rng = random.Random(2026)
    rig = make_rig(tmp_path)
    store, node = rig.store, rig.node
    delivered: dict[int, bytes] = {}  # counter -> plaintext, as opened
    late: list[tuple[bytes, bytes, int]] = []
    sent = 0
    downs: list[tuple[bytes, bytes]] = []

    def send(deliver: bool = True):
        nonlocal sent
        plain = b"m%d" % sent
        sent += 1
        env = rig.up(plain)
        counter = secure.parse_header(env).counter
        if deliver and rng.random() < 0.9:
            _, _, got = secure.open_sealed(env, store)
            assert got == plain
            delivered[counter] = plain
        else:
            late.append((env, plain, counter))
        return env

    def flush_late(keep: int = 0):
        rng.shuffle(late)
        while len(late) > keep:
            env, plain, counter = late.pop()
            hi = store.node_state(NODE_ID).highest
            try:
                _, _, got = secure.open_sealed(env, store)
            except secure.ReplayError:
                assert counter in delivered or counter <= hi - 64
            except SecureError as exc:
                assert counter <= hi - 63, f"{counter} (highest {hi}) refused: {exc}"
            else:
                assert got == plain and counter not in delivered
                delivered[counter] = plain

    for gen in range(1, 13):
        for _ in range(rng.randrange(70, 110)):  # the window moves past the previous generation before each mix
            send()
            if rng.random() < 0.1:
                node.new_epoch()
            if rng.random() < 0.05:
                node.reboot()
        if rng.random() < 0.3:  # a restart of the collector
            store.flush()
            store = KeyStore.load(rig.path, responder = True, clock = rig.clock)
        req = node.rekey_request()
        for _ in range(rng.randrange(0, 4)):  # the answer is lost; the node repeats, the collector resends
            send()
            assert store.rekey_answer(NODE_ID, node.rekey_request()) == store.rekey_answer(NODE_ID, req)
            node.reboot()
        ans = store.rekey_answer(NODE_ID, req)
        assert ans is not None
        for _ in range(rng.randrange(0, 5)):  # sealed before the switch, delivered after
            send(deliver = False)
        assert node.rekey_answer(ans) and node.gen == gen
        downs.append((secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"d%d" % gen, store), b"d%d" % gen))
        assert store.generation(NODE_ID) == gen - 1
        for _ in range(rng.randrange(1, 30)):
            send()
            if rng.random() < 0.3:
                flush_late(keep = rng.randrange(0, 3))
            if rng.random() < 0.2:
                d = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"live", store)
                assert node.open(d)[1] == b"live"
        flush_late()
        assert store.generation(NODE_ID) == gen
        # the downlink sealed just before the switch (in the old generation) is still good for the node
        env, plain = downs[-1]
        assert node.open(env)[1] == plain
    # a message of the first generation, long after: refused
    assert store.node_state(NODE_ID).static_keys is False
    assert store.generation(NODE_ID) == node.gen == 12
    assert len(delivered) > 200


def test_walker_state_survives_a_restart_and_a_long_outage(tmp_path):
    rig = make_rig(tmp_path)
    rig.mix()
    rig.promote()
    secure.open_sealed(rig.up(b"x"), rig.store)
    rig.store.flush()
    # the collector is down for 20 days; the node sends one epoch a minute
    rig.clock.advance(20 * 86400)
    for _ in range(20 * 24 * 60):
        rig.node.new_epoch()
    env = rig.up(b"after the outage")
    revived = KeyStore.load(rig.path, responder = True, clock = rig.clock)
    assert secure.open_sealed(env, revived)[2] == b"after the outage"


# ---- the mix messages and the node twin ----


def test_rekey_message_encoding_matches_the_node():
    """Uplink.rekey = 14 (tag 0x72): gen = 1, pub = 2 (32 B), up_base = 3, as sensei_e2e.c rekey_encode writes it."""
    pub = bytes(range(32))
    req = RekeyRequest(1, pub, 300)
    assert req.encode() == b"\x08\x01\x12\x20" + pub + b"\x18\xac\x02"
    assert req.encode_field() == b"\x72\x27" + req.encode()
    assert len(req.encode_field()) <= 48 + 2  # SENSEI_E2E_REKEY_MAX
    for m in (req, RekeyRequest(200, pub, 2**26 - 1), RekeyAnswer(5, pub, 1)):
        assert type(m).decode(m.encode()) == m
    with pytest.raises(SecureError):
        RekeyRequest.decode(b"\x08")
    with pytest.raises(SecureError):
        RekeyRequest.decode(b"\x12\x20abc")
    with pytest.raises(SecureError):
        RekeyRequest.decode(b"\x28\x01")


def test_node_twin_follows_the_device_rules(tmp_path):
    rig = make_rig(tmp_path)
    node = rig.node
    for _ in range(5):
        node.seal(KIND_UPLINK, b"x")
    req = node.rekey_request()
    assert req.gen == 1 and req.up_base == ratchet.epoch(5) + 1 and node.rekey_request() == req
    ans = rig.store.rekey_answer(NODE_ID, req)
    assert not node.rekey_answer(RekeyAnswer(2, ans.pub, ans.up_base))  # wrong generation
    assert not node.rekey_answer(RekeyAnswer(1, ans.pub, ans.up_base + 1))  # wrong up_base
    assert not node.rekey_answer(RekeyAnswer(1, ans.pub[:31], ans.up_base))  # wrong size
    with pytest.raises(ratchet.DhError):
        node.rekey_answer(RekeyAnswer(1, bytes(32), ans.up_base))
    assert node.gen == 0 and node.pending is not None  # nothing changed
    assert node.rekey_answer(ans) and not node.rekey_answer(ans)  # an answer that was handled is ignored
    # the first counter of the new generation is the next epoch boundary, never before the chain
    env = node.seal(KIND_UPLINK, b"g1")
    c = secure.parse_header(env).counter
    assert c == 64 * req.up_base and node.prev and node.prev_k_down is None
    node.new_epoch()
    assert secure.parse_header(node.seal(KIND_UPLINK, b"g1")).counter == 64 * (req.up_base + 1)
    node.reboot()
    assert secure.parse_header(node.seal(KIND_UPLINK, b"g1")).counter == 64 * (req.up_base + 2)
    # a request with the generation already used is not made again: the next one is for generation 2 with a later base
    assert node.rekey_request().gen == 2 and node.rekey_request().up_base > req.up_base
