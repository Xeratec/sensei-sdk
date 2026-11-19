# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Property test of the collector side (design 1.9): random runs of uplinks, mixes, losses, reordering, duplicates,
forgeries, restarts and downlinks against a plain model of what the key store must accept.

Review of Phase 4: the store does not save by itself (autosave off), so a crash (a restart without `flush`) loses
whatever was not saved by a promotion, an answer, a downlink or a flush, and the model goes back to what the file held;
a node that asks again with a new key replaces the answer in `next`; and the file is checked: it holds no key of a
generation that was dropped and no chain key below the one it keeps."""

from __future__ import annotations

import json
import os
import random

import pytest
from fs_helpers import NODE_ID, make_rig
from sensei_sdk import ratchet, secure
from sensei_sdk.secure import KIND_DOWNLINK, KIND_UPLINK, GenerationError, KeyStore, ReplayError, SecureError

KEY = f"{NODE_ID:016x}"
NO_AUTOSAVE = 1e9
COVER = {"crash": 0, "replaced": 0, "checks": 0, "saves": 0}


class Run:

    def __init__(self, tmp_path, seed: int, monkeypatch) -> None:
        self.rng = random.Random(seed)
        self.rig = make_rig(tmp_path, autosave_s = NO_AUTOSAVE)
        self.saves = 0
        real = secure._write_atomic

        def counting(path, text):
            self.saves += 1
            real(path, text)

        monkeypatch.setattr(secure, "_write_atomic", counting)
        self.secrets: set[tuple] = set()  # every key the store held (kind, gen, ..., hex)
        self.saved_seen: set[int] = set()  # what the file holds
        self.saved_highest = 0
        self.store = self.rig.store
        self.node = self.rig.node
        self.inflight: list[tuple[bytes, bytes, int]] = []  # (envelope, plaintext, counter)
        self.seen: set[int] = set()  # counters opened
        self.highest = 0
        self.n = 0
        self.opened = self.refused = self.forged_refused = 0
        self.mixes = 0

    # the model: a counter is accepted once, if it is above highest - 64
    def expect_accept(self, counter: int) -> bool:
        return counter not in self.seen and counter > self.highest - 64

    def send(self) -> None:
        plain = os.urandom(self.rng.randrange(1, 40)) + b"%d" % self.n
        self.n += 1
        env = self.node.seal(KIND_UPLINK, plain)
        self.inflight.append((env, plain, secure.parse_header(env).counter))
        if self.rng.random() < 0.05:
            self.node.new_epoch()
        if self.rng.random() < 0.03:
            self.node.reboot()

    def deliver(self, env: bytes, plain: bytes | None, counter: int) -> None:
        want = self.expect_accept(counter)
        try:
            node_id, kind, got = secure.open_sealed(env, self.store)
        except ReplayError:
            assert not want, f"counter {counter} (highest {self.highest}) was refused"
            self.refused += 1
            return
        except GenerationError as exc:
            raise AssertionError(f"counter {counter} (highest {self.highest}): {exc}") from exc
        assert want and plain is not None and got == plain and node_id == NODE_ID
        self.seen.add(counter)
        self.highest = max(self.highest, counter)
        self.opened += 1

    # what the file holds: counters at the last save
    def snapshot(self) -> None:
        self.saved_seen, self.saved_highest = set(self.seen), self.highest

    def record(self) -> None:
        """Remember the keys of the store as they are now, to check the file against them."""
        n = self.store._nodes[NODE_ID]
        add = self.secrets.add
        if n.k_up is not None and n.k_down is not None:
            add(("static", 0, n.k_up.hex()))
            add(("static", 0, n.k_down.hex()))

        def keys(gen, root, k_down, chain):
            if root is not None:
                add(("root", gen, root.hex()))
            add(("down", gen, k_down.hex()))
            if chain is not None and chain.gen >= 1:
                add(("ck", gen, chain.up_base, chain.idx, chain.ck.hex()))

        keys(n.cur.gen, n.cur.root, n.cur.k_down, n.cur.up)
        if n.next is not None:
            keys(n.next.gen, n.next.root, n.next.k_down, n.next.up)
        if n.prev is not None and n.prev.gen >= 1:
            add(("ck", n.prev.gen, n.prev.up_base, n.prev.idx, n.prev.ck.hex()))

    def check_file(self) -> None:
        """The saved file against everything the store ever held: no root, downlink key or chain key of a generation that
        is dropped, no chain key below the one the file keeps, the static keys only while generation 0 is in use."""
        text = self.rig.path.read_text()
        f = json.loads(text)["nodes"][KEY]
        cur, nxt, prev = f["cur"], f["next"], f["prev_up"]
        allowed = {
            x for x in (cur["root"], cur["k_down"], nxt and nxt["root"], nxt and nxt["k_down"], f["k_up"], f["k_down"])
            if x
        }
        chains = set()
        if cur["up_ck"] is not None:
            chains.add((cur["gen"], cur["up_base"], cur["up_idx"], cur["up_ck"]))
        if nxt is not None:
            chains.add((nxt["gen"], nxt["up_base"], 0, nxt["ck"]))
        if prev is not None and prev["up_ck"] is not None:
            chains.add((prev["gen"], prev["up_base"], prev["up_idx"], prev["up_ck"]))
        for secret in self.secrets:
            if secret[-1] not in text:
                continue
            if secret[0] == "ck":
                assert secret[1:] in chains, f"the file holds a chain key that it should have dropped: {secret[:-1]}"
            else:
                assert secret[-1] in allowed, f"the file holds a key of a dropped generation: {secret[:2]}"
        assert (f["k_up"] is not None) == (cur["gen"] == 0 or (prev is not None and prev["gen"] == 0))
        assert prev is None or "root" not in prev
        if cur["gen"] >= 1:  # the chain is not behind the epoch before the highest counter (what idx_lo keeps)
            assert cur["up_idx"] >= ratchet.epoch(f["up_highest"]) - 1 - cur["up_base"] or f["up_highest"] == 0
        COVER["checks"] += 1

    def forge(self) -> None:
        h = self.rng.choice([self.highest + self.rng.randrange(-5, 400), self.rng.randrange(1, 2**32 - 1)])
        gen = self.rng.choice([self.node.gen, self.node.gen + 1, max(0, self.node.gen - 1)])
        env = secure.seal_raw(os.urandom(16), self.node.keys.key_id, max(1, h), KIND_UPLINK, b"forged", gen = gen)
        with pytest.raises(SecureError):
            secure.open_sealed(env, self.store)
        self.forged_refused += 1

    def mix_step(self) -> None:
        """Start a mix when the previous one is over, or go on with the open one: the node repeats its request (after a
        reboot), the collector answers the same way, the answer reaches the node or is lost."""
        if self.node.pending is None:
            st = self.store.node_state(NODE_ID)
            if st.prev_gen is not None or st.next_gen is not None:
                return
        elif self.rng.random() < 0.25:  # the node dropped its state and asks again with a new key: replaces `next`
            self.node.pending = None
            COVER["replaced"] += self.store.node_state(NODE_ID).next_gen is not None
        req = self.node.rekey_request()
        if self.rng.random() < 0.5:
            self.node.reboot()
            assert self.node.rekey_request() == req
        ans = self.store.rekey_answer(NODE_ID, req)
        assert ans is not None and self.store.rekey_answer(NODE_ID, req) == ans
        # the answer that was given is the one that is stored, in memory and in the file (a replaced one included)
        assert self.store._nodes[NODE_ID].next.e_c_pub == ans.pub
        assert json.loads(self.rig.path.read_text())["nodes"][KEY]["next"]["e_c_pub"] == ans.pub.hex()
        if self.rng.random() < 0.6:  # else the answer is lost and the node asks again later
            assert self.node.rekey_answer(ans)
            self.mixes += 1

    def downlink(self) -> None:
        d = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"down", self.store)
        # the node takes the generation of the store, or the one before it (until it asks for the next one)
        g_store, g_node = self.store.generation(NODE_ID), self.node.gen
        if g_store == g_node or (g_node == g_store + 1 and self.node.prev):
            assert self.node.open(d) == (KIND_DOWNLINK, b"down")

    def restart(self) -> None:
        self.store.flush()
        self.snapshot()
        self.store = KeyStore.load(self.rig.path, responder = True, clock = self.rig.clock, autosave_s = NO_AUTOSAVE)
        self.check_file()

    def crash(self) -> None:
        """The process dies: what was not saved is gone, the model goes back to what the file holds."""
        self.seen, self.highest = set(self.saved_seen), self.saved_highest
        self.store = KeyStore.load(self.rig.path, responder = True, clock = self.rig.clock, autosave_s = NO_AUTOSAVE)
        COVER["crash"] += 1
        self.check_file()

    def step(self) -> None:
        saves = self.saves
        self.step_once()
        if self.saves != saves:  # something was saved: the file holds all that the store holds now
            self.snapshot()
            COVER["saves"] += 1
        self.record()

    def step_once(self) -> None:
        r = self.rng.random()
        if r < 0.45:
            self.send()
        elif r < 0.80 and self.inflight:
            i = self.rng.randrange(len(self.inflight))
            env, plain, counter = self.inflight[i]
            if self.rng.random() < 0.15:  # delivered twice
                self.deliver(env, plain, counter)
            else:
                self.inflight.pop(i)
            self.deliver(env, plain, counter)
        elif r < 0.84 and self.inflight:
            self.inflight.pop(self.rng.randrange(len(self.inflight)))  # lost
        elif r < 0.88:
            self.forge()
        elif r < 0.92:
            self.mix_step()
        elif r < 0.95:
            self.downlink()
        elif r < 0.96:
            self.restart()
        elif r < 0.975:
            self.crash()
        else:
            self.rig.clock.advance(self.rng.randrange(1, 4000))


@pytest.mark.parametrize("seed", range(12))
def test_collector_matches_the_model(tmp_path, seed, monkeypatch):
    run = Run(tmp_path, seed, monkeypatch)
    for i in range(6000):
        run.step()
        if i % 250 == 249:
            run.check_file()
    run.restart()
    # the whole file comes back as it was: the next message opens
    run.deliver(*_fresh(run))
    gen = run.node.gen
    assert gen >= 3 and gen == run.mixes and run.store.generation(NODE_ID) in (gen, gen - 1)
    assert run.opened > 150 and run.forged_refused > 20


def _fresh(run: Run):
    plain = b"final"
    env = run.node.seal(KIND_UPLINK, plain)
    return env, plain, secure.parse_header(env).counter


def test_the_runs_covered_crashes_replaced_answers_and_file_checks():
    assert COVER["crash"] > 40 and COVER["replaced"] > 10 and COVER["checks"] > 400 and COVER["saves"] > 100, COVER


def test_many_messages_and_mixes_total(tmp_path, monkeypatch):
    """One long run: ten thousand uplinks over thirty mixes (counts for the report)."""
    run = Run(tmp_path, 99, monkeypatch)
    mixes = 0
    for i in range(10_000):
        run.send()
        if run.rng.random() < 0.3:
            run.inflight.reverse() if run.rng.random() < 0.2 else None
        while run.inflight and run.rng.random() < 0.9:
            run.deliver(*run.inflight.pop(run.rng.randrange(len(run.inflight))))
        if i % 300 == 299:
            for env, plain, counter in run.inflight:
                run.deliver(env, plain, counter)
            run.inflight.clear()
            for _ in range(70):  # the window moves on
                run.send()
                run.deliver(*run.inflight.pop())
            req = run.node.rekey_request()
            ans = run.store.rekey_answer(NODE_ID, req)
            assert run.node.rekey_answer(ans)
            mixes += 1
    assert mixes >= 30 and run.node.gen == mixes and run.opened > 9000
