# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Review of Phase 4: provisioning again (`prov`, `add_node(reset=...)`, `rollback_node`), a replaced answer, sender ids,
the lock of the read functions and a promotion that cannot be saved."""

from __future__ import annotations

import json
import threading

import pytest
from fs_helpers import NODE_ID, make_rig
from sensei_sdk import secure
from sensei_sdk.secure import KIND_UPLINK, KeyStore, NodeSession

KEY = f"{NODE_ID:016x}"


def stored(rig) -> dict:
    return json.loads(rig.path.read_text())["nodes"][KEY]


def reprovision(rig, store = None, **kw):
    """The node is provisioned again with the same key pair: a new session, counters kept. Returns it and the backup."""
    again = NodeSession(rig.node_priv, rig.store.backend_public_key(), tx_counter = rig.node.tx_counter)
    backup = (store or rig.store).add_node(NODE_ID, again.node_pub, **kw)
    return again, backup


def at_gen1(tmp_path):
    """A collector (the rig's store), a second store on the same file and a follower, all at generation 1."""
    rig = make_rig(tmp_path)
    rig.mix()
    rig.promote()
    other = KeyStore.load(rig.path, responder = True, clock = rig.clock)
    follower = KeyStore.load(tmp_path / "gui.json", private_pem = rig.pem, follow = rig.path, clock = rig.clock)
    follower.add_node(NODE_ID, rig.store._nodes[NODE_ID].node_pub)
    for store in (rig.store, other):
        secure.open_sealed(rig.up(b"more"), store)
    assert (rig.store.generation(NODE_ID), other.generation(NODE_ID), follower.generation(NODE_ID)) == (1, 1, 1)
    return rig, other, follower


# ---- H1: the reset of add_node is not undone by another process ----


def test_add_node_reset_is_seen_by_a_second_store_and_a_follower(tmp_path):
    rig, other, follower = at_gen1(tmp_path)
    again, backup = reprovision(rig)
    assert backup.changed and rig.store.generation(NODE_ID) == 0
    assert stored(rig)["prov"] == 1
    # the second store still holds generation 1 in memory: its next save takes the reset over, it does not write
    # generation 1 back
    other.save()
    assert stored(rig)["cur"]["gen"] == 0 and other.generation(NODE_ID) == 0
    # the collector refuses nothing of the node in generation 0
    assert secure.open_sealed(again.seal(KIND_UPLINK, b"g0 after provisioning"), other)[2] == b"g0 after provisioning"
    # the provisioning process does not return to generation 1 at its own save and merge
    rig.store._resync_at = -1e9
    rig.store.save()
    assert rig.store.generation(NODE_ID) == 0 and stored(rig)["cur"]["gen"] == 0
    # the follower takes the reset over from the file, with no root key
    assert follower.generation(NODE_ID) == 0
    assert follower.node_state(NODE_ID).static_keys and follower.node_state(NODE_ID).prev_gen is None
    # and goes on with the next mix of the node as before (the same prov: the generation decides)
    req = again.rekey_request()
    ans = other.rekey_answer(NODE_ID, req)
    assert again.rekey_answer(ans)
    assert secure.open_sealed_ex(again.seal(KIND_UPLINK, b"gen 1 again"), other).promoted
    assert follower.generation(NODE_ID) == 1
    assert secure.open_sealed(again.seal(KIND_UPLINK, b"for the follower"), follower)[2] == b"for the follower"


def test_a_store_that_holds_unsaved_progress_loses_it_to_the_reset(tmp_path):
    """The collector keeps running in generation 1 while the node is provisioned elsewhere: whatever it did in memory since
    its last save is dropped at its next save (the node is in generation 0, those frames cannot come any more)."""
    rig, other, follower = at_gen1(tmp_path)
    other.autosave_s = 1e9  # no save by itself
    for i in range(5):
        secure.open_sealed(rig.up(b"progress %d" % i), other)
    highest = other.node_state(NODE_ID).highest
    again, _ = reprovision(rig)
    other.save()
    assert other.generation(NODE_ID) == 0 and stored(rig)["cur"]["gen"] == 0
    assert other.node_state(NODE_ID).highest >= highest  # the counters only go up
    # the restart of the collector finds generation 0, the node's frames open
    revived = KeyStore.load(rig.path, responder = True, clock = rig.clock)
    assert secure.open_sealed(again.seal(KIND_UPLINK, b"x"), revived)[2] == b"x"


def test_a_follower_that_provisions_the_node_itself_keeps_the_reset(tmp_path):
    rig, other, follower = at_gen1(tmp_path)
    again = NodeSession(rig.node_priv, rig.store.backend_public_key(), tx_counter = rig.node.tx_counter)
    follower.add_node(NODE_ID, again.node_pub)
    assert follower.generation(NODE_ID) == 0
    secure.open_sealed(rig.up(b"the collector goes on"), rig.store)
    rig.store.flush()  # the followed file changes
    assert follower.generation(NODE_ID) == 0  # its copy has the higher prov: the file does not bring generation 1 back
    assert secure.open_sealed(again.seal(KIND_UPLINK, b"g0"), follower)[2] == b"g0"


def test_remove_and_add_again_goes_on_above_the_old_prov(tmp_path):
    rig, other, follower = at_gen1(tmp_path)
    again, _ = reprovision(rig)  # prov 1
    rig.store.remove_node(NODE_ID)
    rig.store.add_node(NODE_ID, again.node_pub)  # the same key back: prov 2, counters kept
    assert stored(rig)["prov"] == 2
    other.save()
    assert other.generation(NODE_ID) == 0 and stored(rig)["prov"] == 2


def test_prov_is_saved_and_old_files_read_as_zero(tmp_path):
    rig = make_rig(tmp_path)
    assert stored(rig)["prov"] == 0
    data = json.loads(rig.path.read_text())
    del data["nodes"][KEY]["prov"]
    rig.path.write_text(json.dumps(data))
    assert KeyStore.load(rig.path)._nodes[NODE_ID].prov == 0


def test_add_node_without_reset_leaves_a_known_node_alone(tmp_path):
    rig = make_rig(tmp_path)
    rig.mix()
    rig.promote()
    before = stored(rig)
    backup = rig.store.add_node(NODE_ID, rig.node.node_pub, reset = False)
    assert not backup.changed and stored(rig) == before and rig.store.generation(NODE_ID) == 1
    rig.store.rollback_node(backup)  # nothing to undo
    assert stored(rig) == before
    # a node that is new, or has another key, is added in any case
    priv, pub = secure.generate_keypair()
    assert rig.store.add_node(NODE_ID + 1, pub, reset = False).changed
    priv2, pub2 = secure.generate_keypair()
    assert rig.store.add_node(NODE_ID, pub2, reset = False).changed and rig.store.generation(NODE_ID) == 0


# ---- the rollback of a failed provisioning ----


def test_rollback_brings_the_old_state_back_everywhere(tmp_path):
    rig, other, follower = at_gen1(tmp_path)
    old_state = rig.store.node_state(NODE_ID)
    again, backup = reprovision(rig)
    assert rig.store.generation(NODE_ID) == 0
    other.save()
    assert other.generation(NODE_ID) == 0 and follower.generation(NODE_ID) == 0
    # something of generation 0 was received meanwhile: the counters stay counted
    secure.open_sealed(again.seal(KIND_UPLINK, b"during provisioning"), rig.store)
    highest = rig.store.node_state(NODE_ID).highest
    rig.node.tx_counter = again.tx_counter + 64  # the old node goes on above it
    rig.store.rollback_node(backup)  # the provisioning RPC failed: the node still has its old keys
    st = rig.store.node_state(NODE_ID)
    assert (st.gen, st.static_keys, st.up_idx) == (old_state.gen, old_state.static_keys, old_state.up_idx)
    assert st.highest >= highest
    assert stored(rig)["prov"] == 2 and stored(rig)["cur"]["gen"] == 1
    # the second store and the follower follow the rollback; nobody writes generation 0 back
    other.save()
    assert other.generation(NODE_ID) == 1 and follower.generation(NODE_ID) == 1
    assert secure.open_sealed(rig.up(b"the old node is back"), other)[2] == b"the old node is back"
    rig.store._resync_at = -1e9
    rig.store.save()
    assert stored(rig)["cur"]["gen"] == 1
    # the collector that rebooted opens the old node as well, and its counters are above what was seen meanwhile
    revived = KeyStore.load(rig.path, responder = True, clock = rig.clock)
    assert secure.open_sealed(rig.up(b"after restart"), revived)[2] == b"after restart"
    assert revived.node_state(NODE_ID).highest > highest


def test_rollback_of_a_new_node_and_of_a_new_key(tmp_path):
    rig = make_rig(tmp_path)
    priv, pub = secure.generate_keypair()
    backup = rig.store.add_node(NODE_ID + 7, pub)
    assert rig.store.has_node(NODE_ID + 7) and backup.old is None
    rig.store.rollback_node(backup)
    assert not rig.store.has_node(NODE_ID + 7) and f"{NODE_ID + 7:016x}" not in json.loads(
        rig.path.read_text())["nodes"]
    # the node of the rig gets another key; the rollback gives the old key (and its generation) back
    rig.mix()
    rig.promote()
    priv2, pub2 = secure.generate_keypair()
    backup = rig.store.add_node(NODE_ID, pub2)
    assert rig.store.generation(NODE_ID) == 0 and rig.store.key_id(NODE_ID) != rig.node.keys.key_id
    rig.store.rollback_node(backup)
    assert rig.store.key_id(NODE_ID) == rig.node.keys.key_id and rig.store.generation(NODE_ID) == 1
    assert secure.open_sealed(rig.up(b"still the old key"), rig.store)[2] == b"still the old key"
    revived = KeyStore.load(rig.path, responder = True, clock = rig.clock)
    assert revived.key_id(NODE_ID) == rig.node.keys.key_id and revived.generation(NODE_ID) == 1


def test_a_failed_save_of_add_node_changes_nothing(tmp_path, monkeypatch):
    rig = make_rig(tmp_path)
    rig.mix()
    rig.promote()
    before = stored(rig)

    def boom(path, text):
        raise OSError("disk full")

    monkeypatch.setattr(secure, "_write_atomic", boom)
    with pytest.raises(OSError):
        reprovision(rig)
    monkeypatch.undo()
    assert rig.store.generation(NODE_ID) == 1 and rig.store.key_id(NODE_ID) == rig.node.keys.key_id
    assert stored(rig) == before
    assert secure.open_sealed(rig.up(b"x"), rig.store)[2] == b"x"


# ---- M1: the newer answer wins ----


def test_a_replaced_answer_is_the_one_that_is_stored(tmp_path):
    rig = make_rig(tmp_path)
    secure.open_sealed(rig.up(b"x"), rig.store)
    req = rig.node.rekey_request()
    first = rig.store.rekey_answer(NODE_ID, req)
    rig.node.pending = None  # the node dropped its state and asks again, with a new e_N
    req2 = rig.node.rekey_request()
    assert req2.pub != req.pub
    ans2 = rig.store.rekey_answer(NODE_ID, req2)
    assert ans2.pub != first.pub
    nxt = rig.store._nodes[NODE_ID].next
    assert (nxt.e_c_pub, nxt.e_n_pub) == (ans2.pub, req2.pub)  # memory
    on_disk = stored(rig)["next"]
    assert (on_disk["e_c_pub"], on_disk["e_n_pub"]) == (ans2.pub.hex(), req2.pub.hex())  # file
    fresh = KeyStore.load(rig.path, responder = True, clock = rig.clock)
    assert fresh._nodes[NODE_ID].next.e_c_pub == ans2.pub  # a restart
    assert fresh.rekey_answer(NODE_ID, req2) == ans2
    # the node switches with the answer it got, and its first frame of generation 1 opens and promotes
    assert rig.node.rekey_answer(ans2)
    o = secure.open_sealed_ex(rig.up(b"gen 1"), rig.store)
    assert o.promoted and o.plaintext == b"gen 1"


def test_a_replaced_answer_wins_the_merge_with_an_older_one(tmp_path):
    rig = make_rig(tmp_path)
    secure.open_sealed(rig.up(b"x"), rig.store)
    old = KeyStore.load(rig.path, responder = True, clock = rig.clock)  # a second process
    req = rig.node.rekey_request()
    first = old.rekey_answer(NODE_ID, req)  # it answers first
    rig.node.pending = None
    req2 = rig.node.rekey_request()
    ans2 = rig.store.rekey_answer(NODE_ID, req2)  # the other one replaces the answer
    assert stored(rig)["next"]["e_c_pub"] == ans2.pub.hex()
    old.save()  # the first process holds the older answer and saves: it does not take the file's back
    assert stored(rig)["next"]["e_c_pub"] == ans2.pub.hex()
    assert old._nodes[NODE_ID].next.e_c_pub == ans2.pub and old.rekey_answer(NODE_ID, req2) == ans2
    assert first.pub != ans2.pub
    assert rig.node.rekey_answer(ans2)
    assert secure.open_sealed_ex(rig.up(b"gen 1"), old).promoted


def test_a_follower_takes_the_replaced_answer(tmp_path):
    rig = make_rig(tmp_path)
    secure.open_sealed(rig.up(b"x"), rig.store)
    follower = KeyStore.load(tmp_path / "gui.json", private_pem = rig.pem, follow = rig.path, clock = rig.clock)
    follower.add_node(NODE_ID, rig.store._nodes[NODE_ID].node_pub)
    req = rig.node.rekey_request()
    rig.store.rekey_answer(NODE_ID, req)
    assert follower.node_state(NODE_ID).next_gen == 1
    rig.node.pending = None
    ans2 = rig.store.rekey_answer(NODE_ID, rig.node.rekey_request())
    follower.node_state(NODE_ID)
    assert follower._nodes[NODE_ID].next.e_c_pub == ans2.pub
    assert rig.node.rekey_answer(ans2)
    assert secure.open_sealed_ex(rig.up(b"gen 1"), follower).promoted


# ---- sender ids ----


def _randbelow(monkeypatch, values):
    seq = list(values)
    monkeypatch.setattr(secure.secrets, "randbelow", lambda n: seq.pop(0))
    return seq


def test_a_new_sender_id_is_not_one_that_the_file_has(tmp_path, monkeypatch):
    rig = make_rig(tmp_path)
    taken = rig.store.sender
    monkeypatch.setenv("SENSEI_HOST_ID", "another-host")
    other = KeyStore.load(rig.path, private_pem = rig.pem, clock = rig.clock)
    assert other.sender_changed
    seq = _randbelow(monkeypatch, [taken - 1, taken - 1, 4711])  # two collisions, then a free one
    other.save()
    assert other.sender == 4712 and not seq
    senders = json.loads(rig.path.read_text())["senders"]
    assert sorted(senders.values()) == sorted([f"{taken:08x}", f"{4712:08x}"])
    # by hand as well
    seq = _randbelow(monkeypatch, [4711, 99])
    assert other.new_sender() == 100 and not seq
    assert len(set(json.loads(rig.path.read_text())["senders"].values())) == 2


# ---- L3: the read functions take the lock ----


@pytest.mark.parametrize("call", ["generation", "node_state"])
def test_generation_and_node_state_wait_for_the_lock(tmp_path, call):
    rig = make_rig(tmp_path)
    store = rig.store
    done = threading.Event()
    with store._lock:
        th = threading.Thread(target = lambda: (getattr(store, call)(NODE_ID), done.set()))
        th.start()
        assert not done.wait(0.3)  # a commit or a promotion is in the middle: the reader waits
    assert done.wait(10)
    th.join(10)


# ---- L4: a promotion that cannot be saved ----


def test_a_promotion_that_cannot_be_saved_keeps_the_message(tmp_path, monkeypatch, caplog):
    rig = make_rig(tmp_path, autosave_s = 0.0)
    rig.mix()
    real = secure._write_atomic

    def boom(path, text):
        raise OSError("disk full")

    monkeypatch.setattr(secure, "_write_atomic", boom)
    o = secure.open_sealed_ex(rig.up(b"first of generation 1"), rig.store)  # does not raise
    assert o.promoted and o.plaintext == b"first of generation 1"
    assert rig.store.generation(NODE_ID) == 1 and rig.store.stats["save_failed"] == 1
    assert stored(rig)["cur"]["gen"] == 0  # the file has the old state
    with pytest.raises(secure.ReplayError):  # the message counts as received
        secure.open_sealed(rig.node.seal(KIND_UPLINK, b"x", counter = rig.store.node_state(NODE_ID).highest), rig.store)
    # the next message tries to save again; the disk works now
    monkeypatch.setattr(secure, "_write_atomic", real)
    assert secure.open_sealed(rig.up(b"second"), rig.store)[2] == b"second"
    assert stored(rig)["cur"]["gen"] == 1 and rig.store._dirty is False


def test_a_promotion_that_cannot_be_saved_is_saved_by_flush(tmp_path, monkeypatch):
    rig = make_rig(tmp_path)
    rig.mix()
    real = secure._write_atomic
    monkeypatch.setattr(secure, "_write_atomic", lambda path, text: (_ for _ in ()).throw(OSError("disk full")))
    assert secure.open_sealed_ex(rig.up(b"gen 1"), rig.store).promoted
    assert rig.store._dirty
    monkeypatch.setattr(secure, "_write_atomic", real)
    rig.store.flush()
    assert stored(rig)["cur"]["gen"] == 1
    # a collector that died before the retry promotes again from the `next` in the file
    rig2 = make_rig(tmp_path / "two")
    rig2.mix()
    monkeypatch.setattr(secure, "_write_atomic", lambda path, text: (_ for _ in ()).throw(OSError("disk full")))
    assert secure.open_sealed_ex(rig2.up(b"gen 1"), rig2.store).promoted
    monkeypatch.setattr(secure, "_write_atomic", real)
    revived = KeyStore.load(rig2.path, responder = True, clock = rig2.clock)
    assert secure.open_sealed_ex(rig2.up(b"gen 1 again"), revived).promoted
