# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""The key store at the key step of a commissioning (`add_node_for_commissioning`, `reset_if_node_restarted`,
`commission.check_key_proof`): a node that the store knows with the same key is not reset, the proof is checked against the
state as it is (any generation), and the store is reset only if the proof says that the node started again at generation 0."""

from __future__ import annotations

import pytest
from fs_helpers import NODE_ID, make_rig
from sensei_sdk import commission as com
from sensei_sdk import secure
from sensei_sdk.commission import ProofError

NG, NN = bytes(range(0x30, 0x40)), bytes(range(0x50, 0x60))
SID = bytes(range(0x70, 0x80))


def proof_of(node, sid: bytes = SID) -> bytes:
    """What the firmware does: the proof plaintext sealed through the normal counter path, in the node's generation."""
    return node.seal(secure.KIND_NODE_TO_HOST, com.proof_plain(NODE_ID, NG, NN, sid))


def test_a_known_node_with_the_same_key_is_not_reset_in_generation_one(tmp_path):
    """The reviewer's case: the key step happened, the link dropped, the node mixed to generation 1 within the window. The
    next session repeats the same key; the node seals its proof in generation 1."""
    rig = make_rig(tmp_path)
    rig.mix()
    rig.promote()
    assert rig.store.generation(NODE_ID) == 1
    backup = rig.store.add_node_for_commissioning(NODE_ID, rig.node.node_pub)
    assert not backup.changed and rig.store.generation(NODE_ID) == 1  # nothing reset, nothing saved
    assert com.check_key_proof(proof_of(rig.node), rig.store, NODE_ID, NG, NN, SID) is None
    assert rig.store.generation(NODE_ID) == 1
    # what `add_node` does instead: the store goes back to generation 0 and the proof of generation 1 fails
    (tmp_path / "other").mkdir()
    other = make_rig(tmp_path / "other")
    other.mix()
    other.promote()
    other.store.add_node(NODE_ID, other.node.node_pub)
    assert other.store.generation(NODE_ID) == 0
    with pytest.raises(secure.SecureError):
        secure.open_sealed(proof_of(other.node), other.store)


def test_the_store_is_reset_when_the_proof_says_the_node_started_again(tmp_path):
    rig = make_rig(tmp_path)
    rig.mix()
    rig.promote()
    backup = rig.store.add_node_for_commissioning(NODE_ID, rig.node.node_pub)
    assert not backup.changed
    # the key reached the node: it dropped its ratchet state and seals in generation 0
    fresh = secure.NodeSession(rig.node_priv, rig.store.backend_public_key())
    proof = proof_of(fresh)
    assert not secure.parse_header(proof).ratchet
    reset = com.check_key_proof(proof, rig.store, NODE_ID, NG, NN, SID)
    assert reset is not None and reset.changed and rig.store.generation(NODE_ID) == 0
    # a failure later in the step: rollback gives the generation back
    rig.store.rollback_node(reset)
    assert rig.store.generation(NODE_ID) == 1


def test_a_proof_that_fails_leaves_the_store_as_it_was(tmp_path):
    rig = make_rig(tmp_path)
    rig.mix()
    rig.promote()
    rig.store.add_node_for_commissioning(NODE_ID, rig.node.node_pub)
    fresh = secure.NodeSession(rig.node_priv, rig.store.backend_public_key())
    other = bytes([SID[0] ^ 1]) + SID[1:]  # a proof of another PAKE run
    with pytest.raises(ProofError):
        com.check_key_proof(proof_of(fresh, other), rig.store, NODE_ID, NG, NN, SID)
    assert rig.store.generation(NODE_ID) == 1  # the reset made for the check was rolled back


def test_a_proof_of_another_key_does_not_reset(tmp_path):
    rig = make_rig(tmp_path)
    rig.mix()
    rig.promote()
    rig.store.add_node_for_commissioning(NODE_ID, rig.node.node_pub)
    other_priv, _ = secure.generate_keypair()
    fake = secure.NodeSession(other_priv, rig.store.backend_public_key())
    assert rig.store.reset_if_node_restarted(NODE_ID, proof_of(fake)) is None
    with pytest.raises(ProofError):
        com.check_key_proof(proof_of(fake), rig.store, NODE_ID, NG, NN, SID)
    assert rig.store.generation(NODE_ID) == 1


def test_generation_zero_needs_no_reset(tmp_path):
    rig = make_rig(tmp_path)
    backup = rig.store.add_node_for_commissioning(NODE_ID, rig.node.node_pub)
    assert not backup.changed
    assert com.check_key_proof(proof_of(rig.node), rig.store, NODE_ID, NG, NN, SID) is None
    assert rig.store.reset_if_node_restarted(NODE_ID, proof_of(rig.node)) is None
    assert rig.store.reset_if_node_restarted(NODE_ID + 1, b"x") is None  # unknown node, junk


def test_a_new_node_or_key_is_added_as_add_node_does_and_rolls_back(tmp_path):
    rig = make_rig(tmp_path)
    priv, pub = secure.generate_keypair()
    backup = rig.store.add_node_for_commissioning(NODE_ID + 5, pub)
    assert backup.changed and rig.store.has_node(NODE_ID + 5)
    rig.store.rollback_node(backup)
    assert not rig.store.has_node(NODE_ID + 5)
    # a new key for a known node starts new keys (the old entry is replaced; rollback gives it back)
    old_id = rig.store.key_id(NODE_ID)
    backup = rig.store.add_node_for_commissioning(NODE_ID, pub)
    assert backup.changed and rig.store.key_id(NODE_ID) != old_id
    rig.store.rollback_node(backup)
    assert rig.store.key_id(NODE_ID) == old_id


def _gen1_store_with_restarted_node(tmp_path):
    rig = make_rig(tmp_path)
    rig.mix()
    rig.promote()
    rig.store.add_node_for_commissioning(NODE_ID, rig.node.node_pub)
    fresh = secure.NodeSession(rig.node_priv, rig.store.backend_public_key())
    return rig, fresh


def test_a_fake_generation_zero_proof_writes_nothing(tmp_path):
    """The header is not authenticated: a proof with the real key id and generation 0 whose tag is wrong must neither reset nor
    save the store."""
    rig, fresh = _gen1_store_with_restarted_node(tmp_path)
    good = proof_of(fresh)
    fake = good[:-1] + bytes([good[-1] ^ 1])  # the tag does not fit
    prov, text = rig.store._nodes[NODE_ID].prov, rig.path.read_bytes()
    assert rig.store.reset_if_node_restarted(NODE_ID, fake) is None
    with pytest.raises(ProofError):
        com.check_key_proof(fake, rig.store, NODE_ID, NG, NN, SID)
    assert rig.store._nodes[NODE_ID].prov == prov and rig.path.read_bytes() == text
    assert rig.store.generation(NODE_ID) == 1
    # a counter that the store has seen does not pass the in-memory check either
    rig.store._nodes[NODE_ID].up.update(secure.parse_header(good).counter)
    assert rig.store.reset_if_node_restarted(NODE_ID, good) is None
    assert rig.store._nodes[NODE_ID].prov == prov and rig.path.read_bytes() == text


def test_a_save_that_fails_raises_and_leaves_the_store_as_it_was(tmp_path, monkeypatch):
    rig, fresh = _gen1_store_with_restarted_node(tmp_path)
    prov = rig.store._nodes[NODE_ID].prov

    def broken(*_a, **_k):
        raise OSError("disk full")

    monkeypatch.setattr(rig.store, "_touch", broken)
    with pytest.raises(secure.SecureError) as e:
        com.check_key_proof(proof_of(fresh), rig.store, NODE_ID, NG, NN, SID)
    assert not isinstance(e.value, ProofError) and "could not be saved" in str(e.value)
    assert rig.store._nodes[NODE_ID].prov == prov and rig.store.generation(NODE_ID) == 1


def test_a_rollback_that_fails_raises(tmp_path, monkeypatch):
    rig, fresh = _gen1_store_with_restarted_node(tmp_path)
    other = bytes([SID[0] ^ 1]) + SID[1:]  # a good generation 0 proof of another PAKE run: reset, then the check fails

    def broken(_backup):
        raise OSError("disk full")

    monkeypatch.setattr(rig.store, "rollback_node", broken)
    with pytest.raises(secure.SecureError) as e:
        com.check_key_proof(proof_of(fresh, other), rig.store, NODE_ID, NG, NN, SID)
    assert not isinstance(e.value, ProofError) and "put back" in str(e.value)
