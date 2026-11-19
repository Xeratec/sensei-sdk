# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Review fixes of the forward secrecy Phase 6: the stale request rule of the node twin, `next_since` of a replaced
request, one responder per key store."""

from __future__ import annotations

import json
import logging
import os
import stat

import pytest
from fs_helpers import NODE_ID, Clock, make_rig, slack_walker
from sensei_sdk import ratchet, secure
from sensei_sdk.secure import KeyStore

# ---- stale request (the node twin follows lib/sensei_secure request_stale) ----


def test_node_twin_replaces_a_stale_request_and_refuses_its_answer(tmp_path):
    clock = Clock()
    rig = make_rig(tmp_path, clock = clock, walker = slack_walker(clock))
    node = rig.node
    node.seal(secure.KIND_UPLINK, b"x")
    req = node.rekey_request()
    ans = rig.store.rekey_answer(NODE_ID, req)
    # the last epoch that is not stale yet: the same request again
    node.tx_counter = 64 * (req.up_base + ratchet.STALE_EPOCHS) + 1
    assert node.rekey_request() == req
    # one more epoch: a new request (new key, later base) replaces it
    node.tx_counter = 64 * (req.up_base + ratchet.STALE_EPOCHS + 1) + 1
    req2 = node.rekey_request()
    assert req2 != req and req2.pub != req.pub and req2.up_base > req.up_base
    # the answer to the old request is refused; the new one is answered and used
    assert not node.rekey_answer(ans) and node.gen == 0
    ans2 = rig.store.rekey_answer(NODE_ID, req2)
    assert ans2 is not None and node.rekey_answer(ans2) and node.gen == 1
    assert rig.promote().gen == 1


def test_node_twin_stale_answer_is_refused_before_the_switch(tmp_path):
    rig = make_rig(tmp_path)
    node = rig.node
    req = node.rekey_request()
    ans = rig.store.rekey_answer(NODE_ID, req)
    node.tx_counter = 64 * (req.up_base + ratchet.STALE_EPOCHS + 1)
    assert node.request_stale() and not node.rekey_answer(ans) and node.gen == 0
    assert node.rekey_request() != req and not node.request_stale()


def test_stale_epochs_are_32768():
    assert ratchet.STALE_EPOCHS == 32768


# ---- next_since keeps the first unanswered request ----


def test_next_since_stays_at_the_first_request_when_it_is_replaced(tmp_path):
    clock = Clock()
    rig = make_rig(tmp_path, clock = clock)
    node, store = rig.node, rig.store
    t0 = clock()
    store.rekey_answer(NODE_ID, node.rekey_request())
    assert store.node_state(NODE_ID).next_since == pytest.approx(t0)
    for hours in (6, 12, 18):  # the node asks again with a new key each time (the old one is stale)
        clock.advance(hours * 3600)
        node.pending = None
        node.tx_counter += 64
        ans = store.rekey_answer(NODE_ID, node.rekey_request())
        assert ans is not None
        st = store.node_state(NODE_ID)
        assert st.next_gen == 1 and st.next_since == pytest.approx(t0)
    # on disk, and after a restart
    revived = KeyStore.load(rig.path, responder = True, clock = clock)
    assert revived.node_state(NODE_ID).next_since == pytest.approx(t0)
    # the same request repeated does not move it either, and it is the last answer that the node can use
    assert node.rekey_answer(ans) and rig.promote().gen == 1


# ---- one responder per key store ----


def test_responder_id_file(tmp_path):
    p = tmp_path / "state" / "responder_id"
    a = secure.load_responder_id(p)
    assert len(a) == 16 and secure.load_responder_id(p) == a
    assert stat.S_IMODE(os.stat(p).st_mode) == 0o600
    assert secure.load_responder_id(tmp_path / "other") != a


def test_responder_id_file_empty_damaged_or_unwritable(tmp_path, caplog):
    p = tmp_path / "state" / "responder_id"
    p.parent.mkdir()
    for bad in ("", "\n", "not hex\n", "ABCDEF0123456789\n", "0123"):  # power loss, full disk, garbage
        p.write_text(bad)
        with caplog.at_level(logging.WARNING, logger = "sensei.secure"):
            new = secure.load_responder_id(p)
        assert new is not None and len(new) == 16 and p.read_text().strip() == new
        assert "damaged" in caplog.text
        caplog.clear()
        assert secure.load_responder_id(p) == new  # stable from now on
        assert stat.S_IMODE(os.stat(p).st_mode) == 0o600
    assert not list(p.parent.glob("responder_id.*"))  # no temp file left
    if os.name != "nt" and os.geteuid() != 0:  # a folder that cannot be written: warn, None, no exception
        ro = tmp_path / "ro"
        ro.mkdir()
        ro.chmod(0o500)
        try:
            with caplog.at_level(logging.WARNING, logger = "sensei.secure"):
                assert secure.load_responder_id(ro / "responder_id") is None
            assert "cannot be written" in caplog.text
            assert KeyStore(responder = True,
                            responder_id = secure.load_responder_id(ro / "responder_id")).responder_id is None
        finally:
            ro.chmod(0o700)


def stored_responder(path) -> str | None:
    return json.loads(path.read_text()).get("responder")


def test_responder_records_its_id_and_another_responder_warns_but_answers(tmp_path, caplog):
    rig = make_rig(tmp_path, responder_id = "aaaaaaaaaaaaaaaa")
    assert rig.store.responder_id == "aaaaaaaaaaaaaaaa"
    secure.open_sealed(rig.up(b"x"), rig.store)
    ans = rig.store.rekey_answer(NODE_ID, rig.node.rekey_request())
    assert ans is not None and stored_responder(rig.path) == "aaaaaaaaaaaaaaaa"
    assert rig.store.stats["responder_conflict"] == 0
    # a copy of the store with its own responder id: warns, counts, answers
    b = KeyStore.load(rig.path, responder = True, responder_id = "bbbbbbbbbbbbbbbb", clock = rig.clock)
    assert b.stats["responder_conflict"] == 1
    assert "another responder" in caplog.text and "aaaaaaaaaaaaaaaa" in caplog.text
    req = rig.node.rekey_request()
    assert b.rekey_answer(NODE_ID, req) == ans  # still answers (the same request: the stored answer)
    assert stored_responder(rig.path) == "aaaaaaaaaaaaaaaa"  # a resend writes nothing
    b.save()
    assert stored_responder(rig.path) == "bbbbbbbbbbbbbbbb"  # the last one to save holds the record
    # the first responder sees the change at its next merge
    rig.store._merge_from_disk()
    assert rig.store.stats["responder_conflict"] == 1
    # the same id again: no conflict
    c = KeyStore.load(rig.path, responder = True, responder_id = "bbbbbbbbbbbbbbbb")
    assert c.stats["responder_conflict"] == 0


def test_responder_id_rules(tmp_path, monkeypatch, caplog):
    rig = make_rig(tmp_path, responder = False)
    assert stored_responder(rig.path) is None  # an old file or a store without an id: nothing recorded
    # a store that does not answer neither marks nor warns, and keeps the mark of the responder
    rig2 = make_rig(tmp_path / "two", responder_id = "cccccccccccccccc")
    rig2.store.save()
    follower = KeyStore.load(rig2.path, responder = False, responder_id = "dddddddddddddddd")
    assert follower.responder_id is None
    follower.save()
    assert stored_responder(rig2.path) == "cccccccccccccccc" and follower.stats["responder_conflict"] == 0
    # a responder without an id keeps the file as it is
    plain = KeyStore.load(rig2.path, responder = True)
    plain.save()
    assert plain.responder_id is None and stored_responder(rig2.path) == "cccccccccccccccc"
    # the id comes from the environment file
    monkeypatch.setenv("SENSEI_REKEY_RESPONDER_ID_FILE", str(tmp_path / "rid"))
    env = KeyStore.load(rig2.path, responder = True)
    assert env.responder_id == secure.load_responder_id(tmp_path / "rid")
    assert env.stats["responder_conflict"] == 1
    # seal-file keeps the record
    kf = tmp_path / "key"
    secure.new_key_file(kf)
    secure.seal_file(rig2.path, kf)
    secure.unseal_file(rig2.path, kf)
    assert stored_responder(rig2.path) is not None


# ---- gen_since (the age of a generation, for the alert on a node that does not mix) ----


def test_gen_since_is_set_at_provisioning_and_at_promotion_and_survives_a_restart(tmp_path):
    clock = Clock()
    rig = make_rig(tmp_path, clock = clock)
    t0 = clock()
    assert rig.store.node_state(NODE_ID).gen_since == pytest.approx(t0)  # add_node
    clock.advance(3600)
    rig.mix()
    assert rig.store.node_state(NODE_ID).gen_since == pytest.approx(t0)  # answered, not promoted yet
    clock.advance(60)
    rig.promote()
    t1 = clock()
    assert rig.store.node_state(NODE_ID).gen_since == pytest.approx(t1)
    revived = KeyStore.load(rig.path, clock = clock)
    assert revived.node_state(NODE_ID).gen_since == pytest.approx(t1)
    # a file from before the field: unknown
    data = json.loads(rig.path.read_text())
    del data["nodes"][f"{NODE_ID:016x}"]["cur"]["since"]
    rig.path.write_text(json.dumps(data))
    assert KeyStore.load(rig.path, clock = clock).node_state(NODE_ID).gen_since is None


def test_last_uplink_is_the_time_of_the_last_verified_message_in_any_generation(tmp_path):
    clock = Clock()
    rig = make_rig(tmp_path, clock = clock)
    assert rig.store.node_state(NODE_ID).last_uplink is None
    secure.open_sealed(rig.up(b"x"), rig.store)  # generation 0
    assert rig.store.node_state(NODE_ID).last_uplink == pytest.approx(clock())
    clock.advance(3600)
    rig.mix()
    rig.promote()
    assert rig.store.node_state(NODE_ID).last_uplink == pytest.approx(clock())
