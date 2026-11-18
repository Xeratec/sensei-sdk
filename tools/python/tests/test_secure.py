# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Envelope, replay window and key store of sensei_sdk.secure."""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys

import pytest
from sensei_sdk import secure
from sensei_sdk.secure import KIND_DOWNLINK, KIND_NODE_TO_HOST, KIND_SETTINGS, KIND_UPLINK, KeyStore, NodeSession, \
    ReplayError, SecureError, UnknownKeyError

NODE_ID = 0x0123456789ABCDEF


@pytest.fixture
def backend(tmp_path):
    pem = tmp_path / "backend.pem"
    pub = secure.backend_keygen(pem)
    store = KeyStore.load(tmp_path / "keys.json", private_pem = pem)
    assert store.backend_public_key() == pub
    return store, pem


def make_node(store: KeyStore, node_id: int = NODE_ID) -> NodeSession:
    priv, pub = secure.generate_keypair()
    store.add_node(node_id, pub)
    return NodeSession(priv, store.backend_public_key())


def test_overhead_and_marker():
    assert secure.OVERHEAD == 26 and secure.OVERHEAD_NODE_ID == 34 and secure.OVERHEAD_DOWN == 30
    env = secure.seal_raw(bytes(16), 1, 1, KIND_UPLINK, b"x" * 100)
    assert len(env) == 126 and secure.is_sealed(env) and env[0] == 0xAC
    assert len(secure.seal_raw(bytes(16), 1, 1, KIND_UPLINK, b"x" * 100, node_id = 5)) == 134
    down = secure.seal_raw(bytes(16), 1, 1, KIND_DOWNLINK, b"x" * 100, sender = 7)
    assert len(down) == 130 and secure.is_sealed(down) and down[0] == 0xAD
    assert secure.parse_header(down).sender == 7 and secure.parse_header(env).sender == 0
    with pytest.raises(SecureError, match = "sender"):
        secure.seal_raw(bytes(16), 1, 1, KIND_DOWNLINK, b"x")
    with pytest.raises(SecureError, match = "sender"):
        secure.seal_raw(bytes(16), 1, 1, KIND_UPLINK, b"x", sender = 3)


def test_envelopes_of_other_versions_are_not_sealed():
    env = bytearray(secure.seal_raw(bytes(16), 1, 1, KIND_UPLINK, b"x" * 20))
    for version in (0, 1, 2):  # 1 was the first format
        bad = bytearray(env)
        bad[0] = (bad[0] & 0xF3) | (version << 2)
        assert not secure.is_sealed(bytes(bad))
        with pytest.raises(SecureError):
            secure.parse_header(bytes(bad))


def test_uplink_round_trip_and_counters(backend):
    store, _ = backend
    node = make_node(store)
    for i in range(1, 4):
        env = node.seal(KIND_UPLINK, b"hello %d" % i)
        assert secure.open_sealed(env, store) == (NODE_ID, KIND_UPLINK, b"hello %d" % i)
    assert store._nodes[NODE_ID].up.highest == 3


def test_all_kinds_both_directions(backend):
    store, _ = backend
    node = make_node(store)
    assert secure.open_sealed(node.seal(KIND_NODE_TO_HOST, b"rec"), store)[1:] == (KIND_NODE_TO_HOST, b"rec")
    for kind in (KIND_DOWNLINK, KIND_SETTINGS):
        env = secure.seal_downlink(NODE_ID, kind, b"cmd", store)
        assert node.open(env) == (kind, b"cmd")
    with pytest.raises(SecureError):
        secure.seal_downlink(NODE_ID, KIND_UPLINK, b"", store)


def test_node_id_in_header(backend):
    store, _ = backend
    node = make_node(store)
    assert secure.open_sealed(node.seal(KIND_UPLINK, b"a", node_id = NODE_ID), store)[0] == NODE_ID
    with pytest.raises(SecureError, match = "node id"):
        secure.open_sealed(node.seal(KIND_UPLINK, b"a", node_id = NODE_ID + 1), store)


def test_tamper_every_byte(backend):
    store, _ = backend
    node = make_node(store)
    env = node.seal(KIND_UPLINK, b"payload 0123456789")
    for i in range(len(env)):
        bad = bytearray(env)
        bad[i] ^= 0x01
        with pytest.raises(SecureError):
            secure.open_sealed(bytes(bad), store)
    assert store._nodes[NODE_ID].up.highest == 0  # failures leave no trace
    with pytest.raises(SecureError):
        secure.open_sealed(env[:-1], store)
    assert secure.open_sealed(env, store)[2] == b"payload 0123456789"


def test_replay_and_window(backend):
    store, _ = backend
    node = make_node(store)
    envs = {c: node.seal(KIND_UPLINK, b"m%d" % c, counter = c) for c in (1, 2, 3, 4, 5, 36, 37, 99, 100)}
    for c in (5, 3, 4, 1, 2):  # out of order inside the window
        secure.open_sealed(envs[c], store)
    for c in (1, 2, 3, 4, 5):
        with pytest.raises(ReplayError):
            secure.open_sealed(envs[c], store)
    secure.open_sealed(envs[100], store)
    with pytest.raises(ReplayError):  # 64 behind the highest
        secure.open_sealed(envs[36], store)
    secure.open_sealed(envs[37], store)  # 63 behind: still in the window, once
    with pytest.raises(ReplayError):
        secure.open_sealed(envs[37], store)
    secure.open_sealed(envs[99], store)


def test_counter_zero_and_bounds():
    with pytest.raises(SecureError):
        secure.seal_raw(bytes(16), 1, 0, KIND_UPLINK, b"")
    with pytest.raises(SecureError):
        secure.seal_raw(bytes(16), 1, 2**32, KIND_UPLINK, b"")
    w = secure.ReplayWindow()
    with pytest.raises(ReplayError):
        w.check(0)


def test_wrong_key_id_and_direction(backend, tmp_path):
    store, _ = backend
    node = make_node(store)
    stranger = NodeSession(*[secure.generate_keypair()[0]], store.backend_public_key())
    with pytest.raises(UnknownKeyError):
        secure.open_sealed(stranger.seal(KIND_UPLINK, b"x"), store)
    down = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store)
    with pytest.raises(SecureError, match = "node to backend"):
        secure.open_sealed(down, store)
    up = node.seal(KIND_UPLINK, b"x")
    with pytest.raises(SecureError, match = "direction"):
        node.open(up)
    with pytest.raises(UnknownKeyError):
        stranger.open(down)
    with pytest.raises(UnknownKeyError):
        secure.seal_downlink(42, KIND_DOWNLINK, b"x", store)


def test_downlink_replay_on_node(backend):
    store, _ = backend
    node = make_node(store)
    a = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"a", store)
    b = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"b", store)
    assert node.open(b)[1] == b"b" and node.open(a)[1] == b"a"
    with pytest.raises(ReplayError):
        node.open(a)


def test_wrong_direction_keys_differ(backend):
    store, _ = backend
    make_node(store)
    keys = store._nodes[NODE_ID].keys
    assert keys.k_up != keys.k_down and keys.key_id != 0


def test_both_sides_derive_the_same_keys():
    npriv, npub = secure.generate_keypair()
    bpriv, bpub = secure.generate_keypair()
    assert secure.derive_keys(npriv, bpub, npub, bpub) == secure.derive_keys(bpriv, npub, npub, bpub)


def test_bad_public_keys():
    npriv, npub = secure.generate_keypair()
    _, bpub = secure.generate_keypair()
    for bad in (bpub[:64], b"\x02" + bpub[1:33], bpub[:-1] + bytes([bpub[-1] ^ 1])):
        with pytest.raises(SecureError):
            secure.derive_keys(npriv, bad, npub, bpub)


def test_keystore_persistence(backend, tmp_path):
    store, _pem = backend
    node = make_node(store)
    for _i in range(3):
        secure.open_sealed(node.seal(KIND_UPLINK, b"x"), store)
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store)  # saved at once
    store.save()
    path = store.path
    assert stat.S_IMODE(os.stat(path).st_mode) == 0o600
    # a collector loads the file without the private key
    again = KeyStore.load(path)
    assert again.backend_public_key() == store.backend_public_key()
    assert again.node_ids() == [NODE_ID] and again.key_id(NODE_ID) == store.key_id(NODE_ID)
    with pytest.raises(ReplayError):
        secure.open_sealed(node.seal(KIND_UPLINK, b"old", counter = 2), again)
    assert secure.open_sealed(node.seal(KIND_UPLINK, b"new", counter = 4), again)[2] == b"new"
    nxt = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"y", again)
    assert secure.parse_header(nxt).counter == 2  # the downlink counter survived
    with pytest.raises(SecureError, match = "private key"):
        again.add_node(1, secure.generate_keypair()[1])


def test_downlink_counter_saved_immediately(backend):
    store, _ = backend
    make_node(store)
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store)
    on_disk = json.loads(store.path.read_text())
    assert on_disk["version"] == 2 and on_disk["down_ctr"][f"{NODE_ID:016x}"] == 1
    assert list(on_disk["senders"].values()) == [f"{store.sender:08x}"]


def test_uplink_counters_autosave_and_flush(backend, tmp_path):
    store, _ = backend
    store.autosave_s = 3600
    node = make_node(store)
    store.save()
    secure.open_sealed(node.seal(KIND_UPLINK, b"x"), store)
    assert json.loads(store.path.read_text())["nodes"][f"{NODE_ID:016x}"]["up_highest"] == 0  # throttled
    store.flush()
    assert json.loads(store.path.read_text())["nodes"][f"{NODE_ID:016x}"]["up_highest"] == 1


def test_add_node_again(backend):
    store, _ = backend
    priv, pub = secure.generate_keypair()
    store.add_node(NODE_ID, pub)
    node = NodeSession(priv, store.backend_public_key())
    secure.open_sealed(node.seal(KIND_UPLINK, b"x"), store)
    store.add_node(NODE_ID, pub)  # same key: counters stay
    with pytest.raises(ReplayError):
        secure.open_sealed(node.seal(KIND_UPLINK, b"x", counter = 1), store)
    priv2, pub2 = secure.generate_keypair()
    store.add_node(NODE_ID, pub2)  # new key: new key id and counters
    new = NodeSession(priv2, store.backend_public_key())
    assert secure.open_sealed(new.seal(KIND_UPLINK, b"y"), store)[2] == b"y"
    with pytest.raises(UnknownKeyError):  # the old key id is gone
        secure.open_sealed(node.seal(KIND_UPLINK, b"z"), store)
    store.add_node(7, secure.generate_keypair()[1])
    assert store.node_ids() == [7, NODE_ID]
    store.remove_node(7)
    assert store.node_ids() == [NODE_ID]


def test_keystore_errors(tmp_path):
    empty = KeyStore.load(tmp_path / "none.json")
    with pytest.raises(SecureError):
        empty.backend_public_key()
    bad = tmp_path / "bad.json"
    bad.write_text("{not json")
    with pytest.raises(SecureError, match = "damaged"):
        KeyStore.load(bad)
    bad.write_text(json.dumps({"version": 9}))
    with pytest.raises(SecureError, match = "version"):
        KeyStore.load(bad)
    bad.write_text(json.dumps({"version": 1, "nodes": {}}))
    with pytest.raises(SecureError, match = "damaged"):
        KeyStore.load(bad)


def test_keystore_key_mismatch(backend, tmp_path):
    store, _ = backend
    make_node(store)
    other = tmp_path / "other.pem"
    secure.backend_keygen(other)
    with pytest.raises(SecureError, match = "does not belong"):
        KeyStore.load(store.path, private_pem = other)


def test_backend_keygen(tmp_path):
    pem = tmp_path / "sub" / "k.pem"
    pub = secure.backend_keygen(pem)
    assert len(pub) == 65 and pub[0] == 4
    assert stat.S_IMODE(os.stat(pem).st_mode) == 0o600
    with pytest.raises(FileExistsError):
        secure.backend_keygen(pem)


def test_private_key_from_environment(tmp_path, monkeypatch):
    pem = tmp_path / "k.pem"
    pub = secure.backend_keygen(pem)
    monkeypatch.setenv("SENSEI_BACKEND_KEY", str(pem))
    assert KeyStore.load(tmp_path / "s.json").backend_public_key() == pub


# ---- two store instances on one file (T4) ----


def _nonce_of(env: bytes) -> tuple[int, int]:
    h = secure.parse_header(env)
    return h.key_id, h.counter


def test_two_instances_never_reuse_a_downlink_counter(backend):
    """The GUI and the collector open the same file: each downlink takes a counter the other has not used."""
    a, pem = backend
    make_node(a)
    b = KeyStore.load(a.path, private_pem = pem)
    seen = []
    for store in (a, b, b, a, b, a):
        seen.append(_nonce_of(secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store)))
    assert len(set(seen)) == len(seen)
    assert [c for _, c in seen] == [1, 2, 3, 4, 5, 6]


def test_save_keeps_nodes_added_by_another_process(backend):
    a, pem = backend
    b = KeyStore.load(a.path, private_pem = pem)  # empty, loaded before the node exists
    make_node(a, 0x11)
    make_node(b, 0x22)  # b writes the file without knowing 0x11
    a.save()
    for store in (a, KeyStore.load(a.path, private_pem = pem)):
        assert store.node_ids() == [0x11, 0x22]


def test_save_merges_counters_and_never_lowers_them(backend):
    a, pem = backend
    node = make_node(a)
    b = KeyStore.load(a.path, private_pem = pem)
    for _ in range(3):
        secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", a)
    up = [node.seal(KIND_UPLINK, b"u") for _ in range(3)]
    for env in up:
        secure.open_sealed(env, b)
    b.save()  # b still thinks down_ctr is 0 and has not seen any downlink
    a.save()
    final = KeyStore.load(a.path, private_pem = pem)
    assert final.down_counter(NODE_ID) == 3
    assert final._nodes[NODE_ID].up.highest == 3
    with pytest.raises(ReplayError):
        secure.open_sealed(up[1], final)


def test_same_key_added_again_keeps_its_counters(backend):
    store, pem = backend
    _priv, pub = secure.generate_keypair()
    store.add_node(NODE_ID, pub)
    for _ in range(4):
        secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store)
    store.remove_node(NODE_ID)
    store.add_node(NODE_ID, pub)
    assert _nonce_of(secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store))[1] == 5
    # also across a restart, and with a removed node that another instance still has
    store.remove_node(NODE_ID)
    again = KeyStore.load(store.path, private_pem = pem)
    again.add_node(NODE_ID, pub)
    assert _nonce_of(secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", again))[1] == 6


def test_a_new_key_for_a_node_does_not_restart_the_counters(backend):
    """The counter belongs to the node id and the sender, not to the key: the node keeps its window per sender."""
    store, _ = backend
    make_node(store)
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store)
    make_node(store)  # another node key: other session keys
    assert _nonce_of(secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store))[1] == 2
    store.remove_node(NODE_ID)
    make_node(store)
    assert _nonce_of(secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store))[1] == 3


def test_save_syncs_the_file_and_the_directory(backend, monkeypatch):
    store, _ = backend
    synced = []
    real = os.fsync
    monkeypatch.setattr(os, "fsync", lambda fd: (synced.append(stat.S_ISDIR(os.fstat(fd).st_mode)), real(fd))[1])
    make_node(store)  # add_node saves
    assert synced == [False, True]  # the file first, then the directory
    assert not [p for p in store.path.parent.iterdir() if p.name.startswith("keys.json.") and p.suffix != ".lock"]


# ---- senders (H1, A5): hosts with their own sender id, counters that never go back ----


def test_each_store_has_a_sender_that_stays_in_the_file(backend):
    store, pem = backend
    make_node(store)
    sender = store.sender
    assert 0 < sender < 2**32
    assert list(json.loads(store.path.read_text())["senders"].values()) == [f"{sender:08x}"]
    assert KeyStore.load(store.path, private_pem = pem).sender == sender  # a restart keeps it
    assert KeyStore().sender != KeyStore().sender  # stores without a file: each its own
    env = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store)
    assert secure.parse_header(env).sender == sender


def test_a_copy_of_the_key_store_does_not_reuse_nonces(backend, tmp_path):
    """H1: the GUI on another host works on a copy of the file. Same keys, same counters, but another sender: the
    nonce differs and the node tracks a window for each."""
    store, pem = backend
    node = make_node(store)
    copy = tmp_path / "copy" / "keys.json"
    copy.parent.mkdir()
    shutil.copy(store.path, copy)
    other = KeyStore.load(copy, private_pem = pem)
    assert other.sender != store.sender and other.sender_changed  # a plain copy takes its own sender by itself
    a = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"collector", store)
    b = secure.seal_downlink(NODE_ID, KIND_SETTINGS, b"gui", other)
    ha, hb = secure.parse_header(a), secure.parse_header(b)
    assert (ha.key_id, ha.counter) == (hb.key_id, hb.counter)  # the counters collide ...
    assert ha.sender != hb.sender  # ... the nonces do not
    assert secure._nonce(1, ha.sender, ha.counter) != secure._nonce(1, hb.sender, hb.counter)
    assert node.open(a) == (KIND_DOWNLINK, b"collector") and node.open(b) == (KIND_SETTINGS, b"gui")
    for env in (a, b):
        with pytest.raises(ReplayError):
            node.open(env)
    assert set(node.rx) == {ha.sender, hb.sender}


def test_a_store_opened_elsewhere_takes_a_new_sender(backend, tmp_path, monkeypatch):
    """D5: the file keeps a fingerprint (machine id and absolute path). The same host and path keep the sender; a copy
    in another place, on another machine, or a file from before the fingerprint takes a new one."""
    store, pem = backend
    make_node(store)
    path = store.path
    sender = store.sender
    assert json.loads(path.read_text())["senders"] == {secure.host_fingerprint(path): f"{sender:08x}"}
    again = KeyStore.load(path, private_pem = pem)  # a restart, or a second process on this host
    assert again.sender == sender and not again.sender_changed
    # another place
    copy = tmp_path / "elsewhere" / "keys.json"
    copy.parent.mkdir()
    shutil.copy(path, copy)
    moved = KeyStore.load(copy, private_pem = pem)
    assert moved.sender_changed and moved.sender != sender
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", moved)  # saves the file
    assert json.loads(copy.read_text())["senders"][secure.host_fingerprint(copy)] == f"{moved.sender:08x}"
    assert KeyStore.load(copy, private_pem = pem).sender == moved.sender  # now it belongs here
    assert json.loads(path.read_text())["senders"] == {secure.host_fingerprint(path): f"{sender:08x}"}  # untouched
    # another machine, same path
    monkeypatch.setattr(secure, "_machine_id", lambda: "another-machine")
    other = KeyStore.load(path, private_pem = pem)
    assert other.sender_changed and other.sender != sender
    monkeypatch.undo()
    # a file with one sender and no fingerprint: this host takes it over, no warning
    data = json.loads(path.read_text())
    data["sender"] = f"{sender:08x}"
    del data["senders"]
    path.write_text(json.dumps(data))
    old = KeyStore.load(path, private_pem = pem)
    assert old.sender == sender and not old.sender_changed
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", old)
    assert json.loads(path.read_text())["senders"] == {secure.host_fingerprint(path): f"{sender:08x}"}


def test_senders_are_kept_per_fingerprint_across_restarts(backend, tmp_path, monkeypatch):
    """F2: the GUI on the host and the collector in a container share one file and differ in fingerprint. Every
    restart of either finds its own sender again: the node sees two senders, not one more per restart."""
    store, pem = backend
    make_node(store)
    path = store.path

    def open_as(fingerprint: str) -> KeyStore:
        monkeypatch.setattr(secure, "host_fingerprint", lambda _p: fingerprint)
        return KeyStore.load(path, private_pem = pem)

    gui = open_as("host-gui")
    gui_sender = gui.sender
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", gui)
    senders = {gui_sender}
    col = open_as("container")
    assert col.sender_changed  # the first time: a new sender
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", col)
    for _ in range(5):
        col = open_as("container")
        secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", col)
        gui = open_as("host-gui")
        secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", gui)
        assert not col.sender_changed and not gui.sender_changed
        senders |= {col.sender, gui.sender}
    assert len(senders) == 2 and gui.sender == gui_sender
    assert len(json.loads(path.read_text())["senders"]) == 3  # and the fixture's own
    # a process that is running while the other one saves keeps both
    a, b = open_as("host-gui"), open_as("container")
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", a)
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", b)
    assert len(json.loads(path.read_text())["senders"]) == 3


def test_host_id_replaces_the_machine_id(backend, tmp_path, monkeypatch):
    """G1: a container has no machine id and a new host name at every start; SENSEI_HOST_ID keeps the fingerprint."""
    path = tmp_path / "ks.json"
    monkeypatch.setattr(secure, "_machine_id", lambda: "container-1")
    first = secure.host_fingerprint(path)
    monkeypatch.setattr(secure, "_machine_id", lambda: "container-2")  # recreated: another host name
    assert secure.host_fingerprint(path) != first
    monkeypatch.setenv("SENSEI_HOST_ID", "sensei-collector")
    fixed = secure.host_fingerprint(path)
    monkeypatch.setattr(secure, "_machine_id", lambda: "container-3")
    assert secure.host_fingerprint(path) == fixed
    # the node's table of 8 senders stays at one entry over ten container recreations
    store, pem = backend
    node = make_node(store)
    for i in range(10):
        monkeypatch.setattr(secure, "_machine_id", lambda i = i: f"container-{i}")
        env = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", KeyStore.load(store.path, private_pem = pem))
        assert node.open(env)[0] == KIND_DOWNLINK
    assert len(node.rx) == 1


def test_new_sender_command(backend, capsys):
    store, _ = backend
    make_node(store)
    before = store.sender
    assert secure.main(["new-sender", str(store.path)]) == 0
    printed = capsys.readouterr().out.split()[-1]
    assert int(printed, 16) != before
    assert KeyStore.load(store.path).sender == int(printed, 16)


def test_the_node_refuses_a_ninth_sender(backend):
    store, _ = backend
    node = make_node(store)
    for _ in range(secure.MAX_SENDERS):
        other = KeyStore()
        other._nodes = store._nodes
        other._by_key_id = store._by_key_id
        assert node.open(secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", other))[0] == KIND_DOWNLINK
    extra = KeyStore()
    extra._nodes, extra._by_key_id = store._nodes, store._by_key_id
    with pytest.raises(SecureError, match = "too many senders"):
        node.open(secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", extra))
    assert len(node.rx) == secure.MAX_SENDERS


def test_a_node_session_keeps_counters_over_a_key_change(backend):
    """A5, simulated node: key A, key B, key A again. No counter of A is used twice, no old downlink is accepted."""
    store, _ = backend
    priv, pub = secure.generate_keypair()
    store.add_node(NODE_ID, pub)
    a = NodeSession(priv, store.backend_public_key())
    envs = [a.seal(KIND_UPLINK, b"x") for _ in range(3)]
    old_down = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store)
    a.open(old_down)
    b = NodeSession(priv, secure.generate_keypair()[1], a.tx_counter, a.rx)
    b.seal(KIND_UPLINK, b"y")
    again = NodeSession(priv, store.backend_public_key(), b.tx_counter, b.rx)
    assert secure.parse_header(again.seal(KIND_UPLINK, b"z")).counter == 5
    assert [secure.parse_header(e).counter for e in envs] == [1, 2, 3]
    with pytest.raises(ReplayError):
        again.open(old_down)


def test_a_version_1_key_store_is_migrated(backend, tmp_path):
    """The counters of a version 1 file (down_ctr inside the nodes, no sender) are kept; the next save writes version 2."""
    store, pem = backend
    make_node(store)
    data = json.loads(store.path.read_text())
    data["version"] = 1
    del data["senders"], data["down_ctr"]
    data["nodes"][f"{NODE_ID:016x}"]["down_ctr"] = 41
    path = tmp_path / "v1.json"
    path.write_text(json.dumps(data))
    old = KeyStore.load(path, private_pem = pem)
    assert old.down_counter(NODE_ID) == 41
    env = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", old)
    assert secure.parse_header(env).counter == 42
    saved = json.loads(path.read_text())
    assert saved["version"] == 2 and saved["down_ctr"][f"{NODE_ID:016x}"] == 42 and saved["senders"]
    assert KeyStore.load(path).sender == old.sender


def test_removing_and_adding_a_node_with_another_key_keeps_its_counter(backend):
    store, pem = backend
    make_node(store)
    for _ in range(3):
        secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store)
    store.remove_node(NODE_ID)
    assert store.down_counter(NODE_ID) == 3
    make_node(store)
    again = KeyStore.load(store.path, private_pem = pem)
    assert _nonce_of(secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", again))[1] == 4


_WORKER = """
import sys
from sensei_sdk import secure
store = secure.KeyStore.load(sys.argv[1])
for _ in range(int(sys.argv[2])):
    env = secure.seal_downlink(int(sys.argv[3]), secure.KIND_DOWNLINK, b"x", store)
    print(secure.parse_header(env).counter)
"""


def test_processes_on_one_file_never_use_a_counter_twice(backend):
    """H1 with real processes (the GUI and the collector): five of them seal 20 downlinks each on one file."""
    store, _ = backend
    make_node(store)
    procs = [
        subprocess.Popen(
            [sys.executable, "-c", _WORKER, str(store.path), "20",
             str(NODE_ID)],
            stdout = subprocess.PIPE,
            text = True,
        ) for _ in range(5)
    ]
    counters = []
    for p in procs:
        out, _err = p.communicate(timeout = 120)
        assert p.returncode == 0
        counters += [int(x) for x in out.split()]
    assert sorted(counters) == list(range(1, 101))
    assert KeyStore.load(store.path).down_counter(NODE_ID) == 100


def test_peek_sealed_leaves_the_replay_window(backend):
    store, _ = backend
    node = make_node(store)
    env = node.seal(KIND_NODE_TO_HOST, b"hello")
    assert secure.peek_sealed(env, store) == (KIND_NODE_TO_HOST, b"hello")
    assert secure.peek_sealed(env, store) == (KIND_NODE_TO_HOST, b"hello")  # again: no replay check
    assert secure.open_sealed(env, store)[1:] == (KIND_NODE_TO_HOST, b"hello")  # the real reader still gets it
    with pytest.raises(ReplayError):
        secure.open_sealed(env, store)
    assert secure.peek_sealed(env, store)[1] == b"hello"  # a peek is no reader
    bad = bytearray(env)
    bad[-1] ^= 1
    with pytest.raises(SecureError):
        secure.peek_sealed(bytes(bad), store)
    stranger = NodeSession(secure.generate_keypair()[0], store.backend_public_key())
    with pytest.raises(UnknownKeyError):
        secure.peek_sealed(stranger.seal(KIND_UPLINK, b"x"), store)


def test_there_is_one_sealed_check():
    """S2: secure.is_sealed is framing.looks_sealed, and its constants are the envelope's."""
    from sensei_sdk import framing

    assert secure.OVERHEAD == 26
    assert (secure.MARKER | (secure.VERSION << 2)) == 0xAC
    for first in range(256):
        for n in (0, 25, 26, 40):
            payload = bytes([first]) + bytes(max(n - 1, 0)) if n else b""
            assert secure.is_sealed(payload) == framing.looks_sealed(payload) == (n >= 26 and 0xAC <= first <= 0xAF)
